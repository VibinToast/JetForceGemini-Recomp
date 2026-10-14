// Jet Force Gemini port: native debug menu (Dear ImGui).
//
// Opened and closed with F9 or the ` (grave) key. Drawn through the base project's debug UI backend
// (src/debug_ui), which already owns an ImGui context and renders it on top of the game.
//
// jfg_debug_frame() is called once per game tick on the game's main thread, from a hook at the entry of the main
// loop tick func_80044FAC_45BAC (see [[patches.hook]] in jfg.us.toml). Everything here works by reading and writing
// the game's own variables in emulated RDRAM, or by calling the game's own (recompiled) functions - no game code
// is replaced. Addresses are the US 1.0 ones from ver/symbols/symbol_addrs.us.txt of the decomp.
//
// What each tool relies on:
//   cheats        the game's own retail cheat flags: invulnerabilityCheat, DebugAmmoFlag, allweaponsCheat,
//                 allLevelCheat, debugMenuEnable (all u8 in menu.data / main.data)
//   level warp    mainChangeLevel(level, character, 0, entrance, 1, 0), names from ROM asset sections 0x1E/0x1F
//                 exactly as levelGetName() reads them
//   weapons/ammo  game character struct = gameplay + 0x15C + character * 0x76 (mainGetGameCharacter):
//                 u16 weapon bitmask at +0x0A (mainPlayerHasWeapon), u16 ammo[16] at +0x14, current weapon at +0x70
//   collected     bit array at character + 0x66, bit N = byte N/8, mask 0x80 >> (N & 7) (mainPlayerCollected)
//   game flags    mainGetGameFlag / mainSetGameFlag with the GameFlags numbers from include/enums.h
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>
#include <cctype>
#include <chrono>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <mutex>
#include <iterator>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <span>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "recomp.h"
#include "ultramodern/error_handling.hpp"
#include "imgui/imgui.h"
#include "debug_ui/debug_ui.hpp"
#include "debug_ui/backend.hpp"

namespace recomp { std::span<const uint8_t> get_rom(); }
extern std::atomic<uint32_t> rt64_jfg_present_count; // lib/rt64/src/hle/rt64_present_queue.cpp (JFG patch)
extern std::atomic<int> rt64_jfg_ssao;            // lib/rt64/src/render/rt64_framebuffer_renderer.cpp (JFG patch)
extern std::atomic<float> rt64_jfg_ssao_strength;
extern std::atomic<float> rt64_jfg_ssao_radius;
extern std::atomic<int> rt64_jfg_bloom;
extern std::atomic<float> rt64_jfg_bloom_intensity;
extern std::atomic<float> rt64_jfg_bloom_threshold;
extern std::atomic<float> rt64_jfg_bloom_radius;

extern "C" {
// Recompiled game functions (RecompiledFuncs/*.c).
void mainChangeLevel(uint8_t* rdram, recomp_context* ctx);
void mainTitlePageInit(uint8_t* rdram, recomp_context* ctx);
void mmAlloc(uint8_t* rdram, recomp_context* ctx);
void mmFlushFreeStack(uint8_t* rdram, recomp_context* ctx);
void mathBreakPoint(uint8_t* rdram, recomp_context* ctx);
void mainGetGameFlag(uint8_t* rdram, recomp_context* ctx);
void mainSetGameFlag(uint8_t* rdram, recomp_context* ctx);
void frontGetMode(uint8_t* rdram, recomp_context* ctx);
void objGetPlayerlist(uint8_t* rdram, recomp_context* ctx);
void objGetObjList(uint8_t* rdram, recomp_context* ctx);
void controlSelectWeapon(uint8_t* rdram, recomp_context* ctx);
void objMoveXYZ(uint8_t* rdram, recomp_context* ctx);
void fb_swap(uint8_t* rdram, recomp_context* ctx);
void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx);
void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
}

namespace {

// --- game addresses (US) ---------------------------------------------------------------------------------------
constexpr uint32_t ADDR_gameplay             = 0x800FD7D4; // Game*
constexpr uint32_t ADDR_mainGameMode         = 0x800FD7C4; // s32
constexpr uint32_t ADDR_currentLevel         = 0x800A323C; // s16, mainGetCurrentLevel()
constexpr uint32_t ADDR_currentCharacter     = 0x800A3240; // s16, mainGetPlayerCharacter() in single player
constexpr uint32_t ADDR_nextLevel            = 0x800A3250; // s16, mainGetNextLevel()
constexpr uint32_t ADDR_allLevelCheat        = 0x800A5068; // u8
constexpr uint32_t ADDR_invulnerabilityCheat = 0x800A5070; // u8
constexpr uint32_t ADDR_DebugAmmoFlag        = 0x800A5074; // u8
constexpr uint32_t ADDR_allweaponsCheat      = 0x800A5078; // u8
constexpr uint32_t ADDR_debugMenuEnable      = 0x800A32D0; // s32
constexpr uint32_t ADDR_gSoundPlayerPtr = 0x800A95AC; // sound-effect player; its event queue is at +0x14
constexpr uint32_t ADDR_tuneSeqPlayer = 0x800A0660, ADDR_tuneSeqNo = 0x800E94F4; // music player (state at +0x2C, 1 = playing), current tune (u8)
constexpr uint32_t ADDR_tribaltable = 0x800FD7B8; // pointer to s8[]: tribals per region
constexpr uint32_t ADDR_cloneTaskActive = 0x800A35E8, ADDR_refractTaskActive = 0x800A360C, ADDR_blurTaskActive = 0x800A3630; // s32
constexpr uint32_t ADDR_sResolutionIndex     = 0x800FECA8; // s8, bit 0 = letterboxed ("widescreen") VI mode
constexpr uint32_t ADDR_pauseMode = 0x800FD7BD;          // s8, what mainGetPauseMode() returns
constexpr uint32_t ADDR_runInOneFrame        = 0x800FECCC; // s32, viFrameSync waits for one retrace only
constexpr uint32_t ADDR_buttonsPressed       = 0x800FB0F0; // u16 gControllerButtonsPressed[4]
constexpr uint32_t ADDR_sPlayerID            = 0x800FB100; // u8 sPlayerID[4]
constexpr uint16_t START_BUTTON              = 0x1000;
constexpr uint32_t ADDR_otherScreen          = 0x800FECB8; // s32*, the buffer viFrameSync hands to osViSwapBuffer

constexpr uint32_t GAME_CHARACTERS_OFFSET = 0x15C;
constexpr uint32_t GAME_CHARACTER_SIZE    = 0x76;
constexpr uint32_t GAME_REGION_STATS_OFFSET = 0x334; // 8 bytes per region: u8 best rescued, u8 killed, u16, u32
constexpr uint32_t ADDR_levelHeader       = 0x800FB118; // LevelHeader* (levelGetLevel); +0x102 region, +0x103 room
constexpr uint32_t CHAR_HEALTH_OFFSET     = 0x00; // s16, 256 per health unit
constexpr uint32_t CHAR_HEALTH_LEVEL_OFFSET = 0x02; // s16, 6..12
constexpr uint32_t CHAR_WEAPONS_OFFSET    = 0x0A; // u16 bitmask
constexpr uint32_t CHAR_AMMO_OFFSET       = 0x14; // u16[16]
constexpr uint32_t CHAR_COLLECTED_OFFSET  = 0x66; // 9 bytes of bits (0x66..0x6E)
constexpr uint32_t CHAR_WEAPON_COUNT_OFFSET = 0x6F; // u8 number of weapons owned; weapon switching needs >= 2
constexpr uint32_t CHAR_WEAPON_OFFSET     = 0x70; // u8 current weapon
constexpr int      COLLECTED_BITS         = 72;

// ROM asset table (src/pi.c): count word at 0xB1750 followed by section offsets, data from 0xB1880.
constexpr uint32_t ROM_ASSET_TABLE = 0xB1750;
constexpr uint32_t ROM_ASSET_DATA  = 0xB1880;

struct NamedFlag { int id; const char* name; };
// include/enums.h (GameFlags; list credited there to Jimmie1717, marked unverified).
const NamedFlag ship_part_flags[] = {
    {32, "Power cell"}, {33, "Radar dish"}, {34, "Fin"}, {35, "Cargo bay key"}, {36, "Deflector shield"},
    {37, "Fuse"}, {38, "Vela's hatch key"}, {39, "Juno's hatch key"}, {40, "Lupus's hatch key"},
    {41, "Nitrogen tank"}, {42, "Oxygen tank"}, {43, "Stabilizer"},
};
const NamedFlag robot_flags[] = { {44, "Floyd part: top"}, {45, "Floyd part: right"}, {46, "Floyd part: left"} };
const NamedFlag progress_flags[] = {
    {0, "Vela (character flag)"}, {1, "Juno (character flag)"}, {2, "Lupus (character flag)"}, {3, "Floyd"},
    {20, "Intro seen: Fet Bubb"}, {21, "Intro seen: Lurg"}, {22, "Intro seen: Mechantids"}, {23, "Intro seen: tutorial"},
    {77, "Defeated Fet Bubb"}, {78, "Defeated Lurg"}, {79, "Defeated Mechantids"},
    {88, "All tribals rescued"}, {89, "Quarry generator activated"},
    {90, "Disable Vela select"}, {91, "Disable Juno select"}, {92, "Disable Lupus select"},
    {94, "Pyramid activated"}, {95, "Jetpack"}, {101, "Machine gun lock (Goldwood)"},
};
const char* const character_names[] = { "Vela", "Juno", "Lupus" };

// Per-character pickups (mainPlayerCollected / mainPlayerHas). Keys: the key object's colour number is the pickup
// number, in the order of the game's "... Key Required" messages. Objects: the numbers the pause menu's inventory
// page tests (overlay 18, inventoryScreen), named after the icon it draws for each (checked by setting every bit
// and looking at that page; keys 0, 1, 2, 3 and 9 are the five it shows).
const NamedFlag key_items[] = {
    {0, "Yellow key"}, {1, "Red key"}, {2, "Magenta key"}, {3, "Green key"}, {4, "Cyan (light blue) key"}, {5, "Lime key"},
    {6, "Purple key"}, {7, "Tango (orange) key"}, {8, "Lemon key"}, {9, "Blue key"},
    {10, "Vela Tri-Rocket Check"},
};
const NamedFlag object_items[] = {
    {21, "Crowbar"}, {26, "Ear plugs"}, {20, "Pants"}, {16, "Magazine"},
    {23, "Gold bar 1 of 3"}, {24, "Gold bar 2 of 3"}, {25, "Gold bar 3 of 3"},
    {17, "Mine key"}, {27, "Arcade chip (racing)"}, {22, "Night vision goggles"},
};

// Regions (byte 0x102 of the level header, 1..30) in the game's order; names are the game's own (text 408-437) with
// the planet in front.
const char* const region_names[31] = {
    "(none)",
    "Goldwood - Outset", "Goldwood - Lodge", "S.S. Anubis - Passageway", "Goldwood - Interior", "Goldwood - Rim",
    "Rith Essa - Bluff", "S.S. Anubis - Hold", "Sekhmet - Battle Cruiser", "Walkway - Peak", "S.S. Anubis - Depository",
    "Spawnship - Troop Carrier", "Rith Essa - Interior", "Cerulean - Dune", "Tawfret - Bog", "Tawfret - Bridge",
    "Tawfret - Castle", "Rith Essa - Ascent", "Ichor - Military Base", "Water Ruin - Lost Island", "Rith Essa - Mine",
    "Ichor - Perimeter", "Eschebone - Approach", "Eschebone - Thorax", "Eschebone - Cortex", "Gem Quarry - Landing",
    "Spacestation - Abandoned Wreck", "Mizar's Palace - Lobby", "Mizar's Palace - Flume", "Mizar's Palace - Chasm",
    "Asteroid - Mizar's Lair",
};
// Slot order is the game's weapon index (bit in the owned mask). The names are the game's own, in the order of its
// text table (ROM 0xC36F10: "Pistol", "Homing Missiles", ...); the order was checked against the HUD icon, the ammo
// count and the reticle of each slot.
const char* const weapon_names[16] = {
    "Pistol", "Homing Missiles", "Machine Gun", "Plasma Shotgun", "Shocker", "Tri-Rocket Launcher", "Flamethrower", "Sniper Rifle",
    "Grenades", "Shuriken", "Fish Food", "Timed Mines", "Remote Mines", "Flares", "Cluster Bombs", "(unused)",
};

// --- RDRAM access (words are stored host-endian, so bytes and halves need the usual address swizzle) --------------
uint8_t* g_rdram = nullptr;
#define rdram g_rdram // the MEM_* macros of recomp.h index a variable called rdram
inline uint32_t rd32(uint32_t a) { return (uint32_t)MEM_W(0, (gpr)(int32_t)a); }
inline uint16_t rd16(uint32_t a) { return (uint16_t)MEM_HU(0, (gpr)(int32_t)a); }
inline uint8_t  rd8(uint32_t a)  { return (uint8_t)MEM_BU(0, (gpr)(int32_t)a); }
inline void wr32(uint32_t a, uint32_t v) { MEM_W(0, (gpr)(int32_t)a) = (int32_t)v; }
inline void wr16(uint32_t a, uint16_t v) { MEM_H(0, (gpr)(int32_t)a) = (int16_t)v; }
inline void wr8(uint32_t a, uint8_t v)   { MEM_B(0, (gpr)(int32_t)a) = (int8_t)v; }
inline void wr32_at(gpr base, int offset, uint32_t v) { MEM_W(offset, base) = (int32_t)v; }
#undef rdram

inline bool valid_ptr(uint32_t p) { return p >= 0x80000400u && p < 0x80800000u; }

// Calls a recompiled game function with up to six integer arguments on the current (game) thread and returns $v0.
// The caller's register state is saved and restored, and the callee gets its own stack frame below the current one.
uint32_t call_game(void (*func)(uint8_t*, recomp_context*), recomp_context* ctx, std::initializer_list<uint32_t> args) {
    recomp_context saved = *ctx;
    ctx->r29 = (gpr)(int32_t)(((uint32_t)ctx->r29 - 0x100u) & ~0xFu);
    gpr* reg[4] = { &ctx->r4, &ctx->r5, &ctx->r6, &ctx->r7 };
    int i = 0;
    for (uint32_t a : args) {
        if (i < 4) *reg[i] = (gpr)(int32_t)a;
        else wr32_at(ctx->r29, i * 4, a); // o32: arguments 5+ live at 0x10($sp), 0x14($sp), ...
        i++;
    }
    func(g_rdram, ctx);
    const uint32_t ret = (uint32_t)ctx->r2;
    *ctx = saved;
    return ret;
}

// --- level names -------------------------------------------------------------------------------------------------
std::vector<std::string> g_level_names;

void load_level_names_once();
void load_level_names() { // called from the game thread and the UI thread
    static std::once_flag once;
    std::call_once(once, load_level_names_once);
}
void load_level_names_once() {
    const std::span<const uint8_t> rom = recomp::get_rom();
    auto be32 = [&](size_t o) -> uint32_t {
        if (o + 4 > rom.size()) return 0xFFFFFFFFu;
        return ((uint32_t)rom[o] << 24) | ((uint32_t)rom[o + 1] << 16) | ((uint32_t)rom[o + 2] << 8) | rom[o + 3];
    };
    const size_t offsets = (size_t)ROM_ASSET_DATA + be32(ROM_ASSET_TABLE + 4 * (0x1E + 1));
    const size_t headers = (size_t)ROM_ASSET_DATA + be32(ROM_ASSET_TABLE + 4 * (0x1F + 1));
    for (size_t i = 0; i < 1024; i++) {
        const uint32_t off = be32(offsets + 4 * i);
        if (off == 0xFFFFFFFFu || headers + off + 0x20 > rom.size()) break;
        char name[0x21] = {};
        std::memcpy(name, rom.data() + headers + off, 0x20);
        for (char& c : name) if (c != 0 && (c < 0x20 || c > 0x7E)) c = '?';
        g_level_names.emplace_back(name);
    }
    if (g_level_names.empty()) g_level_names.emplace_back("(level table not found)");
}

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
    bool start_skips_cutscenes = true;
    bool dual_stick = false;       // modern twin-stick layout, see jfg_dual_stick()
    bool invert_aim_y = false;     // dual stick: right stick up aims down (the game's original behaviour)
    int  move_speed = 100;         // how fast the character moves over the ground, percent (any layout)
    bool diagonal_boost = false;   // dual stick: faster when the left stick is pushed diagonally, as running + sidestepping was
    int  strafe_speed = 100;       // dual stick: weight of the sideways part in that boost, percent
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
} g;

// Fly camera (F7 / F10 > Tools): a free camera for screenshots. While it is on, the sticks / WASD and the mouse fly
// it, the player character gets no input, and (option) the world stands still. See fly_camera_tick().
struct FlyCamera {
    bool active = false;
    bool started = false;       // position and angles taken from the game's camera
    float x = 0, y = 0, z = 0;
    float heading = 0;          // as g_cam.heading: the camera looks along (sin, cos)
    float pitch = 0;            // positive looks down (the game's camera pitch)
} g_fly;
std::atomic<bool> g_fly_toggle_request{ false }; // F10 > Tools > Fly Camera (UI thread)

// A cheat checkbox drives one of the game's own u8 flags: forced to 1 every tick while ticked, cleared once on untick.
// The story intro after creating a file (levels 322-327) and where START takes it to (jfg_after_joy).
constexpr uint32_t ADDR_waitMidiSync = 0x800A326C, ADDR_midiSyncReached = 0x800E9774; // see the story skip in jfg_debug_frame
constexpr int LEVEL_STORY_FIRST = 322, LEVEL_STORY_LAST = 327, LEVEL_CHARACTER_SELECT = 127;
bool g_story_skip_pending = false;
bool g_reset_pending = false, g_reset_press_start = false; // F5, see jfg_debug_frame
// The "Frame rate" option (60 / interpolated) is hidden and off: on the user's PC it made no visible difference
// (sessions 35-36 in PROGRESS.md). Set to true to bring it back.
constexpr bool FRAME_RATE_OPTION = true;
std::atomic<float> g_fps{ 0.0f }; // game frames per second, for the "Show FPS" option
std::atomic<float> g_screen_fps{ 0.0f };
std::atomic<int> g_texture_pack_count{ -1 }; // textures listed in bin/texture_pack/rt64.json, -1 = pack not in use
// Texture packs: folders in bin/texture_packs/<name> (and the older single bin/texture_pack). The choice is kept in
// bin/jfg_texture_pack.txt; the renderer picks it up through jfg_texture_pack_current().
std::mutex g_pack_mutex;
std::string g_pack_folder;          // relative to the app folder, "" = none
std::atomic<int> g_pack_generation{ 0 };
std::chrono::steady_clock::time_point g_pack_toast; // when F6 last switched packs (on-screen note)
constexpr const char* PACK_CHOICE_FILE = "jfg_texture_pack.txt";

void apply_flag(bool want, bool& applied, uint32_t addr) {
    if (want) { wr8(addr, 1); applied = true; }
    else if (applied) { wr8(addr, 0); applied = false; }
}

uint32_t character_ptr(int character) {
    const uint32_t game = rd32(ADDR_gameplay);
    if (!valid_ptr(game)) return 0;
    return game + GAME_CHARACTERS_OFFSET + (uint32_t)(character & 3) * GAME_CHARACTER_SIZE;
}

// Puts a weapon in the player's hand through the game's own controlSelectWeapon(playerObject, playerData, slot),
// which only accepts weapons the character owns - so the slot is added to the owned mask first.
// The first player's object (objGetPlayerlist), or 0.
uint32_t player_object(recomp_context* ctx) {
    const uint32_t scratch = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
    wr32(scratch, 0);
    const uint32_t list = call_game(objGetPlayerlist, ctx, { scratch });
    if (!valid_ptr(list) || (int32_t)rd32(scratch) < 1) return 0;
    const uint32_t obj = rd32(list);
    return valid_ptr(obj) ? obj : 0;
}

bool select_weapon(recomp_context* ctx, int slot) {
    const uint32_t ch = character_ptr((int16_t)rd16(ADDR_currentCharacter));
    if (ch == 0 || slot < 0 || slot > 15) return false;
    const uint32_t obj = player_object(ctx);
    if (obj == 0) return false;
    const uint32_t data = rd32(obj + 0x68);
    if (!valid_ptr(data)) return false;
    wr16(ch + CHAR_WEAPONS_OFFSET, (uint16_t)(rd16(ch + CHAR_WEAPONS_OFFSET) | (1u << slot)));
    if (rd16(ch + CHAR_AMMO_OFFSET + 2 * slot) == 0) wr16(ch + CHAR_AMMO_OFFSET + 2 * slot, 50);
    return call_game(controlSelectWeapon, ctx, { obj, data, (uint32_t)slot }) != 0;
}

bool contains_ci(const std::string& s, const char* needle) {
    if (needle[0] == 0) return true;
    auto it = std::search(s.begin(), s.end(), needle, needle + std::strlen(needle),
        [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); });
    return it != s.end();
}

void flag_checkboxes(recomp_context* ctx, const NamedFlag* flags, size_t count) {
    for (size_t i = 0; i < count; i++) {
        bool on = call_game(mainGetGameFlag, ctx, { (uint32_t)flags[i].id }) != 0;
        char label[64];
        std::snprintf(label, sizeof(label), "%s (%d)", flags[i].name, flags[i].id);
        if (ImGui::Checkbox(label, &on)) {
            call_game(mainSetGameFlag, ctx, { (uint32_t)flags[i].id, on ? 1u : 0u });
        }
    }
}

void draw_status() {
    const int level = (int16_t)rd16(ADDR_currentLevel);
    const int next = (int16_t)rd16(ADDR_nextLevel);
    const int character = (int16_t)rd16(ADDR_currentCharacter);
    const char* name = (level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level].c_str() : "?";
    ImGui::Text("Level %d: %s", level, name);
    ImGui::Text("Next level %d   game mode %d   character %d (%s)", next, (int)rd32(ADDR_mainGameMode), character,
        (character >= 0 && character < 3) ? character_names[character] : "?");
}

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

// Options persist in a small text file next to the working directory the game is started from.
const char* const OPTIONS_FILE = "jfg_port_options.txt";
bool g_options_loaded = false;

