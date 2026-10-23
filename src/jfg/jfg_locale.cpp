// Jet Force Gemini port: the Language option, and room for translated text so locale packs (asset mods made with the
// decomp's tools/text.py) can carry full translations.
//
// Language: frontGetLanguage() is a byte the US game never changes (no menu sets it); setLanguage(n) loads language
// n's block of menu text. The option (g.language) is applied each tick with frontSetLanguage, and setLanguage's wrapper
// loads that language whoever calls it (initFront loads 0). A language whose menu block is a stub (Spanish and
// Japanese in the US ROM, until a locale pack adds them) falls back to English.
//
// Front-end (menu) text: asset 7 is a string count and the offsets of one block per language in asset 6; setLanguage
// (menu.c) loads one block into front_text, which initFront allocates with mmAlloc(0x3000). English uses 0x2DA8 of
// it, so a longer translation would overwrite the heap. initFront's wrapper makes that one allocation as big as the
// largest block in the (possibly replaced) asset, never smaller than the game's own.
//
// Subtitles and messages (assets 4/5): subtitleStart(id) checks id against the entry count and, for languages 1-4,
// adds 47-50 to it; in the US ROM those land on "You've taken the wrong route" and a stub. A locale pack can give each
// language its own block of SUBTITLE_BLOCK entries: language L at entries L * SUBTITLE_BLOCK and up. When the table
// holds language L's block, subtitleStart's wrapper passes an id the game's own shift turns into the right entry;
// without it the game is told the language is English, so it shows the English entry.
//
// Hints (Mr. Hints and the other talks, overlay 7): hinttextStart reads language L's table (0x09, 0x0B, 0x0D, 0x0F,
// 0x11) and func_overlay_7_00000988 its data (0x0A, 0x0C, 0x0E, 0x0A again for 3, 0x12). Outside English these are
// 0x10-byte stubs in the US ROM, which leave the hint box empty. When language L's table is a stub, both are told the
// language is English.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <span>

#include "librecomp/game.hpp"
#include "recomp.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"

extern "C" void frontSetLanguage(uint8_t* rdram, recomp_context* ctx);

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

constexpr uint32_t ROM_ASSET_TABLE = 0xB1750; // as in jfg_assets.cpp
constexpr uint32_t ROM_ASSET_AREA = 0xB1880;
constexpr int ASSET_GAME_TEXT_TABLE = 5, ASSET_FRONT_TEXT_TABLE = 7;
constexpr int HINT_TABLE_ASSET[5] = { 0x09, 0x0B, 0x0D, 0x0F, 0x11 }; // hinttextStart's table per language
constexpr uint32_t HINT_STUB_SIZE = 0x10;     // a language without hints in the US ROM
constexpr int LANGUAGES = 5;                  // 0 English, 1 French, 2 German, 3 Spanish, 4 Japanese
constexpr uint32_t FRONT_TEXT_SIZE = 0x3000;  // initFront's mmAlloc
constexpr uint32_t INIT_FRONT_FRAME = 0x18;   // initFront's addiu $sp
constexpr uint32_t SUBTITLE_START_FRAME = 0x20, HINT_START_FRAME = 0x28, HINT_DATA_FRAME = 0x28;
constexpr int SUBTITLE_BLOCK = 51;            // entries per language: the US table's count
constexpr int SUBTITLE_SHIFT[5] = { 0, 47, 48, 49, 50 }; // what subtitleStart adds for languages 0-4
constexpr uint32_t ADDR_language = 0x800FF386; // u8, frontGetLanguage()
constexpr uint32_t ADDR_front_text = 0x800A51C8; // char**, NULL until initFront

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3] : 0xFFFFFFFFu;
}

// One asset of the ROM the game reads (with asset mods applied), or an empty span.
std::span<const uint8_t> asset(int index) {
    const std::span<const uint8_t> rom = recomp::get_rom();
    const uint32_t count = be32(rom, ROM_ASSET_TABLE);
    if ((uint32_t)index >= count) return {};
    const uint64_t start = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 4 + 4 * index);
    const uint64_t end = (uint64_t)ROM_ASSET_AREA + be32(rom, ROM_ASSET_TABLE + 8 + 4 * index);
    if (end < start || end > rom.size()) return {};
    return rom.subspan(start, end - start);
}

// The largest language block of the front-end text: word 0 is the string count, then block offsets ended by -1.
uint32_t front_text_size() {
    const std::span<const uint8_t> table = asset(ASSET_FRONT_TEXT_TABLE);
    uint32_t size = FRONT_TEXT_SIZE;
    for (size_t o = 4; o + 8 <= table.size(); o += 4) {
        const uint32_t start = be32(table, o), end = be32(table, o + 4);
        if (end == 0xFFFFFFFFu) break;
        if (end > start) size = std::max(size, (end - start + 15) & ~15u);
    }
    return size;
}

