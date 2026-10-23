// Jet Force Gemini port: room for translated text, so locale packs (asset mods made with the decomp's tools/text.py)
// can carry full translations.
//
// Front-end (menu) text: asset 7 is a string count and the offsets of one block per language in asset 6; setLanguage
// (menu.c) loads one block into front_text, which initFront allocates with mmAlloc(0x3000). English uses 0x2DA8 of
// it, so a longer translation would overwrite the heap. initFront's wrapper makes that one allocation as big as the
// largest block in the (possibly replaced) asset, never smaller than the game's own.
//
// Subtitles and messages (assets 4/5): subtitleStart(id) checks id against the entry count and, for languages 1-4,
// adds 47-50 to it; in the US ROM those land on "You've taken the wrong route" and a stub, so only English works.
// A locale pack can give each language its own block of SUBTITLE_BLOCK entries: language L at entries
// L * SUBTITLE_BLOCK and up. When the table holds language L's block, subtitleStart's wrapper passes an id the game's
// own shift turns into the right entry; without it the game behaves as it always did.
#include <algorithm>
#include <cstdint>
#include <span>

#include "librecomp/game.hpp"
#include "recomp.h"
#include "jfg_hooks.h"

namespace {

constexpr uint32_t ROM_ASSET_TABLE = 0xB1750; // as in jfg_assets.cpp
constexpr uint32_t ROM_ASSET_AREA = 0xB1880;
constexpr int ASSET_GAME_TEXT_TABLE = 5, ASSET_FRONT_TEXT_TABLE = 7;
constexpr uint32_t FRONT_TEXT_SIZE = 0x3000;  // initFront's mmAlloc
constexpr uint32_t INIT_FRONT_FRAME = 0x18;   // initFront's addiu $sp
constexpr int SUBTITLE_BLOCK = 51;            // entries per language: the US table's count
constexpr int SUBTITLE_SHIFT[5] = { 0, 47, 48, 49, 50 }; // what subtitleStart adds for languages 0-4
constexpr uint32_t ADDR_language = 0x800FF386; // u8, frontGetLanguage()

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

// Entries in the subtitle table, as subtitlesInit counts them.
int subtitle_entries() {
    return (int)(asset(ASSET_GAME_TEXT_TABLE).size() >> 2) - 2;
}

JfgCaller g_init_front;

} // namespace

JFG_WRAP(initFront) {
    JfgCaller::Scope scope(g_init_front, ctx, INIT_FRONT_FRAME);
    initFront_orig(rdram, ctx);
}

// From mmAlloc's wrapper (jfg_diagnostics.cpp), before the game's allocation: initFront's front_text buffer.
extern "C" void jfg_locale_alloc(recomp_context* ctx) {
    if (g_init_front.direct(ctx) && (uint32_t)ctx->r4 == FRONT_TEXT_SIZE) ctx->r4 = (gpr)(int32_t)front_text_size();
}

JFG_WRAP(subtitleStart) {
    const int language = MEM_BU(0, (gpr)(int32_t)ADDR_language);
    const int id = (int)(int32_t)ctx->r4;
    if (language >= 1 && language <= 4 && id >= 0 && id < SUBTITLE_BLOCK &&
        subtitle_entries() >= SUBTITLE_BLOCK * (language + 1)) {
        ctx->r4 = (gpr)(int32_t)(id + SUBTITLE_BLOCK * language - SUBTITLE_SHIFT[language]);
    }
    subtitleStart_orig(rdram, ctx);
}
