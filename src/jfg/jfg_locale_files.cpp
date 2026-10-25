// Jet Force Gemini port: which locales exist (locales/*.json) and which is chosen (jfg_locale.h). Showing it:
// jfg_locale.cpp.
//
// A locale file holds text by key; the port builds the game's text assets from it with the ROM's English as the
// pattern (as the decomp's tools/text.py lays them out): each menu string, subtitle line and hint string English has
// is the locale's when its key is there, else English's own bytes.
#include <algorithm>
#include <chrono>
#include <cstring>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "jfg_assets.h"
#include "jfg_locale.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

using nlohmann::json;

constexpr char FOLDER[] = "locales";
constexpr char CHOICE_FILE[] = "jfg_language.txt";
constexpr char ENGLISH_ID[] = "english";
constexpr int GAME_TEXT = 0x04, GAME_TEXT_TABLE = 0x05, FRONT_TEXT = 0x06, FRONT_TEXT_TABLE = 0x07;
constexpr int HINT_TABLE = 0x09, HINT_DATA = 0x0A, FONTS = 0x3A, FONT_GLYPHS = 0x3B;
constexpr uint32_t FONT_DESC = 0x10, GLYPH_HEADER = 0x10, PAGE_GLYPHS = 4 * 256;
constexpr uint32_t ROM_ASCII_GLYPHS = 0xA7B04; // fontConvertString's table (D_800A6F04): character - 32 -> glyph
constexpr size_t SUBTITLE_ENTRY_MAX = 960;     // gGameTextTableEntries' buffer (subtitlesInit)
// The names the old Language option and locale mods saved, for jfg_language.txt from before locale files.
const char* const SLOT_IDS[5] = { "", "french", "german", "spanish", "japanese" };

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3] : 0xFFFFFFFFu;
}
void put32(std::vector<uint8_t>& b, uint32_t v) {
    b.insert(b.end(), { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v });
}
void pad8(std::vector<uint8_t>& b) {
    while (b.size() % 8) b.push_back(0);
}

std::vector<jfg::locale::Locale> g_locales;
std::mutex g_mutex; // g_list and g_chosen (read by the menus)
std::vector<std::pair<std::string, std::string>> g_list = { { "", "English" } };
bool g_listed = false; // g_list is filled (the registry, or list_names before it)
std::string g_chosen;
bool g_chosen_loaded = false;
std::atomic<bool> g_ui_switch{ false }; // choose() since the menus last took their language (take_ui_switch)

// --- text ------------------------------------------------------------------------------------------------------------

// Characters no glyph draws, and what is written instead (as text.py's flat-import does); Latin letters lose their
// accents (LATIN_BASE).
const std::map<uint32_t, const char*> FALLBACKS = {
    { 0xDF, "ss" }, { 0xC6, "AE" }, { 0xE6, "ae" }, { 0x152, "OE" }, { 0x153, "oe" }, { 0xDE, "Th" }, { 0xFE, "th" },
    { 0xD7, "x" }, { 0xF7, "/" }, { 0xA0, " " }, { 0xB7, "." }, { 0x2022, "*" }, { 0x20AC, "EUR" }, { 0xA3, "L" },
    { 0xB0, "o" }, { 0x2018, "'" }, { 0x2019, "'" }, { 0x201A, "," }, { 0x201C, "\"" }, { 0x201D, "\"" },
    { 0x201E, "\"" }, { 0xAB, "\"" }, { 0xBB, "\"" }, { 0x2039, "'" }, { 0x203A, "'" }, { 0x2013, "-" },
    { 0x2014, "-" }, { 0x2026, "..." }, { 0xA1, "!" }, { 0xBF, "?" },
};
// U+00C0-U+017F without their accents.
constexpr char LATIN_BASE[] =
    "AAAAAAACEEEEIIIIDNOOOOO?OUUUUYTsaaaaaaaceeeeiiiidnooooo?ouuuuyty"
    "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiIiJjJjKkkLlLlLlL"
    "lLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";

struct Encoder {
    const jfg::locale::Locale& locale;
    std::map<uint32_t, std::vector<uint8_t>> characters; // code point -> its code
    std::span<const uint8_t> ascii_glyphs;               // 96 bytes
    std::map<std::string, int> missing;                  // characters written with a fallback, and how often
    std::vector<std::string> notes;

    void put(std::vector<uint8_t>& out, uint8_t c, bool glyphs) {
        if (glyphs && c >= 32 && c < 0x80 && ascii_glyphs.size() == 96) out.insert(out.end(), { 0x80, ascii_glyphs[c - 32] });
        else out.push_back(c);
    }

    static int hex(char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    }

