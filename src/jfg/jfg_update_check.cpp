// JFG PORT: checks GitHub for a newer release of this project when the launcher opens, and installs it on request
// (src/ui/ui_launcher.cpp shows a warning triangle next to the version number; clicking it shows what changed and asks).
//
// The check is one request for the list of releases, on a background thread with a short timeout: when it fails (no
// internet, GitHub busy, rate limit) nothing is shown and nothing else is affected. Nothing is sent about the user.
//
// Installing (Windows only), on a background thread:
//   1. download JFG-<version>-windows-x64-portable.zip from the release, over https;
//   2. check the zip against its SHA-256 (GitHub's digest of the asset, else an attached .sha256; with neither it is not installed);
//   3. unpack the files the game needs (the exe, the DLLs, assets/) into <game folder>\update\new;
//   4. rename each file that is in use (the running exe, loaded DLLs) to <name>.old - Windows allows renaming a running
//      file, though not overwriting or deleting it - and move the new file into its place. Anything that fails undoes
//      the renames and leaves the old version as it was;
//   5. jfg_update_restart() starts the new exe and the launcher quits. The new exe removes the *.old files.
// Saves, settings, texture packs and mods are not in the zip and are never touched.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jfg/jfg_update_check.h"
#include "json/json.hpp"
#include <miniz/miniz.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

namespace fs = std::filesystem;

namespace {
constexpr const char* RELEASES_URL = "https://api.github.com/repos/VibinToast/JetForceGemini-Recomp/releases?per_page=30";
constexpr const char* ZIP_SUFFIX = "-windows-x64-portable.zip";

struct Release {
    std::string tag, version, body, zip_name, zip_url, sha_url, zip_digest; // zip_digest: GitHub's own SHA-256 of the zip, lower case hex
};

std::mutex g_mutex;
bool g_found = false;
JfgUpdateInfo g_info;
Release g_target;
std::atomic<int> g_stage{ 0 }, g_percent{ 0 };
std::string g_text;
std::atomic<bool> g_installing{ false };
std::atomic<bool> g_deferred{ false }; // the files are still in <game>\update\new: a script copies them in after this exe exits

void set_progress(int stage, int percent, const std::string& text) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_stage = stage;
    g_percent = percent;
    g_text = text;
}

// ---- versions -------------------------------------------------------------------------------------------------------
// "v1.2.3" / "1.2" -> {1, 2, 3}; anything that is not a number ends the version.
std::vector<int> parse_version(const std::string& text) {
    std::vector<int> parts;
    size_t i = 0;
    while (i < text.size() && !std::isdigit((unsigned char)text[i])) i++;
    int value = -1;
    for (; i <= text.size(); i++) {
        const char c = i < text.size() ? text[i] : '\0';
        if (std::isdigit((unsigned char)c)) { value = (value < 0 ? 0 : value) * 10 + (c - '0'); }
        else {
            if (value >= 0) parts.push_back(value);
            value = -1;
            if (c != '.') break;
        }
    }
    return parts;
}

int compare_versions(const std::string& a_text, const std::string& b_text) {
    std::vector<int> a = parse_version(a_text), b = parse_version(b_text);
    const size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a < b ? -1 : (a > b ? 1 : 0);
}

bool is_newer(const std::string& latest, const std::string& current) {
    if (parse_version(latest).empty() || parse_version(current).empty()) return false;
    return compare_versions(latest, current) > 0;
}

std::string strip_v(std::string tag) {
    if (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) tag.erase(0, 1);
    return tag;
}

