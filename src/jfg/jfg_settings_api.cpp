// Jet Force Gemini port: settings API for the "Select" config menu and the launcher (jfg_settings_api.h). Same data
// and files as the F9 tabs.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

#include "jfg_settings_api.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_update_check.h"
#include "ultramodern/ultramodern.hpp"

namespace jfg::settings {
int get(const std::string& key) {
    load_options(); // the menu document can be built before the game's first frame
    if (key == "texture_dump") return g.texture_dump ? 1 : 0;
    const std::string text = options_text(g);
    const std::string needle = key + "=";
    size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
        if (pos == 0 || text[pos - 1] == '\n') return std::atoi(text.c_str() + pos + needle.size());
        pos += needle.size();
    }
    return 0;
}

void set(const std::string& key, int value) {
    { static const bool lg = std::getenv("JFG_DEBUG_SETTINGS") != nullptr; if (lg) std::fprintf(stderr, "[jfg] settings::set %s = %d\n", key.c_str(), value); }
    load_options();
    // RmlUi writes a bound value back whenever its element is refreshed: only real changes are applied and saved.
    if (get(key) == value) return;
    if (key == "texture_dump") { g.texture_dump = value != 0; return; }
    apply_option(key.c_str(), value);
    push_render_options();
    save_options();
}

bool frame_rate_needs_restart() {
    return jfg::frame_rate_needs_restart();
}

void restart_game() {
    save_options();
    jfg_update_restart(); // with no update waiting it only starts this exe again
    ultramodern::quit();
}

std::vector<std::string> profiles() {
    load_profile_choice();
    list_user_profiles();
    std::vector<std::string> out;
    for (const BuiltinProfile& p : BUILTIN_PROFILES) out.emplace_back(p.name);
    out.insert(out.end(), g_user_profiles.begin(), g_user_profiles.end());
    return out;
}

std::string active_profile() { load_profile_choice(); return g_active_profile; }

std::string profile_description() {
    load_profile_choice();
    if (const BuiltinProfile* b = builtin_profile(g_active_profile)) return b->description;
    if (g_active_profile.empty()) return "No profile chosen: your own settings.";
    return "Your profile (profiles\\" + g_active_profile + ".txt). Changes you make are saved to it.";
}

bool active_profile_is_builtin() { load_profile_choice(); return builtin_profile(g_active_profile) != nullptr; }

bool profile_changed() {
    load_profile_choice();
    return builtin_profile(g_active_profile) != nullptr && settings_snapshot() != g_profile_snapshot;
}

void choose_profile(const std::string& name) {
    { static const bool lg = std::getenv("JFG_DEBUG_SETTINGS") != nullptr; if (lg) std::fprintf(stderr, "[jfg] settings::choose_profile %s\n", name.c_str()); }
    load_profile_choice();
    jfg::choose_profile(name);
}

std::string profile_preview(const std::string& name) { load_options(); return profile_preview_text(name); }
bool keep_controls() { load_profile_choice(); return g_profile_keep_controls; }
void set_keep_controls(bool keep) { set_profile_keep_controls(keep); }

bool controller_bindings_unused() { load_options(); return (g.dual_stick || g.mouse_keyboard) && g.dual_layout; }
bool keyboard_bindings_unused() { load_options(); return g.mouse_keyboard; }

bool save_profile(const std::string& name) {
    load_profile_choice();
    if (!valid_profile_name(name) || builtin_profile(name) != nullptr) {
        g_profile_message = "Use letters, digits, spaces, - and _ (not the name of a built-in profile).";
        return false;
    }
    write_user_profile(name);
    g_active_profile = name;
    save_profile_choice();
    g_profile_snapshot = settings_snapshot();
    list_user_profiles();
    g_profile_message = "Saved profile \"" + name + "\".";
    return true;
}

void delete_active_profile() {
    load_profile_choice();
    if (g_active_profile.empty() || builtin_profile(g_active_profile) != nullptr) return;
    std::error_code ec;
    std::filesystem::remove(user_profile_path(g_active_profile), ec);
    g_profile_message = "Profile \"" + g_active_profile + "\" deleted.";
    g_active_profile.clear();
    save_profile_choice();
    list_user_profiles();
}

std::string profile_message() { return g_profile_message; }

std::vector<std::string> texture_packs() {
    std::vector<std::string> out{ "" };
    const std::vector<std::string> packs = list_texture_packs();
    out.insert(out.end(), packs.begin(), packs.end());
    return out;
}
std::string texture_pack_name(const std::string& folder) { return pack_display_name(folder); }
std::string texture_pack() { std::lock_guard lock(g_pack_mutex); return g_pack_folder; }
void set_texture_pack(const std::string& folder) { jfg::set_texture_pack(folder); }
int texture_pack_count() { return g_texture_pack_count.load(); }
int dumped_texture_files() {
    std::error_code ec; int n = 0;
    for (std::filesystem::recursive_directory_iterator it("texture_dump", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) n++;
    }
    return ec ? -1 : n;
}
// Opens the texture_packs folder (next to the game; made if it is not there yet) in the file manager.
void open_texture_packs_folder() {
    std::error_code ec;
    std::filesystem::create_directories("texture_packs", ec);
    const std::filesystem::path folder = std::filesystem::absolute("texture_packs", ec);
#if defined(_WIN32)
    ShellExecuteW(NULL, L"open", folder.wstring().c_str(), NULL, NULL, SW_SHOWDEFAULT);
#elif defined(__APPLE__)
    std::system(("open \"" + folder.string() + "\"").c_str());
#else
    std::system(("xdg-open \"" + folder.string() + "\" &").c_str());
#endif
}
} // namespace jfg::settings
