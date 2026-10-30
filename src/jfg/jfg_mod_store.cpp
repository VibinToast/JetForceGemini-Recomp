// Jet Force Gemini port: the launcher's mod store (Get Mods page). The design: Recomp-Docs OFFICIAL_MODS_PLAN.md
// "Mod Store".
// - Sources: collections of mods, each one GitHub repo publishing a catalog.json at its latest release; the owner's
//   are built in, and the player can add other people's by "owner/repo" or address (mod_store.json "added", after
//   the built-in ones). Each can be turned off (mod_store.json). The list is in priority order: when two sources list the
//   same mod id, the first one's entry is used and the page says another source has it too.
// - A catalog lists each mod with its own version and release: name, descriptions, authors, tags, thumbnail, file URL,
//   size, SHA-256, the port version and mod API it needs, its dependencies.
// - Install / update: the mod's dependencies first, then the mod; each file is downloaded next to its place in the mods
//   folder (<id>.nrm.part), checked against the catalog's SHA-256 (a mismatch installs nothing), then renamed over the
//   old one. The Mods page rescans afterwards; mods take effect when the game starts. A scan leaves every mod's file
//   open and Windows won't replace or delete an open file, so the mods are closed first (with_mods_closed).
// - Remove: only mods the store manages: ones it installed (mod_store.json) or whose file is a catalog's own (its hash
//   matches), never ones a player put in the mods folder by hand. Their settings are kept.
// - Online: the page's first showing (and its Refresh) downloads each source's catalog in the background, then the
//   thumbnails it lacks into bin/mod_store_cache (<source>/<mod id>-<version>.png, kept between runs); the list fills
//   in as they arrive. Nothing is asked of GitHub while Check for Updates is off (bin/jfg_launcher.txt).
// - Preview (testing): with JFG_MOD_STORE_PREVIEW=1 and bin/mod_store_preview there (tools/make_mod_store_preview.py),
//   the catalogs and files are read from that folder instead, by the same GitHub URLs, with a pretend download time.
#include "jfg_mod_store.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
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
constexpr const char* CACHE_FOLDER = "mod_store_cache";
constexpr const char* GITHUB = "https://github.com/";

struct BuiltIn { const char* id; const char* name; const char* repo; };
// In priority order. The owner's collections; the ones the player added come after them, in the order added.
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
std::vector<std::string> g_added;                          // the player's added collections ("owner/repo"), in order
std::vector<JfgStoreMod> g_mods;
JfgStoreState g_state;
bool g_rescan = false;                                     // an install finished: rescan on the UI thread
bool g_fetching = false;                                   // the catalogs and thumbnails are being downloaded

bool preview() {
    const char* on = std::getenv("JFG_MOD_STORE_PREVIEW");
    return on != nullptr && *on != '\0' && *on != '0' && fs::is_directory(PREVIEW_FOLDER);
}

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

// An added collection's source id (also its cache folder): "gh_<owner>_<repo>", lower case.
std::string added_id(const std::string& repo) {
    std::string id = "gh_" + repo;
    for (char& c : id) c = c == '/' ? '_' : (char)std::tolower((unsigned char)c);
    return id;
}