// Hands the renderer options to RT64 (read by its render thread every frame).
void push_render_options() {
    rt64_jfg_ssao_strength.store(g.ssao_strength / 100.0f, std::memory_order_relaxed);
    rt64_jfg_ssao_radius.store(float(g.ssao_radius), std::memory_order_relaxed);
    rt64_jfg_ssao.store(g.ssao ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_bloom_intensity.store(g.bloom_intensity / 100.0f, std::memory_order_relaxed);
    rt64_jfg_bloom_threshold.store(g.bloom_threshold / 100.0f, std::memory_order_relaxed);
    rt64_jfg_bloom_radius.store(float(g.bloom_radius), std::memory_order_relaxed);
    rt64_jfg_bloom.store(g.bloom ? 1 : 0, std::memory_order_relaxed);
}

// Sets one option from its saved form (the options file, profiles).
void apply_option(const char* key, int value) {
    if (std::strcmp(key, "no_blood") == 0) g.no_blood = value != 0;
    else if (std::strcmp(key, "show_fps") == 0) g.show_fps = value != 0;
    else if (std::strcmp(key, "show_level") == 0) g.show_level = value != 0;
    else if (std::strcmp(key, "show_pack") == 0) g.show_pack = value != 0;
    else if (std::strcmp(key, "dump_by_level") == 0) g.dump_by_level = value != 0;
    else if (std::strcmp(key, "no_lod") == 0) g.no_lod = value != 0;
    else if (std::strcmp(key, "far_objects") == 0) g.far_objects = value != 0;
    else if (std::strcmp(key, "ssao") == 0) g.ssao = value != 0;
    else if (std::strcmp(key, "ssao_strength") == 0) g.ssao_strength = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "ssao_radius") == 0) g.ssao_radius = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "bloom") == 0) g.bloom = value != 0;
    else if (std::strcmp(key, "bloom_intensity") == 0) g.bloom_intensity = std::clamp(value, 0, 300);
    else if (std::strcmp(key, "bloom_threshold") == 0) g.bloom_threshold = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "bloom_radius") == 0) g.bloom_radius = std::clamp(value, 1, 20);
    else if (std::strcmp(key, "frame_rate") == 0) g.frame_rate = FRAME_RATE_OPTION ? std::clamp(value, 0, 2) : 0;
    else if (std::strcmp(key, "widescreen") == 0) g.widescreen = value != 0;
    else if (std::strcmp(key, "start_skips_cutscenes") == 0) g.start_skips_cutscenes = value != 0;
    else if (std::strcmp(key, "dual_stick") == 0) g.dual_stick = value != 0;
    else if (std::strcmp(key, "invert_aim_y") == 0) g.invert_aim_y = value != 0;
    else if (std::strcmp(key, "swap_triggers") == 0) g.swap_triggers = value != 0;
    else if (std::strcmp(key, "dual_layout") == 0) g.dual_layout = value != 0;
    else if (std::strcmp(key, "aim_from_character") == 0) g.aim_from_character = value != 0;
    else if (std::strcmp(key, "move_speed") == 0) g.move_speed = std::clamp(value, 25, 400);
    else if (std::strcmp(key, "diagonal_boost") == 0) g.diagonal_boost = value != 0;
    else if (std::strcmp(key, "strafe_speed") == 0) g.strafe_speed = std::clamp(value, 0, 300);
    else if (std::strcmp(key, "invert_aim_normal") == 0) g.invert_aim_normal = value != 0;
    else if (std::strcmp(key, "invert_camera_y") == 0) g.invert_camera_y = value != 0;
    else if (std::strcmp(key, "camera_speed_h") == 0) g.camera_speed_h = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "camera_speed_v") == 0) g.camera_speed_v = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "aim_shoulder") == 0) g.aim_shoulder = value != 0;
    else if (std::strcmp(key, "aim_centered") == 0) g.aim_centered = value != 0;
    else if (std::strcmp(key, "aim_jump") == 0) g.aim_jump = value != 0;
    else if (std::strcmp(key, "aim_speed") == 0) g.aim_speed = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "mouse_keyboard") == 0) g.mouse_keyboard = value != 0;
    else if (std::strcmp(key, "mouse_camera_speed") == 0) g.mouse_camera_speed = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "mouse_aim_speed") == 0) g.mouse_aim_speed = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "invert_mouse_y") == 0) g.invert_mouse_y = value != 0;
    else if (std::strcmp(key, "refuel_speed") == 0) g.refuel_speed = std::clamp(value, 25, 1000);
    else if (std::strcmp(key, "sniper_fixed_zoom") == 0) g.sniper_fixed_zoom = value != 0;
    else if (std::strcmp(key, "sniper_zoom") == 0) g.sniper_zoom = std::clamp(value, 100, 500);
    else if (std::strcmp(key, "damage_taken") == 0) g.damage_taken = std::clamp(value, 0, 400);
    else if (std::strcmp(key, "dive_speed") == 0) g.dive_speed = std::clamp(value, 50, 400);
    else if (std::strcmp(key, "fly_speed") == 0) g.fly_speed = std::clamp(value, 10, 1000);
    else if (std::strcmp(key, "fly_freeze") == 0) g.fly_freeze = value != 0;
    else if (std::strcmp(key, "fly_hide_hud") == 0) g.fly_hide_hud = value != 0;
}

void load_options() {
    if (g_options_loaded) return;
    g_options_loaded = true;
    if (std::FILE* f = std::fopen(OPTIONS_FILE, "r")) {
        char key[64]; int value;
        while (std::fscanf(f, "%63[^=]=%d\n", key, &value) == 2) apply_option(key, value);
        std::fclose(f);
    }
    push_render_options();
}

std::string options_text(const State& s) {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "no_blood=%d\nstart_skips_cutscenes=%d\n", s.no_blood ? 1 : 0, s.start_skips_cutscenes ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "show_fps=%d\nwidescreen=%d\nframe_rate=%d\n", s.show_fps ? 1 : 0, s.widescreen ? 1 : 0, s.frame_rate); out += buf;
    std::snprintf(buf, sizeof(buf), "show_level=%d\nshow_pack=%d\ndump_by_level=%d\n", s.show_level ? 1 : 0, s.show_pack ? 1 : 0, s.dump_by_level ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "no_lod=%d\nfar_objects=%d\n", s.no_lod ? 1 : 0, s.far_objects ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "ssao=%d\nssao_strength=%d\nssao_radius=%d\n", s.ssao ? 1 : 0, s.ssao_strength, s.ssao_radius); out += buf;
    std::snprintf(buf, sizeof(buf), "bloom=%d\nbloom_intensity=%d\nbloom_threshold=%d\nbloom_radius=%d\n", s.bloom ? 1 : 0, s.bloom_intensity, s.bloom_threshold, s.bloom_radius); out += buf;
    std::snprintf(buf, sizeof(buf), "dual_stick=%d\ninvert_aim_y=%d\nswap_triggers=%d\ndual_layout=%d\n", s.dual_stick ? 1 : 0, s.invert_aim_y ? 1 : 0, s.swap_triggers ? 1 : 0, s.dual_layout ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "invert_aim_normal=%d\ninvert_camera_y=%d\naim_from_character=%d\n", s.invert_aim_normal ? 1 : 0, s.invert_camera_y ? 1 : 0, s.aim_from_character ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "move_speed=%d\ndiagonal_boost=%d\nstrafe_speed=%d\n", s.move_speed, s.diagonal_boost ? 1 : 0, s.strafe_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "camera_speed_h=%d\ncamera_speed_v=%d\naim_shoulder=%d\naim_centered=%d\naim_speed=%d\n", s.camera_speed_h, s.camera_speed_v, s.aim_shoulder ? 1 : 0, s.aim_centered ? 1 : 0, s.aim_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "aim_jump=%d\n", s.aim_jump ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "mouse_keyboard=%d\nmouse_camera_speed=%d\nmouse_aim_speed=%d\ninvert_mouse_y=%d\n", s.mouse_keyboard ? 1 : 0, s.mouse_camera_speed, s.mouse_aim_speed, s.invert_mouse_y ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "refuel_speed=%d\nsniper_fixed_zoom=%d\nsniper_zoom=%d\ndamage_taken=%d\ndive_speed=%d\n", s.refuel_speed, s.sniper_fixed_zoom ? 1 : 0, s.sniper_zoom, s.damage_taken, s.dive_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "fly_speed=%d\nfly_freeze=%d\nfly_hide_hud=%d\n", s.fly_speed, s.fly_freeze ? 1 : 0, s.fly_hide_hud ? 1 : 0); out += buf;
    return out;
}

void profile_autosave();

void save_options() {
    if (std::FILE* f = std::fopen(OPTIONS_FILE, "w")) {
        std::fputs(options_text(g).c_str(), f);
        std::fclose(f);
    }
    profile_autosave();
}

// The available packs: "texture_pack" if that folder exists, then every folder and .rtz file in texture_packs, sorted.
std::vector<std::string> list_texture_packs() {
    std::vector<std::string> packs;
    std::error_code ec;
    if (std::filesystem::is_directory("texture_pack", ec)) packs.emplace_back("texture_pack");
    std::vector<std::string> named;
    for (std::filesystem::directory_iterator it("texture_packs", ec), end; !ec && it != end; it.increment(ec)) {
        // A pack is a folder, or a packed pack: a .rtz file (a zip with rt64.json at its root).
        bool is_pack = it->is_directory();
        if (!is_pack && it->is_regular_file()) {
            std::string ext = it->path().extension().string();
            for (char& c : ext) c = (char)std::tolower((unsigned char)c);
            is_pack = ext == ".rtz";
        }
        if (is_pack) { const auto n8 = it->path().filename().u8string(); named.emplace_back("texture_packs/" + std::string(n8.begin(), n8.end())); }
    }
    std::sort(named.begin(), named.end());
    packs.insert(packs.end(), named.begin(), named.end());
    return packs;
}

std::string pack_display_name(const std::string& folder) {
    if (folder.empty()) return "Off (original textures)";
    std::string name = folder.rfind("texture_packs/", 0) == 0 ? folder.substr(14) : folder;
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".rtz") == 0) name.resize(name.size() - 4);
    return name;
}

// Chooses a pack (or "" for none), saves the choice and makes the renderer (re)load it.
void set_texture_pack(const std::string& folder) {
    {
        std::lock_guard lock(g_pack_mutex);
        g_pack_folder = folder;
    }
    g_pack_generation++;
    if (std::FILE* f = std::fopen(PACK_CHOICE_FILE, "w")) { std::fprintf(f, "%s\n", folder.c_str()); std::fclose(f); }
}

void load_texture_pack_choice() {
    static bool loaded = false;
    if (loaded) return;
    loaded = true;
    std::string folder;
    if (std::FILE* f = std::fopen(PACK_CHOICE_FILE, "r")) {
        char line[512] = {};
        if (std::fgets(line, sizeof(line), f)) folder = line;
        std::fclose(f);
        while (!folder.empty() && (folder.back() == '\n' || folder.back() == '\r')) folder.pop_back();
    }
    else {
        std::error_code ec;
        if (std::filesystem::is_directory("texture_pack", ec)) folder = "texture_pack"; // as before packs had names
    }
    std::lock_guard lock(g_pack_mutex);
    g_pack_folder = folder;
    g_pack_generation++;
}

