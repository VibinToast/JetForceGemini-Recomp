// Jet Force Gemini port: the lighting tool (F12) for modern lighting (JFG-125, Recomp-Docs RENDERER_ROADMAP.md 3a).
//
// A window of its own (src/debug_ui/backend.cpp opens it with F12) for everything the modern lighting can do:
//   settings     every lighting setting, saved for the level (lighting/level_N.txt, "own settings") or as the defaults
//                every other level uses (lighting/default.txt); quality settings (shadow maps, light count) are global
//   placed lights  lights of one's own, saved with the level: "placement mode" gives the game the controls, and each
//                shot (Z) puts the light being made where the middle of the screen points (a crosshair shows it: the
//                game's own collision ray, trackNearestIntersection, from the camera), or above the player when it
//                hits nothing; each can be moved, recoloured, resized, made to flicker, switched off or removed
//   level lights the game's own torches and lamps (its object lights), found again by where they stand: switched off,
//                recoloured, brighter or dimmer, larger or smaller, flickering; saved with the level
// Changes apply at once and are saved a moment later. Game thread only: the tool is drawn from jfg_debug_frame's UI
// frame, its tick runs every game tick, and the lighting reads the lights at rcpFast3d, all on the game thread.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace dino::debug_ui::backend {
bool lighting_tool_open();           // F12
bool lighting_tool_playing();        // placement mode: the game has the controls
void set_lighting_tool_playing(bool playing);
void set_lighting_tool_open(bool open);
}

