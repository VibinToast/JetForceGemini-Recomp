// Jet Force Gemini port: the launcher's mod store (Get Mods page). The design: Recomp-Docs OFFICIAL_MODS_PLAN.md
// "Mod Store".
// - Sources: collections of mods, each one GitHub repo publishing a catalog.json at its latest release; the owner's
//   are built in, each can be turned off (mod_store.json). The list is in priority order: when two sources list the
//   same mod id, the first one's entry is used and the page says another source has it too.
// - A catalog lists each mod with its own version and release: name, descriptions, authors, tags, thumbnail, file URL,
//   size, SHA-256, the port version and mod API it needs, its dependencies.
// - Install / update: the mod's dependencies first, then the mod; each file is downloaded next to its place in the mods
//   folder (<id>.nrm.part), checked against the catalog's SHA-256 (a mismatch installs nothing), then renamed over the
//   old one. The Mods page rescans afterwards; mods take effect when the game starts.
// - Remove: only mods the store manages: ones it installed (mod_store.json) or whose file is a catalog's own (its hash
//   matches), never ones a player put in the mods folder by hand. Their settings are kept.
// - Preview: nothing is published yet, so while bin/mod_store_preview exists (tools/make_mod_store_preview.py) the
//   catalogs and files are read from it, by the same GitHub URLs the real ones will have, with a pretend download time.
//   Without it, a source says it isn't published yet.
#include "jfg_mod_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "json/json.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "jfg_update_check.h"

namespace fs = std::filesystem;