    // `text` (UTF-8) as the game stores it, 0 ended. With `glyphs`, characters are glyph codes (0x80, glyph), as the
    // hints are. {XX:YY} is a two byte code, {XX} one byte, {{ and }} the braces.
    std::vector<uint8_t> encode(const std::string& text, bool glyphs) {
        std::vector<uint8_t> out;
        for (size_t i = 0; i < text.size();) {
            const char c = text[i];
            if (c == '{' || c == '}') {
                if (i + 1 < text.size() && text[i + 1] == c) { put(out, (uint8_t)c, glyphs); i += 2; continue; }
                if (c == '{' && i + 6 < text.size() && text[i + 3] == ':' && text[i + 6] == '}') {
                    const int a = hex(text[i + 1]), b = hex(text[i + 2]), d = hex(text[i + 4]), e = hex(text[i + 5]);
                    if (a >= 8 && b >= 0 && d >= 0 && e >= 0) { out.insert(out.end(), { (uint8_t)(a << 4 | b), (uint8_t)(d << 4 | e) }); i += 7; continue; }
                }
                if (c == '{' && i + 3 < text.size() && text[i + 3] == '}') {
                    const int a = hex(text[i + 1]), b = hex(text[i + 2]);
                    if (a >= 0 && b >= 0) { out.push_back((uint8_t)(a << 4 | b)); i += 4; continue; }
                }
                put(out, c == '{' ? '(' : ')', glyphs); // a lone brace
                i++;
                continue;
            }
            const uint8_t u = (uint8_t)c;
            if (u < 0x80) { put(out, u, glyphs); i++; continue; }
            // One UTF-8 character.
            const int len = u >= 0xF0 ? 4 : u >= 0xE0 ? 3 : u >= 0xC0 ? 2 : 1;
            uint32_t cp = len == 1 ? u : u & (0x7F >> len);
            for (int k = 1; k < len && i + k < text.size(); k++) cp = cp << 6 | ((uint8_t)text[i + k] & 0x3F);
            const std::string ch = text.substr(i, len);
            i += len;
            if (auto it = characters.find(cp); it != characters.end()) { out.insert(out.end(), it->second.begin(), it->second.end()); continue; }
            std::string plain = "?";
            if (auto it = FALLBACKS.find(cp); it != FALLBACKS.end()) plain = it->second;
            else if (cp >= 0xC0 && cp < 0x180) plain = std::string(1, LATIN_BASE[cp - 0xC0]);
            missing[ch + " -> " + plain]++;
            for (char p : plain) put(out, (uint8_t)p, glyphs);
        }
        out.push_back(0);
        return out;
    }
};

// A 0 ended string at `pos` of `data`, with its 0.
std::vector<uint8_t> raw_string(std::span<const uint8_t> data, size_t pos) {
    size_t end = pos;
    while (end < data.size() && data[end] != 0) end++;
    std::vector<uint8_t> out(data.begin() + std::min(pos, data.size()), data.begin() + end);
    out.push_back(0);
    return out;
}

const std::string* text_of(const json& text, const std::string& key) {
    auto it = text.find(key);
    return it != text.end() && it->is_string() ? it->get_ptr<const std::string*>() : nullptr;
}

// Menus: English's block with the locale's strings (menu.NNN).
std::vector<uint8_t> build_menu(const json& text, Encoder& enc) {
    const std::span<const uint8_t> table = jfg::assets::read_rom(FRONT_TEXT_TABLE), front = jfg::assets::read_rom(FRONT_TEXT);
    const uint32_t count = be32(table, 0), start = be32(table, 4), end = be32(table, 8);
    if (count == 0xFFFFFFFFu || end < start || end > front.size() || 4ull * count > end - start) return {};
    const std::span<const uint8_t> block = front.subspan(start, end - start);
    std::vector<uint8_t> offsets, body;
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t o = be32(block, 4 * i);
        if (o == 0xFFFFFFFFu || o < 4 * count || o >= block.size()) { put32(offsets, 0xFFFFFFFFu); continue; }
        char key[16];
        std::snprintf(key, sizeof key, "menu.%03u", i);
        put32(offsets, 4 * count + (uint32_t)body.size());
        const std::string* s = text_of(text, key);
        const std::vector<uint8_t> bytes = s ? enc.encode(*s, false) : raw_string(block, o);
        body.insert(body.end(), bytes.begin(), bytes.end());
    }
    offsets.insert(offsets.end(), body.begin(), body.end());
    pad8(offsets);
    return offsets;
}

