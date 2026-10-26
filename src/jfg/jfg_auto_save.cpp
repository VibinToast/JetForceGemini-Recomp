// Jet Force Gemini port: auto save.
//
// The game only saves when the player presses Z on the pause screen (overlay 18) or when a level's stats screen shows
// a new record (overlay 14). Both call mainSaveGame(game->slot), which only sets a request in load_save_flags; the
// next joyRead writes the whole Game struct (progress, flags, the characters' health, weapons and ammo, and the level
// and entrance each character resumes at, which mainChangeLevel updates on every level change) to the save's flash
// sectors. The runtime then writes the flash contents to the save file on its own thread within a few milliseconds.
//
// The auto save makes the same request: a few seconds after arriving in a level, and (optionally) every few minutes of
// play. Only when a manual save would also be possible and sensible: single-player gameplay, not paused, no cutscene,
// race, Floyd mission or level change under way, the character alive, and the file has a valid slot. Any save the game
// makes itself restarts the timer.
//
// The slot byte alone does not show that a file is in play: after start-up it reads 0 (file 1) whatever was chosen, so
// a warp from the title screen (F9, JFG_DEBUG_WARP) would save over file 1. The file-select screen (overlay 42) reads
// all six files for its list (mainLoadAllGames) and puts the chosen one into play by copying it; that request is made
// and carried out while the screen is set up, inside one tick, so it is never seen here. What every way into a game
// does go through is character select (level 127): after choosing a file, and after a new file's story intro. So a
// file counts as in play from character select (or a save the game makes) until the front end's level 0 (the logo
// screens and the file-select menus) or a port warp from outside gameplay (auto_save_forget_file); a port warp straight
// to character select does not count.
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

std::chrono::steady_clock::time_point g_save_toast; // when the last auto save was made (on-screen note)

namespace {
constexpr uint32_t ADDR_load_save_flags = 0x800A32A8; // u32: save / load requests, carried out by joyRead
constexpr uint32_t ADDR_racingInGame = 0x800A4FB4;    // u8
constexpr uint32_t SAVE_REQUEST = 0x40000000;         // mainSaveGame's request bit
constexpr uint32_t DELETE_REQUEST = 0x20000000;       // mainDeleteGame (slot in bits 6-8)
constexpr uint32_t ERASE_REQUESTS = 0x10000000 | 0x01000000; // mainResetSaveGames, mainEraseFlashRom
constexpr int LEVEL_FRONT_END = 0; // the logo screens and the file-select menus (in front-end game mode)
constexpr int SAVE_SLOTS = 6;
constexpr float SETTLE_SECONDS = 3.0f; // after arriving: the door / respawn camera has settled, nothing is changing

using clock = std::chrono::steady_clock;

bool g_file_in_play = false;   // a file was chosen (character select reached) or saved, see above
bool g_port_warped = false;    // the port warped in from outside gameplay since the file-select menus

// The save file the game is played from, or a negative reason when a save now would not be one a player could make.
int save_slot(recomp_context* ctx) {
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0) return -1; // front end, multiplayer, paused
    if (g.skipping || in_cutscene() || g_fly.active) return -2;
    if (rd8(ADDR_robotMission) != 0 || rd8(ADDR_racingInGame) != 0) return -3;
    if (rd16(ADDR_currentLevel) != rd16(ADDR_nextLevel)) return -4; // a level change is under way
    if (rd32(ADDR_load_save_flags) != 0) return -5;                  // the game is saving or loading this tick
    const uint32_t game = rd32(ADDR_gameplay);
    if (!valid_ptr(game)) return -6;
    const int slot = Game::SaveSlot::get(game);
    if (slot < 0 || slot >= SAVE_SLOTS) return -7;
    const uint32_t obj = player_object(ctx);
    if (obj == 0) return -8;
    const uint32_t hit = Obj::Damage::get(obj); // the character's damage data (see apply_damage_taken)
    if (!valid_ptr(hit) || Damage::Health::get(hit) <= 0) return -9;
    return slot;
}
} // namespace