// F6: the next pack in the list, then Off, then the first again.
void next_texture_pack() {
    const std::vector<std::string> packs = list_texture_packs();
    std::string current;
    { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
    std::string next;
    if (current.empty()) next = packs.empty() ? "" : packs.front();
    else {
        auto it = std::find(packs.begin(), packs.end(), current);
        next = (it == packs.end() || it + 1 == packs.end()) ? "" : *(it + 1);
    }
    set_texture_pack(next);
    g_pack_toast = std::chrono::steady_clock::now();
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
            changed |= ImGui::Checkbox("START skips cutscenes", &g.start_skips_cutscenes);
            options_help("Also skips the logo screens at start-up and the story intro after creating a file. A skipped cutscene "
                         "still runs to its end at full machine speed, so everything it does (flags, items, the next level) still "
                         "happens.");
            if (g.skipping) ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.3f, 1.0f), "Skipping...");
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
            changed |= ImGui::Checkbox("Disable LOD (full-detail models at any distance)", &g.no_lod);
            options_help("Characters and objects normally switch to a simpler model once they are a little way off.");
            changed |= ImGui::Checkbox("Draw distant objects (no object draw distance)", &g.far_objects);
            options_help("Objects normally fade out and disappear beyond a set distance. With this they stay drawn. Takes effect "
                         "for the objects in play; level scenery itself is not affected.");
            if (FRAME_RATE_OPTION) {
                static const char* const rates[] = { "Original (30, drops to 20)", "60", "60 + interpolated (monitor rate)" };
                ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 22.0f));
                changed |= ImGui::Combo("Frame rate", &g.frame_rate, rates, IM_ARRAYSIZE(rates));
                options_help("60: the game updates every video frame instead of every second one; game speed stays the same. If a "
                             "frame runs long, the game simply advances further in the next one. "
                             "Interpolated: the renderer also adds in-between frames up to your monitor's refresh rate (like the "
                             "Graphics menu's Framerate: Display); some moving objects may look off. With Original, the Graphics "
                             "menu's own Framerate setting applies.");
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
                changed |= options_slider("Camera left/right %", &g.camera_speed_h, 25, 300);
                changed |= options_slider("Camera up/down %", &g.camera_speed_v, 25, 300);
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight() * 0.3f));
                changed |= ImGui::Checkbox("Diagonal speed boost", &g.diagonal_boost);
                options_help("With the original controls, running forward while sidestepping adds the two speeds together, so "
                             "going diagonally is about 40% faster. This gives the left stick the same boost when it is pushed "
                             "diagonally; the slider sets how much the sideways part counts.");
                ImGui::BeginDisabled(!g.diagonal_boost);
                changed |= options_slider("Strafe speed %", &g.strafe_speed, 0, 200);
                ImGui::EndDisabled();
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
            changed |= options_slider("Movement speed %", &g.move_speed, 50, 300);

            options_section("Aiming (any control layout)");
            changed |= ImGui::Checkbox("Over-the-shoulder view while aiming", &g.aim_shoulder);
            changed |= ImGui::Checkbox("Keep the reticle centered", &g.aim_centered);
            changed |= ImGui::Checkbox("Jump while aiming", &g.aim_jump);
            options_help("The jump button (C-up; A in the dual-stick layout, Space with mouse and keyboard) jumps while aiming, "
                         "also while walking or strafing: the character leaves the aim for the jump, the left stick steers it "
                         "like a normal jump, and it aims again on landing if the aim button is still held. Moving the left "
                         "stick forward while aiming still just walks. The sniper scope zooms with the D-pad (dual stick) or "
                         "1 / 3 (mouse and keyboard); on the original layout C-up jumps instead of zooming.");
            changed |= options_slider("Aim speed %", &g.aim_speed, 20, 200);
            changed |= ImGui::Checkbox("Sniper rifle: fixed zoom", &g.sniper_fixed_zoom);
            changed |= options_slider("Sniper zoom %", &g.sniper_zoom, 100, 500);
            options_help("With the fixed zoom the zoom buttons do nothing, so walking forward and back while aiming the "
                         "sniper rifle works as with the other weapons.");

            options_section("Player");
            changed |= options_slider("Damage taken %", &g.damage_taken, 0, 400);
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
            ImGui::Checkbox("Dump textures to bin\\texture_dump", &g.texture_dump);
            changed |= ImGui::Checkbox("One folder per level", &g.dump_by_level);
            options_help("Dumps into bin\\texture_dump\\<level number> <level name> instead. Each level starts its own set, so "
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
                if (dumped >= 0) ImGui::TextDisabled("bin\\texture_dump: %d files", dumped);
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
            options_help("Put each pack in its own folder (bin\\texture_packs\\<name>) or as a packed .rtz file (bin\\texture_packs\\<name>.rtz). Images named like the dumps replace "
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

// The game keeps the number of owned weapons next to the mask and only lets the player change weapon when it is 2 or
// more (the "all weapons" cheat writes mask 0x7FFF and count 15 the same way, in the character control overlays).
void set_owned_weapons(uint32_t ch, uint16_t owned) {
    owned &= 0x7FFF;
    int count = 0;
    for (int w = 0; w < 15; w++) count += (owned >> w) & 1;
    wr16(ch + CHAR_WEAPONS_OFFSET, owned);
    wr8(ch + CHAR_WEAPON_COUNT_OFFSET, (uint8_t)count);
}

int current_region() {
    const uint32_t header = rd32(ADDR_levelHeader);
    return valid_ptr(header) ? (int)rd8(header + 0x102) : 0;
}

void collected_checkbox(uint32_t ch, int n, const char* name) {
    const uint32_t addr = ch + CHAR_COLLECTED_OFFSET + (uint32_t)(n >> 3);
    const uint8_t mask = (uint8_t)(0x80u >> (n & 7));
    bool on = (rd8(addr) & mask) != 0;
    char label[64];
    std::snprintf(label, sizeof(label), "%s##c%d", name, n);
    if (ImGui::Checkbox(label, &on)) wr8(addr, (uint8_t)((rd8(addr) & ~mask) | (on ? mask : 0)));
}

void draw_tribals(recomp_context* ctx) {
    const uint32_t game = rd32(ADDR_gameplay);
    const uint32_t table = rd32(ADDR_tribaltable);
    ImGui::TextWrapped("For each region the game keeps your best visit: how many Tribals were rescued and how many died. "
                       "When a visit sets a new record and the rescued numbers of all regions add up to the total, it sets the "
                       "\"All tribals rescued\" flag, which is what earns the Tribals' ship part. Editing numbers here does not "
                       "run that check, so the button below sets the flag as well.");
    int total = 0, rescued = 0;
    if (valid_ptr(table)) for (int r = 0; r < 31; r++) { total += (int8_t)rd8(table + r); rescued += rd8(game + GAME_REGION_STATS_OFFSET + 8 * r); }
    ImGui::Text("Rescued in total: %d of %d", rescued, total);
    if (valid_ptr(table) && ImGui::Button("Mark every region fully rescued")) {
        for (int r = 0; r < 31; r++) {
            wr8(game + GAME_REGION_STATS_OFFSET + 8 * r, (uint8_t)std::max(0, (int)(int8_t)rd8(table + r)));
            wr8(game + GAME_REGION_STATS_OFFSET + 8 * r + 1, 0);
        }
        call_game(mainSetGameFlag, ctx, { 88u, 1u }); // ALL_TRIBALS_RESCUED, as frontShowRegionStats does
    }
    {
        bool flag = call_game(mainGetGameFlag, ctx, { 88u }) != 0;
        if (ImGui::Checkbox("\"All tribals rescued\" flag", &flag)) call_game(mainSetGameFlag, ctx, { 88u, flag ? 1u : 0u });
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear all##trib")) for (int r = 0; r < 31; r++) wr16(game + GAME_REGION_STATS_OFFSET + 8 * r, 0);
    if (ImGui::BeginTable("tribals", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Region", ImGuiTableColumnFlags_WidthStretch, 4.0f);
        ImGui::TableSetupColumn("Rescued", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Killed", ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn("Tribals", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableHeadersRow();
        for (int r = 1; r < 31; r++) {
            const int count = valid_ptr(table) ? (int)(int8_t)rd8(table + r) : 0;
            if (count <= 0) continue;
            const uint32_t rec = game + GAME_REGION_STATS_OFFSET + 8 * (uint32_t)r;
            ImGui::PushID(r);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("%s", region_names[r]);
            ImGui::TableNextColumn();
            int v = rd8(rec);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##saved", &v)) wr8(rec, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            v = rd8(rec + 1);
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputInt("##killed", &v)) wr8(rec + 1, (uint8_t)std::clamp(v, 0, count));
            ImGui::TableNextColumn();
            ImGui::Text("%d", count);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("This visit (the region you are in)");
    const int region = current_region();
    ImGui::Text("Region: %s", (region >= 1 && region <= 30) ? region_names[region] : "(none)");
    int v = rd8(game + 4);
    if (ImGui::InputInt("Rescued so far", &v)) wr8(game + 4, (uint8_t)std::clamp(v, 0, 255));
    v = rd8(game + 5);
    if (ImGui::InputInt("Killed so far", &v)) wr8(game + 5, (uint8_t)std::clamp(v, 0, 255));
    v = rd8(game + 6);
    if (ImGui::InputInt("Remaining", &v)) wr8(game + 6, (uint8_t)std::clamp(v, 0, 255));
    ImGui::TextDisabled("\"Killed\" above 0 or \"Remaining\" above 0 when you leave means the visit is not a full rescue.");
}

// Multiplayer unlocks. Totem poles (totemPoleControl: mainSetGameFlag(romdef+0xA)), Floyd medals and race wins set game
// flags that mainSetGameFlag also copies into globalflags (0x800FF318), the part of the Controller Pak / EEPROM data
// shared by all save files, which is what the multiplayer menus read:
//   flags 0..19  -> globalflags+0x64 bit n: multiplayer characters (frontMultiSelect's table at 0x82381050: flags 0-2 the
//                   three heroes, 4-6 set with the jetpack (flag 95), 8-16 and 18 the totem-pole characters, 17 the
//                   Cyborg won with expert Floyd medals ("Won the CYBORG"));
//   flags 66..70 -> +0x6A bits 0..4: race tracks (frontMultiModeSelect's table at 0x823C285C: 66 level 268, 67 level
//                   258, 68 level 397, 70 level 202);
//   flags 73..76 -> +0x6B bits 0..3: target ranges (table at 0x823C2878: 73 level 251, 74 level 364, 75 level 408);
//   flags 82..87 -> +0x68 bits 0..5: battle arenas (table at 0x823C2828: 82 level 221, 83 level 144, 84 level 355,
//                   85 level 270).
// Unlocking sets the game flag the way the game does (which also saves the shared data); locking clears both bits
// (the shared data is written the next time the game saves it).
constexpr uint32_t ADDR_globalflags = 0x800FF318;
struct MultiUnlock { int flag; const char* name; };
const MultiUnlock multi_characters[] = {
    {4, "Character (jetpack, flag 4)"}, {5, "Character (jetpack, flag 5)"}, {6, "Character (jetpack, flag 6)"},
    {8, "Totem character (flag 8)"}, {9, "Totem character (flag 9)"}, {10, "Totem character (flag 10)"},
    {11, "Totem character (flag 11)"}, {12, "Totem character (flag 12)"}, {13, "Totem character (flag 13)"},
    {14, "Totem character (flag 14)"}, {15, "Totem character (flag 15)"}, {16, "Totem character (flag 16)"},
    {18, "Totem character (flag 18)"}, {17, "Cyborg Ant (expert Floyd medals)"},
};
const MultiUnlock multi_levels[] = {
    {82, "Battle arena: Mine (level 221)"}, {83, "Battle arena: Sewer (level 144)"}, {84, "Battle arena: Tunnels (level 355)"},
    {85, "Battle arena: Swamp (level 270)"},
    {66, "Race: Mizar's Race (level 268)"}, {67, "Race: Jeff and Barry (level 258)"}, {68, "Race: Jeff and Barry II (level 397)"},
    {70, "Race: Greenwood Village (level 202)"},
    {73, "Target range: Goldwood (level 251)"}, {74, "Target range: SS Anubis (level 364)"}, {75, "Target range: Rith Essa (level 408)"},
};
bool multi_unlocked(int flag) {
    if (flag >= 0 && flag < 20) return (rd32(ADDR_globalflags + 0x64) >> flag) & 1;
    if (flag >= 66 && flag <= 70) return (rd8(ADDR_globalflags + 0x6A) >> (flag - 66)) & 1;
    if (flag >= 73 && flag <= 76) return (rd8(ADDR_globalflags + 0x6B) >> (flag - 73)) & 1;
    if (flag >= 82 && flag <= 87) return (rd16(ADDR_globalflags + 0x68) >> (flag - 82)) & 1;
    return false;
}
void set_multi_unlocked(recomp_context* ctx, int flag, bool on) {
    if (on) { call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 1u }); return; }
    call_game(mainSetGameFlag, ctx, { (uint32_t)flag, 0u });
    if (flag >= 0 && flag < 20) wr32(ADDR_globalflags + 0x64, rd32(ADDR_globalflags + 0x64) & ~(1u << flag));
    else if (flag >= 66 && flag <= 70) wr8(ADDR_globalflags + 0x6A, (uint8_t)(rd8(ADDR_globalflags + 0x6A) & ~(1u << (flag - 66))));
    else if (flag >= 73 && flag <= 76) wr8(ADDR_globalflags + 0x6B, (uint8_t)(rd8(ADDR_globalflags + 0x6B) & ~(1u << (flag - 73))));
    else if (flag >= 82 && flag <= 87) wr16(ADDR_globalflags + 0x68, (uint16_t)(rd16(ADDR_globalflags + 0x68) & ~(1u << (flag - 82))));
}
void draw_multiplayer_unlocks(recomp_context* ctx) {
    ImGui::TextWrapped("Bonuses found in the game (totem poles, Floyd medals, race wins) that unlock multiplayer characters "
                       "and levels. They are shared by all save files. The game does not name the totem characters in "
                       "its data, so they are listed by flag.");
    if (ImGui::Button("Unlock all##multi")) {
        for (const MultiUnlock& u : multi_characters) set_multi_unlocked(ctx, u.flag, true);
        for (const MultiUnlock& u : multi_levels) set_multi_unlocked(ctx, u.flag, true);
    }
    ImGui::SeparatorText("Characters");
    if (ImGui::BeginTable("multi_chars", 2)) {
        for (const MultiUnlock& u : multi_characters) {
            ImGui::TableNextColumn();
            bool on = multi_unlocked(u.flag);
            if (ImGui::Checkbox(u.name, &on)) set_multi_unlocked(ctx, u.flag, on);
        }
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Levels");
    for (const MultiUnlock& u : multi_levels) {
        bool on = multi_unlocked(u.flag);
        if (ImGui::Checkbox(u.name, &on)) set_multi_unlocked(ctx, u.flag, on);
    }
}

void draw_inventory(recomp_context* ctx) {
    if (g.edit_character < 0) {
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        g.edit_character = (c >= 0 && c < 3) ? c : 1;
    }
    ImGui::Combo("Character##inv", &g.edit_character, character_names, 3);
    const uint32_t ch = character_ptr(g.edit_character);
    if (ch == 0) {
        ImGui::TextDisabled("No game loaded.");
        return;
    }

    if (ImGui::CollapsingHeader("Weapons and ammo", ImGuiTreeNodeFlags_DefaultOpen)) {
        uint16_t owned = rd16(ch + CHAR_WEAPONS_OFFSET);
        ImGui::Text("Current weapon slot: %d", (int)rd8(ch + CHAR_WEAPON_OFFSET));
        if (ImGui::Button("Give all")) { owned = 0x7FFF; set_owned_weapons(ch, owned); }
        ImGui::SameLine();
        if (ImGui::Button("Fill ammo")) {
            for (int w = 0; w < 16; w++) wr16(ch + CHAR_AMMO_OFFSET + 2 * w, 999);
        }
        if (ImGui::BeginTable("weapons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Ammo", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (int w = 0; w < 15; w++) {
                ImGui::PushID(w);
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                bool has = (owned >> w) & 1;
                char label[48];
                std::snprintf(label, sizeof(label), "%d  %s", w, weapon_names[w]);
                if (ImGui::Checkbox(label, &has)) {
                    owned = (uint16_t)((owned & ~(1u << w)) | ((has ? 1u : 0u) << w));
                    set_owned_weapons(ch, owned);
                }
                ImGui::TableNextColumn();
                int ammo = rd16(ch + CHAR_AMMO_OFFSET + 2 * w);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::InputInt("##ammo", &ammo, 10, 100)) wr16(ch + CHAR_AMMO_OFFSET + 2 * w, (uint16_t)std::clamp(ammo, 0, 0xFFFF));
                ImGui::TableNextColumn();
                if (g.edit_character == (int16_t)rd16(ADDR_currentCharacter) && ImGui::SmallButton("Hold")) select_weapon(ctx, w);
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (ImGui::CollapsingHeader("Health")) {
        // Game character: +0 s16 health (256 per unit), +2 s16 health level. A level allows 5 * level + 4 units; a new
        // character starts at level 6 and each "Gemini capacity" pickup adds one, up to 12 (healthPickupControl).
        // The character on screen keeps its live health in its object's damage data (object + 0x4C, +6).
        int level = (int16_t)rd16(ch + CHAR_HEALTH_LEVEL_OFFSET);
        const bool on_screen = g.edit_character == (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t obj = on_screen ? player_object(ctx) : 0;
        const uint32_t live = (obj != 0 && valid_ptr(rd32(obj + 0x4C))) ? rd32(obj + 0x4C) + 6 : 0;
        int units = (int16_t)rd16(live != 0 ? live : ch + CHAR_HEALTH_OFFSET) / 256;
        auto set_health = [&](int new_units) {
            const uint16_t v = (uint16_t)(std::clamp(new_units, 1, 5 * level + 4) * 256);
            wr16(ch + CHAR_HEALTH_OFFSET, v);
            if (live != 0) wr16(live, v);
        };
        int upgrades = level - 6;
        if (ImGui::SliderInt("Upgrades", &upgrades, 0, 6)) {
            level = std::clamp(upgrades, 0, 6) + 6;
            wr16(ch + CHAR_HEALTH_LEVEL_OFFSET, (uint16_t)level);
            set_health(5 * level + 4);
        }
        ImGui::Text("Maximum health: %d units", 5 * level + 4);
        if (ImGui::InputInt("Health", &units)) set_health(units);
        if (ImGui::Button("Fill health")) set_health(5 * level + 4);
    }

    if (ImGui::CollapsingHeader("Keys and objects (per character)")) {
        ImGui::SeparatorText("Keys");
        if (ImGui::BeginTable("keys", 2)) {
            for (const NamedFlag& k : key_items) { ImGui::TableNextColumn(); collected_checkbox(ch, k.id, k.name); }
            ImGui::EndTable();
        }
        ImGui::SeparatorText("Objects");
        if (ImGui::BeginTable("objects", 2)) {
            for (const NamedFlag& k : object_items) { ImGui::TableNextColumn(); collected_checkbox(ch, k.id, k.name); }
            ImGui::EndTable();
        }
        if (ImGui::TreeNode("Other pickup numbers (no known use)")) {
            for (int n = 0, col = 0; n < COLLECTED_BITS; n++) {
                bool named = false;
                for (const NamedFlag& k : key_items) named |= k.id == n;
                for (const NamedFlag& k : object_items) named |= k.id == n;
                if (named) continue;
                if ((col++ & 7) != 0) ImGui::SameLine((float)((col - 1) & 7) * ImGui::GetFontSize() * 4.0f);
                char label[8];
                std::snprintf(label, sizeof(label), "%02d", n);
                collected_checkbox(ch, n, label);
            }
            ImGui::TreePop();
        }
    }

    if (ImGui::CollapsingHeader("Tribals")) draw_tribals(ctx);

    if (ImGui::CollapsingHeader("Ship parts")) {
        if (ImGui::Button("Give all##ship")) {
            for (const NamedFlag& f : ship_part_flags) call_game(mainSetGameFlag, ctx, { (uint32_t)f.id, 1u });
        }
        flag_checkboxes(ctx, ship_part_flags, std::size(ship_part_flags));
    }
    if (ImGui::CollapsingHeader("Floyd parts")) flag_checkboxes(ctx, robot_flags, std::size(robot_flags));
    if (ImGui::CollapsingHeader("Multiplayer unlocks")) draw_multiplayer_unlocks(ctx);
    if (ImGui::CollapsingHeader("Progress flags")) flag_checkboxes(ctx, progress_flags, std::size(progress_flags));
    if (ImGui::CollapsingHeader("Any game flag by number")) {
        ImGui::InputInt("Flag", &g.raw_flag);
        g.raw_flag = std::clamp(g.raw_flag, 0, 127);
        bool on = call_game(mainGetGameFlag, ctx, { (uint32_t)g.raw_flag }) != 0;
        if (ImGui::Checkbox("Set", &on)) call_game(mainSetGameFlag, ctx, { (uint32_t)g.raw_flag, on ? 1u : 0u });
    }
}

} // namespace

// Called once per game tick on the game's main thread (hook at the entry of func_80044FAC_45BAC).
void jfg_camera_restore();
void update_dump_folder();
void apply_no_lod(recomp_context* ctx);
void apply_weapons();
void draw_weapons();
void draw_profiles();
void draw_toolbar(recomp_context* ctx);
// Height of F10's toolbar while it is open (0 otherwise), measured when drawn: the corner overlays start below it.
float g_toolbar_height = 0.0f;
static float toolbar_offset() {
    return (dino::debug_ui::is_open() && dino::debug_ui::backend::toolbar_mode()) ? g_toolbar_height : 0.0f;
}
void debug_profile_tick();
extern "C" void jfg_debug_frame(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    jfg_camera_restore();
    load_options();
    update_dump_folder();
    apply_no_lod(ctx);
    apply_weapons();
    debug_profile_tick();

    // Cheats keep applying while the menu is closed.
    apply_flag(g.invulnerable, g.applied_invulnerable, ADDR_invulnerabilityCheat);
    apply_flag(g.infinite_ammo, g.applied_ammo, ADDR_DebugAmmoFlag);
    apply_flag(g.all_weapons, g.applied_weapons, ADDR_allweaponsCheat);
    apply_flag(g.all_levels, g.applied_levels, ADDR_allLevelCheat);

    // Scripted testing without the UI: JFG_DEBUG_CHEATS=1 turns the four cheats on;
    // JFG_DEBUG_WARP="seconds,level,character,entrance" warps once that many seconds after start.
    {
        static uint32_t tick = 0;
        static const auto start = std::chrono::steady_clock::now();
        static bool warped = false;
        static const char* warp = std::getenv("JFG_DEBUG_WARP");
        static const bool cheats = std::getenv("JFG_DEBUG_CHEATS") != nullptr;
        static int w[4] = { -1, 0, 1, 0 };
        if (tick == 0) {
            if (cheats) g.invulnerable = g.infinite_ammo = g.all_weapons = g.all_levels = true;
            if (warp != nullptr) std::sscanf(warp, "%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3]);
        }
        tick++;
        if (!warped && w[0] >= 0 && std::chrono::steady_clock::now() - start >= std::chrono::seconds(w[0])) {
            warped = true;
            g.warp_level = w[1]; g.warp_character = w[2]; g.warp_entrance = w[3]; g.pending_warp = true;
        }
    }

    // Scripted testing: JFG_DEBUG_ITEMS=1 gives the current character every key and object;
    // JFG_DEBUG_WEAPONS=<mask> sets the current character's owned-weapon mask the way the Items tab does.
    {
        static const bool items = std::getenv("JFG_DEBUG_ITEMS") != nullptr;
        static const char* const weapons = std::getenv("JFG_DEBUG_WEAPONS");
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
        if (ch != 0 && rd32(ADDR_mainGameMode) == 0) {
            if (items) for (uint32_t b = 0; b < 4; b++) wr8(ch + CHAR_COLLECTED_OFFSET + b, 0xFF);
            if (weapons != nullptr) set_owned_weapons(ch, (uint16_t)std::strtoul(weapons, nullptr, 0));
        }
    }

    // Scripted testing: JFG_DEBUG_HOLD_WEAPON=<slot> keeps that weapon in the current character's hand.
    {
        static const char* hold = std::getenv("JFG_DEBUG_HOLD_WEAPON");
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
        if (hold != nullptr && ch != 0 && rd32(ADDR_mainGameMode) == 0) {
            const int slot = std::atoi(hold);
            if (rd8(ch + CHAR_WEAPON_OFFSET) != slot && select_weapon(ctx, slot)) {
                const uint32_t obj = player_object(ctx);
                if (obj != 0 && valid_ptr(rd32(obj + 0x68))) wr8(rd32(obj + 0x68) + 0x585, (uint8_t)slot);
            }
        }
    }

    // Scripted testing: JFG_DEBUG_WEAPON_CYCLE="start,period" puts weapon slot 1, 2, 3... in the current
    // character's hand, one every `period` seconds from `start` seconds after launch.
    {
        static const char* cyc = std::getenv("JFG_DEBUG_WEAPON_CYCLE");
        static int c[2] = { -1, 5 };
        static bool parsed = false;
        static const auto start = std::chrono::steady_clock::now();
        static int last = 0;
        if (cyc != nullptr) {
            if (!parsed) { std::sscanf(cyc, "%d,%d", &c[0], &c[1]); parsed = true; }
            const int secs = (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            const int slot = (c[0] >= 0 && secs >= c[0] && c[1] > 0) ? 1 + (secs - c[0]) / c[1] : 0;
            if (slot != last && slot >= 1 && slot <= 15) {
                last = slot;
                const bool ok = select_weapon(ctx, slot);
                std::fprintf(stderr, "[jfg] debug: weapon slot %d %s\n", slot, ok ? "selected" : "refused");
            }
        }
    }

    // "Press F9" hint: shown for a few seconds after the game starts, then faded out. It is drawn through the same
    // ImGui context as the menu; the backend renders it even though the menu is closed (set_overlay).
    static const auto hint_start = std::chrono::steady_clock::now();
    const float hint_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - hint_start).count();
    constexpr float HINT_DELAY = 2.0f, HINT_HOLD = 7.0f, HINT_FADE = 2.0f;
    static bool hint_done = false;
    if (dino::debug_ui::is_open() || hint_t > HINT_DELAY + HINT_HOLD + HINT_FADE) hint_done = true;
    const bool hint_active = !hint_done && hint_t >= HINT_DELAY;
    const float pack_toast_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_pack_toast).count();
    const bool pack_toast = g_pack_toast.time_since_epoch().count() != 0 && pack_toast_t < 3.0f;
    dino::debug_ui::backend::set_overlay(hint_active || g.show_fps || g.show_level || g.show_pack || pack_toast);

    dino::debug_ui::ui_frame_begin();
    if (hint_active && dino::debug_ui::backend::in_ui_frame()) {
        const float t = hint_t - HINT_DELAY;
        float alpha = 1.0f;
        if (t < 0.5f) alpha = t / 0.5f;
        else if (t > HINT_HOLD) alpha = std::max(0.0f, 1.0f - (t - HINT_HOLD) / HINT_FADE);
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const char* text = "Settings: Select / Esc  -  Tools: F10  -  Debug window: F9";
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(18.0f, size.y / 32.0f);
        const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
        const ImVec2 pos(size.x * 0.5f - extent.x * 0.5f, size.y * 0.90f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float pad = font_size * 0.4f;
        dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f * alpha)), pad * 0.5f);
        dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), text);
    }
    if (pack_toast && dino::debug_ui::backend::in_ui_frame()) {
        // F6: which pack is now in use, for a few seconds at the top of the screen.
        std::string current;
        { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
        std::string text = "Texture pack: " + pack_display_name(current);
        const int n = g_texture_pack_count.load();
        if (!current.empty() && n >= 0) text += "  (" + std::to_string(n) + " textures)";
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(18.0f, size.y / 32.0f);
        const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 pos(size.x * 0.5f - extent.x * 0.5f, size.y * 0.06f + toolbar_offset());
        const float alpha = pack_toast_t > 2.5f ? (3.0f - pack_toast_t) / 0.5f : 1.0f;
        const float pad = font_size * 0.4f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f * alpha)), pad * 0.5f);
        dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), text.c_str());
    }
    if ((g.show_fps || g.show_level || g.show_pack) && dino::debug_ui::backend::in_ui_frame()) {
        // Top right corner, one line each: frame rate (main loop ticks per second, measured in jfg_frame_sync), the
        // level, the texture pack.
        struct Line { std::string text; ImVec4 color; };
        std::vector<Line> lines;
        if (g.show_fps) {
            char text[64];
            if (FRAME_RATE_OPTION) std::snprintf(text, sizeof(text), "%.0f FPS  (screen %.0f)", g_fps.load(std::memory_order_relaxed), g_screen_fps.load(std::memory_order_relaxed));
            else std::snprintf(text, sizeof(text), "%.0f FPS", g_fps.load(std::memory_order_relaxed));
            lines.push_back({ text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f) });
        }
        if (g.show_level && g_rdram != nullptr) {
            load_level_names();
            const int level = (int16_t)rd16(ADDR_currentLevel);
            std::string text = "Level " + std::to_string(level);
            if (level >= 0 && level < (int)g_level_names.size()) text += ": " + g_level_names[level];
            lines.push_back({ text, ImVec4(0.75f, 0.95f, 1.0f, 1.0f) });
        }
        if (g.show_pack) {
            std::string current;
            { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
            lines.push_back({ "Texture pack: " + pack_display_name(current), ImVec4(0.8f, 1.0f, 0.8f, 1.0f) });
        }
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(16.0f, size.y / 40.0f);
        const float pad = font_size * 0.35f;
        float y = pad * 1.5f + toolbar_offset(); // below F10's toolbar while it is open
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const Line& line : lines) {
            const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.text.c_str());
            const ImVec2 pos(size.x - extent.x - pad * 2.5f, y);
            dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)), pad * 0.5f);
            dl->AddText(font, font_size, pos, ImGui::GetColorU32(line.color), line.text.c_str());
            y += extent.y + pad * 1.4f;
        }
    }
    if (dino::debug_ui::is_open() && dino::debug_ui::backend::in_ui_frame() && dino::debug_ui::backend::toolbar_mode()) {
        load_level_names();
        draw_toolbar(ctx);
    }
    else if (dino::debug_ui::is_open() && dino::debug_ui::backend::in_ui_frame()) {
        load_level_names();
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
    dino::debug_ui::ui_frame_end();

    // 60 fps: viFrameSync paces the game by gVideoDeltaTime (2 video frames, 3 when it falls behind); the game's own
    // runInOneFrame flag makes it 1. viFrameRateReset() clears the flag again (level changes), so set it every tick.
    if (g.frame_rate >= 1) wr32(ADDR_runInOneFrame, 1);

    // F5: back to the title screen (F1-F4 belong to RT64's developer tools), the way the game itself gets there (mainTitlePageInit: game mode 1, then
    // mainChangeLevel(title level, 0, 0, 3, 1, 0)). Not while a skip runs. From the pause screen the title level
    // does not load (trackInit hits a break), so the pause screen is closed first with a START press.
    {
        static int wait_ticks = 0;
        bool& pending = g_reset_pending;
        if (dino::debug_ui::backend::take_reset_request()) { pending = true; wait_ticks = 0; }
        load_texture_pack_choice();
        if (dino::debug_ui::backend::take_next_pack_request()) next_texture_pack();
        if (dino::debug_ui::backend::take_fly_request()) g_fly_toggle_request = true; // F7: fly camera
        const int mode = (int)rd32(ADDR_mainGameMode);
        if (pending && !g.skipping && (mode == 0 || mode == 1)) {
            if (rd8(ADDR_pauseMode) != 0) {
                // jfg_after_joy presses START (after joyRead, which would overwrite it here) every 15th tick.
                g_reset_press_start = (wait_ticks++ % 15) == 0;
                if (wait_ticks > 300) { pending = false; g_reset_press_start = false; } // the pause screen did not close
            }
            else {
                pending = false;
                g_reset_press_start = false;
                std::fprintf(stderr, "[jfg] F5: back to the title screen (level %d, game mode %d)\n", (int)(int16_t)rd16(ADDR_currentLevel), mode);
                wr8(ADDR_waitMidiSync, 0);
                wr32(ADDR_midiSyncReached, 1);
                call_game(mainTitlePageInit, ctx, {});
            }
        }
    }
    {
        // Tests: JFG_DEBUG_FORCE_BREAK=<secs> calls the game's mathBreakPoint once.
        static const char* const brk = std::getenv("JFG_DEBUG_FORCE_BREAK");
        static const auto start = std::chrono::steady_clock::now();
        if (brk != nullptr && std::chrono::steady_clock::now() - start > std::chrono::seconds(std::atoi(brk))) call_game(mathBreakPoint, ctx, {});
    }
    if (g_story_skip_pending) {
        g_story_skip_pending = false;
        const int level = (int16_t)rd16(ADDR_currentLevel);
        if (level >= LEVEL_STORY_FIRST && level <= LEVEL_STORY_LAST) {
            // The intro's level changes wait for a marker in its music (func_800468EC calls amWaitForMidiSync when
            // u8 D_800A326C is set; amWaitForMidiSync returns at once when D_800E9774 is set). Neither may hold up
            // this change, or the main loop sits there with a frozen picture while the music plays on.
            wr8(ADDR_waitMidiSync, 0);
            wr32(ADDR_midiSyncReached, 1);
            call_game(mainChangeLevel, ctx, { (uint32_t)LEVEL_CHARACTER_SELECT, 0u, 0u, 5u, 1u, 0u });
        }
    }
    if (g.pending_warp) {
        g.pending_warp = false;
        // Same argument pattern as the game's own level exits (controlPlayer): the front-end mode is passed
        // through unchanged - passing 0 there starts the level without its HUD and weapon selection.
        const uint32_t mode = call_game(frontGetMode, ctx, {}) & 0xFFu;
        std::fprintf(stderr, "[jfg] debug menu: mainChangeLevel(%d, %d, %d, %u, 1, 0)\n", g.warp_level, g.warp_character, g.warp_entrance, mode);
        call_game(mainChangeLevel, ctx, { (uint32_t)g.warp_level, (uint32_t)g.warp_character, (uint32_t)g.warp_entrance, mode, 1u, 0u });
    }
}

// --- options used by hooks in jfg.us.toml ---------------------------------------------------------------------------

// bloodSpurtAdd / bloodHitSpriteAdd (overlay 8) return immediately when this is non-zero.
// --- out of memory and game assertions --------------------------------------------------------------------------
constexpr uint32_t ADDR_gFreeQueueCount = 0x800FDD88; // s32: frees waiting out mmSetDelay() ticks (gFreeQueue)

void jfg_log_line(const char* file, const char* text) {
    std::fprintf(stderr, "[jfg] %s\n", text);
    if (std::FILE* f = std::fopen(file, "a")) {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "%s  %s\n", stamp, text);
        std::fclose(f);
    }
}

// mmAlloc, after mempool_slot_find ($v0 = block or 0; size and colour are at 0x30/0x34($sp)). A level load asks for
// big blocks (the track buffer is 0x9F000 bytes, see func_overlay_24_000001F8) while earlier frees may still wait in
// the delayed free queue. When the heap has no room, release that queue and try once more; log either way to
// bin/jfg_memory.log.
extern "C" void jfg_mm_alloc_check(uint8_t* rdram_, recomp_context* ctx) {
    {
        // Tests: JFG_DEBUG_FAIL_ALLOC=<size hex> makes the first allocation of that size look failed (it leaks).
        static const char* const fail = std::getenv("JFG_DEBUG_FAIL_ALLOC");
        static bool failed_once = false;
        if (fail != nullptr && !failed_once && (uint32_t)ctx->r2 != 0) {
            g_rdram = rdram_;
            if (rd32((uint32_t)ctx->r29 + 0x30) == (uint32_t)std::strtoul(fail, nullptr, 16)) { failed_once = true; ctx->r2 = 0; }
        }
    }
    if ((uint32_t)ctx->r2 != 0) return;
    g_rdram = rdram_;
    static thread_local bool retrying = false;
    if (retrying) return;
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t size = rd32(sp + 0x30), colour = rd32(sp + 0x34);
    const int pending = (int)rd32(ADDR_gFreeQueueCount);
    uint32_t block = 0;
    if (pending > 0) {
        retrying = true;
        call_game(mmFlushFreeStack, ctx, {});
        block = call_game(mmAlloc, ctx, { size, colour });
        retrying = false;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "mmAlloc(0x%X) found no room (level %d, next %d, %d delayed frees); %s", size, (int)(int16_t)rd16(ADDR_currentLevel),
        (int)(int16_t)rd16(ADDR_nextLevel), pending, block != 0 ? "retry after releasing them worked" : "still no room");
    jfg_log_line("jfg_memory.log", line);
    if (block != 0) ctx->r2 = (gpr)(int32_t)block;
}

// mathBreakPoint: the game stopped itself on a failed check (on the N64 this is a debugger break). Leave a note and a
// message instead of letting the runtime's exit() run its shutdown (which has crashed and left an empty crash.log).
extern "C" void jfg_game_break(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    char line[320];
    std::snprintf(line, sizeof(line), "game error (break) called from 0x%08X; level %d, next level %d, game mode %d, last mmAlloc result 0x%08X",
        (uint32_t)ctx->r31, (int)(int16_t)rd16(ADDR_currentLevel), (int)(int16_t)rd16(ADDR_nextLevel), (int)rd32(ADDR_mainGameMode), (uint32_t)ctx->r2);
    jfg_log_line("jfg_error.log", line);
    std::string msg = std::string("Jet Force Gemini stopped with an internal game error.\n\n") + line +
        "\n\nThe details were saved to jfg_error.log (and jfg_memory.log, if it exists) in the game folder.";
    ultramodern::error_handling::message_box(msg.c_str());
    std::fflush(nullptr);
    std::_Exit(1);
}

// Entry of mainChangeLevel (hook in jfg.us.toml): logs the arguments with JFG_DEBUG_STATS.
extern "C" void jfg_change_level_log(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    static const bool log = std::getenv("JFG_DEBUG_STATS") != nullptr;
    if (!log) return;
    const uint32_t sp = (uint32_t)ctx->r29;
    std::fprintf(stderr, "[jfg] mainChangeLevel(%d, %d, %d, %d, %d, %d) from level %d, game mode %d, ra %08X\n", (int)ctx->r4, (int)ctx->r5, (int)ctx->r6, (int)ctx->r7,
        (int)rd32(sp + 0x10), (int)rd32(sp + 0x14), (int)(int16_t)rd16(ADDR_currentLevel), (int)rd32(ADDR_mainGameMode), (uint32_t)ctx->r31);
}

