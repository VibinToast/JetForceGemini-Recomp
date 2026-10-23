// Jet Force Gemini port: French, German and Spanish from the player's own European ROM (NJFP, En/Fr/De/Es).
//
// The launcher's Languages row (src/ui/ui_launcher.cpp) passes the ROM the player picks to jfg_locale_import_europe,
// which builds the text assets the way the decomp's tools/text.py does with `import` and `pack`, from the stored US
// ROM (jfg-us.z64 in the app folder) and the European one, and writes them to locale/europe.bin there. At start-up
// jfg_assets.cpp applies that file like an asset mod below every real mod (jfg_locale_replacements). The file comes
// from the player's ROMs, so it is never packaged (tools/package_release.ps1 lists what ships).
//
// What is built (formats in text.py's notes and jfg_locale.cpp):
//   - Menus (assets 6/7): the European ROM has 536 strings per language, the US 532. US string i is European string i,
//     or i + 1 from 296 on. Strings the European ROM leaves empty come from that language's US block, else English.
//   - Subtitles (4/5): the European ROM keeps 47 entries per language; they become the port's 51-entry blocks
//     (jfg_locale.cpp), with the US entries 47-50 kept in each.
//   - Hints: the European assets 0x0B-0x10 (French, German, Spanish tables and data) as they are.
//   - Fonts (0x3B): the European ROM's accented glyphs 83-8E (font 2), and lower case versions drawn at the same codes
//     in font 0 (base letter plus accent, as text.py's draw_font0_accents).
// Everything is copied as bytes; no string is decoded beyond finding where it ends.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "config/config.hpp"

namespace {

constexpr uint32_t US_LUT = 0xB1750, US_BLOB = 0xB1880;
constexpr uint32_t EU_LUT = 0xB1A30, EU_BLOB = 0xB1B60;
constexpr int GAME_TEXT = 0x04, GAME_TEXT_TABLE = 0x05, FRONT_TEXT = 0x06, FRONT_TEXT_TABLE = 0x07;
constexpr int FONTS = 0x3A, FONT_GLYPHS = 0x3B;
constexpr int EU_HINT_FIRST = 0x0B, EU_HINT_LAST = 0x10; // French, German, Spanish table/data pairs
constexpr int EU_SUBTITLES = 47;    // entries per language in the European ROM
constexpr int SUBTITLE_BLOCK = 51;  // the port's entries per language (jfg_locale.cpp)
constexpr int EU_FRONT_EXTRA = 296; // the European ROM has one more menu string here
constexpr uint32_t FILE_VERSION = 1;
constexpr char FILE_MAGIC[8] = { 'J', 'F', 'G', 'L', 'O', 'C', 'A', 'L' };

using Bytes = std::vector<uint8_t>;

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    if (o + 4 > b.size()) throw std::runtime_error("read past the end of an asset");
    return (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3];
}
void put32(Bytes& b, uint32_t v) {
    b.insert(b.end(), { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v });
}
std::vector<int32_t> words(std::span<const uint8_t> b) {
    std::vector<int32_t> w;
    for (size_t o = 0; o + 4 <= b.size(); o += 4) w.push_back((int32_t)be32(b, o));
    return w;
}
size_t index_of(const std::vector<int32_t>& w, int32_t v, size_t from = 0) {
    for (size_t i = from; i < w.size(); i++) if (w[i] == v) return i;
    throw std::runtime_error("a table has no end marker");
}

struct Rom {
    Bytes data;
    uint32_t lut, blob;
    std::span<const uint8_t> asset(int i) const {
        const uint32_t start = be32(data, lut + 4 + 4 * i), end = be32(data, lut + 8 + 4 * i);
        if (end < start || (uint64_t)blob + end > data.size()) throw std::runtime_error("bad asset table");
        return std::span<const uint8_t>(data).subspan(blob + start, end - start);
    }
};

std::optional<Bytes> read_file(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return std::nullopt;
    return Bytes(std::istreambuf_iterator<char>(f), {});
}

// The end of a 0 ended string at pos: a byte with bit 7 set starts a two byte glyph code (text.py decode_string).
size_t string_end(std::span<const uint8_t> b, size_t pos) {
    while (true) {
        if (pos >= b.size()) throw std::runtime_error("a string runs past its asset");
        const uint8_t c = b[pos];
        if (c == 0) return pos + 1;
        pos += (c & 0x80) ? 2 : 1;
    }
}