void load_state() {
    g_installed.clear();
    g_added.clear();
    std::map<std::string, bool> enabled;
    try {
        const nlohmann::json j = nlohmann::json::parse(read_text(STATE_FILE));
        if (j.contains("sources") && j["sources"].is_object()) {
            for (auto& [id, on] : j["sources"].items()) if (on.is_boolean()) enabled[id] = on.get<bool>();
        }
        if (j.contains("added") && j["added"].is_array()) {
            for (const auto& r : j["added"]) if (r.is_string()) g_added.push_back(r.get<std::string>());
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
    for (const std::string& repo : g_added) {
        JfgStoreSource s;
        s.id = added_id(repo);
        s.name = repo; // until its catalog gives its name
        s.url = std::string(GITHUB) + repo + "/releases/latest/download/catalog.json";
        s.removable = true;
        s.enabled = enabled.count(s.id) ? enabled[s.id] : true;
        g_sources.push_back(s);
    }
}

void save_state() {
    nlohmann::json j;
    j["sources"] = nlohmann::json::object();
    for (const JfgStoreSource& s : g_sources) j["sources"][s.id] = s.enabled;
    j["added"] = g_added;
    j["installed"] = nlohmann::json::object();
    for (const auto& [id, m] : g_installed) j["installed"][id] = { { "source", m.source }, { "version", m.version }, { "sha256", m.sha256 } };
    std::ofstream(STATE_FILE, std::ios::binary) << j.dump(2) << "\n";
}

// A source's catalog from its text (the preview's file, or the download).
void parse_catalog(JfgStoreSource& s, const std::string& text) {
    std::vector<CatalogMod>& mods = g_catalogs[s.id];
    mods.clear();
    s.loaded = false;
    s.mods = 0;
    s.error.clear();
    try {
        const nlohmann::json j = nlohmann::json::parse(text);
        if (s.removable && j.contains("name") && j["name"].is_string() && !j["name"].get<std::string>().empty())
            s.name = j["name"].get<std::string>(); // an added collection goes by its own name
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

// A mod's thumbnail file: the preview's, else its place in the cache (there once downloaded).
fs::path thumb_file(const std::string& source_id, const CatalogMod& c) {
    if (c.thumb_url.empty()) return {};
    if (preview()) return preview_path(c.thumb_url);
    return fs::path(CACHE_FOLDER) / fs::u8path(source_id) / fs::u8path(c.id + "-" + c.version + ".png");
}

// url into file, whole: downloaded beside it (.part) and renamed over it once complete.
bool download(const std::string& url, const fs::path& file, uint64_t size = 0,
              const std::function<bool(uint64_t, uint64_t)>& progress = nullptr) {
    std::error_code ec;
    fs::create_directories(file.parent_path(), ec);
    const fs::path part = fs::path(file).concat(".part");
    fs::remove(part, ec);
    if (jfg_http_download(url, part, size, progress ? progress : [](uint64_t, uint64_t) { return true; }) != 1) {
        fs::remove(part, ec);
        return false;
    }
    fs::rename(part, file, ec);
    return !ec;
}

void build_list();

// change (replacing or deleting mod files) with the mods closed; the caller has them scanned again. Only the launcher
// has this page, and mods load when the game starts, so nothing else is using them. A few tries: a virus scanner can
// hold a file for a moment, and a rescan on the UI thread can open them again.
void with_mods_closed(const std::function<void(std::error_code&)>& change, std::error_code& ec) {
    for (int attempt = 0; attempt < 10; attempt++) {
        if (attempt > 0) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        recomp::mods::close_mods();
        ec.clear();
        change(ec);
        if (!ec) return;
    }
}

// The background download of the catalogs, then of the thumbnails not in the cache yet; the list is built again after
// each catalog and once the thumbnails are in.
void fetch_worker(std::vector<std::pair<std::string, std::string>> sources) { // id, catalog url
    for (const auto& [id, url] : sources) {
        const fs::path file = fs::path(CACHE_FOLDER) / fs::u8path(id) / "catalog.json";
        const bool ok = download(url, file);
        std::lock_guard<std::mutex> lock(g_mutex);
        for (JfgStoreSource& s : g_sources) {
            if (s.id != id) continue;
            if (ok) parse_catalog(s, read_text(file));
            else { g_catalogs[id].clear(); s.loaded = false; s.mods = 0; s.error = "Couldn't reach GitHub"; }
        }
        build_list();
    }
    std::vector<std::pair<std::string, fs::path>> thumbs; // url, file
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        for (const auto& [id, mods] : g_catalogs) {
            for (const CatalogMod& c : mods) {
                const fs::path f = thumb_file(id, c);
                if (!f.empty() && !fs::is_regular_file(f)) thumbs.push_back({ c.thumb_url, f });
            }
        }
    }
    for (const auto& [url, f] : thumbs) download(url, f);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fetching = false;
    build_list();
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
            const fs::path thumb = thumb_file(s.id, c);
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
        const fs::path dest = recomp::mods::get_mods_directory() / (c.id + ".nrm");
        const fs::path part = fs::path(dest).concat(".part");
        const fs::path from = preview() ? preview_path(c.file_url) : fs::path();
        if (!preview()) {
            std::error_code rm;
            fs::remove(part, rm);
            const int got = jfg_http_download(c.file_url, part, c.size, [&](uint64_t done, uint64_t total) {
                set_busy(what + "...", (int)(((double)k + (double)done / std::max<uint64_t>(1, total)) / ids.size() * 100.0));
                return true;
            });
            if (got != 1) failure = "Could not download " + c.name + ": check your connection";
        } else if (from.empty() || !fs::is_regular_file(from)) {
            failure = "Could not download " + c.name;
            break;
        } else {
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
        with_mods_closed([&](std::error_code& change_ec) {
            if (!old.empty() && old != dest) fs::remove(old, change_ec);
            if (!change_ec) fs::rename(part, dest, change_ec);
        }, ec);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_rescan = true; // the mods are closed: open them again at once, not after the whole plan
        }
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
    if (g_state.busy || g_fetching) return;
    load_state();
    std::vector<std::pair<std::string, std::string>> fetch;
    for (JfgStoreSource& s : g_sources) {
        if (preview()) {
            const fs::path file = preview_path(s.url);
            if (!file.empty() && fs::is_regular_file(file)) parse_catalog(s, read_text(file));
            else s.error = "Not in the preview";
            continue;
        }
        // The catalog read last time shows until the new one arrives.
        const fs::path cached = fs::path(CACHE_FOLDER) / fs::u8path(s.id) / "catalog.json";
        if (fs::is_regular_file(cached)) parse_catalog(s, read_text(cached));
        if (!jfg_launcher_check_updates()) {
            if (!s.loaded) s.error = "Turn on Check for Updates (General page) to see it";
            continue;
        }
        if (!s.loaded) s.error = "Loading...";
        fetch.push_back({ s.id, s.url });
    }
    build_list();
    if (fetch.empty()) return;
    g_fetching = true;
    std::thread(fetch_worker, fetch).detach();
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

std::string jfg_store_add_source(const std::string& text) {
    // "owner/repo", or the repo's address ("https://github.com/owner/repo/...", with or without "https://").
    std::string repo = text;
    repo.erase(0, repo.find_first_not_of(" \t"));
    repo.erase(repo.find_last_not_of(" \t/") + 1);
    for (const char* prefix : { "https://", "http://", "www.", "github.com/" }) {
        if (repo.rfind(prefix, 0) == 0) repo = repo.substr(std::strlen(prefix));
    }
    if (const size_t slash = repo.find('/'); slash != std::string::npos) {
        if (const size_t next = repo.find('/', slash + 1); next != std::string::npos) repo.resize(next); // owner/repo/releases/...
    }
    if (repo.size() > 4 && repo.compare(repo.size() - 4, 4, ".git") == 0) repo.resize(repo.size() - 4);
    const size_t at = repo.find('/');
    const bool valid = at != std::string::npos && at > 0 && at + 1 < repo.size() &&
        std::all_of(repo.begin(), repo.end(), [](char c) { return std::isalnum((unsigned char)c) || c == '/' || c == '-' || c == '_' || c == '.'; });
    if (!valid) return "Type a GitHub repo as owner/repo, or paste its address";
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.busy || g_fetching) return "Wait for the store to finish first";
        for (const BuiltIn& b : BUILT_IN) if (added_id(b.repo) == added_id(repo)) return "That collection is built in already";
        for (const std::string& r : g_added) if (added_id(r) == added_id(repo)) return "You added that one already";
        g_added.push_back(repo);
        save_state();
    }
    jfg_store_refresh(); // its catalog loads with the others
    return "";
}

void jfg_store_remove_source(const std::string& source_id) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_state.busy || g_fetching) return;
    const auto it = std::find_if(g_added.begin(), g_added.end(), [&](const std::string& r) { return added_id(r) == source_id; });
    if (it == g_added.end()) return; // never a built-in one
    g_added.erase(it);
    g_catalogs.erase(source_id);
    g_sources.erase(std::remove_if(g_sources.begin(), g_sources.end(), [&](const JfgStoreSource& s) { return s.id == source_id; }),
                    g_sources.end());
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

namespace recompui { void update_mod_list(bool scan_mods); } // ui_mod_menu.cpp

void jfg_store_remove(const std::string& mod_id) {
    std::string text;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_state.busy) return;
        const auto m = std::find_if(g_mods.begin(), g_mods.end(), [&](const JfgStoreMod& x) { return x.id == mod_id; });
        if (m == g_mods.end() || !m->managed) return; // never a mod the player put there by hand
        const fs::path file = mod_file(mod_id); // asked before closing: a closed mod has no file name
        std::error_code ec;
        with_mods_closed([&](std::error_code& change_ec) { fs::remove(file, change_ec); }, ec);
        if (!ec) {
            g_installed.erase(mod_id);
            save_state();
        }
        text = ec ? "Could not remove it: " + ec.message() : m->name + " removed. Its settings are kept.";
    }
    recomp::mods::scan_mods();
    recompui::update_mod_list(false); // the Mods page drops it
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.text = text;
    build_list();
}

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
