// Jet Force Gemini port: profiles - whole sets of settings (Options and Weapons tabs) to switch between.
//
// The current settings stay in jfg_port_options.txt and jfg_weapons.txt; choosing a profile resets every setting to
// the game's own behaviour and then applies the profile's values. Built-in profiles: Default (the original game),
// Modern, Modern and Balanced, Extreme. Player profiles are bin\profiles\<name>.txt (option lines, and weapon lines with "weapon." in front)
// and follow changes made while they are active. The active profile's name is kept in jfg_profile.txt.
// Not part of profiles: cheats, the texture pack choice and texture dumping.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

constexpr const char* PROFILE_DIR = "profiles";
constexpr const char* PROFILE_CHOICE_FILE = "jfg_profile.txt";

const BuiltinProfile BUILTIN_PROFILES[] = {
    { "Default", "The original game, as it played on the N64.", "start_skips_cutscenes=0\n" },
    { "Modern", "Modern controls and visuals at 60 fps, with the original weapons and gameplay.",
      "no_blood=0\nstart_skips_cutscenes=1\nshow_fps=0\nwidescreen=1\nframe_rate=1\nshow_level=0\nshow_pack=0\ndump_by_level=0\n"
      "no_lod=1\nfar_objects=1\nssao=1\nssao_strength=30\nssao_radius=139\nbloom=1\nbloom_intensity=40\nbloom_threshold=19\n"
      "bloom_radius=6\ndual_stick=1\ninvert_aim_y=0\nswap_triggers=0\ndual_layout=1\ninvert_aim_normal=0\ninvert_camera_y=0\naim_from_character=0\n"
      "move_speed=110\ndiagonal_boost=0\nstrafe_speed=100\ncamera_speed_h=134\ncamera_speed_v=107\naim_shoulder=1\naim_centered=1\naim_speed=100\n"
      "aim_jump=1\nmouse_keyboard=1\nmouse_camera_speed=100\nmouse_aim_speed=100\ninvert_mouse_y=0\nrefuel_speed=482\nsniper_fixed_zoom=1\nsniper_zoom=365\n"
      "damage_taken=100\ndive_speed=100\nfly_speed=100\nfly_freeze=1\nfly_hide_hud=1\n" },
    { "Modern and Balanced", "Modern, plus rebalanced weapons. The balance changes are subjective.",
      "no_blood=0\nstart_skips_cutscenes=1\nshow_fps=0\nwidescreen=1\nframe_rate=1\nshow_level=0\nshow_pack=0\ndump_by_level=0\n"
      "no_lod=1\nfar_objects=1\nssao=1\nssao_strength=30\nssao_radius=139\nbloom=1\nbloom_intensity=40\nbloom_threshold=19\n"
      "bloom_radius=6\ndual_stick=1\ninvert_aim_y=0\nswap_triggers=0\ndual_layout=1\ninvert_aim_normal=0\ninvert_camera_y=0\naim_from_character=0\n"
      "move_speed=110\ndiagonal_boost=0\nstrafe_speed=100\ncamera_speed_h=134\ncamera_speed_v=107\naim_shoulder=1\naim_centered=1\naim_speed=100\n"
      "aim_jump=1\nmouse_keyboard=1\nmouse_camera_speed=100\nmouse_aim_speed=100\ninvert_mouse_y=0\nrefuel_speed=482\nsniper_fixed_zoom=1\nsniper_zoom=365\n"
      "damage_taken=100\ndive_speed=100\nfly_speed=100\nfly_freeze=1\nfly_hide_hud=1\n"
      "weapon.0.modify=1\nweapon.0.damage=1\nweapon.0.delay=12\nweapon.0.throw_delay=0\nweapon.0.lifetime=30\nweapon.0.spread_x=16\nweapon.0.spread_y=16\nweapon.0.speed_x100=12080\nweapon.0.size=105\nweapon.0.charge=100\nweapon.0.mines=8\nweapon.0.no_heat=0\n"
      "weapon.1.modify=1\nweapon.1.damage=6\nweapon.1.delay=30\nweapon.1.throw_delay=1\nweapon.1.lifetime=130\nweapon.1.spread_x=16\nweapon.1.spread_y=16\nweapon.1.speed_x100=2000\nweapon.1.size=100\nweapon.1.charge=100\nweapon.1.mines=8\nweapon.1.no_heat=0\n"
      "weapon.2.modify=1\nweapon.2.damage=1\nweapon.2.delay=4\nweapon.2.throw_delay=1\nweapon.2.lifetime=10\nweapon.2.spread_x=768\nweapon.2.spread_y=384\nweapon.2.speed_x100=5080\nweapon.2.size=90\nweapon.2.charge=100\nweapon.2.mines=8\nweapon.2.no_heat=0\n"
      "weapon.3.modify=1\nweapon.3.damage=5\nweapon.3.delay=9\nweapon.3.throw_delay=1\nweapon.3.lifetime=91\nweapon.3.spread_x=16\nweapon.3.spread_y=16\nweapon.3.speed_x100=3800\nweapon.3.size=100\nweapon.3.charge=215\nweapon.3.mines=8\nweapon.3.no_heat=0\n"
      "weapon.4.modify=1\nweapon.4.damage=10\nweapon.4.delay=12\nweapon.4.throw_delay=1\nweapon.4.lifetime=10\nweapon.4.spread_x=135\nweapon.4.spread_y=95\nweapon.4.speed_x100=3200\nweapon.4.size=100\nweapon.4.charge=100\nweapon.4.mines=8\nweapon.4.no_heat=0\n"
      "weapon.5.modify=1\nweapon.5.damage=8\nweapon.5.delay=30\nweapon.5.throw_delay=8\nweapon.5.lifetime=134\nweapon.5.spread_x=28\nweapon.5.spread_y=22\nweapon.5.speed_x100=1520\nweapon.5.size=100\nweapon.5.charge=100\nweapon.5.mines=8\nweapon.5.no_heat=0\n"
      "weapon.6.modify=0\nweapon.6.damage=6\nweapon.6.delay=0\nweapon.6.throw_delay=1\nweapon.6.lifetime=10\nweapon.6.spread_x=64\nweapon.6.spread_y=32\nweapon.6.speed_x100=3200\nweapon.6.size=100\nweapon.6.charge=100\nweapon.6.mines=8\nweapon.6.no_heat=0\n"
      "weapon.7.modify=1\nweapon.7.damage=12\nweapon.7.delay=40\nweapon.7.throw_delay=1\nweapon.7.lifetime=8\nweapon.7.spread_x=0\nweapon.7.spread_y=0\nweapon.7.speed_x100=34000\nweapon.7.size=80\nweapon.7.charge=100\nweapon.7.mines=8\nweapon.7.no_heat=0\n"
      "weapon.8.modify=1\nweapon.8.damage=15\nweapon.8.delay=0\nweapon.8.throw_delay=14\nweapon.8.lifetime=25\nweapon.8.spread_x=0\nweapon.8.spread_y=0\nweapon.8.speed_x100=1040\nweapon.8.size=100\nweapon.8.charge=100\nweapon.8.mines=8\nweapon.8.no_heat=0\n"
      "weapon.9.modify=1\nweapon.9.damage=2\nweapon.9.delay=0\nweapon.9.throw_delay=8\nweapon.9.lifetime=30\nweapon.9.spread_x=0\nweapon.9.spread_y=0\nweapon.9.speed_x100=1030\nweapon.9.size=100\nweapon.9.charge=100\nweapon.9.mines=8\nweapon.9.no_heat=0\n"
      "weapon.10.modify=1\nweapon.10.damage=10\nweapon.10.delay=0\nweapon.10.throw_delay=20\nweapon.10.lifetime=30\nweapon.10.spread_x=768\nweapon.10.spread_y=384\nweapon.10.speed_x100=800\nweapon.10.size=100\nweapon.10.charge=100\nweapon.10.mines=8\nweapon.10.no_heat=0\n"
      "weapon.11.modify=1\nweapon.11.damage=15\nweapon.11.delay=0\nweapon.11.throw_delay=1\nweapon.11.lifetime=30\nweapon.11.spread_x=80\nweapon.11.spread_y=32\nweapon.11.speed_x100=1250\nweapon.11.size=100\nweapon.11.charge=100\nweapon.11.mines=8\nweapon.11.no_heat=0\n"
      "weapon.12.modify=1\nweapon.12.damage=25\nweapon.12.delay=0\nweapon.12.throw_delay=30\nweapon.12.lifetime=30\nweapon.12.spread_x=80\nweapon.12.spread_y=32\nweapon.12.speed_x100=820\nweapon.12.size=100\nweapon.12.charge=100\nweapon.12.mines=8\nweapon.12.no_heat=0\n"
      "weapon.13.modify=1\nweapon.13.damage=10\nweapon.13.delay=30\nweapon.13.throw_delay=20\nweapon.13.lifetime=100\nweapon.13.spread_x=0\nweapon.13.spread_y=0\nweapon.13.speed_x100=1200\nweapon.13.size=100\nweapon.13.charge=100\nweapon.13.mines=8\nweapon.13.no_heat=0\n"
      "weapon.14.modify=1\nweapon.14.damage=8\nweapon.14.delay=0\nweapon.14.throw_delay=26\nweapon.14.lifetime=30\nweapon.14.spread_x=80\nweapon.14.spread_y=32\nweapon.14.speed_x100=1520\nweapon.14.size=100\nweapon.14.charge=100\nweapon.14.mines=8\nweapon.14.no_heat=0\n" },
    { "Extreme", "Modern visuals at 60 fps with everything turned up: much faster movement and over-the-top weapons, "
                 "just for fun.",
      "start_skips_cutscenes=1\nframe_rate=1\nwidescreen=1\nno_lod=1\nfar_objects=1\n"
      "ssao=1\nssao_strength=44\nssao_radius=143\nbloom=1\nbloom_intensity=52\nbloom_threshold=34\nbloom_radius=6\n"
      "dual_stick=1\ndual_layout=1\nmouse_keyboard=1\naim_shoulder=1\naim_centered=1\naim_jump=1\n"
      "move_speed=200\ndiagonal_boost=1\nstrafe_speed=150\ncamera_speed_h=150\ncamera_speed_v=150\naim_speed=150\n"
      "mouse_camera_speed=150\nmouse_aim_speed=150\n"
      // Pistol: rapid, strong, big bolts, no overheating.
      "weapon.0.modify=1\nweapon.0.damage=10\nweapon.0.delay=4\nweapon.0.speed_x100=20000\nweapon.0.size=250\nweapon.0.no_heat=1\n"
      // Homing missiles: fast and frequent.
      "weapon.1.modify=1\nweapon.1.damage=100\nweapon.1.delay=2\nweapon.1.speed_x100=4000\n"
      // Machine gun: a hose of big bolts, no spread.
      "weapon.2.modify=1\nweapon.2.damage=5\nweapon.2.delay=1\nweapon.2.spread_x=0\nweapon.2.spread_y=0\nweapon.2.size=200\n"
      // Plasma shotgun: charges 8 times as fast.
      "weapon.3.modify=1\nweapon.3.delay=4\nweapon.3.charge=800\n"
      // Tri-rocket launcher: fast and heavy.
      "weapon.5.modify=1\nweapon.5.damage=40\nweapon.5.delay=2\nweapon.5.speed_x100=4000\n"
      // Sniper rifle: huge damage, very fast bolts.
      "weapon.7.modify=1\nweapon.7.damage=100\nweapon.7.delay=2\nweapon.7.speed_x100=40000\nweapon.7.size=300\n"
      // Throwables: thrown at once, far, often.
      "weapon.8.modify=1\nweapon.8.damage=50\nweapon.8.delay=6\nweapon.8.throw_delay=4\nweapon.8.speed_x100=1600\n"
      "weapon.9.modify=1\nweapon.9.damage=20\nweapon.9.delay=2\nweapon.9.throw_delay=2\nweapon.9.speed_x100=1600\n"
      "weapon.12.modify=1\nweapon.12.damage=50\nweapon.12.delay=6\nweapon.12.throw_delay=4\n"
      "weapon.13.modify=1\nweapon.13.delay=4\nweapon.13.throw_delay=4\n"
      "weapon.14.modify=1\nweapon.14.delay=4\nweapon.14.throw_delay=4\nweapon.14.speed_x100=2000\n" },
};