// Read by the renderer thread (src/renderer/renderer.cpp) for the Textures section of the Options tab.
extern "C" int jfg_texture_dump_active(void) { static const bool env = std::getenv("JFG_DEBUG_TEXTURE_DUMP") != nullptr; return (g.texture_dump || env) ? 1 : 0; }
extern "C" void jfg_texture_pack_set_count(int count) { g_texture_pack_count = count; }

// "Disable LOD". objPrintModelObject draws an object with its next, simpler model (model index + 1) when the object
// is further from the camera than the distance in its object definition (float at header + 0xCC; 0 = no LOD). With the
// option, that distance is set to 0 in the definition of every object in play; the old values are kept and put back
// when the option is turned off (definitions are reloaded with each level, so the record is dropped on a level change).
// Nothing else reads header + 0xCC except the reflection drawing (overlay 137), which makes the same choice.
void apply_no_lod(recomp_context* ctx) {
    // Two fields of an object definition, each set to 0 while its option is on (originals kept to put back):
    //   + 0xCC float: LOD distance (objPrintModelObject); + 0x16 s16: draw distance (track.c func_80015ED8 skips the
    //   object beyond it, and fades it out just before; 0 = always drawn).
    struct Field { uint32_t offset; uint32_t size; const bool* option; std::unordered_map<uint32_t, uint32_t> saved; };
    static Field fields[2] = { { 0xCC, 4, &g.no_lod, {} }, { 0x16, 2, &g.far_objects, {} } };
    static int saved_level = -1;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != saved_level) { for (Field& f : fields) f.saved.clear(); saved_level = level; }
    bool work = false;
    for (const Field& f : fields) work |= *f.option || !f.saved.empty();
    if (!work) return;
    const uint32_t scratch = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u;
    wr32(scratch, 0); wr32(scratch + 4, 0);
    const uint32_t list = call_game(objGetObjList, ctx, { scratch, scratch + 4 });
    const int first = (int32_t)rd32(scratch), last = (int32_t)rd32(scratch + 4);
    if (!valid_ptr(list) || first < 0 || last > 2000) return;
    static const bool log = std::getenv("JFG_DEBUG_LOD") != nullptr;
    for (int i = first; i < last; i++) {
        const uint32_t obj = rd32(list + 4 * i);
        if (!valid_ptr(obj)) continue;
        const uint32_t def = rd32(obj + 0x40);
        if (!valid_ptr(def)) continue;
        for (Field& f : fields) {
            const uint32_t value = f.size == 4 ? rd32(def + f.offset) : rd16(def + f.offset);
            auto put = [&](uint32_t v) { if (f.size == 4) wr32(def + f.offset, v); else wr16(def + f.offset, (uint16_t)v); };
            if (*f.option) {
                if (value != 0 && f.saved.find(def) == f.saved.end()) {
                    f.saved[def] = value; put(0);
                    if (log) std::fprintf(stderr, "[jfg] object %d (definition %08X): field +0x%X was %08X, now 0\n", (int)(int16_t)rd16(obj + 0x48), def, f.offset, value);
                }
            }
            else {
                auto it = f.saved.find(def);
                if (it != f.saved.end() && value == 0) put(it->second);
            }
        }
    }
    for (Field& f : fields) if (!*f.option) f.saved.clear();
}

// Texture dumping, "One folder per level": the sub-folder of texture_dump to write into ("" = texture_dump itself).
// Worked out on the game thread (jfg_debug_frame, where the level names are loaded) and read by the renderer thread.
std::mutex g_dump_folder_mutex;
std::string g_dump_folder;
extern "C" void jfg_texture_dump_folder(char* buf, int size) {
    std::lock_guard lock(g_dump_folder_mutex);
    std::snprintf(buf, (size_t)size, "%s", g_dump_folder.c_str());
}
void update_dump_folder() {
    std::string folder;
    if (g.dump_by_level && g_rdram != nullptr) {
        load_level_names();
        const int level = (int16_t)rd16(ADDR_currentLevel);
        char num[16];
        std::snprintf(num, sizeof(num), "%03d", level);
        folder = num;
        if (level >= 0 && level < (int)g_level_names.size()) {
            std::string name;
            for (char c : g_level_names[level]) name += (std::isalnum((unsigned char)c) || c == ' ' || c == '-' || c == '_') ? c : '_';
            while (!name.empty() && name.back() == ' ') name.pop_back();
            if (!name.empty()) folder += " " + name;
        }
    }
    std::lock_guard lock(g_dump_folder_mutex);
    g_dump_folder = folder;
}
extern "C" int jfg_texture_pack_current(char* buf, int size) {
    std::lock_guard lock(g_pack_mutex);
    std::snprintf(buf, (size_t)size, "%s", g_pack_folder.c_str());
    return g_pack_generation.load();
}

// Read by the renderer thread (src/renderer/renderer.cpp) for the "Frame rate" option: -1 = keep the Graphics menu's
// Framerate setting, 0 = Original (the frames the game draws), 1 = Display (RT64 adds in-between frames up to the
// monitor's refresh rate).
// Scheduler thread, every video retrace (hook in __scHandleRetrace, see jfg.us.toml): 1 = count the retrace twice, so
// the scheduler hands each finished frame back after one retrace instead of two (60 fps instead of 30).
extern "C" int jfg_sched_extra_frame(void) {
    return g.frame_rate >= 1 ? 1 : 0;
}

extern "C" int jfg_refresh_override(void) {
    switch (g.frame_rate) {
        case 1: return 0;
        case 2: return 1;
        default: return -1;
    }
}

// Read by the renderer thread (src/renderer/renderer.cpp): 1 = render at the window's aspect ratio instead of 4:3.
// RT64 widens perspective projections only, so 2D (HUD, menus, text) stays in the middle 4:3 area. Only in the game
// proper (game mode 0, which includes its pause menus and cutscenes): the front end masks its 3D backdrops with
// 4:3 overlays (the title screen's oval), and the uncovered sides look broken.
extern "C" int jfg_widescreen_active(void) {
    static const bool env = std::getenv("JFG_WIDESCREEN") != nullptr; // for tests
    if (!(g.widescreen || env) || g_rdram == nullptr) return 0;
    return rd32(ADDR_mainGameMode) == 0 ? 1 : 0;
}

extern "C" int jfg_no_blood(void) {
    static const bool env = std::getenv("JFG_NO_BLOOD") != nullptr;
    return (g.no_blood || env) ? 1 : 0;
}

// --- dual-stick controls ----------------------------------------------------------------------------------------------
// A mod, off by default. The game's own layout is from 1999: the stick runs forward/back and turns the character, the
// camera stays behind it, the C buttons sidestep (left/right), jump (up) and crouch (down); with R held the stick moves
// the reticle and all four C buttons move the character.
//
// With this option the two sticks work as in a modern third-person game:
//   walking: the right stick swings the camera around the character (and up/down); the left stick runs in the pushed
//            direction as seen from the camera;
//   aiming (R held): the character turns to face where the camera was looking (or, as an option, the view snaps to the
//            way the character faces); the right stick aims, the left stick moves through the four C buttons.
//
// How it is done, without changing the game's code:
//   - src/input/controls.cpp hands both sticks to jfg_dual_stick() when it builds the N64 controller state;
//   - jfg_camera_controls() runs on the game thread right after the game has read the controller (hook after joyRead).
//     The game's own movement is already camera-relative - relative to its own camera, which swings round behind the
//     character - so the left stick is rotated by the angle between the camera the player sees and the game's own and
//     written back as the game's stick value; the game's movement code then turns and runs the character as usual;
//   - jfg_camera_override() runs right before the scene is drawn (hook at the trackDraw call): it moves the active
//     camera, which the game keeps behind the character, to the angle the right stick has chosen, keeping the game's
//     own distance and looking at the same point. The game's own camera values are put back at the start of the next
//     tick (jfg_debug_frame), so the game's camera logic never sees the change.
// Angle conventions, measured in the game (heading = atan2(x, z)): the character runs along heading yaw + 180 degrees;
// the camera looks along heading -yaw; camera pitch is positive looking down; screen-right is heading - 90 degrees.
bool in_cutscene();
// A Floyd mission (the player flies Floyd; `robotMission`, u8, is what the game itself checks to show the mission's
// pause menu). The control and camera options are built around the character, so they are all off during one.
constexpr uint32_t ADDR_robotMission = 0x800A3208;
inline bool control_mods_blocked() { return in_cutscene() || rd8(ADDR_robotMission) != 0; }
constexpr uint32_t ADDR_sControllerCurrData = 0x800FB0C0; // OSContPad[4]: u16 button, s8 stick_x, s8 stick_y, u8 errno (6 bytes)
constexpr uint32_t ADDR_cameraActorArray = 0x800FA4D0;   // Camera[], 0x4C bytes each (src/camera.h)
constexpr uint32_t ADDR_cameraNo = 0x800FA634;           // index of the active camera
constexpr uint32_t ADDR_delayDat = 0x800A3374;           // video frames the current tick simulates
constexpr uint32_t CAMERA_SIZE = 0x4C;

namespace {
constexpr float PI_F = 3.14159265f;
inline float angle_from_s16(uint16_t v) { return (float)(int16_t)v * (PI_F / 32768.0f); }
inline uint16_t angle_to_s16(float a) { return (uint16_t)(int32_t)std::lround(a * (32768.0f / PI_F)); }
inline float wrap_angle(float a) { while (a > PI_F) a -= 2 * PI_F; while (a < -PI_F) a += 2 * PI_F; return a; }
inline float rdf(uint32_t addr) { const uint32_t v = rd32(addr); float f; std::memcpy(&f, &v, 4); return f; }
inline void wrf(uint32_t addr, float f) { uint32_t v; std::memcpy(&v, &f, 4); wr32(addr, v); }

// Sticks as last read by the input thread (jfg_dual_stick), up/right positive.
std::atomic<float> g_stick_lx{ 0 }, g_stick_ly{ 0 }, g_stick_rx{ 0 }, g_stick_ry{ 0 };
// "Jump while aiming" (aim_jump): the jump button as held in the dual-stick input, and the jump under way.
std::atomic<bool> g_jump_button{ false };     // dual-stick input: the jump button is held (input thread)
std::atomic<bool> g_aim_jump_active{ false }; // a jump from aiming is under way: R is held back (both threads)

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
    // Arriving somewhere new (another room through a door, a respawn, a teleport): the free camera starts again
    // behind the character, looking the way it faces, instead of keeping the heading from the place it came from.
    bool face_reset = true;
    int level = -1;
    uint32_t last_player = 0;
    float last_x = 0, last_z = 0;
    int showing = 0;            // ticks the game's camera has been pointed away from the character (a camera "shot")
} g_cam;


// Boss fights: the player's boss control (boyBossControl / dogBossControl / girlBossControl) runs in place of the
// normal one; jfg_boss_tick (hook at their entry) notes the tick. See boss_controls().
uint32_t g_tick_count = 0;     // jfg_camera_controls calls
uint32_t g_boss_seen_tick = 0; // g_tick_count when the boss control last ran
bool boss_control_active() { return g_boss_seen_tick != 0 && g_tick_count - g_boss_seen_tick <= 2; }
// Boss fights with the mouse: how far the camera is turned in place (16-bit angle units: the aim's yaw offset beyond the box, same sign),
// set by boss_controls and applied by jfg_camera_override. 0 when not in use.
int g_boss_cam_turn = 0;

bool dual_stick_wanted() {
    static const bool env = std::getenv("JFG_DUAL_STICK") != nullptr; // for tests
    return g.dual_stick || g.mouse_keyboard || env;
}

// Mouse movement since the game thread last took it (input.cpp adds every SDL_MOUSEMOTION while the mouse is captured).
std::atomic<int> g_mouse_dx{ 0 }, g_mouse_dy{ 0 };
// Mouse per tick, in radians of camera turn / 16-bit angle units of aim, taken by jfg_camera_controls this tick.
float g_mouse_cam_x = 0, g_mouse_cam_y = 0;
int g_mouse_aim_x = 0, g_mouse_aim_y = 0;
constexpr float MOUSE_RADIANS_PER_COUNT = 0.0025f; // about 0.14 degrees per mouse count at 100%

// Handedness: on screen, "right" is the look heading minus 90 degrees, so turning right lowers the heading (measured:
// with these signs the character runs where the stick points and the view turns the way the stick is pushed).
// JFG_DUAL_STICK_SIGNS="move,camera" overrides them for tests.
float g_sign_move = -1.0f, g_sign_cam = -1.0f;
} // namespace

// Levels played with the game's own controls even with dual stick / mouse and keyboard on: the Mizar race (the ship
// steers with the stick, which the dual-stick camera and movement would take over) and the multiplayer races.
bool level_uses_original_controls(int level) {
    return level == 268 /* Mizar Racer */ || level == 258 /* Multi Race */ || level == 397 /* Multi Race II */;
}

extern "C" int jfg_dual_stick_active(void) {
    if (g_fly.active && g_rdram != nullptr) return 1; // the fly camera reads both sticks / WASD and the mouse
    if (!dual_stick_wanted() || g_rdram == nullptr) return 0;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || control_mods_blocked()) return 0;
    if (level_uses_original_controls((int16_t)rd16(ADDR_currentLevel))) return 0;
    return 1;
}

// 1 when dual-stick mode uses its own controller layout (src/input/controls.cpp) instead of the controller bindings.
extern "C" int jfg_dual_stick_layout(void) {
    return g.dual_layout ? 1 : 0;
}

// Mouse and keyboard (src/input/controls.cpp, src/input/input.cpp): 1 while the mouse and keyboard play the game -
// the option is on, the dual-stick controls are in charge (gameplay, not paused, no cutscene) and the debug UI is
// closed (or F10's toolbar is open but handed the mouse and keyboard to the game with Alt / a click outside it).
// The cursor is captured (relative mouse mode) exactly then.
extern "C" int jfg_mouse_keyboard_active(void) {
    return (g.mouse_keyboard || g_fly.active) && jfg_dual_stick_active() && dino::debug_ui::game_has_input() ? 1 : 0;
}

std::atomic<int> g_mouse_wheel{ 0 };
// Wheel notches (up positive) while mouse and keyboard play; jfg_camera_controls turns them into weapon changes.
extern "C" void jfg_mouse_wheel(int notches) {
    if (jfg_mouse_keyboard_active()) g_mouse_wheel.fetch_add(notches, std::memory_order_relaxed);
}

extern "C" void jfg_mouse_motion(int dx, int dy) {
    g_mouse_dx.fetch_add(dx, std::memory_order_relaxed);
    g_mouse_dy.fetch_add(dy, std::memory_order_relaxed);
}

// Normal controls: 1 when the stick's up/down has to be flipped because the player is aiming (R held) and asked for it.
extern "C" int jfg_invert_aim_normal(uint16_t buttons) {
    if (!g.invert_aim_normal || dual_stick_wanted() || g_rdram == nullptr || (buttons & 0x0010) == 0) return 0;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || control_mods_blocked()) return 0;
    return 1;
}