namespace jfg {

namespace {
constexpr const char* LIGHTING_DIR = "lighting";
constexpr uint32_t OBJ_POS = 0x0C;                     // ObjectTransform: f32 x, y, z
constexpr uint32_t ADDR_activeLights = 0x800A1008, ADDR_activeLightCount = 0x800A1004; // see jfg_lighting.cpp
constexpr uint32_t OBJ_LIGHT_TYPE = 0x03, OBJ_LIGHT_POS = 0x18, OBJ_LIGHT_RADIUS = 0x24, OBJ_LIGHT_COLOUR = 0x40;
constexpr uint16_t Z_BUTTON = 0x2000;                  // fire
constexpr float SAME_LIGHT = 2.0f;                     // a game light this close to an override's place is that one

// --- settings -----------------------------------------------------------------------------------------------------
enum class Group { Lights, Shadows, Sun, Surfaces, Characters, Debug };
struct Setting {
    const char* key;
    const char* label;
    std::atomic<int>* i;
    std::atomic<bool>* b;
    int lo, hi;
    Group group;
    bool global; // quality: the same everywhere, saved with the defaults
    const char* help;
};

std::vector<Setting>& settings_table() {
    LightingSettings& s = lighting_settings();
    static std::vector<Setting> table = {
        { "mode", "Lights", &s.mode, nullptr, 0, 2, Group::Lights, false,
          "0: as the game. 1: the game's shots and flashes light the level per pixel. 2: its torches and lamps too." },
        { "level_ambient", "Level ambient (%)", &s.level_ambient, nullptr, 0, 199, Group::Lights, false,
          "The level's own colours, its ambient light. 100 is the original; lower is darker." },
        { "level_strength", "Shots and flashes (%)", &s.level_strength, nullptr, 0, 200, Group::Lights, false,
          "Brightness of the game's moving lights: shots, explosions, flashes." },
        { "object_strength", "Torches and lamps (%)", &s.object_strength, nullptr, 0, 200, Group::Lights, false,
          "Brightness of the game's torches and lamps on the level. Their glow is already painted into the level." },
        { "object_radius", "Torch and lamp reach (%)", &s.object_radius, nullptr, 25, 300, Group::Lights, false,
          "How far torches and lamps reach, as a share of their own radius (125 is what the game uses)." },
        { "facing", "Facing the light (%)", &s.facing, nullptr, 0, 100, Group::Lights, false,
          "How much a surface turned away from a light gets less of it. 0 keeps the game's look." },
        { "max_lights", "Most lights per frame", &s.max_lights, nullptr, 0, 64, Group::Lights, true,
          "The nearest lights are kept. Every lit pixel adds up each light." },
        { "shadows", "Shadow darkness (%)", &s.shadows, nullptr, 0, 100, Group::Shadows, false,
          "How dark shadows from the lights are. 0 is off, as in the game." },
        { "glow_shadow", "Shadows on painted glow (%)", &s.glow_shadow, nullptr, 0, 100, Group::Shadows, false,
          "How much of the torch glow painted into the level a full shadow takes away." },
        { "no_game_shadows", "Game's shadows off", nullptr, &s.no_game_shadows, 0, 1, Group::Shadows, false,
          "Hides the dark blob the game draws under characters and objects." },
        { "shadow_lights", "Lights with shadows", &s.shadow_lights, nullptr, 0, 4, Group::Shadows, true,
          "The nearest lights that cast shadows. Each draws the scene six more times a frame." },
        { "shadow_size", "Shadow detail", &s.shadow_size, nullptr, 128, 2048, Group::Shadows, true,
          "Pixels per side of each shadow map: sharper edges, more memory and work." },
        { "sun", "Directional light", nullptr, &s.sun, 0, 1, Group::Sun, false,
          "A light from one direction over the whole level, like the sun or the moon." },
        { "sun_yaw", "Direction (degrees around)", &s.sun_yaw, nullptr, 0, 359, Group::Sun, false, "" },
        { "sun_pitch", "Height (degrees up)", &s.sun_pitch, nullptr, -30, 90, Group::Sun, false,
          "90 is straight down from above; 0 is level with the horizon." },
        { "sun_r", "Colour R", &s.sun_r, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_g", "Colour G", &s.sun_g, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_b", "Colour B", &s.sun_b, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_intensity", "Intensity (%)", &s.sun_intensity, nullptr, 0, 200, Group::Sun, false, "" },
        { "sun_characters", "On characters (%)", &s.sun_characters, nullptr, 0, 200, Group::Sun, false,
          "How much of it falls on characters and objects." },
        { "detail", "Surface detail", &s.detail, nullptr, 0, 100, Group::Surfaces, false,
          "Bumps from the textures (their light and dark taken as height) that catch the lights. Tenths of a unit." },
        { "detail_characters", "Detail on characters (%)", &s.detail_characters, nullptr, 0, 200, Group::Surfaces, false,
          "How much of the surface detail characters and objects get. Their textures are mostly painted features (faces, "
          "clothes), which don't make good bumps; carved objects do." },
        { "shine", "Shine (%)", &s.shine, nullptr, 0, 200, Group::Surfaces, false,
          "Highlights where a light reflects towards the camera; brighter parts of a texture shine more." },
        { "sharpness", "Shine sharpness", &s.sharpness, nullptr, 1, 256, Group::Surfaces, false,
          "Small values: broad soft highlights; large values: small sharp ones." },
        { "characters", "New lighting for characters", nullptr, &s.characters, 0, 1, Group::Characters, false,
          "Lights characters and objects per pixel with their models' own surface directions, from the same lights." },
        { "character_lights", "Frame's lights on characters (%)", &s.character_lights, nullptr, 0, 100, Group::Characters, false,
          "How much shots, torches and placed lights add on characters on top of the game's own light on them." },
        { "no_object_lighting", "Game's character lighting off", nullptr, &s.no_object_lighting, 0, 1, Group::Characters, false,
          "Test: characters and objects without any light from the game." },
        { "no_game_lighting", "Game's level lights off", nullptr, &s.no_game_lighting, 0, 1, Group::Characters, false,
          "Test: the game's per-vertex level lighting skipped without replacing it." },
        { "debug_world", "Show world position", nullptr, &s.debug_world, 0, 1, Group::Debug, true, "" },
        { "debug_normal", "Show surface directions", nullptr, &s.debug_normal, 0, 1, Group::Debug, true, "" },
        { "debug_shadow", "Show shadows in red", nullptr, &s.debug_shadow, 0, 1, Group::Debug, true, "" },
        { "log", "Log lights to jfg_lighting.log", nullptr, &s.log, 0, 1, Group::Debug, true, "" },
    };
    return table;
}

int get(const Setting& t) { return t.i ? t.i->load() : (t.b->load() ? 1 : 0); }
void put(const Setting& t, int v) {
    if (t.i) t.i->store(std::clamp(v, t.lo, t.hi));
    else t.b->store(v != 0);
}

// --- state --------------------------------------------------------------------------------------------------------
int g_level = -32768;          // the level whose file is loaded
bool g_level_own = false;      // it has its own settings (else the defaults)
std::vector<PlacedLight> g_placed;
std::vector<LightOverride> g_overrides;
std::vector<int> g_built_in;   // the settings as built in, for "back to built-in"
bool g_dirty_level = false, g_dirty_default = false;
uint32_t g_dirty_tick = 0;
PlacedLight g_template;        // the light placement makes
int g_selected = -1;           // placed light being edited
bool g_pulse = true;           // pulse the selected placed light
uint32_t g_selected_game = 0;  // game light being edited (its object light)
float g_aim[3] = { 0, 0, 0 };
uint32_t g_aim_tick = 0;
float g_player[3] = { 0, 0, 0 };
bool g_have_player = false;
std::string g_message;
uint32_t g_message_tick = 0;
bool g_confirm_clear = false;

bool use_files() { return std::getenv("JFG_LIGHTING_NO_FILES") == nullptr; }
// Test switch: JFG_LIGHTING_TOOL=place opens the tool in placement mode at start (JFG_DEBUG_INPUT can then fire).
void test_switch() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* env = std::getenv("JFG_LIGHTING_TOOL")) {
        dino::debug_ui::backend::set_lighting_tool_open(true);
        dino::debug_ui::backend::set_lighting_tool_playing(std::strcmp(env, "place") == 0);
    }
}
std::filesystem::path default_path() { return std::filesystem::path(LIGHTING_DIR) / "default.txt"; }
std::filesystem::path level_path(int level) { return std::filesystem::path(LIGHTING_DIR) / ("level_" + std::to_string(level) + ".txt"); }
void say(const std::string& text) { g_message = text; g_message_tick = g_tick_count; }
void touched_level() { g_dirty_level = true; g_dirty_tick = g_tick_count; }
void touched_default() { g_dirty_default = true; g_dirty_tick = g_tick_count; }
void touched_setting(const Setting& t) {
    if (t.global || !g_level_own) touched_default();
    else touched_level();
}

// Key=value lines; "set.<key>" settings, "light.N" placed lights, "override.N" game light changes.
void write_settings(std::FILE* f, bool level_file) {
    for (const Setting& t : settings_table()) {
        if (t.group == Group::Debug) continue;
        if (level_file && t.global) continue;
        std::fprintf(f, "set.%s=%d\n", t.key, get(t));
    }
}

void save_default() {
    if (!use_files()) return;
    std::error_code ec;
    std::filesystem::create_directories(LIGHTING_DIR, ec);
    if (std::FILE* f = std::fopen(default_path().string().c_str(), "w")) {
        std::fputs("# Jet Force Gemini modern lighting: the defaults (lighting tool, F12)\n", f);
        write_settings(f, false);
        std::fclose(f);
    }
}

// Reads a lighting file: its settings (set.*) and, from a level's file, whether it has its own settings, its placed
// lights and its changed game lights.
void read_file(const std::filesystem::path& path, bool level_file, bool settings, bool lights = true) {
    std::FILE* f = std::fopen(path.string().c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char* key = line;
        const char* value = eq + 1;
        if (std::strncmp(key, "set.", 4) == 0) {
            if (!settings) continue;
            for (const Setting& t : settings_table()) {
                if (std::strcmp(t.key, key + 4) == 0 && !(level_file && t.global)) put(t, std::atoi(value));
            }
        }
        else if (level_file && std::strcmp(key, "own_settings") == 0) {
            g_level_own = std::atoi(value) != 0;
        }
        else if (level_file && lights && std::strncmp(key, "light.", 6) == 0) {
            PlacedLight p;
            int on = 1;
            if (std::sscanf(value, "%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d", &p.x, &p.y, &p.z, &p.r, &p.g, &p.b, &p.intensity,
                            &p.radius, &p.inner, &p.flicker, &p.flicker_speed, &on) >= 8) {
                p.on = on != 0;
                g_placed.push_back(p);
            }
        }
        else if (level_file && lights && std::strncmp(key, "override.", 9) == 0) {
            LightOverride o;
            int on = 1, recolour = 0;
            if (std::sscanf(value, "%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d", &o.x, &o.y, &o.z, &on, &recolour, &o.r, &o.g, &o.b,
                            &o.intensity, &o.radius, &o.flicker, &o.flicker_speed) >= 10) {
                o.on = on != 0;
                o.recolour = recolour != 0;
                g_overrides.push_back(o);
            }
        }
    }
    std::fclose(f);
}

void save_level() {
    if (!use_files() || g_level < 0) return;
    std::error_code ec;
    if (!g_level_own && g_placed.empty() && g_overrides.empty()) {
        std::filesystem::remove(level_path(g_level), ec); // nothing of its own left
        return;
    }
    std::filesystem::create_directories(LIGHTING_DIR, ec);
    if (std::FILE* f = std::fopen(level_path(g_level).string().c_str(), "w")) {
        std::fprintf(f, "# Jet Force Gemini modern lighting: level %d (lighting tool, F12)\n", g_level);
        std::fprintf(f, "own_settings=%d\n", g_level_own ? 1 : 0);
        if (g_level_own) write_settings(f, true);
        for (size_t i = 0; i < g_placed.size(); i++) {
            const PlacedLight& p = g_placed[i];
            std::fprintf(f, "light.%zu=%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, p.x, p.y, p.z, p.r, p.g, p.b, p.intensity,
                         p.radius, p.inner, p.flicker, p.flicker_speed, p.on ? 1 : 0);
        }
        for (size_t i = 0; i < g_overrides.size(); i++) {
            const LightOverride& o = g_overrides[i];
            std::fprintf(f, "override.%zu=%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, o.x, o.y, o.z, o.on ? 1 : 0,
                         o.recolour ? 1 : 0, o.r, o.g, o.b, o.intensity, o.radius, o.flicker, o.flicker_speed);
        }
        std::fclose(f);
    }
}

void flush() {
    if (g_dirty_default) save_default();
    if (g_dirty_level) save_level();
    g_dirty_default = g_dirty_level = false;
}

// A new level: the defaults (built in, then lighting/default.txt), then the level's own file.
void load_level(int level) {
    flush();
    g_level = level;
    g_level_own = false;
    g_placed.clear();
    g_overrides.clear();
    g_selected = -1;
    g_selected_game = 0;
    if (!use_files()) return;
    std::vector<Setting>& table = settings_table();
    for (size_t i = 0; i < table.size() && i < g_built_in.size(); i++) {
        if (!table[i].global && table[i].group != Group::Debug) put(table[i], g_built_in[i]);
    }
    read_file(default_path(), false, true);
    read_file(level_path(level), true, false, true);
    if (g_level_own) read_file(level_path(level), true, true, false);
}

bool aim_fresh() { return g_aim_tick != 0 && g_tick_count - g_aim_tick <= 3; }

// Where a light goes: the aim point (pulled back a little from the surface it hit), else in front of the player.
bool placement_point(float* out) {
    if (aim_fresh()) {
        float dx = 0, dy = 0, dz = 0;
        if (g_have_player) {
            dx = g_player[0] - g_aim[0];
            dy = g_player[1] + 40.0f - g_aim[1];
            dz = g_player[2] - g_aim[2];
        }
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float back = len > 1.0f ? std::min(12.0f, len * 0.5f) / len : 0.0f;
        out[0] = g_aim[0] + dx * back;
        out[1] = g_aim[1] + dy * back;
        out[2] = g_aim[2] + dz * back;
        return true;
    }
    if (g_have_player) {
        out[0] = g_player[0];
        out[1] = g_player[1] + 60.0f;
        out[2] = g_player[2];
        return true;
    }
    return false;
}

void place_light() {
    float p[3];
    if (!placement_point(p)) {
        say("No aim point or player to place the light at.");
        return;
    }
    PlacedLight light = g_template;
    light.x = p[0];
    light.y = p[1];
    light.z = p[2];
    g_placed.push_back(light);
    g_selected = (int)g_placed.size() - 1;
    touched_level();
    char text[96];
    std::snprintf(text, sizeof(text), "Light %d placed at %.0f, %.0f, %.0f.", g_selected + 1, p[0], p[1], p[2]);
    say(text);
}

LightOverride* override_for(float x, float y, float z) {
    for (LightOverride& o : g_overrides) {
        const float dx = o.x - x, dy = o.y - y, dz = o.z - z;
        if (dx * dx + dy * dy + dz * dz <= SAME_LIGHT * SAME_LIGHT) return &o;
    }
    return nullptr;
}

// --- UI helpers ---------------------------------------------------------------------------------------------------
void help(const char* text) {
    if (!text || !text[0]) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

bool slider_int(const char* label, int* v, int lo, int hi) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    return ImGui::SliderInt(label, v, lo, hi);
}

void draw_setting(const Setting& t) {
    if (t.b) {
        bool v = t.b->load();
        if (ImGui::Checkbox(t.label, &v)) {
            t.b->store(v);
            touched_setting(t);
        }
    }
    else {
        int v = t.i->load();
        if (slider_int(t.label, &v, t.lo, t.hi)) {
            put(t, v);
            touched_setting(t);
        }
    }
    help(t.help);
}

void draw_group(Group group) {
    for (const Setting& t : settings_table()) {
        if (t.group != group) continue;
        if (group == Group::Sun && (std::strcmp(t.key, "sun_g") == 0 || std::strcmp(t.key, "sun_b") == 0)) continue;
        if (group == Group::Sun && std::strcmp(t.key, "sun_r") == 0) {
            LightingSettings& s = lighting_settings();
            float c[3] = { s.sun_r.load() / 255.0f, s.sun_g.load() / 255.0f, s.sun_b.load() / 255.0f };
            if (ImGui::ColorEdit3("Colour", c)) {
                s.sun_r = (int)std::lround(c[0] * 255.0f);
                s.sun_g = (int)std::lround(c[1] * 255.0f);
                s.sun_b = (int)std::lround(c[2] * 255.0f);
                touched_setting(t);
            }
            continue;
        }
        draw_setting(t);
    }
}

bool colour_edit(const char* label, int* r, int* g, int* b) {
    float c[3] = { *r / 255.0f, *g / 255.0f, *b / 255.0f };
    if (!ImGui::ColorEdit3(label, c)) return false;
    *r = (int)std::lround(c[0] * 255.0f);
    *g = (int)std::lround(c[1] * 255.0f);
    *b = (int)std::lround(c[2] * 255.0f);
    return true;
}

// The fields a placed light has; true when one changed.
bool edit_placed(PlacedLight& p, bool with_position) {
    bool changed = false;
    ImGui::PushID(&p);
    if (with_position) {
        float pos[3] = { p.x, p.y, p.z };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::DragFloat3("Position", pos, 1.0f)) {
            p.x = pos[0];
            p.y = pos[1];
            p.z = pos[2];
            changed = true;
        }
        changed |= ImGui::Checkbox("On", &p.on);
    }
    changed |= colour_edit("Colour", &p.r, &p.g, &p.b);
    changed |= slider_int("Intensity (%)", &p.intensity, 0, 400);
    changed |= slider_int("Reach (units)", &p.radius, 10, 2000);
    help("How far the light reaches; it fades to nothing at this distance.");
    changed |= slider_int("Full-strength core (%)", &p.inner, 0, 95);
    help("The share of its reach where it is at full strength before fading.");
    changed |= slider_int("Flicker (%)", &p.flicker, 0, 100);
    help("How much it dims at most as it flickers, like a flame. 0 is steady.");
    changed |= slider_int("Flicker speed (%)", &p.flicker_speed, 10, 400);
    ImGui::PopID();
    return changed;
}

