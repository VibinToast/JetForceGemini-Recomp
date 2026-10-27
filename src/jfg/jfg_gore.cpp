// Jet Force Gemini port: the gore options - blood splats and severed body parts that stay, and more blood.
//
// Blood (overlay 8): bloodSpurtAdd puts a drop into a pool of 64 entries (0x54 bytes each). A drop flies (state +0x4A
// = 1), lands (2) and lies as a splat (3) whose life (+0x3C, s16, 300 at the start) bloodSpurtUpdateAll counts down by
// the update rate; at 0 the entry is freed. When all 64 entries are in use (count at overlay + 0x17A0) bloodSpurtAdd
// drops the new blood. "Blood stays" keeps the life of every splat full (bloodSpurtUpdateAll's wrapper sets it before
// the game counts it down), and when the pool is full, frees the oldest splats for the new drops (jfg_on_blood_spurt
// handler, before bloodSpurtAdd reads the count), so new blood always appears and the oldest stains make room. Drops
// still flying or splashing are never taken. With the option off the game's own behaviour is unchanged.
// The splats that make room are not lost: each is copied into a host list (Blood Splat Limit - 64 at most, a new level
// empties it: jfg_on_level_change handler), and bloodSpurtsDraw(&gfx, &mtx, &vtx), which draws every entry of the pool
// by its state, is run again for them (its wrapper), 64 at a time: the list's newest are put into the pool, drawn, and
// the pool is put back. The level's own frame buffers are sized for its normal drawing (listReallocate), so the passes
// write into blocks of their own in the second 8 MB of RDRAM, which the game never uses, and the frame's display list
// calls them. When a block is full the oldest kept splats are not drawn that frame (2 MB each: far more than
// SPLATS_KEPT need).
//
// Body parts (objects.c / spranim.c): a severed limb is an object (behaviour severedLimbControl, data at object + 0x68)
// with a life timer (data + 0x14, s16, counted down by twice the update rate) after which it fades out (object + 0x39)
// and is freed. Limbs whose type (data + 0x17) is 2 or more are pickups (drone heads) and are left alone. "Body parts
// stay" keeps the timer of the newest LIMBS_KEPT limbs up (jfg_on_severed_limb_tick handler); limbs are objects
// with models of their own, so the older ones still go, to keep the object heap from filling up.
//
// "No blood" and "Blood amount": handlers of jfg_on_blood_spurt / jfg_on_blood_hit (jfg_api.cpp). bloodSpurtAdd's
// wrapper makes the extra drops of the event's amount with the same arguments and a little spread in their direction
// and speed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <unordered_map>

#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" void bloodSpurtAdd_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void bloodSpurtUpdateAll_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void bloodSpurtsDraw_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace jfg {

void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp

namespace {
constexpr int16_t SPLAT_LIFE = 300;    // what bloodSpurtAdd starts an entry with
constexpr int LIMBS_KEPT = 24;
constexpr int16_t LIMB_LIFE = 600;
// The blood pool: overlay 8 (ROM 0x01EFF168) + 0x17C0, 64 entries of 0x54 bytes; +0x4A is the state, 3 = a splat. The
// count of entries in use is at +0x17A0; +0x16C0 is set once bloodSpurtAdd has set the pool up.
constexpr uint32_t OVERLAY_8_ROM = 0x01EFF168, BLOOD_POOL = 0x17C0, BLOOD_COUNT = 0x17A0, BLOOD_READY = 0x16C0;
constexpr int BLOOD_ENTRIES = 64; // BloodEntry[BLOOD_ENTRIES]
constexpr uint8_t BLOOD_SPLAT = 3; // state: lying on the ground or a wall

// Something seen on consecutive ticks, by address: when it first appeared, and when it was last seen (a gap means the
// address now holds a new one).
struct Seen { uint32_t first = 0, last = 0; };

struct Tracker {
    std::unordered_map<uint32_t, Seen> seen;
    int level = -1;
};

// True if `addr` is among the `keep` newest of the tracker (updated with this tick's sighting); entries gone for a
// while are dropped.
bool keep_newest(Tracker& t, uint32_t addr, int keep) {
    const uint32_t tick = g_tick_count;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != t.level) { t.seen.clear(); t.level = level; } // a new level frees all of them
    auto& seen = t.seen;
    Seen& s = seen[addr];
    if (s.last == 0 || tick - s.last > 2) s.first = tick;
    s.last = tick;
    int newer = 0;
    for (auto it = seen.begin(); it != seen.end();) {
        if (tick - it->second.last > 30) { it = seen.erase(it); continue; }
        if (it->first != addr && it->second.first > s.first) newer++;
        ++it;
    }
    return newer < keep;
}

Tracker g_splats, g_limbs;
uint32_t g_blood_recycled = 0; // splats freed for new blood, for the test log