// Input thread. buttons, x, y: the N64 buttons and the (left) stick as bound, in and out. right_x/right_y: right stick,
// up positive. While walking the stick is passed on unchanged; jfg_camera_controls() replaces it on the game thread.
extern "C" void jfg_dual_stick(uint16_t* buttons, float* x, float* y, float right_x, float right_y) {
    constexpr uint16_t BTN_Z = 0x2000, BTN_R = 0x0010, C_UP = 0x0008, C_DOWN = 0x0004, C_LEFT = 0x0002, C_RIGHT = 0x0001;
    constexpr float DIGITAL = 0.5f; // stick deflection that counts as a C button press
    uint16_t b = *buttons;
    if (g.swap_triggers) {
        const bool z = (b & BTN_Z) != 0, r = (b & BTN_R) != 0;
        b = (uint16_t)((b & ~(BTN_Z | BTN_R)) | (z ? BTN_R : 0) | (r ? BTN_Z : 0));
    }
    // Tests without a controller: JFG_DUAL_STICK_TEST names a text file holding "lx,ly,rx,ry", read on every poll.
    static const char* const test_file = std::getenv("JFG_DUAL_STICK_TEST");
    if (test_file != nullptr) {
        if (FILE* f = std::fopen(test_file, "r")) {
            float v[4];
            if (std::fscanf(f, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4) { *x = v[0]; *y = v[1]; right_x = v[2]; right_y = v[3]; }
            std::fclose(f);
        }
    }
    const float lx = *x, ly = *y;
    g_stick_lx = lx; g_stick_ly = ly; g_stick_rx = right_x; g_stick_ry = right_y;
    g_jump_button.store((b & C_UP) != 0, std::memory_order_relaxed); // the jump button itself, before the stick adds C-up
    if (g_aim_jump_active.load(std::memory_order_relaxed)) b &= (uint16_t)~BTN_R; // jumping from aiming (aim_jump)
    if ((b & BTN_R) != 0) {
        if (lx < -DIGITAL) b |= C_LEFT;
        if (lx > DIGITAL) b |= C_RIGHT;
        if (ly > DIGITAL) b |= C_UP;
        if (ly < -DIGITAL) b |= C_DOWN;
        // D-pad up/down while aiming: zoom in/out (C-up/C-down) instead of changing weapon.
        constexpr uint16_t D_UP = 0x0800, D_DOWN = 0x0400;
        if ((b & D_UP) != 0) b = (uint16_t)((b & ~D_UP) | C_UP);
        if ((b & D_DOWN) != 0) b = (uint16_t)((b & ~D_DOWN) | C_DOWN);
        *x = right_x;
        // The game itself aims like a flight stick (stick up = look down). Here up looks up unless "invert" is set.
        *y = g.invert_aim_y ? right_y : -right_y;
    }
    *buttons = b;
}

// Aiming. The character control data (object + 0x68) holds, as 16-bit angles: the direction the body faces (+0x11C,
// copy +0x106, and the object's yaw) and the view pitch (+0x1E2, limited to 60 degrees); the reticle's offset from
// them (yaw +0x1CE, copies +0x1DC and +0x246; pitch +0x1E0); and their sums, the aim direction (yaw +0x1C6/+0x1CA;
// pitch +0x1C8/+0x1CC/+0x1D0/+0x1DE). The byte at +0x568 is 0x0B while the character is in its aiming state.
constexpr uint32_t PD_BODY_YAW = 0x11C, PD_BODY_YAW_COPY = 0x106, PD_VIEW_PITCH = 0x1E2, PD_RETICLE_YAW = 0x1CE, PD_RETICLE_PITCH = 0x1E0, PD_STATE = 0x568;
constexpr uint8_t PD_STATE_AIMING = 0x0B;

static bool aim_state(uint32_t obj) {
    const uint32_t data = rd32(obj + 0x68);
    return valid_ptr(data) && rd8(data + PD_STATE) == PD_STATE_AIMING;
}

// "Jump while aiming" option. In the game, C-up while aiming (R held) zooms the sniper scope / steps forward, and
// the character can only jump from its normal state. With the option a press of the jump button while aiming is
// turned into the game's own jump: one tick with R let go (the aiming state ends the way it does when R is
// released), then one tick with C-up pressed (the normal state starts its jump, with its animation and sound). R stays
// let go until the jump is over, so the left stick steers the jump like a normal one (a running jump keeps its
// momentum) and the camera follows as when walking; with R still held the character aims again on landing.
// The jump button: in the dual-stick input C-up as bound / laid out (A, Space), not the C-up the left stick and the
// D-pad give while aiming (walking forward, zoom) - jfg_dual_stick() records it; otherwise the N64 C-up itself.
// Edits this tick's controller data and pressed-buttons word, after joyRead, like the mouse wheel does.

static void aim_jump(uint32_t obj, uint32_t pad, uint32_t pressed) {
    constexpr uint16_t BTN_R = 0x0010, C_UP = 0x0008;
    constexpr int STATE_JUMP = 3;
    static int phase = 0, ticks = 0;
    static bool jump_was_held = false;
    const bool jump_held = g_jump_button.load(std::memory_order_relaxed);
    const bool jump_edge = jump_held && !jump_was_held;
    jump_was_held = jump_held;
    auto finish = [&]() { phase = 0; g_aim_jump_active.store(false, std::memory_order_relaxed); };
    if (!g.aim_jump) { finish(); return; }

    const uint16_t cur = rd16(pad), down = rd16(pressed);
    if (phase == 0) {
        const bool press = jfg_dual_stick_active() ? jump_edge : (down & C_UP) != 0;
        if (!aim_state(obj) || !press) return;
        phase = 1;
        g_aim_jump_active.store(true, std::memory_order_relaxed);
        wr16(pad, (uint16_t)(cur & ~(BTN_R | C_UP)));   // leave the aiming state
        wr16(pressed, (uint16_t)(down & ~(BTN_R | C_UP)));
        return;
    }

    if (phase == 1) {                                   // jump from the normal state
        wr16(pad, (uint16_t)((cur & ~BTN_R) | C_UP));
        wr16(pressed, (uint16_t)((down & ~BTN_R) | C_UP));
        phase = 2;
        ticks = 0;
        return;
    }

    // In the air: keep R let go until the jump state ends (or the jump never started).
    const uint32_t data = rd32(obj + 0x68);
    const int state = valid_ptr(data) ? rd8(data + PD_STATE) : -1;
    ticks++;
    if ((state != STATE_JUMP && ticks > 2) || ticks > 300) { finish(); return; }
    wr16(pad, (uint16_t)(cur & ~BTN_R));
    wr16(pressed, (uint16_t)(down & ~BTN_R));
}

// Applies the aim speed, and the "keep the reticle centered" option, to the stick the game is about to read.
//
// In the game's own aiming the stick first moves the reticle inside a box in the middle of the screen, the view only
// starts to turn once the reticle reaches the edge of the box, and the reticle drifts back to the middle when the
// stick is let go - taking the aim with it. Without the option only the stick is scaled, which slows both.
// With the option the game is given a centered stick, so its reticle offset stays at zero, and the stick turns the
// body direction and the view pitch directly, at a steady rate.
static void aim_controls(uint32_t obj, uint32_t pad, float frames) {
    const uint32_t data = rd32(obj + 0x68);
    const float speed = (float)g.aim_speed / 100.0f;
    const int raw_x = (int8_t)rd8(pad + 2), raw_y = (int8_t)rd8(pad + 3);
    if (!g.aim_centered && !jfg_mouse_keyboard_active()) { // the mouse always turns the view directly
        if (g.aim_speed != 100) {
            wr8(pad + 2, (uint8_t)(int8_t)std::clamp((int)std::lround(raw_x * speed), -80, 80));
            wr8(pad + 3, (uint8_t)(int8_t)std::clamp((int)std::lround(raw_y * speed), -80, 80));
        }
        return;
    }
    auto axis = [](int v) { // the game's own dead zone and range (joyClamp)
        const float a = (std::abs(v) - 6) / 64.0f;
        return a <= 0.0f ? 0.0f : std::min(a, 1.0f) * (v < 0 ? -1.0f : 1.0f);
    };
    // Full stick at 100%: 110 degrees a second sideways, 80 up/down. Stick right lowers the yaw; stick up lowers the
    // pitch value (which is the game's "push up to look down").
    constexpr float YAW_RATE = 110.0f / 60.0f * (32768.0f / 180.0f), PITCH_RATE = 80.0f / 60.0f * (32768.0f / 180.0f);
    constexpr int PITCH_LIMIT = 0x2AAA;
    // The mouse turns the view directly, by how far it was moved (already scaled, see jfg_camera_controls).
    const int yaw_step = (int)std::lround(axis(raw_x) * YAW_RATE * frames * speed) + g_mouse_aim_x;
    const int pitch_step = (int)std::lround(axis(raw_y) * PITCH_RATE * frames * speed) + g_mouse_aim_y;
    // Whatever offset the game still holds (from before the option was switched on) goes into the view first.
    const uint16_t body = (uint16_t)(rd16(data + PD_BODY_YAW) + (int16_t)rd16(data + PD_RETICLE_YAW) - yaw_step);
    wr16(obj, body);
    wr16(data + PD_BODY_YAW_COPY, body);
    wr16(data + PD_BODY_YAW, body);
    for (uint32_t k : { PD_RETICLE_YAW, 0x1DCu, 0x246u }) wr16(data + k, 0);
    const int view = std::clamp((int)(int16_t)rd16(data + PD_VIEW_PITCH) + (int)(int16_t)rd16(data + PD_RETICLE_PITCH) - pitch_step, -PITCH_LIMIT, PITCH_LIMIT);
    wr16(data + PD_VIEW_PITCH, (uint16_t)(int16_t)view);
    wr16(data + PD_RETICLE_PITCH, 0);
    wr8(pad + 2, 0);
    wr8(pad + 3, 0);
}

// --- fly camera ------------------------------------------------------------------------------------------------------
// Game thread, right after joyRead (from jfg_camera_controls). Returns true while the fly camera is on: it then owns
// this tick's input. Left stick / WASD: fly forward, back and sideways in the direction the camera looks; right stick
// / mouse: look; C-up (A, Space) up, C-down (B, Ctrl, C) down; holding fire (RT, left mouse button) flies 4x faster.
// The player character gets an empty controller meanwhile. jfg_camera_override puts the camera where it is.
bool fly_camera_tick(recomp_context* ctx) {
    // The HUD: the game's own "hide instruments" flag (frontSetInstrumentsHide, 0x800FF310), put back afterwards.
    constexpr uint32_t ADDR_instrumentsHide = 0x800FF310;
    static bool hud_hidden = false;
    static uint8_t hud_before = 0;
    auto show_hud = [&]() { if (hud_hidden) { wr8(ADDR_instrumentsHide, hud_before); hud_hidden = false; } };
    if (g_fly_toggle_request.exchange(false)) {
        g_fly.active = !g_fly.active;
        g_fly.started = false;
        std::fprintf(stderr, "[jfg] fly camera %s\n", g_fly.active ? "on" : "off");
    }
    if (!g_fly.active) { show_hud(); return false; }
    if (g_rdram == nullptr || rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || player_object(ctx) == 0) {
        // left the game proper (front end, pause menu, loading): the fly camera switches off
        g_fly.active = false;
        g_fly.started = false;
        show_hud();
        std::fprintf(stderr, "[jfg] fly camera off (not in gameplay)\n");
        return false;
    }
    if (g.fly_hide_hud) {
        if (!hud_hidden) { hud_before = rd8(ADDR_instrumentsHide); hud_hidden = true; }
        wr8(ADDR_instrumentsHide, 1);
    }
    else show_hud();
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    if (!g_fly.started) {
        g_fly.started = true;
        g_fly.x = rdf(cam + 0x0C); g_fly.y = rdf(cam + 0x10); g_fly.z = rdf(cam + 0x14);
        g_fly.heading = -angle_from_s16(rd16(cam));
        g_fly.pitch = angle_from_s16(rd16(cam + 2));
    }
    const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
    const uint32_t pad = ADDR_sControllerCurrData + 6u * i;
    const uint16_t buttons = rd16(pad);
    // The character stands still: nothing reaches the game this tick.
    wr16(pad, 0); wr8(pad + 2, 0); wr8(pad + 3, 0);
    wr16(ADDR_buttonsPressed + 2u * i, 0);

    const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    const float lx = g_stick_lx, ly = g_stick_ly, rx = g_stick_rx, ry = g_stick_ry;
    constexpr float YAW_RATE = 120.0f / 60.0f * (PI_F / 180.0f), PITCH_RATE = 90.0f / 60.0f * (PI_F / 180.0f);
    g_fly.heading = wrap_angle(g_fly.heading + g_sign_cam * (rx * YAW_RATE * frames * (float)g.camera_speed_h / 100.0f + g_mouse_cam_x));
    g_fly.pitch = std::clamp(g_fly.pitch + (g.invert_camera_y ? -ry : ry) * -PITCH_RATE * frames * (float)g.camera_speed_v / 100.0f - g_mouse_cam_y,
                             -1.45f, 1.45f);
    float speed = 5.0f * frames * (float)g.fly_speed / 100.0f;
    if (buttons & 0x2000) speed *= 4.0f; // fire held: fast
    const float ch = std::cos(g_fly.heading), sh = std::sin(g_fly.heading), cp = std::cos(g_fly.pitch), sp = std::sin(g_fly.pitch);
    const float up = ((buttons & 0x0008) ? 1.0f : 0.0f) - ((buttons & 0x0004) ? 1.0f : 0.0f);
    auto dz = [](float v) { return std::fabs(v) < 0.12f ? 0.0f : v; };
    const float f = dz(ly), r = dz(lx) * -g_sign_move; // stick right: towards screen right
    g_fly.x += (sh * cp * f - ch * r) * speed;
    g_fly.y += (-sp * f + up) * speed;
    g_fly.z += (ch * cp * f + sh * r) * speed;
    static const bool log = std::getenv("JFG_DEBUG_PLAYER") != nullptr;
    if (log && (g_tick_count % 10) == 0) std::fprintf(stderr, "[fly] pos %.0f %.0f %.0f heading %.0f pitch %.0f stick %.2f %.2f buttons %04X\n", g_fly.x, g_fly.y, g_fly.z, g_fly.heading * 180 / PI_F, g_fly.pitch * 180 / PI_F, lx, ly, buttons);
    return true;
}

// Hook in func_80045F00 (0x80046058, where the game decides between running the world and its pause tick): 1 makes
// this tick a pause tick (objects drawn but not moved) without the pause menu. Used by the fly camera's "freeze".
extern "C" int jfg_freeze_world(void) {
    return g_fly.active && g.fly_freeze ? 1 : 0;
}

// --- boss fights -----------------------------------------------------------------------------------------------------
extern "C" void jfg_boss_tick(void) {
    g_boss_seen_tick = g_tick_count;
}

// --- damage taken ----------------------------------------------------------------------------------------------------
// The character on screen keeps its health in its object's damage data (object + 0x4C, +6; 256 per unit). Whatever
// lowered it since the last tick (enemy fire, falls, explosions) is scaled here, before the game's own death check in
// the player's control code later in this tick. Rises (pickups, respawns) are left alone.
void apply_damage_taken(uint32_t obj) {
    static uint32_t last_obj = 0;
    static int last_health = -1;
    const uint32_t hit = obj != 0 ? rd32(obj + 0x4C) : 0;
    if (!valid_ptr(hit)) { last_obj = 0; last_health = -1; return; }
    int health = (int16_t)rd16(hit + 6);
    if (obj == last_obj && last_health >= 0 && health < last_health && g.damage_taken != 100 && !g.invulnerable) {
        const int lost = last_health - health;
        const int scaled = (int)std::lround(lost * (double)g.damage_taken / 100.0);
        health = std::max(-0x7FFF, last_health - scaled);
        wr16(hit + 6, (uint16_t)(int16_t)health);
    }
    last_obj = obj;
    last_health = health;
}

// --- jetpack refuelling ----------------------------------------------------------------------------------------------
// Hook in refuelControl (overlay 100): the fuel a pad adds this tick.
extern "C" int jfg_refuel_amount(int amount) {
    if (g.refuel_speed == 100 || amount <= 0) return amount;
    return std::max(1, (int)std::lround(amount * (double)g.refuel_speed / 100.0));
}

// --- sniper zoom -----------------------------------------------------------------------------------------------------
// Hook in controlGetManualAim where it stores the zoom (1 = none .. 5): with the option the zoom is fixed.
extern "C" float jfg_sniper_zoom(float zoom) {
    if (!g.sniper_fixed_zoom) return zoom;
    return std::clamp((float)g.sniper_zoom / 100.0f, 1.0f, 5.0f);
}

// Hook in the characters' aim control (overlays 15-17), where it skips walking with the sniper rifle because the
// walk keys also zoom its scope. With the fixed zoom the weapon number is hidden there, so the sniper walks too.
extern "C" int jfg_sniper_walk(int weapon) {
    return (weapon == 7 && g.sniper_fixed_zoom) ? -1 : weapon;
}

// --- skip the Tribal check -------------------------------------------------------------------------------------------
// Hook at the entry of mainGetGameFlag: 1 = report the flag as set.
extern "C" int jfg_flag_override(int flag) {
    return g.skip_tribal_check && flag == 88 ? 1 : 0;
}

// --- moving platforms ------------------------------------------------------------------------------------------------
bool g_platform_move = false; // controlPlatform is carrying the player (its objMoveXYZ call), see jfg_move_scale
extern "C" void jfg_platform_move(int on) {
    g_platform_move = on != 0;
}

// Boss fights with the dual-stick / mouse and keyboard controls. In a boss fight the game runs a control of its own
// (boyBossControl / girlBossControl / dogBossControl, character state 32): the camera stays on the boss, the stick
// turns the character and aims (up/down: the aim's pitch), and C-left / C-right step round the arena. Measured in the
// Lurg fight (level 195): stick right turned the character and its reticle, C-left moved it 350 units sideways, the
// stick alone never moved it. The free camera and the camera-relative stick are off here; instead
//   left stick / A, D: step left and right (C-left / C-right), forward/back keep their buttons (jump, crouch);
//   right stick: the game's stick (turn and aim), up looks up unless "Invert Aim Up/Down" is set;
//   mouse: moves the aim directly, not through the stick; the body never turns (a rail).
void boss_controls(uint32_t obj, uint32_t pad, bool aiming) {
    (void)aiming;
    constexpr uint16_t C_LEFT = 0x0002, C_RIGHT = 0x0001;
    const float lx = g_stick_lx, rx = g_stick_rx, ry = g_stick_ry;
    uint16_t buttons = rd16(pad);
    buttons = (uint16_t)(buttons & ~(C_LEFT | C_RIGHT));
    if (lx < -0.4f) buttons |= C_LEFT;
    if (lx > 0.4f) buttons |= C_RIGHT;
    const uint32_t pressed = ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u);
    static uint16_t last = 0; // new C presses count as pressed this tick, as a real press would
    wr16(pressed, (uint16_t)((rd16(pressed) & ~(C_LEFT | C_RIGHT)) | (buttons & ~last & (C_LEFT | C_RIGHT))));
    last = buttons;
    wr16(pad, buttons);
    // Aim: the right stick and the mouse both move the aim directly (below); the game's own stick is left centred.
    // The mouse does not go through the stick: its movement comes in lumps (none on one tick, a lot on the next), and
    // as a stick deflection that made the reticle jump about and drift back towards the middle of the screen, so the
    // aim could not be held. It moves the aim directly, by exactly how far the mouse moved. The right stick is
    // treated the same way (at the rates of the "Keep the reticle centered" aiming, scaled by the aim speed), so the
    // controller works like the mouse here.
    //
    // In a boss fight the character is on a rail: it faces the boss, never turns, and only steps left and right
    // (C-left / C-right move it sideways relative to where it faces). So the mouse never turns the body - that is held
    // at the direction it had when the fight began (taken again whenever the boss control starts running anew). Left
    // and right move the reticle's offset from that direction instead, up and down tilt the view.
    constexpr int RETICLE_BOX = 0x1C71;   // 40 degrees: the aim may go this far to either side before the camera turns with it
    constexpr int RETICLE_LIMIT = 0x5555; // 120 degrees either side, as 16-bit angle units (some bosses move round the arena)
    constexpr int PITCH_LIMIT = 0x2AAA;
    static uint32_t last_obj = 0, last_tick = 0;
    static uint16_t centre = 0;
    static int reticle = 0;
    const uint32_t data = rd32(obj + 0x68);
    if (valid_ptr(data)) {
        if (obj != last_obj || g_tick_count - last_tick > 3) { last_obj = obj; centre = rd16(obj); reticle = 0; }
        // Right stick as aim movement, with the game's own dead zone and range (joyClamp), as in aim_controls.
        auto axis = [](float v) {
            const float a = (std::abs(v) * 78.0f - 6.0f) / 64.0f;
            return a <= 0.0f ? 0.0f : std::min(a, 1.0f) * (v < 0 ? -1.0f : 1.0f);
        };
        constexpr float YAW_RATE = 110.0f / 60.0f * (32768.0f / 180.0f), PITCH_RATE = 80.0f / 60.0f * (32768.0f / 180.0f);
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6), speed = (float)g.aim_speed / 100.0f;
        const float stick_x = std::clamp(rx, -1.0f, 1.0f), stick_y = std::clamp(ry, -1.0f, 1.0f);
        const int yaw_step = (int)std::lround(axis(stick_x) * YAW_RATE * frames * speed) + g_mouse_aim_x;
        // stick up = look up unless "Invert Aim Up/Down" is set; the view pitch value goes down to look up
        const int pitch_step = (int)std::lround(axis(g.invert_aim_y ? stick_y : -stick_y) * PITCH_RATE * frames * speed) + g_mouse_aim_y;
        reticle = std::clamp(reticle - yaw_step, -RETICLE_LIMIT, RETICLE_LIMIT);
        wr16(obj, centre);
        wr16(data + PD_BODY_YAW_COPY, centre);
        wr16(data + PD_BODY_YAW, centre);
        for (uint32_t k : { PD_RETICLE_YAW, 0x1DCu, 0x246u }) wr16(data + k, (uint16_t)(int16_t)reticle);
        const int view = std::clamp((int)(int16_t)rd16(data + PD_VIEW_PITCH) + (int)(int16_t)rd16(data + PD_RETICLE_PITCH) - pitch_step, -PITCH_LIMIT, PITCH_LIMIT);
        wr16(data + PD_VIEW_PITCH, (uint16_t)(int16_t)view);
        wr16(data + PD_RETICLE_PITCH, 0);
        wr8(pad + 2, 0);
        wr8(pad + 3, 0);
        // Past the box the camera turns on the spot, so the boss can be followed to the sides of the arena. It turns
        // about the player's position, never moving the player.
        // With "Keep the reticle centered" and a controller the camera follows the whole aim, so the reticle stays in
        // the middle of the screen.
        const int box = (!g.aim_centered || jfg_mouse_keyboard_active() != 0) ? RETICLE_BOX : 0;
        g_boss_cam_turn = reticle > box ? reticle - box : reticle < -box ? reticle + box : 0;
    }
    else {
        g_boss_cam_turn = 0;
    }
    last_tick = g_tick_count;
}

// Tests: JFG_DEBUG_PLAYER=<ticks> logs the player's state every that many ticks.
void debug_player_log(uint32_t obj) {
    {
        // Tests: JFG_DEBUG_HURT="secs,amount" takes that much health once (scaled by "Damage taken" the next tick).
        static const char* const hurt = std::getenv("JFG_DEBUG_HURT");
        static const auto start = std::chrono::steady_clock::now();
        static bool done = false;
        float t = 0; int amount = 0;
        if (hurt != nullptr && !done && obj != 0 && std::sscanf(hurt, "%f,%d", &t, &amount) == 2 &&
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() >= t) {
            done = true;
            const uint32_t hit = rd32(obj + 0x4C);
            if (valid_ptr(hit)) {
                std::fprintf(stderr, "[player] hurt: health %d - %d\n", (int)(int16_t)rd16(hit + 6), amount);
                wr16(hit + 6, (uint16_t)(int16_t)((int16_t)rd16(hit + 6) - amount));
            }
        }
    }
    static const int every = std::getenv("JFG_DEBUG_PLAYER") ? std::max(1, std::atoi(std::getenv("JFG_DEBUG_PLAYER"))) : 0;
    if (every == 0 || obj == 0 || (g_tick_count % (uint32_t)every) != 0) return;
    const uint32_t data = rd32(obj + 0x68), hit = rd32(obj + 0x4C);
    const uint32_t pad = ADDR_sControllerCurrData + 6u * (rd8(ADDR_sPlayerID) & 3u);
    std::fprintf(stderr, "[player] tick %u level %d boss %d state %d anim %d pos %.1f %.1f %.1f yaw %d pad %04X %d %d hp %d dyaw %d rpitch %d vpitch %d 577 %d zoom %.2f fuel %d\n",
        g_tick_count, (int)(int16_t)rd16(ADDR_currentLevel), int(boss_control_active()), valid_ptr(data) ? rd8(data + PD_STATE) : -1,
        (int)(int8_t)rd8(obj + 0x3B), rdf(obj + 0x0C), rdf(obj + 0x10), rdf(obj + 0x14), (int)(int16_t)rd16(obj),
        rd16(pad), (int)(int8_t)rd8(pad + 2), (int)(int8_t)rd8(pad + 3), valid_ptr(hit) ? (int)(int16_t)rd16(hit + 6) : -1,
        valid_ptr(data) ? (int)(int16_t)rd16(data + PD_RETICLE_YAW) : 0, valid_ptr(data) ? (int)(int16_t)rd16(data + PD_RETICLE_PITCH) : 0,
        valid_ptr(data) ? (int)(int16_t)rd16(data + PD_VIEW_PITCH) : 0, valid_ptr(data) ? (int)(int8_t)rd8(data + 0x577) : 0,
        valid_ptr(data) ? rdf(data + 0x1EC) : 0.0f, valid_ptr(data) ? (int)(int16_t)rd16(data + 0x58C) : 0);
}

// Game thread, right after joyRead. Decides whether the right stick owns the camera this tick, advances the camera
// angles, turns the character and rewrites the game's stick value.
void jfg_camera_controls(recomp_context* ctx) {
    static bool signs_read = false;
    if (!signs_read) {
        signs_read = true;
        if (const char* e = std::getenv("JFG_DUAL_STICK_SIGNS")) std::sscanf(e, "%f,%f", &g_sign_move, &g_sign_cam);
    }
    const bool was_active = g_cam.active;
    g_cam.active = false;
    g_cam.shoulder = false;
    {
        // Mouse movement for this tick (always taken, so nothing piles up in menus).
        const int mx = g_mouse_dx.exchange(0, std::memory_order_relaxed), my = g_mouse_dy.exchange(0, std::memory_order_relaxed);
        const bool mouse = jfg_mouse_keyboard_active() != 0;
        const float cam = MOUSE_RADIANS_PER_COUNT * (float)g.mouse_camera_speed / 100.0f;
        const float aim = MOUSE_RADIANS_PER_COUNT * (32768.0f / PI_F) * (float)g.mouse_aim_speed / 100.0f;
        const float up = g.invert_mouse_y ? -1.0f : 1.0f; // mouse forward (SDL: negative y) looks up
        g_mouse_cam_x = mouse ? (float)mx * cam : 0.0f;
        g_mouse_cam_y = mouse ? -(float)my * cam * up : 0.0f;
        // aim: right lowers the yaw like the stick; forward = the stick pushed up, which (not inverted) lowers the step
        g_mouse_aim_x = mouse ? (int)std::lround((float)mx * aim) : 0;
        g_mouse_aim_y = mouse ? (int)std::lround((float)my * aim * up) : 0;
    }
    g_tick_count++;
    if (fly_camera_tick(ctx)) { g_cam.player = 0; g_cam.was_aiming = false; return; }
    const bool gameplay = g_rdram != nullptr && rd32(ADDR_mainGameMode) == 0 && rd8(ADDR_pauseMode) == 0 && !control_mods_blocked() &&
        !level_uses_original_controls((int16_t)rd16(ADDR_currentLevel)); // races: no control changes at all
    const uint32_t obj = gameplay ? player_object(ctx) : 0;
    g_cam.player = obj;
    apply_damage_taken(obj);
    debug_player_log(obj);
    g_cam.move_factor = (float)g.move_speed / 100.0f;
    {
        // Mouse wheel: each notch is one press of the game's own weapon buttons (N64 A: next, B: previous - what RB /
        // LB give in the dual-stick layout), so the switch has its sound and the weapon list scrolls on screen. The
        // press is put straight into this tick's controller data (after joyRead, as a real press would arrive), one
        // per tick: a flick of several notches steps through several weapons in as many ticks. Nothing is kept
        // waiting - at most 2 further notches are remembered, and turning the wheel the other way drops them.
        static int wheel_pending = 0;
        const int notches = g_mouse_wheel.exchange(0, std::memory_order_relaxed);
        if (notches != 0) {
            if ((notches > 0) != (wheel_pending > 0)) wheel_pending = 0;
            wheel_pending = std::clamp(wheel_pending + notches, -3, 3);
        }
        if (wheel_pending != 0 && (obj == 0 || !jfg_mouse_keyboard_active())) wheel_pending = 0;
        if (wheel_pending != 0) {
            const uint16_t button = wheel_pending < 0 ? 0x8000 : 0x4000; // wheel down: A (next), up: B (previous)
            wheel_pending += wheel_pending < 0 ? 1 : -1;
            const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
            const uint32_t pad = ADDR_sControllerCurrData + 6u * i;
            wr16(pad, (uint16_t)(rd16(pad) | button));
            wr16(ADDR_buttonsPressed + 2u * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2u * i) | button));
            static const bool log = std::getenv("JFG_DEBUG_WHEEL") != nullptr;
            const int c = (int16_t)rd16(ADDR_currentCharacter);
            const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
            if (log) std::fprintf(stderr, "[jfg] wheel: press %s (weapon %d, %d more)\n", button == 0x8000 ? "A" : "B", ch ? rd8(ch + CHAR_WEAPON_OFFSET) : -1, wheel_pending);
        }
    }
    if (obj == 0) { g_cam.was_aiming = false; return; }
    const uint32_t pad = ADDR_sControllerCurrData + 6u * (rd8(ADDR_sPlayerID) & 3u);
    aim_jump(obj, pad, ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u));
    const bool aiming = (rd16(pad) & 0x0010) != 0;

    // The aiming options work with any control layout.
    const float tick_frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    if (aim_state(obj)) {
        g_cam.shoulder = g.aim_shoulder;
        g_cam.shoulder_blend = g_cam.shoulder ? std::min(1.0f, g_cam.shoulder_blend + tick_frames / 6.0f) : 0.0f;
        aim_controls(obj, pad, tick_frames);
    }
    else {
        g_cam.shoulder_blend = 0.0f;
    }
    {
        const int level = (int16_t)rd16(ADDR_currentLevel);
        const float x = rdf(obj + 0x0C), z = rdf(obj + 0x14);
        const float moved = std::sqrt((x - g_cam.last_x) * (x - g_cam.last_x) + (z - g_cam.last_z) * (z - g_cam.last_z));
        if (level != g_cam.level || obj != g_cam.last_player || moved > 400.0f) g_cam.face_reset = true;
        g_cam.level = level; g_cam.last_player = obj; g_cam.last_x = x; g_cam.last_z = z;
    }
    if (!jfg_dual_stick_active()) { g_cam.was_aiming = false; return; }
    if (boss_control_active()) { boss_controls(obj, pad, aiming); g_cam.was_aiming = false; return; }
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float game_heading = -angle_from_s16(rd16(cam)); // where the game's own camera looks

    // The game sometimes takes the camera to show something - the door that opens once a room has been cleared, for
    // instance. Its follow camera always looks towards the character; a camera that is pointed well away from it (or
    // has been moved far off) is such a shot, and the right stick leaves the camera to the game until it is back.
    // The left stick goes to the game unchanged meanwhile, and the free camera restarts from the game's afterwards.
    {
        const float px = rdf(obj + 0x0C), pz = rdf(obj + 0x14), cx = rdf(cam + 0x0C), cz = rdf(cam + 0x14);
        const float flat = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
        const float off = flat < 30.0f ? 0.0f : std::fabs(wrap_angle(std::atan2(px - cx, pz - cz) - game_heading));
        const bool away = flat > 2000.0f || off > 50.0f * PI_F / 180.0f;
        // a couple of ticks of grace, so a single odd frame (a wall pushing the camera in) does not let go
        g_cam.showing = away ? g_cam.showing + 1 : 0;
        static const bool log_cam = std::getenv("JFG_DEBUG_CAM") != nullptr;
        if (log_cam) {
            std::fprintf(stderr, "[cam] level %d flat %.0f off %.1f showing %d game %.1f facing %.1f free %.1f\n", g_cam.level, flat,
                         off * 180.0f / PI_F, g_cam.showing, game_heading * 180.0f / PI_F,
                         wrap_angle(angle_from_s16(rd16(obj)) + PI_F) * 180.0f / PI_F, g_cam.heading * 180.0f / PI_F);
        }
        if (g_cam.showing >= 2) { g_cam.was_aiming = false; return; }
    }

    if (aiming) {
        // The game's aiming view takes over; it looks the way the character is facing. By default the character is
        // first turned to face where the free camera was looking, so the aim starts on what the player was looking
        // at; with the option, the character is left alone and the view jumps round to its facing instead.
        // While aiming, follow the game's camera so the free camera continues from it afterwards.
        if (!g_cam.was_aiming && was_active && !g.aim_from_character) {
            const uint16_t yaw = angle_to_s16(g_cam.heading + PI_F);
            wr16(obj, yaw);
            const uint32_t data = rd32(obj + 0x68);
            // the object's yaw and the copies the control code keeps and writes back every tick
            if (valid_ptr(data)) { for (uint32_t k : { 0x106u, 0x11Cu, 0x122u, 0x1C6u, 0x1CAu }) wr16(data + k, yaw); }
        }
        g_cam.was_aiming = true;
        g_cam.heading = game_heading;
        g_cam.elevation = 0;
        return;
    }
    if (g_cam.face_reset) { // somewhere new: start behind the character, looking the way it faces
        g_cam.heading = wrap_angle(angle_from_s16(rd16(obj)) + PI_F);
        g_cam.elevation = 0;
        g_cam.face_reset = false;
    }
    else if (!was_active || g_cam.was_aiming) { // (re)start from the game's camera
        g_cam.heading = game_heading;
        g_cam.elevation = 0;
    }
    g_cam.was_aiming = false;
    g_cam.active = true;

    const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    const float lx = g_stick_lx, ly = g_stick_ly, rx = g_stick_rx, ry = g_stick_ry;

    // Right stick: 150 degrees a second sideways, 75 up/down, at full deflection.
    constexpr float YAW_RATE = 150.0f / 60.0f * (PI_F / 180.0f), PITCH_RATE = 75.0f / 60.0f * (PI_F / 180.0f);
    g_cam.heading = wrap_angle(g_cam.heading + g_sign_cam * (rx * YAW_RATE * frames * (float)g.camera_speed_h / 100.0f + g_mouse_cam_x));
    g_cam.elevation = std::clamp(g_cam.elevation + (g.invert_camera_y ? -ry : ry) * -PITCH_RATE * frames * (float)g.camera_speed_v / 100.0f - g_mouse_cam_y, -1.1f, 0.75f);

    // Left stick. The game's own controls are already relative to a camera - its own, the one that swings round behind
    // the character: the stick's direction, seen from that camera, is the way the character turns to and runs. So the
    // stick only has to be rotated by the angle between the camera the player sees and the game's own.
    const float deflection = std::min(1.0f, std::sqrt(lx * lx + ly * ly));
    int stick_x = 0, stick_y = 0;
    if (deflection > 0.12f) {
        if (g.diagonal_boost) {
            // Forward and sideways parts as the original's "stick forward + C button" would give them: the larger one
            // counts in full, so straight is 1, a full diagonal is the square root of 2 at 100%.
            const float larger = std::max(std::fabs(lx), std::fabs(ly));
            const float forward = ly / larger, sideways = lx / larger * (float)g.strafe_speed / 100.0f;
            g_cam.move_factor *= std::sqrt(forward * forward + sideways * sideways);
        }
        const float wanted = g_cam.heading + g_sign_move * std::atan2(lx, ly); // world heading to run along
        const float relative = wrap_angle(wanted - game_heading);
        stick_x = (int)std::lround(std::sin(relative) * deflection * 78.0f * g_sign_move);
        stick_y = (int)std::lround(std::cos(relative) * deflection * 78.0f);
    }
    wr8(pad + 2, (uint8_t)(int8_t)stick_x);
    wr8(pad + 3, (uint8_t)(int8_t)stick_y);
}

