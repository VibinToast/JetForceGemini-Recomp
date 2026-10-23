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

#include "jfg_game.h"

namespace jfg {

// --- jfg_tick.cpp ------------------------------------------------------------------------------------------------
extern bool g_story_skip_pending; // START in the story intro: go to character select at the next tick
extern bool g_reset_pending, g_reset_press_start; // F5, see jfg_debug_frame

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
    uint8_t saved[CAMERA_SIZE] = {};
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
};
void camera_hand_over_to_aim(uint32_t cam); // jfg_camera.cpp
extern CameraControl g_cam;
// Sticks as last read by the input thread (jfg_dual_stick), up/right positive.
extern std::atomic<float> g_stick_lx, g_stick_ly, g_stick_rx, g_stick_ry;
extern float g_mouse_cam_x, g_mouse_cam_y; // mouse this tick, in radians of camera turn
extern float g_sign_move, g_sign_cam;      // handedness of the stick and the camera
extern uint32_t g_tick_count;              // jfg_camera_controls calls
extern int g_boss_cam_turn;                // boss fights: the camera turned on the spot (16-bit angle units)
bool boss_control_active();
bool control_mods_blocked();               // cutscene or Floyd mission: the control and camera options are off
void jfg_camera_controls(recomp_context* ctx); // game thread, right after joyRead (jfg_after_joy)

// Aiming. The character control data (object + 0x68) holds, as 16-bit angles: the direction the body faces (+0x11C,
// copy +0x106, and the object's yaw) and the view pitch (+0x1E2, limited to 60 degrees); the reticle's offset from
// them (yaw +0x1CE, copies +0x1DC and +0x246; pitch +0x1E0); and their sums, the aim direction (yaw +0x1C6/+0x1CA;
// pitch +0x1C8/+0x1CC/+0x1D0/+0x1DE). The byte at +0x568 is the character's state: 0x0B aiming, 3 jumping (and falling).
constexpr uint32_t PD_BODY_YAW = 0x11C, PD_BODY_YAW_COPY = 0x106, PD_VIEW_PITCH = 0x1E2, PD_RETICLE_YAW = 0x1CE, PD_RETICLE_PITCH = 0x1E0, PD_STATE = 0x568;
constexpr uint8_t PD_STATE_AIMING = 0x0B, PD_STATE_JUMP = 3;

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
void tribal_saves_region_reset();     // end of mainResetRegion: puts the region's rescues back

// --- jfg_visuals.cpp ---------------------------------------------------------------------------------------------
void apply_no_lod(recomp_context* ctx); // "Disable LOD", "Draw distant objects"
void fov_refresh();                     // before the scene is drawn
std::string depth_fog_status();         // the F9 menu's depth fog status line

// --- jfg_lighting.cpp --------------------------------------------------------------------------------------------
// Modern lighting, in progress (JFG-125): the F9 Lighting tab edits these live; they start from the JFG_* test switches.
struct LightingSettings {
    std::atomic<int> mode{ 0 };              // 0 off, 1 the game's level lights per pixel, 2 also torches and lamps
    std::atomic<int> level_ambient{ 100 };   // % of the level's own colours, its ambient light (later per level)
    std::atomic<int> level_strength{ 100 };  // % of the game's level lights (shots, flashes)
    std::atomic<int> object_strength{ 35 };  // % of the torch and lamp lights on the level (mode 2)
    std::atomic<int> object_radius{ 125 };   // % of a torch or lamp light's radius (the game's own conversion: 125)
    std::atomic<int> max_lights{ 32 };       // lights sent per frame (RT64 takes at most 32)
    std::atomic<bool> no_game_lighting{ false }; // skip the game's per-vertex level lighting without replacing it
    std::atomic<bool> no_object_lighting{ false }; // skip the game's per-vertex lighting of characters and objects
    std::atomic<bool> characters{ false };   // light characters and objects in RT64 (per pixel, with their normals)
    std::atomic<int> character_lights{ 0 };  // % the frame's lights add on characters (the game's own come from them)
    std::atomic<int> facing{ 0 };            // % the level lighting depends on a surface facing its light (0 = as the game)
    std::atomic<int> shadows{ 0 };           // % how dark characters' and objects' shadows from these lights are
    std::atomic<int> caster_size{ 100 };     // % of the size worked out from each one's own shadow
    std::atomic<bool> no_game_shadows{ false }; // drop the game's blob shadows under characters (still casters here)
    std::atomic<bool> debug_world{ false };  // show the world position as colour
    std::atomic<bool> debug_normal{ false }; // show the surface direction as colour
    std::atomic<bool> log{ false };          // jfg_lighting.log
    std::atomic<int> sent_level_lights{ 0 }, sent_object_lights{ 0 }; // what the last frame sent
};
LightingSettings& lighting_settings();
// A shadow the game draws this frame (func_8001FA24): its object, where it is (x, z), and the shadow image's scale
// (image size over shadow size), from which the caster's size is worked out.
void lighting_add_caster(uint32_t object, float x, float z, float scale);

// --- jfg_weapons.cpp ---------------------------------------------------------------------------------------------
extern bool g_weapon_orig_read;            // the weapon table's original values have been read
extern std::recursive_mutex g_weapon_mutex; // UI thread edits, game thread applies
void apply_weapon_key(const char* key, int value); // "<slot>.<field>" as saved
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

// --- jfg_ui_items.cpp --------------------------------------------------------------------------------------------
void draw_inventory(recomp_context* ctx);  // the Items tab (F9)
std::vector<ToolField> item_fields();      // the Items window (F10), for the character in g.edit_character

// --- jfg_ui_tools.cpp --------------------------------------------------------------------------------------------
// F10: the tools bar and its windows (RmlUi). Height of the bar in pixels while it is shown (0 otherwise): the corner
// overlays start below it.
extern std::atomic<float> g_toolbar_height;
void tools_game_tick(recomp_context* ctx); // game thread, every tick: carries out what the bar asked for, refreshes values

} // namespace jfg