// Subtitles: English's 51 entries, line by line (subtitle.EE.L), each line's 8 byte head (id + 1, colour, font, align,
// time) English's unless "layout" changes it.
std::vector<jfg::locale::SubtitleEntry> build_subtitles(const json& text, const json& layout, Encoder& enc) {
    const std::span<const uint8_t> table = jfg::assets::read_rom(GAME_TEXT_TABLE), data = jfg::assets::read_rom(GAME_TEXT);
    std::vector<jfg::locale::SubtitleEntry> out;
    for (int e = 0; e < jfg::locale::SUBTITLE_BLOCK; e++) {
        const uint32_t w = be32(table, 4 * e), start = w & 0xFFFFFF, end = be32(table, 4 * e + 4) & 0xFFFFFF;
        if (w == 0xFFFFFFFFu || end < start || end > data.size()) return {};
        const std::vector<uint8_t> english(data.begin() + start, data.begin() + end);
        std::vector<uint8_t> built;
        size_t pos = start;
        bool ok = true;
        for (int n = 0; pos < end && data[pos] != 0; n++) {
            if (pos + 8 > end) { ok = false; break; }
            std::vector<uint8_t> head(data.begin() + pos, data.begin() + pos + 8);
            std::vector<uint8_t> line = raw_string(data, pos + 8);
            pos += 8 + line.size();
            char key[24];
            std::snprintf(key, sizeof key, "subtitle.%02d.%d", e, n);
            if (const std::string* s = text_of(text, key)) line = enc.encode(*s, false);
            if (auto it = layout.find(key); it != layout.end() && it->is_object()) {
                const json& l = *it;
                if (l.contains("id") && l["id"].is_number_integer()) head[0] = (uint8_t)(l["id"].get<int>() + 1);
                if (l.contains("colour") && l["colour"].is_array() && l["colour"].size() == 4)
                    for (int k = 0; k < 4; k++) head[1 + k] = (uint8_t)l["colour"][k].get<int>();
                if (l.contains("font") && l["font"].is_number_integer()) head[5] = (uint8_t)l["font"].get<int>();
                if (l.contains("align")) {
                    const json& a = l["align"];
                    if (a.is_number_integer()) head[6] = (uint8_t)a.get<int>();
                    else if (a == "left") head[6] = 0;
                    else if (a == "right") head[6] = 1;
                    else if (a == "center") head[6] = 4;
                }
                if (l.contains("time") && l["time"].is_number_integer()) head[7] = (uint8_t)l["time"].get<int>();
            }
            built.insert(built.end(), head.begin(), head.end());
            built.insert(built.end(), line.begin(), line.end());
            if (pos < end && (data[pos] == 0x0A || data[pos] == 0x0C)) built.push_back(data[pos++]); // next line / page
        }
        built.push_back(0);
        if (!ok) built = english;
        if (built.size() > SUBTITLE_ENTRY_MAX) {
            enc.notes.push_back("subtitle " + std::to_string(e) + " is " + std::to_string(built.size()) +
                                " bytes, over the game's " + std::to_string(SUBTITLE_ENTRY_MAX) + "; it stays English");
            built = english;
        }
        out.push_back({ (uint8_t)(w >> 24), std::move(built) });
    }
    return out;
}

// A hint section's words before its strings: their length and where its pointer words are. A menu (sec[0] = 0, as
// func_overlay_7_00000C90 reads it) is 16 byte entries, the first one's s16 the count, +4 of each an offset (0 = the
// overlay's own string); a script (func_overlay_7_00000B00) ends at op 0, ops 2-5 point, 6 and 7 are two words.
bool hint_head(std::span<const uint8_t> sec, size_t& head_len, std::vector<size_t>& pointers) {
    pointers.clear();
    if (sec.empty()) return false;
    if (sec[0] == 0) {
        const int count = sec.size() >= 2 ? (int16_t)(sec[0] << 8 | sec[1]) : 0;
        head_len = 16 * (size_t)count + 4;
        if (count <= 0 || head_len > sec.size()) return false;
        for (int e = 0; e < count; e++) if (be32(sec, 16 * e + 4) != 0) pointers.push_back(16 * e + 4);
        return true;
    }
    for (size_t pos = 0; pos + 4 <= sec.size();) {
        const uint8_t op = sec[pos];
        if (op == 0) { head_len = pos + 4; return true; }
        if (op >= 2 && op <= 5) pointers.push_back(pos);
        else if (op != 1 && op != 6 && op != 7) return false;
        pos += op == 6 || op == 7 ? 8 : 4;
    }
    return false;
}

std::vector<uint32_t> hint_targets(std::span<const uint8_t> head, const std::vector<size_t>& pointers) {
    std::vector<uint32_t> t;
    for (size_t p : pointers) t.push_back(be32(head, p) & 0xFFFFFF);
    std::sort(t.begin(), t.end());
    t.erase(std::unique(t.begin(), t.end()), t.end());
    return t;
}

