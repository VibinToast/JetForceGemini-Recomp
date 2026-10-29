// Jet Force Gemini port: more enemies per squad (mod API jfg_squad_multiplier; decomp MSR 13).
//
// A squadron's wanted member count (squadron data +0x01, u8) comes from its list entry (+0x0B) in SquadronInit, its
// only writer. Each squadron also reserves one saved kill flag per member, in a row from +0x88 (squadsInitialiseAfter-
// Objects -> func_80051CCC: levelGetObjectID(+0x01)), out of a 128-bit block per level: placed members take the first
// ones (entry +0x18), generated members the next (+0x8C, counting up as they are made). A set flag means "killed
// before": the member is counted as made (+0x63) and not spawned, and a squadron with all its flags set opens its door
// at load. Keep Area Progress saves and restores those blocks.
//
// So the multiplied count must not change the flags: every squadron reserves at its stock count (+0x01 is raised only
// after squadsInitialiseAfterObjects, which also runs the door-at-load test on the stock range), and the extra members
// have none. Their ids run past the reservation into the next squadron's bits, so while a squadron makes a member
// (func_overlay_3_00014694) an id at or past +0x88 + stock reads as clear, and a member with such an id doesn't set it
// when it dies. The saved blocks then read the same at every multiplier, and in levels that would run out of flags
// (2x: two levels, 3x: five) nothing runs out. The extras are made for the stock members still alive at load (all of
// them, the first time), so a cleared squadron stays cleared after a load; extras killed before a load come back.
//
// Left at their count: Tribals (the rescue totals are per level), the bosses and big-scale types, type 0x2CE (no
// flags), kind 4 (no members), and sentries unless asked for (they would stand in a clump at one post). An extra member
// is also not made (counted as made, as a killed one is) when the object list is nearly full (512 objects, unchecked
// by the game), and one whose object could not be made is counted too, so its squadron still ends and opens its door.
// One made on top of another member is launched a short way out, so the extras don't stand stacked.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_per_mod.h"
#include "jfg_port.h"

extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp
extern "C" void squadsIsTribal(uint8_t* rdram, recomp_context* ctx);
extern "C" void levelObjectFlagSet(uint8_t* rdram, recomp_context* ctx);
extern "C" void BaddyTypeToIndex(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackGetBlock(uint8_t* rdram, recomp_context* ctx);

namespace jfg {

uint32_t kills_squaddie(); // jfg_kills.cpp: the member SquaddieControl is updating, else 0
bool fling_member(uint32_t obj, float vx, float vy, float vz, int calls); // jfg_hero_api.cpp (jfg_enemy_fling)

namespace {
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0, O3_SQUADRON = 0x1A1D8; // the squadron being worked on (its data)
// The squadron list entry SquadronInit reads (decomp MSR 13a, 13d).
constexpr uint32_t ENTRY_COUNT = 0x0B, ENTRY_IDLE_BEHAVIOUR = 0x0D, ENTRY_SCALE = 0x15, ENTRY_KIND = 0x29,
                   ENTRY_TYPE = 0x36;
// The squadron's data.
constexpr uint32_t SQD_WANTED = 0x01, SQD_CREATED = 0x63, SQD_FLAG_BASE = 0x88;
constexpr uint32_t SQ_FLAG = 0x57; // a member's kill flag id (u8)
// An extra member made on top of another (most spawn modes make every member at the squadron's spot) is launched
// a short way out, each in its own direction (the golden angle by its number), with the game's own launch: track
// collision, walls stop it, its AI back on when it lands.
constexpr float CLOSE = 40.0f, SPREAD_SPEED = 9.0f, SPREAD_HOP = 4.0f;
constexpr int SPREAD_CALLS = 10;
constexpr int OBJECTS_KEEP_FREE = 64; // of the 512 the object list holds: shots, effects and pickups need room

int g_percent = 100;     // 100..300, from the mod (jfg_squad_multiplier)
int g_flyer_percent = 100; // the same for flying squads (drones, flying insects): extra flyers drift where you can't reach
bool g_sentries = false; // multiply sentries too

struct Squadron {
    uint32_t data;  // the squadron's data
    int stock;      // its list entry's count
    int percent;    // its multiplier (flyers have their own)
    int wanted;     // the count it was given (stock until squadsInitialiseAfterObjects)
    bool flyers;    // a flying squad (its extras keep near the stock members' places)
    bool drones;    // of drones (posted at sniper points too): its extras fly beside a stock drone
};

// An extra drone flying beside a stock one (its anchor), side -1 left / 1 right times how many places out.
struct Wingman {
    uint32_t obj, anchor;
    int side;
};
std::vector<Wingman> g_wingmen;

// The drone member types (decomp MSR 13g: the flying rows of movement type 5 and 6 that are drones).
bool drone_type(uint16_t type) {
    switch (type) {
    case 0x2B: case 0x2D: case 0x54: case 0x149: case 0x150: case 0x151: case 0x152: case 0x153: return true;
    default: return false;
    }
}
std::vector<Squadron> g_squadrons; // this level's multiplied squadrons
bool g_after_objects = false;      // squadsInitialiseAfterObjects has run for this level
int g_making = 0;                  // inside func_overlay_3_00014694
bool g_making_extra = false;       // ... and the member it makes is an extra one

Squadron* find(uint32_t data) {
    for (Squadron& s : g_squadrons) if (s.data == data) return &s;
    return nullptr;
}

// Past the squadron's reserved flags: an extra member's id.
bool extra_id(const Squadron& s, uint32_t id) {
    const int32_t base = (int32_t)rd32(s.data + SQD_FLAG_BASE);
    return base >= 0 && (int)(id & 0xFF) - base >= s.stock;
}

bool tribal(uint8_t* rdram_, recomp_context* ctx, uint16_t type) {
    recomp_context c = *ctx;
    c.r4 = type;
    squadsIsTribal(rdram_, &c);
    return (uint32_t)c.r2 != 0;
}

// A flying squad (decomp MSR 13g): its members' baddy row (BaddyDataArray, 0x80 bytes a row, loaded at boot) moves
// them by type +0x2B, and only type 1 has gravity and lands; 0, 4, 5 and 6 seek their formation height. Turrets are 1.
constexpr uint32_t ADDR_BaddyDataArray = 0x800A38E0, ROW_SIZE = 0x80, ROW_MOVEMENT = 0x2B;
bool flying(uint8_t* rdram_, recomp_context* ctx, uint16_t type) {
    recomp_context c = *ctx;
    c.r4 = type;
    BaddyTypeToIndex(rdram_, &c);
    const int row = (int32_t)c.r2;
    const uint32_t rows = rd32(ADDR_BaddyDataArray);
    return row >= 0 && valid_ptr(rows) && rd8(rows + (uint32_t)row * ROW_SIZE + ROW_MOVEMENT) != 1;
}

bool multiplied(uint8_t* rdram_, recomp_context* ctx, uint32_t entry) {
    const uint16_t type = rd16(entry + ENTRY_TYPE);
    if (type == 0x2CE || rd8(entry + ENTRY_KIND) == 4 || rd8(entry + ENTRY_COUNT) == 0) return false;
    if (type == 0x67 || type == 0x144 || type == 0xE6) return false; // bosses
    if (rd8(entry + ENTRY_SCALE) >= 150) return false;               // big-scale types (0xF5, 0x6A, 0xF6)
    if (!g_sentries && rd8(entry + ENTRY_IDLE_BEHAVIOUR) == 9) return false;
    return !tribal(rdram_, ctx, type);
}
} // namespace

// From func_overlay_3_00014694's wrapper (jfg_engine_wrappers.cpp), around each member a squadron makes.
void squad_mult_making(bool on) {
    if (on) { g_making++; g_making_extra = false; }
    else if (g_making > 0) g_making--;
}

// After it: an extra member that could not be made still counts as made (else its squadron never ends).
// After it: an extra member that could not be made still counts as made (else its squadron never ends); one made on
// top of another is spread out.
void squad_mult_made(uint8_t* rdram_, recomp_context* ctx, uint8_t created_before) {
    g_rdram = rdram_;
    if (!g_making_extra) return;
    g_making_extra = false;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    const uint32_t data = base ? rd32(base + O3_SQUADRON) : 0;
    if (data == 0) return;
    const uint32_t obj = (uint32_t)ctx->r2;
    if (obj == 0) {
        if (rd8(data + SQD_CREATED) == created_before) wr8(data + SQD_CREATED, (uint8_t)(created_before + 1));
        return;
    }
    const float x = Obj::PosX::get(obj), z = Obj::PosZ::get(obj);
    // A drone squad's extra flies beside a stock drone: the one with the fewest beside it so far.
    if (const Squadron* s = find(data); s != nullptr && s->drones) {
        uint32_t best = 0;
        int best_count = 1 << 30;
        int guard = 0;
        for (uint32_t m = SquadronData::FirstMember::get(data); m != 0 && guard < 256; guard++) {
            bool extra = m == obj;
            for (const Wingman& w : g_wingmen) extra |= w.obj == m;
            if (!extra) {
                int n = 0;
                for (const Wingman& w : g_wingmen) n += w.anchor == m;
                if (n < best_count) { best_count = n; best = m; }
            }
            const uint32_t sq = Obj::Data::get(m);
            m = sq ? SquadMemberData::NextMember::get(sq) : 0;
        }
        if (best != 0) {
            const int side = (best_count % 2 == 0 ? 1 : -1) * (best_count / 2 + 1);
            g_wingmen.push_back({ obj, best, side });
            return;
        }
    }
    bool close = false;
    int guard = 0;
    for (uint32_t m = SquadronData::FirstMember::get(data); m != 0 && guard < 256; guard++) {
        if (m != obj && std::fabs(Obj::PosX::get(m) - x) < CLOSE && std::fabs(Obj::PosZ::get(m) - z) < CLOSE) {
            close = true;
            break;
        }
        const uint32_t sq = Obj::Data::get(m);
        m = sq ? SquadMemberData::NextMember::get(sq) : 0;
    }
    if (!close) return;
    const float angle = (float)created_before * 2.39996f;
    fling_member(obj, std::cos(angle) * SPREAD_SPEED, SPREAD_HOP, std::sin(angle) * SPREAD_SPEED, SPREAD_CALLS);
}

uint8_t squad_mult_created(uint8_t* rdram_) {
    g_rdram = rdram_;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    const uint32_t data = base ? rd32(base + O3_SQUADRON) : 0;
    return data ? rd8(data + SQD_CREATED) : 0;
}

} // namespace jfg

using namespace jfg;

// A new level's objects are about to be set up: its squadrons start over.
JFG_WRAP(squadsInitialiseBeforeObjects) {
    g_rdram = rdram;
    g_squadrons.clear();
    g_wingmen.clear();
    g_after_objects = false;
    squadsInitialiseBeforeObjects_orig(rdram, ctx);
}

// SquadronInit(obj, entry): note the squadrons to multiply. Their count stays stock until the flags are reserved; one
// set up again later gets back the count it was given.
JFG_WRAP(SquadronInit) {
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4, entry = (uint32_t)ctx->r5;
    SquadronInit_orig(rdram, ctx);
    const uint32_t data = Obj::Data::get(obj);
    if (data == 0) return;
    if (Squadron* s = find(data)) {
        if (g_after_objects) wr8(data + SQD_WANTED, (uint8_t)s->wanted);
        return;
    }
    if ((g_percent <= 100 && g_flyer_percent <= 100) || g_after_objects || !multiplied(rdram, ctx, entry)) return;
    const bool flyers = flying(rdram, ctx, rd16(entry + ENTRY_TYPE));
    const int percent = flyers ? g_flyer_percent : g_percent;
    if (percent <= 100) return;
    const int stock = rd8(entry + ENTRY_COUNT);
    g_squadrons.push_back({ data, stock, percent, stock, flyers, flyers && drone_type(rd16(entry + ENTRY_TYPE)) });
}

// After the flags are reserved and the door-at-load test ran: the extra members, for the stock members not killed.
JFG_WRAP(squadsInitialiseAfterObjects) {
    g_rdram = rdram;
    squadsInitialiseAfterObjects_orig(rdram, ctx);
    g_after_objects = true;
    int extras = 0;
    for (Squadron& s : g_squadrons) {
        const int32_t base = (int32_t)rd32(s.data + SQD_FLAG_BASE);
        int alive = s.stock;
        if (base >= 0) {
            for (int i = 0; i < s.stock; i++) {
                recomp_context c = *ctx;
                c.r4 = (gpr)(base + i);
                levelObjectFlagSet(rdram, &c);
                if ((uint32_t)c.r2 != 0) alive--;
            }
        }
        const int add = (alive * s.percent + 50) / 100 - alive;
        s.wanted = std::min(255, s.stock + std::max(0, add));
        wr8(s.data + SQD_WANTED, (uint8_t)s.wanted);
        extras += s.wanted - s.stock;
    }
    if (!g_squadrons.empty()) {
        std::fprintf(stderr, "[jfg] squad multiplier %d%%: level %d, %d squadrons, %d extra members\n", g_percent,
            (int)(int16_t)rd16(ADDR_currentLevel), (int)g_squadrons.size(), extras);
    }
}

// An extra member's flag id reads as clear while its squadron makes it (it belongs to another squadron), or as set
// (made, not spawned) when the object list is nearly full.
JFG_WRAP(levelObjectFlagSet) {
    g_rdram = rdram;
    if (g_making > 0 && !g_squadrons.empty()) {
        const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
        const Squadron* s = base ? find(rd32(base + O3_SQUADRON)) : nullptr;
        if (s && extra_id(*s, (uint32_t)ctx->r4)) {
            const bool full = (int32_t)rd32(ADDR_ObjListCount) >= 512 - OBJECTS_KEEP_FREE;
            g_making_extra = !full;
            ctx->r2 = full ? 1 : 0;
            return;
        }
    }
    levelObjectFlagSet_orig(rdram, ctx);
}

namespace jfg {
namespace {
constexpr float APART = 45.0f, WALL_GAP = 30.0f, WING = 70.0f;
// How far from (ax, ay, az) along (dx, dz) (horizontal, up to len) before a level surface, less WALL_GAP; len if clear.
// trackNearestIntersection is the crosshair's test of the level.
float clear_reach(recomp_context* ctx, float ax, float ay, float az, float dx, float dz, float len) {
    if (len <= 0.0f) return 0.0f;
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // in the gap call_game leaves below $sp
    const float reach = len + WALL_GAP;
    wrf(buf + 0, ax); wrf(buf + 4, ay); wrf(buf + 8, az);
    wrf(buf + 12, ax + dx * reach); wrf(buf + 16, ay); wrf(buf + 20, az + dz * reach);
    const recomp_context saved = *ctx;
    const bool hit = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
    *ctx = saved;
    if (!hit) return len;
    const float hx = rdf(buf + 28) - ax, hz = rdf(buf + 36) - az;
    const float d = std::sqrt(hx * hx + hz * hz);
    return std::isfinite(d) ? std::clamp(d - WALL_GAP, 0.0f, len) : 0.0f;
}

bool in_rdram(uint32_t p) { return p >= 0x80000000u && p < 0x80800000u && (p & 3) == 0; }
} // namespace

// After a squad member's SquaddieControl (jfg_frame_rate.cpp): an extra drone is put beside its stock drone, to its
// left or right as that one faces (Obj yaw: forward sin/cos, so its right is (cos, -sin)), at its height, only as far
// as the level lets it (and its track segment found again, as for anything the port moves). The stock drone gone
// or dead, the extra is on its own.
void squad_mult_member_moved(uint8_t* rdram_, recomp_context* ctx, uint32_t obj) {
    if (g_wingmen.empty()) return;
    g_rdram = rdram_;
    for (size_t i = 0; i < g_wingmen.size(); i++) {
        Wingman& w = g_wingmen[i];
        if (w.obj != obj) continue;
        const uint32_t a = w.anchor;
        const uint32_t hit = valid_ptr(a) && Obj::Kind::get(a) == 0x18 ? Obj::Damage::get(a) : 0;
        const bool alive = valid_ptr(obj) && Obj::Kind::get(obj) == 0x18 && valid_ptr(hit) && (int16_t)rd16(hit + 0x06) >= 0x100;
        if (!alive) { g_wingmen.erase(g_wingmen.begin() + (long)i); return; }
        const float yaw = (float)Obj::Yaw::get(a) * (6.2831853f / 65536.0f);
        float rx = std::cos(yaw), rz = -std::sin(yaw);
        if (w.side < 0) { rx = -rx; rz = -rz; }
        const float ax = Obj::PosX::get(a), ay = Obj::PosY::get(a), az = Obj::PosZ::get(a);
        const float reach = clear_reach(ctx, ax, ay, az, rx, rz, WING * (float)std::abs(w.side));
        Obj::PosX::set(obj, ax + rx * reach);
        Obj::PosY::set(obj, ay);
        Obj::PosZ::set(obj, az + rz * reach);
        const recomp_context saved = *ctx;
        ctx->f12.u32l = rd32(obj + 0x0C);
        ctx->f14.u32l = rd32(obj + 0x10);
        const int32_t block = (int32_t)call_game(trackGetBlock, ctx, { 0u, 0u, rd32(obj + 0x14) });
        *ctx = saved;
        if (block >= 0) Obj::Segment::set(obj, (int16_t)block);
        return;
    }
}
} // namespace jfg

// squadsGetFormationPosition(squadron, slot, *x, *y, *z): where a member heads when it isn't on a mission. A flying
// squad's extra member has a slot past the formation the level was laid out for, which put flyers inside trees and
// walls. Such an extra (slot from the stock count up to the count wanted; the squadron's own calls use other slots)
// takes a stock member's slot and stands a little apart from it, its own way (the golden angle by its slot), only as
// far as the level allows. Walkers keep the game's slots (their squads run about through them).
JFG_WRAP(squadsGetFormationPosition) {
    g_rdram = rdram;
    const uint32_t squadron = (uint32_t)ctx->r4;
    const int slot = (int)(ctx->r5 & 0xFF);
    const Squadron* s = !g_squadrons.empty() && valid_ptr(squadron) ? find(Obj::Data::get(squadron)) : nullptr;
    const uint32_t px = (uint32_t)ctx->r6, py = (uint32_t)ctx->r7;
    const uint32_t pz = in_rdram((uint32_t)ctx->r29 + 0x10) ? rd32((uint32_t)ctx->r29 + 0x10) : 0;
    if (s == nullptr || !s->flyers || s->stock <= 0 || slot < s->stock || slot >= s->wanted ||
        !in_rdram(px) || !in_rdram(py) || !in_rdram(pz)) {
        squadsGetFormationPosition_orig(rdram, ctx);
        return;
    }
    ctx->r5 = (gpr)(slot % s->stock);
    squadsGetFormationPosition_orig(rdram, ctx);
    g_rdram = rdram;
    const gpr result = ctx->r2;
    const float bx = rdf(px), by = rdf(py), bz = rdf(pz);
    if (!std::isfinite(bx) || !std::isfinite(by) || !std::isfinite(bz)) return;
    const float angle = (float)slot * 2.39996f, dx = std::cos(angle), dz = std::sin(angle);
    const float reach = clear_reach(ctx, bx, by, bz, dx, dz, APART * (float)(slot / s->stock));
    wrf(px, bx + dx * reach);
    wrf(pz, bz + dz * reach);
    ctx->r2 = result;
}

// An extra member dying leaves the flags alone.
JFG_WRAP(levelSetObjectFlag) {
    g_rdram = rdram;
    const uint32_t member = kills_squaddie();
    if (member != 0 && !g_squadrons.empty()) {
        const uint32_t sq = Obj::Data::get(member);
        const uint32_t squadron = sq ? SquadMemberData::Squadron::get(sq) : 0;
        const Squadron* s = squadron ? find(Obj::Data::get(squadron)) : nullptr;
        if (s && (uint32_t)(ctx->r4 & 0xFF) == rd8(sq + SQ_FLAG) && extra_id(*s, (uint32_t)ctx->r4)) return;
    }
    levelSetObjectFlag_orig(rdram, ctx);
}

namespace jfg {
// For the drop pool (jfg_drops.cpp): a member of a multiplied squadron drops its stock size / its size now as often.
float squad_mult_drop_scale(uint32_t member) {
    const uint32_t sq = valid_ptr(member) ? Obj::Data::get(member) : 0;
    const uint32_t squadron = valid_ptr(sq) ? SquadMemberData::Squadron::get(sq) : 0;
    const Squadron* s = valid_ptr(squadron) ? find(Obj::Data::get(squadron)) : nullptr;
    return s && s->wanted > s->stock ? (float)s->stock / (float)s->wanted : 1.0f;
}
} // namespace jfg

// jfg_squad_multiplier(int percent, int flags, int flyer_percent): squads made from now on (the next level loaded)
// have percent / 100 times their members (100..300), flying squads flyer_percent / 100; flags 1: sentries too. Each
// mod's request is kept: the largest applies, and sentries are multiplied when any mod asks.
namespace { PerMod<int> g_percent_requests, g_flyer_requests; PerMod<bool> g_sentry_requests; }
void squad_multiplier_export(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_percent_requests.set(mod_index, std::clamp((int)(int32_t)ctx->r4, 100, 300), 100);
    g_flyer_requests.set(mod_index, std::clamp((int)(int32_t)ctx->r6, 100, 300), 100);
    g_sentry_requests.set(mod_index, ((int32_t)ctx->r5 & 1) != 0 && (int32_t)ctx->r4 > 100, false);
    g_percent = g_percent_requests.max(100);
    g_flyer_percent = g_flyer_requests.max(100);
    g_sentries = g_sentry_requests.any();
}