// Entry of objMoveXYZ(object, dx, dy, dz), the game's "move this object by this much, with collision". The character
// control code calls it once a tick for the player with velocity * frames, and afterwards works out the velocity again
// from how far the character actually got, measured from the position it saved in its data (object + 0x68: x at +0x3C,
// z at +0x44) - so simply enlarging the step would come back as a higher velocity and be enlarged again every tick.
// Instead the extra distance is covered by a second, separate objMoveXYZ call made from here (so it is collision
// checked too), and the saved position is moved along by what that call achieved. The game then makes its own step
// and sees exactly its own speed.
extern "C" void jfg_move_scale(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    static bool busy = false;
    if (busy || g_platform_move || g_cam.player == 0 || (uint32_t)ctx->r4 != g_cam.player) return;
    const uint32_t obj = g_cam.player, data = rd32(obj + 0x68);
    if (!valid_ptr(data)) return;
    // Underwater (character state 4, Vela's swimming) the "Dive speed" option scales the whole 3D step, up and down too.
    constexpr uint8_t STATE_SWIM = 4;
    const bool swim = rd8(data + PD_STATE) == STATE_SWIM;
    const float dive = swim ? (float)g.dive_speed / 100.0f : 1.0f;
    const float factor_xz = g_cam.move_factor * dive, factor_y = dive;
    if (factor_xz == 1.0f && factor_y == 1.0f) return;
    auto as_float = [](gpr reg) { const uint32_t bits = (uint32_t)reg; float f; std::memcpy(&f, &bits, 4); return f; };
    auto as_bits = [](float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; };
    const float dx = as_float(ctx->r5) * (factor_xz - 1.0f), dz = as_float(ctx->r7) * (factor_xz - 1.0f);
    const float dy = swim ? as_float(ctx->r6) * (factor_y - 1.0f) : 0.0f;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return;
    const float x0 = rdf(obj + 0x0C), y0 = rdf(obj + 0x10), z0 = rdf(obj + 0x14);
    busy = true;
    call_game(objMoveXYZ, ctx, { obj, as_bits(dx), as_bits(dy), as_bits(dz) });
    busy = false;
    wrf(data + 0x3C, rdf(data + 0x3C) + (rdf(obj + 0x0C) - x0));
    if (swim) wrf(data + 0x40, rdf(data + 0x40) + (rdf(obj + 0x10) - y0));
    wrf(data + 0x44, rdf(data + 0x44) + (rdf(obj + 0x14) - z0));
}

// Start of the tick: give the game its own camera back.
void jfg_camera_restore() {
    if (!g_cam.overridden) return;
    g_cam.overridden = false;
    for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { uint32_t v; std::memcpy(&v, &g_cam.saved[i], 4); wr32(g_cam.camera_addr + i, v); }
}

// Right before the scene is drawn: move the camera round the character to the chosen angles.
extern "C" void jfg_camera_override(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (g_fly.active && g_fly.started && !g_cam.overridden) {
        const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        wrf(cam + 0x0C, g_fly.x); wrf(cam + 0x10, g_fly.y); wrf(cam + 0x14, g_fly.z);
        wr16(cam + 0, angle_to_s16(-g_fly.heading));
        wr16(cam + 2, angle_to_s16(g_fly.pitch));
        wr16(cam + 4, 0);
        return;
    }
    if (g_boss_cam_turn != 0 && boss_control_active() && !g_cam.overridden) {
        // Boss fight, mouse and keyboard: the game's camera, turned on the spot by how far the aim has gone to the side.
        const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        wr16(cam + 0, (uint16_t)(rd16(cam + 0) - g_boss_cam_turn)); // camera angle = minus the heading it looks along
        return;
    }
    if ((!g_cam.active && !g_cam.shoulder) || g_cam.overridden) return;
    const uint32_t obj = player_object(ctx);
    if (obj == 0) return;
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float px = rdf(obj + 0x0C), py = rdf(obj + 0x10), pz = rdf(obj + 0x14);
    const float cx = rdf(cam + 0x0C), cy = rdf(cam + 0x10), cz = rdf(cam + 0x14);
    const float flat = std::sqrt((cx - px) * (cx - px) + (cz - pz) * (cz - pz));

    if (g_cam.shoulder) {
        // The game's own aiming camera glides, over about a second, to a point 50 units straight behind the character's
        // head and then trails the character's turns. Here the view is built directly from the character instead: at
        // head height, a little behind and to the right, looking exactly where the character aims, turned slightly
        // inwards so the middle of the screen meets the line of fire some way ahead. It takes a tenth of a second to
        // get there from wherever the game's camera is.
        const uint32_t data = rd32(obj + 0x68);
        if (!valid_ptr(data)) return;
        constexpr float SIDE = 18.0f, BACK = 40.0f, HEAD = 60.0f, CONVERGE = 700.0f;
        const float heading = angle_from_s16(rd16(data + PD_BODY_YAW)) + PI_F;
        // the game's own aiming camera pitch: view pitch plus 3/8 of the reticle's offset, positive looking down
        const float pitch = -(angle_from_s16(rd16(data + PD_VIEW_PITCH)) + 0.375f * angle_from_s16(rd16(data + PD_RETICLE_PITCH)));
        const float fx = std::sin(heading) * std::cos(pitch), fy = -std::sin(pitch), fz = std::cos(heading) * std::cos(pitch);
        // screen-right is heading - 90 degrees: (-cos, sin)
        const float tx = px - fx * BACK - std::cos(heading) * SIDE;
        const float ty = py + HEAD - fy * BACK;
        const float tz = pz - fz * BACK + std::sin(heading) * SIDE;
        const float look = heading + std::atan2(SIDE, CONVERGE);
        const float b = g_cam.shoulder_blend;
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        const float game_look = -angle_from_s16(rd16(cam)), game_pitch = angle_from_s16(rd16(cam + 2));
        wrf(cam + 0x0C, cx + (tx - cx) * b);
        wrf(cam + 0x10, cy + (ty - cy) * b);
        wrf(cam + 0x14, cz + (tz - cz) * b);
        wr16(cam + 0, angle_to_s16(-(game_look + wrap_angle(look - game_look) * b)));
        wr16(cam + 2, angle_to_s16(game_pitch + (pitch - game_pitch) * b));
        return;
    }
    if (flat < 20.0f || flat > 2000.0f) return; // not an ordinary follow camera (first person, a fixed view, ...)
    const float game_elevation = angle_from_s16(rd16(cam + 2));
    // The game's camera looks at a point above the character's feet; keep looking at that point from the same distance.
    const float pivot = std::clamp((cy - py) - flat * std::tan(game_elevation), 0.0f, 200.0f);
    const float boom = std::sqrt(flat * flat + (cy - py - pivot) * (cy - py - pivot));
    // Stick up lowers the camera towards the ground behind the character; once it is nearly level it stays there
    // (so it cannot sink into the floor) and tilts upwards on the spot instead.
    const float pitch = std::clamp(game_elevation + g_cam.elevation, -0.45f, 1.25f);
    const float elevation = std::max(pitch, 0.06f);

    for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
    g_cam.camera_addr = cam;
    g_cam.overridden = true;
    wrf(cam + 0x0C, px - std::sin(g_cam.heading) * boom * std::cos(elevation));
    wrf(cam + 0x10, py + pivot + boom * std::sin(elevation));
    wrf(cam + 0x14, pz - std::cos(g_cam.heading) * boom * std::cos(elevation));
    wr16(cam + 0, angle_to_s16(-g_cam.heading));
    wr16(cam + 2, angle_to_s16(pitch));
}

// --- cutscene skip --------------------------------------------------------------------------------------------------
// START during a cutscene runs the game flat out until the cutscene has ended:
//   - jfg_after_joy (hook after joyRead in the main loop tick) sees the START press, swallows it and starts the skip;
//   - while skipping, jfg_vi_skip (hook at the entry of viFrameSync) does the buffer swap but none of the waiting for
//     video retraces, and jfg_frame_sync (hook after viFrameSync returns) makes every tick simulate 6 video frames,
//     the most the game allows. Nothing is bypassed in the game's logic, so whatever the cutscene sets up (flags,
//     items, the level it leads to) still happens; it just takes a fraction of a second instead of its real length.
// The skip ends when the level changes, when the cutscene is over, or after a time limit.
// A cutscene is recognised by the letterboxed VI mode the game switches to for most of them (sResolutionIndex bit 0)
// or by the level being one of the developers' sequence levels (name containing "Seq" or "Intro").
bool level_is_sequence(int level) {
    load_level_names();
    if (level < 0 || level >= (int)g_level_names.size()) return false;
    const std::string& n = g_level_names[level];
    return contains_ci(n, "seq") || contains_ci(n, "intro");
}

bool in_cutscene() {
    if (rd32(ADDR_mainGameMode) != 0) return false; // not in the game proper (front end, title)
    return (rd8(ADDR_sResolutionIndex) & 1) != 0 || level_is_sequence((int16_t)rd16(ADDR_currentLevel));
}

// Two stretches of the front end that START may also skip:
//  - the company and logo screens after power-on: level 0 until the title screen (level 349) has been reached once
//    (level 0 is also what is loaded behind the file-select menus later, which must not be skipped);
//  - the story intro after a new file has been created (levels 322-327, see jfg_after_joy): not fast-forwarded
//    (it is timed to its music, so it does not run faster) but left for character select directly.
constexpr int LEVEL_SPLASH = 0, LEVEL_TITLE = 349;
bool g_title_reached = false;
bool in_front_end_scene() {
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (rd32(ADDR_mainGameMode) == 0) return false;
    if (level == LEVEL_TITLE) g_title_reached = true;
    return level == LEVEL_SPLASH && !g_title_reached;
}
inline bool skippable_scene() { return in_cutscene() || in_front_end_scene(); }

std::chrono::steady_clock::time_point g_skip_started;
int g_skip_ticks = 0;
bool g_audio_catch_up = false; // a music player's event queue is nearly full: let the audio thread run
int g_skip_real_frames = 0;
std::thread::id g_main_thread; // the game's main thread (set in jfg_after_joy)
std::atomic<bool> jfg_skip_render{ false };
extern "C" int jfg_skip_render_active(void) { return jfg_skip_render.load(std::memory_order_relaxed) ? 1 : 0; } // read by the render thread

extern "C" void jfg_after_joy(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    g_main_thread = std::this_thread::get_id();
    jfg_camera_controls(ctx);
    {
        // Scripted testing: JFG_DEBUG_INPUT="secs,dur,buttons,x,y;..." holds N64 buttons (hex mask) and the stick on
        // controller 1 for dur seconds from secs seconds after launch (the buttons count as pressed on the first tick).
        static const char* const inp = std::getenv("JFG_DEBUG_INPUT");
        struct Step { float t, dur; unsigned buttons; int x, y; bool started; };
        static std::vector<Step> steps;
        static bool parsed = false;
        static const auto start = std::chrono::steady_clock::now();
        if (inp != nullptr && !parsed) {
            parsed = true;
            for (const char* p = inp; *p; ) {
                Step st{}; int n = 0;
                if (std::sscanf(p, "%f,%f,%x,%d,%d%n", &st.t, &st.dur, &st.buttons, &st.x, &st.y, &n) >= 5) steps.push_back(st);
                const char* q = std::strchr(p, ';'); if (q == nullptr) break; p = q + 1;
            }
        }
        const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const uint32_t pad = ADDR_sControllerCurrData + 6u * (rd8(ADDR_sPlayerID) & 3u);
        for (Step& st : steps) {
            if (now < st.t || now >= st.t + st.dur) continue;
            wr16(pad, (uint16_t)(rd16(pad) | st.buttons));
            if (st.x != 0 || st.y != 0) { wr8(pad + 2, (uint8_t)(int8_t)st.x); wr8(pad + 3, (uint8_t)(int8_t)st.y); }
            if (!st.started) { st.started = true; wr16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u), (uint16_t)(rd16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u)) | st.buttons)); }
        }
    }
    if (g_reset_press_start) { // F5 while paused: close the pause screen first (see jfg_debug_frame)
        g_reset_press_start = false;
        const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
        wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) | START_BUTTON));
        return;
    }
    static const bool env_skip = std::getenv("JFG_SKIP_CUTSCENES") != nullptr; // scripted tests: skip every cutscene
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (!g.skipping) {
        bool start = false;
        for (uint32_t i = 0; i < 4; i++) start |= (rd16(ADDR_buttonsPressed + 2 * i) & START_BUTTON) != 0;
        { static const bool log = std::getenv("JFG_DEBUG_STATS") != nullptr; if (log && start) std::fprintf(stderr, "[jfg] START pressed: level %d, game mode %d, skippable %d, option %d\n", level, (int)rd32(ADDR_mainGameMode), int(skippable_scene()), int(g.start_skips_cutscenes)); }
        if (g.start_skips_cutscenes && start && level >= LEVEL_STORY_FIRST && level <= LEVEL_STORY_LAST) {
            // Story intro after creating a file: 322 JFG ship, 324 ship look-in, 323 control room, 325 ant attack,
            // 326 spawn attack, 327 character select sequence. These scenes wait for their music, so fast-forwarding
            // them only blacks the screen out. Go to character select the way the file-select menu does instead
            // (logged: mainChangeLevel(127, 0, 0, 5, 1, 0)); jfg_debug_frame makes the call at the next tick.
            if (!g_story_skip_pending) std::fprintf(stderr, "[jfg] story intro skip (level %d)\n", level);
            g_story_skip_pending = true;
            for (uint32_t i = 0; i < 4; i++) wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) & ~START_BUTTON));
        }
        else if (((g.start_skips_cutscenes && start) || env_skip) && skippable_scene()) {
            g.skipping = true;
            g.skip_level = level;
            g_skip_started = std::chrono::steady_clock::now();
            g_skip_ticks = 0;
            g_skip_real_frames = 0;
            g_audio_catch_up = false;
            jfg_skip_render = true;
            std::fprintf(stderr, "[jfg] cutscene skip: start (level %d)\n", level);
            // The game must not also see this START press (it could open the pause menu and stall the cutscene).
            for (uint32_t i = 0; i < 4; i++) wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) & ~START_BUTTON));
        }
    }
    else {
        // Safety limits: 10 minutes of cutscene (6 frames per tick) or one minute of real time.
        const bool timeout = ++g_skip_ticks > 6000 || std::chrono::steady_clock::now() - g_skip_started > std::chrono::seconds(60);
        if (level != g.skip_level && skippable_scene() && !timeout) {
            // One scene led straight into another (new game: intro -> arrival cutscene): keep going.
            std::fprintf(stderr, "[jfg] cutscene skip: continues (level %d -> %d)\n", g.skip_level, level);
            g.skip_level = level;
        }
        else if (level != g.skip_level || !skippable_scene() || timeout) {
            std::fprintf(stderr, "[jfg] cutscene skip: end (level %d -> %d%s)\n", g.skip_level, level, timeout ? ", time limit" : "");
            std::fprintf(stderr, "[jfg] cutscene skip: %d ticks (%d with a real frame for the audio thread) in %.2f s; tune %d state %d\n", g_skip_ticks, g_skip_real_frames, std::chrono::duration<double>(std::chrono::steady_clock::now() - g_skip_started).count(),
                (int)rd8(ADDR_tuneSeqNo), valid_ptr(rd32(ADDR_tuneSeqPlayer)) ? (int)rd32(rd32(ADDR_tuneSeqPlayer) + 0x2C) : -1);
            g.skipping = false;
            jfg_skip_render = false;
        }
    }
}

// Entry of rcpFast3d(start, end, type, flags), which sends a graphics task to the scheduler. Returns 1 to drop the task.
// Type 0 is the frame's main task (sent at the top of the main loop tick); 3, 4 and 5 are the clone, refraction and
// blur side tasks of the same frame. The decision is taken at the main task and kept until the next one, so a frame's
// tasks are either all sent or all dropped (rcpWaitDP waits for the main task whenever any of them was sent).
extern "C" int jfg_rcp_skip(uint8_t* rdram_, uint32_t type) {
    g_rdram = rdram_;
    static bool dropping = false;
    if (type == 0) {
        if (dropping) { // side tasks dropped last frame left their "active" flags set; rcpWaitDP would wait for them
            wr32(ADDR_cloneTaskActive, 0);
            wr32(ADDR_refractTaskActive, 0);
            wr32(ADDR_blurTaskActive, 0);
        }
        dropping = g.skipping;
    }
    return dropping ? 1 : 0;
}

// Entry of ad_sndp_play(bank, sound, volume, pan, pitch, ?, handlePtr). Returns 1 to drop the sound (cutscene skip in
// progress); the caller's sound handle, the 7th argument at 0x18($sp), is then set to NULL.
extern "C" int jfg_snd_skip(uint8_t* rdram_, uint32_t sp) {
    g_rdram = rdram_;
    if (!g.skipping) return 0;
    const uint32_t handle_ptr = rd32(sp + 0x18);
    if (handle_ptr >= 0x80000000u && handle_ptr < 0x80800000u) wr32(handle_ptr, 0);
    return 1;
}

// Entry of n_alEvtqPostEvent(evtq, evt, delta). Returns 1 to drop the event: during a skip, everything the game's main
// thread posts except "stop" events, and those too if the queue is nearly full (see jfg.us.toml).
extern "C" int jfg_evt_skip(uint8_t* rdram_, uint32_t evtq, uint32_t evt) {
    if (!g.skipping || std::this_thread::get_id() != g_main_thread) return 0;
    g_rdram = rdram_;
    int free_items = 0;
    for (uint32_t p = rd32(evtq); p >= 0x80000000u && p < 0x80800000u && free_items < 12; p = rd32(p)) free_items++; // evtq->freeList.next chain
    if (evtq != rd32(ADDR_gSoundPlayerPtr) + 0x14) {
        // A sequence player (music, ambience): everything goes through, or the music of the skipped cutscene would
        // never be stopped or replaced. If its queue is filling up, the next tick waits for a real video frame so the
        // audio thread can work the queue off (jfg_vi_skip).
        if (free_items < 12) g_audio_catch_up = true;
        return free_items < 2 ? 1 : 0;
    }
    constexpr uint16_t AL_SNDP_STOP_EVT = 1 << 10;
    if (rd16(evt) != AL_SNDP_STOP_EVT) return 1;
    return free_items < 8 ? 1 : 0;
}

