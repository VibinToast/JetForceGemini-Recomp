// Jet Force Gemini port: the official HD texture pack (github.com/VibinToast/JetForceGemini-HD_TexturePack) in the
// launcher: which version is out, which is installed, and downloading / updating it (jfg_pack_install.h).
//
// The latest release is read from GitHub (one request, only with Check for Updates on). Its files are named
// <id>-<variant>_v<version>.rtz (Official_Recomp_HD-2K_v1.1.0.rtz), or .rtz.001, .rtz.002, ... for a pack split into
// parts (tools/textures/split_rtz.ps1); GitHub gives each one's size and SHA-256. Nothing else needs publishing.
//
// Installed packs are found in texture_packs by their credits.json (id and variant, version), or for the first release
// (1.0.0, which has no credits.json) by their exact size.
//
// Installing, on a thread of its own: each file is downloaded to <file>.download (a stopped download carries on from
// there next time), checked against its size and SHA-256, and renamed into place; parts are then joined
// (jfg_texture_packs.cpp). The choice of pack moves from the version it replaces (or from Off, for a first download) to
// the new one, and the old one is deleted - at the next start if it is still open.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "json/json.hpp"

#include "jfg_pack_install.h"
#include "jfg_port.h"
#include "jfg_update_check.h"

namespace fs = std::filesystem;