// The splats freed for new blood, oldest first, as the pool entry's words, and the level they were made in.
constexpr uint32_t BLOOD_ENTRY_WORDS = BloodEntry::Size / 4;
constexpr size_t SPLATS_KEPT = 4096 - BLOOD_ENTRIES; // the most Blood Splat Limit (blood_splats) keeps besides the pool
size_t kept_limit() { return (size_t)std::clamp(g.blood_splats - BLOOD_ENTRIES, 0, (int)SPLATS_KEPT); }
struct KeptSplat { uint32_t words[BLOOD_ENTRY_WORDS]; };
std::deque<KeptSplat> g_kept_splats;
int g_kept_level = -1;
int g_extra_passes = 0, g_extra_drawn = 0; // in the last bloodSpurtsDraw, for the test log

// Where overlay 8 is now (0 = not loaded).
uint32_t g_blood_base = 0;
uint32_t blood_count() { return g_blood_base + BLOOD_COUNT; }
uint32_t blood_entry(int i) { return g_blood_base + BLOOD_POOL + (uint32_t)i * BloodEntry::Size; }

// Tests: JFG_DEBUG_BLOOD=1 writes the pool's use to jfg_memory.log about once a second while blood is updated.
void blood_log() {
    static const bool on = std::getenv("JFG_DEBUG_BLOOD") != nullptr;
    static uint32_t last = 0;
    if (!on || g_blood_base == 0 || g_tick_count - last < 60) return;
    last = g_tick_count;
    int splats = 0, moving = 0;
    for (int i = 0; i < BLOOD_ENTRIES; i++) {
        const uint8_t state = BloodEntry::State::get(blood_entry(i));
        if (state == BLOOD_SPLAT) splats++;
        else if (state != 0) moving++;
    }
    char line[224];
    std::snprintf(line, sizeof(line), "blood: %d splats, %d flying or splashing, count %d, %u recycled so far, %d kept, %d of them "
        "drawn in %d extra passes (level %d)", splats, moving, (int)rd32(blood_count()), g_blood_recycled, (int)g_kept_splats.size(),
        g_extra_drawn, g_extra_passes, (int)(int16_t)rd16(ADDR_currentLevel));
    jfg_log_line("jfg_memory.log", line);
}

// Frees the oldest splat (as bloodSpurtUpdateAll frees one: state 0, count - 1) into the kept list. A splat the
// tracker has not seen yet counts as the oldest. False if there is none to take (all flying or splashing).
bool free_oldest_splat() {
    uint32_t oldest = 0, oldest_first = UINT32_MAX;
    for (int i = 0; i < BLOOD_ENTRIES; i++) {
        const uint32_t entry = blood_entry(i);
        if (BloodEntry::State::get(entry) != BLOOD_SPLAT) continue;
        const auto it = g_splats.seen.find(entry);
        const uint32_t first = it == g_splats.seen.end() ? 0 : it->second.first;
        if (first < oldest_first) { oldest_first = first; oldest = entry; }
    }
    if (oldest == 0) return false;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != g_kept_level) { g_kept_splats.clear(); g_kept_level = level; }
    KeptSplat kept;
    for (uint32_t w = 0; w < BLOOD_ENTRY_WORDS; w++) kept.words[w] = rd32(oldest + w * 4);
    if (kept_limit() > 0) g_kept_splats.push_back(kept);
    while (g_kept_splats.size() > kept_limit()) g_kept_splats.pop_front();
    BloodEntry::State::set(oldest, 0);
    const int32_t count = (int32_t)rd32(blood_count());
    if (count > 0) wr32(blood_count(), (uint32_t)(count - 1));
    g_splats.seen.erase(oldest); // the new drop in this entry is a new splat when it lands
    g_blood_recycled++;
    return true;
}

// Two blocks in the second 8 MB of RDRAM, which the game never uses (lib/rt64 keeps it readable for this microcode:
// JFG's addresses, see rt64_state.h), one per frame as the game's own buffers (the frame drawn now may still be read
// while the next one is made), each with the kept splats' display lists in its first half and their vertices in the
// second. bloodSpurtsDraw runs once per camera, so the calls of one frame go one after the other in its block (from the
// start again at camera 0).
constexpr uint32_t ADDR_frame = 0x800FD7C0;           // which of the two buffers the game draws into now
constexpr uint32_t BLOCKS[2] = { 0x80C00000, 0x80E00000 };
constexpr uint32_t BLOCK_SIZE = 0x200000;
constexpr uint32_t G_DL = 0x06000000, G_ENDDL = 0xB8000000;
} // namespace

