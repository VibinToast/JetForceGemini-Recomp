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
    bool no_blood = false;
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
    int  frame_rate = 0;           // 0: the game's own pacing (30, dropping to 20), 1: 60 (runInOneFrame), 2: 60 + interpolated up to the monitor's rate
    bool widescreen = false;       // render the 3D view to the full window width, see jfg_widescreen_active()
    int  fov = 100;                // gameplay field of view, percent of the game's own (jfg_fov)
    bool vignette = false;         // darker screen edges in RT64 (src/shaders/VignettePS.hlsl), see push_render_options()
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
    bool dual_stick = false;       // modern twin-stick layout, see jfg_dual_stick()
    bool invert_aim_y = false;     // dual stick: right stick up aims down (the game's original behaviour)
    int  move_speed = 100;         // how fast the character moves over the ground, percent (any layout)
    bool diagonal_boost = false;   // dual stick: faster when the left stick is pushed diagonally, as running + sidestepping was
    int  strafe_speed = 100;       // dual stick: weight of the sideways part in that boost, percent
    bool camera_collision = true;  // dual stick: the camera stays inside the level (the game's own wall check)
    bool aim_from_character = false; // dual stick: on aiming, the view turns to the character instead of the character to the view
    bool dual_layout = true;       // dual stick: built-in controller layout instead of the controller bindings
    bool swap_triggers = false;    // dual stick: swap the N64 Z (fire) and R (aim) inputs
    int  camera_speed_h = 100;     // dual stick: camera speed sideways, percent
    int  camera_speed_v = 100;     // dual stick: camera speed up/down, percent
    bool aim_shoulder = false;     // aiming: view from over the character's right shoulder
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
    bool sniper_fixed_zoom = false; // sniper rifle: the zoom stays at sniper_zoom, the zoom buttons do nothing
    int  sniper_zoom = 250;        // sniper rifle: fixed zoom, percent of the unzoomed view (100..500, the game's range)
    int  damage_taken = 100;       // damage the player takes, percent (apply_damage_taken)
    int  dive_speed = 100;         // Vela underwater: swimming speed, percent (jfg_move_scale)
    int  fly_speed = 100;          // fly camera: movement speed, percent
    bool fly_freeze = true;        // fly camera: the game stands still while flying
    bool fly_hide_hud = true;      // fly camera: no HUD (the game's frontSetInstrumentsHide flag) while flying
    bool skip_tribal_check = false; // cheat (not saved): game flag 88 "all Tribals rescued" reads as set
    // cutscene fast-forward state
    bool skipping = false;
    int  skip_level = -1;
    int  skip_ticks = 0;
};
extern State g;

// jfg_options.cpp
void apply_option(const char* key, int value); // sets one option from its saved form (the options file, profiles)
void load_options();                           // once, at the first tick (or when the Select menu is built)
std::string options_text(const State& s);      // every option in its saved form, one "key=value" line each
void save_options();                           // also into the active player profile
void push_render_options();                    // hands the renderer options to RT64

} // namespace jfg