// Hints: English's sections (hint.SSS.K, glyph coded), or the section head "layout" gives (hint.SSS: hex, a release
// that fitted its hints to its own text) with that many strings.
void build_hints(const json& text, const json& layout, Encoder& enc, std::vector<uint8_t>& table_out, std::vector<uint8_t>& data_out) {
    const std::span<const uint8_t> table = jfg::assets::read_rom(HINT_TABLE), data = jfg::assets::read_rom(HINT_DATA);
    const size_t words = table.size() / 4;
    if (words < 2) return;
    std::vector<uint8_t> t, d;
    for (size_t s = 0; s + 1 < words; s++) {
        put32(t, (uint32_t)d.size());
        const uint32_t start = be32(table, 4 * s), end = be32(table, 4 * s + 4);
        if (end < start || end > data.size()) return;
        const std::span<const uint8_t> sec = data.subspan(start, end - start);
        char key[16];
        std::snprintf(key, sizeof key, "hint.%03zu", s);
        std::vector<uint8_t> head;
        size_t head_len = 0;
        std::vector<size_t> pointers;
        bool own = false;
        if (auto it = layout.find(key); it != layout.end() && it->is_string()) {
            const std::string& h = it->get_ref<const std::string&>();
            for (size_t k = 0; k + 1 < h.size(); k += 2) head.push_back((uint8_t)(Encoder::hex(h[k]) << 4 | Encoder::hex(h[k + 1])));
            own = hint_head(head, head_len, pointers) && head_len == head.size();
            if (!own) enc.notes.push_back(std::string(key) + ": its layout isn't a hint head; English's is used");
        }
        if (!own && !(hint_head(sec, head_len, pointers))) { d.insert(d.end(), sec.begin(), sec.end()); continue; }
        if (!own) head.assign(sec.begin(), sec.begin() + head_len);
        const std::vector<uint32_t> targets = hint_targets(head, pointers);
        std::vector<std::vector<uint8_t>> strings;
        size_t next = head_len;
        bool english_ok = !own;
        for (size_t k = 0; k < targets.size(); k++) {
            const std::string* s = text_of(text, std::string(key) + "." + std::to_string(k));
            if (s) { strings.push_back(enc.encode(*s, true)); continue; }
            if (own) break;
            if (targets[k] != next) { english_ok = false; break; } // English's strings aren't where dump reads them
            strings.push_back(raw_string(sec, targets[k]));
            next += strings.back().size();
        }
        if (strings.size() != targets.size() || (!own && !english_ok)) {
            if (own) enc.notes.push_back(std::string(key) + ": its layout needs " + std::to_string(targets.size()) + " strings; English's section is used");
            d.insert(d.end(), sec.begin(), sec.end());
            continue;
        }
        std::map<uint32_t, uint32_t> moved;
        std::vector<uint8_t> body;
        for (size_t k = 0; k < targets.size(); k++) {
            moved[targets[k]] = (uint32_t)(head.size() + body.size());
            body.insert(body.end(), strings[k].begin(), strings[k].end());
        }
        for (size_t p : pointers) {
            const uint32_t w = be32(head, p), to = (w & 0xFF000000u) | moved[w & 0xFFFFFF];
            head[p] = to >> 24; head[p + 1] = to >> 16; head[p + 2] = to >> 8; head[p + 3] = (uint8_t)to;
        }
        d.insert(d.end(), head.begin(), head.end());
        d.insert(d.end(), body.begin(), body.end());
        pad8(d);
    }
    put32(t, (uint32_t)d.size());
    const uint32_t tail = be32(table, 4 * (words - 1));
    if (tail <= data.size()) d.insert(d.end(), data.begin() + tail, data.end());
    table_out = std::move(t);
    data_out = std::move(d);
}

// --- glyphs ------------------------------------------------------------------------------------------------------------

// One glyph from {"box": [x0, y0, x1, y1], "advance": n, "rows": ["hex", ...]}: `header` (the ROM's 16 bytes) with
// the box and advance, then the rows (each row's bytes; odd rows get the words of each 8 bytes swapped back, as TMEM
// holds them). Empty when its size isn't `size`.
std::vector<uint8_t> build_glyph(const json& g, std::span<const uint8_t> header, uint32_t size) {
    if (!g.is_object() || header.size() < GLYPH_HEADER || !g.contains("rows") || !g["rows"].is_array()) return {};
    std::vector<uint8_t> out(header.begin(), header.begin() + GLYPH_HEADER);
    if (g.contains("box") && g["box"].is_array() && g["box"].size() == 4)
        for (int k = 0; k < 4; k++) out[4 + k] = (uint8_t)g["box"][k].get<int>();
    if (g.contains("advance") && g["advance"].is_number_integer()) out[8] = (uint8_t)g["advance"].get<int>();
    size_t row = 0;
    int r = 0;
    for (const json& line : g["rows"]) {
        const std::string h = line.is_string() ? line.get<std::string>() : "";
        if (row == 0) row = h.size() / 2;
        if (h.size() != 2 * row || row == 0) return {};
        const size_t at = out.size();
        for (size_t k = 0; k < row; k++) {
            const int a = Encoder::hex(h[2 * k]), b = Encoder::hex(h[2 * k + 1]);
            if (a < 0 || b < 0) return {};
            out.push_back((uint8_t)(a << 4 | b));
        }
        if (r++ % 2 == 1)
            for (size_t k = at; k + 8 <= at + row; k += 8) std::rotate(out.begin() + k, out.begin() + k + 4, out.begin() + k + 8);
    }
    if (out.size() != size) return {};
    return out;
}

