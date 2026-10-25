// Jet Force Gemini port: showing the chosen locale (jfg_locale.h; which locales exist: jfg_locale_files.cpp).
//
// The game has language slots (frontGetLanguage(), a byte no US menu changes): setLanguage(n) loads menu block n,
// subtitleStart adds 47 + n - 1 to the id for n > 0, and the hint loader (overlay 7) reads slot n's own assets. The
// port only ever uses slots 0 (English) and 1: with a locale chosen, it serves that locale as slot 1 through runtime
// assets (jfg_assets.h, owner "locale"), so any number of locales can be installed and switched while playing:
//   - menus (assets 6/7): English's block, then the locale's (or English again);
//   - subtitles (4/5): English's 51 entries, then the locale's 51 (or English again). subtitleStart's wrapper turns
//     id into 51 + id, which the game's own shift (+47) would not;
//   - hints (0x0B/0x0C, slot 1's table and data): the locale's, or English's;
//   - fonts (0x3A/0x3B): the ROM's, with the locale's replaced glyphs, and pages for fonts 4-7 (below).
// The language is applied each tick with frontSetLanguage, and setLanguage's wrapper loads that slot whoever calls
// it (initFront loads 0). On a switch the fonts' widths (dxTable) are read again from the new glyphs and the glyph
// cache is emptied, since a replaced glyph keeps its code.
//
// Menu text room: initFront allocates front_text with mmAlloc(0x3000), English uses 0x2DA8 of it, and setLanguage
// reads a whole block into it. initFront's wrapper makes that one allocation as big as the largest menu block of any
// locale, so switching never needs a bigger one.
//
// Pages (Japanese, and any script with more than 256 characters): text codes each character as (0x80 + page,
// glyph). The US renderer (func_8006FD98) ignores the page and draws every glyph with the descriptor of the font the
// string started in, which it reads once. When a locale with pages is installed, 0x3A carries 8 descriptors and
// fontInit's wrapper creates fonts 4-7's display lists and a width table from page 0. While such a locale is shown,
// fonts 0-3 are swapped for the pages (descriptors and widths, as in Star Twins, where all four fonts are pages) and
// the glyph lookup the renderer makes (func_80071388) runs with fontInUse = 4 + the character's page, so each glyph is
// loaded and cached from its own page.
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <vector>

#include "recomp.h"
#include "jfg_api.h"
#include "jfg_assets.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_locale.h"

extern "C" void frontSetLanguage(uint8_t* rdram, recomp_context* ctx);
extern "C" void fontCreateDisplayList(uint8_t* rdram, recomp_context* ctx);
extern "C" void jfg_locale_build_registry(void); // jfg_locale_files.cpp

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

constexpr int GAME_TEXT = 0x04, GAME_TEXT_TABLE = 0x05, FRONT_TEXT = 0x06, FRONT_TEXT_TABLE = 0x07;
constexpr int HINT_TABLE = 0x09, HINT_DATA = 0x0A, SLOT1_HINT_TABLE = 0x0B, SLOT1_HINT_DATA = 0x0C;
constexpr int FONTS = 0x3A, FONT_GLYPHS = 0x3B;
constexpr int SERVED[] = { GAME_TEXT, GAME_TEXT_TABLE, FRONT_TEXT, FRONT_TEXT_TABLE, SLOT1_HINT_TABLE, SLOT1_HINT_DATA };
constexpr char OWNER[] = "locale";
constexpr int SUBTITLE_BLOCK = jfg::locale::SUBTITLE_BLOCK;
constexpr int SLOT1_SHIFT = 47;               // what subtitleStart adds for language 1
constexpr uint32_t FRONT_TEXT_SIZE = 0x3000;  // initFront's mmAlloc
constexpr uint32_t INIT_FRONT_FRAME = 0x18, SUBTITLE_START_FRAME = 0x20, RENDER_FRAME = 0x80; // their addiu $sp
constexpr uint32_t ADDR_language = 0x800FF386;   // u8, frontGetLanguage()
constexpr uint32_t ADDR_front_text = 0x800A51C8; // char**, NULL until initFront
constexpr uint32_t ADDR_fontInUse = 0x80103BA0, ADDR_Font = 0x80103BA4, ADDR_dxTable = 0x801040E8; // u8, ptr, u8*[4]
constexpr uint32_t ADDR_cacheLine = 0x801040FC;  // FontCacheLine*, 256 lines of 0x10
constexpr uint32_t FONT_DESC = 0x10, FONTS_WITH_PAGES = 8 * FONT_DESC, PAGE_FONT = 4, PAGES = 4;
constexpr uint32_t MM_COLOUR = 0xFF0000FF; // fontInit's mmAlloc colour

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3] : 0xFFFFFFFFu;
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.insert(b.end(), { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v });
}