// --- menus ------------------------------------------------------------------------------------------------------------
using MenuString = std::optional<Bytes>; // nullopt = no string (-1); an empty string is an empty vector

std::vector<MenuString> menu_strings(std::span<const uint8_t> block, uint32_t count) {
    const uint32_t blank = 4 * count - 1; // the European ROM points the strings it dropped here
    std::vector<MenuString> out;
    for (uint32_t i = 0; i < count; i++) {
        const int32_t o = (int32_t)be32(block, 4 * i);
        if (o == -1) out.emplace_back();
        else if ((uint32_t)o == blank) out.emplace_back(Bytes{});
        else {
            if ((uint32_t)o < 4 * count || (uint32_t)o >= block.size()) throw std::runtime_error("bad menu string offset");
            const size_t end = string_end(block, o);
            out.emplace_back(Bytes(block.begin() + o, block.begin() + end - 1));
        }
    }
    return out;
}

bool has_text(const MenuString& s) { return s.has_value() && !s->empty(); }

Bytes build_menu_block(const std::vector<MenuString>& strings) {
    const uint32_t count = (uint32_t)strings.size();
    Bytes offsets, body;
    for (const MenuString& s : strings) {
        if (!s) { put32(offsets, 0xFFFFFFFFu); continue; }
        put32(offsets, 4 * count + (uint32_t)body.size());
        body.insert(body.end(), s->begin(), s->end());
        body.push_back(0);
    }
    while ((4 * count + body.size()) % 8) body.push_back(0);
    offsets.insert(offsets.end(), body.begin(), body.end());
    return offsets;
}

void build_menus(const Rom& us, const Rom& eu, std::vector<std::pair<int, Bytes>>& out) {
    const std::vector<int32_t> table = words(us.asset(FRONT_TEXT_TABLE)), eu_table = words(eu.asset(FRONT_TEXT_TABLE));
    const std::span<const uint8_t> front = us.asset(FRONT_TEXT), eu_front = eu.asset(FRONT_TEXT);
    const uint32_t count = (uint32_t)table.at(0), eu_count = (uint32_t)eu_table.at(0);
    const size_t end = index_of(table, -1, 1), eu_end = index_of(eu_table, -1, 1);
    const std::vector<int32_t> starts(table.begin() + 1, table.begin() + end), eu_starts(eu_table.begin() + 1, eu_table.begin() + eu_end);
    if (starts.size() < 5 || eu_starts.size() < 5 || eu_count != count + 4) throw std::runtime_error("unexpected menu text layout");
    auto block = [](std::span<const uint8_t> f, const std::vector<int32_t>& s, size_t l) { return f.subspan(s[l], s[l + 1] - s[l]); };
    const std::vector<MenuString> english = menu_strings(block(front, starts, 0), count);

    Bytes data, new_table;
    put32(new_table, count);
    for (size_t l = 0; l + 1 < starts.size(); l++) {
        put32(new_table, (uint32_t)data.size());
        if (l < 1 || l > 3) { // English and the stubs stay as they are
            const std::span<const uint8_t> b = block(front, starts, l);
            data.insert(data.end(), b.begin(), b.end());
            continue;
        }
        const std::vector<MenuString> eu_strings = menu_strings(block(eu_front, eu_starts, l), eu_count);
        // The US ROM's Spanish stub points every string at its own end: no strings (text.py keeps it raw).
        std::vector<MenuString> us_lang(count);
        try { us_lang = menu_strings(block(front, starts, l), count); } catch (const std::runtime_error&) {}
        std::vector<MenuString> strings;
        for (uint32_t i = 0; i < count; i++) {
            MenuString s = eu_strings[i < EU_FRONT_EXTRA ? i : i + 1];
            if (!has_text(s) && has_text(english[i])) s = has_text(us_lang[i]) ? us_lang[i] : english[i];
            strings.push_back(std::move(s));
        }
        const Bytes b = build_menu_block(strings);
        data.insert(data.end(), b.begin(), b.end());
    }
    put32(new_table, (uint32_t)data.size());
    data.insert(data.end(), front.begin() + starts.back(), front.end());
    put32(new_table, 0xFFFFFFFFu);
    for (size_t i = end + 1; i < table.size(); i++) put32(new_table, (uint32_t)table[i]);
    out.emplace_back(FRONT_TEXT, std::move(data));
    out.emplace_back(FRONT_TEXT_TABLE, std::move(new_table));
}