// "Blood stays", before bloodSpurtUpdateAll counts the splats' lives down: every splat is kept; the tracker only
// records which is oldest, for make_room_for_blood.
void keep_splats() {
    if (!g.gore_keep_blood) return;
    g_blood_base = jfg_overlay_loaded_at(OVERLAY_8_ROM);
    if (g_blood_base == 0) return;
    for (int i = 0; i < BLOOD_ENTRIES; i++) {
        const uint32_t entry = blood_entry(i);
        if (BloodEntry::State::get(entry) != BLOOD_SPLAT) continue;
        keep_newest(g_splats, entry, BLOOD_ENTRIES);
        BloodEntry::Life::set(entry, SPLAT_LIFE);
    }
    blood_log();
}

// jfg_on_blood_spurt handler (after No blood and Blood amount's), "Blood stays": with every entry in use, free the
// oldest splats so the spurt's drops (one, or the event's amount in percent) get entries. With no splat to take the
// drops are dropped, as in the game.
extern "C" void jfg_gore_make_room(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t blood = (uint32_t)ctx->r4;
    if (rd32(blood + api::blood_spurt::CANCEL) != 0) return;
    g_blood_base = jfg_overlay_loaded_at(OVERLAY_8_ROM);
    if (g_blood_base == 0 || rd32(g_blood_base + BLOOD_READY) == 0) return; // bloodSpurtAdd sets the pool up first
    const int32_t amount = (int32_t)rd32(blood + api::blood_spurt::AMOUNT);
    const int drops = amount > 100 ? std::min((amount + 99) / 100, BLOOD_ENTRIES) : 1;
    int free = 0;
    for (int i = 0; i < BLOOD_ENTRIES; i++) if (BloodEntry::State::get(blood_entry(i)) == 0) free++;
    free = std::min(free, BLOOD_ENTRIES - (int)(int32_t)rd32(blood_count()));
    while (free < drops && free_oldest_splat()) free++;
}

// jfg_on_level_change handler: the kept splats go with the level.
extern "C" void jfg_gore_level_change(uint8_t* rdram_, recomp_context* ctx) {
    (void)rdram_; (void)ctx;
    g_kept_splats.clear();
}

// bloodSpurtsDraw's wrapper, before the game's code: draws the kept splats (see the top of the file) in extra passes
// of the function, before it draws the pool. The passes write into this frame's block, which the frame's display list
// calls.
void draw_kept_splats(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_blood || g.no_blood) {
        g_kept_splats.clear();
        g_extra_passes = g_extra_drawn = 0;
        return;
    }
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != g_kept_level) g_kept_splats.clear();
    while (g_kept_splats.size() > kept_limit()) g_kept_splats.pop_front(); // the limit was lowered
    if (g_kept_splats.empty()) { g_extra_passes = g_extra_drawn = 0; return; }
    g_blood_base = jfg_overlay_loaded_at(OVERLAY_8_ROM);
    if (g_blood_base == 0) return;

    // Where this frame's block is filled up to (a new frame starts at its beginning).
    static uint32_t cursor_frame = 0xFFFFFFFFu, gfx_cursor = 0, vtx_cursor = 0;
    const uint32_t frame = rd32(ADDR_frame) & 1;
    const uint32_t block = BLOCKS[frame];
    if (frame != cursor_frame || rd32(ADDR_cameraNo) == 0) { // camera 0 draws first in every frame
        cursor_frame = frame;
        gfx_cursor = block;
        vtx_cursor = block + BLOCK_SIZE / 2;
        g_extra_passes = g_extra_drawn = 0;
    }
    const uint32_t gfx_end = block + BLOCK_SIZE / 2 - 8, vtx_end = block + BLOCK_SIZE; // 8: the G_ENDDL
    // The most one pass of 64 has written, in bytes (a guess until one is seen).
    static uint32_t pass_gfx = 64 * 8 * 8 + 0x100, pass_vtx = 64 * 6 * 10;
    if (gfx_cursor + pass_gfx > gfx_end || vtx_cursor + pass_vtx > vtx_end) return;

    // The frame's list calls this part of the block; the passes write into it.
    const uint32_t gfx_at = (uint32_t)ctx->r4, vtx_at = (uint32_t)ctx->r6;
    const uint32_t main_gfx = rd32(gfx_at), main_vtx = rd32(vtx_at);
    wr32(main_gfx, G_DL);
    wr32(main_gfx + 4, gfx_cursor);
    wr32(gfx_at, gfx_cursor);
    wr32(vtx_at, vtx_cursor);

    uint32_t pool[BLOOD_ENTRIES * BLOOD_ENTRY_WORDS];
    const uint32_t pool_at = blood_entry(0);
    for (uint32_t w = 0; w < BLOOD_ENTRIES * BLOOD_ENTRY_WORDS; w++) pool[w] = rd32(pool_at + w * 4);
    size_t next = g_kept_splats.size(); // newest first
    while (next > 0 && rd32(gfx_at) + pass_gfx <= gfx_end && rd32(vtx_at) + pass_vtx <= vtx_end) {
        const int n = (int)std::min<size_t>(next, BLOOD_ENTRIES);
        for (int i = 0; i < BLOOD_ENTRIES; i++) {
            const uint32_t entry = blood_entry(i);
            if (i < n) {
                const KeptSplat& k = g_kept_splats[next - n + i];
                for (uint32_t w = 0; w < BLOOD_ENTRY_WORDS; w++) wr32(entry + w * 4, k.words[w]);
                BloodEntry::State::set(entry, BLOOD_SPLAT);
            } else {
                BloodEntry::State::set(entry, 0);
            }
        }
        const uint32_t g0 = rd32(gfx_at), v0 = rd32(vtx_at);
        recomp_context c = *ctx;
        bloodSpurtsDraw_orig(rdram_, &c);
        pass_gfx = std::max(pass_gfx, rd32(gfx_at) - g0);
        pass_vtx = std::max(pass_vtx, rd32(vtx_at) - v0);
        g_extra_passes++;
        g_extra_drawn += n;
        next -= n;
    }
    for (uint32_t w = 0; w < BLOOD_ENTRIES * BLOOD_ENTRY_WORDS; w++) wr32(pool_at + w * 4, pool[w]);

    const uint32_t end = rd32(gfx_at);
    wr32(end, G_ENDDL);
    wr32(end + 4, 0);
    gfx_cursor = end + 8;
    vtx_cursor = rd32(vtx_at);
    wr32(gfx_at, main_gfx + 8);
    wr32(vtx_at, main_vtx);
}

