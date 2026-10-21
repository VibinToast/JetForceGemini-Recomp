// Jet Force Gemini port: "Keep Area Progress" - loading a file made in the middle of an area (region) keeps what was
// done there: Tribals rescued and killed, cleared rooms, continues, and the room the file was saved in.
//
// What a region holds while it is played:
//   - in the Game struct, so already in the game's save: the Tribal bits of each room (+0x5C rescued, +0xDC killed,
//     indexed by the level header's regionFlag), the counts (+4 rescued, +5 killed, +6 left), the region time (+0x514),
//     +0x530, and the weapon stats the region's stats screen shows (+0x474..+0x513);
//   - in RAM only: the object flags of each of the region's rooms (levelSetObjectFlag / levelObjectFlagSet,
//     D_800FB1E0: 32 x { u8 id counter, u8 bits[16] }), which mark killed squad members, so cleared rooms stay clear
//     and kill-all doors stay open; `continues`; and where a continue restarts (D_800A3270..D_800A327C).
// The game calls mainResetRegion whenever the level it changes to is in another region than D_800FD7A4 (RAM, so
// after loading a file always), clearing all of the above. The character's resume point (game character +0x74:
// level << 5 | entrance) is only set on entering a region, so a file always restarts at the region's entrance.
//
// With the option on, every save (auto save or the game's own) also writes, for that file, the region's RAM part and a
// copy of its Game-struct part to saves/<save name>_area.bin, and sets the resume point to the room the player is in.
// On the first region entry after a file was chosen (not after a game over, which clears the region on purpose), the
// Game-struct part is compared with that copy just before mainResetRegion clears it. If they match, the file is the one
// the entry was written with, and everything is put back after the reset (continues and the continue point one tick
// later, since the caller sets them after mainResetRegion returns).
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "ultramodern/ultramodern.hpp"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

namespace {
constexpr uint32_t ADDR_currentRegion = 0x800FD7A4;   // s16 D_800FD7A4: the region being played (0 / 0xFF: none)
constexpr uint32_t ADDR_continuePoint = 0x800A3270;   // s16 x4: level, arg, entrance, region a continue restarts at
constexpr uint32_t ADDR_racingInGame = 0x800A4FB4;    // u8
constexpr uint32_t ADDR_roomFlags = 0x800FB1E0;       // 32 x 17 bytes
constexpr int ROOMS = 32, ROOM_STRIDE = 17, ROOM_BITS = 16;
constexpr uint32_t GAME_SLOT_OFFSET = 0x03, GAME_REGION_TIME = 0x514, CHAR_RESUME_OFFSET = 0x74;
constexpr int SAVE_SLOTS = 6;

// The Game-struct ranges mainResetRegion clears, except the region time (cleared by its caller just before it).
struct Range { uint32_t offset, size; };
constexpr Range GAME_RANGES[] = { { 0x004, 3 }, { 0x05C, 0x100 }, { 0x474, 0xA0 }, { 0x530, 2 } };
constexpr uint32_t GAME_BYTES = 3 + 0x100 + 0xA0 + 2;

struct Entry {
    uint8_t valid = 0;
    int16_t region = 0;
    int16_t continues = 0;
    int16_t continue_point[4] = {};
    uint32_t region_time = 0;
    uint8_t game[GAME_BYTES] = {};
    uint8_t rooms[ROOMS][ROOM_BITS] = {};
};
constexpr uint32_t FILE_MAGIC = 0x4A464741; // "JFGA"
constexpr uint32_t FILE_VERSION = 1;

Entry g_entries[SAVE_SLOTS];
bool g_loaded = false;
bool g_armed = false;          // the next region entry is the first since a file was chosen
uint8_t g_snapshot[GAME_BYTES]; // the Game-struct part as it was when mainResetRegion was entered
bool g_post_pending = false;   // continues and the continue point still to be put back (next tick)
int g_post_slot = -1;

std::filesystem::path file_path() {
    std::filesystem::path save = ultramodern::get_save_file_path();
    return save.parent_path() / (save.stem().u8string() + u8"_area.bin");
}

void load_file() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(file_path(), std::ios::binary);
    uint32_t magic = 0, version = 0;
    if (!f.read(reinterpret_cast<char*>(&magic), 4) || !f.read(reinterpret_cast<char*>(&version), 4)) return;
    if (magic != FILE_MAGIC || version != FILE_VERSION) return;
    Entry entries[SAVE_SLOTS];
    if (f.read(reinterpret_cast<char*>(entries), sizeof(entries))) std::memcpy(g_entries, entries, sizeof(entries));
}

