// Jet Force Gemini port: "Permanent Tribal Saves" - a Tribal once rescued stays rescued: going back into its area,
// a game over or loading the file no longer brings it back to be rescued again.
//
// What the game keeps of a rescue: mainSetTribalStatus sets the Tribal's bit in the Game struct (u16 per room at +0x5C,
// indexed by the level header's regionFlag, bit = squadron identifier & 15) and counts it (+4 rescued, +6 left); then
// squadsKillSquaddie removes the Tribal, which sets its object flag in the room's flags (D_800FB1E0[regionFlag], RAM),
// so it is not spawned again (func_overlay_3_00014694 checks the flag before each squad member). mainResetRegion
// clears both whenever a different region is entered, which after loading a file is always.
//
// With the option on, each rescue is noticed at the start of the next tick (a new bit at +0x5C), together with the
// object flags the room gained in the ticks around it (the Tribal's), and kept for the file in play: in memory at once,
// and in saves/<save name>_tribals.bin whenever the game saves to that file. After every mainResetRegion the kept
// rescues of the region being entered are put back: the bits, the counts, and the object flags, so they stay away.
// Choosing a file reloads what was saved for it (rescues made after the last save are lost, as with the game's save).
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>

#include "ultramodern/ultramodern.hpp"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

namespace {
constexpr uint32_t ADDR_currentRegion = 0x800FD7A4; // s16: the region being played (0 / 0xFF: none)
constexpr uint32_t ADDR_roomFlags = 0x800FB1E0;     // 32 x { u8 id counter, u8 bits[16] }, by regionFlag
constexpr int ROOMS = 32, ROOM_STRIDE = 17, ROOM_BITS = 16, SAVE_SLOTS = 6;
constexpr int CAPTURE_TICKS = 3; // ticks after a rescue in which the room's new object flags are taken as the Tribal's

struct Region {
    uint16_t rescued[ROOMS] = {};
    uint8_t flags[ROOMS][ROOM_BITS] = {};
};
using Store = std::map<std::pair<int, int>, Region>; // (slot, region)

constexpr uint32_t FILE_MAGIC = 0x4A464754; // "JFGT"
constexpr uint32_t FILE_VERSION = 1;

Store g_disk;   // as saved
Store g_memory; // as played
bool g_loaded = false;

// The last tick's view of the current room, to notice rescues.
struct Watch {
    int level = -1, region = -1, room = -1, slot = -1;
    uint16_t rescued = 0;
    uint8_t flags[ROOM_BITS] = {};
    int capture = 0; // ticks left in which new object flags belong to a rescue
} g_watch;

std::filesystem::path file_path() {
    std::filesystem::path save = ultramodern::get_save_file_path();
    return save.parent_path() / (save.stem().u8string() + u8"_tribals.bin");
}

void load_file() {
    if (g_loaded) return;
    g_loaded = true;
    g_disk.clear();
    std::ifstream f(file_path(), std::ios::binary);
    uint32_t header[3] = {};
    if (!f.read(reinterpret_cast<char*>(header), sizeof(header)) || header[0] != FILE_MAGIC || header[1] != FILE_VERSION) return;
    for (uint32_t i = 0; i < header[2]; i++) {
        int32_t key[2];
        Region r;
        if (!f.read(reinterpret_cast<char*>(key), sizeof(key)) || !f.read(reinterpret_cast<char*>(&r), sizeof(r))) break;
        g_disk[{ key[0], key[1] }] = r;
    }
}

void write_file() {
    const std::filesystem::path path = file_path();
    std::filesystem::path tmp = path;
    tmp += u8".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t header[3] = { FILE_MAGIC, FILE_VERSION, (uint32_t)g_disk.size() };
        f.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (const auto& [key, r] : g_disk) {
            const int32_t k[2] = { key.first, key.second };
            f.write(reinterpret_cast<const char*>(k), sizeof(k));
            f.write(reinterpret_cast<const char*>(&r), sizeof(r));
        }
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

// Replaces one slot's regions in `to` with those in `from`.
void copy_slot(const Store& from, Store& to, int slot) {
    for (auto it = to.begin(); it != to.end();) it = it->first.first == slot ? to.erase(it) : std::next(it);
    for (const auto& [key, r] : from) if (key.first == slot) to[key] = r;
}

bool real_region(int region) { return region != 0 && region != 0xFF; }

int current_slot(uint32_t game) {
    if (!auto_save_file_in_play()) return -1; // a port warp from the title: the Game struct is no file's
    const int slot = Game::SaveSlot::get(game);
    return (slot >= 0 && slot < SAVE_SLOTS) ? slot : -1;
}
} // namespace

// A file was chosen in the front end: what was saved for it is what counts.
void tribal_saves_file_chosen() {
    load_file();
    g_memory = g_disk;
    g_watch = Watch{};
}

// A save to `slot` is carried out this tick: its rescues are saved with it.
void tribal_saves_on_save(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    load_file();
    copy_slot(g_memory, g_disk, slot);
    write_file();
}

// The game deletes a file (slot >= 0) or all of them (-1).
void tribal_saves_forget(int slot) {
    load_file();
    for (int i = 0; i < SAVE_SLOTS; i++) {
        if (slot >= 0 && i != slot) continue;
        copy_slot(Store{}, g_disk, i);
        copy_slot(Store{}, g_memory, i);
    }
    write_file();
}

// Game thread, start of the tick: notices rescues in the current room.
void tribal_saves_tick() {
    const uint32_t game = rd32(ADDR_gameplay), header = rd32(ADDR_levelHeader);
    if (!g.permanent_tribals || !valid_ptr(game) || !valid_ptr(header) || rd32(ADDR_mainGameMode) != 0) { g_watch = Watch{}; return; }
    const int region = (int16_t)rd16(ADDR_currentRegion), room = LevelHeader::Room::get(header);
    const int level = (int16_t)rd16(ADDR_currentLevel), slot = current_slot(game);
    if (!real_region(region) || room >= ROOMS || slot < 0 || level != (int16_t)rd16(ADDR_nextLevel)) { g_watch = Watch{}; return; }
    const uint16_t rescued = Game::TribalsRescued::get(game, room);
    uint8_t flags[ROOM_BITS];
    for (int b = 0; b < ROOM_BITS; b++) flags[b] = rd8(ADDR_roomFlags + room * ROOM_STRIDE + 1 + b);
    Watch& w = g_watch;
    if (w.level == level && w.region == region && w.room == room && w.slot == slot) {
        Region& kept = g_memory[{ slot, region }];
        const uint16_t added = rescued & ~w.rescued;
        if (added != 0) {
            kept.rescued[room] |= added;
            w.capture = CAPTURE_TICKS;
            std::fprintf(stderr, "[jfg] tribal saves: rescued 0x%04X in room %d of region %d (file %d)\n", added, room, region, slot + 1);
        }
        if (w.capture > 0) {
            for (int b = 0; b < ROOM_BITS; b++) kept.flags[room][b] |= flags[b] & ~w.flags[b];
            w.capture--;
        }
    }
    w.level = level; w.region = region; w.room = room; w.slot = slot;
    w.rescued = rescued;
    std::memcpy(w.flags, flags, sizeof(flags));
}

// mainResetRegion, before it returns (jfg_region_reset_end): the region being entered gets its kept rescues back.
void tribal_saves_region_reset() {
    g_watch = Watch{};
    const uint32_t game = rd32(ADDR_gameplay);
    if (!g.permanent_tribals || !valid_ptr(game)) return;
    const int region = (int16_t)rd16(ADDR_currentRegion), slot = current_slot(game);
    if (!real_region(region) || slot < 0) return;
    const auto it = g_memory.find({ slot, region });
    if (it == g_memory.end()) return;
    const Region& kept = it->second;
    int restored = 0;
    for (int room = 0; room < ROOMS; room++) {
        const uint32_t rescued_addr = game + Game::TribalsRescued::offset + 2 * room;
        const uint16_t taken = rd16(rescued_addr) | Game::TribalsKilled::get(game, room);
        const uint16_t add = kept.rescued[room] & ~taken;
        for (int bit = 0; bit < 16; bit++) {
            if (!(add & (1 << bit))) continue;
            restored++;
            Game::TribalsRescuedCount::set(game, Game::TribalsRescuedCount::get(game) + 1);
            if (Game::TribalsLeftCount::get(game) > 0) Game::TribalsLeftCount::set(game, Game::TribalsLeftCount::get(game) - 1);
        }
        wr16(rescued_addr, rd16(rescued_addr) | add);
        for (int b = 0; b < ROOM_BITS; b++) {
            const uint32_t a = ADDR_roomFlags + room * ROOM_STRIDE + 1 + b;
            wr8(a, rd8(a) | kept.flags[room][b]);
        }
    }
    if (restored > 0) std::fprintf(stderr, "[jfg] tribal saves: %d rescued Tribals kept in region %d (file %d)\n", restored, region, slot + 1);
}

} // namespace jfg