// English's menu block (block 0) from the ROM.
std::span<const uint8_t> english_menu() {
    const std::span<const uint8_t> table = jfg::assets::read_rom(FRONT_TEXT_TABLE), front = jfg::assets::read_rom(FRONT_TEXT);
    const uint32_t start = be32(table, 4), end = be32(table, 8);
    if (end == 0xFFFFFFFFu || end < start || end > front.size()) return {};
    return front.subspan(start, end - start);
}

// The first locale with pages: fonts 4-7 are laid out for it from the start (fontInit sizes Font from 0x3A once).
const jfg::locale::Locale* g_page_layout = nullptr;
const jfg::locale::Locale* g_shown = nullptr;   // nullptr = English
bool g_started = false;
std::string g_shown_id;
int g_loaded = -1;          // the slot setLanguage last loaded
bool g_fonts_ready = false; // fontInit has run
bool g_jp_ready = false;    // fonts 4-7 exist
bool g_jp_active = false;   // fonts 0-3 are swapped for the pages
uint32_t g_jp_widths = 0;   // RDRAM: page 0's advances, 256 bytes
uint32_t g_saved_fonts[4][FONT_DESC / 4];
uint32_t g_saved_widths[4];
JfgCaller g_init_front, g_subtitle_start, g_render;

void serve_text(const jfg::locale::Locale* l) {
    if (l == nullptr) {
        for (int a : SERVED) jfg::assets::clear_runtime(a, OWNER);
        return;
    }
    // Menus: English, then the locale's block.
    const std::span<const uint8_t> table7 = jfg::assets::read_rom(FRONT_TEXT_TABLE), english = english_menu();
    const std::vector<uint8_t> menu = l->menu.empty() ? std::vector<uint8_t>(english.begin(), english.end()) : l->menu;
    std::vector<uint8_t> front(english.begin(), english.end()), t7;
    front.insert(front.end(), menu.begin(), menu.end());
    put32(t7, be32(table7, 0));
    put32(t7, 0);
    put32(t7, (uint32_t)english.size());
    put32(t7, (uint32_t)front.size());
    put32(t7, 0xFFFFFFFFu);
    jfg::assets::set_runtime(FRONT_TEXT, std::move(front), OWNER);
    jfg::assets::set_runtime(FRONT_TEXT_TABLE, std::move(t7), OWNER);

    // Subtitles: English's 51 entries, then the locale's 51.
    const std::span<const uint8_t> t5 = jfg::assets::read_rom(GAME_TEXT_TABLE), d4 = jfg::assets::read_rom(GAME_TEXT);
    std::vector<jfg::locale::SubtitleEntry> entries;
    for (int i = 0; i < SUBTITLE_BLOCK; i++) {
        const uint32_t w = be32(t5, 4 * i), start = w & 0xFFFFFF, end = be32(t5, 4 * i + 4) & 0xFFFFFF;
        if (end < start || end > d4.size()) { log("locale: unexpected subtitle table; subtitles stay English"); return; }
        entries.push_back({ (uint8_t)(w >> 24), std::vector<uint8_t>(d4.begin() + start, d4.begin() + end) });
    }
    const std::vector<jfg::locale::SubtitleEntry> second = l->subtitles.empty() ? entries : l->subtitles;
    entries.insert(entries.end(), second.begin(), second.end());
    std::vector<uint8_t> data, t5_out;
    for (const jfg::locale::SubtitleEntry& e : entries) {
        put32(t5_out, (uint32_t)e.flags << 24 | (uint32_t)data.size());
        data.insert(data.end(), e.data.begin(), e.data.end());
        while (data.size() % 8) data.push_back(0); // romCopy needs even offsets
    }
    put32(t5_out, (be32(t5, 4 * SUBTITLE_BLOCK) & 0xFF000000u) | (uint32_t)data.size());
    put32(t5_out, 0xFFFFFFFFu);
    jfg::assets::set_runtime(GAME_TEXT, std::move(data), OWNER);
    jfg::assets::set_runtime(GAME_TEXT_TABLE, std::move(t5_out), OWNER);

    // Hints: slot 1's table and data.
    const bool own = !l->hint_table.empty();
    const std::span<const uint8_t> ht = jfg::assets::read_rom(HINT_TABLE), hd = jfg::assets::read_rom(HINT_DATA);
    jfg::assets::set_runtime(SLOT1_HINT_TABLE, own ? l->hint_table : std::vector<uint8_t>(ht.begin(), ht.end()), OWNER);
    jfg::assets::set_runtime(SLOT1_HINT_DATA, own ? l->hint_data : std::vector<uint8_t>(hd.begin(), hd.end()), OWNER);
}