namespace {
constexpr const char* STATE_FILE = "mod_store.json";
constexpr const char* PREVIEW_FOLDER = "mod_store_preview";
constexpr const char* GITHUB = "https://github.com/";

struct BuiltIn { const char* id; const char* name; const char* repo; };
// In priority order. The owner's collections; more are added here (and, later, a reviewed index of others').
const BuiltIn BUILT_IN[] = {
    { "official", "Official Mods", "VibinToast/JFG-Mods" },
    { "restored", "Restored Content", "VibinToast/JFG-Restored-Content" },
};

struct CatalogMod {
    std::string id, name, short_text, description, version, authors, tags, thumb_url, file_url, sha256, min_port;
    uint64_t size = 0;
    std::vector<std::string> dependencies;
};
struct Installed { std::string source, version, sha256; };

std::mutex g_mutex; // guards everything below
std::vector<JfgStoreSource> g_sources;
std::map<std::string, std::vector<CatalogMod>> g_catalogs; // source id -> its mods
std::map<std::string, Installed> g_installed;              // mods the store installed
std::vector<JfgStoreMod> g_mods;
JfgStoreState g_state;
bool g_rescan = false;                                     // an install finished: rescan on the UI thread

bool preview() { return fs::is_directory(PREVIEW_FOLDER); }

std::string utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// A GitHub URL as a file of the preview folder: <owner>/<repo>/releases/latest/download/<f> -> <repo>/latest/<f>,
// <owner>/<repo>/releases/download/<tag>/<f> -> <repo>/<tag>/<f>.
fs::path preview_path(const std::string& url) {
    if (url.rfind(GITHUB, 0) != 0) return {};
    std::string rest = url.substr(std::string(GITHUB).size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos) return {};
    rest = rest.substr(slash + 1); // <repo>/releases/...
    for (const char* from : { "/releases/latest/download/", "/releases/download/" }) {
        const size_t at = rest.find(from);
        if (at == std::string::npos) continue;
        const std::string to = std::string(from) == "/releases/latest/download/" ? "/latest/" : "/";
        return fs::path(PREVIEW_FOLDER) / fs::u8path(rest.substr(0, at) + to + rest.substr(at + std::string(from).size()));
    }
    return {};
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// "1.2.10" -> comparable parts; a missing part is 0.
std::vector<int> version_parts(const std::string& v) {
    std::vector<int> parts;
    int n = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') { n = n * 10 + (c - '0'); any = true; }
        else if (c == '.') { parts.push_back(n); n = 0; any = false; }
        else break;
    }
    if (any || !parts.empty()) parts.push_back(n);
    return parts;
}
bool newer(const std::string& a, const std::string& b) { // a > b
    std::vector<int> x = version_parts(a), y = version_parts(b);
    const size_t n = std::max(x.size(), y.size());
    x.resize(n, 0);
    y.resize(n, 0);
    return x > y;
}

std::string size_text(uint64_t bytes) {
    char text[32];
    if (bytes >= 1048576) std::snprintf(text, sizeof(text), "%.1f MB", bytes / 1048576.0);
    else std::snprintf(text, sizeof(text), "%.0f KB", std::max<double>(1.0, bytes / 1024.0));
    return text;
}

std::string join(const nlohmann::json& list) {
    std::string out;
    if (!list.is_array()) return out;
    for (const auto& v : list) {
        if (!v.is_string()) continue;
        if (!out.empty()) out += ", ";
        out += v.get<std::string>();
    }
    return out;
}

fs::path mod_file(const std::string& id) {
    const fs::path known = recomp::mods::get_mod_filename(id);
    if (!known.empty()) return known;
    return recomp::mods::get_mods_directory() / (id + ".nrm");
}

void load_state() {
    g_installed.clear();
    std::map<std::string, bool> enabled;
    try {
        const nlohmann::json j = nlohmann::json::parse(read_text(STATE_FILE));
        if (j.contains("sources") && j["sources"].is_object()) {
            for (auto& [id, on] : j["sources"].items()) if (on.is_boolean()) enabled[id] = on.get<bool>();
        }
        if (j.contains("installed") && j["installed"].is_object()) {
            for (auto& [id, m] : j["installed"].items()) {
                g_installed[id] = { m.value("source", ""), m.value("version", ""), m.value("sha256", "") };
            }
        }
    } catch (...) {
    }
    g_sources.clear();
    for (const BuiltIn& b : BUILT_IN) {
        JfgStoreSource s;
        s.id = b.id;
        s.name = b.name;
        s.url = std::string(GITHUB) + b.repo + "/releases/latest/download/catalog.json";
        s.official = true;
        s.enabled = enabled.count(b.id) ? enabled[b.id] : true;
        g_sources.push_back(s);
    }
}

void save_state() {
    nlohmann::json j;
    j["sources"] = nlohmann::json::object();
    for (const JfgStoreSource& s : g_sources) j["sources"][s.id] = s.enabled;
    j["installed"] = nlohmann::json::object();
    for (const auto& [id, m] : g_installed) j["installed"][id] = { { "source", m.source }, { "version", m.version }, { "sha256", m.sha256 } };
    std::ofstream(STATE_FILE, std::ios::binary) << j.dump(2) << "\n";
}

void load_catalog(JfgStoreSource& s) {
    std::vector<CatalogMod>& mods = g_catalogs[s.id];
    mods.clear();
    s.loaded = false;
    s.mods = 0;
    s.error.clear();
    if (!preview()) { s.error = "Not published yet"; return; }
    const fs::path file = preview_path(s.url);
    if (file.empty() || !fs::is_regular_file(file)) { s.error = "Not published yet"; return; }
    try {
        const nlohmann::json j = nlohmann::json::parse(read_text(file));
        for (const auto& m : j.value("mods", nlohmann::json::array())) {
            CatalogMod c;
            c.id = m.value("id", "");
            if (c.id.empty()) continue;
            c.name = m.value("name", c.id);
            c.short_text = m.value("short_description", "");
            c.description = m.value("description", "");
            c.version = m.value("version", "");
            c.authors = join(m.value("authors", nlohmann::json::array()));
            c.tags = join(m.value("tags", nlohmann::json::array()));
            c.thumb_url = m.value("thumbnail", "");
            c.file_url = m.value("file", "");
            c.sha256 = m.value("sha256", "");
            c.min_port = m.value("minimum_port_version", "");
            c.size = m.value("size", (uint64_t)0);
            for (const auto& d : m.value("dependencies", nlohmann::json::array())) if (d.is_string()) c.dependencies.push_back(d);
            mods.push_back(c);
        }
        s.loaded = true;
        s.mods = (int)mods.size();
    } catch (...) {
        s.error = "Its catalog could not be read";
    }
}

const CatalogMod* find_entry(const std::string& id, std::string* source = nullptr) {
    for (const JfgStoreSource& s : g_sources) {
        if (!s.enabled) continue;
        for (const CatalogMod& c : g_catalogs[s.id]) {
            if (c.id != id) continue;
            if (source) *source = s.id;
            return &c;
        }
    }
    return nullptr;
}

std::string file_hash(const fs::path& file) {
    std::string hex;
    return fs::is_regular_file(file) && jfg_sha256_file(file, hex) ? hex : std::string();
}

// The merged list (under g_mutex).
void build_list() {
    g_mods.clear();
    const std::string port = recomp::get_project_version().to_string();
    std::map<std::string, size_t> at; // id -> index in g_mods
    for (const JfgStoreSource& s : g_sources) {
        if (!s.enabled) continue;
        for (const CatalogMod& c : g_catalogs[s.id]) {
            if (auto it = at.find(c.id); it != at.end()) {
                if (g_mods[it->second].shadowed.empty()) g_mods[it->second].shadowed = s.name;
                continue;
            }
            JfgStoreMod m;
            m.id = c.id;
            m.name = c.name;
            m.short_text = c.short_text;
            m.description = c.description;
            m.version = c.version;
            m.authors = c.authors;
            m.tags = c.tags;
            m.source = s.id;
            m.source_name = s.name;
            m.size_text = size_text(c.size);
            m.dependencies = c.dependencies;
            const fs::path thumb = preview_path(c.thumb_url);
            if (!thumb.empty() && fs::is_regular_file(thumb)) m.thumb = utf8(thumb);
            if (auto details = recomp::mods::get_details_for_mod(c.id)) {
                m.installed = true;
                m.installed_version = details->version.to_string();
                const std::string hash = file_hash(mod_file(c.id));
                const auto rec = g_installed.find(c.id);
                if (rec != g_installed.end() && !hash.empty() && hash == rec->second.sha256) {
                    m.managed = true;
                    m.installed_version = rec->second.version; // the catalog's version it was installed as
                } else {
                    m.managed = !hash.empty() && hash == c.sha256; // the catalog's own file, put there another way
                }
                m.update = newer(c.version, m.installed_version);
            }
            if (!c.min_port.empty() && newer(c.min_port, port)) {
                m.blocked = true;
                m.blocked_reason = "Needs version " + c.min_port + " of the game";
            }
            at[c.id] = g_mods.size();
            g_mods.push_back(m);
        }
    }
    for (JfgStoreMod& m : g_mods) {
        for (const std::string& d : m.dependencies) {
            if (m.blocked) break;
            if (at.count(d) || recomp::mods::get_details_for_mod(d)) continue;
            m.blocked = true;
            m.blocked_reason = "Needs " + d + ", which no source you have on offers";
        }
    }
    std::sort(g_mods.begin(), g_mods.end(), [](const JfgStoreMod& a, const JfgStoreMod& b) { return a.name < b.name; });
    g_state.preview = preview();
    g_state.serial++;
}

// The mods to download for one install: its dependencies not installed yet (depth first), then itself.
void plan(const std::string& id, std::vector<std::string>& out, int depth = 0) {
    if (depth > 8 || std::find(out.begin(), out.end(), id) != out.end()) return;
    const CatalogMod* c = find_entry(id);
    if (c == nullptr) return;
    for (const std::string& d : c->dependencies) {
        if (!recomp::mods::get_details_for_mod(d)) plan(d, out, depth + 1);
    }
    out.push_back(id);
}

void set_busy(const std::string& text, int percent) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.text = text;
    g_state.percent = percent;
    g_state.serial++;
}