std::string json_string(const nlohmann::json& j, const char* key) {
    const auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

// ---- network (https only) ----------------------------------------------------------------------------------------------
using Sink = std::function<bool(const char*, size_t)>;                 // false = stop
using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

bool http_get(const std::string& url, const Sink& sink, const ProgressFn& progress) {
    wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
    URL_COMPONENTS parts = {};
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host;      parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;       parts.dwUrlPathLength = 2048;
    parts.lpszExtraInfo = extra;    parts.dwExtraInfoLength = 2048;
    const std::wstring wurl = widen(url);
    if (!WinHttpCrackUrl(wurl.c_str(), (DWORD)wurl.size(), 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS) return false;
    const std::wstring target = std::wstring(path) + extra;

    bool ok = false;
    HINTERNET session = WinHttpOpen(L"JetForceGemini-Recomp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return false;
    WinHttpSetTimeouts(session, 8000, 8000, 20000, 30000);
    HINTERNET connection = WinHttpConnect(session, host, parts.nPort, 0);
    HINTERNET request = connection == nullptr ? nullptr : WinHttpOpenRequest(connection, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (request != nullptr &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json, application/octet-stream\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        DWORD length = 0;
        size = sizeof(length);
        if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &length, &size, WINHTTP_NO_HEADER_INDEX)) length = 0;
        if (status == 200) {
            ok = true;
            std::vector<char> buffer(64 * 1024);
            uint64_t done = 0;
            DWORD got = 0;
            bool finished = false;
            while (WinHttpReadData(request, buffer.data(), (DWORD)buffer.size(), &got)) {
                if (got == 0) { finished = true; break; }
                if (!sink(buffer.data(), got)) { ok = false; break; }
                done += got;
                if (progress) progress(done, length);
            }
            if (!finished) ok = false; // the read failed part way
        }
    }
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
    return ok;
}
#else
// Linux / others: curl, when it is installed. Only the release list is read here; installing is Windows only.
bool http_get(const std::string& url, const Sink& sink, const ProgressFn&) {
    if (url.rfind("https://", 0) != 0 || url.find('\'') != std::string::npos) return false;
    const std::string command = "curl -fsS -m 8 -H 'Accept: application/vnd.github+json' '" + url + "' 2>/dev/null";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return false;
    char buffer[4096];
    size_t got;
    bool ok = true;
    while ((got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        if (!sink(buffer, got)) { ok = false; break; }
    }
    return pclose(pipe) == 0 && ok;
}
#endif

bool http_get_string(const std::string& url, std::string& out, size_t limit = 4u << 20) {
    out.clear();
    return http_get(url, [&](const char* data, size_t n) { if (out.size() + n > limit) return false; out.append(data, n); return true; }, nullptr);
}

// ---- finding releases ---------------------------------------------------------------------------------------------------
void check_thread(std::string current) {
    std::string body;
    if (!http_get_string(RELEASES_URL, body)) return;
    const nlohmann::json list = nlohmann::json::parse(body, nullptr, false);
    if (list.is_discarded() || !list.is_array()) return;

    std::vector<Release> releases;
    for (const nlohmann::json& r : list) {
        if (!r.is_object()) continue;
        const auto flag = [&](const char* key) { const auto it = r.find(key); return it != r.end() && it->is_boolean() && it->get<bool>(); };
        if (flag("draft") || flag("prerelease")) continue;
        Release release;
        release.tag = json_string(r, "tag_name");
        release.version = strip_v(release.tag);
        if (parse_version(release.version).empty()) continue;
        release.body = json_string(r, "body");
        const auto assets = r.find("assets");
        if (assets != r.end() && assets->is_array()) {
            for (const nlohmann::json& a : *assets) {
                const std::string name = json_string(a, "name"), url = json_string(a, "browser_download_url");
                if (name.size() > std::strlen(ZIP_SUFFIX) && name.compare(name.size() - std::strlen(ZIP_SUFFIX), std::string::npos, ZIP_SUFFIX) == 0) {
                    release.zip_name = name;
                    release.zip_url = url;
                    // GitHub computes a "sha256:<hex>" digest for every uploaded asset and lists it with the release.
                    const std::string digest = json_string(a, "digest");
                    if (digest.rfind("sha256:", 0) == 0 && digest.size() == 7 + 64) {
                        release.zip_digest = digest.substr(7);
                        for (char& c : release.zip_digest) c = (char)std::tolower((unsigned char)c);
                    }
                }
            }
            for (const nlohmann::json& a : *assets) {
                if (!release.zip_name.empty() && json_string(a, "name") == release.zip_name + ".sha256") release.sha_url = json_string(a, "browser_download_url");
            }
        }
        releases.push_back(std::move(release));
    }
    std::stable_sort(releases.begin(), releases.end(), [](const Release& a, const Release& b) { return compare_versions(a.version, b.version) > 0; });
    if (releases.empty()) return;

    if (!is_newer(releases[0].version, current)) return;

    std::string changelog;
    for (const Release& release : releases) {
        if (!is_newer(release.version, current)) continue;
        std::string notes = release.body;
        notes.erase(std::remove(notes.begin(), notes.end(), '\r'), notes.end());
        while (!notes.empty() && std::isspace((unsigned char)notes.back())) notes.pop_back();
        if (notes.empty()) notes = "(no notes)";
        changelog += "v" + release.version + "\n" + notes + "\n\n";
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    g_target = releases[0];
    g_info.version = releases[0].version;
    g_info.changelog = changelog;
#ifdef _WIN32
    g_info.can_install = (!g_target.zip_url.empty() && (!g_target.zip_digest.empty() || !g_target.sha_url.empty())) || std::getenv("JFG_UPDATE_ZIP") != nullptr;
    if (!g_info.can_install) {
        g_info.unavailable = g_target.zip_url.empty()
            ? "This release has no portable zip attached yet, so it cannot be installed from here. Use the release page."
            : "GitHub has no checksum for this release's zip, so it cannot be verified. Use the release page.";
    }
#else
    g_info.can_install = false;
    g_info.unavailable = "Updating from the launcher is only available on Windows. Use the release page.";
#endif
    g_found = true;
}

#ifdef _WIN32
// ---- installing -----------------------------------------------------------------------------------------------------------
fs::path exe_folder() {
    wchar_t buffer[MAX_PATH * 4] = {};
    GetModuleFileNameW(nullptr, buffer, (DWORD)std::size(buffer));
    return fs::path(buffer).parent_path();
}

bool sha256_of_file(const fs::path& file, std::string& hex) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = false;
    std::FILE* f = _wfopen(file.c_str(), L"rb");
    if (f != nullptr && BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
        std::vector<unsigned char> buffer(1 << 16);
        size_t got;
        ok = true;
        while ((got = std::fread(buffer.data(), 1, buffer.size(), f)) > 0) {
            if (BCryptHashData(hash, buffer.data(), (ULONG)got, 0) != 0) { ok = false; break; }
        }
        unsigned char digest[32] = {};
        if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0) {
            static const char digits[] = "0123456789abcdef";
            hex.clear();
            for (unsigned char byte : digest) { hex += digits[byte >> 4]; hex += digits[byte & 15]; }
        }
        else ok = false;
    }
    if (hash != nullptr) BCryptDestroyHash(hash);
    if (alg != nullptr) BCryptCloseAlgorithmProvider(alg, 0);
    if (f != nullptr) std::fclose(f);
    return ok;
}

bool download_to_file(const std::string& url, const fs::path& file, int from_percent, int to_percent, const std::string& label) {
    std::FILE* f = _wfopen(file.c_str(), L"wb");
    if (f == nullptr) return false;
    bool write_ok = true;
    const bool ok = http_get(url,
        [&](const char* data, size_t n) { if (std::fwrite(data, 1, n, f) != n) write_ok = false; return write_ok; },
        [&](uint64_t done, uint64_t total) {
            const int percent = total > 0 ? from_percent + (int)((to_percent - from_percent) * std::min<uint64_t>(done, total) / total) : from_percent;
            char text[160];
            if (total > 0) std::snprintf(text, sizeof(text), "%s %.1f of %.1f MB", label.c_str(), done / 1048576.0, total / 1048576.0);
            else std::snprintf(text, sizeof(text), "%s %.1f MB", label.c_str(), done / 1048576.0);
            set_progress(1, percent, text);
        });
    std::fclose(f);
    return ok && write_ok;
}

// A name inside the zip that is safe to unpack and that the game needs: the exe, DLLs next to it, the controller
// database and assets/. Anything else (README, ...) is left alone. `rel` is relative to the zip's top folder.
bool wanted_file(const std::string& rel) {
    if (rel.empty() || rel[0] == '/' || rel.find("..") != std::string::npos || rel.find(':') != std::string::npos || rel.find('\\') != std::string::npos) return false;
    if (rel.find('/') == std::string::npos) {
        std::string lower = rel;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        return lower == "jetforcegeminirecompiled.exe" || lower == "recompcontrollerdb.txt" || (lower.size() > 4 && lower.compare(lower.size() - 4, 4, ".dll") == 0);
    }
    return rel.rfind("assets/", 0) == 0;
}

size_t write_to_file(void* opaque, mz_uint64, const void* data, size_t n) {
    return std::fwrite(data, 1, n, static_cast<std::FILE*>(opaque));
}

bool unpack_zip(const fs::path& zip_file, const fs::path& out_dir, std::vector<std::string>& unpacked, std::string& error) {
    std::FILE* f = _wfopen(zip_file.c_str(), L"rb");
    if (f == nullptr) { error = "Could not open the download."; return false; }
    mz_zip_archive zip = {};
    if (!mz_zip_reader_init_cfile(&zip, f, 0, 0)) { std::fclose(f); error = "The download is not a valid zip file."; return false; }
    bool ok = true;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count && ok; i++) {
        mz_zip_archive_file_stat stat = {};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) { error = "The zip file is damaged."; ok = false; break; }
        if (mz_zip_reader_is_file_a_directory(&zip, i)) continue;
        std::string name = stat.m_filename;
        const size_t slash = name.find('/');
        const std::string rel = slash == std::string::npos ? name : name.substr(slash + 1); // drop the top folder
        if (!wanted_file(rel)) continue;
        const fs::path dest = out_dir / fs::path(std::u8string(rel.begin(), rel.end()));
        std::error_code ec;
        fs::create_directories(dest.parent_path(), ec);
        std::FILE* out = _wfopen(dest.c_str(), L"wb");
        if (out == nullptr) { error = "Could not write " + rel + "."; ok = false; break; }
        const bool extracted = mz_zip_reader_extract_to_callback(&zip, i, write_to_file, out, 0) != 0;
        std::fclose(out);
        if (!extracted) { error = "Could not unpack " + rel + "."; ok = false; break; }
        unpacked.push_back(rel);
        set_progress(1, 75 + (int)(15ull * (i + 1) / count), "Unpacking...");
    }
    mz_zip_reader_end(&zip);
    std::fclose(f);
    if (ok && std::find_if(unpacked.begin(), unpacked.end(), [](const std::string& s) {
            std::string l = s; for (char& c : l) c = (char)std::tolower((unsigned char)c); return l == "jetforcegeminirecompiled.exe"; }) == unpacked.end()) {
        error = "The download does not contain the game.";
        ok = false;
    }
    return ok;
}

