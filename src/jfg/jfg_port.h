// Jet Force Gemini port: what the port's files in src/jfg share with each other, by the file that defines it.
// (Game access is in jfg_game.h, the options in jfg_options.h, the functions the rest of the program calls in
// jfg_hooks.h.)
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"

namespace jfg {

// --- jfg_tick.cpp ------------------------------------------------------------------------------------------------
extern bool g_story_skip_pending; // START in the story intro: go to character select at the next tick
extern bool g_reset_pending, g_reset_press_start; // F5, see jfg_debug_frame
// A level change at the next tick, as the level warp does; front_mode -1 keeps the game's front-end mode.
void request_warp(int level, int character, int entrance, int front_mode);

// --- jfg_launch.cpp ----------------------------------------------------------------------------------------------
void launch_tick(recomp_context* ctx); // every tick: --start-level, --mods-only report

// --- jfg_texture_packs.cpp ---------------------------------------------------------------------------------------
extern std::atomic<int> g_texture_pack_count; // textures listed in the pack's rt64.json, -1 = pack not in use
// Texture packs: folders in bin/texture_packs/<name> (and the older single bin/texture_pack). The choice is kept in
// bin/jfg_texture_pack.txt; the renderer picks it up through jfg_texture_pack_current().
extern std::mutex g_pack_mutex;
extern std::string g_pack_folder; // relative to the app folder, "" = none
extern std::chrono::steady_clock::time_point g_pack_toast; // when F6 last switched packs (on-screen note)
std::vector<std::string> list_texture_packs();
// A pack's credits.json (at the top of its folder or .rtz) and the image it names; all empty if it has none.
struct PackInfo {
    bool has_json = false;
    int serial = 0;          // a new number each time the details are read again (the files changed)
    std::string id, version, variant, author, website, description; // id + variant: which pack and size (1K, 2K, 4K)
    std::vector<char> image; // the image file's bytes (pack_image.png unless credits.json names another)
};
PackInfo texture_pack_info(const std::string& folder); // re-read when the pack's files change
std::string pack_display_name(const std::string& folder); // the folder / .rtz name, without .rtz or a _v1.1.0 at its end
void set_texture_pack(const std::string& folder); // saves the choice and makes the renderer (re)load it
void load_texture_pack_choice();
void next_texture_pack();                         // F6
// Split packs (<pack>.rtz.001, .002, ...) in texture_packs: joined into <pack>.rtz on a thread of their own.
void join_split_packs();                          // looks for parts and starts joining a whole set
std::string pack_join_text(int& finished);        // progress / result for the Textures page; finished: joins done
bool pack_join_running();
// --- jfg_pack_install.cpp ----------------------------------------------------------------------------------------
void delete_replaced_packs();                     // at start-up: packs an update replaced while they were open
void update_dump_folder();                        // texture dumping, "One folder per level", and the scene tag of draw_trace.txt

// --- jfg_frame_rate.cpp ------------------------------------------------------------------------------------------
extern std::atomic<float> g_fps;        // game frames per second, for the "Show FPS" option
extern std::atomic<float> g_screen_fps; // frames the renderer presented per second
void frame_step_key(bool step);         // End (freeze, unfreeze) or F8 (one frame) for frame step
int game_frame_rate();                  // the "Frame rate" this run uses (taken at the game's first ask; game side)
int running_frame_rate();               // the same, or the option while the game has not asked yet (any thread)
bool frame_rate_needs_restart();        // the option was changed while the game runs: it applies after a restart

// --- jfg_cutscene_skip.cpp ---------------------------------------------------------------------------------------
bool in_cutscene();
extern bool g_audio_catch_up; // a music player's event queue is nearly full: let the audio thread run (jfg_vi_skip)
extern int g_skip_real_frames;

// --- jfg_controls.cpp --------------------------------------------------------------------------------------------
// 1 brings back the former dual-stick over-the-shoulder view (a fixed glide to the shoulder); Over-the-Shoulder Aiming is
// now a preset of the aiming view's sliders (aim_view in jfg_options.h).
#define JFG_OLD_SHOULDER_GLIDE 0

struct CameraControl {
    bool active = false;        // the right stick owns the camera this tick
    bool was_aiming = false;
    float heading = 0;          // where the camera looks, heading in radians
    float elevation = 0;        // added to the game's own camera elevation, radians
    uint32_t player = 0;        // the character's object this tick, for jfg_move_scale
    float move_factor = 1.0f;   // ground speed multiplier this tick
    float sprint_factor = 1.0f; // the part of it that is sprinting, or the swim sprint (also speeds up the character's
                                // animation, not the jump's)
    bool shoulder = false;      // aiming with the over-the-shoulder option this tick
    float shoulder_blend = 0;   // 0..1: how far the view has moved from the game's camera to the shoulder
    bool overridden = false;    // the camera in RDRAM currently holds our values; saved[] holds the game's
    uint32_t camera_addr = 0;
    uint8_t saved[Camera::Size] = {};
    float collide_scale = 1.0f; // how much of the way from the character to the wanted camera spot is free (eases back out)
    // Arriving somewhere new (another room through a door, a respawn, a teleport): the free camera starts again
    // behind the character, looking the way it faces, instead of keeping the heading from the place it came from.
    bool face_reset = true;
    // Game frames after an arrival in which the camera stays behind the character: the game's own camera is still
    // swinging in from wherever it was (in the last room's coordinates), which is not one of its shots to wait out.
    int arrive_hold = 0;
    int level = -1;
    uint32_t last_player = 0;
    float last_x = 0, last_z = 0;
    int showing = 0;            // ticks the game's camera has been pointed away from the character (a camera "shot")
    // The free camera's view as last drawn (camera position, and the game camera's angles: yaw at +0, pitch at +2), so
    // aiming can start from it: the game's own camera stays behind the character's old facing meanwhile.
    bool view_saved = false;
    float view_x = 0, view_y = 0, view_z = 0;
    uint16_t view_yaw = 0, view_pitch = 0;
    int aim_pitch_pending = -1; // the free camera's tilt for the aim, set once the game's aiming has started (-1: none)
    // Dual-stick aiming (issue #14): while aiming, the free camera stays the view and the character aims at the point
    // under the middle of the screen. The orbit's distance and the game camera's own elevation are kept from the last
    // frame before aiming: the game's aiming camera moves in close meanwhile.
    bool free_aim = false;
    bool orbit_valid = false;
    float orbit_pivot = 0, orbit_boom = 0, orbit_elevation = 0; // orbit_pivot: the height orbited above the feet, as last placed
    bool orbit_easing = false;  // after aiming: the orbit goes back over to the game camera's (jfg_camera_override)
    // The view as last drawn, held while "Pause in the Settings Menu" stops the game (menu_paused).
    float drawn_x = 0, drawn_y = 0, drawn_z = 0;
    uint16_t drawn_yaw = 0, drawn_pitch = 0;
    bool drawn_valid = false;
    int orbit_settle = 0;       // frames left in which it waits for the game's camera to swing back out
    float glide = 0;            // 0..1: how far the view has glided to the over-the-shoulder spot while aiming
    // The height orbited eases after the character's (gently in the air: jumps, Lupus's hover, issue #16).
    float follow_look = 0;      // the height orbited, as last placed
    uint32_t follow_tick = 0;   // g_tick_count when it was placed; anything older starts again at the character
    // While aiming the point orbited has drag margins: its sideways offset from the character, and where that was.
    float orbit_dx = 0, orbit_dz = 0, orbit_dy = 0, orbit_last_x = 0, orbit_last_z = 0, orbit_last_heading = 0, orbit_last_pitch = 0;
    bool orbit_drag_valid = false;
    float aim_offset_blend = 0;     // 0..1: how far the "Aim Offset" options are applied (aiming, Over-the-Shoulder off)
    float aim_offset_wx = 0, aim_offset_wz = 0; // their sideways part, across the ground, as last placed
};
// The free camera's orbit: round the middle of the character's body, this far above its feet, from looking up at it
// from below (-70 degrees) to looking down on it (80 degrees). The camera's pitch is positive looking down.
constexpr float ORBIT_CENTRE = 40.0f, ORBIT_PITCH_MIN = -1.22f, ORBIT_PITCH_MAX = 1.40f;
constexpr float ORBIT_DISTANCE = 160.0f; // the game's own follow distance on foot (161 in the first level), times "Camera Distance"
void camera_hand_over_to_aim(uint32_t cam); // jfg_camera.cpp
// Dual-stick aiming: the character aims at the first surface on the view's centre line (from o along the unit vector
// f), or the point on it at its weapon's range (jfg_controls.cpp).
void jfg_aim_along_view(recomp_context* ctx, uint32_t obj, float ox, float oy, float oz, float fx, float fy, float fz);
extern CameraControl g_cam;
// Sticks as last read by the input thread (jfg_dual_stick), up/right positive.
extern std::atomic<float> g_stick_lx, g_stick_ly, g_stick_rx, g_stick_ry;
extern float g_mouse_cam_x, g_mouse_cam_y; // mouse this tick, in radians of camera turn
extern float g_sign_move, g_sign_cam;      // handedness of the stick and the camera
extern uint32_t g_tick_count;              // jfg_camera_controls calls
bool boss_control_active();
bool control_mods_blocked();               // cutscene or Floyd mission: the control and camera options are off
void jfg_camera_controls(recomp_context* ctx); // game thread, right after joyRead (jfg_after_joy)

// Aiming. The character control data (object + 0x68) holds, as 16-bit angles: the direction the body faces (+0x11C,
// copy +0x106, and the object's yaw) and the view pitch (+0x1E2, limited to 60 degrees); the reticle's offset from
// them (yaw +0x1CE, copies +0x1DC and +0x246; pitch +0x1E0); and their sums, the aim direction (yaw +0x1C6/+0x1CA;
// pitch +0x1C8/+0x1CC/+0x1D0/+0x1DE). The byte at +0x568 is the character's state: 0x0B aiming, 3 jumping (and falling).
// The fields are PlayerData in jfg_fields.h.

// --- jfg_camera.cpp ----------------------------------------------------------------------------------------------
// Fly camera (F7 / F10 > Tools): a free camera for screenshots. While it is on, the sticks / WASD and the mouse fly
// it, the player character gets no input, and (option) the world stands still. See fly_camera_tick().
struct FlyCamera {
    bool active = false;
    bool started = false;       // position and angles taken from the game's camera
    float x = 0, y = 0, z = 0;
    float heading = 0;          // as g_cam.heading: the camera looks along (sin, cos)
    float pitch = 0;            // positive looks down (the game's camera pitch)
};
extern FlyCamera g_fly;
extern std::atomic<bool> g_fly_toggle_request; // F10 > Tools > Fly Camera, F7 (UI thread)
bool fly_camera_tick(recomp_context* ctx);   // true while the fly camera owns this tick's input
void jfg_camera_restore();                   // start of the tick: the game gets its own camera back
bool menu_paused();                          // "Pause in the Settings Menu" has the game stopped (jfg_freeze_world)

// --- jfg_gameplay.cpp --------------------------------------------------------------------------------------------
void apply_damage_taken(uint32_t obj); // "Damage taken %"
void gameplay_tick();                  // game thread, start of the tick: "Unlimited continues"

// --- jfg_auto_save.cpp -------------------------------------------------------------------------------------------
extern std::chrono::steady_clock::time_point g_save_toast; // when the last auto save was made (on-screen note)
void auto_save_tick(recomp_context* ctx);                  // game thread, start of the tick (before joyRead)
void auto_save_forget_file();                              // a warp from outside gameplay: no file is in play
bool auto_save_file_in_play();                             // a file was chosen or saved (not a port warp from the title)

// --- jfg_area_progress.cpp ---------------------------------------------------------------------------------------
// "Keep Area Progress": what was done in an unfinished region is kept with each save and restored on loading.
void area_progress_arm();             // a file was chosen: its first region entry may be restored
void area_progress_disarm();          // a port warp from outside gameplay
void area_progress_forget(int slot);  // the game deletes a file (-1: all files)
void area_progress_on_save(int slot); // a save to slot is carried out this tick (before joyRead)
void area_progress_tick();            // start of the tick

// --- jfg_tribal_saves.cpp ----------------------------------------------------------------------------------------
// "Permanent Tribal Saves": rescued Tribals stay rescued after leaving their area, a game over or loading.
void tribal_saves_file_chosen();      // a file was chosen: what was saved for it counts
void tribal_saves_on_save(int slot);  // a save to slot is carried out this tick
void tribal_saves_forget(int slot);   // the game deletes a file (-1: all files)
void tribal_saves_tick();             // start of the tick: notices rescues

// Mods' planets and galaxies on the planet-select map (jfg_worldmap.cpp). Planets and galaxies by index; keys "mod:id".
int world_find(const std::string& key);    // a planet's index, -1 = none
int world_state(int planet);               // bits: 1 open, 2 entered, 4 completed; -1 = no such planet
void world_unlock(int planet);             // opens it for the file in play
void world_complete(int planet);           // marks it completed for the file in play (opens "completed" planets)
int galaxy_find(const std::string& key);   // "main" = 0, -1 = none
std::string galaxy_key(int galaxy);
int galaxy_current();                      // the galaxy the map shows, -1 = the map is closed
void galaxy_show(int galaxy);              // the map opens on it next
void worldmap_tick();                      // each tick: entered planets
void worldmap_file_chosen();               // the save file hooks, as tribal_saves_*
void worldmap_on_save(int slot);
void worldmap_forget(int slot);
void register_world_api();                 // the helpers above for mods (register_api)
void tribal_saves_region_reset();     // end of mainResetRegion: puts the region's rescues back

// --- jfg_visuals.cpp ---------------------------------------------------------------------------------------------
void apply_no_lod(recomp_context* ctx); // "Disable LOD", "Draw distant objects"
void fov_refresh();                     // before the scene is drawn
std::string depth_fog_status();         // the F9 menu's depth fog status line

// --- jfg_lighting.cpp, jfg_lighting_tool.cpp ----------------------------------------------------------------------
// Modern lighting (JFG-125). The lighting tool (F12) edits these live and saves them per level (lighting/level_N.txt)
// or as the defaults (lighting/default.txt); the JFG_* test switches set them at start. The values here are the
// built-in look the "Modern Lighting" option gives before anything is saved (nothing applies while it is off).
struct LightingSettings {
    std::atomic<int> mode{ 2 };              // 0 off, 1 the game's level lights per pixel, 2 also torches and lamps
    std::atomic<int> level_ambient{ 100 };   // % of the level's own colours, its ambient light
    std::atomic<int> level_strength{ 100 };  // % of the game's level lights (shots, flashes)
    std::atomic<int> object_strength{ 50 };  // % of the torch and lamp lights on the level (mode 2)
    std::atomic<int> object_radius{ 125 };   // % of a torch or lamp light's radius (the game's own conversion: 125)
    std::atomic<int> max_lights{ 32 };       // lights sent per frame (at most 64)
    std::atomic<bool> no_game_lighting{ true };  // skip the game's per-vertex level lighting without replacing it
    std::atomic<bool> no_object_lighting{ true };  // skip the game's per-vertex lighting of characters and objects
    std::atomic<bool> unlit_objects{ true }; // objects the game draws unlit (bridges, crates) are lit like the level
    std::atomic<bool> characters{ true };    // light characters and objects in RT64 (per pixel, with their normals)
    std::atomic<int> character_lights{ 40 }; // % the frame's lights add on characters (the game's own come from them)
    std::atomic<bool> ray_shadows{ false };  // ray-traced shadows instead of the shadow maps (when the GPU can)
    std::atomic<int> ray_light_size{ 4 };    // ... the lights' size (radius, world units) for soft edges, 0 = hard
    std::atomic<int> ray_sun_softness{ 10 }; // ... the directional light's angular radius (tenths of a degree)
    std::atomic<bool> ray_ao{ false };       // ray-traced ambient occlusion (with ray-traced shadows on)
    std::atomic<int> ray_ao_reach{ 60 };     // ... how far it looks (world units)
    std::atomic<int> ray_ao_strength{ 100 }; // ... how much it darkens (%)
    std::atomic<int> ray_sky{ 0 };           // ray-traced sky occlusion (%): darker where the sky isn't seen, 0 = off
    std::atomic<int> ray_sky_reach{ 3000 };  // ... how far a ray must get to count as seeing the sky (world units)
    std::atomic<bool> ray_gi{ false };       // ray-traced indirect light (one bounce and on, from probes) and sky light
    std::atomic<int> ray_gi_strength{ 100 }; // ... its strength (%)
    std::atomic<int> ray_gi_spacing{ 80 };   // ... how far apart its probes are (world units)
    std::atomic<int> ray_sky_light{ 0 };     // ... the sky's light (%)
    std::atomic<int> sky_r{ 140 }, sky_g{ 170 }, sky_b{ 220 }; // ... its colour
    std::atomic<int> ray_reflect{ 0 };       // ray-traced reflections on shiny surfaces (specular maps; %), 0 = off
    std::atomic<int> ray_reflect_water{ 0 }; // ... on water (%), 0 = off
    std::atomic<int> light_tint{ 0 };        // % the frame's lights on the level take each surface's own colour
    std::atomic<int> facing{ 40 };           // % the level lighting depends on a surface facing its light (0 = as the game)
    std::atomic<int> shadows{ 80 };          // % how dark shadows from the lights are
    std::atomic<int> shadow_lights{ 4 };     // how many of the nearest lights get a shadow map (0-4)
    std::atomic<int> shadow_size{ 512 };     // each shadow map face's size in texels
    std::atomic<int> glow_shadow{ 75 };      // % of the level's painted glow a full shadow takes
    std::atomic<bool> no_game_shadows{ true };  // drop the game's blob shadows under characters
    std::atomic<bool> sun{ false };          // the directional light
    std::atomic<int> sun_yaw{ 45 };          // its direction: degrees around (0 = towards +z)
    std::atomic<int> sun_pitch{ 50 };        // ... and up (90 = straight down from above)
    std::atomic<int> sun_r{ 255 }, sun_g{ 244 }, sun_b{ 214 }; // its colour
    std::atomic<int> sun_intensity{ 35 };    // % of full white
    std::atomic<int> sun_characters{ 100 };  // % of it on characters and objects
    std::atomic<bool> sun_shadows{ true };   // it casts shadows (around where the camera looks)
    std::atomic<int> sun_shadow_area{ 1000 }; // half the size of the square it shadows, world units
    std::atomic<int> detail{ 10 };           // surface detail from the textures: tenths of a world unit of height
    std::atomic<int> detail_characters{ 0 }; // % of it on characters and objects (their textures are mostly painted features)
    std::atomic<int> shine{ 20 };            // % shine (specular highlights)
    std::atomic<int> sharpness{ 24 };        // how tight the highlights are
    std::atomic<bool> smooth_surfaces{ true }; // the level's smooth surface directions (else one per triangle)
    std::atomic<int> shine_characters{ 20 };   // % shine on characters and objects (new character lighting)
    std::atomic<int> sharpness_characters{ 24 }; // how tight their highlights are
    std::atomic<bool> normal_maps{ true };   // authored normal maps (materials/<hash>_n.png) on
    std::atomic<int> normal_strength{ 100 }; // ... how much they count (%)
    std::atomic<bool> specular_maps{ true }; // authored specular maps (materials/<hash>_s.png) on
    std::atomic<int> specular_strength{ 100 }; // ... how much they count (%)
    std::atomic<bool> emission_maps{ true }; // emission maps (materials/<hash>_e.png) on: their glow and their lights
    std::atomic<int> off_glow{ 100 };        // % of a switched-off level light's painted glow taken away
    std::atomic<bool> steady_all{ false };   // every torch and lamp Steady (the game's flicker off), as each light's Steady
    std::atomic<int> glow{ 100 };            // % emission maps (materials/<hash>_e.png)
    std::atomic<bool> emission_lights{ true }; // lights placed where the level's emission-mapped textures are
    std::atomic<int> emission_light{ 100 };  // % their brightness
    std::atomic<int> emission_reach{ 300 };  // their reach for a 64 x 64 unit patch (world units)
    std::atomic<bool> emission_shadows{ false }; // they cast shadows
    std::atomic<bool> emission_objects{ true }; // also where objects, characters and effects with emission maps are drawn
    std::atomic<bool> debug_world{ false };  // show the world position as colour
    std::atomic<bool> debug_normal{ false }; // show the surface direction as colour
    std::atomic<bool> debug_shadow{ false }; // paint the lights' shadows red
    std::atomic<bool> log{ false };          // jfg_lighting.log
    std::atomic<int> sent_level_lights{ 0 }, sent_object_lights{ 0 }, sent_placed_lights{ 0 }; // what the last frame sent
};
LightingSettings& lighting_settings();

// A light placed with the lighting tool (saved with the level), and changes to one of the game's own torches or lamps
// (found again by where it is).
struct PlacedLight {
    float x = 0, y = 0, z = 0;
    int r = 255, g = 200, b = 140;
    int intensity = 100;  // %
    int radius = 200;     // world units
    int inner = 30;       // % of the radius at full strength
    int flicker = 0;      // % how much it dims at most
    int flicker_speed = 100; // %
    bool on = true;
    bool shadows = true;  // casts shadows
};
struct LightOverride {
    float x = 0, y = 0, z = 0;
    bool on = true;
    bool recolour = false;
    int r = 255, g = 255, b = 255;
    int intensity = 100;  // % of its own
    int radius = 100;     // % of its own
    int flicker = 0;
    int flicker_speed = 100;
    bool shadows = true;  // casts shadows
    int nth = 0;          // which of the game's lights at this spot (several can stand on one), in the game's order
    bool steady = false;  // the game's own flicker off: held at its average colour, brightness and size
};
const std::vector<PlacedLight>& lighting_placed_lights();
const std::vector<PlacedLight>& lighting_emission_lights(); // where the level's emission maps are (the lighting tool's Surfaces tab)
const LightOverride* lighting_override_for(uint32_t light); // a game light's changes (its object light); nullptr: its own look
float lighting_flicker(uint32_t seed, int amount, int speed);        // 1 = full, down to 1 - amount
int lighting_pulse_light();                    // the placed light the tool pulses to show it (-1 = none)
bool lighting_active();                        // the player's "Modern Lighting" option allows it
bool ray_tracing_active();                     // ... and the "Ray Tracing" option (every ray-traced feature)
void lighting_tool_tick(recomp_context* ctx);  // game thread, every tick
void draw_lighting_tool(recomp_context* ctx);  // game thread, in a UI frame while the tool is open

// --- jfg_weapons.cpp ---------------------------------------------------------------------------------------------
extern bool g_weapon_orig_read;            // the weapon table's original values have been read
extern std::recursive_mutex g_weapon_mutex; // UI thread edits, game thread applies
void apply_weapon_key(const char* key, int value); // "<slot>.<field>" as saved
bool set_pack_weapon(int weapon, const char* field, double value); // a pack's value (jfg.weapon)
void reset_weapons();                      // every weapon back to its original values, "Modify" off
std::string weapons_text(const char* prefix);
void save_weapons();
void apply_weapons();                      // game thread, every tick
void draw_weapons();                       // the Weapons tab (F9)
struct ToolField;
std::vector<ToolField> weapon_fields();    // the Weapons window (F10)

// --- jfg_profiles.cpp --------------------------------------------------------------------------------------------
struct BuiltinProfile { const char* name; const char* description; const char* text; };
extern const BuiltinProfile BUILTIN_PROFILES[4];
extern std::string g_active_profile;   // "" = none chosen (the settings as they are)
extern std::string g_profile_snapshot; // settings right after the active profile was applied (to show "changed")
extern std::string g_profile_message;
extern std::vector<std::string> g_user_profiles;
const BuiltinProfile* builtin_profile(const std::string& name); // nullptr if not a built-in one
std::filesystem::path user_profile_path(const std::string& name);
std::string settings_snapshot();           // all settings as profile text
void list_user_profiles();
void save_profile_choice();
void load_profile_choice();
void write_user_profile(const std::string& name);
void choose_profile(const std::string& name);
bool valid_profile_name(const std::string& name);
void profile_autosave();                   // changes made while a player profile is active are written to it
// What choosing a profile would change, shown before it is applied (issue #7: a profile's dual-stick layout silently
// took the place of the player's own bindings).
struct ProfilePreview {
    std::vector<std::string> controls; // "Name: Off to On" for each Controls-tab setting that changes, in menu order
    int other_changes = 0;             // settings on the other tabs that change
    bool weapons = false;              // the weapon settings change (to the profile's, or back to the original)
    bool replaces_bindings = false;    // afterwards the built-in dual-stick layout takes the place of the controller bindings
};
extern bool g_profile_keep_controls;       // Keep My Controls (saved in jfg_profile.txt)
void set_profile_keep_controls(bool keep);
ProfilePreview preview_profile(const std::string& name);
std::string profile_preview_text(const std::string& name); // as lines for a prompt; "" when nothing would change
void draw_profiles();                      // the Profiles tab
void debug_profile_tick();                 // JFG_DEBUG_PROFILE

// --- jfg_ui_window.cpp -------------------------------------------------------------------------------------------
void draw_cheats();
void draw_warp();
void draw_debug_window(recomp_context* ctx); // F9 (Dear ImGui)

// --- what F10's windows show and edit (jfg_ui_tools.cpp draws them) ----------------------------------------------
// One row of a window. get / set / text run on the game thread (jfg_debug_frame), so they may read and write the game
// and call its functions. Toggle: get returns 0 / 1. Number: lo..hi, changed by step / big_step. Action: a button,
// set(ctx, 1). Heading and Note: text only (text, when given, replaces the label with what it returns each refresh).
struct ToolField {
    enum Kind { Heading, Note, Toggle, Number, Action } kind = Note;
    std::string label;
    int lo = 0, hi = 1, step = 1, big_step = 10;
    std::function<int(recomp_context*)> get;
    std::function<void(recomp_context*, int)> set;
    std::function<std::string(recomp_context*)> text;
};

// --- jfg_assets.cpp ---------------------------------------------------------------------------------------------
int asset_number(const std::string& ref); // "mod_id:name" of an appended entry -> its number this start, or -1
int baddy_like(int object_id);             // a mod's object type -> the stock object id squads treat it as, or -1

// --- jfg_music.cpp ----------------------------------------------------------------------------------------------
void music_tick(recomp_context* ctx); // every tick: the level's mod music, the game's tune silenced meanwhile
void music_force(const std::string& mod_id, const std::string& name); // a script's track for this level ("" = none)

// --- jfg_lua.cpp ------------------------------------------------------------------------------------------------
void lua_tick(recomp_context* ctx);                     // every tick: mods' scripts ("level", "tick" events)
bool lua_overlay_text(std::string& text, float& alpha); // UI thread: a script's on-screen message, if one shows

// --- jfg_ui_items.cpp --------------------------------------------------------------------------------------------
void draw_inventory(recomp_context* ctx);  // the Items tab (F9)
std::vector<ToolField> item_fields();      // the Items window (F10), for the character in g.edit_character

// --- jfg_ui_tools.cpp --------------------------------------------------------------------------------------------
// F10: the tools bar and its windows (RmlUi). Height of the bar in pixels while it is shown (0 otherwise): the corner
// overlays start below it.
extern std::atomic<float> g_toolbar_height;
void tools_game_tick(recomp_context* ctx); // game thread, every tick: carries out what the bar asked for, refreshes values

} // namespace jfg