// The background install of a plan (no lock held while copying).
void install_worker(std::vector<std::string> ids, std::string main_id) {
    std::string failure;
    for (size_t k = 0; k < ids.size() && failure.empty(); k++) {
        CatalogMod c;
        std::string source;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            const CatalogMod* e = find_entry(ids[k], &source);
            if (e == nullptr) { failure = ids[k] + " is no longer listed"; break; }
            c = *e;
        }
        const std::string what = (ids.size() > 1 && ids[k] != main_id ? "Installing " + c.name + " (needed)" : "Installing " + c.name);
        const fs::path from = preview_path(c.file_url);
        if (from.empty() || !fs::is_regular_file(from)) { failure = "Could not download " + c.name; break; }
        const fs::path dest = recomp::mods::get_mods_directory() / (c.id + ".nrm");
        const fs::path part = fs::path(dest).concat(".part");
        {
            std::ifstream in(from, std::ios::binary);
            std::ofstream out(part, std::ios::binary | std::ios::trunc);
            const uint64_t total = std::max<uint64_t>(1, fs::file_size(from));
            std::vector<char> buf(64 * 1024);
            uint64_t done = 0;
            // The preview pretends to download: about a second a mod, whatever its size.
            const int steps = std::max<int>(1, (int)((total + buf.size() - 1) / buf.size()));
            const auto pause = std::chrono::milliseconds(std::clamp(900 / steps, 4, 120));
            while (in) {
                in.read(buf.data(), (std::streamsize)buf.size());
                const std::streamsize n = in.gcount();
                if (n <= 0) break;
                out.write(buf.data(), n);
                done += (uint64_t)n;
                set_busy(what + "...", (int)(((double)k + (double)done / total) / ids.size() * 100.0));
                std::this_thread::sleep_for(pause);
            }
            if (!out) failure = "Could not write " + utf8(part.filename());
        }
        std::error_code ec;
        if (failure.empty()) {
            std::string hex;
            if (!jfg_sha256_file(part, hex) || hex != c.sha256) failure = c.name + "'s download didn't match its catalog: not installed";
        }
        if (!failure.empty()) { fs::remove(part, ec); break; }
        // Over the old file: its mod's id may have another file name (installed by hand).
        const fs::path old = mod_file(c.id);
        if (!old.empty() && old != dest) fs::remove(old, ec);
        fs::rename(part, dest, ec);
        if (ec) { failure = "Could not replace " + utf8(dest.filename()) + ": " + ec.message(); fs::remove(part, ec); break; }
        std::lock_guard<std::mutex> lock(g_mutex);
        g_installed[c.id] = { source, c.version, c.sha256 };
        save_state();
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.busy = false;
    g_state.busy_id.clear();
    g_state.percent = failure.empty() ? 100 : 0;
    g_state.text = failure.empty() ? "Installed. It takes effect when the game starts." : failure;
    g_rescan = true;
    g_state.serial++;
}
} // namespace