// Renames with plain MoveFileExW. std::filesystem::rename can use POSIX-semantics renames, which Windows refuses
// ("being used by another process") for a running exe or a loaded DLL; the classic rename is allowed for those.
void move_file(const fs::path& from, const fs::path& to, std::error_code& ec) {
    ec.clear();
    if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED))
        ec = std::error_code((int)GetLastError(), std::system_category());
}

// Moves the unpacked files into the game folder, renaming whatever is there to <name>.old first. Undoes it all on failure.
bool install_files(const fs::path& new_dir, const fs::path& game_dir, const std::vector<std::string>& files, std::string& error, bool& locked) {
    locked = false;
    struct Move { fs::path target, backup; bool had_old; bool placed; };
    std::vector<Move> done;
    auto rollback = [&]() {
        for (auto it = done.rbegin(); it != done.rend(); ++it) {
            std::error_code ec;
            if (it->placed) fs::remove(it->target, ec);
            if (it->had_old) move_file(it->backup, it->target, ec);
        }
    };
    for (size_t i = 0; i < files.size(); i++) {
        const fs::path rel = fs::path(std::u8string(files[i].begin(), files[i].end()));
        const fs::path target = game_dir / rel, source = new_dir / rel;
        Move move{ target, fs::path(target.native() + L".old"), false, false };
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (fs::exists(target, ec)) {
            fs::remove(move.backup, ec); // a leftover from an earlier update
            ec.clear();
            move_file(target, move.backup, ec);
            if (ec) { error = "Could not replace " + files[i] + " (" + ec.message() + ")."; locked = ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_ACCESS_DENIED || ec.value() == ERROR_LOCK_VIOLATION; rollback(); return false; }
            move.had_old = true;
        }
        done.push_back(move);
        move_file(source, target, ec);
        if (ec) { error = "Could not install " + files[i] + " (" + ec.message() + ")."; locked = ec.value() == ERROR_SHARING_VIOLATION || ec.value() == ERROR_ACCESS_DENIED || ec.value() == ERROR_LOCK_VIOLATION; rollback(); return false; }
        done.back().placed = true;
        set_progress(1, 90 + (int)(10ull * (i + 1) / files.size()), "Installing...");
    }
    return true;
}