// --- subtitles --------------------------------------------------------------------------------------------------------
struct Entry { uint8_t flags; Bytes data; };

// An entry's bytes: kept as they are when 8-aligned, else its lines and end padded to 8 (text.py build_game_entry).
Entry game_entry(std::span<const uint8_t> data, const std::vector<int32_t>& table, size_t i) {
    const uint32_t start = table[i] & 0xFFFFFF, end = table[i + 1] & 0xFFFFFF;
    if (end < start || end > data.size()) throw std::runtime_error("bad subtitle offset");
    Entry e{ (uint8_t)((uint32_t)table[i] >> 24), {} };
    if ((end - start) % 8 == 0) {
        e.data.assign(data.begin() + start, data.begin() + end);
        return e;
    }
    size_t pos = start;
    while (data[pos] != 0) {
        pos = string_end(data, pos + 8);
        if (data[pos] == 0x0A || data[pos] == 0x0C) pos++;
    }
    pos++;
    e.data.assign(data.begin() + start, data.begin() + pos);
    while (e.data.size() % 8) e.data.push_back(0);
    return e;
}

void build_subtitles(const Rom& us, const Rom& eu, std::vector<std::pair<int, Bytes>>& out) {
    const std::vector<int32_t> table = words(us.asset(GAME_TEXT_TABLE)), eu_table = words(eu.asset(GAME_TEXT_TABLE));
    const std::span<const uint8_t> data = us.asset(GAME_TEXT), eu_data = eu.asset(GAME_TEXT);
    const size_t n = index_of(table, -1) - 1, eu_n = index_of(eu_table, -1) - 1;
    if (n != SUBTITLE_BLOCK || eu_n < 4 * EU_SUBTITLES) throw std::runtime_error("unexpected subtitle layout");
    std::vector<Entry> entries;
    for (size_t i = 0; i < n; i++) entries.push_back(game_entry(data, table, i));
    for (size_t l = 1; l <= 3; l++) {
        for (size_t i = 0; i < EU_SUBTITLES; i++) entries.push_back(game_entry(eu_data, eu_table, l * EU_SUBTITLES + i));
        for (size_t i = EU_SUBTITLES; i < n; i++) entries.push_back(game_entry(data, table, i));
    }
    Bytes new_data, new_table;
    for (const Entry& e : entries) {
        put32(new_table, (uint32_t)e.flags << 24 | (uint32_t)new_data.size());
        new_data.insert(new_data.end(), e.data.begin(), e.data.end());
    }
    put32(new_table, ((uint32_t)table[n] & 0xFF000000u) | (uint32_t)new_data.size());
    new_data.insert(new_data.end(), data.begin() + (table[n] & 0xFFFFFF), data.end());
    put32(new_table, 0xFFFFFFFFu);
    for (size_t i = n + 2; i < table.size(); i++) put32(new_table, (uint32_t)table[i]);
    out.emplace_back(GAME_TEXT, std::move(new_data));
    out.emplace_back(GAME_TEXT_TABLE, std::move(new_table));
}

// --- fonts ------------------------------------------------------------------------------------------------------------
constexpr uint8_t EU_GLYPHS[] = { 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8C, 0x8D, 0x8E }; // ¡ ¿ ä ö ü á ó ú í é ñ
enum Mark { ACUTE, UMLAUT, TILDE, FLIP };
struct Accent { uint8_t glyph, base; Mark mark; };
// Font 0 has katakana at those codes: lower case letters drawn over them (text.py FONT0_ACCENTS and MARKS).
constexpr Accent FONT0_ACCENTS[] = {
    { 0x85, 0x95, UMLAUT }, { 0x86, 0xA3, UMLAUT }, { 0x87, 0xA9, UMLAUT }, { 0x88, 0x95, ACUTE }, { 0x89, 0xA3, ACUTE },
    { 0x8A, 0xA9, ACUTE }, { 0x8C, 0x9D, ACUTE }, { 0x8D, 0x99, ACUTE }, { 0x8E, 0xA2, TILDE }, { 0x83, 0x34, FLIP },
    { 0x84, 0x40, FLIP },
};
struct Point { int x, y; };
const std::vector<Point> MARK_POINTS[3] = {
    { { 8, 1 }, { 9, 1 }, { 9, 0 }, { 10, 0 } },
    { { 4, 0 }, { 5, 0 }, { 4, 1 }, { 5, 1 }, { 9, 0 }, { 10, 0 }, { 9, 1 }, { 10, 1 } },
    { { 3, 1 }, { 4, 0 }, { 5, 0 }, { 6, 1 }, { 7, 1 }, { 8, 0 }, { 9, 0 }, { 10, 1 } },
};
constexpr int F0_W = 16, F0_H = 16; // font 0: 16x16, 4 bits per pixel