void draw_placed() {
    LightingSettings& s = lighting_settings();
    if (s.mode.load() < 1) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Lights is 0 (off) in the Lights tab: placed lights don't show.");
    ImGui::TextWrapped("Placement mode gives the game the controls: turn the camera so the crosshair in the middle of the "
                       "screen is on a spot and fire (Z) to place the light just in front of it, or above the player when "
                       "it points at nothing. F12 comes back here.");
    if (ImGui::Button("Placement mode")) {
        dino::debug_ui::backend::set_lighting_tool_playing(true);
        say("Placement mode: aim and fire to place lights; F12 to come back.");
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!aim_fresh() && !g_have_player);
    if (ImGui::Button(aim_fresh() ? "Place at the screen's middle" : "Place above the player")) place_light();
    ImGui::EndDisabled();
    ImGui::Checkbox("Pulse the selected light", &g_pulse);
    if (ImGui::CollapsingHeader("Light to place", ImGuiTreeNodeFlags_DefaultOpen)) edit_placed(g_template, false);

    ImGui::SeparatorText("Placed lights");
    if (g_placed.empty()) ImGui::TextDisabled("None on this level yet.");
    for (int i = 0; i < (int)g_placed.size(); i++) {
        const PlacedLight& p = g_placed[i];
        char label[128];
        std::snprintf(label, sizeof(label), "%d: %.0f, %.0f, %.0f   reach %d   %s%s##placed%d", i + 1, p.x, p.y, p.z, p.radius,
                      p.on ? "" : "(off) ", p.flicker > 0 ? "flickers" : "", i);
        ImVec4 colour(p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, 1.0f);
        ImGui::ColorButton("##c", colour, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        if (ImGui::Selectable(label, g_selected == i)) g_selected = (g_selected == i) ? -1 : i;
    }
    if (g_selected >= 0 && g_selected < (int)g_placed.size()) {
        ImGui::SeparatorText("Selected light");
        PlacedLight& p = g_placed[g_selected];
        if (edit_placed(p, true)) touched_level();
        float pt[3];
        ImGui::BeginDisabled(!placement_point(pt));
        if (ImGui::Button(aim_fresh() ? "Move to the screen's middle" : "Move above the player")) {
            placement_point(pt);
            p.x = pt[0];
            p.y = pt[1];
            p.z = pt[2];
            touched_level();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            PlacedLight copy = p;
            copy.x += 20.0f;
            g_placed.push_back(copy);
            g_selected = (int)g_placed.size() - 1;
            touched_level();
        }
        ImGui::SameLine();
        if (ImGui::Button("Use for new lights")) g_template = p;
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            g_placed.erase(g_placed.begin() + g_selected);
            g_selected = -1;
            touched_level();
        }
    }
    if (!g_placed.empty()) {
        ImGui::Spacing();
        if (!g_confirm_clear) {
            if (ImGui::Button("Remove all placed lights")) g_confirm_clear = true;
        }
        else {
            if (ImGui::Button("Yes, remove them all")) {
                g_placed.clear();
                g_selected = -1;
                g_confirm_clear = false;
                touched_level();
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep them")) g_confirm_clear = false;
        }
    }
}

void draw_level_lights() {
    ImGui::TextWrapped("The game's own torches and lamps here (nearest to the player first). Changes are kept for this "
                       "level and found again by where the light stands. Their glow painted into the level stays.");
    if (lighting_settings().mode.load() < 2) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Lights is below 2: torches and lamps don't light the level per pixel.");
    struct Row { uint32_t l; float x, y, z, d2; };
    std::vector<Row> rows;
    const uint32_t objects = rd32(ADDR_activeLights);
    const int32_t count = (int32_t)rd32(ADDR_activeLightCount);
    for (int32_t i = 0; valid_ptr(objects) && i < count && i < 256; i++) {
        const uint32_t l = rd32(objects + (uint32_t)i * 4);
        if (!valid_ptr(l) || !(rd8(l + OBJ_LIGHT_TYPE) & 0x40)) continue;
        Row r{ l, rdf(l + OBJ_LIGHT_POS), rdf(l + OBJ_LIGHT_POS + 4), rdf(l + OBJ_LIGHT_POS + 8), 0 };
        if (g_have_player) {
            const float dx = r.x - g_player[0], dy = r.y - g_player[1], dz = r.z - g_player[2];
            r.d2 = dx * dx + dy * dy + dz * dz;
        }
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d2 < b.d2; });
    if (rows.empty()) ImGui::TextDisabled("No torches or lamps active here.");
    const Row* selected = nullptr;
    for (const Row& r : rows) {
        const LightOverride* o = override_for(r.x, r.y, r.z);
        char label[160];
        std::snprintf(label, sizeof(label), "%.0f, %.0f, %.0f   %.0f away   reach %.0f%s##game%08X", r.x, r.y, r.z, std::sqrt(r.d2),
                      rdf(r.l + OBJ_LIGHT_RADIUS), o ? (o->on ? "   (changed)" : "   (off)") : "", r.l);
        ImVec4 colour(rd8(r.l + OBJ_LIGHT_COLOUR) / 255.0f, rd8(r.l + OBJ_LIGHT_COLOUR + 1) / 255.0f, rd8(r.l + OBJ_LIGHT_COLOUR + 2) / 255.0f, 1.0f);
        ImGui::ColorButton("##c", colour, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        if (ImGui::Selectable(label, g_selected_game == r.l)) g_selected_game = (g_selected_game == r.l) ? 0 : r.l;
        if (g_selected_game == r.l) selected = &r;
    }
    if (!selected) return;
    ImGui::SeparatorText("Selected torch or lamp");
    LightOverride* o = override_for(selected->x, selected->y, selected->z);
    LightOverride edit = o ? *o : LightOverride{};
    if (!o) {
        edit.x = selected->x;
        edit.y = selected->y;
        edit.z = selected->z;
        edit.r = rd8(selected->l + OBJ_LIGHT_COLOUR);
        edit.g = rd8(selected->l + OBJ_LIGHT_COLOUR + 1);
        edit.b = rd8(selected->l + OBJ_LIGHT_COLOUR + 2);
    }
    bool changed = ImGui::Checkbox("On", &edit.on);
    changed |= ImGui::Checkbox("Own colour", &edit.recolour);
    ImGui::BeginDisabled(!edit.recolour);
    changed |= colour_edit("Colour", &edit.r, &edit.g, &edit.b);
    ImGui::EndDisabled();
    changed |= slider_int("Intensity (% of its own)", &edit.intensity, 0, 400);
    changed |= slider_int("Reach (% of its own)", &edit.radius, 10, 400);
    changed |= slider_int("Flicker (%)", &edit.flicker, 0, 100);
    changed |= slider_int("Flicker speed (%)", &edit.flicker_speed, 10, 400);
    if (changed) {
        if (o) *o = edit;
        else g_overrides.push_back(edit);
        touched_level();
    }
    if (o && ImGui::Button("Back to the game's own")) {
        g_overrides.erase(g_overrides.begin() + (o - g_overrides.data()));
        touched_level();
    }
}
} // namespace