// Fonts: the ROM's, the shown locale's glyphs, and pages for fonts 4-7 (the shown locale's, else the layout one's).
void serve_fonts(const jfg::locale::Locale* l) {
    const jfg::locale::Locale* pages = (l != nullptr && !l->pages.empty()) ? l : g_page_layout;
    if (pages == nullptr && (l == nullptr || l->glyphs.empty())) {
        jfg::assets::clear_runtime(FONTS, OWNER);
        jfg::assets::clear_runtime(FONT_GLYPHS, OWNER);
        return;
    }
    const std::span<const uint8_t> fonts = jfg::assets::read_rom(FONTS), glyphs = jfg::assets::read_rom(FONT_GLYPHS);
    std::vector<uint8_t> f(fonts.begin(), fonts.begin() + std::min<size_t>(fonts.size(), 4 * FONT_DESC));
    std::vector<uint8_t> g(glyphs.begin(), glyphs.end());
    if (l != nullptr) {
        for (const jfg::locale::Glyph& glyph : l->glyphs) {
            const uint32_t offset = be32(fonts, glyph.font * FONT_DESC + 4), size = be32(fonts, glyph.font * FONT_DESC + 8) >> 16;
            if (glyph.data.size() != size || (uint64_t)offset + (glyph.glyph + 1) * size > g.size()) {
                log("locale: " + l->id + ": glyph " + std::to_string(glyph.glyph) + " of font " + std::to_string(glyph.font) +
                    " must be " + std::to_string(size) + " bytes; not used");
                continue;
            }
            std::copy(glyph.data.begin(), glyph.data.end(), g.begin() + offset + glyph.glyph * size);
        }
    }
    if (pages != nullptr) {
        const uint32_t base = (uint32_t)g.size(), size = be32(pages->page_font, 8) >> 16;
        const uint32_t count = size ? std::min<uint32_t>(PAGES, (uint32_t)(pages->pages.size() / (256 * size))) : 0;
        g.insert(g.end(), pages->pages.begin(), pages->pages.end());
        for (uint32_t i = 0; i < PAGES; i++) {
            std::vector<uint8_t> d = pages->page_font;
            const uint32_t offset = base + (i < count ? i : 0) * 256 * size;
            d[4] = offset >> 24; d[5] = offset >> 16; d[6] = offset >> 8; d[7] = (uint8_t)offset;
            f.insert(f.end(), d.begin(), d.end());
        }
    }
    jfg::assets::set_runtime(FONTS, std::move(f), OWNER);
    jfg::assets::set_runtime(FONT_GLYPHS, std::move(g), OWNER);
}

uint32_t front_text_size() {
    size_t size = std::max<size_t>(FRONT_TEXT_SIZE, english_menu().size());
    for (const jfg::locale::Locale& l : jfg::locale::all()) size = std::max(size, l.menu.size());
    return (uint32_t)((size + 15) & ~size_t(15));
}

int slot() { return g_shown != nullptr ? 1 : 0; }

void set_page_fonts(bool on) {
    if (!g_jp_ready) on = false;
    if (on == g_jp_active) return;
    const uint32_t font = jfg::rd32(ADDR_Font);
    for (uint32_t k = 0; k < PAGES; k++) {
        for (uint32_t w = 0; w < FONT_DESC / 4; w++) {
            const uint32_t a = font + k * FONT_DESC + 4 * w;
            if (on) {
                g_saved_fonts[k][w] = jfg::rd32(a);
                jfg::wr32(a, jfg::rd32(font + (PAGE_FONT + k) * FONT_DESC + 4 * w));
            } else {
                jfg::wr32(a, g_saved_fonts[k][w]);
            }
        }
        if (on) { g_saved_widths[k] = jfg::rd32(ADDR_dxTable + 4 * k); jfg::wr32(ADDR_dxTable + 4 * k, g_jp_widths); }
        else jfg::wr32(ADDR_dxTable + 4 * k, g_saved_widths[k]);
    }
    g_jp_active = on;
}

