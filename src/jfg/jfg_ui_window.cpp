// Jet Force Gemini port: native debug menu (Dear ImGui).
//
// Opened and closed with F9 or the ` (grave) key. Drawn through the base project's debug UI backend
// (src/debug_ui), which already owns an ImGui context and renders it on top of the game.
//
// This file: the F9 window and its Cheats and Level warp tabs. The other tabs are in jfg_profiles.cpp, jfg_weapons.cpp
// and jfg_ui_items.cpp. F10's tools are RmlUi, in jfg_ui_tools.cpp.
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_settings_api.h"

namespace jfg {

namespace {
void draw_status() {
    const int level = (int16_t)rd16(ADDR_currentLevel);
    const int next = (int16_t)rd16(ADDR_nextLevel);
    const int character = (int16_t)rd16(ADDR_currentCharacter);
    const char* name = (level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level].c_str() : "?";
    ImGui::Text("Level %d: %s", level, name);
    ImGui::Text("Next level %d   game mode %d   character %d (%s)", next, (int)rd32(ADDR_mainGameMode), character,
        (character >= 0 && character < 3) ? character_names[character] : "?");
}

// Layout helpers for the Options tab: a coloured heading with space above it, dimmed wrapped help text.
void options_section(const char* title) {
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.80f, 1.0f, 1.0f));
    ImGui::SeparatorText(title);
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.2f));
}
void options_help(const char* text) {
    ImGui::Indent(ImGui::GetFontSize() * 1.6f);
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - ImGui::GetFontSize());
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
    ImGui::Unindent(ImGui::GetFontSize() * 1.6f);
}
bool options_slider(const char* label, int* value, int lo, int hi) {
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
    ImGui::SliderInt(label, value, lo, hi);
    return ImGui::IsItemDeactivatedAfterEdit();
}

