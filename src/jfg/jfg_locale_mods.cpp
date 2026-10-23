// Jet Force Gemini port: which locales exist and which is chosen (jfg_locale.h). Showing it: jfg_locale.cpp.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "jfg_assets.h"
#include "jfg_locale.h"
#include "jfg_options.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

constexpr char CONTENT_FILE[] = "jfg_locale.json";
constexpr char CHOICE_FILE[] = "jfg_language.txt";
constexpr int FRONT_TEXT = 0x06, FRONT_TEXT_TABLE = 0x07, GAME_TEXT = 0x04, GAME_TEXT_TABLE = 0x05;
constexpr int FONTS = 0x3A, FONT_GLYPHS = 0x3B;
constexpr uint32_t FONT_DESC = 0x10, HINT_TABLE_STUB = 0x10, HINT_DATA_STUB = 0x2E0;
// The ROM's language slots: hint table and data assets (the US loader reads Spanish's data from 0x0A; a European
// import puts it in 0x10) and the name a built-in locale gets.
constexpr int SLOT_HINT_TABLE[5] = { 0x09, 0x0B, 0x0D, 0x0F, 0x11 };
constexpr int SLOT_HINT_DATA[5] = { 0x0A, 0x0C, 0x0E, 0x10, 0x12 };
const char* const SLOT_NAMES[5] = { "English", "French", "German", "Spanish", "Japanese" };

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return o + 4 <= b.size() ? (uint32_t)b[o] << 24 | b[o + 1] << 16 | b[o + 2] << 8 | b[o + 3] : 0xFFFFFFFFu;
}

struct Pending {
    size_t order;
    jfg::locale::Locale locale;
};
std::vector<Pending> g_pending; // locale mods, filled by load_mods
std::vector<jfg::locale::Locale> g_locales;
std::mutex g_mutex; // g_list and g_chosen (read by the menus)
std::vector<std::pair<std::string, std::string>> g_list = { { "", "English" } };
std::string g_chosen;
bool g_chosen_loaded = false;

// A table/data pair in the subtitle format: entries [first, first + count).
std::vector<jfg::locale::SubtitleEntry> subtitle_entries(std::span<const uint8_t> table, std::span<const uint8_t> data,
                                                         size_t first, size_t count) {
    std::vector<jfg::locale::SubtitleEntry> out;
    for (size_t i = first; i < first + count; i++) {
        const uint32_t w = be32(table, 4 * i), next = be32(table, 4 * i + 4);
        const uint32_t start = w & 0xFFFFFF, end = next & 0xFFFFFF;
        if (w == 0xFFFFFFFFu || next == 0xFFFFFFFFu || end < start || end > data.size()) return {};
        out.push_back({ (uint8_t)(w >> 24), std::vector<uint8_t>(data.begin() + start, data.begin() + end) });
    }
    return out;
}

// Menu block `slot` of the ROM, when it holds strings (a stub is only its offsets, or points them all past itself).
std::vector<uint8_t> rom_menu_block(int slot) {
    const std::span<const uint8_t> table = jfg::assets::read_rom(FRONT_TEXT_TABLE), front = jfg::assets::read_rom(FRONT_TEXT);
    const uint32_t count = be32(table, 0), start = be32(table, 4 * (slot + 1)), end = be32(table, 4 * (slot + 2));
    if (end == 0xFFFFFFFFu || start == 0xFFFFFFFFu || end <= start || end > front.size() || end - start <= count * 4) return {};
    const std::span<const uint8_t> block = front.subspan(start, end - start);
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t o = be32(block, 4 * i);
        if (o != 0xFFFFFFFFu && o != 4 * count - 1 && (o < 4 * count || o >= block.size())) return {};
    }
    return std::vector<uint8_t>(block.begin(), block.end());
}