void jfg_store_refresh() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.busy) return;
    load_state();
    for (JfgStoreSource& s : g_sources) load_catalog(s);
    build_list();
}

std::vector<JfgStoreSource> jfg_store_sources() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_sources;
}

std::vector<JfgStoreMod> jfg_store_mods() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_mods;
}

JfgStoreState jfg_store_state() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state;
}

void jfg_store_set_source_enabled(const std::string& source_id, bool enabled) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (JfgStoreSource& s : g_sources) if (s.id == source_id) s.enabled = enabled;
    save_state();
    build_list();
}

void jfg_store_install(const std::string& mod_id) {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.busy) return;
        plan(mod_id, ids);
        if (ids.empty()) return;
        g_state.busy = true;
        g_state.busy_id = mod_id;
        g_state.percent = 0;
        g_state.text = "Starting...";
        g_state.serial++;
    }
    std::thread(install_worker, ids, mod_id).detach();
}

void jfg_store_remove(const std::string& mod_id) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.busy) return;
        const auto m = std::find_if(g_mods.begin(), g_mods.end(), [&](const JfgStoreMod& x) { return x.id == mod_id; });
        if (m == g_mods.end() || !m->managed) return; // never a mod the player put there by hand
        std::error_code ec;
        fs::remove(mod_file(mod_id), ec);
        g_installed.erase(mod_id);
        save_state();
        text = ec ? "Could not remove it: " + ec.message() : m->name + " removed. Its settings are kept.";
    }
    recomp::mods::scan_mods();
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.text = text;
    build_list();
}

namespace recompui { void update_mod_list(bool scan_mods); } // ui_mod_menu.cpp

void jfg_store_tick() {
    bool rescan = false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        rescan = g_rescan;
        g_rescan = false;
    }
    if (!rescan) return;
    recomp::mods::scan_mods();
    recompui::update_mod_list(false); // the Mods page lists the new files
    std::lock_guard<std::mutex> lock(g_mutex);
    build_list();
}
