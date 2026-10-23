// Jet Force Gemini port: the gore options - blood splats and severed body parts that stay, and more blood.
//
// Blood (overlay 8): bloodSpurtAdd puts a drop into a pool of 64 entries (0x54 bytes each). A drop flies (state +0x4A
// = 1), lands (2) and lies as a splat (3) whose life (+0x3C, s16, 300 at the start) bloodSpurtUpdateAll counts down by
// the update rate; at 0 the entry is freed. When all 64 entries are in use (count at 0x820817A0) bloodSpurtAdd drops the
// new blood. "Blood stays" keeps the life of every splat full (hook at that countdown, the entry in $s0), and when the
// pool is full, frees the oldest splat for the new drop (hook in bloodSpurtAdd before the count is read), so new blood
// always appears and the oldest stains make room. Drops still flying or splashing are never taken. With the option
// off the game's own behaviour is unchanged.
// The splats that make room are not lost: each is copied into a host list (SPLATS_KEPT at most, a new level empties
// it), and bloodSpurtsDraw(&gfx, &mtx, &vtx), which draws every entry of the pool by its state, is run again for them,
// 64 at a time: the list's newest are put into the pool, drawn, and the pool is put back. Passes stop while the frame's
// display list, matrix or vertex buffer would come within BUFFER_RESERVE of its size (MAX_gfx, MAX_mtx, MAX_vtx; past
// them the game drops the whole frame), so the oldest kept splats are the ones not drawn in a busy frame.
//
// Body parts (objects.c / spranim.c): a severed limb is an object (behaviour severedLimbControl, data at object + 0x68)
// with a life timer (data + 0x14, s16, counted down by twice the update rate) after which it fades out (object + 0x39)
// and is freed. Limbs whose type (data + 0x17) is 2 or more are pickups (drone heads) and are left alone. "Body parts
// stay" keeps the timer of the newest LIMBS_KEPT limbs up (hook at the entry of severedLimbControl); limbs are objects
// with models of their own, so the older ones still go, to keep the object heap from filling up.
//
// "Blood amount": the entry hook of bloodSpurtAdd makes extra drops with the same arguments and a little spread in
// their direction and speed.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <random>
#include <unordered_map>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" void bloodSpurtAdd(uint8_t* rdram, recomp_context* ctx);
extern "C" void bloodSpurtsDraw(uint8_t* rdram, recomp_context* ctx);