// Replaces osSyncPrintf(fmt, ...): the format string goes to stderr, unformatted.
extern "C" void jfg_sync_printf(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t fmt = (uint32_t)ctx->r4;
    static int budget = 200; // a warning repeated every frame must not flood the log
    if (budget <= 0 || fmt < 0x80000000u || fmt >= 0x80800000u) return;
    budget--;
    char buf[160]; size_t n = 0;
    for (; n < sizeof(buf) - 1; n++) { const char c = (char)rd8(fmt + (uint32_t)n); if (c == 0) break; buf[n] = c; }
    while (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = 0;
    std::fprintf(stderr, "[game] %s  (args %08X %08X %08X)\n", buf, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7);
}

// Entry of viFrameSync(mesg). Returns 1 when the call has been handled here (skip in progress).
// Frame pacing log (JFG_DEBUG_PACING=1): per tick, how long the tick took up to viFrameSync and in viFrameSync.
std::chrono::steady_clock::time_point g_pace_tick_start, g_pace_sync_start;
extern "C" int jfg_vi_skip(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    g_pace_sync_start = std::chrono::steady_clock::now(); { static const bool pl = std::getenv("JFG_DEBUG_PACING") != nullptr; if (pl) std::fprintf(stderr, "[pace] vi queue at entry: %d\n", (int)rd32(0x800FEB80 + 8)); }
    if (g.frame_rate >= 1 && !g.skipping) {
        // 60 fps. viFrameSync waits until gVideoDeltaTime retraces have been counted (runInOneFrame makes that 1) and
        // then always waits for one more retrace after the buffer swap. The scheduler only starts a graphics task at a
        // retrace, so a tick has always seen one retrace by the time it gets here; that extra wait made every tick two
        // retraces long. Same steps as viFrameSync (src/gameVi.c) without it: wait until at least one retrace has
        // passed since the last tick, swap, and report how many passed (the time the next tick simulates).
        constexpr uint32_t ADDR_sBlackScreenTimer = 0x800FECA9, ADDR_gSkipVideoDeltaAdjust = 0x800FECAD, ADDR_gVideoMesgQueue = 0x800FEB80;
        const uint8_t black = rd8(ADDR_sBlackScreenTimer);
        if (black != 0) {
            wr8(ADDR_sBlackScreenTimer, (uint8_t)(black - 1));
            if (black == 1) call_game(osViBlack_recomp, ctx, { 0 });
        }
        if ((int32_t)ctx->r4 != 8) call_game(fb_swap, ctx, {}); // MESG_SKIP_BUFFER_SWAP
        uint32_t frames = 0;
        while ((int32_t)call_game(osRecvMesg_recomp, ctx, { ADDR_gVideoMesgQueue, 0, 0 /* NOBLOCK */ }) != -1) frames++;
        if (frames == 0) { call_game(osRecvMesg_recomp, ctx, { ADDR_gVideoMesgQueue, 0, 1 /* BLOCK */ }); frames = 1; }
        call_game(osViSwapBuffer_recomp, ctx, { rd32(ADDR_otherScreen) });
        wr8(ADDR_gSkipVideoDeltaAdjust, 0);
        ctx->r2 = frames;
        return 1;
    }
    if (!g.skipping) return 0;
    if (g_audio_catch_up) { // run the real viFrameSync once: it blocks until a video frame has passed
        g_audio_catch_up = false;
        g_skip_real_frames++;
        return 0;
    }
    if ((uint32_t)ctx->r4 != 0xFFFFFFFFu) call_game(fb_swap, ctx, {}); // as viFrameSync: unless MESG_SKIP_BUFFER_SWAP
    call_game(osViSwapBuffer_recomp, ctx, { rd32(ADDR_otherScreen) });
    ctx->r2 = 6;
    return 1;
}

// Right after viFrameSync() returned in the main loop tick: $v0 = video frames the next tick simulates (clamped to 6).
extern "C" uint32_t jfg_frame_sync(uint8_t* rdram_, uint32_t frames) {
    g_rdram = rdram_;
    {
        static const bool pacing = std::getenv("JFG_DEBUG_PACING") != nullptr;
        const auto now = std::chrono::steady_clock::now();
        if (pacing && g_pace_tick_start != decltype(g_pace_tick_start){}) {
            std::fprintf(stderr, "[pace] work %.1f ms sync %.1f ms frames %u delta %u counter %u\n",
                std::chrono::duration<double, std::milli>(g_pace_sync_start - g_pace_tick_start).count(),
                std::chrono::duration<double, std::milli>(now - g_pace_sync_start).count(), frames,
                rd8(0x800FECAC), rd8(0x800FECAB));
        }
        g_pace_tick_start = now;
    }
    {
        // Getting back to 30 fps after a hitch. viFrameSync (src/gameVi.c) drops the game to 20 fps (3 video frames a
        // tick, gVideoDeltaTime) as soon as one tick takes 3 retraces, and only returns to 30 after 30 ticks in a row
        // that took fewer. Under the recompilation's timing the 20 fps state locks itself in: the ticks alternate
        // between 2 and 4 retraces (the scheduler holds back each frame's "done" until 2 retraces have passed), so
        // every second tick counts as slow and the run of 30 never comes - one long frame (a texture pack being
        // loaded, the window losing focus) left the game at 20 fps for good. Measured with JFG_DEBUG_PACING and a slowed
        // VI clock: at 20 fps the ticks went 70 / 230 ms of work forever; set back to 30 fps, the same scene held it.
        // So after a while at 20 fps the game is put back to 30 fps; if a tick is slow again it drops straight back by
        // itself (one slightly long frame), and the next try waits twice as long (up to about half a minute).
        constexpr uint32_t ADDR_gVideoDeltaTime = 0x800FECAC, ADDR_gVideoDeltaCounter = 0x800FECAB;
        static int ticks_at_20 = 0, wait_ticks = 45, held_ticks = 0;
        static bool trying = false;
        static const bool log = std::getenv("JFG_DEBUG_PACING") != nullptr;
        const uint8_t delta = rd8(ADDR_gVideoDeltaTime);
        static int level_seen = -1;
        const int level_now = (int16_t)rd16(ADDR_currentLevel);
        if (level_now != level_seen) { level_seen = level_now; wait_ticks = 45; } // a new area: start trying early again
        if (g.skipping || rd32(ADDR_runInOneFrame) != 0 || (delta != 2 && delta != 3)) {
            ticks_at_20 = 0; trying = false;
        }
        else if (delta == 3) {
            if (trying) { // dropped again while trying: the game really needs 20 fps here for now
                trying = false;
                wait_ticks = std::min(wait_ticks * 2, 600);
                if (log) std::fprintf(stderr, "[pace] back to 20 fps after %d ticks; next try in %d ticks\n", held_ticks, wait_ticks);
            }
            // Switched right after one of the long ticks (counter 0), the next tick is a short one and 30 fps starts
            // cleanly; switched before a long one, it drops back at once.
            if (++ticks_at_20 >= wait_ticks && rd8(ADDR_gVideoDeltaCounter) == 0) {
                ticks_at_20 = 0; trying = true; held_ticks = 0;
                wr8(ADDR_gVideoDeltaTime, 2);
                wr8(ADDR_gVideoDeltaCounter, 0);
                if (log) std::fprintf(stderr, "[pace] trying 30 fps again\n");
            }
        }
        else { // 30 fps
            ticks_at_20 = 0;
            if (trying && ++held_ticks >= 30) { trying = false; wait_ticks = 45; if (log) std::fprintf(stderr, "[pace] 30 fps holds\n"); }
        }
    }
    {
        static auto fps_t0 = std::chrono::steady_clock::now();
        static int fps_n = 0;
        fps_n++;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - fps_t0).count();
        if (dt >= 0.5) {
            static uint32_t last_presents = 0;
            const uint32_t presents = rt64_jfg_present_count.load(std::memory_order_relaxed);
            g_fps.store((float)(fps_n / dt), std::memory_order_relaxed);
            g_screen_fps.store((float)((presents - last_presents) / dt), std::memory_order_relaxed);
            last_presents = presents;
            fps_n = 0; fps_t0 = std::chrono::steady_clock::now();
        }
    }
    {
        static const bool stats = std::getenv("JFG_DEBUG_STATS") != nullptr;
        static const int stats_every = stats ? std::atoi(std::getenv("JFG_DEBUG_STATS")) : 0; // JFG_DEBUG_STATS=<ticks between lines>, default 200
        static uint32_t n = 0, sum = 0;
        static auto t0 = std::chrono::steady_clock::now();
        if (stats) {
            sum += frames;
            if (++n == (stats_every > 0 ? stats_every : 200)) {
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::fprintf(stderr, "[jfg] ticks/s %.1f, mean frames per tick %.2f, level %d, skipping %d, tune %d state %d\n", n / secs, sum / double(n), (int)(int16_t)rd16(ADDR_currentLevel), int(g.skipping),
                    (int)rd8(ADDR_tuneSeqNo), valid_ptr(rd32(ADDR_tuneSeqPlayer)) ? (int)rd32(rd32(ADDR_tuneSeqPlayer) + 0x2C) : -1);
                if (g_cam.player != 0) std::fprintf(stderr, "[jfg] player at %.0f %.0f %.0f, speed factor %.2f\n", rdf(g_cam.player + 0x0C), rdf(g_cam.player + 0x10), rdf(g_cam.player + 0x14), g_cam.move_factor);
                std::fprintf(stderr, "[jfg] robotMission %d, dual stick active %d, game mode %d, vi mode %d, next level %d\n", (int)rd8(ADDR_robotMission), jfg_dual_stick_active(), (int)rd32(ADDR_mainGameMode), (int)rd8(ADDR_sResolutionIndex), (int)(int16_t)rd16(ADDR_nextLevel));
                { load_level_names(); const int lv = (int16_t)rd16(ADDR_currentLevel); if (lv >= 0 && lv < (int)g_level_names.size()) std::fprintf(stderr, "[jfg] level name '%s'\n", g_level_names[lv].c_str()); }
                n = 0; sum = 0; t0 = std::chrono::steady_clock::now();
            }
        }
    }
    {
        // Scripted testing: JFG_DEBUG_REFLECT=<secs> lists the level's reflective floor batches once;
        // JFG_DEBUG_TELEPORT="secs,x,y,z" moves the player there once.
        static const char* const refl = std::getenv("JFG_DEBUG_REFLECT");
        static const char* const tele = std::getenv("JFG_DEBUG_TELEPORT");
        static const auto start = std::chrono::steady_clock::now();
        static bool done_r = false, done_t = false;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (refl != nullptr && !done_r && secs >= std::atof(refl)) {
            done_r = true;
            const uint32_t track = rd32(0x800A0D60);
            if (valid_ptr(track)) {
                const int nseg = (int16_t)rd16(track + 0x1A);
                const uint32_t segs = rd32(track + 4);
                for (int i = 0; i < nseg; i++) {
                    const uint32_t seg = segs + 0x48 * i;
                    const uint32_t verts = rd32(seg), tris = rd32(seg + 4), batches = rd32(seg + 0xC);
                    const int nb = (int16_t)rd16(seg + 0x28);
                    for (int b = 0; b < nb; b++) {
                        const uint32_t bt = batches + 0x10 * b;
                        const uint32_t flags = rd32(bt + 0xC);
                        if (!(flags & 0x20000000)) continue;
                        const int vo = (int16_t)rd16(bt + 6), fo = (int16_t)rd16(bt + 8), vn = (int16_t)rd16(bt + 0x16), fn = (int16_t)rd16(bt + 0x18);
                        int x0 = 99999, x1 = -99999, z0 = 99999, z1 = -99999, y = 0;
                        for (int f = fo; f < fn; f++) for (int k = 1; k <= 3; k++) {
                            const uint32_t v = verts + 10 * (vo + rd8(tris + 16 * f + k));
                            const int x = (int16_t)rd16(v), z = (int16_t)rd16(v + 4); y = (int16_t)rd16(v + 2);
                            x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z);
                        }
                        std::fprintf(stderr, "[jfg] reflect seg %d batch %d flags %08X tex %d y %d x[%d,%d] z[%d,%d] faces %d..%d (next vo %d)\n", i, b, flags, (int)rd8(bt), y, x0, x1, z0, z1, fo, fn, vn);
                    }
                }
            }
        }
        if (tele != nullptr && !done_t && g_cam.player != 0) {
            float t = 0, x = 0, y = 0, z = 0;
            std::sscanf(tele, "%f,%f,%f,%f", &t, &x, &y, &z);
            if (secs >= t) {
                done_t = true;
                const uint32_t obj = g_cam.player, data = rd32(obj + 0x68);
                wrf(obj + 0x0C, x); wrf(obj + 0x10, y); wrf(obj + 0x14, z);
                if (valid_ptr(data)) { wrf(data + 0x3C, x); wrf(data + 0x40, y); wrf(data + 0x44, z); }
            }
        }
    }
    static const int force_frames = std::getenv("JFG_DEBUG_FORCE_FRAMES") ? std::atoi(std::getenv("JFG_DEBUG_FORCE_FRAMES")) : 0; // tests: frames per tick
    if (force_frames > 0 && !g.skipping) return (uint32_t)force_frames;
    return g.skipping ? 6 : frames;
}

// ---------------------------------------------------------------------------------------------------------------------
// Weapons tab: per-weapon stats.
//
// The player's weapons are described by weaponTable (0x800A1490, 0x30 bytes per weapon, indexed by the weapon slot;
// controlGetWeaponDef), read when a shot is fired (controlUpdateWeapon -> func_800386B0, which hands the values to the
// weapon's spawn function). Fields used here:
//   +0x03 u8   damage (passed to the projectile; laser bolts: laserbolt data+0x61 and, as in the original, the bolt's
//              model/size class); +0x04 u8 the same value in the table, kept equal
//   +0x05 u8   frames between shots (data+0x538 countdown, decremented by the frames each tick simulates)
//   +0x06 s8   > 0: frames from the button press to the release of a thrown weapon (data+0x53D); -1 charge, -2 beam
//   +0x07 u8   projectile lifetime (laser bolts live lifetime*6 frames)
//   +0x08 s16, +0x0A s16  random spread added to the aim, sideways / up-down (angle units, 65536 = full turn)
//   +0x10 f32  projectile / throw speed
// Other per-weapon behaviour:
//   plasma shotgun charge: data+0x53E grows by 2x the frames while the button is held, up to character+0x57 << 5;
//     a faster charge adds the extra growth at the start of each tick (the game clamps it to the maximum itself).
//   mines: the player's mines are recorded when created (hook at the end of mineInit); beyond the limit the oldest is
//     removed the way mineRemove does it (mine data+0x01 = 6, +0xAC = 0). (The game's own list at 0x800F2D20, up to 8
//     with the oldest dropped, only holds some mine types.)
//   pistol: rapid presses build "heat" (data+0x542, +0x2A per shot, no shot at >= 0xD3); "no rapid-fire limit" clears it.
//   laser bolt size: laserboltInit's scale (bolt data+0x6C) for bolts fired by the player (hook at its end).
// When a weapon's "Modify" box is off its original values are written back.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
constexpr uint32_t ADDR_weaponTable = 0x800A1490, WEAPON_DEF_SIZE = 0x30;
constexpr int WEAPON_COUNT = 15;
constexpr const char* WEAPONS_FILE = "jfg_weapons.txt";

enum WeaponField : uint32_t {
    WF_DAMAGE = 1, WF_DELAY = 2, WF_SPEED = 4, WF_LIFE = 8, WF_SPREAD = 16, WF_THROW = 32, WF_SIZE = 64,
    WF_CHARGE = 128, WF_MINES = 256, WF_HEAT = 512,
};

struct WeaponInfo { const char* name; uint32_t fields; bool bolt; };
const WeaponInfo WEAPON_INFO[WEAPON_COUNT] = {
    { "Pistol",              WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE | WF_HEAT, true },
    { "Homing Missiles",     WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD, false },
    { "Machine Gun",         WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE, true },
    { "Plasma Shotgun",      WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_CHARGE, false },
    { "Shocker",             WF_DELAY | WF_SPREAD, false },
    { "Tri-Rocket Launcher", WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD, false },
    { "Flamethrower",        WF_SPREAD, false },
    { "Sniper Rifle",        WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_SPREAD | WF_SIZE, true },
    { "Grenades",            WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Shuriken",            WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Fish Food",           WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Timed Mines",         WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_MINES, false },
    { "Remote Mines",        WF_DAMAGE | WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW | WF_MINES, false },
    { "Flares",              WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
    { "Cluster Bombs",       WF_DELAY | WF_SPEED | WF_LIFE | WF_THROW, false },
};

struct WeaponStats {
    int damage = 0, delay = 0, throw_delay = 0, life = 0, spread_x = 0, spread_y = 0;
    float speed = 0;
};

struct WeaponMod {
    bool on = false;
    WeaponStats s;
    int size = 100;          // laser bolt size, percent
    int charge = 100;        // plasma charge speed, percent
    int mines = 8;           // mines on the field (shared by both mine types)
    bool no_heat = false;    // pistol: no rapid-fire limit
};

WeaponStats g_weapon_orig[WEAPON_COUNT];
WeaponMod g_weapon_mod[WEAPON_COUNT];
bool g_weapon_orig_read = false;
bool g_weapons_loaded = false;
std::recursive_mutex g_weapon_mutex; // UI thread edits, game thread applies
std::vector<std::pair<uint32_t, uint32_t>> g_player_mines; // the player's mines (object, data), oldest first

WeaponStats read_weapon_stats(int w) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    WeaponStats s;
    s.damage = rd8(d + 3); s.delay = rd8(d + 5); s.throw_delay = (int8_t)rd8(d + 6); s.life = rd8(d + 7);
    s.spread_x = (int16_t)rd16(d + 8); s.spread_y = (int16_t)rd16(d + 0xA); s.speed = rdf(d + 0x10);
    return s;
}

void write_weapon_stats(int w, const WeaponStats& s, const WeaponStats& orig) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    const uint32_t f = WEAPON_INFO[w].fields;
    if (f & WF_DAMAGE) { wr8(d + 3, (uint8_t)std::clamp(s.damage, 0, 255)); wr8(d + 4, (uint8_t)std::clamp(s.damage, 0, 255)); }
    if (f & WF_DELAY) wr8(d + 5, (uint8_t)std::clamp(s.delay, 0, 255));
    if ((f & WF_THROW) && orig.throw_delay > 0) wr8(d + 6, (uint8_t)std::clamp(s.throw_delay, 1, 127));
    if (f & WF_LIFE) wr8(d + 7, (uint8_t)std::clamp(s.life, 1, 255));
    if (f & WF_SPREAD) { wr16(d + 8, (uint16_t)std::clamp(s.spread_x, 0, 8192)); wr16(d + 0xA, (uint16_t)std::clamp(s.spread_y, 0, 8192)); }
    if (f & WF_SPEED) wrf(d + 0x10, std::clamp(s.speed, 0.1f, 2000.0f));
}

void restore_weapon_stats(int w) {
    const uint32_t d = ADDR_weaponTable + WEAPON_DEF_SIZE * (uint32_t)w;
    const WeaponStats& o = g_weapon_orig[w];
    wr8(d + 3, (uint8_t)o.damage); wr8(d + 4, (uint8_t)o.damage); wr8(d + 5, (uint8_t)o.delay); wr8(d + 6, (uint8_t)o.throw_delay);
    wr8(d + 7, (uint8_t)o.life); wr16(d + 8, (uint16_t)o.spread_x); wr16(d + 0xA, (uint16_t)o.spread_y); wrf(d + 0x10, o.speed);
}

// Sets one weapon value from its saved form "<slot>.<field>" (jfg_weapons.txt, profiles with "weapon." in front).
void apply_weapon_key(const char* key, int value) {
    int w = -1; char field[48] = {};
    if (std::sscanf(key, "%d.%47s", &w, field) != 2 || w < 0 || w >= WEAPON_COUNT) return;
    WeaponMod& m = g_weapon_mod[w];
    if (!std::strcmp(field, "modify")) m.on = value != 0;
    else if (!std::strcmp(field, "damage")) m.s.damage = std::clamp(value, 0, 255);
    else if (!std::strcmp(field, "delay")) m.s.delay = std::clamp(value, 0, 255);
    else if (!std::strcmp(field, "throw_delay")) m.s.throw_delay = std::clamp(value, 1, 127);
    else if (!std::strcmp(field, "lifetime")) m.s.life = std::clamp(value, 1, 255);
    else if (!std::strcmp(field, "spread_x")) m.s.spread_x = std::clamp(value, 0, 8192);
    else if (!std::strcmp(field, "spread_y")) m.s.spread_y = std::clamp(value, 0, 8192);
    else if (!std::strcmp(field, "speed_x100")) m.s.speed = std::clamp(value, 10, 200000) / 100.0f;
    else if (!std::strcmp(field, "size")) m.size = std::clamp(value, 10, 1000);
    else if (!std::strcmp(field, "charge")) m.charge = std::clamp(value, 10, 1000);
    else if (!std::strcmp(field, "mines")) m.mines = std::clamp(value, 1, 8);
    else if (!std::strcmp(field, "no_heat")) m.no_heat = value != 0;
}

// Every weapon back to its original values, "Modify" off.
void reset_weapons() {
    for (int w = 0; w < WEAPON_COUNT; w++) { g_weapon_mod[w] = WeaponMod(); g_weapon_mod[w].s = g_weapon_orig[w]; }
}

std::string weapons_text(const char* prefix) {
    std::string out;
    char buf[512];
    for (int w = 0; w < WEAPON_COUNT; w++) {
        const WeaponMod& m = g_weapon_mod[w];
        std::snprintf(buf, sizeof(buf), "%s%d.modify=%d\n%s%d.damage=%d\n%s%d.delay=%d\n%s%d.throw_delay=%d\n%s%d.lifetime=%d\n%s%d.spread_x=%d\n",
            prefix, w, m.on ? 1 : 0, prefix, w, m.s.damage, prefix, w, m.s.delay, prefix, w, m.s.throw_delay, prefix, w, m.s.life, prefix, w, m.s.spread_x);
        out += buf;
        std::snprintf(buf, sizeof(buf), "%s%d.spread_y=%d\n%s%d.speed_x100=%d\n%s%d.size=%d\n%s%d.charge=%d\n%s%d.mines=%d\n%s%d.no_heat=%d\n",
            prefix, w, m.s.spread_y, prefix, w, (int)std::lround(m.s.speed * 100.0f), prefix, w, m.size, prefix, w, m.charge, prefix, w, m.mines, prefix, w, m.no_heat ? 1 : 0);
        out += buf;
    }
    return out;
}

void load_weapons() {
    if (g_weapons_loaded || !g_weapon_orig_read) return;
    g_weapons_loaded = true;
    reset_weapons();
    if (std::FILE* f = std::fopen(WEAPONS_FILE, "r")) {
        char key[64]; int value;
        while (std::fscanf(f, "%63[^=]=%d\n", key, &value) == 2) apply_weapon_key(key, value);
        std::fclose(f);
    }
}

void save_weapons() {
    if (std::FILE* f = std::fopen(WEAPONS_FILE, "w")) {
        std::fputs(weapons_text("").c_str(), f);
        std::fclose(f);
    }
    profile_autosave();
}
} // namespace

// Throwing. The character's control code starts its throw animation (Juno: 0x15 / 0x2E, Vela: 0x16 / 0x31, Lupus:
// 0x0E / 0x1B, the second one for the Shuriken) and locks the controls for 40 frames (data+0x577, counted down by the
// frames each tick simulates); the weapon leaves the hand after its throw delay. A throw delay set shorter than the
// original also plays the animation and runs the lock that much faster (factor = original / new delay, at most 8).
float g_throw_factor = 1.0f;
bool is_throw_anim(uint32_t obj) {
    const int anim = (int8_t)rd8(obj + 0x3B);
    switch ((int16_t)rd16(ADDR_currentCharacter)) {
    case 0: return anim == 0x16 || anim == 0x31; // Vela
    case 1: return anim == 0x15 || anim == 0x2E; // Juno
    case 2: return anim == 0x0E || anim == 0x1B; // Lupus
    default: return false;
    }
}