// --- what the lighting reads --------------------------------------------------------------------------------------
const std::vector<PlacedLight>& lighting_placed_lights() { return g_placed; }

const LightOverride* lighting_override_at(float x, float y, float z) {
    if (g_overrides.empty()) return nullptr;
    return override_for(x, y, z);
}

float lighting_flicker(uint32_t seed, int amount, int speed) {
    if (amount <= 0) return 1.0f;
    static const auto start = std::chrono::steady_clock::now();
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() * std::max(speed, 1) / 100.0f;
    const float phase = (float)((seed * 2654435761u) >> 8) / (float)(1u << 24) * 6.2831853f;
    // A flame: a slow sway and quicker flutters, 0 to 1.
    const float n = 0.5f + 0.22f * std::sin(t * 5.1f + phase) + 0.16f * std::sin(t * 11.7f + phase * 1.7f) +
                    0.12f * std::sin(t * 23.3f + phase * 2.9f);
    return 1.0f - std::clamp(amount, 0, 100) / 100.0f * std::clamp(n, 0.0f, 1.0f);
}

int lighting_pulse_light() {
    return (g_pulse && dino::debug_ui::backend::lighting_tool_open()) ? g_selected : -1;
}

bool lighting_active() {
    static const bool forced = std::getenv("JFG_PIXEL_LIGHTS") != nullptr || std::getenv("JFG_MODERN_LIGHTING") != nullptr;
    return forced || g.modern_lighting;
}