namespace jfg {

void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp

namespace {
// Overlay 8 is loaded wherever the heap has room, so its data is found from where it is now: the count of entries in
// use at +0x17A0 and the 64 entries at +0x17C0 from the start of the overlay.
constexpr uint32_t BLOOD_COUNT_OFFSET = 0x17A0, BLOOD_POOL_OFFSET = 0x17C0;
constexpr int BLOOD_ENTRIES = 64;
constexpr uint32_t BLOOD_ENTRY_SIZE = 0x54, BLOOD_STATE_OFFSET = 0x4A;
constexpr uint8_t BLOOD_SPLAT = 3;      // state: lying on the ground or a wall
constexpr int16_t SPLAT_LIFE = 300;    // what bloodSpurtAdd starts an entry with
constexpr int LIMBS_KEPT = 24;
constexpr int16_t LIMB_LIFE = 600;
constexpr uint32_t SPLAT_LIFE_OFFSET = 0x3C, LIMB_LIFE_OFFSET = 0x14, LIMB_TYPE_OFFSET = 0x17;

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
constexpr uint32_t BLOOD_ENTRY_WORDS = BLOOD_ENTRY_SIZE / 4;
constexpr size_t SPLATS_KEPT = 4096;
struct KeptSplat { uint32_t words[BLOOD_ENTRY_WORDS]; };
std::deque<KeptSplat> g_kept_splats;
int g_kept_level = -1;
int g_extra_passes = 0, g_extra_drawn = 0; // in the last bloodSpurtsDraw, for the test log

// The frame's buffers (main.c): the two bases of each (one per frame), the size of an entry and the most it may use.
constexpr uint32_t ADDR_frame = 0x800FD7C0;
constexpr uint32_t ADDR_gfxBase = 0x800FB400, ADDR_mtxBase = 0x800FB410, ADDR_vtxBase = 0x800FB420;
constexpr uint32_t ADDR_MAX_gfx = 0x800FD7DC, ADDR_MAX_mtx = 0x800FD7E0, ADDR_MAX_vtx = 0x800FD7E8;
constexpr uint32_t GFX_SIZE = 8, MTX_SIZE = 64, VTX_SIZE = 10;
constexpr float BUFFER_RESERVE = 0.25f; // of each buffer, left for what the frame draws after the blood

// The overlay's start as last seen by a hook (0 = not seen yet).
uint32_t g_blood_base = 0;
uint32_t blood_count() { return g_blood_base + BLOOD_COUNT_OFFSET; }
uint32_t blood_entry(int i) { return g_blood_base + BLOOD_POOL_OFFSET + (uint32_t)i * BLOOD_ENTRY_SIZE; }

// Tests: JFG_DEBUG_BLOOD=1 writes the pool's use to jfg_memory.log about once a second while blood is updated.
void blood_log() {
    static const bool on = std::getenv("JFG_DEBUG_BLOOD") != nullptr;
    static uint32_t last = 0;
    if (!on || g_blood_base == 0 || g_tick_count - last < 60) return;
    last = g_tick_count;
    int splats = 0, moving = 0;
    for (int i = 0; i < BLOOD_ENTRIES; i++) {
        const uint8_t state = rd8(blood_entry(i) + BLOOD_STATE_OFFSET);
        if (state == BLOOD_SPLAT) splats++;
        else if (state != 0) moving++;
    }
    char line[224];
    std::snprintf(line, sizeof(line), "blood: %d splats, %d flying or splashing, count %d, %u recycled so far, %d kept, %d of them "
        "drawn in %d extra passes (level %d)", splats, moving, (int)rd32(blood_count()), g_blood_recycled, (int)g_kept_splats.size(),
        g_extra_drawn, g_extra_passes, (int)(int16_t)rd16(ADDR_currentLevel));
    jfg_log_line("jfg_memory.log", line);
}
} // namespace

// bloodSpurtUpdateAll, where a splat's life is counted down ($s0 = the pool entry, $s5 = the count's address). Every
// splat is kept; the tracker only records which is oldest, for jfg_blood_make_room.
extern "C" void jfg_gore_splat(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_blood) return;
    g_blood_base = (uint32_t)ctx->r21 - BLOOD_COUNT_OFFSET;
    const uint32_t entry = (uint32_t)ctx->r16;
    keep_newest(g_splats, entry, BLOOD_ENTRIES);
    wr16(entry + SPLAT_LIFE_OFFSET, (uint16_t)SPLAT_LIFE);
    blood_log();
}

// bloodSpurtAdd, before it reads the count: with "Blood stays" on and every entry in use, free the oldest splat (as
// bloodSpurtUpdateAll frees one: state 0, count - 1) so the new drop gets its entry. A splat the tracker has not seen
// yet counts as the oldest. With no splat to take (all flying or splashing) the drop is dropped, as in the game.
// `base` = where the overlay starts now (section_addresses[8]).
extern "C" void jfg_blood_make_room(uint8_t* rdram_, uint32_t base) {
    g_rdram = rdram_;
    if (!g.gore_keep_blood) return;
    g_blood_base = base;
    uint32_t oldest = 0, oldest_first = UINT32_MAX;
    bool free_entry = false;
    for (int i = 0; i < BLOOD_ENTRIES; i++) {
        const uint32_t entry = blood_entry(i);
        const uint8_t state = rd8(entry + BLOOD_STATE_OFFSET);
        if (state == 0) { free_entry = true; continue; }
        if (state != BLOOD_SPLAT) continue;
        const auto it = g_splats.seen.find(entry);
        const uint32_t first = it == g_splats.seen.end() ? 0 : it->second.first;
        if (first < oldest_first) { oldest_first = first; oldest = entry; }
    }
    if ((free_entry && (int32_t)rd32(blood_count()) < BLOOD_ENTRIES) || oldest == 0) return;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != g_kept_level) { g_kept_splats.clear(); g_kept_level = level; }
    KeptSplat kept;
    for (uint32_t w = 0; w < BLOOD_ENTRY_WORDS; w++) kept.words[w] = rd32(oldest + w * 4);
    g_kept_splats.push_back(kept);
    if (g_kept_splats.size() > SPLATS_KEPT) g_kept_splats.pop_front();
    wr8(oldest + BLOOD_STATE_OFFSET, 0);
    const int32_t count = (int32_t)rd32(blood_count());
    if (count > 0) wr32(blood_count(), (uint32_t)(count - 1));
    g_splats.seen.erase(oldest); // the new drop in this entry is a new splat when it lands
    g_blood_recycled++;
}