std::string g_active_profile;          // "" = none chosen (the settings as they are)
std::string g_profile_snapshot;        // settings right after the active profile was applied (to show "changed")
bool g_profile_loaded = false;
char g_new_profile_name[48] = {};
std::string g_profile_message;
std::vector<std::string> g_user_profiles;
bool g_user_profiles_listed = false;

const BuiltinProfile* builtin_profile(const std::string& name) {
    for (const BuiltinProfile& p : BUILTIN_PROFILES) if (name == p.name) return &p;
    return nullptr;
}

std::filesystem::path user_profile_path(const std::string& name) {
    return std::filesystem::path(PROFILE_DIR) / (name + ".txt");
}

// All settings as profile text.
std::string settings_snapshot() {
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    return options_text(g) + (g_weapon_orig_read ? weapons_text("weapon.") : std::string());
}

void list_user_profiles() {
    g_user_profiles.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(PROFILE_DIR, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec) || it->path().extension() != ".txt") continue;
        const std::string name = it->path().stem().string();
        if (builtin_profile(name) == nullptr) g_user_profiles.push_back(name);
    }
    std::sort(g_user_profiles.begin(), g_user_profiles.end());
    g_user_profiles_listed = true;
}

void save_profile_choice() {
    if (std::FILE* f = std::fopen(PROFILE_CHOICE_FILE, "w")) { std::fputs(g_active_profile.c_str(), f); std::fclose(f); }
}

