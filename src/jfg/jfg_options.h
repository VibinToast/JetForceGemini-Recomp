// Jet Force Gemini port: the port's options - the F9 Options tab, the Select / Esc menu (jfg_settings_api.h) and
// profiles all read and change `g`. Saved in jfg_port_options.txt (jfg_options.cpp).
#pragma once

#include <string>

namespace jfg {

// The "Frame rate" option (60 / interpolated). It used to run at 30-45 and still look like 30: video retrace messages
// reached the game two at a time, held up behind a surplus controller message the runtime kept putting back into its
// queue (fixed in src/main.cpp, message_queue_control). False hides it again.
constexpr bool FRAME_RATE_OPTION = true;
// The "Depth fog" option (FogPS.hlsl in RT64) is hidden and off: in testing nothing showed in any level. The renderer
// pass, the option and its status line are kept for when it is looked at again. Set to true to bring it back.
constexpr bool DEPTH_FOG_OPTION = false;

// --- persistent menu state -----------------------------------------------------------------------------------------
struct State {
    bool invulnerable = false, infinite_ammo = false, all_weapons = false, all_levels = false;
    bool applied_invulnerable = false, applied_ammo = false, applied_weapons = false, applied_levels = false;
    int  warp_level = 0, warp_character = -1, warp_entrance = 0;
    char level_filter[32] = {};
    int  edit_character = -1;
    int  raw_flag = 0;
    bool pending_warp = false;
    // options
    bool show_fps = false;         // frame rate in the top right corner
    bool show_level = false;       // level number and name in the top right corner
    bool show_pack = false;        // texture pack in use in the top right corner
    bool dump_by_level = false;    // texture dumping: one sub-folder per level (texture_dump/<number> <name>)
    bool no_lod = false;           // always draw objects with their full-detail model, see apply_no_lod()
    bool far_objects = false;      // no draw distance for objects, see apply_no_lod()
    bool ssao = false;             // ambient occlusion pass in RT64 (src/shaders/SsaoPS.hlsl), see push_render_options()
    int  ssao_strength = 100;      // ambient occlusion: darkening, percent
    int  ssao_radius = 150;        // ambient occlusion: reach in game units
    bool bloom = false;            // bloom passes in RT64 (src/shaders/BloomPS.hlsl), see push_render_options()
    int  bloom_intensity = 60;     // bloom: amount added back, percent
    int  bloom_threshold = 70;     // bloom: brightness where glow starts, percent
    int  bloom_radius = 6;         // bloom: blur size (gaussian sigma in ~270-line bloom buffer pixels)
    bool texture_dump = false;     // write every texture drawn to bin/texture_dump as <hash>.png (not saved: off at start)
    int  frame_rate = 0;           // "Game Frame Rate": 0 = the game's own pacing (30, dropping to 20), 1 = 60 (runInOneFrame). A saved 2
                                   // (the old "60 + Interpolated") loads as 1 with Smooth Motion on
    int  smooth_motion = 0;        // "Smooth Motion": in-between frames from RT64, for either game rate. 0 = off, 1 = up to the
                                   // monitor's rate, else at most that many frames a second (one of smooth_motion_values)
    bool smooth_shadows = true;    // "Smooth Shadows": character shadows follow the in-between frames (jfg_shadow_interp.cpp)
    int  interp_limit = 0;         // only read from older settings files: the old Interpolation Limit, carried over to Smooth Motion
    bool widescreen = false;       // render the 3D view to the full window width, see jfg_widescreen_active()
    bool hud_corners = false;      // in widescreen, the HUD at the window's edges (jfg_hud_corners.cpp)
    int  fov = 100;                // gameplay field of view, percent of the game's own (jfg_fov)
    int  hit_flash = 100;          // red screen when the character is hit: strength, percent (jfg_hit_flash)
    int  shadow_opacity = 50;      // how dark character shadows are drawn, percent of the game's (jfg_shadow_alpha); 50 in every profile
    int  language = 0;             // "Language": 0 English, 1 French, 2 German, 3 Spanish, 4 Japanese (jfg_locale.cpp); not part of profiles
    bool vignette = false;        // darker screen edges in RT64 (src/shaders/VignettePS.hlsl), see push_render_options()
    int  vignette_strength = 40;   // vignette: how dark the corners get, percent
    int  vignette_size = 45;       // vignette: how far from the centre the darkening starts, percent of the way to the corners
    int  level_fog_distance = 100; // the game's own fog: how far away it is, percent (jfg_game_fog)
    bool no_level_fog = false;     // the game's own fog: off
    bool depth_fog = false;        // experimental: fog in RT64 from the depth buffer (src/shaders/FogPS.hlsl), in levels without fog
    int  fog_start = 1500;         // depth fog: where it starts, game units
    int  fog_end = 9000;           // depth fog: where it is thickest, game units
    int  fog_density = 50;         // depth fog: the most fog there is, percent
    int  fog_color = 0;            // depth fog: colour preset (FOG_COLORS)
    int  texture_filter = 0;       // 0: the N64's three-point texture filter, 1: bilinear (RT64 threePointFiltering)
    int  output_filter = 0;        // picture scaling to the window: 0 anti-aliased pixel scaling, 1 linear, 2 nearest
    bool start_skips_cutscenes = true;
    bool fast_pause = true;        // the pause menu's frame opens and closes FAST_PAUSE times as fast (frontMenuFrameTick's wrapper); off in Default
    bool pause_in_menu = true;     // the game stands still while the Select / Esc settings menu is open (jfg_freeze_world)
    bool auto_save = true;         // save to the file in play on arriving in a level and every few minutes (jfg_auto_save.cpp); on in every profile but Default
    int  auto_save_minutes = 5;    // auto save: minutes of play between saves, 0 = only on arriving in a level
    bool save_notification = true; // auto save: "Game saved" in the bottom right corner
    bool keep_area_progress = false; // saves keep an unfinished region's progress and room (jfg_area_progress.cpp)
    bool permanent_tribals = false; // rescued Tribals stay rescued when their region is entered again (jfg_tribal_saves.cpp)
    bool dual_stick = false;       // modern twin-stick layout, see jfg_dual_stick()
    bool invert_aim_y = false;     // dual stick: right stick up aims down (the game's original behaviour)
    int  move_speed = 100;         // how fast the character moves over the ground, percent (any layout)
    bool sprint = false;           // hold left-stick click / Left Shift to run faster (jfg_camera_controls)
    int  sprint_speed = 160;       // sprint: ground speed while it is held, percent
    bool sprint_aiming = false;    // sprint also works while aiming (the aim walk)
    bool sprint_swimming = false;  // sprint also works while swimming (Vela, at the surface and diving)
    int  sprint_swim_speed = 160;  // swim sprint: swimming speed while it is held, percent (on top of the Dive speed)
    bool normalize_diagonal = false; // running while sidestepping is no faster than running alone (jfg_move_scale)
    bool aim_even_speed = false;   // while aiming the character walks as fast sideways and backwards as forwards (jfg_move_scale)
    bool camera_collision = true;  // dual stick: the camera stays inside the level (the game's own wall check)
    bool aim_from_character = false; // dual stick: on aiming, the view turns to the character instead of the character to the view
    bool dual_layout = true;       // dual stick: built-in controller layout instead of the controller bindings
    bool swap_triggers = false;    // dual stick: swap the N64 Z (fire) and R (aim) inputs
    int  camera_speed_h = 100;     // dual stick: camera speed sideways, percent
    int  camera_speed_v = 100;     // dual stick: camera speed up/down, percent
    int  camera_distance = 100;    // dual stick: the camera's distance from the character, percent of the game's
    int  aim_drag_margin = 15;     // dual stick, aiming: how far the character may step aside before the camera follows, percent of the half screen
    int  aim_drag_return = 200;    // dual stick, aiming: how fast the camera comes back onto a character standing still, percent (0: never)
    int  aim_offset_x = 0;         // dual stick, aiming: the point the camera orbits, moved right of the character (units)
    int  aim_offset_y = 0;         // ... up
    int  aim_offset_z = 0;         // ... and the camera this much closer in (a zoom)
    bool aim_shoulder = false;     // aiming: view from over the character's right shoulder (dual stick: aim_view's preset)
    bool aim_centered = false;     // aiming: the view turns at once and the reticle stays in the middle
    bool aim_jump = false;         // aiming: the jump button (C-up) jumps instead of zooming, see aim_jump()
    int  aim_speed = 70;           // aiming: how fast the stick turns the view / moves the reticle, percent
    bool invert_camera_y = false;  // dual stick: right stick up lowers the camera instead of raising the view
    bool invert_aim_normal = false; // normal controls: flip the stick's up/down while aiming (R held)
    bool mouse_keyboard = false;   // mouse and keyboard in gameplay, on top of the dual-stick controls (src/input/controls.cpp)
    int  mouse_camera_speed = 100; // mouse: camera turn per mouse movement, percent
    int  mouse_aim_speed = 100;    // mouse: aim turn per mouse movement, percent
    bool invert_mouse_y = false;   // mouse: moving the mouse forward looks/aims down
    int  refuel_speed = 100;       // jetpack fuel pads: fuel added per tick, percent (jfg_refuel_amount)
    int  floyd_fuel = 100;         // Floyd missions: how long Floyd's fuel lasts, percent (jfg_floyd_fuel); 100 in every profile
    bool sniper_fixed_zoom = false; // sniper rifle: the zoom stays at sniper_zoom, the zoom buttons do nothing
    int  sniper_zoom = 250;        // sniper rifle: fixed zoom, percent of the unzoomed view (100..500, the game's range)
    int  damage_taken = 100;       // damage the player takes, percent (apply_damage_taken)
    bool unlimited_continues = false; // continues never run out (gameplay_tick)
    int  dive_speed = 100;         // Vela underwater: swimming speed, percent (jfg_move_scale)
    int  fly_speed = 100;          // fly camera: movement speed, percent
    bool fly_freeze = true;        // fly camera: the game stands still while flying
    bool fly_hide_hud = true;      // fly camera: no HUD (the game's frontSetInstrumentsHide flag) while flying
    bool skip_tribal_check = false; // cheat (not saved): game flag 88 "all Tribals rescued" reads as set
    bool squads_ignore = false;    // cheat (not saved): squads never see, hear or react to the heroes (jfg_squad_cheat.cpp)
    // cutscene fast-forward state
    bool skipping = false;
    int  skip_level = -1;
    int  skip_ticks = 0;
};

// Dual-stick aiming view (Over-the-Shoulder off: the player's sliders; on: fixed to a view from over the right
// shoulder, the sliders greyed out). The player's own values stay saved underneath.
struct AimView { int drag_margin, drag_return, offset_x, offset_y, zoom; };
constexpr AimView AIM_SHOULDER_VIEW = { 0, 100, 17, 21, 125 };
AimView aim_view();
bool aim_view_key(const char* key, int* preset_value); // one of the sliders aim_view covers, and its value in the preset
extern State g;

// jfg_options.cpp
void apply_option(const char* key, int value); // sets one option from its saved form (the options file, profiles)
// The old blood and Hide Floyd keys and "mod:<id>:<option>" keys: a mod's options (jfg_mod_options.cpp). False for
// any other key.
bool apply_mod_option(const char* key, int value);
void mod_options_frame(); // every UI frame
void load_options();                           // once, at the first tick (or when the Select menu is built)
std::string options_text(const State& s);      // every option in its saved form, one "key=value" line each
void save_options();                           // also into the active player profile
void push_render_options();                    // hands the renderer options to RT64

} // namespace jfg