// The ROM's language slots 1-4 that have menu text, as built-in locales.
void add_builtin() {
    const std::span<const uint8_t> gtable = jfg::assets::read_rom(GAME_TEXT_TABLE), gdata = jfg::assets::read_rom(GAME_TEXT);
    size_t entries = 0;
    while (4 * entries + 4 <= gtable.size() && be32(gtable, 4 * entries) != 0xFFFFFFFFu) entries++;
    if (entries > 0) entries--; // the last word before -1 is the end
    const std::span<const uint8_t> fonts = jfg::assets::read_rom(FONTS), glyphs = jfg::assets::read_rom(FONT_GLYPHS);
    for (int slot = 1; slot < 5; slot++) {
        jfg::locale::Locale l;
        l.menu = rom_menu_block(slot);
        if (l.menu.empty()) continue;
        l.id = "builtin." + std::to_string(slot);
        l.name = l.english_name = std::string(SLOT_NAMES[slot]) + " (game)";
        if (entries >= (size_t)jfg::locale::SUBTITLE_BLOCK * (slot + 1))
            l.subtitles = subtitle_entries(gtable, gdata, jfg::locale::SUBTITLE_BLOCK * slot, jfg::locale::SUBTITLE_BLOCK);
        const std::span<const uint8_t> ht = jfg::assets::read_rom(SLOT_HINT_TABLE[slot]);
        std::span<const uint8_t> hd = jfg::assets::read_rom(SLOT_HINT_DATA[slot]);
        if (slot == 3 && hd.size() <= HINT_DATA_STUB) hd = jfg::assets::read_rom(0x0A); // as the US loader reads it
        if (ht.size() > HINT_TABLE_STUB && hd.size() > HINT_DATA_STUB) {
            l.hint_table.assign(ht.begin(), ht.end());
            l.hint_data.assign(hd.begin(), hd.end());
        }
        // Pages in slot 4 (an asset mod with Japanese, as text.py packs it): descriptors 4-7 of 0x3A, their glyphs
        // at the end of 0x3B.
        if (slot == 4 && fonts.size() >= 8 * FONT_DESC) {
            const uint32_t offset = be32(fonts, 4 * FONT_DESC + 4), size = be32(fonts, 4 * FONT_DESC + 8) >> 16;
            if (size != 0 && (uint64_t)offset + 4ull * 256 * size <= glyphs.size()) {
                l.page_font.assign(fonts.begin() + 4 * FONT_DESC, fonts.begin() + 5 * FONT_DESC);
                l.pages.assign(glyphs.begin() + offset, glyphs.begin() + offset + 4 * 256 * size);
            }
        }
        g_locales.push_back(std::move(l));
    }
}

std::vector<uint8_t> mod_file(const recomp::mods::ModHandle& mod, const nlohmann::json& j, const char* key, bool& missing) {
    if (!j.contains(key)) return {};
    const std::string name = j[key].is_string() ? j[key].get<std::string>() : "";
    bool ok = false;
    std::vector<char> bytes = mod.manifest.file_handle->read_file(name, ok);
    const size_t slash = name.find_last_of("/\\");
    if (!ok && slash != std::string::npos) bytes = mod.manifest.file_handle->read_file(name.substr(slash + 1), ok);
    if (!ok) {
        log("locale: " + mod.manifest.mod_id + ": " + name + " isn't in the mod (add it to additional_files in mod.toml)");
        missing = true;
        return {};
    }
    return std::vector<uint8_t>(bytes.begin(), bytes.end());
}