namespace {
constexpr const char* RELEASE_URL = "https://api.github.com/repos/VibinToast/JetForceGemini-HD_TexturePack/releases/latest";
constexpr const char* OFFICIAL_ID = "Official_Recomp_HD";
constexpr const char* DELETE_LATER_FILE = "jfg_pack_delete.txt"; // packs replaced while open, deleted at the next start

// The first release (1.0.0): 1K-Toasty-HD.rtz and 2K-Toasty-HD.rtz, without a credits.json.
struct LegacyPack { const char* variant; uint64_t size; };
constexpr LegacyPack LEGACY_PACKS[] = { { "1K", 263698739ull }, { "2K", 1043291144ull } };

struct Asset { std::string name, url, sha256; uint64_t size = 0; };
struct Offer {
    std::string variant, version;
    Asset whole;               // the .rtz, or
    std::vector<Asset> parts;  // its parts, in order
    uint64_t size() const {
        if (parts.empty()) return whole.size;
        uint64_t total = 0;
        for (const Asset& a : parts) total += a.size;
        return total;
    }
    std::string pack_name() const { return parts.empty() ? whole.name : parts[0].name.substr(0, parts[0].name.size() - 4); }
};

std::mutex g_mutex;
int g_stage = 0;
std::string g_latest;
std::vector<Offer> g_offers;
bool g_busy = false;
int g_percent = 0;
std::string g_text;
int g_serial = 0;
std::atomic<bool> g_cancel{ false };

// "1.2.3" -> {1, 2, 3}, compared with the shorter one padded with zeros.
int compare_versions(const std::string& a_text, const std::string& b_text) {
    auto parse = [](const std::string& t) {
        std::vector<int> v;
        int value = -1;
        for (size_t i = 0; i <= t.size(); i++) {
            const char c = i < t.size() ? t[i] : '.';
            if (std::isdigit((unsigned char)c)) value = (value < 0 ? 0 : value) * 10 + (c - '0');
            else if (c == '.') { if (value >= 0) v.push_back(value); value = -1; }
            else if (value >= 0 || !v.empty()) break;
        }
        return v;
    };
    std::vector<int> a = parse(a_text), b = parse(b_text);
    const size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a < b ? -1 : (a > b ? 1 : 0);
}

// <id>-<variant>_v<version>.rtz[.NNN] -> variant, version, part (0 = the whole pack). False for any other name.
bool parse_asset_name(const std::string& name, std::string& variant, std::string& version, int& part) {
    const std::string prefix = std::string(OFFICIAL_ID) + "-";
    if (name.rfind(prefix, 0) != 0) return false;
    std::string rest = name.substr(prefix.size());
    part = 0;
    if (rest.size() > 4 && rest[rest.size() - 4] == '.' && std::isdigit((unsigned char)rest[rest.size() - 1]) &&
        std::isdigit((unsigned char)rest[rest.size() - 2]) && std::isdigit((unsigned char)rest[rest.size() - 3])) {
        part = std::atoi(rest.c_str() + rest.size() - 3);
        rest.resize(rest.size() - 4);
        if (part <= 0) return false;
    }
    if (rest.size() < 4 || rest.compare(rest.size() - 4, 4, ".rtz") != 0) return false;
    rest.resize(rest.size() - 4);
    const size_t v = rest.rfind("_v");
    if (v == std::string::npos || v == 0) return false;
    variant = rest.substr(0, v);
    version = rest.substr(v + 2);
    return !variant.empty() && !version.empty() && std::isdigit((unsigned char)version[0]);
}

void check_thread() {
    std::string body;
    std::vector<Offer> offers;
    std::string latest;
    bool ok = jfg_http_get_text(RELEASE_URL, body);
    if (ok) {
        nlohmann::json release = nlohmann::json::parse(body, nullptr, false);
        ok = release.is_object();
        if (ok && release.contains("assets") && release["assets"].is_array()) {
            std::map<std::string, Offer> by_variant;
            for (const nlohmann::json& a : release["assets"]) {
                if (!a.is_object()) continue;
                const std::string name = a.value("name", "");
                std::string variant, version;
                int part = 0;
                if (!parse_asset_name(name, variant, version, part)) continue;
                Asset asset;
                asset.name = name;
                asset.url = a.value("browser_download_url", "");
                asset.size = a.value("size", (uint64_t)0);
                const std::string digest = a.value("digest", "");
                if (digest.rfind("sha256:", 0) == 0) asset.sha256 = digest.substr(7);
                if (asset.url.rfind("https://", 0) != 0 || asset.size == 0) continue;
                Offer& offer = by_variant[variant];
                if (!offer.version.empty() && offer.version != version) {
                    if (compare_versions(version, offer.version) < 0) continue; // an older file left in the release
                    offer = Offer{};
                }
                offer.variant = variant;
                offer.version = version;
                if (part == 0) offer.whole = asset;
                else {
                    if ((int)offer.parts.size() < part) offer.parts.resize(part);
                    offer.parts[part - 1] = asset;
                }
                if (latest.empty() || compare_versions(version, latest) > 0) latest = version;
            }
            for (auto& [variant, offer] : by_variant) {
                if (!offer.whole.url.empty()) offer.parts.clear(); // the whole pack is there too: use it
                bool complete = !offer.whole.url.empty() || !offer.parts.empty();
                for (const Asset& p : offer.parts) if (p.url.empty()) complete = false; // a part is missing
                if (complete) offers.push_back(offer);
            }
            std::sort(offers.begin(), offers.end(), [](const Offer& a, const Offer& b) { return a.size() < b.size(); });
        }
    }
    std::lock_guard lock(g_mutex);
    g_offers = std::move(offers);
    g_latest = latest;
    g_stage = ok ? 2 : 3;
    g_serial++;
}

// Which official pack a file in texture_packs is: its variant and version (credits.json, or for the first release its
// exact size). False for any other pack.
bool identify_official(const std::string& folder, std::string& variant, std::string& version) {
    if (folder.size() < 4 || folder.compare(folder.size() - 4, 4, ".rtz") != 0) return false;
    const jfg::PackInfo info = jfg::texture_pack_info(folder);
    if (info.has_json) {
        if (info.id != OFFICIAL_ID || info.variant.empty()) return false;
        variant = info.variant;
        version = info.version.empty() ? "0" : info.version;
        return true;
    }
    std::error_code ec;
    const uint64_t size = fs::file_size(fs::u8path(folder), ec);
    for (const LegacyPack& legacy : LEGACY_PACKS) {
        if (!ec && size == legacy.size) { variant = legacy.variant; version = "1.0.0"; return true; }
    }
    return false;
}

// The official packs in texture_packs, by variant: the newest version, and every file of that variant.
struct Installed { std::string version; std::vector<std::string> files; };
std::map<std::string, Installed> scan_installed() {
    std::map<std::string, Installed> out;
    for (const std::string& folder : jfg::list_texture_packs()) {
        std::string variant, version;
        if (!identify_official(folder, variant, version)) continue;
        Installed& entry = out[variant];
        entry.files.push_back(folder);
        if (entry.version.empty() || compare_versions(version, entry.version) > 0) entry.version = version;
    }
    return out;
}

void set_progress(int percent, const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_percent = percent;
    g_text = text;
}

void finish(const std::string& text) {
    std::lock_guard lock(g_mutex);
    g_busy = false;
    g_text = text;
    g_serial++;
}

std::string size_text(uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(text, sizeof(text), "%.0f MB", bytes / 1048576.0);
    return text;
}

// Deletes a replaced pack, or notes it for the next start when it is still open (the renderer may not have let go yet).
void delete_or_later(const std::string& folder) {
    std::error_code ec;
    for (int attempt = 0; attempt < 5; attempt++) {
        if (fs::remove(fs::u8path(folder), ec) || !fs::exists(fs::u8path(folder), ec)) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    if (std::FILE* f = std::fopen(DELETE_LATER_FILE, "a")) { std::fprintf(f, "%s\n", folder.c_str()); std::fclose(f); }
}

void install_thread(Offer offer) {
    const std::string label = offer.variant + " " + offer.version;
    std::error_code ec;
    fs::create_directories("texture_packs", ec);
    const std::vector<Asset> files = offer.parts.empty() ? std::vector<Asset>{ offer.whole } : offer.parts;
    const uint64_t total = offer.size();

    // Room for the download (and, for parts, the joined pack as well), less what an earlier attempt already has.
    uint64_t have = 0;
    for (const Asset& a : files) {
        const fs::path part = fs::u8path("texture_packs/" + a.name + ".download");
        if (fs::exists(part, ec)) have += fs::file_size(part, ec);
    }
    const uint64_t needed = (offer.parts.empty() ? total : total * 2) - std::min(have, total) + (64ull << 20);
    const fs::space_info space = fs::space("texture_packs", ec);
    if (!ec && space.available < needed) { finish("Not enough disk space for the " + offer.variant + " pack (" + size_text(needed) + " needed)."); return; }

    uint64_t done_before = 0;
    for (const Asset& a : files) {
        const fs::path target = fs::u8path("texture_packs/" + a.name);
        const fs::path temp = fs::u8path("texture_packs/" + a.name + ".download");
        const int result = jfg_http_download(a.url, temp, a.size, [&](uint64_t done, uint64_t) {
            const uint64_t now = done_before + done;
            set_progress(total > 0 ? (int)(now * 90 / total) : 0,
                         "Downloading " + label + ": " + size_text(now) + " of " + size_text(total));
            return !g_cancel.load();
        });
        if (result < 0) {
            for (const Asset& f : files) fs::remove(fs::u8path("texture_packs/" + f.name + ".download"), ec);
            finish("Download cancelled.");
            return;
        }
        if (result == 0) { finish("The download stopped (no connection?). Press it again to carry on where it stopped."); return; }
        if (fs::file_size(temp, ec) != a.size) {
            fs::remove(temp, ec);
            finish("The download of " + label + " came out the wrong size. Try again.");
            return;
        }
        if (!a.sha256.empty()) {
            set_progress((int)((done_before + a.size) * 90 / total), "Checking " + label + "...");
            std::string hex;
            if (!jfg_sha256_file(temp, hex) || hex != a.sha256) {
                fs::remove(temp, ec);
                finish("The download of " + label + " did not check out (damaged). Try again.");
                return;
            }
        }
        fs::remove(target, ec);
        fs::rename(temp, target, ec);
        if (ec) { finish("Could not put " + a.name + " in texture_packs."); return; }
        done_before += a.size;
    }

    const std::string pack = "texture_packs/" + offer.pack_name();
    if (!offer.parts.empty()) {
        // The parts are joined like parts put there by hand; this waits for it.
        int finished_before = 0;
        jfg::pack_join_text(finished_before);
        jfg::join_split_packs();
        while (jfg::pack_join_running()) {
            int finished = 0;
            const std::string text = jfg::pack_join_text(finished);
            set_progress(95, text.empty() ? "Joining " + label + "..." : text);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        if (!fs::exists(fs::u8path(pack), ec)) {
            int finished = 0;
            const std::string text = jfg::pack_join_text(finished);
            finish(text.empty() ? "The parts of " + label + " could not be joined." : text);
            return;
        }
    }

    // The choice of pack moves from the version this replaces (or from Off, for a first download) to the new one, and
    // the replaced versions go.
    std::vector<std::string> replaced;
    for (const auto& [variant, installed] : scan_installed()) {
        if (variant != offer.variant) continue;
        for (const std::string& file : installed.files) if (file != pack) replaced.push_back(file);
    }
    std::string current;
    { std::lock_guard lock(jfg::g_pack_mutex); current = jfg::g_pack_folder; }
    const bool was_replaced = std::find(replaced.begin(), replaced.end(), current) != replaced.end();
    if (was_replaced || (current.empty() && replaced.empty())) jfg::set_texture_pack(pack);
    for (const std::string& file : replaced) delete_or_later(file);
    finish("Installed the " + label + " pack." + (replaced.empty() ? "" : " The older version was removed."));
}
} // namespace

namespace jfg {
// At start-up (load_texture_pack_choice): deletes the packs an update replaced while they were open.
void delete_replaced_packs() {
    std::vector<std::string> left;
    if (std::FILE* f = std::fopen(DELETE_LATER_FILE, "r")) {
        char line[1024];
        while (std::fgets(line, sizeof(line), f)) {
            std::string folder = line;
            while (!folder.empty() && (folder.back() == '\n' || folder.back() == '\r')) folder.pop_back();
            if (folder.rfind("texture_packs/", 0) != 0 || folder.find("..") != std::string::npos) continue; // only packs
            std::error_code ec;
            if (!fs::remove(fs::u8path(folder), ec) && fs::exists(fs::u8path(folder), ec)) left.push_back(folder);
        }
        std::fclose(f);
    }
    else return;
    std::error_code ec;
    if (left.empty()) { fs::remove(DELETE_LATER_FILE, ec); return; }
    if (std::FILE* f = std::fopen(DELETE_LATER_FILE, "w")) {
        for (const std::string& folder : left) std::fprintf(f, "%s\n", folder.c_str());
        std::fclose(f);
    }
}
} // namespace jfg

void jfg_official_check_start() {
    if (!jfg_launcher_check_updates() || !jfg_launcher_show_official_pack()) return;
    {
        std::lock_guard lock(g_mutex);
        if (g_stage != 0) return;
        g_stage = 1;
    }
    std::thread(check_thread).detach();
}

JfgOfficialState jfg_official_state(bool scan) {
    JfgOfficialState state;
    std::vector<Offer> offers;
    {
        std::lock_guard lock(g_mutex);
        state.stage = g_stage;
        state.latest = g_latest;
        state.busy = g_busy;
        state.percent = g_percent;
        state.text = g_text;
        state.serial = g_serial;
        offers = g_offers;
    }
    if (!scan) return state;
    const std::map<std::string, Installed> installed = scan_installed();
    for (const Offer& offer : offers) {
        JfgOfficialPack pack;
        pack.variant = offer.variant;
        pack.size = offer.size();
        auto it = installed.find(offer.variant);
        if (it != installed.end()) pack.installed = it->second.version;
        pack.action = pack.installed.empty() ? "Download" : (compare_versions(pack.installed, offer.version) < 0 ? "Update" : "Installed");
        state.packs.push_back(pack);
    }
    return state;
}

void jfg_official_install(const std::string& variant) {
    Offer offer;
    {
        std::lock_guard lock(g_mutex);
        if (g_busy || g_stage != 2) return;
        auto it = std::find_if(g_offers.begin(), g_offers.end(), [&](const Offer& o) { return o.variant == variant; });
        if (it == g_offers.end()) return;
        offer = *it;
        g_busy = true;
        g_percent = 0;
        g_text = "Starting the download...";
    }
    g_cancel = false;
    std::thread(install_thread, offer).detach();
}

void jfg_official_cancel() { g_cancel = true; }

bool jfg_official_outdated(const std::string& folder) {
    std::string variant, version;
    if (!identify_official(folder, variant, version)) return false;
    std::lock_guard lock(g_mutex);
    if (g_stage != 2) return false;
    for (const Offer& offer : g_offers) {
        if (offer.variant == variant) return compare_versions(version, offer.version) < 0;
    }
    return false;
}

std::vector<JfgOfficialUpdate> jfg_official_updates() {
    std::vector<Offer> offers;
    {
        std::lock_guard lock(g_mutex);
        if (g_stage != 2) return {};
        offers = g_offers;
    }
    std::vector<JfgOfficialUpdate> out;
    const std::map<std::string, Installed> installed = scan_installed();
    for (const Offer& offer : offers) {
        auto it = installed.find(offer.variant);
        if (it != installed.end() && compare_versions(it->second.version, offer.version) < 0) {
            out.push_back({ offer.variant, it->second.version, offer.version });
        }
    }
    return out;
}