// Whether language L has menu text: its block (L + 1 in the table) holds strings after the offsets. A stub is the
// offsets alone, all -1.
bool has_menu_text(int language) {
    const std::span<const uint8_t> table = asset(ASSET_FRONT_TEXT_TABLE);
    const uint32_t count = be32(table, 0);
    const uint32_t start = be32(table, 4 + 4 * (language + 1)), end = be32(table, 8 + 4 * (language + 1));
    return language >= 0 && language < LANGUAGES && end != 0xFFFFFFFFu && end > start && end - start > count * 4;
}

// Entries in the subtitle table, as subtitlesInit counts them.
int subtitle_entries() {
    return (int)(asset(ASSET_GAME_TEXT_TABLE).size() >> 2) - 2;
}

bool has_subtitles(int language) {
    return language == 0 || subtitle_entries() >= SUBTITLE_BLOCK * (language + 1);
}

bool has_hints(int language) {
    return language == 0 || asset(HINT_TABLE_ASSET[language]).size() > HINT_STUB_SIZE;
}

// The language the menus use: the option, or English when it has no menu text.
int menu_language() {
    const int wanted = std::clamp(jfg::g.language, 0, LANGUAGES - 1);
    return has_menu_text(wanted) ? wanted : 0;
}

JfgCaller g_init_front, g_subtitle_start, g_hint_start, g_hint_data;
int g_loaded = -1; // the language setLanguage last loaded

} // namespace

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
        jfg::jfg_log_line("jfg_mods.log", line);
    }
}

JFG_WRAP(setLanguage) {
    const int language = menu_language();
    ctx->r4 = (gpr)language;
    setLanguage_orig(rdram, ctx);
    if (MEM_W(0, (gpr)(int32_t)ADDR_front_text) != 0) g_loaded = language;
}

// From the game tick (jfg_tick.cpp): the Language option, once the menu text exists.
extern "C" void jfg_locale_tick(recomp_context* ctx) {
    if (jfg::rd32(ADDR_front_text) == 0) return;
    const int language = menu_language();
    if (jfg::rd8(ADDR_language) != language || g_loaded != language) {
        jfg::call_game(frontSetLanguage, ctx, { (uint32_t)language });
        static int logged = -1;
        if (logged != language) {
            static const char* const names[LANGUAGES] = { "English", "French", "German", "Spanish", "Japanese" };
            char line[160];
            std::snprintf(line, sizeof line, "locale: language %s (option %d); subtitles %s, hints %s", names[language],
                          jfg::g.language, has_subtitles(language) ? "own" : "English", has_hints(language) ? "own" : "English");
            jfg::jfg_log_line("jfg_mods.log", line);
            logged = language;
        }
    }
}

JFG_WRAP(subtitleStart) {
    const int language = jfg::rd8(ADDR_language);
    const int id = (int)(int32_t)ctx->r4;
    if (language >= 1 && language < LANGUAGES && id >= 0 && id < SUBTITLE_BLOCK && has_subtitles(language)) {
        ctx->r4 = (gpr)(int32_t)(id + SUBTITLE_BLOCK * language - SUBTITLE_SHIFT[language]);
    }
    JfgCaller::Scope scope(g_subtitle_start, ctx, SUBTITLE_START_FRAME);
    subtitleStart_orig(rdram, ctx);
}

JFG_WRAP(hinttextStart) {
    JfgCaller::Scope scope(g_hint_start, ctx, HINT_START_FRAME);
    hinttextStart_orig(rdram, ctx);
}

JFG_WRAP(func_overlay_7_00000988_1EFBAB0) {
    JfgCaller::Scope scope(g_hint_data, ctx, HINT_DATA_FRAME);
    func_overlay_7_00000988_1EFBAB0_orig(rdram, ctx);
}

// A leaf function: called from one of the scoped callers, $sp is that caller's own.
JFG_WRAP(frontGetLanguage) {
    frontGetLanguage_orig(rdram, ctx);
    const int language = (int)(uint8_t)ctx->r2;
    if (language == 0 || language >= LANGUAGES) return;
    if ((g_subtitle_start.direct(ctx) && !has_subtitles(language)) ||
        ((g_hint_start.direct(ctx) || g_hint_data.direct(ctx)) && !has_hints(language))) {
        ctx->r2 = 0;
    }
}
