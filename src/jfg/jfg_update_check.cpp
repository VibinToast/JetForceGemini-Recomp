// JFG PORT: checks GitHub for a newer release of this project when the launcher opens (src/ui/ui_launcher.cpp shows a
// warning triangle next to the version number and links to the release page). One request, on a background thread,
// with a short timeout: when the check fails (no internet, GitHub busy, rate limit) nothing is shown and nothing else
// is affected. Only the latest release's tag name is read; nothing is sent about the user or the computer.
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#endif

namespace {
constexpr const char* RELEASES_API_HOST = "api.github.com";
constexpr const char* RELEASES_API_PATH = "/repos/VibinToast/JetForceGemini-Recomp/releases/latest";

std::atomic<int> g_state{ 0 }; // 0 = running or nothing found, 1 = a newer version was found (g_version set)
std::mutex g_mutex;
std::string g_version;

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

bool is_newer(const std::string& latest, const std::string& current) {
    std::vector<int> a = parse_version(latest), b = parse_version(current);
    if (a.empty() || b.empty()) return false;
    const size_t n = std::max(a.size(), b.size());
    a.resize(n, 0);
    b.resize(n, 0);
    return a > b;
}

// The value of "tag_name" in the release's JSON, or "".
std::string tag_name_from_json(const std::string& json) {
    const size_t key = json.find("\"tag_name\"");
    if (key == std::string::npos) return "";
    const size_t colon = json.find(':', key);
    const size_t open = colon == std::string::npos ? colon : json.find('"', colon);
    if (open == std::string::npos) return "";
    const size_t close = json.find('"', open + 1);
    return close == std::string::npos ? "" : json.substr(open + 1, close - open - 1);
}

std::string fetch_latest_release_json() {
    std::string body;
#ifdef _WIN32
    HINTERNET session = WinHttpOpen(L"JetForceGemini-Recomp", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (session == nullptr) return body;
    WinHttpSetTimeouts(session, 4000, 4000, 4000, 6000);
    HINTERNET connection = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = nullptr;
    if (connection != nullptr) {
        request = WinHttpOpenRequest(connection, L"GET", L"/repos/VibinToast/JetForceGemini-Recomp/releases/latest", nullptr,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    }
    if (request != nullptr &&
        WinHttpSendRequest(request, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD status = 0, size = sizeof(status);
        WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
        if (status == 200) {
            char buffer[4096];
            DWORD got = 0;
            while (body.size() < (1u << 20) && WinHttpReadData(request, buffer, sizeof(buffer), &got) && got > 0) body.append(buffer, got);
        }
    }
    if (request != nullptr) WinHttpCloseHandle(request);
    if (connection != nullptr) WinHttpCloseHandle(connection);
    WinHttpCloseHandle(session);
#else
    // Linux / others: curl, when it is installed.
    const std::string command = std::string("curl -fsS -m 6 -H 'Accept: application/vnd.github+json' https://") + RELEASES_API_HOST + RELEASES_API_PATH + " 2>/dev/null";
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[4096];
        size_t got;
        while (body.size() < (1u << 20) && (got = std::fread(buffer, 1, sizeof(buffer), pipe)) > 0) body.append(buffer, got);
        pclose(pipe);
    }
#endif
    return body;
}

void check_thread(std::string current) {
    const std::string tag = tag_name_from_json(fetch_latest_release_json());
    if (tag.empty() || !is_newer(tag, current)) return;
    std::string shown = tag;
    if (!shown.empty() && (shown[0] == 'v' || shown[0] == 'V')) shown.erase(0, 1);
    std::lock_guard<std::mutex> lock(g_mutex);
    g_version = shown;
    g_state = 1;
}
} // namespace

// Starts the check (once). `current` is this build's version, like "1.0.0".
void jfg_update_check_start(const std::string& current) {
    static bool started = false;
    if (started) return;
    started = true;
    try {
        std::thread(check_thread, current).detach();
    }
    catch (...) {}
}

// True once a newer release has been found; `version` is then its number, like "1.2.0".
bool jfg_update_available(std::string& version) {
    if (g_state.load() != 1) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    version = g_version;
    return true;
}