namespace {
// Where the middle of the screen points: the camera's view ray (Camera: s16 yaw at +0, s16 pitch at +2, position at
// +0x0C; the fly camera's conventions, jfg_camera.cpp) against the level with the game's trackNearestIntersection(0,
// &start, &end, &result, 0x2000, 0), as frontPlayerTarget calls it for the aim: non-zero on a hit, the distance to
// it at result + 0x20. The vectors live in the gap below $sp that call_game leaves.
void update_aim(recomp_context* ctx) {
    constexpr float REACH = 4000.0f;
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float heading = -angle_from_s16(rd16(cam)), pitch = angle_from_s16(rd16(cam + 2));
    const float dir[3] = { std::sin(heading) * std::cos(pitch), -std::sin(pitch), std::cos(heading) * std::cos(pitch) };
    const float from[3] = { rdf(cam + 0x0C), rdf(cam + 0x10), rdf(cam + 0x14) };
    const uint32_t base = ((uint32_t)ctx->r29 - 0xA0u) & ~0xFu;
    const uint32_t start = base, end = base + 0x10, result = base + 0x20;
    for (uint32_t c = 0; c < 3; c++) {
        wrf(start + c * 4, from[c]);
        wrf(end + c * 4, from[c] + dir[c] * REACH);
    }
    for (uint32_t i = 0; i < 0x40; i += 4) wr32(result + i, 0);
    const uint32_t hit = call_game(trackNearestIntersection, ctx, { 0, start, end, result, 0x2000, 0 });
    if (hit == 0) return;
    const float distance = rdf(result + 0x20);
    if (!(distance > 0.0f && distance <= REACH)) return;
    for (uint32_t c = 0; c < 3; c++) g_aim[c] = from[c] + dir[c] * distance;
    g_aim_tick = g_tick_count;
}
} // namespace