// A port warp into a level from the title or the front end: the Game struct may not be any file's.
void auto_save_forget_file() { g_file_in_play = false; g_port_warped = true; area_progress_disarm(); }
bool auto_save_file_in_play() { return g_file_in_play; }

// Game thread, at the start of the main loop tick (jfg_debug_frame), before this tick's joyRead.
void auto_save_tick(recomp_context* ctx) {
    static int saved_level = -1;     // the level the last save (auto or manual) was made in
    static clock::time_point last_save = clock::now();
    static clock::time_point safe_since{};
    static bool safe = false;
    const auto now = clock::now();

    area_progress_tick();
    const uint32_t requests = rd32(ADDR_load_save_flags); // made last tick, carried out by this tick's joyRead
    if (requests & SAVE_REQUEST) { // Z on the pause screen, a record, a new file
        last_save = now;
        saved_level = (int16_t)rd16(ADDR_currentLevel);
        area_progress_on_save((int)((requests >> 3) & 7));
        tribal_saves_on_save((int)((requests >> 3) & 7));
        worldmap_on_save((int)((requests >> 3) & 7));
    }
    if (requests & DELETE_REQUEST) { area_progress_forget((int)((requests >> 6) & 7)); tribal_saves_forget((int)((requests >> 6) & 7)); worldmap_forget((int)((requests >> 6) & 7)); }
    if (requests & ERASE_REQUESTS) { area_progress_forget(-1); tribal_saves_forget(-1); worldmap_forget(-1); }
    const int current = (int16_t)rd16(ADDR_currentLevel);
    if (current == LEVEL_FRONT_END && rd32(ADDR_mainGameMode) != 0) { g_file_in_play = false; g_port_warped = false; }
    if ((current == LEVEL_CHARACTER_SELECT && !g_port_warped) || (requests & SAVE_REQUEST)) {
        if (!g_file_in_play) { area_progress_arm(); tribal_saves_file_chosen(); worldmap_file_chosen(); } // a file was just chosen: its first region entry may be restored
        g_file_in_play = true;
    }
    const int slot = (g.auto_save && g_file_in_play) ? save_slot(ctx) : 0;
    {
        // JFG_DEBUG_AUTOSAVE=1: once a second, why an auto save would or would not be made now (and every request seen).
        static const bool log = std::getenv("JFG_DEBUG_AUTOSAVE") != nullptr;
        static clock::time_point last_log{};
        if (log && requests != 0) std::fprintf(stderr, "[jfg] auto save: game request 0x%08X\n", requests);
        if (log && now - last_log >= std::chrono::seconds(1)) {
            last_log = now;
            std::fprintf(stderr, "[jfg] auto save: option %d, file in play %d, slot/reason %d, level %d, mode %d\n", int(g.auto_save),
                int(g_file_in_play), slot, (int)(int16_t)rd16(ADDR_currentLevel), (int)rd32(ADDR_mainGameMode));
        }
    }
    if (!g.auto_save || !g_file_in_play) { safe = false; return; }
    if (slot < 0) { safe = false; return; }
    if (!safe) { safe = true; safe_since = now; }
    if (std::chrono::duration<float>(now - safe_since).count() < SETTLE_SECONDS) return;

    const int level = (int16_t)rd16(ADDR_currentLevel);
    const bool arrived = level != saved_level;
    const bool timed = g.auto_save_minutes > 0 && now - last_save >= std::chrono::minutes(g.auto_save_minutes);
    if (!arrived && !timed) return;

    call_game(mainSaveGame, ctx, { (uint32_t)slot }); // carried out by joyRead later in this tick
    area_progress_on_save(slot);
    tribal_saves_on_save(slot);
    worldmap_on_save(slot);
    std::fprintf(stderr, "[jfg] auto save: file %d, level %d (%s)\n", slot + 1, level, arrived ? "arrived" : "timer");
    saved_level = level;
    last_save = now;
    g_save_toast = now;
}

} // namespace jfg