void install_thread() {
    const Release release = [] { std::lock_guard<std::mutex> lock(g_mutex); return g_target; }();
    const fs::path game_dir = exe_folder();
    const fs::path work = game_dir / "update";
    const fs::path zip_file = work / "download.zip", new_dir = work / "new";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(new_dir, ec);

    const char* local_zip = std::getenv("JFG_UPDATE_ZIP"); // tests: install this zip instead of downloading (no checksum)
    std::string error;
    if (local_zip != nullptr) {
        set_progress(1, 70, "Using the zip file from JFG_UPDATE_ZIP...");
        fs::copy_file(fs::path(local_zip), zip_file, fs::copy_options::overwrite_existing, ec);
        if (ec) error = "Could not read JFG_UPDATE_ZIP.";
    }
    else {
        set_progress(1, 0, "Downloading...");
        std::string checksum_text;
        if (!download_to_file(release.zip_url, zip_file, 0, 70, "Downloading")) error = "The download failed. Check your internet connection and try again.";
        else if (release.zip_digest.empty() && (!http_get_string(release.sha_url, checksum_text, 4096) || checksum_text.size() < 64)) error = "The release's checksum could not be downloaded.";
        else {
            set_progress(1, 72, "Checking the download...");
            std::string actual;
            std::string wanted = release.zip_digest.empty() ? checksum_text.substr(0, 64) : release.zip_digest; // GitHub's digest, else the .sha256 asset
            for (char& c : wanted) c = (char)std::tolower((unsigned char)c);
            if (!sha256_of_file(zip_file, actual)) error = "Could not check the download.";
            else if (actual != wanted) error = "The download does not match its checksum, so it was not installed.";
        }
    }
    std::vector<std::string> files;
    if (error.empty()) { set_progress(1, 75, "Unpacking..."); unpack_zip(zip_file, new_dir, files, error); }
    bool locked = false;
    if (error.empty()) { set_progress(1, 90, "Installing..."); install_files(new_dir, game_dir, files, error, locked); }
    if (!error.empty() && locked) {
        // Windows would not let this exe (or something loaded by it) be renamed - an antivirus scan, OneDrive, a locked DLL.
        // Everything was put back as it was; instead the unpacked files stay in update\new and a script copies them over
        // once this process has exited (jfg_update_restart).
        error.clear();
        g_deferred = true;
    }
    else fs::remove_all(work, ec);
    if (!error.empty()) set_progress(2, 0, error);
    else set_progress(3, 100, "Updated. Restarting...");
    g_installing = false;
}