void lighting_tool_tick(recomp_context* ctx) {
    if (g_built_in.empty()) {
        for (const Setting& t : settings_table()) g_built_in.push_back(get(t));
        read_file(default_path(), false, true); // the defaults' global settings at once
    }
    test_switch();
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != g_level) load_level(level);

    g_have_player = false;
    if (valid_ptr(rd32(ADDR_gameplay))) {
        const uint32_t player = player_object(ctx);
        if (valid_ptr(player)) {
            for (uint32_t c = 0; c < 3; c++) g_player[c] = rdf(player + OBJ_POS + c * 4);
            g_have_player = true;
        }
    }

    if (dino::debug_ui::backend::lighting_tool_open() && g_have_player) update_aim(ctx);

    // Placement mode: each shot places the light being made.
    if (dino::debug_ui::backend::lighting_tool_open() && dino::debug_ui::backend::lighting_tool_playing()) {
        if (rd16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u)) & Z_BUTTON) place_light();
    }

    if ((g_dirty_default || g_dirty_level) && g_tick_count - g_dirty_tick > 30) flush();
}

void draw_lighting_tool(recomp_context* ctx) {
    (void)ctx;
    LightingSettings& s = lighting_settings();
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGui::SetNextWindowSize(ImVec2(620, 760), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 640, 20), ImGuiCond_FirstUseEver);
    const bool playing = dino::debug_ui::backend::lighting_tool_playing();
    if (playing) ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Lighting tool (F12)")) {
        ImGui::SetWindowFontScale(std::max(1.0f, ImGui::GetIO().DisplaySize.y / 900.0f));
        if (playing) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "PLACEMENT MODE: point the crosshair and fire (Z) to place a light.");
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "F12: back to the tool.");
            if (aim_fresh()) ImGui::Text("Crosshair on %.0f, %.0f, %.0f", g_aim[0], g_aim[1], g_aim[2]);
            else ImGui::Text("Crosshair on nothing: the light goes above the player.");
            // The crosshair: the middle of the screen, where the ray goes.
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
            const float r = std::max(8.0f, ImGui::GetIO().DisplaySize.y / 90.0f);
            const ImU32 col = aim_fresh() ? IM_COL32(120, 255, 140, 230) : IM_COL32(255, 170, 80, 230);
            dl->AddCircle(c, r, col, 24, 2.0f);
            dl->AddLine(ImVec2(c.x - r * 1.8f, c.y), ImVec2(c.x - r * 0.5f, c.y), col, 2.0f);
            dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x + r * 1.8f, c.y), col, 2.0f);
            dl->AddLine(ImVec2(c.x, c.y - r * 1.8f), ImVec2(c.x, c.y - r * 0.5f), col, 2.0f);
            dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x, c.y + r * 1.8f), col, 2.0f);
        }
        if (!lighting_active()) {
            ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Modern Lighting is off in the Graphics options.");
            ImGui::SameLine();
            if (ImGui::Button("Turn it on")) {
                g.modern_lighting = true;
                save_options();
            }
        }
        load_level_names();
        const std::string name = (g_level >= 0 && g_level < (int)g_level_names.size()) ? g_level_names[g_level] : "";
        ImGui::Text("Level %d %s", g_level, name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(g_level_own ? "(its own settings)" : "(the defaults)");
        if (!g_level_own) {
            if (ImGui::Button("Give this level its own settings")) {
                g_level_own = true;
                touched_level();
                say("This level now keeps its own settings; the defaults stay as they were.");
            }
            help("Copies the current settings into this level's file. Changes then stay with this level only.");
        }
        else {
            if (ImGui::Button("Back to the defaults for this level")) {
                g_level_own = false;
                save_level();
                const int level = g_level;
                g_level = -32768;
                load_level(level);
                say("This level uses the defaults again (its placed lights and changed torches stay).");
            }
            help("Forgets this level's own settings; it uses the defaults again.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Built-in settings")) {
            std::vector<Setting>& table = settings_table();
            for (size_t i = 0; i < table.size() && i < g_built_in.size(); i++) {
                if (table[i].group != Group::Debug) put(table[i], g_built_in[i]);
            }
            if (g_level_own) touched_level();
            touched_default();
            say("Settings back to the built-in ones.");
        }
        help("Every setting back to how the port ships it, for this level's settings or the defaults (whichever it uses).");
        if (!g_message.empty() && g_tick_count - g_message_tick < 60 * 6) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", g_message.c_str());
        ImGui::TextDisabled("Last frame: %d level lights, %d torches and lamps, %d placed", s.sent_level_lights.load(),
                            s.sent_object_lights.load(), s.sent_placed_lights.load());
        ImGui::Separator();
        if (ImGui::BeginTabBar("lighting_tabs")) {
            if (ImGui::BeginTabItem("Lights")) { draw_group(Group::Lights); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Shadows")) { draw_group(Group::Shadows); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Directional")) { draw_group(Group::Sun); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Surfaces")) { draw_group(Group::Surfaces); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Characters")) { draw_group(Group::Characters); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Placed lights")) { draw_placed(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Level lights")) { draw_level_lights(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug")) {
                draw_group(Group::Debug);
                ImGui::TextDisabled("Files: %s, %s", default_path().string().c_str(), level_path(g_level).string().c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace jfg