// ModContentType::on_enabled for jfg_locale.json: once per enabled mod, at every game start.
void on_locale_mod_enabled(recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    const std::string& id = mod.manifest.mod_id;
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (j.is_discarded() || !j.is_object() || !j.contains("name") || !j["name"].is_string()) {
        log("locale: " + id + ": jfg_locale.json isn't valid JSON with a \"name\" - the locale isn't used");
        return;
    }
    jfg::locale::Locale l;
    l.id = "mod." + id;
    l.name = j["name"].get<std::string>();
    if (j.contains("english_name") && j["english_name"].is_string()) l.english_name = j["english_name"].get<std::string>();
    bool missing = false;
    l.menu = mod_file(mod, j, "menu", missing);
    const std::vector<uint8_t> st = mod_file(mod, j, "subtitle_table", missing), sd = mod_file(mod, j, "subtitle_data", missing);
    if (!st.empty()) {
        l.subtitles = subtitle_entries(st, sd, 0, jfg::locale::SUBTITLE_BLOCK);
        if (l.subtitles.empty()) log("locale: " + id + ": the subtitles need " + std::to_string(jfg::locale::SUBTITLE_BLOCK) + " entries; not used");
    }
    l.hint_table = mod_file(mod, j, "hint_table", missing);
    l.hint_data = mod_file(mod, j, "hint_data", missing);
    if (l.hint_table.empty() != l.hint_data.empty()) { l.hint_table.clear(); l.hint_data.clear(); }
    for (const nlohmann::json& g : j.value("glyphs", nlohmann::json::array())) {
        jfg::locale::Glyph glyph;
        glyph.font = g.value("font", -1);
        glyph.glyph = g.contains("glyph") && g["glyph"].is_string() ? (int)std::strtol(g["glyph"].get<std::string>().c_str(), nullptr, 16) : -1;
        glyph.data = mod_file(mod, g, "file", missing);
        if (glyph.font < 0 || glyph.font > 3 || glyph.glyph < 0 || glyph.glyph > 255 || glyph.data.empty()) {
            log("locale: " + id + ": a glyph needs \"font\" 0-3, \"glyph\" (hex) and \"file\": " + g.dump());
            continue;
        }
        l.glyphs.push_back(std::move(glyph));
    }
    l.pages = mod_file(mod, j, "pages", missing);
    l.page_font = mod_file(mod, j, "page_font", missing);
    if (!l.pages.empty() && l.page_font.size() != FONT_DESC) {
        log("locale: " + id + ": \"pages\" needs a 16-byte \"page_font\"; the pages aren't used");
        l.pages.clear();
        l.page_font.clear();
    }
    g_pending.push_back({ context.get_mod_order_index(id), std::move(l) });
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

void load_choice() {
    if (g_chosen_loaded) return;
    g_chosen_loaded = true;
    std::ifstream f(CHOICE_FILE);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("language=", 0) == 0) { g_chosen = line.substr(9); return; }
    }
    // Before locales, the Language option was a slot number (jfg_port_options.txt "language="), read here since the
    // options aren't loaded yet at start-up.
    std::ifstream options("jfg_port_options.txt");
    while (std::getline(options, line)) {
        if (line.rfind("language=", 0) == 0) {
            const int slot = std::atoi(line.c_str() + 9);
            if (slot > 0) g_chosen = "builtin." + std::to_string(slot);
        }
    }
}

} // namespace

// Called from jfg_register_mod_exports (src/main.cpp, before the runtime starts).
extern "C" void jfg_register_locale_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_locale_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
}

// The registry, once mods are loaded and boot assets applied (jfg_locale.cpp, jfg_locale_start). Mods with the same
// name as another locale are told apart by their mod's name.
extern "C" void jfg_locale_build_registry(void) {
    g_locales.clear();
    add_builtin();
    std::stable_sort(g_pending.begin(), g_pending.end(), [](const Pending& a, const Pending& b) { return a.order < b.order; });
    const std::vector<recomp::mods::ModDetails> details = recomp::mods::get_all_mod_details("jet-force-gemini");
    std::map<std::string, int> names;
    for (const jfg::locale::Locale& l : g_locales) names[l.name]++;
    for (const Pending& p : g_pending) names[p.locale.name]++;
    for (Pending& p : g_pending) {
        if (names[p.locale.name] > 1) {
            const std::string mod_id = p.locale.id.substr(4);
            for (const recomp::mods::ModDetails& d : details) {
                if (d.mod_id == mod_id) p.locale.name += " (" + d.display_name + ")";
            }
        }
        g_locales.push_back(std::move(p.locale));
    }
    g_pending.clear();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_list = { { "", "English" } };
    for (const jfg::locale::Locale& l : g_locales) g_list.emplace_back(l.id, label(l));
    load_choice();
    char line[160];
    std::snprintf(line, sizeof line, "locale: %zu locales besides English", g_locales.size());
    log(line);
}

namespace jfg::locale {

const std::vector<Locale>& all() { return g_locales; }

const Locale* find(const std::string& id) {
    for (const Locale& l : g_locales) if (l.id == id) return &l;
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
    g_chosen = id;
    if (FILE* f = std::fopen(CHOICE_FILE, "w")) {
        std::fprintf(f, "# The game's language: the id of a locale (jfg_locale.h); empty = English.\nlanguage=%s\n", id.c_str());
        std::fclose(f);
    }
}

std::vector<std::pair<std::string, std::string>> list() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_list;
}

} // namespace jfg::locale
