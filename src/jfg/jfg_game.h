// Jet Force Gemini port: access to the game itself, shared by the port's files in src/jfg.
//
// The port works by reading and writing the game's own variables in emulated RDRAM, or by calling the game's own
// (recompiled) functions - no game code is replaced. Addresses are the US 1.0 ones from
// ver/symbols/symbol_addrs.us.txt of the decomp.
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
#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <vector>

#include "recomp.h"

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
void n_alEvtqPostEvent(uint8_t* rdram, recomp_context* ctx);
void func_8002CC70_2D870(uint8_t* rdram, recomp_context* ctx); // the follow camera's wall check, see camera_collide()
void fb_swap(uint8_t* rdram, recomp_context* ctx);
void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx);
void osRecvMesg_recomp(uint8_t* rdram, recomp_context* ctx);
}

namespace jfg {

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
constexpr uint32_t ADDR_currentScreen        = 0x800FECB0; // s32*, the buffer the next frame is drawn into

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

// The story intro after creating a file (levels 322-327) and where START takes it to (jfg_after_joy).
constexpr uint32_t ADDR_waitMidiSync = 0x800A326C, ADDR_midiSyncReached = 0x800E9774; // see the story skip in jfg_debug_frame
constexpr int LEVEL_STORY_FIRST = 322, LEVEL_STORY_LAST = 327, LEVEL_CHARACTER_SELECT = 127;

// A Floyd mission (the player flies Floyd; `robotMission`, u8, is what the game itself checks to show the mission's
// pause menu). The control and camera options are built around the character, so they are all off during one.
constexpr uint32_t ADDR_robotMission = 0x800A3208;
constexpr uint32_t ADDR_sControllerCurrData = 0x800FB0C0; // OSContPad[4]: u16 button, s8 stick_x, s8 stick_y, u8 errno (6 bytes)
constexpr uint32_t ADDR_cameraActorArray = 0x800FA4D0;   // Camera[], 0x4C bytes each (src/camera.h)
constexpr uint32_t ADDR_cameraNo = 0x800FA634;           // index of the active camera
constexpr uint32_t ADDR_delayDat = 0x800A3374;           // video frames the current tick simulates
constexpr uint32_t CAMERA_SIZE = 0x4C;

const char* const character_names[] = { "Vela", "Juno", "Lupus" };

// --- RDRAM access (words are stored host-endian, so bytes and halves need the usual address swizzle) --------------
extern uint8_t* g_rdram; // the game's RDRAM (jfg_game.cpp); every hook sets it
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

constexpr float PI_F = 3.14159265f;
inline float angle_from_s16(uint16_t v) { return (float)(int16_t)v * (PI_F / 32768.0f); }
inline uint16_t angle_to_s16(float a) { return (uint16_t)(int32_t)std::lround(a * (32768.0f / PI_F)); }
inline float wrap_angle(float a) { while (a > PI_F) a -= 2 * PI_F; while (a < -PI_F) a += 2 * PI_F; return a; }
inline float rdf(uint32_t addr) { const uint32_t v = rd32(addr); float f; std::memcpy(&f, &v, 4); return f; }
inline void wrf(uint32_t addr, float f) { uint32_t v; std::memcpy(&v, &f, 4); wr32(addr, v); }

// Calls a recompiled game function with up to six integer arguments on the current (game) thread and returns $v0.
// The caller's register state is saved and restored, and the callee gets its own stack frame below the current one.
uint32_t call_game(void (*func)(uint8_t*, recomp_context*), recomp_context* ctx, std::initializer_list<uint32_t> args);

// Level names: the developers' internal ones, read from the ROM once (jfg_game.cpp).
extern std::vector<std::string> g_level_names;
void load_level_names(); // called from the game thread and the UI thread

// Characters (jfg_game.cpp).
uint32_t character_ptr(int character);              // game character struct of Vela (0), Juno (1), Lupus (2); 0 = no game
uint32_t player_object(recomp_context* ctx);        // the first player's object, or 0
bool select_weapon(recomp_context* ctx, int slot);  // puts that weapon in the current character's hand
void set_owned_weapons(uint32_t ch, uint16_t owned); // owned-weapon mask and count

bool contains_ci(const std::string& s, const char* needle); // case-insensitive substring test

} // namespace jfg