// jfg_on_severed_limb_tick(limb, frames) handler.
extern "C" void jfg_gore_limb(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_limbs) return;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    if (!valid_ptr(data) || LimbData::Type::get(data) >= 2) return;
    if (keep_newest(g_limbs, obj, LIMBS_KEPT) && LimbData::Life::get(data) > 0) LimbData::Life::set(data, LIMB_LIFE);
}

// jfg_on_blood_spurt / jfg_on_blood_hit handlers: "No blood" cancels, "Blood amount" scales the drops.
extern "C" void jfg_gore_on_blood_spurt(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t blood = (uint32_t)ctx->r4;
    if (jfg_no_blood()) { wr32(blood + api::blood_spurt::CANCEL, 1); return; }
    const int32_t amount = (int32_t)rd32(blood + api::blood_spurt::AMOUNT);
    wr32(blood + api::blood_spurt::AMOUNT, (uint32_t)(amount * g.blood_amount / 100));
}

extern "C" void jfg_gore_on_blood_hit(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (jfg_no_blood()) wr32((uint32_t)ctx->r4 + api::blood_hit::CANCEL, 1);
}

// bloodSpurtAdd's wrapper (jfg_api.cpp), before the game's own drop: (amount - 100) percent extra drops with the same
// arguments (x, y in $f12/$f14, z and vx in $a2/$a3, vy and vz at $sp + 0x10 / 0x14) and a little spread.
extern "C" void jfg_blood_extra_drops(uint8_t* rdram_, recomp_context* ctx, int amount) {
    if (amount <= 100) return;
    g_rdram = rdram_;
    static std::mt19937 rng{ 0x4A464721u };
    std::uniform_real_distribution<float> spread(0.75f, 1.25f), chance(0.0f, 1.0f);
    const float extra_f = (float)(amount - 100) / 100.0f;
    int extra = (int)extra_f;
    if (chance(rng) < extra_f - (float)extra) extra++;
    auto as_float = [](uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; };
    auto as_bits = [](float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; };
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t dy_bits = rd32(sp + 0x10), dz_bits = rd32(sp + 0x14);
    for (int i = 0; i < extra; i++) {
        recomp_context c = *ctx;
        c.r7 = (gpr)(int32_t)as_bits(as_float((uint32_t)ctx->r7) * spread(rng));
        wr32(sp + 0x10, as_bits(as_float(dy_bits) * spread(rng)));
        wr32(sp + 0x14, as_bits(as_float(dz_bits) * spread(rng)));
        bloodSpurtAdd_orig(rdram_, &c);
    }
    wr32(sp + 0x10, dy_bits);
    wr32(sp + 0x14, dz_bits);
}

} // namespace jfg

JFG_WRAP(bloodSpurtUpdateAll) {
    jfg::g_rdram = rdram;
    jfg::keep_splats();
    bloodSpurtUpdateAll_orig(rdram, ctx);
}

JFG_WRAP(bloodSpurtsDraw) {
    jfg::draw_kept_splats(rdram, ctx);
    bloodSpurtsDraw_orig(rdram, ctx);
}