namespace {
// How much of a frame buffer is in use: `ptr` (where bloodSpurtsDraw's pointer is now) from this frame's base, in
// entries; -1 if it is not in the buffer.
int buffer_used(uint32_t base_addr, uint32_t ptr, uint32_t size, int max) {
    const uint32_t base = rd32(base_addr + 4 * (rd32(ADDR_frame) & 1));
    if (ptr < base) return -1;
    const uint32_t used = (ptr - base) / size;
    return used <= (uint32_t)max ? (int)used : -1;
}
} // namespace

// Entry of bloodSpurtsDraw(Gfx **dList, Mtx **mtx, Vertex **vtx): draws the kept splats (see the top of the file) in
// extra passes of the function, before it draws the pool. `base` = where overlay 8 starts now (section_addresses[8]).
extern "C" void jfg_blood_draw_kept(uint8_t* rdram_, recomp_context* ctx, uint32_t base) {
    static bool busy = false;
    if (busy) return;
    g_rdram = rdram_;
    g_extra_passes = g_extra_drawn = 0;
    if (!g.gore_keep_blood || g.no_blood) { g_kept_splats.clear(); return; }
    if ((int16_t)rd16(ADDR_currentLevel) != g_kept_level) g_kept_splats.clear();
    if (g_kept_splats.empty()) return;
    g_blood_base = base;
    const uint32_t gfx_at = (uint32_t)ctx->r4, mtx_at = (uint32_t)ctx->r5, vtx_at = (uint32_t)ctx->r6;
    const int max_gfx = (int)rd32(ADDR_MAX_gfx), max_mtx = (int)rd32(ADDR_MAX_mtx), max_vtx = (int)rd32(ADDR_MAX_vtx);
    if (max_gfx <= 0 || max_mtx <= 0 || max_vtx <= 0) return;
    auto used = [&](int& g_used, int& m_used, int& v_used) {
        g_used = buffer_used(ADDR_gfxBase, rd32(gfx_at), GFX_SIZE, max_gfx);
        m_used = buffer_used(ADDR_mtxBase, rd32(mtx_at), MTX_SIZE, max_mtx);
        v_used = buffer_used(ADDR_vtxBase, rd32(vtx_at), VTX_SIZE, max_vtx);
        return g_used >= 0 && m_used >= 0 && v_used >= 0;
    };
    int gu, mu, vu;
    if (!used(gu, mu, vu)) return;
    const int limit_gfx = (int)(max_gfx * (1.0f - BUFFER_RESERVE)), limit_mtx = (int)(max_mtx * (1.0f - BUFFER_RESERVE));
    const int limit_vtx = (int)(max_vtx * (1.0f - BUFFER_RESERVE));
    // The most one pass of 64 has cost (a guess until one is seen). Room for two is needed: the pass and the game's
    // own pool drawn after it.
    static int pass_gfx = 64 * 24, pass_mtx = 64, pass_vtx = 64 * 8;
    auto room = [&] { return gu + 2 * pass_gfx <= limit_gfx && mu + 2 * pass_mtx <= limit_mtx && vu + 2 * pass_vtx <= limit_vtx; };
    if (!room()) return;

    uint32_t pool[BLOOD_ENTRIES * BLOOD_ENTRY_WORDS];
    const uint32_t pool_at = blood_entry(0);
    for (uint32_t w = 0; w < BLOOD_ENTRIES * BLOOD_ENTRY_WORDS; w++) pool[w] = rd32(pool_at + w * 4);
    busy = true;
    size_t next = g_kept_splats.size(); // newest first
    while (next > 0 && room()) {
        const int n = (int)std::min<size_t>(next, BLOOD_ENTRIES);
        for (int i = 0; i < BLOOD_ENTRIES; i++) {
            const uint32_t entry = blood_entry(i);
            if (i < n) {
                const KeptSplat& k = g_kept_splats[next - n + i];
                for (uint32_t w = 0; w < BLOOD_ENTRY_WORDS; w++) wr32(entry + w * 4, k.words[w]);
                wr8(entry + BLOOD_STATE_OFFSET, BLOOD_SPLAT);
            } else {
                wr8(entry + BLOOD_STATE_OFFSET, 0);
            }
        }
        recomp_context c = *ctx;
        bloodSpurtsDraw(rdram_, &c);
        int gu2, mu2, vu2;
        const bool ok = used(gu2, mu2, vu2);
        g_extra_passes++;
        g_extra_drawn += n;
        next -= n;
        if (!ok) break;
        pass_gfx = std::max(pass_gfx, gu2 - gu); pass_mtx = std::max(pass_mtx, mu2 - mu); pass_vtx = std::max(pass_vtx, vu2 - vu);
        gu = gu2; mu = mu2; vu = vu2;
    }
    busy = false;
    for (uint32_t w = 0; w < BLOOD_ENTRIES * BLOOD_ENTRY_WORDS; w++) wr32(pool_at + w * 4, pool[w]);
}

