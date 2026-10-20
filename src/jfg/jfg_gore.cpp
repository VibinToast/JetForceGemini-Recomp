// Jet Force Gemini port: the gore options - blood splats and severed body parts that stay, and more blood.
//
// Blood (overlay 8): bloodSpurtAdd puts a drop into a pool of 64 entries (0x54 bytes each). A drop flies (state +0x4A
// = 1), lands (2) and lies as a splat (3) whose life (+0x3C, s16, 300 at the start) bloodSpurtUpdateAll counts down by
// the update rate; at 0 the entry is freed. "Blood stays" keeps the life of the newest splats full (hook at that
// countdown, the entry in $s0). Only BLOOD_KEPT are kept, so the pool always has room for new drops: older splats run
// out within a second.
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
#include <random>
#include <unordered_map>

#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" void bloodSpurtAdd_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void bloodSpurtUpdateAll_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace jfg {

namespace {
constexpr int BLOOD_KEPT = 40;         // of the pool's 64 entries
constexpr int16_t SPLAT_LIFE = 300;    // what bloodSpurtAdd starts an entry with
constexpr int16_t SPLAT_LIFE_LEFT = 40; // at most, once a splat is no longer kept
constexpr int LIMBS_KEPT = 24;
constexpr int16_t LIMB_LIFE = 600;
constexpr uint32_t SPLAT_LIFE_OFFSET = 0x3C, LIMB_LIFE_OFFSET = 0x14, LIMB_TYPE_OFFSET = 0x17;
// The blood pool: overlay 8 (ROM 0x01EFF168) + 0x17C0, 64 entries of 0x54 bytes; +0x4A is the state, 3 = a splat.
constexpr uint32_t OVERLAY_8_ROM = 0x01EFF168, BLOOD_POOL = 0x17C0, BLOOD_ENTRIES = 64, BLOOD_ENTRY_SIZE = 0x54;
constexpr uint32_t BLOOD_STATE_OFFSET = 0x4A;

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
} // namespace

// bloodSpurtUpdateAll, where a splat's life is counted down ($s0 = the pool entry).
extern "C" void jfg_gore_splat(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_blood) return;
    const uint32_t entry = (uint32_t)ctx->r16;
    if (keep_newest(g_splats, entry, BLOOD_KEPT)) wr16(entry + SPLAT_LIFE_OFFSET, (uint16_t)SPLAT_LIFE);
    // One that is no longer among the newest makes room soon, so new drops find free entries.
    else if ((int16_t)rd16(entry + SPLAT_LIFE_OFFSET) > SPLAT_LIFE_LEFT) wr16(entry + SPLAT_LIFE_OFFSET, (uint16_t)SPLAT_LIFE_LEFT);
    const uint32_t ovl = jfg_overlay_loaded_at(OVERLAY_8_ROM);
    if (ovl == 0) return;
    static const bool stats = std::getenv("JFG_DEBUG_STATS") != nullptr;
    static uint32_t calls = 0;
    int splats = 0, kept = 0;
    for (uint32_t i = 0; i < BLOOD_ENTRIES; i++) {
        const uint32_t entry = ovl + BLOOD_POOL + i * BLOOD_ENTRY_SIZE;
        if (rd8(entry + BLOOD_STATE_OFFSET) != 3) continue;
        splats++;
        if (keep_newest(g_splats, entry, BLOOD_KEPT)) { wr16(entry + SPLAT_LIFE_OFFSET, (uint16_t)SPLAT_LIFE); kept++; }
        // One that is no longer among the newest makes room soon, so new drops find free entries.
        else if ((int16_t)rd16(entry + SPLAT_LIFE_OFFSET) > SPLAT_LIFE_LEFT) wr16(entry + SPLAT_LIFE_OFFSET, (uint16_t)SPLAT_LIFE_LEFT);
    }
    if (stats && ++calls % 300 == 0) std::fprintf(stderr, "[jfg] blood pool: %d splats, %d kept\n", splats, kept);
}

// jfg_on_severed_limb_tick(limb, frames) handler.
extern "C" void jfg_gore_limb(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g.gore_keep_limbs) return;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = valid_ptr(obj) ? rd32(obj + 0x68) : 0;
    if (!valid_ptr(data) || rd8(data + LIMB_TYPE_OFFSET) >= 2) return;
    if (keep_newest(g_limbs, obj, LIMBS_KEPT) && (int16_t)rd16(data + LIMB_LIFE_OFFSET) > 0) wr16(data + LIMB_LIFE_OFFSET, (uint16_t)LIMB_LIFE);
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
    const float extra_f = (float)(g.blood_amount - 100) / 100.0f;
    const float extra_f = (float)(amount - 100) / 100.0f;
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
        bloodSpurtAdd_orig(rdram_, &c);
    }
    busy = false;
    wr32(sp + 0x10, dy_bits);
    wr32(sp + 0x14, dz_bits);
}

} // namespace jfg