// After a switch: the fonts' widths from the glyphs now served, page 0's widths, fonts 4-7's places, and an empty
// glyph cache (fontInit's values).
void refresh_fonts() {
    if (!g_fonts_ready) return;
    set_page_fonts(false);
    const std::span<const uint8_t> fonts = jfg::assets::read(FONTS), glyphs = jfg::assets::read(FONT_GLYPHS);
    const uint32_t font = jfg::rd32(ADDR_Font);
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t offset = be32(fonts, k * FONT_DESC + 4), size = be32(fonts, k * FONT_DESC + 8) >> 16;
        const uint32_t widths = jfg::rd32(ADDR_dxTable + 4 * k);
        if (widths == 0 || (uint64_t)offset + 256ull * size > glyphs.size()) continue;
        for (uint32_t c = 0; c < 256; c++) jfg::wr8(widths + c, glyphs[offset + c * size + 8]);
    }
    if (g_jp_ready && fonts.size() >= FONTS_WITH_PAGES) {
        for (uint32_t k = PAGE_FONT; k < PAGE_FONT + PAGES; k++) jfg::wr32(font + k * FONT_DESC + 4, be32(fonts, k * FONT_DESC + 4));
        const uint32_t offset = be32(fonts, PAGE_FONT * FONT_DESC + 4), size = be32(fonts, PAGE_FONT * FONT_DESC + 8) >> 16;
        if ((uint64_t)offset + 256ull * size <= glyphs.size())
            for (uint32_t c = 0; c < 256; c++) jfg::wr8(g_jp_widths + c, glyphs[offset + c * size + 8]);
    }
    const uint32_t lines = jfg::rd32(ADDR_cacheLine);
    if (lines != 0) {
        for (uint32_t i = 0; i < 256; i++) {
            jfg::wr8(lines + i * 0x10 + 0, 0xFF); // font
            jfg::wr8(lines + i * 0x10 + 4, 0);    // state
            jfg::wr8(lines + i * 0x10 + 5, 0);    // lines remaining
        }
    }
}

void show(const std::string& id) {
    g_shown_id = id;
    g_shown = jfg::locale::find(id); // "": english.json, else the ROM's
    if (!id.empty() && g_shown == nullptr) log("locale: " + id + " isn't installed; English is shown");
    serve_text(g_shown);
    serve_fonts(g_shown);
    refresh_fonts();
    g_loaded = -1;
    log("locale: showing " + (g_shown ? g_shown->name + " (locales/" + g_shown->id + ".json)" : std::string("English (the game's own)")));
}

} // namespace

// After mods are loaded and boot assets applied, before the game's first instruction (jfg_apply_asset_mods).
extern "C" void jfg_locale_start(void) {
    jfg_locale_build_registry();
    g_page_layout = nullptr;
    for (const jfg::locale::Locale& l : jfg::locale::all()) {
        if (!l.pages.empty()) { g_page_layout = &l; break; }
    }
    g_started = true;
    show(jfg::locale::chosen());
}

// The game tick (jfg_tick.cpp): a new choice from the menus, and the slot the menus show.
extern "C" void jfg_locale_tick(recomp_context* ctx) {
    if (!g_started) return;
    // Test: JFG_DEBUG_LANGUAGE="secs,id" picks locale `id` ("" = English) that many seconds after start, as the
    // Language list does (Recomp-Docs/REFERENCE.md).
    static const char* debug_language = std::getenv("JFG_DEBUG_LANGUAGE");
    static const auto start = std::chrono::steady_clock::now();
    if (debug_language != nullptr) {
        const char* comma = std::strchr(debug_language, ',');
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(std::atoi(debug_language))) {
            jfg::locale::choose(comma ? comma + 1 : "");
            debug_language = nullptr;
        }
    }
    const std::string chosen = jfg::locale::chosen();
    if (chosen != g_shown_id) {
        show(chosen);
        jfg::api::fire(jfg::g_rdram, ctx, jfg::api::Event::LanguageChange);
    }
    if (jfg::rd32(ADDR_front_text) == 0) return;
    set_page_fonts(g_shown != nullptr && !g_shown->pages.empty());
    if (jfg::rd8(ADDR_language) != slot() || g_loaded != slot()) jfg::call_game(frontSetLanguage, ctx, { (uint32_t)slot() });
}