void write_file() {
    const std::filesystem::path path = file_path();
    std::filesystem::path tmp = path;
    tmp += u8".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t header[2] = { FILE_MAGIC, FILE_VERSION };
        f.write(reinterpret_cast<const char*>(header), sizeof(header));
        f.write(reinterpret_cast<const char*>(g_entries), sizeof(g_entries));
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

void read_game_bytes(uint32_t game, uint8_t* out) {
    for (const Range& r : GAME_RANGES)
        for (uint32_t i = 0; i < r.size; i++) *out++ = rd8(game + r.offset + i);
}
void write_game_bytes(uint32_t game, const uint8_t* in) {
    for (const Range& r : GAME_RANGES)
        for (uint32_t i = 0; i < r.size; i++) wr8(game + r.offset + i, *in++);
}

bool real_region(int region) { return region != 0 && region != 0xFF; }

uint32_t shots_fired(uint32_t game) { // the region's weaponShots, for the log
    uint32_t n = 0;
    for (int i = 0; i < 16; i++) n += rd32(game + 0x474 + 4 * i);
    return n;
}
} // namespace

// A file was chosen in the front end (auto_save_tick): its first region entry may be restored.
void area_progress_arm() { g_armed = true; }
// A port warp from outside gameplay: no file to restore for.
void area_progress_disarm() { g_armed = false; g_post_pending = false; }

// The game deletes a file (slot >= 0) or all of them (-1): their entries no longer belong to anything.
void area_progress_forget(int slot) {
    load_file();
    bool changed = false;
    for (int i = 0; i < SAVE_SLOTS; i++) {
        if ((slot < 0 || i == slot) && g_entries[i].valid) { g_entries[i] = Entry{}; changed = true; }
    }
    if (changed) write_file();
}

// Game thread, start of the tick, when a save to `slot` will be carried out by this tick's joyRead (the game's own
// request or the auto save's). Records the region for that file and points the resume point at the current room.
void area_progress_on_save(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    load_file();
    const uint32_t game = rd32(ADDR_gameplay);
    Entry e{};
    const int region = (int16_t)rd16(ADDR_currentRegion);
    // The region's stats screen (a new record saves) is shown by mainChangeLevel on the way into another region, so
    // its save has the next level differing from the current one: the region has ended and nothing is kept.
    const bool in_region = g.keep_area_progress && valid_ptr(game) && rd32(ADDR_mainGameMode) == 0 && real_region(region) &&
                           rd8(ADDR_racingInGame) == 0 && rd16(ADDR_currentLevel) == rd16(ADDR_nextLevel);
    {
        static const bool log = std::getenv("JFG_DEBUG_AUTOSAVE") != nullptr;
        if (log) std::fprintf(stderr, "[jfg] area progress: save to file %d: option %d, mode %d, region %d, racing %d, level %d/%d\n",
            slot + 1, int(g.keep_area_progress), (int)rd32(ADDR_mainGameMode), region, (int)rd8(ADDR_racingInGame),
            (int)(int16_t)rd16(ADDR_currentLevel), (int)(int16_t)rd16(ADDR_nextLevel));
    }
    if (in_region) {
        e.valid = 1;
        e.region = (int16_t)region;
        e.continues = (int16_t)rd16(ADDR_continues);
        for (int i = 0; i < 4; i++) e.continue_point[i] = (int16_t)rd16(ADDR_continuePoint + 2 * i);
        e.region_time = rd32(game + GAME_REGION_TIME);
        read_game_bytes(game, e.game);
        for (int r = 0; r < ROOMS; r++)
            for (int b = 0; b < ROOM_BITS; b++) e.rooms[r][b] = rd8(ADDR_roomFlags + r * ROOM_STRIDE + 1 + b);
        // Resume in this room: the level and the entrance it was entered by, as mainChangeLevel stores it on
        // entering a region.
        const int level = (int16_t)rd16(ADDR_currentLevel), entrance = (int16_t)rd16(ADDR_currentEntrance);
        const uint32_t ch = character_ptr((int16_t)rd16(ADDR_currentCharacter) & 3);
        if (ch != 0 && level >= 0 && level < 2048 && entrance >= 0 && entrance < 32)
            wr16(ch + CHAR_RESUME_OFFSET, (uint16_t)((level << 5) | entrance));
        std::fprintf(stderr, "[jfg] area progress: kept for file %d (region %d, level %d, entrance %d, %u shots, %d rescued)\n", slot + 1, region,
            level, entrance, shots_fired(game), (int)rd8(game + 4));
    }
    if (!e.valid && !g_entries[slot].valid) return; // nothing kept before, nothing now
    g_entries[slot] = e;
    write_file();
}

