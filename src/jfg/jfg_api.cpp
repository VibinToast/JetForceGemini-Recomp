// Jet Force Gemini port: the JFG mod API (mod_api/jfg_api.h for mods, jfg_api.h for the port).
#include <algorithm>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_hooks.h"

extern "C" void recomp_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t event_index);

namespace jfg::api {
namespace {
// Event names for mods, in the order of Event.
const char* const g_event_names[] = { "jfg_on_game_tick", "jfg_on_level_change", "jfg_on_boss_fight_control", nullptr };
const char* const g_event_names[] = {
    "jfg_on_game_tick", "jfg_on_level_change", "jfg_on_boss_fight_control", "jfg_on_blood_spurt", "jfg_on_blood_hit",
    "jfg_on_severed_limb_tick", nullptr
};
static_assert(sizeof(g_event_names) / sizeof(g_event_names[0]) == (size_t)Event::Count + 1);

std::vector<Handler> g_internal[(size_t)Event::Count];

// --- helpers mods import ("*") -------------------------------------------------------------------------------------
void api_version(uint8_t*, recomp_context* ctx) {
    ctx->r2 = VERSION;
}

// 1.0 at 30 fps, 0.5 at 60 fps: how much game time this tick covers compared with the original game.
void get_tick_scale(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->f0.fl = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6) / 2.0f;
}

void get_level(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->r2 = (gpr)(int32_t)(int16_t)rd16(ADDR_currentLevel);
}
} // namespace

Payload::Payload(recomp_context* ctx_, uint32_t size) : ctx(ctx_), saved_sp(ctx_->r29) {
    addr = ((uint32_t)saved_sp - size - 0x20) & ~0xFu;
    ctx->r29 = (gpr)(int32_t)(addr - 0x20);
}

Payload::~Payload() {
    ctx->r29 = saved_sp;
}

void add_internal(Event event, Handler handler) {
    g_internal[(size_t)event].push_back(handler);
}

void fire(uint8_t* rdram_, recomp_context* ctx, Event event, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    const recomp_context saved = *ctx;
    ctx->r4 = (gpr)arg0;
    ctx->r5 = (gpr)arg1;
    ctx->r6 = (gpr)arg2;
    ctx->r7 = (gpr)arg3;
    const recomp_context args = *ctx;
    for (Handler handler : g_internal[(size_t)event]) {
        handler(rdram_, ctx);
        *ctx = args;
    }
    recomp_trigger_event(rdram_, ctx, (uint32_t)event); // base events come first, so the index is the event's
    *ctx = saved;
}

void register_api() {
    add_internal(Event::LevelChange, jfg_change_level_log); // JFG_DEBUG_STATS log (jfg_diagnostics.cpp)
    add_internal(Event::BossFightControl, jfg_boss_tick);   // boss-fight control layout (jfg_controls.cpp)
    add_internal(Event::BloodSpurt, jfg_gore_on_blood_spurt); // No blood, Blood amount (jfg_gore.cpp)
    add_internal(Event::BloodHit, jfg_gore_on_blood_hit);     // No blood
    add_internal(Event::SeveredLimbTick, jfg_gore_limb);      // Body parts stay
    recomp::overlays::register_base_events(g_event_names);
    recomp::overlays::register_base_export("jfg_api_version", api_version);
    recomp::overlays::register_base_export("jfg_get_tick_scale", get_tick_scale);
    recomp::overlays::register_base_export("jfg_get_level", get_level);
}

} // namespace jfg::api

// --- event sources (game functions the port wraps, tools/wrapped_funcs.txt) ----------------------------------------

// mainChangeLevel(level, character, ...): jfg_on_level_change, then the game's code.
JFG_WRAP(mainChangeLevel) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::LevelChange, (int32_t)ctx->r4, (int32_t)ctx->r5);
    mainChangeLevel_orig(rdram, ctx);
}

// The player's control in a boss fight (overlays 50-52, one per character; (obj, updateRate)):
// jfg_on_boss_fight_control(player, frames, character), then the game's code. Characters as character_names.
JFG_WRAP(girlBossControl) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::BossFightControl, (int32_t)ctx->r4, (int32_t)ctx->r5, 0);
    girlBossControl_orig(rdram, ctx);
}

JFG_WRAP(boyBossControl) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::BossFightControl, (int32_t)ctx->r4, (int32_t)ctx->r5, 1);
    boyBossControl_orig(rdram, ctx);
}

JFG_WRAP(dogBossControl) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::BossFightControl, (int32_t)ctx->r4, (int32_t)ctx->r5, 2);
    dogBossControl_orig(rdram, ctx);
}

// bloodSpurtAdd(x, y, z, vx, vy, vz, alpha, count, scale): jfg_on_blood_spurt with the arguments in a JfgBloodSpurt.
// x and y are in $f12/$f14, z and vx in $a2/$a3, the rest in the caller's argument slots ($sp + 0x10..). Callbacks may
// change them, cancel the spurt, or set amount: drops in percent, the extra ones spread a little (jfg_gore.cpp).
JFG_WRAP(bloodSpurtAdd) {
    using namespace jfg;
    namespace b = jfg::api::blood_spurt;
    g_rdram = rdram;
    const uint32_t sp = (uint32_t)ctx->r29;
    int32_t amount, cancel;
    {
        api::Payload p(ctx, b::SIZE);
        wr32(p.addr + b::X, ctx->f12.u32l);
        wr32(p.addr + b::Y, ctx->f14.u32l);
        wr32(p.addr + b::Z, (uint32_t)ctx->r6);
        wr32(p.addr + b::VX, (uint32_t)ctx->r7);
        for (uint32_t i = 0; i < 5; i++) wr32(p.addr + b::VY + i * 4, rd32(sp + 0x10 + i * 4)); // vy, vz, alpha, count, scale
        wr32(p.addr + b::AMOUNT, 100);
        wr32(p.addr + b::CANCEL, 0);
        api::fire(rdram, ctx, api::Event::BloodSpurt, (int32_t)p.addr);
        g_rdram = rdram;
        ctx->f12.u32l = rd32(p.addr + b::X);
        ctx->f14.u32l = rd32(p.addr + b::Y);
        ctx->r6 = (gpr)(int32_t)rd32(p.addr + b::Z);
        ctx->r7 = (gpr)(int32_t)rd32(p.addr + b::VX);
        for (uint32_t i = 0; i < 5; i++) wr32(sp + 0x10 + i * 4, rd32(p.addr + b::VY + i * 4));
        amount = (int32_t)rd32(p.addr + b::AMOUNT);
        cancel = (int32_t)rd32(p.addr + b::CANCEL);
    }
    if (cancel) return;
    jfg_blood_extra_drops(rdram, ctx, amount);
    bloodSpurtAdd_orig(rdram, ctx);
}