void load_glyphs(const json& j, jfg::locale::Locale& l, Encoder& enc) {
    const std::span<const uint8_t> fonts = jfg::assets::read_rom(FONTS), glyphs = jfg::assets::read_rom(FONT_GLYPHS);
    const json list_glyphs = j.value("glyphs", json::array());
    for (const json& g : list_glyphs) {
        const int font = g.is_object() ? g.value("font", -1) : -1;
        const int glyph = g.is_object() && g.contains("glyph") && g["glyph"].is_string()
                              ? (int)std::strtol(g["glyph"].get<std::string>().c_str(), nullptr, 16) : -1;
        if (font < 0 || font > 3 || glyph < 0 || glyph > 255 || fonts.size() < 4 * FONT_DESC) {
            enc.notes.push_back("a glyph needs \"font\" 0-3 and \"glyph\" (hex): " + g.dump().substr(0, 60));
            continue;
        }
        const uint32_t offset = be32(fonts, font * FONT_DESC + 4), size = be32(fonts, font * FONT_DESC + 8) >> 16;
        if ((uint64_t)offset + (glyph + 1) * size > glyphs.size()) continue;
        std::vector<uint8_t> data = build_glyph(g, glyphs.subspan(offset + glyph * size, size), size);
        if (data.empty()) {
            enc.notes.push_back("glyph " + g["glyph"].get<std::string>() + " of font " + std::to_string(font) + ": its rows must make " +
                                std::to_string(size - GLYPH_HEADER) + " bytes");
            continue;
        }
        l.glyphs.push_back({ font, glyph, std::move(data) });
    }
    // Pages (fonts 4-7): their descriptor (else font 1's, 16x12), 256 glyphs each.
    auto pages = j.find("pages");
    if (pages == j.end() || !pages->is_object() || fonts.size() < 2 * FONT_DESC) return;
    std::vector<uint8_t> desc(fonts.begin() + FONT_DESC, fonts.begin() + 2 * FONT_DESC);
    if (pages->contains("descriptor") && (*pages)["descriptor"].is_string()) {
        const std::string h = (*pages)["descriptor"].get<std::string>();
        if (h.size() == 2 * FONT_DESC)
            for (size_t k = 0; k < FONT_DESC; k++) desc[k] = (uint8_t)(Encoder::hex(h[2 * k]) << 4 | Encoder::hex(h[2 * k + 1]));
    }
    const uint32_t size = be32(desc, 8) >> 16;
    const json list = pages->value("glyphs", json::array());
    if (size <= GLYPH_HEADER || list.empty() || list.size() > PAGE_GLYPHS || glyphs.size() < GLYPH_HEADER) return;
    const std::span<const uint8_t> header = glyphs.subspan(be32(fonts, FONT_DESC + 4), GLYPH_HEADER);
    std::vector<uint8_t> blank(header.begin(), header.end());
    blank.resize(size, 0);
    blank[8] = 0;
    const size_t count = (list.size() + 255) / 256 * 256;
    for (size_t i = 0; i < count; i++) {
        std::vector<uint8_t> g = i < list.size() ? build_glyph(list[i], header, size) : std::vector<uint8_t>();
        if (g.empty() && i < list.size() && !list[i].is_null())
            enc.notes.push_back("page glyph " + std::to_string(i) + ": its rows must make " + std::to_string(size - GLYPH_HEADER) + " bytes");
        const std::vector<uint8_t>& use = g.empty() ? blank : g;
        l.pages.insert(l.pages.end(), use.begin(), use.end());
    }
    l.page_font = desc;
}

// --- files -------------------------------------------------------------------------------------------------------------

