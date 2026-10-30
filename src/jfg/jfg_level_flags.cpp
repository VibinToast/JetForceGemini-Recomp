// Jet Force Gemini port: the level object (kill) flags, 256 per room instead of the game's 128 (JFG-170; decomp MSR 13,
// MSR 16 section 4).
//
// The game keeps 32 room blocks (the level header's +0x103 picks one), each a u8 counter (the next free id) and 128
// flags. A level hands out ids from 0 on every load, in object-load order (levelGetObjectID): each squadron reserves
// one per member, each crate one. A set flag means killed or broken before, so it stays that way when the room is
// entered again. A pack level with many squad members ran out at 128, and the failed id (-1) stored in a u8 member
// field became 255, which the game's flag functions then wrote past the block with.
//
// The port keeps the blocks itself, with 256 flags and a 16-bit counter, and hands out ids 0-254: 255 stays "no flag"
// (what a failed id reads as in a u8) and the flag functions ignore ids out of range. A stock level never uses more
// than 102, so its ids and flags are the same as the game's. Keep Area Progress saves the blocks
// (jfg_area_progress.cpp). levelObjectFlagSet and levelSetObjectFlag are wrapped in jfg_squad_multiplier.cpp, which
// calls these.
//
// JFG_COLLISION_CHECK=1 also runs the game's own five functions on the game's blocks and logs where an answer differs
// (stderr): in a stock level none should.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"

extern "C" {
void levelObjectFlagSet_orig(uint8_t* rdram, recomp_context* ctx);
void levelSetObjectFlag_orig(uint8_t* rdram, recomp_context* ctx);
}

namespace jfg {

constexpr int ROOM_BLOCKS = 32, ROOM_FLAG_BYTES = 32;
constexpr int ROOM_IDS = 255; // ids 0-254; 255 = "no flag"

namespace {
struct RoomBlock {
    uint16_t next_id = 0;
    uint8_t flags[ROOM_FLAG_BYTES] = {};
};
RoomBlock g_blocks[ROOM_BLOCKS];
int g_room = -1; // the block of the level being played, -1 = none

bool in_range(int32_t id) { return id >= 0 && id < ROOM_IDS; }

bool check_on() {
    static const bool on = [] { const char* v = std::getenv("JFG_COLLISION_CHECK"); return v && v[0] == '1'; }();
    return on;
}
int g_diffs = 0;
// The game's own version on a copy of the registers; logs when its answer isn't `port`.
void check(void (*orig)(uint8_t*, recomp_context*), uint8_t* rdram, const recomp_context* ctx, const char* what, int32_t port) {
    recomp_context c = *ctx;
    orig(rdram, &c);
    if (port != INT32_MIN && (int32_t)c.r2 != port && g_diffs++ < 20)
        std::fprintf(stderr, "[jfg] kill flags check: %s(%d) is %d, the game's %d (level %d)\n", what, (int32_t)ctx->r4, port,
            (int32_t)c.r2, (int16_t)rd16(ADDR_currentLevel));
}
} // namespace

// The flags of room block `room` (ROOM_FLAG_BYTES), for Keep Area Progress.
uint8_t* room_flags(int room) { return room >= 0 && room < ROOM_BLOCKS ? g_blocks[room].flags : nullptr; }

// levelObjectFlagSet(id): was this object killed (broken) before? Never in a level whose header's +0x102 is 0 or 0xFF.
bool flag_is_set(int32_t id) {
    const uint32_t header = rd32(ADDR_levelHeader);
    if (!valid_ptr(header) || g_room < 0 || !in_range(id)) return false;
    const uint8_t region = LevelHeader::Region::get(header);
    if (region == 0 || region == 0xFF) return false;
    return (g_blocks[g_room].flags[id >> 3] >> (id & 7)) & 1;
}
bool level_flag_is_set(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const bool set = flag_is_set((int32_t)ctx->r4);
    if (check_on()) check(levelObjectFlagSet_orig, rdram, ctx, "levelObjectFlagSet", set ? 1 : 0);
    return set;
}

// levelSetObjectFlag(id).
void level_flag_set(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const int32_t id = (int32_t)ctx->r4;
    if (check_on()) check(levelSetObjectFlag_orig, rdram, ctx, "levelSetObjectFlag", INT32_MIN);
    if (g_room < 0 || !in_range(id)) return;
    g_blocks[g_room].flags[id >> 3] |= (uint8_t)(1u << (id & 7));
}

} // namespace jfg

// A new game or a region reset: every block cleared.
JFG_WRAP(levelInitRegionFlags) {
    if (jfg::check_on()) {
        if (jfg::g_room >= 0)
            std::fprintf(stderr, "[jfg] kill flags check: room %d had %d ids, %d answers differed (region reset)\n", jfg::g_room,
                (int)jfg::g_blocks[jfg::g_room].next_id, jfg::g_diffs);
        levelInitRegionFlags_orig(rdram, ctx);
    }
    for (jfg::RoomBlock& b : jfg::g_blocks) b = {};
}

// A level loads: its room's block, ids from 0 again.
JFG_WRAP(levelGetRegionFlags) {
    jfg::g_rdram = rdram;
    if (jfg::check_on()) {
        if (jfg::g_room >= 0)
            std::fprintf(stderr, "[jfg] kill flags check: room %d had %d ids, %d answers differed\n", jfg::g_room,
                (int)jfg::g_blocks[jfg::g_room].next_id, jfg::g_diffs);
        recomp_context c = *ctx;
        levelGetRegionFlags_orig(rdram, &c);
    }
    const uint32_t header = jfg::rd32(jfg::ADDR_levelHeader);
    const int room = jfg::valid_ptr(header) ? jfg::LevelHeader::Room::get(header) : jfg::ROOM_BLOCKS;
    if (room >= jfg::ROOM_BLOCKS) {
        jfg::g_room = -1;
        return;
    }
    jfg::g_room = room;
    jfg::g_blocks[room].next_id = 0;
}

// levelGetObjectID(n): the first of n ids in a row, or -1 when the room has no block or not enough ids are left.
static int32_t get_object_id(int32_t n) {
    if (jfg::g_room < 0) return -1;
    jfg::RoomBlock& b = jfg::g_blocks[jfg::g_room];
    const int32_t first = b.next_id, next = first + n;
    if (next > jfg::ROOM_IDS) return -1;
    b.next_id = (uint16_t)next;
    return first;
}

JFG_WRAP(levelGetObjectID) {
    jfg::g_rdram = rdram;
    const int32_t id = get_object_id((int32_t)ctx->r4);
    if (jfg::check_on()) jfg::check(levelGetObjectID_orig, rdram, ctx, "levelGetObjectID", id);
    ctx->r2 = (gpr)id;
}