// The new exe removes what the update left behind (<name>.old) once the old one has exited and released the files.
void cleanup_thread() {
    const fs::path game_dir = exe_folder();
    for (int attempt = 0; attempt < 30; attempt++) {
        bool any_left = false;
        // Only the game folder itself and assets/: the update puts nothing anywhere else.
        auto sweep = [&](auto iterator) {
            std::error_code ec;
            for (; !ec && iterator != decltype(iterator)(); iterator.increment(ec)) {
                if (!iterator->is_regular_file(ec) || iterator->path().extension() != L".old") continue;
                std::error_code remove_ec;
                if (!fs::remove(iterator->path(), remove_ec) || remove_ec) any_left = true;
            }
        };
        std::error_code ec;
        sweep(fs::directory_iterator(game_dir, fs::directory_options::skip_permission_denied, ec));
        sweep(fs::recursive_directory_iterator(game_dir / "assets", fs::directory_options::skip_permission_denied, ec));
        std::error_code remove_ec;
        fs::remove_all(game_dir / "update", remove_ec);
        if (!any_left) return;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}
#endif
} // namespace

void jfg_update_check_start(const std::string& current) {
    static bool started = false;
    if (started) return;
    started = true;
    try {
        std::thread(check_thread, current).detach();
#ifdef _WIN32
        std::thread(cleanup_thread).detach();
#endif
    }
    catch (...) {}
}