void draw_options() {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetFontSize() * 0.55f));
    bool changed = false;

    // Sub-tabs, one per group of features.
    if (ImGui::BeginTabBar("options_tabs", ImGuiTabBarFlags_FittingPolicyResizeDown)) {
        if (ImGui::BeginTabItem("General")) {
            options_section("General");
            changed |= ImGui::Checkbox("No blood (as in the Japanese version)", &g.no_blood);
            ImGui::BeginDisabled(g.no_blood);
            changed |= ImGui::Checkbox("Blood stays (splats on the ground and walls)", &g.gore_keep_blood);
            changed |= ImGui::Checkbox("Body parts stay", &g.gore_keep_limbs);
            changed |= options_slider("Blood amount %", &g.blood_amount, 100, 400);
            ImGui::EndDisabled();
            changed |= ImGui::Checkbox("START skips cutscenes", &g.start_skips_cutscenes);
            options_help("Also skips the logo screens at start-up and the story intro after creating a file. A skipped cutscene "
                         "still runs to its end at full machine speed, so everything it does (flags, items, the next level) still "
                         "happens.");
            if (g.skipping) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Skipping...");
            changed |= ImGui::Checkbox("Pause while the settings menu (Select / Esc) is open", &g.pause_in_menu);
            changed |= ImGui::Checkbox("Auto save", &g.auto_save);
            options_help("Saves to the file you are playing a few seconds after arriving in a level, and every few minutes of "
                         "play, as Z on the pause screen does. Not during cutscenes, races, Floyd missions or while paused.");
            changed |= options_slider("Minutes between auto saves (0: only on arriving)", &g.auto_save_minutes, 0, 30);
            changed |= ImGui::Checkbox("Show save notification (\"Game saved\" in the corner)", &g.save_notification);
            changed |= ImGui::Checkbox("Keep area progress", &g.keep_area_progress);
            options_help("Saving in the middle of an area also keeps what was done there (Tribals rescued, cleared rooms, "
                         "continues) and the room you were in; loading the file puts it all back. Off: the game's own "
                         "behaviour, where an area starts over from its entrance after loading.");
            changed |= ImGui::Checkbox("Permanent Tribal saves", &g.permanent_tribals);
            options_help("A Tribal you rescued stays rescued: going back into its area, a game over or loading the file does "
                         "not bring it back. Kept with each save of the file.");
            if (ImGui::Button("Back to the title screen (F5)")) g_reset_pending = true;
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Display")) {
            options_section("Display");
            changed |= ImGui::Checkbox("Show FPS", &g.show_fps);
            changed |= ImGui::Checkbox("Show level number and name", &g.show_level);
            changed |= ImGui::Checkbox("Show texture pack", &g.show_pack);
            changed |= ImGui::Checkbox("Widescreen (fill the window)", &g.widescreen);
            options_help("During gameplay only; the HUD stays in the middle. Things at the far left and right may appear late.");
            changed |= options_slider("Field of view %", &g.fov, 75, 140);
            options_help("Widens or narrows the view during gameplay, relative to each level's own field of view. Cutscenes, "
                         "menus, the pause screen and zoomed scopes keep the game's own.");
            changed |= options_slider("Hit flash opacity %", &g.hit_flash, 0, 100);
            options_help("How strongly the screen turns red when the character is hit. 0: no red flash.");
            changed |= ImGui::Checkbox("Hide Floyd", &g.hide_floyd);
            options_help("Floyd, his shadow and his sounds are gone while he follows the character; he still flies along "
                         "and shoots. Floyd missions and a second player flying him show him as usual.");
            changed |= ImGui::Checkbox("Disable LOD (full-detail models at any distance)", &g.no_lod);
            options_help("Characters and objects normally switch to a simpler model once they are a little way off.");
            changed |= ImGui::Checkbox("Draw distant objects (no object draw distance)", &g.far_objects);
            options_help("Objects normally fade out and disappear beyond a set distance. With this they stay drawn. Takes effect "
                         "for the objects in play; level scenery itself is not affected.");
            if (FRAME_RATE_OPTION) {
                static const char* const rates[] = { "30 (Original, drops to 20)", "60 (experimental)" };
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                changed |= ImGui::Combo("Game frame rate", &g.frame_rate, rates, IM_ARRAYSIZE(rates));
                if (frame_rate_needs_restart()) {
                    ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Used after the game restarts.");
                    ImGui::SameLine();
                    if (ImGui::Button("Restart now")) jfg::settings::restart_game();
                }
                options_help("How many times a second the game itself updates. 60: the game updates every video frame instead "
                             "of every second one; game speed stays the same. 60 is experimental: some enemies and bosses can "
                             "behave differently than at 30.");
                static const int smooth[] = { 0, 60, 90, 120, 144, 240, 1 };
                static const char* const smooth_names[] = { "Off", "60", "90", "120", "144", "240", "Monitor" };
                int smooth_index = 0;
                for (int i = 0; i < IM_ARRAYSIZE(smooth); i++) if (g.smooth_motion == smooth[i]) smooth_index = i;
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                if (ImGui::Combo("Frame interpolation", &smooth_index, smooth_names, IM_ARRAYSIZE(smooth_names))) {
                    g.smooth_motion = smooth[smooth_index];
                    changed = true;
                }
                options_help("How many frames a second you see: the renderer adds in-between frames to the game's own. Monitor: "
                             "up to your monitor's refresh rate. Rain and a few effects still move at the game's rate.");
                changed |= ImGui::Checkbox("Smooth shadows", &g.smooth_shadows);
                options_help("With Frame Interpolation, character shadows move with the in-between frames instead of at the "
                             "game's rate, so they no longer trail behind the character.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Effects")) {
            options_section("Ambient occlusion and bloom");
            changed |= ImGui::Checkbox("Ambient occlusion (SSAO)", &g.ssao);
            options_help("Darkens creases, corners and the ground around objects, worked out from the depth of the 3D view. "
                         "The HUD is drawn afterwards and is not affected.");
            if (g.ssao) {
                changed |= options_slider("Strength (%)##ssao", &g.ssao_strength, 10, 300);
                changed |= options_slider("Radius##ssao", &g.ssao_radius, 10, 300);
            }
            changed |= ImGui::Checkbox("Bloom", &g.bloom);
            options_help("Bright parts of the 3D view (lights, explosions, sky, shots) glow softly into their surroundings. The "
                         "HUD is drawn afterwards and does not glow.");
            if (g.bloom) {
                changed |= options_slider("Intensity (%)##bloom", &g.bloom_intensity, 0, 300);
                changed |= options_slider("Threshold (%)##bloom", &g.bloom_threshold, 0, 100);
                options_help("How bright a colour must be to glow. Lower: more of the picture glows.");
                changed |= options_slider("Radius##bloom", &g.bloom_radius, 1, 20);
            }
            changed |= ImGui::Checkbox("Vignette", &g.vignette);
            options_help("Darkens the edges and corners of the 3D view. The HUD is drawn afterwards and is not darkened.");
            if (g.vignette) {
                changed |= options_slider("Strength (%)##vignette", &g.vignette_strength, 0, 100);
                changed |= options_slider("Size (%)##vignette", &g.vignette_size, 0, 90);
                options_help("How far from the middle the darkening starts: higher keeps more of the picture clear.");
            }

            options_section("Fog");
            changed |= options_slider("Level fog distance %", &g.level_fog_distance, 50, 400);
            changed |= ImGui::Checkbox("No level fog", &g.no_level_fog);
            options_help("The fog the levels have themselves. Some levels use it to hide where the scenery stops being drawn, "
                         "so moving it back or turning it off can show things appearing in the distance.");
            if (DEPTH_FOG_OPTION) {
                changed |= ImGui::Checkbox("Depth fog (experimental)", &g.depth_fog);
                options_help("Adds distance fog to levels that have no fog of their own (it stays off in levels that do). Worked "
                             "out from the depth of the 3D view; the sky and the HUD are not fogged.");
            }
            if (DEPTH_FOG_OPTION && g.depth_fog) {
                ImGui::TextDisabled("%s", depth_fog_status().c_str());
                static const char* const colors[] = { "Haze", "Mist", "Dusk", "Night" };
                changed |= options_slider("Start##fog", &g.fog_start, 0, 10000);
                changed |= options_slider("Thickest at##fog", &g.fog_end, 500, 15000);
                changed |= options_slider("Density (%)##fog", &g.fog_density, 0, 100);
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                changed |= ImGui::Combo("Colour##fog", &g.fog_color, colors, IM_ARRAYSIZE(colors));
            }

            options_section("Filtering");
            {
                static const char* const textures[] = { "N64 three-point", "Bilinear" };
                static const char* const outputs[] = { "Anti-aliased pixel scaling", "Linear", "Nearest" };
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                changed |= ImGui::Combo("Texture filtering", &g.texture_filter, textures, IM_ARRAYSIZE(textures));
                options_help("Three-point is how the N64 smooths textures; bilinear is smoother, as on PC.");
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                changed |= ImGui::Combo("Picture scaling", &g.output_filter, outputs, IM_ARRAYSIZE(outputs));
                options_help("How the picture is scaled to the window. Nearest keeps pixels sharp; linear is softer.");
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controls")) {
            options_section("Controller");
            ImGui::BeginDisabled(g.dual_stick);
            changed |= ImGui::Checkbox("Invert aim up/down (normal controls)", &g.invert_aim_normal);
            ImGui::EndDisabled();
            changed |= ImGui::Checkbox("Dual-stick controls", &g.dual_stick);
            options_help("Right stick: moves the camera around the character. Left stick: runs in the pushed direction as seen "
                         "from the camera. While the aim button is held the right stick aims and the left stick moves, as in a "
                         "first-person game. Menus, the title screen, cutscenes, Floyd missions and the races (Mizar race, "
                         "multiplayer races) keep the normal layout.");

            if (g.dual_stick || g.mouse_keyboard) {
                options_section("Dual-stick settings");
                changed |= ImGui::Checkbox("Invert aim up/down (dual stick)", &g.invert_aim_y);
                changed |= ImGui::Checkbox("Invert camera up/down", &g.invert_camera_y);
                changed |= ImGui::Checkbox("Aim where the character faces (not where the camera looks)", &g.aim_from_character);
                changed |= ImGui::Checkbox("Camera collision", &g.camera_collision);
                options_help("Keeps the camera from going through walls and floors when it is swung round the character, using the "
                             "game's own wall check. With it off the camera can end up outside the level.");
                changed |= options_slider("Camera left/right %", &g.camera_speed_h, 10, 400);
                changed |= options_slider("Camera up/down %", &g.camera_speed_v, 10, 400);
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.3f));
                changed |= ImGui::Checkbox("Built-in button layout", &g.dual_layout);
                options_help("LT aim, RT fire. A jump, B or left-stick click crouch. RB / Y next weapon, LB / X previous weapon. "
                             "D-pad: weapon select; up/down zoom while aiming. View / right-stick click: N64 L. Untick to use your "
                             "own controller bindings instead.");
                changed |= ImGui::Checkbox("Swap triggers (RT aims, LT fires)", &g.swap_triggers);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mouse/Keyboard")) {
            options_section("Mouse and keyboard");
            changed |= ImGui::Checkbox("Mouse and keyboard controls", &g.mouse_keyboard);
            options_help("Plays like the dual-stick controls (switched on with it): WASD run, the mouse turns the camera, hold the "
                         "right button to aim and click the left to fire, the wheel changes weapon (down: next, up: previous; "
                         "Q / E too). Space jumps, Left Ctrl or C crouches, Enter is START, Tab is N64 L, 1-4 are the D-pad "
                         "(1 / 3 zoom in / out while aiming). The cursor is captured during gameplay only; F9 and menus free it.");
            if (g.mouse_keyboard) {
                changed |= options_slider("Mouse camera speed %", &g.mouse_camera_speed, 10, 400);
                changed |= options_slider("Mouse aim speed %", &g.mouse_aim_speed, 10, 400);
                changed |= ImGui::Checkbox("Invert mouse up/down", &g.invert_mouse_y);
            }
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Move & Aim")) {
            options_section("Movement (any control layout)");
            changed |= options_slider("Movement speed %", &g.move_speed, 25, 400);
            changed |= ImGui::Checkbox("Sprint", &g.sprint);
            options_help("Hold the left stick click (controller) or Left Shift (mouse and keyboard) to run faster. In the "
                         "built-in dual-stick layout the left stick click then sprints instead of crouching (B still crouches).");
            changed |= options_slider("Sprint speed %", &g.sprint_speed, 100, 300);
            changed |= ImGui::Checkbox("Sprint while aiming", &g.sprint_aiming);
            options_help("Sprinting also works while aiming, so the character moves faster with the aim held.");
            changed |= ImGui::Checkbox("Sprint while swimming", &g.sprint_swimming);
            options_help("Sprinting also works in the water: Vela swims and dives faster while the sprint button is held.");
            changed |= options_slider("Swim sprint speed %", &g.sprint_swim_speed, 100, 300);
            changed |= ImGui::Checkbox("Normalize diagonal speed", &g.normalize_diagonal);
            options_help("In the game, running forward while sidestepping adds the two speeds together, so going diagonally "
                         "is about 40% faster. With this on a diagonal is as fast as running straight.");

            options_section("Aiming (any control layout)");
            changed |= ImGui::Checkbox("Over-the-shoulder view while aiming", &g.aim_shoulder);
            changed |= ImGui::Checkbox("Keep the reticle centered", &g.aim_centered);
            changed |= ImGui::Checkbox("Jump while aiming", &g.aim_jump);
            options_help("The jump button (C-up; A in the dual-stick layout, Space with mouse and keyboard) jumps while aiming, "
                         "also while walking or strafing: the character leaves the aim for the jump, the left stick steers it "
                         "like a normal jump, and it aims again on landing if the aim button is still held. Moving the left "
                         "stick forward while aiming still just walks. The sniper scope zooms with the D-pad (dual stick) or "
                         "1 / 3 (mouse and keyboard); on the original layout C-up jumps instead of zooming.");
            changed |= options_slider("Aim speed %", &g.aim_speed, 10, 300);
            changed |= ImGui::Checkbox("Sniper rifle: fixed zoom", &g.sniper_fixed_zoom);
            changed |= options_slider("Sniper zoom %", &g.sniper_zoom, 100, 500);
            options_help("With the fixed zoom the zoom buttons do nothing, so walking forward and back while aiming the "
                         "sniper rifle works as with the other weapons.");

            options_section("Player");
            changed |= options_slider("Damage taken %", &g.damage_taken, 0, 400);
            changed |= ImGui::Checkbox("Unlimited continues", &g.unlimited_continues);
            options_help("Dying never uses up the last continue, so an area is never restarted from its entrance.");
            changed |= options_slider("Jetpack refuel speed %", &g.refuel_speed, 25, 1000);
            changed |= options_slider("Vela's dive speed %", &g.dive_speed, 50, 400);

            options_section("Fly camera (F7)");
            if (ImGui::Button(g_fly.active ? "Stop flying (F7)" : "Fly camera (F7)")) g_fly_toggle_request = true;
            changed |= options_slider("Fly speed %", &g.fly_speed, 10, 1000);
            changed |= ImGui::Checkbox("Freeze the game while flying", &g.fly_freeze);
            changed |= ImGui::Checkbox("Hide the HUD while flying", &g.fly_hide_hud);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Textures")) {
            options_section("Textures");
            ImGui::Checkbox("Dump textures to the texture_dump folder", &g.texture_dump);
            changed |= ImGui::Checkbox("One folder per level", &g.dump_by_level);
            options_help("Dumps into texture_dump\\<level number> <level name> instead. Each level starts its own set, so "
                         "textures used in several levels are saved in each of them.");
            options_help("Saves every texture the game draws, the first time it appears, as <hash>.png. Leave it on while you "
                         "play through the areas you want; it is off again at the next start. Upscale the files without "
                         "renaming them and put them in a pack folder (below).");
            {
                static int dumped = -1, frame = 0;
                if (frame++ % 60 == 0) { // count the dumped files about once a second while the menu is open
                    std::error_code ec; int n = 0;
                    for (std::filesystem::recursive_directory_iterator it("texture_dump", ec), end; !ec && it != end; it.increment(ec)) {
                        if (it->is_regular_file(ec)) n++;
                    }
                    dumped = ec ? -1 : n;
                }
                if (dumped >= 0) ImGui::TextDisabled("texture_dump folder: %d files", dumped);
            }
            {
                static std::vector<std::string> packs;
                static int frame = 0;
                if (frame++ % 60 == 0) packs = list_texture_packs(); // look for new pack folders about once a second
                std::string current;
                { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                if (ImGui::BeginCombo("Texture pack", pack_display_name(current).c_str())) {
                    if (ImGui::Selectable(pack_display_name("").c_str(), current.empty())) set_texture_pack("");
                    for (const std::string& p : packs) {
                        if (ImGui::Selectable(pack_display_name(p).c_str(), p == current)) set_texture_pack(p);
                    }
                    ImGui::EndCombo();
                }
                ImGui::SameLine();
                if (ImGui::Button("Reload")) set_texture_pack(current);
                const int n = g_texture_pack_count.load();
                if (!current.empty() && n >= 0) ImGui::TextDisabled("%s: %d replacement textures loaded", current.c_str(), n);
            }
            options_help("Put each pack in its own folder (texture_packs\\<name>, next to the game) or as a packed .rtz file (texture_packs\\<name>.rtz). Images named like the dumps replace "
                         "those textures, at any size; a suffix after the name (as Upscayl adds) is fine. F6 switches to the "
                         "next pack (and to Off) while playing, to compare. Reload after adding or changing files.");
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    push_render_options();
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
    ImGui::PopStyleVar();
    if (changed) save_options();
}
} // namespace

void draw_cheats() {
    ImGui::Checkbox("Invulnerable (game's invulnerabilityCheat)", &g.invulnerable);
    ImGui::Checkbox("Unlimited ammo (game's DebugAmmoFlag)", &g.infinite_ammo);
    ImGui::Checkbox("All weapons (game's allweaponsCheat)", &g.all_weapons);
    ImGui::Checkbox("All levels open (game's allLevelCheat)", &g.all_levels);
    bool game_debug = rd32(ADDR_debugMenuEnable) != 0;
    if (ImGui::Checkbox("Game's own debug display (debugMenuEnable)", &game_debug)) {
        wr32(ADDR_debugMenuEnable, game_debug ? 1 : 0);
    }
    ImGui::Checkbox("Skip the Tribal check (all Tribals count as rescued)", &g.skip_tribal_check);
    ImGui::TextDisabled("The first four switch the game's own retail cheat flags. The Tribal check makes the game's\n"
                        "\"all Tribals rescued\" test pass (game flag 88) without changing your save.");
}

void draw_warp() {
    if (g.warp_character < 0) {
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        g.warp_character = (c >= 0 && c < 3) ? c : 1;
    }
    ImGui::InputText("Filter", g.level_filter, sizeof(g.level_filter));
    if (ImGui::BeginListBox("##levels", ImVec2(-FLT_MIN, 14 * ImGui::GetTextLineHeightWithSpacing()))) {
        for (int i = 0; i < (int)g_level_names.size(); i++) {
            if (!contains_ci(g_level_names[i], g.level_filter)) continue;
            char label[64];
            std::snprintf(label, sizeof(label), "%3d  %s", i, g_level_names[i].c_str());
            if (ImGui::Selectable(label, g.warp_level == i)) g.warp_level = i;
        }
        ImGui::EndListBox();
    }
    ImGui::InputInt("Level number", &g.warp_level);
    g.warp_level = std::clamp(g.warp_level, 0, std::max(0, (int)g_level_names.size() - 1));
    ImGui::Combo("Character", &g.warp_character, character_names, 3);
    ImGui::InputInt("Entrance", &g.warp_entrance);
    if (ImGui::Button("Go to level")) g.pending_warp = true;
    ImGui::SameLine();
    if (ImGui::Button("Restart current level")) {
        g.warp_level = (int16_t)rd16(ADDR_currentLevel);
        g.pending_warp = true;
    }
    ImGui::TextDisabled("Names are the developers' internal ones (Forest = Goldwood, CargoShip = S.S. Anubis,\n"
                        "AlphaSewer = Sekhmet, ...). Entries ending in Seq are cutscenes. Use from inside a game.");
}

// A slider straight on a live setting: stored on every change (options_slider reports only the release, and the value
// is read back from the setting each frame, so the drag would snap back).
static void live_slider(const char* label, std::atomic<int>& setting, int lo, int hi) {
    int value = setting.load();
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
    if (ImGui::SliderInt(label, &value, lo, hi)) setting = value;
}

// Lighting tab: modern lighting, in progress (jfg_lighting.cpp). Changes apply at once; nothing is saved.
static void draw_lighting() {
    LightingSettings& s = lighting_settings();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, ImGui::GetFontSize() * 0.55f));
    ImGui::TextDisabled("Work in progress (JFG-125). Changes apply at once and are not saved.");

    options_section("Level ambient");
    live_slider("Level ambient (%)", s.level_ambient, 0, 199);
    options_help("How bright the level's own colours are: its ambient light, painted in by the developers. 100 is the "
                 "original; lower makes the levels darker so lights stand out more. Works with per-pixel lighting off too. "
                 "Later this becomes a per-level adjustment.");

    options_section("Per-pixel lighting");
    static const char* const modes[] = { "Off (the game's own lighting)", "The game's level lights per pixel",
                                         "Also torches and lamps on the level" };
    int mode = s.mode.load();
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
    if (ImGui::Combo("Mode", &mode, modes, IM_ARRAYSIZE(modes))) s.mode = mode;
    options_help("The game lights its levels by brightening vertices near each light, which smears the light over whole "
                 "triangles. Per pixel, the same lights make smooth pools. The game only does this for shots, flashes and "
                 "the like; torches and lamps light characters and objects only, and their glow on the walls is painted "
                 "into the level. The third mode lets them light the level too, which the original never does.");
    live_slider("Level lights (%)", s.level_strength, 0, 200);
    options_help("Shots, flashes and other lights the game puts on the level. 100 is the original brightness.");
    ImGui::BeginDisabled(mode < 2);
    live_slider("Torches and lamps (%)", s.object_strength, 0, 200);
    options_help("How strongly torches and lamps light the level. Their glow is already painted into the walls, so high "
                 "values light them twice.");
    live_slider("Torch and lamp reach (%)", s.object_radius, 25, 400);
    options_help("How far their light reaches, as a share of the light's own radius. 125 is what the game uses when it "
                 "turns one of its object lights into a level light.");
    ImGui::EndDisabled();
    live_slider("Facing the light (%)", s.facing, 0, 100);
    options_help("How much a surface turned away from a light gets less of it, which gives walls and corners depth. The "
                 "game never does this: 0 keeps its look. Surfaces get one direction per triangle, so low-detail ones look "
                 "faceted.");
    live_slider("Shadows of characters and objects (%)", s.shadows, 0, 100);
    options_help("Characters and objects that have a shadow also block these lights: they throw a soft shadow away from "
                 "a torch or a flash. 0 is off, as in the game.");
    live_slider("Shadow caster size (%)", s.caster_size, 25, 300);
    bool no_game_shadows = s.no_game_shadows.load();
    if (ImGui::Checkbox("Game's shadows off", &no_game_shadows)) s.no_game_shadows = no_game_shadows;
    options_help("Hides the dark blob the game draws on the ground under characters and objects. They still block "
                 "these lights.");
    live_slider("Most lights per frame", s.max_lights, 0, 32);
    ImGui::TextDisabled("Last frame: %d level lights, %d torches and lamps", s.sent_level_lights.load(),
                        s.sent_object_lights.load());

    options_section("Characters and objects");
    bool characters = s.characters.load();
    if (ImGui::Checkbox("New lighting for characters and objects", &characters)) s.characters = characters;
    options_help("Lights characters and objects per pixel with their models' own surface directions, from the same "
                 "ambient and lights the game uses, instead of the game's per-vertex lighting. The level ambient slider "
                 "scales their ambient too.");
    ImGui::BeginDisabled(!characters);
    live_slider("Frame's lights on characters (%)", s.character_lights, 0, 100);
    options_help("How much the per-pixel lights (shots, flashes, torches and lamps) add on characters, with facing and "
                 "shadows. The game's own light on them already comes from the nearest lights, so this adds to it.");
    ImGui::EndDisabled();

    options_section("Debug");
    bool flag = s.no_game_lighting.load();
    if (ImGui::Checkbox("Game's per-vertex level lighting off", &flag)) s.no_game_lighting = flag;
    options_help("Leaves the levels with only their painted-in colours, without adding anything per pixel either (unless a "
                 "mode is on).");
    flag = s.no_object_lighting.load();
    if (ImGui::Checkbox("Game's lighting of characters and objects off", &flag)) s.no_object_lighting = flag;
    options_help("Leaves characters and objects without the light the game puts on them from nearby lights (test for "
                 "moving them to the new lighting).");
    flag = s.debug_world.load();
    if (ImGui::Checkbox("Show world position", &flag)) s.debug_world = flag;
    options_help("Colours every 3D surface by where it is in the world, repeating every 256 units; magenta has no position "
                 "(the HUD, rectangles).");
    flag = s.debug_normal.load();
    if (ImGui::Checkbox("Show surface directions", &flag)) s.debug_normal = flag;
    options_help("Colours every 3D surface by which way it faces: red east-west, green up, blue north-south.");
    flag = s.debug_shadow.load();
    if (ImGui::Checkbox("Show shadows from the lights", &flag)) s.debug_shadow = flag;
    options_help("Paints red where characters and objects block a torch, lamp or flash, at full darkness, whatever the "
                 "shadow slider says.");
    flag = s.log.load();
    if (ImGui::Checkbox("Log lights to jfg_lighting.log", &flag)) s.log = flag;
    ImGui::PopStyleVar();
}

// F9: the original all-in-one window (jfg_debug_frame, while the debug UI is open without the toolbar).
void draw_debug_window(recomp_context* ctx) {
    ImGui::GetIO().FontGlobalScale = 1.0f; // the toolbar scales globally; this window scales itself
    ImGui::SetNextWindowSize(ImVec2(720, 820), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Jet Force Gemini debug menu (F9 closes)")) {
        ImGui::SetWindowFontScale(std::max(1.0f, ImGui::GetIO().DisplaySize.y / 720.0f));
        draw_status();
        ImGui::Separator();
        const bool in_game = valid_ptr(rd32(ADDR_gameplay));
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Profiles")) { draw_profiles(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Cheats")) { draw_cheats(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Options")) { draw_options(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Lighting")) { draw_lighting(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Level warp")) { draw_warp(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Weapons")) { draw_weapons(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Items")) {
                if (in_game) draw_inventory(ctx); else ImGui::TextDisabled("No game loaded.");
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace jfg