bool load_file(const std::filesystem::path& path, jfg::locale::Locale& l) {
    const std::string file = path.filename().string();
    std::ifstream f(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json j;
    try {
        j = json::parse(bytes);
    } catch (const json::parse_error& e) {
        log("locale: " + file + " isn't valid JSON (" + e.what() + "); not listed");
        return false;
    }
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) {
        log("locale: " + file + " needs a \"name\"; not listed");
        return false;
    }
    l.id = path.stem().string();
    std::transform(l.id.begin(), l.id.end(), l.id.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    l.name = j["name"].get<std::string>();
    if (j.contains("english_name") && j["english_name"].is_string()) l.english_name = j["english_name"].get<std::string>();
    const json text = j.value("text", json::object()), layout = j.value("layout", json::object());

    Encoder enc{ l };
    const std::span<const uint8_t> rom = recomp::get_rom();
    if (rom.size() >= ROM_ASCII_GLYPHS + 96) enc.ascii_glyphs = rom.subspan(ROM_ASCII_GLYPHS, 96);
    const json characters = j.value("characters", json::object());
    for (const auto& [ch, code] : characters.items()) {
        uint32_t cp = 0;
        const int len = ch.empty() ? 0 : (uint8_t)ch[0] >= 0xF0 ? 4 : (uint8_t)ch[0] >= 0xE0 ? 3 : (uint8_t)ch[0] >= 0xC0 ? 2 : 1;
        if (len == 0 || (size_t)len != ch.size() || !code.is_string()) { enc.notes.push_back("\"characters\": \"" + ch + "\" isn't one character"); continue; }
        cp = len == 1 ? (uint8_t)ch[0] : (uint8_t)ch[0] & (0x7F >> len);
        for (int k = 1; k < len; k++) cp = cp << 6 | ((uint8_t)ch[k] & 0x3F);
        const std::string c = code.get<std::string>();
        const unsigned v = (unsigned)std::strtoul(c.c_str(), nullptr, 16), g = c.size() == 5 ? (unsigned)std::strtoul(c.c_str() + 3, nullptr, 16) : v;
        if (c.size() == 2 && v < 256) enc.characters[cp] = { 0x80, (uint8_t)v };
        else if (c.size() == 5 && c[2] == ':' && v >= 0x80 && v < 256 && g < 256) enc.characters[cp] = { (uint8_t)v, (uint8_t)g };
        else enc.notes.push_back("\"characters\": \"" + ch + "\" needs a glyph \"XX\" or a code \"PP:XX\"");
    }

    size_t known = 0;
    for (const auto& [key, value] : text.items()) known += value.is_string();
    l.menu = build_menu(text, enc);
    l.subtitles = build_subtitles(text, layout, enc);
    build_hints(text, layout, enc, l.hint_table, l.hint_data);
    load_glyphs(j, l, enc);

    std::string line = "locale: " + file + ": " + l.name + ", " + std::to_string(known) + " strings, " +
                       std::to_string(l.glyphs.size()) + " glyphs" + (l.pages.empty() ? "" : ", " + std::to_string(l.pages.size() / (be32(l.page_font, 8) >> 16)) + " page glyphs");
    if (!enc.missing.empty()) {
        line += "; characters with no glyph, written as:";
        for (const auto& [what, n] : enc.missing) line += " " + what + " (" + std::to_string(n) + ")";
    }
    log(line);
    for (const std::string& n : enc.notes) log("locale: " + file + ": " + n);
    return true;
}

// Whether the menus' font draws every character of `text` (UTF-8): Latin, up to U+024F.
bool latin(const std::string& text) {
    for (size_t i = 0; i < text.size(); i++) {
        const uint8_t c = (uint8_t)text[i];
        if (c < 0x80) continue;
        if ((c & 0xE0) != 0xC0 || i + 1 >= text.size()) return false; // 3 and 4 byte sequences are U+0800 and up
        if ((((c & 0x1F) << 6) | ((uint8_t)text[i + 1] & 0x3F)) > 0x24F) return false;
        i++;
    }
    return true;
}

std::string label(const jfg::locale::Locale& l) {
    if (latin(l.name)) return l.name;
    if (!l.english_name.empty()) return l.english_name;
    return l.id;
}

// Older ids: "builtin.<slot>" (the ROM's slots) and "mod.jfg_locale_<name>" (locale mods) are now the files' names.
std::string migrate(std::string id) {
    if (id.rfind("builtin.", 0) == 0) {
        const int slot = std::atoi(id.c_str() + 8);
        return slot > 0 && slot < 5 ? SLOT_IDS[slot] : "";
    }
    if (id.rfind("mod.jfg_locale_", 0) == 0) return id.substr(15);
    if (id.rfind("mod.", 0) == 0) return id.substr(4);
    return id == ENGLISH_ID ? "" : id;
}

void load_choice() {
    if (g_chosen_loaded) return;
    g_chosen_loaded = true;
    std::ifstream f(CHOICE_FILE);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("language=", 0) == 0) { g_chosen = migrate(line.substr(9)); return; }
    }
    // Before locales, the Language option was a slot number (jfg_port_options.txt "language="), read here since the
    // options aren't loaded yet at start-up.
    std::ifstream options("jfg_port_options.txt");
    while (std::getline(options, line)) {
        if (line.rfind("language=", 0) == 0) {
            const int slot = std::atoi(line.c_str() + 9);
            if (slot > 0 && slot < 5) g_chosen = SLOT_IDS[slot];
        }
    }
}

