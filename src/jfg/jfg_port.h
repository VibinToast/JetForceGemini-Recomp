// Jet Force Gemini port: what the port's files in src/jfg share with each other, by the file that defines it.
// (Game access is in jfg_game.h, the options in jfg_options.h, the functions the rest of the program calls in
// jfg_hooks.h.)
#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
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
std::string pack_display_name(const std::string& folder);
void set_texture_pack(const std::string& folder); // saves the choice and makes the renderer (re)load it
void load_texture_pack_choice();
void next_texture_pack();                         // F6
void update_dump_folder();                        // texture dumping, "One folder per level", and the scene tag of draw_trace.txt

// --- jfg_frame_rate.cpp ------------------------------------------------------------------------------------------
extern std::atomic<float> g_fps;        // game frames per second, for the "Show FPS" option
extern std::atomic<float> g_screen_fps; // frames the renderer presented per second

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
};
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
// pitch +0x1C8/+0x1CC/+0x1D0/+0x1DE). The byte at +0x568 is 0x0B while the character is in its aiming state.
constexpr uint32_t PD_BODY_YAW = 0x11C, PD_BODY_YAW_COPY = 0x106, PD_VIEW_PITCH = 0x1E2, PD_RETICLE_YAW = 0x1CE, PD_RETICLE_PITCH = 0x1E0, PD_STATE = 0x568;
constexpr uint8_t PD_STATE_AIMING = 0x0B;

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

// --- jfg_area_progress.cpp ---------------------------------------------------------------------------------------
// "Keep Area Progress": what was done in an unfinished region is kept with each save and restored on loading.
void area_progress_arm();             // a file was chosen: its first region entry may be restored
void area_progress_disarm();          // a port warp from outside gameplay
void area_progress_forget(int slot);  // the game deletes a file (-1: all files)
void area_progress_on_save(int slot); // a save to slot is carried out this tick (before joyRead)
void area_progress_tick();            // start of the tick

// --- jfg_visuals.cpp ---------------------------------------------------------------------------------------------
void apply_no_lod(recomp_context* ctx); // "Disable LOD", "Draw distant objects"
void fov_refresh();                     // before the scene is drawn
std::string depth_fog_status();         // the F9 menu's depth fog status line

// --- jfg_weapons.cpp ---------------------------------------------------------------------------------------------
extern bool g_weapon_orig_read;            // the weapon table's original values have been read
extern std::recursive_mutex g_weapon_mutex; // UI thread edits, game thread applies
void apply_weapon_key(const char* key, int value); // "<slot>.<field>" as saved
void reset_weapons();                      // every weapon back to its original values, "Modify" off
std::string weapons_text(const char* prefix);
void save_weapons();
void apply_weapons();                      // game thread, every tick
void draw_weapons();                       // the Weapons tab / window

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
void draw_profiles();                      // the Profiles tab
void debug_profile_tick();                 // JFG_DEBUG_PROFILE

// --- jfg_ui_window.cpp -------------------------------------------------------------------------------------------
void draw_cheats();
void draw_warp();
void draw_debug_window(recomp_context* ctx); // F9

// --- jfg_ui_items.cpp --------------------------------------------------------------------------------------------
void draw_inventory(recomp_context* ctx);  // the Items tab / window

// --- jfg_ui_toolbar.cpp ------------------------------------------------------------------------------------------
// Height of F10's toolbar while it is open (0 otherwise), measured when drawn: the corner overlays start below it.
extern float g_toolbar_height;
void draw_toolbar(recomp_context* ctx);    // F10

} // namespace jfg