void load_profile_choice() {
    if (g_profile_loaded) return;
    g_profile_loaded = true;
    if (std::FILE* f = std::fopen(PROFILE_CHOICE_FILE, "r")) {
        char name[64] = {};
        if (std::fgets(name, sizeof(name), f)) {
            g_active_profile = name;
            while (!g_active_profile.empty() && (g_active_profile.back() == '\n' || g_active_profile.back() == '\r')) g_active_profile.pop_back();
        }
        std::fclose(f);
    }
    g_profile_snapshot = settings_snapshot();
}

// Resets every setting, applies the profile text, and stores the result as the current settings.
void apply_profile_text(const std::string& text) {
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    const State defaults;
    const std::string base = options_text(defaults);
    auto apply_lines = [](const std::string& t, bool weapons) {
        size_t pos = 0;
        while (pos < t.size()) {
            size_t end = t.find('\n', pos);
            if (end == std::string::npos) end = t.size();
            const std::string line = t.substr(pos, end - pos);
            pos = end + 1;
            const size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = line.substr(0, eq);
            const int value = std::atoi(line.c_str() + eq + 1);
            if (key.rfind("weapon.", 0) == 0) { if (weapons) apply_weapon_key(key.c_str() + 7, value); }
            else apply_option(key.c_str(), value);
        }
    };
    apply_lines(base, false);
    const bool weapons = g_weapon_orig_read;
    if (weapons) reset_weapons();
    apply_lines(text, weapons);
    push_render_options();
}

