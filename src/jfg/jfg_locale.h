// Jet Force Gemini port: locales - the languages the game can show (jfg_locale_mods.cpp: which exist and which is
// chosen; jfg_locale.cpp: showing it).
//
// English is the game's own text. Every other locale comes from one place:
//   - built in: a language slot of the ROM with menu text of its own (the US ROM's French and German menus, or any
//     slot an asset mod fills);
//   - a locale mod: a mod with jfg_locale.json, one language each. Any number can be installed; they never replace
//     each other, and two mods for the same language are listed side by side under their mods' names.
// The chosen one is shown in the game's language slot 1 through runtime assets (jfg_assets.h): menus, subtitles,
// hints and fonts. What a locale lacks is shown in English.
//
// jfg_locale.json (all files in the game's own formats, as the decomp's tools/text.py writes them):
//   {
//     "name": "Italiano",                     shown in the Language list
//     "english_name": "Italian",              listed instead when the menus' font can't draw "name" (e.g. 日本語)
//     "menu": "menu.bin",                     one menu block: 532 s32 offsets (-1 = none), then the strings
//     "subtitle_table": "sub_table.bin",      51 entries: words (flags << 24 | offset), the end word, -1
//     "subtitle_data": "sub_data.bin",
//     "hint_table": "hint_table.bin",         as assets 0x09 / 0x0A
//     "hint_data": "hint_data.bin",
//     "glyphs": [ { "font": 0, "glyph": "85", "file": "glyph_0_85.bin" } ],   glyph header + pixels, replacing one
//     "pages": "pages.bin",                   optional: up to 4 pages of 256 glyphs (fonts 4-7), for scripts that
//     "page_font": "page_font.bin"            need more than one font's 256: one 16-byte font descriptor for them
//   }
// Every key but "name" is optional.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jfg::locale {

constexpr int SUBTITLE_BLOCK = 51; // subtitle entries per language
constexpr int MAX_PAGES = 4;       // fonts 4-7

struct Glyph {
    int font;
    int glyph;
    std::vector<uint8_t> data; // header + pixels, the font's glyph size
};

struct SubtitleEntry {
    uint8_t flags;
    std::vector<uint8_t> data;
};

struct Locale {
    std::string id;   // "builtin.<slot>" or "mod.<mod id>"
    std::string name;         // the locale's own name
    std::string english_name; // for menus whose font can't draw `name`
    std::vector<uint8_t> menu;
    std::vector<SubtitleEntry> subtitles; // empty or SUBTITLE_BLOCK
    std::vector<uint8_t> hint_table, hint_data;
    std::vector<Glyph> glyphs;
    std::vector<uint8_t> page_font; // 16 bytes when there are pages
    std::vector<uint8_t> pages;
};

// The locales after English, in list order (built in first, then mods in mod-list order). Built on the game thread
// at start-up (jfg_locale_start); empty before.
const std::vector<Locale>& all();
const Locale* find(const std::string& id);

// The chosen locale's id ("" = English), saved in jfg_language.txt. Any thread.
std::string chosen();
void choose(const std::string& id);

// For menus (any thread): (id, label) of English and every locale, in list order. The label is the locale's name,
// or its English name when the name has characters the menus' font lacks (beyond Latin: U+024F).
std::vector<std::pair<std::string, std::string>> list();

} // namespace jfg::locale