// locales/*.json, by name.
std::vector<std::filesystem::path> locale_files() {
    std::vector<std::filesystem::path> files;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(FOLDER, ec)) {
        std::string ext = e.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        if (e.is_regular_file(ec) && ext == ".json") files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    return files;
}

// Under g_mutex: the Language list from `locales` (only id, name and english_name are read). english.json names the
// first entry; two that look alike get their file's name.
void set_list(const std::vector<jfg::locale::Locale>& locales) {
    std::map<std::string, int> names;
    for (const jfg::locale::Locale& l : locales) names[label(l)]++;
    g_list = { { "", "English" } };
    for (const jfg::locale::Locale& l : locales) {
        if (l.id == ENGLISH_ID) { g_list[0].second = label(l); continue; }
        g_list.emplace_back(l.id, names[label(l)] > 1 ? label(l) + " (" + l.id + ".json)" : label(l));
    }
    g_listed = true;
}

// Under g_mutex, before the registry (the launcher's Language list): each file's names, without building its text,
// which needs the ROM.
void list_names() {
    std::vector<jfg::locale::Locale> locales;
    for (const std::filesystem::path& p : locale_files()) {
        std::ifstream f(p, std::ios::binary);
        const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const json j = json::parse(bytes, nullptr, false);
        if (j.is_discarded() || !j.is_object() || !j.contains("name") || !j["name"].is_string()) continue;
        jfg::locale::Locale l;
        l.id = p.stem().string();
        std::transform(l.id.begin(), l.id.end(), l.id.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        l.name = j["name"].get<std::string>();
        if (j.contains("english_name") && j["english_name"].is_string()) l.english_name = j["english_name"].get<std::string>();
        locales.push_back(std::move(l));
    }
    set_list(locales);
}

} // namespace

// The registry, at start-up before the game runs (jfg_locale.cpp, jfg_locale_start): every locales/*.json, by file
// name. english.json, when there is one, is shown in place of the ROM's English.
extern "C" void jfg_locale_build_registry(void) {
    g_locales.clear();
    for (const std::filesystem::path& p : locale_files()) {
        jfg::locale::Locale l;
        if (load_file(p, l) && !l.menu.empty()) g_locales.push_back(std::move(l));
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    set_list(g_locales);
    load_choice();
    char line[160];
    std::snprintf(line, sizeof line, "locale: %zu locale files in %s/", g_locales.size(), FOLDER);
    log(line);
}

namespace jfg::locale {

const std::vector<Locale>& all() { return g_locales; }

const Locale* find(const std::string& id) {
    const std::string want = id.empty() ? ENGLISH_ID : id;
    for (const Locale& l : g_locales) if (l.id == want) return &l;
    return nullptr;
}

std::string chosen() {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_choice();
    return g_chosen;
}

void choose(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    load_choice();
    if (id != g_chosen) g_ui_switch = true;
    g_chosen = id;
    if (FILE* f = std::fopen(CHOICE_FILE, "w")) {
        std::fprintf(f, "# The game's language: a locale file's name (locales/<name>.json); empty = English.\nlanguage=%s\n", id.c_str());
        std::fclose(f);
    }
}

// Test: JFG_DEBUG_LANGUAGE="secs,id" picks locale `id` ("" = English) that many seconds after the first call, as the
// Language list does (Recomp-Docs/REFERENCE.md). Called by the game tick and the launcher's.
void debug_pick() {
    static const char* pick = std::getenv("JFG_DEBUG_LANGUAGE");
    static const auto start = std::chrono::steady_clock::now();
    if (pick == nullptr || std::chrono::steady_clock::now() - start < std::chrono::seconds(std::atoi(pick))) return;
    const char* comma = std::strchr(pick, ',');
    choose(comma ? comma + 1 : "");
    pick = nullptr;
}

std::vector<std::pair<std::string, std::string>> list() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_listed) list_names();
    return g_list;
}

// --- the port's menus -------------------------------------------------------------------------------------------------

namespace {
std::mutex g_ui_mutex;
bool g_ui_loaded = false;
bool g_ui_accents = false;                     // g_ui has letters beyond ASCII
std::map<std::string, std::string> g_ui;       // English -> the chosen language's
std::map<std::string, std::string> g_exported; // JFG_UI_EXPORT: every text asked for

} // namespace