// Game thread, every tick (jfg_debug_frame).
void apply_weapons() {
    if (g_rdram == nullptr) return;
    if (!g_weapon_orig_read) {
        for (int w = 0; w < WEAPON_COUNT; w++) g_weapon_orig[w] = read_weapon_stats(w);
        g_weapon_orig_read = true;
    }
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    load_weapons();
    for (int w = 0; w < WEAPON_COUNT; w++) {
        if (g_weapon_mod[w].on) write_weapon_stats(w, g_weapon_mod[w].s, g_weapon_orig[w]);
        else restore_weapon_stats(w);
    }

    const uint32_t obj = g_cam.player;
    if (obj == 0 || rd32(ADDR_mainGameMode) != 0) return;
    const uint32_t data = rd32(obj + 0x68);
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
    if (!valid_ptr(data) || ch == 0) return;
    const int weapon = rd8(ch + CHAR_WEAPON_OFFSET);

    // Pistol: no rapid-fire limit.
    if (weapon == 0 && g_weapon_mod[0].on && g_weapon_mod[0].no_heat) wr8(data + 0x542, 0);

    // Throwing: faster animation and shorter control lock with a shorter throw delay (see g_throw_factor).
    g_throw_factor = 1.0f;
    if (weapon >= 0 && weapon < WEAPON_COUNT && (WEAPON_INFO[weapon].fields & WF_THROW) && g_weapon_mod[weapon].on) {
        const int o = g_weapon_orig[weapon].throw_delay, m = g_weapon_mod[weapon].s.throw_delay;
        if (o > 0 && m > 0 && m < o) g_throw_factor = std::min(8.0f, (float)o / (float)m);
    }
    if (g_throw_factor > 1.0f && is_throw_anim(obj)) {
        const int lock = (int8_t)rd8(data + 0x577);
        if (lock > 0) {
            const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
            const int extra = (int)std::lround((g_throw_factor - 1.0f) * frames);
            wr8(data + 0x577, (uint8_t)std::max(0, lock - extra));
        }
    }

    // Plasma shotgun: faster (or slower) charge.
    static int last_charge = 0;
    const int charge = (int16_t)rd16(data + 0x53E);
    if (weapon == 3 && g_weapon_mod[3].on && g_weapon_mod[3].charge != 100 && charge > last_charge) {
        const int grown = charge - last_charge;
        const int max_charge = (int)rd8(ch + 0x54 + 3) << 5;
        const int extra = grown * (g_weapon_mod[3].charge - 100) / 100;
        const int next = std::clamp(charge + extra, 0, std::max(max_charge, charge));
        wr16(data + 0x53E, (uint16_t)next);
        last_charge = next;
    }
    else last_charge = charge;

    // Mines on the field: the oldest of the player's mines beyond the limit is removed the way mineRemove does it.
    const bool mines_on = g_weapon_mod[11].on || g_weapon_mod[12].on;
    for (size_t i = 0; i < g_player_mines.size();) { // drop mines that are gone (object freed or reused, or already going)
        const uint32_t mo = g_player_mines[i].first, md = g_player_mines[i].second;
        const bool alive = valid_ptr(mo) && rd32(mo + 0x68) == md && rd8(md + 1) != 5 && rd8(md + 1) != 6;
        if (alive) i++; else g_player_mines.erase(g_player_mines.begin() + (long)i);
    }
    if (mines_on) {
        const int limit = g_weapon_mod[11].on ? g_weapon_mod[11].mines : g_weapon_mod[12].mines;
        while ((int)g_player_mines.size() > limit) {
            const uint32_t md = g_player_mines.front().second;
            wr8(md + 1, 6);   // what mineRemove does
            wr8(md + 0xAC, 0);
            g_player_mines.erase(g_player_mines.begin());
        }
    }
}

// Hook at the entry of objAnimDframe(object, speed, frames): the player's throw animation runs faster (see above).
// The speed arrives as the bits of a float in a1 (o32: the first argument is not a float).
extern "C" uint32_t jfg_anim_speed(uint8_t* rdram, uint32_t obj, uint32_t speed_bits) {
    if (g_throw_factor <= 1.0f || obj == 0 || obj != g_cam.player) return speed_bits;
    g_rdram = rdram;
    if (!is_throw_anim(obj)) return speed_bits;
    float speed; std::memcpy(&speed, &speed_bits, 4);
    speed *= g_throw_factor;
    std::memcpy(&speed_bits, &speed, 4);
    return speed_bits;
}

// Hook at the end of mineInit (overlay 112): s1 = the new mine, s0 = its data. Mines placed while the player holds a
// mine weapon are the player's.
extern "C" void jfg_mine_init(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const uint32_t mine = (uint32_t)ctx->r17, md = (uint32_t)ctx->r16;
    if (!valid_ptr(mine) || !valid_ptr(md) || g_cam.player == 0) return;
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
    if (ch == 0) return;
    const int weapon = rd8(ch + CHAR_WEAPON_OFFSET);
    if (weapon != 11 && weapon != 12) return;
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    g_player_mines.emplace_back(mine, md);
}

// Hook at the end of laserboltInit (overlay 109): a0 = the new bolt. Scales bolts the player fires.
extern "C" void jfg_bolt_init(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const uint32_t bolt = (uint32_t)ctx->r4;
    if (!valid_ptr(bolt) || g_cam.player == 0) return;
    const uint32_t bd = rd32(bolt + 0x68);
    if (!valid_ptr(bd) || rd32(bd + 0x70) != g_cam.player) return;
    const int weapon = (int8_t)rd8(bd + 0x7C);
    static const bool log = std::getenv("JFG_DEBUG_BOLTS") != nullptr;
    if (log) std::fprintf(stderr, "[jfg] bolt: weapon %d damage %d speed %.1f life %d scale %.2f\n", weapon, rd8(bd + 0x61), rdf(bd + 0x64), (int)(int16_t)rd16(bd + 0x6A), rdf(bd + 0x6C));
    if (weapon < 0 || weapon >= WEAPON_COUNT || !WEAPON_INFO[weapon].bolt) return;
    const WeaponMod& m = g_weapon_mod[weapon];
    if (!m.on || m.size == 100) return;
    wrf(bd + 0x6C, rdf(bd + 0x6C) * (float)m.size / 100.0f);
}

namespace {
bool weapon_slider(const char* label, int* v, int lo, int hi, int def) {
    char fmt[48];
    std::snprintf(fmt, sizeof(fmt), "%%d   (default %d)", def);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 20.0f));
    return ImGui::SliderInt(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}
bool weapon_slider_f(const char* label, float* v, float lo, float hi, float def) {
    char fmt[48];
    std::snprintf(fmt, sizeof(fmt), "%%.1f   (default %.1f)", def);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, ImGui::GetFontSize() * 20.0f));
    return ImGui::SliderFloat(label, v, lo, hi, fmt, ImGuiSliderFlags_AlwaysClamp);
}
} // namespace

void draw_weapons() {
    if (!g_weapon_orig_read) { ImGui::TextDisabled("Not available yet."); return; }
    std::lock_guard<std::recursive_mutex> lock(g_weapon_mutex);
    ImGui::TextWrapped("Tick \"Modify\" to change a weapon; untick it and the weapon is back to its original values. "
                       "Changes apply from the next shot, to every character using that weapon, and are saved in "
                       "jfg_weapons.txt.");
    bool changed = false;
    for (int w = 0; w < WEAPON_COUNT; w++) {
        const WeaponInfo& info = WEAPON_INFO[w];
        const WeaponStats& o = g_weapon_orig[w];
        WeaponMod& m = g_weapon_mod[w];
        ImGui::PushID(w);
        ImGui::Separator();
        changed |= ImGui::Checkbox("Modify", &m.on);
        ImGui::SameLine();
        const bool open = ImGui::CollapsingHeader(info.name);
        if (open) {
            ImGui::Indent();
            ImGui::BeginDisabled(!m.on);
            const uint32_t f = info.fields;
            if (f & WF_DAMAGE) changed |= weapon_slider("Damage", &m.s.damage, 1, 100, o.damage);
            if (f & WF_DAMAGE && info.bolt) ImGui::TextDisabled("  (also sets the bolt's look, as in the game)");
            if (f & WF_DELAY) changed |= weapon_slider("Shot delay (frames)", &m.s.delay, 0, 120, o.delay);
            if (f & WF_DELAY) ImGui::TextDisabled("  (frames between shots; 60 frames = 1 second)");
            if ((f & WF_THROW) && o.throw_delay > 0) changed |= weapon_slider("Throw delay (frames)", &m.s.throw_delay, 1, 60, o.throw_delay);
            if ((f & WF_THROW) && o.throw_delay > 0) ImGui::TextDisabled("  (press to release; the game keeps at least about 4 ticks for the throw)");
            if (f & WF_SPEED) changed |= weapon_slider_f(info.bolt ? "Speed" : "Speed / throw", &m.s.speed, 1.0f, std::max(400.0f, o.speed * 4.0f), o.speed);
            if (f & WF_LIFE) changed |= weapon_slider(info.bolt ? "Lifetime (x0.1 s)" : "Lifetime / fuse", &m.s.life, 1, 255, o.life);
            if (f & WF_SPREAD) {
                changed |= weapon_slider("Spread sideways", &m.s.spread_x, 0, 4096, o.spread_x);
                changed |= weapon_slider("Spread up/down", &m.s.spread_y, 0, 4096, o.spread_y);
            }
            if (f & WF_SIZE) changed |= weapon_slider("Bolt size (%)", &m.size, 25, 400, 100);
            if (f & WF_CHARGE) changed |= weapon_slider("Charge speed (%)", &m.charge, 25, 800, 100);
            if (f & WF_MINES) {
                changed |= weapon_slider("Mines at once", &m.mines, 1, 8, 8);
                ImGui::TextDisabled("  (your mines of both kinds; the oldest is removed when another is placed)");
            }
            if (f & WF_HEAT) {
                changed |= ImGui::Checkbox("No rapid-fire limit", &m.no_heat);
                ImGui::SameLine(); ImGui::TextDisabled("(fast presses no longer slow it down)");
            }
            if (w == 6) ImGui::TextDisabled("  (its flames have their own code; only the spread is in the weapon table)");
            if (w == 4) ImGui::TextDisabled("  (its beam has its own code; damage and range are not in the weapon table)");
            ImGui::EndDisabled();
            if (ImGui::Button("Reset to default")) {
                const bool on = m.on;
                m = WeaponMod();
                m.on = on;
                m.s = o;
                changed = true;
            }
            ImGui::Unindent();
        }
        ImGui::PopID();
    }
    if (changed) save_weapons();
}

// ---------------------------------------------------------------------------------------------------------------------
// Profiles tab: whole sets of settings (Options and Weapons tabs) to switch between.
//
// The current settings stay in jfg_port_options.txt and jfg_weapons.txt; choosing a profile resets every setting to
// the game's own behaviour and then applies the profile's values. Built-in profiles: Default (the original game),
// Modern, Modern and Balanced, Extreme. Player profiles are bin\profiles\<name>.txt (option lines, and weapon lines with "weapon." in front)
// and follow changes made while they are active. The active profile's name is kept in jfg_profile.txt.
// Not part of profiles: cheats, the texture pack choice and texture dumping.
// ---------------------------------------------------------------------------------------------------------------------
namespace {
constexpr const char* PROFILE_DIR = "profiles";
constexpr const char* PROFILE_CHOICE_FILE = "jfg_profile.txt";

struct BuiltinProfile { const char* name; const char* description; const char* text; };
const BuiltinProfile BUILTIN_PROFILES[] = {
    { "Default", "The original game: every option and weapon as on the N64 (cutscenes play in full).", "start_skips_cutscenes=0\n" },
    { "Modern", "Modern controls and looks: dual stick + mouse and keyboard, over-the-shoulder aim with a centred "
                "reticle, jump while aiming, slightly faster movement and camera, fixed sniper zoom, faster jetpack refuel, "
                "widescreen, no LOD or draw distance, light ambient occlusion and bloom. "
                "Original frame rate pacing; weapons as in the original.",
      "no_blood=0\nstart_skips_cutscenes=1\nshow_fps=0\nwidescreen=1\nframe_rate=0\nshow_level=0\nshow_pack=0\ndump_by_level=0\n"
      "no_lod=1\nfar_objects=1\nssao=1\nssao_strength=30\nssao_radius=139\nbloom=1\nbloom_intensity=40\nbloom_threshold=19\n"
      "bloom_radius=6\ndual_stick=1\ninvert_aim_y=0\nswap_triggers=0\ndual_layout=1\ninvert_aim_normal=0\ninvert_camera_y=0\naim_from_character=0\n"
      "move_speed=115\ndiagonal_boost=0\nstrafe_speed=100\ncamera_speed_h=134\ncamera_speed_v=107\naim_shoulder=1\naim_centered=1\naim_speed=92\n"
      "aim_jump=1\nmouse_keyboard=1\nmouse_camera_speed=100\nmouse_aim_speed=100\ninvert_mouse_y=0\nrefuel_speed=482\nsniper_fixed_zoom=1\nsniper_zoom=365\n"
      "damage_taken=100\ndive_speed=100\nfly_speed=100\nfly_freeze=1\nfly_hide_hud=1\n" },
    { "Modern and Balanced", "Modern's settings with the weapons rebalanced (damage, fire rate, range, spread, "
                             "bolt speed and size; the flamethrower is left as in the original).",
      "no_blood=0\nstart_skips_cutscenes=1\nshow_fps=0\nwidescreen=1\nframe_rate=0\nshow_level=0\nshow_pack=0\ndump_by_level=0\n"
      "no_lod=1\nfar_objects=1\nssao=1\nssao_strength=30\nssao_radius=139\nbloom=1\nbloom_intensity=40\nbloom_threshold=19\n"
      "bloom_radius=6\ndual_stick=1\ninvert_aim_y=0\nswap_triggers=0\ndual_layout=1\ninvert_aim_normal=0\ninvert_camera_y=0\naim_from_character=0\n"
      "move_speed=115\ndiagonal_boost=0\nstrafe_speed=100\ncamera_speed_h=134\ncamera_speed_v=107\naim_shoulder=1\naim_centered=1\naim_speed=92\n"
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
    { "Extreme", "Modern-style looks (ambient occlusion and bloom) at the original 30 fps, turned up: double movement speed with the "
                 "diagonal boost, fast camera and aim, and over-the-top weapons (rapid fire, big bolts, fast charge, short throws).",
      "start_skips_cutscenes=1\nframe_rate=0\nwidescreen=1\nno_lod=1\nfar_objects=1\n"
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
} // namespace

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

// ---------------------------------------------------------------------------------------------------------------------
// Settings API for the "Select" config menu (jfg_settings_api.h). Same data and files as the F9 tabs.
// ---------------------------------------------------------------------------------------------------------------------
#include "jfg_settings_api.h"

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
    ::choose_profile(name);
}

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
void set_texture_pack(const std::string& folder) { ::set_texture_pack(folder); }
int texture_pack_count() { return g_texture_pack_count.load(); }
int dumped_texture_files() {
    std::error_code ec; int n = 0;
    for (std::filesystem::recursive_directory_iterator it("texture_dump", ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_regular_file(ec)) n++;
    }
    return ec ? -1 : n;
}
} // namespace jfg::settings

// ---------------------------------------------------------------------------------------------------------------------
// F10: toolbar. The settings moved to the Select / Esc menu (Gameplay, Visuals, Profiles tabs); the developer tools
// that do not fit there are here: cheats and the profile / texture pack quick switches in menus, and Level warp,
// Items and Weapons in their own windows. F9 still opens the original all-in-one window.
// ---------------------------------------------------------------------------------------------------------------------
// Windows-style look (dark mode) for the bar and its drop-down menus: a dark bar with light text, a highlight on the
// menu under the mouse, drop-downs with a thin border, and thin vertical separators between the menus.
struct ToolbarTheme {
    int colors = 0, vars = 0;
    void color(ImGuiCol c, ImU32 v) { ImGui::PushStyleColor(c, v); colors++; }
    void var(ImGuiStyleVar s, float v) { ImGui::PushStyleVar(s, v); vars++; }
    void var(ImGuiStyleVar s, ImVec2 v) { ImGui::PushStyleVar(s, v); vars++; }
    ToolbarTheme() {
        color(ImGuiCol_MenuBarBg, IM_COL32(32, 32, 32, 255));
        color(ImGuiCol_PopupBg, IM_COL32(43, 43, 43, 255));
        color(ImGuiCol_Border, IM_COL32(75, 75, 75, 255));
        color(ImGuiCol_Text, IM_COL32(240, 240, 240, 255));
        color(ImGuiCol_TextDisabled, IM_COL32(150, 150, 150, 255));
        color(ImGuiCol_Header, IM_COL32(65, 65, 65, 255));
        color(ImGuiCol_HeaderHovered, IM_COL32(58, 58, 58, 255));
        color(ImGuiCol_HeaderActive, IM_COL32(70, 70, 70, 255));
        color(ImGuiCol_Separator, IM_COL32(70, 70, 70, 255));
        color(ImGuiCol_FrameBg, IM_COL32(30, 30, 30, 255));
        color(ImGuiCol_FrameBgHovered, IM_COL32(50, 50, 50, 255));
        color(ImGuiCol_FrameBgActive, IM_COL32(60, 60, 60, 255));
        color(ImGuiCol_CheckMark, IM_COL32(96, 205, 255, 255));
        color(ImGuiCol_SliderGrab, IM_COL32(96, 205, 255, 255));
        color(ImGuiCol_SliderGrabActive, IM_COL32(150, 220, 255, 255));
        color(ImGuiCol_Button, IM_COL32(55, 55, 55, 255));
        color(ImGuiCol_ButtonHovered, IM_COL32(68, 68, 68, 255));
        color(ImGuiCol_ButtonActive, IM_COL32(80, 80, 80, 255));
        color(ImGuiCol_ScrollbarBg, IM_COL32(43, 43, 43, 255));
        color(ImGuiCol_ScrollbarGrab, IM_COL32(100, 100, 100, 255));
        var(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
        var(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
        var(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        var(ImGuiStyleVar_PopupRounding, 0.0f);
        var(ImGuiStyleVar_PopupBorderSize, 1.0f);
        var(ImGuiStyleVar_FrameBorderSize, 1.0f);
        var(ImGuiStyleVar_FrameRounding, 0.0f);
    }
    ~ToolbarTheme() { ImGui::PopStyleVar(vars); ImGui::PopStyleColor(colors); }
};

// Thin vertical line between two menus of the bar.
static void toolbar_separator() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + 5.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, p.y + h * 0.8f), IM_COL32(85, 85, 85, 255), 1.0f);
    ImGui::Dummy(ImVec2(11.0f, h));
}

// A boxed "label  value" field on the right of the bar, so each value is clearly tied to its label.
static float toolbar_field_width(const char* label, const std::string& value) {
    return ImGui::CalcTextSize(label).x + ImGui::CalcTextSize(value.c_str()).x + 28.0f;
}
static void toolbar_field(const char* label, const std::string& value, ImU32 value_color = IM_COL32(240, 240, 240, 255)) {
    const float h = ImGui::GetFrameHeight();
    const float lw = ImGui::CalcTextSize(label).x, vw = ImGui::CalcTextSize(value.c_str()).x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = lw + vw + 28.0f, y0 = p.y + 3.0f, y1 = p.y + h - 3.0f, ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + lw + 14.0f, y1), IM_COL32(52, 52, 52, 255));
    dl->AddRectFilled(ImVec2(p.x + lw + 14.0f, y0), ImVec2(p.x + w, y1), IM_COL32(20, 20, 20, 255));
    dl->AddRect(ImVec2(p.x, y0), ImVec2(p.x + w, y1), IM_COL32(85, 85, 85, 255));
    dl->AddText(ImVec2(p.x + 7.0f, ty), IM_COL32(165, 165, 165, 255), label);
    dl->AddText(ImVec2(p.x + lw + 21.0f, ty), value_color, value.c_str());
    ImGui::Dummy(ImVec2(w, h));
}

void draw_toolbar(recomp_context* ctx) {
    static bool show_warp = false, show_items = false, show_weapons = false;
    const bool in_game = valid_ptr(rd32(ADDR_gameplay));
    ImGui::GetIO().FontGlobalScale = std::max(1.0f, ImGui::GetIO().DisplaySize.y / 720.0f);
    load_profile_choice();
    std::string current_pack;
    { std::lock_guard lock(g_pack_mutex); current_pack = g_pack_folder; }
    bool bar_open;
    float bar_height = 0.0f;
    {
        ToolbarTheme theme;
        bar_open = ImGui::BeginMainMenuBar();
        bar_height = ImGui::GetFrameHeight();
        g_toolbar_height = bar_height;
        if (bar_open) {
            if (ImGui::BeginMenu("Cheats")) {
                draw_cheats();
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Tools")) {
                ImGui::MenuItem("Level Warp...", nullptr, &show_warp);
                ImGui::MenuItem("Items...", nullptr, &show_items);
                ImGui::MenuItem("Weapons...", nullptr, &show_weapons);
                ImGui::Separator();
                if (ImGui::MenuItem("Fly Camera", "F7", g_fly.active)) g_fly_toggle_request = true;
                if (ImGui::MenuItem("Freeze While Flying", nullptr, &g.fly_freeze)) save_options();
                if (ImGui::MenuItem("Hide HUD While Flying", nullptr, &g.fly_hide_hud)) save_options();
                ImGui::Separator();
                ImGui::TextDisabled("The windows open on their own. Fly camera: WASD / left stick move,");
                ImGui::TextDisabled("mouse / right stick look, Space / A up, Ctrl / B down, fire = faster.");
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Profile")) {
                list_user_profiles();
                ImGui::TextDisabled("Built-in");
                for (const BuiltinProfile& p : BUILTIN_PROFILES) {
                    if (ImGui::MenuItem(p.name, nullptr, g_active_profile == p.name)) choose_profile(p.name);
                }
                if (!g_user_profiles.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Your profiles");
                }
                for (const std::string& n : std::vector<std::string>(g_user_profiles)) {
                    if (ImGui::MenuItem(n.c_str(), nullptr, g_active_profile == n)) choose_profile(n);
                }
                ImGui::Separator();
                ImGui::TextDisabled("Save and edit profiles in the Select / Esc menu > Profiles.");
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Textures")) {
                ImGui::TextDisabled("Texture pack");
                if (ImGui::MenuItem(pack_display_name("").c_str(), nullptr, current_pack.empty())) set_texture_pack("");
                for (const std::string& p : list_texture_packs()) {
                    if (ImGui::MenuItem(pack_display_name(p).c_str(), nullptr, p == current_pack)) set_texture_pack(p);
                }
                if (ImGui::MenuItem("Reload Pack", "F6: next pack")) set_texture_pack(current_pack);
                ImGui::Separator();
                ImGui::TextDisabled("Texture dumping (bin\\texture_dump)");
                ImGui::MenuItem("Dump Textures (this session)", nullptr, &g.texture_dump);
                if (ImGui::MenuItem("One Folder per Level", nullptr, &g.dump_by_level)) save_options();
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Game")) {
                if (ImGui::MenuItem("Back to the Title Screen", "F5")) g_reset_pending = true;
                ImGui::Separator();
                ImGui::TextDisabled("Settings: Select (controller) or Esc.");
                ImGui::TextDisabled("F9: the original debug window. F10: this toolbar.");
                ImGui::TextDisabled("F7: fly camera. F5: title screen. F6: next texture pack.");
                ImGui::TextDisabled("Alt or a click outside the toolbar: play with the mouse and keyboard; Alt again: back to the toolbar.");
                ImGui::EndMenu();
            }
            // Status fields on the right, each boxed with its label.
            const int level = (int16_t)rd16(ADDR_currentLevel);
            const std::string level_text = std::to_string(level) + " " +
                ((level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level] : std::string("?"));
            const std::string profile_text = g_active_profile.empty() ? std::string("(none)") : g_active_profile;
            const std::string pack_text = pack_display_name(current_pack);
            const bool game_input = dino::debug_ui::backend::toolbar_game_input();
            const std::string input_text = game_input ? "Game (Alt: toolbar)" : "Toolbar (Alt / click outside: game)";
            const float gap = 8.0f;
            const float total = toolbar_field_width("Mouse & keys", input_text) + toolbar_field_width("Profile", profile_text) +
                                toolbar_field_width("Pack", pack_text) + toolbar_field_width("Level", level_text) + gap * 4.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - total));
            toolbar_field("Mouse & keys", input_text, game_input ? IM_COL32(120, 220, 120, 255) : IM_COL32(96, 205, 255, 255));
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Profile", profile_text);
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Pack", pack_text);
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Level", level_text);
            ImGui::EndMainMenuBar();
        }
    }
    // A thin shadow line under the bar, like a window's menu bar border.
    if (bar_open) {
        ImGui::GetForegroundDrawList()->AddLine(ImVec2(0.0f, bar_height), ImVec2(ImGui::GetIO().DisplaySize.x, bar_height), IM_COL32(70, 70, 70, 255));
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto window = [&](const char* title, bool* open, float x, auto&& body) {
        if (!*open) return;
        ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, display.x * 0.45f), display.y * 0.75f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(x, ImGui::GetFrameHeight() * 2.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, open)) body();
        ImGui::End();
    };
    window("Level warp", &show_warp, 20.0f, [&]() { draw_warp(); });
    window("Items", &show_items, 60.0f, [&]() {
        if (in_game) draw_inventory(ctx); else ImGui::TextDisabled("No game loaded.");
    });
    window("Weapons", &show_weapons, display.x * 0.5f, [&]() { draw_weapons(); });
}