// Entry of severedLimbControl(object, updateRate).
extern "C" void jfg_gore_limb(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_limbs) return;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = valid_ptr(obj) ? rd32(obj + 0x68) : 0;
    if (!valid_ptr(data) || rd8(data + LIMB_TYPE_OFFSET) >= 2) return;
    if (keep_newest(g_limbs, obj, LIMBS_KEPT) && (int16_t)rd16(data + LIMB_LIFE_OFFSET) > 0) wr16(data + LIMB_LIFE_OFFSET, (uint16_t)LIMB_LIFE);
}

// Entry of bloodSpurtAdd(x, y, z, dx, dy, dz, ...): x and y in $f12/$f14, z and dx in $a2/$a3, dy and dz in the
// caller's argument slots at $sp + 0x10 / 0x14. Adds the extra drops of "Blood amount" before the game adds its own.
extern "C" void jfg_blood_more(uint8_t* rdram_, recomp_context* ctx) {
    static bool busy = false;
    if (busy || g.blood_amount <= 100) return;
    g_rdram = rdram_;
    static std::mt19937 rng{ 0x4A464721u };
    std::uniform_real_distribution<float> spread(0.75f, 1.25f), chance(0.0f, 1.0f);
    const float extra_f = (float)(g.blood_amount - 100) / 100.0f;
    int extra = (int)extra_f;
    if (chance(rng) < extra_f - (float)extra) extra++;
    auto as_float = [](uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; };
    auto as_bits = [](float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; };
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t dy_bits = rd32(sp + 0x10), dz_bits = rd32(sp + 0x14);
    busy = true;
    for (int i = 0; i < extra; i++) {
        recomp_context c = *ctx;
        c.r7 = (gpr)(int32_t)as_bits(as_float((uint32_t)ctx->r7) * spread(rng));
        wr32(sp + 0x10, as_bits(as_float(dy_bits) * spread(rng)));
        wr32(sp + 0x14, as_bits(as_float(dz_bits) * spread(rng)));
        bloodSpurtAdd(rdram_, &c);
    }
    busy = false;
    wr32(sp + 0x10, dy_bits);
    wr32(sp + 0x14, dz_bits);
}

} // namespace jfg