// Odd rows have the two words of each 8 byte group swapped (TMEM); the same call undoes it.
void swap_odd_rows(std::span<uint8_t> pix, int row_bytes, int height) {
    for (int r = 1; r < height; r += 2)
        for (int i = r * row_bytes; i + 8 <= (r + 1) * row_bytes; i += 8)
            std::swap_ranges(pix.begin() + i, pix.begin() + i + 4, pix.begin() + i + 4);
}

void build_fonts(const Rom& us, const Rom& eu, std::vector<std::pair<int, Bytes>>& out) {
    const std::span<const uint8_t> fonts = us.asset(FONTS), eu_fonts = eu.asset(FONTS), eu_glyphs = eu.asset(FONT_GLYPHS);
    const std::span<const uint8_t> glyphs = us.asset(FONT_GLYPHS);
    Bytes data(glyphs.begin(), glyphs.end());
    auto layout = [](std::span<const uint8_t> f, int k) {
        return std::pair<uint32_t, uint32_t>(be32(f, 16 * k + 4), be32(f, 16 * k + 8) >> 16);
    };
    const auto [off2, size2] = layout(fonts, 2);
    const auto [eu_off2, eu_size2] = layout(eu_fonts, 2);
    if (size2 != eu_size2) throw std::runtime_error("unexpected font layout");
    for (uint8_t g : EU_GLYPHS) {
        if (eu_off2 + (g + 1) * size2 > eu_glyphs.size() || off2 + (g + 1) * size2 > data.size()) throw std::runtime_error("bad font");
        std::copy_n(eu_glyphs.begin() + eu_off2 + g * size2, size2, data.begin() + off2 + g * size2);
    }
    const auto [off0, size0] = layout(fonts, 0);
    if (size0 != 16 + F0_W * F0_H / 2) throw std::runtime_error("unexpected font 0 layout");
    for (const Accent& a : FONT0_ACCENTS) {
        Bytes g(glyphs.begin() + off0 + a.base * size0, glyphs.begin() + off0 + (a.base + 1) * size0);
        std::span<uint8_t> pix(g.data() + 16, size0 - 16);
        swap_odd_rows(pix, F0_W / 2, F0_H);
        std::vector<uint8_t> v(F0_W * F0_H);
        for (int i = 0; i < F0_W * F0_H; i++) v[i] = (i % 2 == 0) ? pix[i / 2] >> 4 : pix[i / 2] & 15;
        if (a.mark == FLIP) { // ¡ and ¿: ! and ? turned round
            std::reverse(v.begin(), v.end());
            const uint8_t x0 = g[4], y0 = g[5], x1 = g[6], y1 = g[7];
            g[4] = F0_W - x1; g[6] = F0_W - x0; g[5] = F0_H - y1; g[7] = F0_H - y0;
        } else { // clear the top three rows (the dot of the i) and draw the mark there
            std::fill(v.begin(), v.begin() + 3 * F0_W, 0);
            for (const Point& p : MARK_POINTS[a.mark]) v[p.y * F0_W + p.x] = 15;
            g[5] = 0;
        }
        for (int i = 0; i < F0_W * F0_H; i += 2) pix[i / 2] = (uint8_t)(v[i] << 4 | v[i + 1]);
        swap_odd_rows(pix, F0_W / 2, F0_H);
        std::copy(g.begin(), g.end(), data.begin() + off0 + a.glyph * size0);
    }
    out.emplace_back(FONT_GLYPHS, std::move(data));
}

std::filesystem::path locale_folder() { return dino::config::get_app_folder_path() / "locale"; }

} // namespace