// Game thread, start of the tick: continues and the continue point after a restore.
void area_progress_tick() {
    if (!g_post_pending) return;
    g_post_pending = false;
    const Entry& e = g_entries[g_post_slot];
    wr16(ADDR_continues, (uint16_t)e.continues);
    for (int i = 0; i < 4; i++) wr16(ADDR_continuePoint + 2 * i, (uint16_t)e.continue_point[i]);
}

// Entry of mainResetRegion. D_800FD7A4 already holds the region being entered.
extern "C" void jfg_region_reset_begin(uint8_t* rdram_, recomp_context* ctx) {
    (void)ctx;
    g_rdram = rdram_;
    const uint32_t game = rd32(ADDR_gameplay);
    if (valid_ptr(game)) read_game_bytes(game, g_snapshot);
}

namespace {
// The region is cleared; put the kept progress back if it is this file's.
void restore_area_progress() {
    const bool armed = g_armed;
    g_armed = false; // only the first region entry after choosing a file (a game over clears the region on purpose)
    if (!armed || !g.keep_area_progress) return;
    const uint32_t game = rd32(ADDR_gameplay);
    if (!valid_ptr(game)) return;
    const int slot = (int8_t)rd8(game + GAME_SLOT_OFFSET);
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    load_file();
    const Entry& e = g_entries[slot];
    const int region = (int16_t)rd16(ADDR_currentRegion);
    if (!e.valid || e.region != region) return;
    if (std::memcmp(e.game, g_snapshot, GAME_BYTES) != 0) {
        std::fprintf(stderr, "[jfg] area progress: file %d does not match what was kept (saved elsewhere?), not restored\n", slot + 1);
        return;
    }
    write_game_bytes(game, e.game);
    wr32(game + GAME_REGION_TIME, e.region_time);
    for (int r = 0; r < ROOMS; r++)
        for (int b = 0; b < ROOM_BITS; b++) wr8(ADDR_roomFlags + r * ROOM_STRIDE + 1 + b, e.rooms[r][b]);
    g_post_pending = true;
    g_post_slot = slot;
    std::fprintf(stderr, "[jfg] area progress: restored for file %d (region %d, %d rescued, %d killed, %u shots)\n", slot + 1, region,
        (int)rd8(game + 4), (int)rd8(game + 5), shots_fired(game));
}
} // namespace

// mainResetRegion, before it returns: the region is cleared. Kept area progress first, then the permanent rescues.
extern "C" void jfg_region_reset_end(uint8_t* rdram_, recomp_context* ctx) {
    (void)ctx;
    g_rdram = rdram_;
    restore_area_progress();
    tribal_saves_region_reset();
}

} // namespace jfg

// mainResetRegion clears an area's progress when it is entered: before it, the progress loaded with the file is
// noted; after it, it's put back when Keep Area Progress is on and it matches what was kept with that save.
JFG_WRAP(mainResetRegion) {
    jfg_region_reset_begin(rdram, ctx);
    mainResetRegion_orig(rdram, ctx);
    jfg_region_reset_end(rdram, ctx);
}