std::string read_text_file(const std::filesystem::path& path) {
    std::string out;
    if (std::FILE* f = std::fopen(path.string().c_str(), "r")) {
        char buf[4096]; size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
    }
    return out;
}

void write_user_profile(const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(PROFILE_DIR, ec);
    if (std::FILE* f = std::fopen(user_profile_path(name).string().c_str(), "w")) {
        std::fputs(settings_snapshot().c_str(), f);
        std::fclose(f);
    }
}

void choose_profile(const std::string& name) {
    const BuiltinProfile* b = builtin_profile(name);
    const std::string text = b ? std::string(b->text) : read_text_file(user_profile_path(name));
    g_active_profile = "";              // no autosave into the profile while it is being applied
    apply_profile_text(text);
    save_options();
    { std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex); if (g_weapon_orig_read) save_weapons(); }
    g_active_profile = name;
    save_profile_choice();
    g_profile_snapshot = settings_snapshot();
    g_profile_message = "Profile \"" + name + "\" applied.";
}

bool valid_profile_name(const std::string& name) {
    if (name.empty() || name.size() > 40) return false;
    for (char c : name) if (!(std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_')) return false;
    return name.front() != ' ' && name.back() != ' ';
}

// Changes made while a player profile is active are written to it (save_options / save_weapons).
void profile_autosave() {
    if (g_active_profile.empty() || builtin_profile(g_active_profile) != nullptr) return;
    write_user_profile(g_active_profile);
    g_profile_snapshot = settings_snapshot();
}

void draw_profiles() {
    load_profile_choice();
    if (!g_user_profiles_listed) list_user_profiles();
    ImGui::TextWrapped("A profile is a whole set of settings from the Options and Weapons tabs. Choosing one first puts "
                       "everything back to the original game, then applies the profile. Cheats, the texture pack and "
                       "texture dumping are not part of profiles.");
    ImGui::Spacing();

    const bool changed = settings_snapshot() != g_profile_snapshot;
    std::string label = g_active_profile.empty() ? std::string("(none - your own settings)") : g_active_profile;
    if (changed && !g_active_profile.empty() && builtin_profile(g_active_profile)) label += "  (changed)";
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, ImGui::GetFontSize() * 22.0f));
    if (ImGui::BeginCombo("Profile", label.c_str())) {
        for (const BuiltinProfile& p : BUILTIN_PROFILES) {
            if (ImGui::Selectable(p.name, g_active_profile == p.name)) choose_profile(p.name);
        }
        if (!g_user_profiles.empty()) ImGui::Separator();
        for (const std::string& n : g_user_profiles) {
            if (ImGui::Selectable(n.c_str(), g_active_profile == n)) choose_profile(n);
        }
        ImGui::EndCombo();
    }

    if (const BuiltinProfile* b = builtin_profile(g_active_profile)) {
        ImGui::TextWrapped("%s", b->description);
        if (changed) {
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Settings changed since choosing it. Built-in profiles are not changed; "
                                                                "save your settings as your own profile to keep them.");
            if (ImGui::Button("Go back to this profile")) choose_profile(g_active_profile);
        }
    }
    else if (!g_active_profile.empty()) {
        ImGui::TextDisabled("Your profile (profiles\\%s.txt). Changes you make are saved to it.", g_active_profile.c_str());
        if (ImGui::Button("Delete this profile")) ImGui::OpenPopup("Delete profile?");
        if (ImGui::BeginPopupModal("Delete profile?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::Text("Delete \"%s\"? The current settings stay as they are.", g_active_profile.c_str());
            if (ImGui::Button("Delete")) {
                std::error_code ec;
                std::filesystem::remove(user_profile_path(g_active_profile), ec);
                g_profile_message = "Profile \"" + g_active_profile + "\" deleted.";
                g_active_profile.clear();
                save_profile_choice();
                list_user_profiles();
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("New profile from the current settings");
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, ImGui::GetFontSize() * 22.0f));
    ImGui::InputText("Name", g_new_profile_name, sizeof(g_new_profile_name));
    const std::string new_name = g_new_profile_name;
    const bool name_ok = valid_profile_name(new_name) && builtin_profile(new_name) == nullptr;
    ImGui::BeginDisabled(!name_ok);
    const bool exists = std::find(g_user_profiles.begin(), g_user_profiles.end(), new_name) != g_user_profiles.end();
    if (ImGui::Button(exists ? "Overwrite profile" : "Save as new profile")) {
        write_user_profile(new_name);
        g_active_profile = new_name;
        save_profile_choice();
        g_profile_snapshot = settings_snapshot();
        list_user_profiles();
        g_profile_message = "Saved profile \"" + new_name + "\".";
        g_new_profile_name[0] = 0;
    }
    ImGui::EndDisabled();
    if (!new_name.empty() && !name_ok) ImGui::TextDisabled("Use letters, digits, spaces, - and _ (not the name of a built-in profile).");
    if (!g_profile_message.empty()) ImGui::TextDisabled("%s", g_profile_message.c_str());
}

// Scripted testing: JFG_DEBUG_PROFILE="<seconds>,<name>" chooses that profile once, that many seconds after start.
void debug_profile_tick() {
    static const char* env = std::getenv("JFG_DEBUG_PROFILE");
    static bool done = false;
    static const auto start = std::chrono::steady_clock::now();
    if (env == nullptr || done || !g_weapon_orig_read) return;
    int secs = 0; char name[48] = {};
    if (std::sscanf(env, "%d,%47[^\n]", &secs, name) != 2) { done = true; return; }
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(secs)) return;
    done = true;
    load_profile_choice();
    choose_profile(name);
    std::fprintf(stderr, "[jfg] debug: profile \"%s\" chosen\n", name);
}

} // namespace jfg