// `text` without its leading and trailing white space and with each run of white space inside as one space.
std::string ui_key(const std::string& text) {
    std::string out;
    bool space = false;
    for (char c : text) {
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') { space = !out.empty(); continue; }
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

std::string ui_breaks(const std::string& text) {
    // UTF-8 of \u3001\u3002\uFF0C\uFF09\u300D\u300F\uFF1A\uFF01\uFF1F
    static const char* const PUNCTUATION[] = { "\xE3\x80\x81", "\xE3\x80\x82", "\xEF\xBC\x8C", "\xEF\xBC\x89", "\xE3\x80\x8D",
                                               "\xE3\x80\x8F", "\xEF\xBC\x9A", "\xEF\xBC\x81", "\xEF\xBC\x9F" };
    std::string out;
    for (size_t i = 0; i < text.size();) {
        size_t n = 0;
        for (const char* p : PUNCTUATION) {
            if (text.compare(i, std::strlen(p), p) == 0) { n = std::strlen(p); break; }
        }
        if (n == 0) { out.push_back(text[i++]); continue; }
        out.append(text, i, n);
        i += n;
        if (i < text.size() && text[i] != ' ' && text[i] != '<') out.push_back(' ');
    }
    return out;
}

namespace {

// Whether `key` has a Latin letter outside {{ data expressions }}: only those are worth translating (not the
// controller icons' symbols).
bool has_words(const std::string& key) {
    int depth = 0;
    for (size_t i = 0; i < key.size(); i++) {
        if (key.compare(i, 2, "{{") == 0) { depth++; i++; }
        else if (key.compare(i, 2, "}}") == 0 && depth > 0) { depth--; i++; }
        else if (depth == 0 && std::isalpha((unsigned char)key[i])) return true;
    }
    return false;
}

// Under g_ui_mutex.
void load_ui() {
    if (g_ui_loaded) return;
    g_ui_loaded = true;
    std::string id = jfg::locale::chosen();
    if (id.empty()) id = ENGLISH_ID;
    std::ifstream f(std::filesystem::path(FOLDER) / (id + ".json"), std::ios::binary);
    if (!f) return;
    const std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const json j = json::parse(bytes, nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("ui") || !j["ui"].is_object()) return;
    for (const auto& [english, text] : j["ui"].items())
        if (text.is_string() && !text.get_ref<const std::string&>().empty()) g_ui[ui_key(english)] = text.get<std::string>();
    std::map<std::string, std::string> seen; // switching the menus' language reads each text back to its English
    for (const auto& [english, text] : g_ui) {
        for (char c : text) if ((uint8_t)c >= 0x80) g_ui_accents = true;
        auto [it, added] = seen.emplace(ui_key(text), english);
        if (!added) log("locale: " + id + ".json: \"" + it->second + "\" and \"" + english + "\" have the same translation; "
                        "switching language may swap them");
    }
    log("locale: " + id + ".json: " + std::to_string(g_ui.size()) + " menu texts");
}

// Under g_ui_mutex: JFG_UI_EXPORT=<file> collects `key` (rewritten whenever one is new).
void export_key(const std::string& key) {
    static const char* path = std::getenv("JFG_UI_EXPORT");
    if (path == nullptr || g_exported.count(key) || !has_words(key)) return;
    g_exported[key] = key;
    std::ofstream(path, std::ios::binary) << json(g_exported).dump(1);
}

const std::string* lookup(const std::string& key) {
    load_ui();
    export_key(key);
    auto it = g_ui.find(key);
    return it == g_ui.end() ? nullptr : &it->second;
}
} // namespace

std::string ui(const std::string& english) {
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    const std::string* t = lookup(ui_key(english));
    return t ? ui_breaks(*t) : english;
}

bool ui_has_accents() {
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    load_ui();
    return g_ui_accents;
}

bool take_ui_switch(std::map<std::string, std::string>& old_ui, std::map<std::string, std::string>& new_ui) {
    if (!g_ui_switch.exchange(false)) return false;
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    load_ui();
    old_ui = g_ui;
    g_ui.clear();
    g_ui_loaded = g_ui_accents = false;
    load_ui();
    new_ui = g_ui;
    return true;
}

bool translate_ui(std::string& out, const std::string& in) {
    const std::string key = ui_key(in);
    std::lock_guard<std::mutex> lock(g_ui_mutex);
    const std::string* t = key.empty() ? nullptr : lookup(key);
    if (t == nullptr) { out = in; return false; }
    // The white space around the text stays: it separates it from the elements next to it.
    const size_t first = in.find_first_not_of(" \t\r\n"), last = in.find_last_not_of(" \t\r\n");
    out = in.substr(0, first) + ui_breaks(*t) + in.substr(last + 1);
    return true;
}

} // namespace jfg::locale
