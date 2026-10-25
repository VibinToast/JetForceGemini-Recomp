// Jet Force Gemini port: locales - the languages the game can show (jfg_locale_files.cpp: which exist and which is
// chosen; jfg_locale.cpp: showing it).
//
// English is the ROM's own text. Every other locale is a file in the game's folder, locales/<id>.json, one language
// each, that players can write and edit: the port builds the game's menus, subtitles and hints from it with the
// ROM's English as the pattern, so whatever a file lacks is shown in English. locales/english.json, when there is
// one, is shown in place of the ROM's English. The chosen one is shown in the game's language slot 1 through
// runtime assets (jfg_assets.h): menus, subtitles, hints and fonts.
//
// locales/<id>.json (every key but "name" and "text" optional):
//   {
//     "name": "Deutsch",                      shown in the Language list
//     "english_name": "German",               listed instead when the menus' font can't draw "name" (e.g. 日本語)
//     "text": {                               a missing key is English's string
//       "menu.000": "...",                    the menu strings (532)
//       "subtitle.00.0": "...",               cutscene captions: entry 00-50, line 0-n
//       "hint.000.0": "..."                   Mr. Hints and the other talks: section, string
//     },
//     "characters": { "ä": "85" },            a character the text uses -> its glyph ("XX": code 80 XX, or "PP:XX")
//     "glyphs": [ { "font": 2, "glyph": "85", "box": [x0, y0, x1, y1], "advance": 8, "rows": ["00ff...", ...] } ],
//                                             glyphs of fonts 0-3 replaced: each row's bytes in hex (4 bits a pixel,
//                                             font 2: 8)
//     "pages": { "descriptor": "<hex>", "glyphs": [ ... ] },  up to 4 pages of 256 glyphs (fonts 4-7), for scripts
//                                             that need more than one font's 256: codes 80 XX - 83 XX draw page 0-3
//     "ui": { "Master Volume": "Volume général" },  the port's own menus (launcher, settings): English text ->
//                                             this language's; it may hold RML markup (<b>On</b>)
//     "layout": {                             for text from another release, which fitted some of it to its own:
//       "hint.007": "<hex>",                  a hint section's own head (its strings: hint.007.0..n in its order)
//       "subtitle.07.0": { "time": 30 }       a caption line's id / colour / font / align / time
//     }
//   }
// Text: plain characters; {XX:YY} a two byte code, {XX} a byte, {{ and }} the braces. A character with no glyph is
// written without its accent (or '?'), and jfg_mods.log lists them.
#pragma once

#include <cstdint>
#include <map>
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
    std::string id;           // the file's name: locales/<id>.json
    std::string name;         // the locale's own name
    std::string english_name; // for menus whose font can't draw `name`
    std::vector<uint8_t> menu;
    std::vector<SubtitleEntry> subtitles; // empty or SUBTITLE_BLOCK
    std::vector<uint8_t> hint_table, hint_data;
    std::vector<Glyph> glyphs;
    std::vector<uint8_t> page_font; // 16 bytes when there are pages
    std::vector<uint8_t> pages;
};

// The locale files, by name. Built on the game thread at start-up (jfg_locale_start); empty before.
const std::vector<Locale>& all();
const Locale* find(const std::string& id); // "" = english.json, if any

// The chosen locale's id ("" = English), saved in jfg_language.txt. Any thread.
std::string chosen();
void choose(const std::string& id);

// For menus (any thread): (id, label) of English and every locale, in list order. Before the game starts (the
// launcher) it comes from the files' names alone. The label is the locale's name,
// or its English name when the name has characters the menus' font lacks (beyond Latin: U+024F).
std::vector<std::pair<std::string, std::string>> list();
// JFG_DEBUG_LANGUAGE (a test switch): the game tick and the launcher's call it.
void debug_pick();

// The port's own menus: `english` in the chosen language (its file's "ui"; english.json's for English), else
// `english` itself. Any thread. With JFG_UI_EXPORT=<file> every text asked for is collected there, as the English
// "ui" to translate.
std::string ui(const std::string& english);
// A text as "ui" keys it: no white space around it, each run inside as one space.
std::string ui_key(const std::string& text);
// `text` as the menus show it: RmlUi breaks lines only at ASCII spaces, so Japanese and Chinese punctuation
// (、。，）」：！？) gets one after it. ui() and translate_ui() give it so.
std::string ui_breaks(const std::string& text);
// After choose(), once (the UI thread, src/ui/ui_state.cpp): the "ui" of the language the menus show and of the new
// one (English -> text), to put the menus' text nodes in the new language in place; the new one is then in use.
bool take_ui_switch(std::map<std::string, std::string>& old_ui, std::map<std::string, std::string>& new_ui);
// For RmlUi's SystemInterface::TranslateString (src/ui/ui_state.cpp): `in` is a text node's text, white space and
// all. False when it has no translation (`out` = `in`).
bool translate_ui(std::string& out, const std::string& in);
// Whether the menus' translation has letters beyond ASCII (src/ui/ui_state.cpp then sets their capitals itself).
bool ui_has_accents();

} // namespace jfg::locale