// For the mod API (jfg_api.cpp): the shown locale's id ("" = English).
std::string jfg_locale_shown() { return g_shown_id; }

JFG_WRAP(initFront) {
    JfgCaller::Scope scope(g_init_front, ctx, INIT_FRONT_FRAME);
    initFront_orig(rdram, ctx);
}

// From mmAlloc's wrapper (jfg_diagnostics.cpp), before the game's allocation: initFront's front_text buffer.
extern "C" void jfg_locale_alloc(recomp_context* ctx) {
    if (!g_init_front.direct(ctx) || (uint32_t)ctx->r4 != FRONT_TEXT_SIZE) return;
    const uint32_t size = front_text_size();
    ctx->r4 = (gpr)(int32_t)size;
    if (size > FRONT_TEXT_SIZE) {
        char line[120];
        std::snprintf(line, sizeof line, "locale: menu text buffer 0x%X bytes (the game's own: 0x%X)", size, FRONT_TEXT_SIZE);
        log(line);
    }
}

JFG_WRAP(setLanguage) {
    ctx->r4 = (gpr)slot();
    setLanguage_orig(rdram, ctx);
    if (MEM_W(0, (gpr)(int32_t)ADDR_front_text) != 0) g_loaded = slot();
}

JFG_WRAP(subtitleStart) {
    const int id = (int)(int32_t)ctx->r4;
    if (jfg::rd8(ADDR_language) == 1 && id >= 0 && id < SUBTITLE_BLOCK) ctx->r4 = (gpr)(int32_t)(id + SUBTITLE_BLOCK - SLOT1_SHIFT);
    JfgCaller::Scope scope(g_subtitle_start, ctx, SUBTITLE_START_FRAME);
    subtitleStart_orig(rdram, ctx);
}

JFG_WRAP(fontInit) {
    fontInit_orig(rdram, ctx);
    g_fonts_ready = true;
    g_jp_ready = g_jp_active = false;
    const std::span<const uint8_t> fonts = jfg::assets::read(FONTS), glyphs = jfg::assets::read(FONT_GLYPHS);
    if (fonts.size() < FONTS_WITH_PAGES) return;
    const uint32_t font = jfg::rd32(ADDR_Font);
    const uint32_t page0 = be32(fonts, PAGE_FONT * FONT_DESC + 4), size = be32(fonts, PAGE_FONT * FONT_DESC + 8) >> 16;
    if ((uint64_t)page0 + 256ull * size > glyphs.size()) return;
    g_jp_widths = jfg::call_game(mmAlloc, ctx, { 0x400u, MM_COLOUR });
    if (g_jp_widths == 0) return;
    for (uint32_t c = 0; c < 256; c++) jfg::wr8(g_jp_widths + c, glyphs[page0 + c * size + 8]); // the header's advance
    for (uint32_t k = PAGE_FONT; k < PAGE_FONT + PAGES; k++) jfg::call_game(fontCreateDisplayList, ctx, { font + k * FONT_DESC });
    g_jp_ready = true;
}

JFG_WRAP(func_8006FD98_70998) {
    JfgCaller::Scope scope(g_render, ctx, RENDER_FRAME);
    func_8006FD98_70998_orig(rdram, ctx);
}

// Called by the renderer with $s5 just past the glyph byte, so the code's first byte is at $s5 - 2.
JFG_WRAP(func_80071388_71F88) {
    if (g_jp_active && g_render.direct(ctx)) {
        const uint8_t page = jfg::rd8((uint32_t)ctx->r21 - 2);
        if ((page & 0x80) && (page & 0x7F) < PAGES) {
            const uint8_t saved = jfg::rd8(ADDR_fontInUse);
            jfg::wr8(ADDR_fontInUse, (uint8_t)(PAGE_FONT + (page & 0x7F)));
            func_80071388_71F88_orig(rdram, ctx);
            jfg::wr8(ADDR_fontInUse, saved);
            return;
        }
    }
    func_80071388_71F88_orig(rdram, ctx);
}