bool jfg_update_available(JfgUpdateInfo& info) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_found) return false;
    info = g_info;
    return true;
}

void jfg_update_install() {
#ifdef _WIN32
    if (g_installing.exchange(true)) return;
    set_progress(1, 0, "Starting...");
    try {
        std::thread(install_thread).detach();
    }
    catch (...) {
        g_installing = false;
        set_progress(2, 0, "Could not start the update.");
    }
#endif
}

JfgUpdateProgress jfg_update_progress() {
    JfgUpdateProgress progress;
    std::lock_guard<std::mutex> lock(g_mutex);
    progress.stage = g_stage;
    progress.percent = g_percent;
    progress.text = g_text;
    return progress;
}

void jfg_update_fail(const std::string& message) {
    set_progress(2, 0, message);
}

void jfg_update_reset() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_stage == 2) { g_stage = 0; g_percent = 0; g_text.clear(); }
}

void jfg_update_restart() {
#ifdef _WIN32
    const fs::path exe = exe_folder() / L"JetForceGeminiRecompiled.exe";
    if (g_deferred) {
        // Wait for this process to exit, copy the new files over the old (robocopy retries files that are still locked),
        // then start the game. The new exe removes the update folder.
        const fs::path game_dir = exe_folder(), new_dir = game_dir / "update" / "new", script = game_dir / "update" / "apply.cmd";
        auto utf8 = [](const fs::path& path) { const std::u8string u = path.u8string(); return std::string(u.begin(), u.end()); };
        const std::string pid = std::to_string(GetCurrentProcessId());
        std::string text = "@echo off\r\nchcp 65001 >nul\r\n:wait\r\ntasklist /FI \"PID eq " + pid + "\" /NH 2>nul | find \"" + pid + "\" >nul\r\n"
            "if not errorlevel 1 (\r\n  ping -n 2 127.0.0.1 >nul\r\n  goto wait\r\n)\r\n"
            "robocopy \"" + utf8(new_dir) + "\" \"" + utf8(game_dir) + "\" /E /R:60 /W:1 /NFL /NDL /NJH /NJS /NP >nul\r\n"
            "start \"\" /D \"" + utf8(game_dir) + "\" \"" + utf8(exe) + "\"\r\n";
        if (std::FILE* f = _wfopen(script.c_str(), L"wb")) {
            std::fwrite(text.data(), 1, text.size(), f);
            std::fclose(f);
            std::wstring command = L"cmd.exe /d /c \"\"" + script.wstring() + L"\"\"";
            STARTUPINFOW startup = {};
            startup.cb = sizeof(startup);
            PROCESS_INFORMATION process = {};
            if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, game_dir.c_str(), &startup, &process)) {
                CloseHandle(process.hProcess);
                CloseHandle(process.hThread);
            }
        }
        return;
    }
    std::wstring command_line = GetCommandLineW(); // the same arguments as this run
    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    const std::wstring folder = exe_folder().wstring();
    if (CreateProcessW(exe.c_str(), command_line.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) {
        CloseHandle(process.hProcess);
        CloseHandle(process.hThread);
    }
#endif
}