// Builds locale/europe.bin from the European ROM at `path`. Returns an empty string, or what went wrong.
std::string jfg_locale_import_europe(const std::filesystem::path& path) {
    std::optional<Bytes> eu_data = read_file(path);
    if (!eu_data) return "The file could not be opened.";
    if (eu_data->size() < EU_BLOB || std::memcmp(eu_data->data(), "\x80\x37\x12\x40", 4) != 0)
        return "This is not a Nintendo 64 ROM in .z64 format.";
    if (std::memcmp(eu_data->data() + 0x3B, "NJFP", 4) != 0)
        return "This is not the European Jet Force Gemini ROM (En, Fr, De, Es).";
    std::optional<Bytes> us_data = read_file(dino::config::get_app_folder_path() / "jfg-us.z64");
    if (!us_data) return "Load your US ROM on the Play page first.";
    const Rom us{ std::move(*us_data), US_LUT, US_BLOB }, eu{ std::move(*eu_data), EU_LUT, EU_BLOB };
    std::vector<std::pair<int, Bytes>> assets;
    try {
        if (be32(eu.data, EU_LUT) != be32(us.data, US_LUT)) return "This European ROM has an unexpected layout.";
        build_menus(us, eu, assets);
        build_subtitles(us, eu, assets);
        for (int i = EU_HINT_FIRST; i <= EU_HINT_LAST; i++) {
            const std::span<const uint8_t> a = eu.asset(i);
            assets.emplace_back(i, Bytes(a.begin(), a.end()));
        }
        build_fonts(us, eu, assets);
    } catch (const std::exception& e) {
        return std::string("The European ROM could not be read (") + e.what() + ").";
    }
    std::error_code ec;
    std::filesystem::create_directories(locale_folder(), ec);
    const std::filesystem::path out = locale_folder() / "europe.bin", tmp = locale_folder() / "europe.bin.tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        Bytes head(FILE_MAGIC, FILE_MAGIC + 8);
        put32(head, FILE_VERSION);
        put32(head, (uint32_t)assets.size());
        f.write((const char*)head.data(), head.size());
        for (const auto& [index, data] : assets) {
            Bytes h;
            put32(h, (uint32_t)index);
            put32(h, (uint32_t)data.size());
            f.write((const char*)h.data(), h.size());
            f.write((const char*)data.data(), data.size());
        }
        if (!f) return "locale/europe.bin could not be written.";
    }
    std::filesystem::rename(tmp, out, ec);
    if (ec) return "locale/europe.bin could not be written.";
    return {};
}

// The languages locale/ adds, for the launcher ("French, German and Spanish" or empty).
std::string jfg_locale_installed() {
    std::error_code ec;
    return std::filesystem::exists(locale_folder() / "europe.bin", ec) ? "French, German and Spanish" : "";
}

// For jfg_assets.cpp: the assets in locale/*.bin (asset index, data), applied below every mod.
// Scripted testing: JFG_LOCALE_IMPORT=<European ROM> builds locale/europe.bin first, as the launcher's button does.
std::vector<std::pair<int, std::vector<uint8_t>>> jfg_locale_replacements(std::string& error) {
    if (const char* rom = std::getenv("JFG_LOCALE_IMPORT")) {
        const std::string result = jfg_locale_import_europe(std::filesystem::u8path(rom));
        std::fprintf(stderr, "[jfg] JFG_LOCALE_IMPORT: %s\n", result.empty() ? "locale/europe.bin written" : result.c_str());
        if (!result.empty()) error = "JFG_LOCALE_IMPORT: " + result;
    }
    std::vector<std::pair<int, Bytes>> out;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(locale_folder(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".bin") continue;
        std::optional<Bytes> file = read_file(it->path());
        if (!file || file->size() < 16 || std::memcmp(file->data(), FILE_MAGIC, 8) != 0 || be32(*file, 8) != FILE_VERSION) {
            error = it->path().filename().string() + " is not a language file of this version; add the languages again";
            continue;
        }
        const uint32_t count = be32(*file, 12);
        size_t pos = 16;
        for (uint32_t i = 0; i < count; i++) {
            if (pos + 8 > file->size()) break;
            const uint32_t index = be32(*file, pos), size = be32(*file, pos + 4);
            pos += 8;
            if (pos + size > file->size()) break;
            out.emplace_back((int)index, Bytes(file->begin() + pos, file->begin() + pos + size));
            pos += size;
        }
    }
    return out;
}
