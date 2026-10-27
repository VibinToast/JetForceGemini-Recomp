// Jet Force Gemini port: the JFG mod API (mod_api/jfg_api.h for mods, jfg_api.h for the port).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <string>
#include <vector>

#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" void recomp_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t event_index);
std::string jfg_locale_shown(); // jfg_locale.cpp

namespace jfg::api {
namespace {
// Event names for mods, in the order of Event.
const char* const g_event_names[] = {
    "jfg_on_game_tick", "jfg_on_level_change", "jfg_on_boss_fight_control", "jfg_on_blood_spurt", "jfg_on_blood_hit",
    "jfg_on_severed_limb_tick", "jfg_on_object_draw", "jfg_on_sound_play", "jfg_on_screen_flash", "jfg_on_sidekick_tick",
    "jfg_on_language_change", "jfg_on_map_open", "jfg_on_galaxy_change", "jfg_on_planet_select", "jfg_on_hero_control",
    "jfg_on_object_draw_gfx", nullptr
};
static_assert(sizeof(g_event_names) / sizeof(g_event_names[0]) == (size_t)Event::Count + 1);

struct Internal {
    Handler handler;
    Active active;
};
std::vector<Internal> g_internal[(size_t)Event::Count];

// Counted on the game thread, read by the F9 window.
std::atomic<uint64_t> g_fired[(size_t)Event::Count];
std::atomic<uint64_t> g_ran[(size_t)Event::Count];

bool internal_active(const Internal& internal) {
    return internal.active == nullptr || internal.active();
}

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

// jfg_get_language(char* id, int size): the shown locale's id into the mod's buffer (0 ended, cut to fit); returns 0
// for English, 1 for any other locale.
void get_language(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const std::string id = jfg_locale_shown();
    const uint32_t buffer = (uint32_t)ctx->r4;
    const int32_t size = (int32_t)ctx->r5;
    if (buffer != 0 && size > 0) {
        const size_t n = std::min<size_t>(id.size(), (size_t)size - 1);
        for (size_t i = 0; i < n; i++) wr8(buffer + (uint32_t)i, (uint8_t)id[i]);
        wr8(buffer + (uint32_t)n, 0);
    }
    ctx->r2 = id.empty() ? 0 : 1;
}
} // namespace

Payload::Payload(recomp_context* ctx_, uint32_t size) : ctx(ctx_), saved_sp(ctx_->r29) {
    addr = ((uint32_t)saved_sp - size - 0x20) & ~0xFu;
    ctx->r29 = (gpr)(int32_t)(addr - 0x20);
}

Payload::~Payload() {
    ctx->r29 = saved_sp;
}

void add_internal(Event event, Handler handler, Active active) {
    g_internal[(size_t)event].push_back({handler, active});
}

// Base events come first in the runtime's list, so an event's index is its own.
bool listened(Event event) {
    for (const Internal& internal : g_internal[(size_t)event]) {
        if (internal_active(internal)) return true;
    }
    return recomp::mods::event_has_callbacks((size_t)event);
}

void fire(uint8_t* rdram_, recomp_context* ctx, Event event, int32_t arg0, int32_t arg1, int32_t arg2, int32_t arg3) {
    g_fired[(size_t)event].fetch_add(1, std::memory_order_relaxed);
    const bool mods = recomp::mods::event_has_callbacks((size_t)event);
    bool any = mods;
    for (const Internal& internal : g_internal[(size_t)event]) any = any || internal_active(internal);
    if (!any) return;
    g_ran[(size_t)event].fetch_add(1, std::memory_order_relaxed);

    const recomp_context saved = *ctx;
    ctx->r4 = (gpr)arg0;
    ctx->r5 = (gpr)arg1;
    ctx->r6 = (gpr)arg2;
    ctx->r7 = (gpr)arg3;
    const recomp_context args = *ctx;
    for (const Internal& internal : g_internal[(size_t)event]) {
        if (!internal_active(internal)) continue;
        internal.handler(rdram_, ctx);
        *ctx = args;
    }
    if (mods) recomp_trigger_event(rdram_, ctx, (uint32_t)event);
    *ctx = saved;
}

const char* event_name(Event event) {
    return g_event_names[(size_t)event];
}

Stats stats(Event event) {
    return {g_fired[(size_t)event].load(std::memory_order_relaxed), g_ran[(size_t)event].load(std::memory_order_relaxed)};
}

size_t mod_callbacks(Event event) {
    return recomp::mods::event_callback_count((size_t)event);
}

void log_stats() {
    static Stats last[(size_t)Event::Count] = {};
    std::string line = "[jfg] mod API events (fired/listened):";
    for (size_t i = 0; i < (size_t)Event::Count; i++) {
        const Stats s = stats((Event)i);
        if (s.fired != last[i].fired) {
            line += " " + std::string(g_event_names[i] + 7) + " " + std::to_string(s.fired - last[i].fired) + "/" +
                    std::to_string(s.ran - last[i].ran); // name without "jfg_on_"
        }
        last[i] = s;
    }
    std::fprintf(stderr, "%s\n", line.c_str());
}

void register_api() {
    // A handler with an option runs only while it's on (the last argument), so with the options at their defaults and no
    // mod listening, the frequent events (object draw, sound) cost nothing.
    add_internal(Event::LevelChange, jfg_change_level_log); // JFG_DEBUG_STATS log (jfg_diagnostics.cpp)
    add_internal(Event::BossFightControl, jfg_boss_tick);   // boss-fight control layout (jfg_controls.cpp)
    add_internal(Event::BloodSpurt, jfg_gore_on_blood_spurt, // No blood, Blood amount (jfg_gore.cpp)
                 [] { return jfg_no_blood() != 0 || g.blood_amount != 100; });
    add_internal(Event::BloodSpurt, jfg_gore_make_room, [] { return g.gore_keep_blood; });   // Blood stays: room for it
    add_internal(Event::LevelChange, jfg_gore_level_change, [] { return g.gore_keep_blood; }); // Blood stays: kept splats go
    add_internal(Event::BloodHit, jfg_gore_on_blood_hit, [] { return jfg_no_blood() != 0; }); // No blood
    add_internal(Event::SeveredLimbTick, jfg_gore_limb, [] { return g.gore_keep_limbs; });     // Body parts stay
    add_internal(Event::SidekickTick, jfg_floyd_seen);       // Hide Floyd: his object kind (jfg_visuals.cpp)
    add_internal(Event::ObjectDraw, jfg_hide_object, [] { return g.hide_floyd; }); // Hide Floyd
    add_internal(Event::SoundPlay, jfg_floyd_mute, [] { return g.hide_floyd; });   // Hide Floyd: his sounds
    add_internal(Event::ScreenFlash, jfg_hit_flash_event, [] { return g.hit_flash < 100; }); // Hit flash opacity
    recomp::overlays::register_base_events(g_event_names);
    recomp::overlays::register_base_export("jfg_api_version", api_version);
    recomp::overlays::register_base_export("jfg_get_tick_scale", get_tick_scale);
    recomp::overlays::register_base_export("jfg_get_level", get_level);
    recomp::overlays::register_base_export("jfg_get_language", get_language);
    jfg::register_world_api(); // planets and galaxies (jfg_worldmap.cpp)
    register_hero_api();       // heroes, input, level collision, damage (jfg_hero_api.cpp)
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
    if (!api::listened(api::Event::BloodSpurt)) { // no payload to build; fire only counts it
        api::fire(rdram, ctx, api::Event::BloodSpurt);
        bloodSpurtAdd_orig(rdram, ctx);
        return;
    }
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

// bloodHitSpriteAdd(x, y, z, ...): jfg_on_blood_hit; callbacks may cancel it.
JFG_WRAP(bloodHitSpriteAdd) {
    using namespace jfg;
    namespace b = jfg::api::blood_hit;
    if (!api::listened(api::Event::BloodHit)) { // no payload to build; fire only counts it
        api::fire(rdram, ctx, api::Event::BloodHit);
        bloodHitSpriteAdd_orig(rdram, ctx);
        return;
    }
    g_rdram = rdram;
    int32_t cancel;
    {
        api::Payload p(ctx, b::SIZE);
        wr32(p.addr + b::X, ctx->f12.u32l);
        wr32(p.addr + b::Y, ctx->f14.u32l);
        wr32(p.addr + b::Z, (uint32_t)ctx->r6);
        wr32(p.addr + b::CANCEL, 0);
        api::fire(rdram, ctx, api::Event::BloodHit, (int32_t)p.addr);
        g_rdram = rdram;
        cancel = (int32_t)rd32(p.addr + b::CANCEL);
    }
    if (cancel) return;
    bloodHitSpriteAdd_orig(rdram, ctx);
}

// severedLimbControl(limb, updateRate): jfg_on_severed_limb_tick, then the game's code.
JFG_WRAP(severedLimbControl) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::SeveredLimbTick, (int32_t)ctx->r4, (int32_t)ctx->r5);
    severedLimbControl_orig(rdram, ctx);
}

// An event with one value a callback may change, and cancel (JfgObjectDraw, JfgSoundPlay, JfgScreenFlash). Returns
// false when it was cancelled; else value holds what to use.
static bool fire_value_cancel(uint8_t* rdram, recomp_context* ctx, jfg::api::Event event, uint32_t& value) {
    using namespace jfg;
    namespace v = jfg::api::value_cancel;
    if (!api::listened(event)) { // no payload to build; fire only counts it
        api::fire(rdram, ctx, event);
        return true;
    }
    g_rdram = rdram;
    api::Payload p(ctx, v::SIZE);
    wr32(p.addr + v::VALUE, value);
    wr32(p.addr + v::CANCEL, 0);
    api::fire(rdram, ctx, event, (int32_t)p.addr);
    g_rdram = rdram;
    value = rd32(p.addr + v::VALUE);
    return rd32(p.addr + v::CANCEL) == 0;
}

// objPrintObject(dList, mtx, vtx, obj): jfg_on_object_draw (Hide Floyd cancels his drawing), then the game's code with
// the interpolation's bookkeeping around it (jfg_interpolation.cpp).
JFG_WRAP(objPrintObject) {
    uint32_t object = (uint32_t)ctx->r7;
    if (!fire_value_cancel(rdram, ctx, jfg::api::Event::ObjectDraw, object)) return;
    ctx->r7 = (gpr)(int32_t)object;
    // The interpolation's bookkeeping starts before jfg_on_object_draw_gfx, so the matrices a mod's own drawing pushes
    // (camPushModelMtx) are tagged as the object's and RT64 interpolates them too.
    jfg_interp_object(rdram, ctx);
    // jfg_on_object_draw_gfx: a callback may write its own commands at the game's cursors, and cancel the game's own.
    if (jfg::api::listened(jfg::api::Event::ObjectDrawGfx)) {
        using namespace jfg;
        namespace o = jfg::api::object_gfx;
        g_rdram = rdram;
        int32_t cancel;
        {
            api::Payload p(ctx, o::SIZE);
            wr32(p.addr + o::OBJECT, object);
            wr32(p.addr + o::GFX, (uint32_t)ctx->r4);
            wr32(p.addr + o::MTX, (uint32_t)ctx->r5);
            wr32(p.addr + o::VTX, (uint32_t)ctx->r6);
            wr32(p.addr + o::CANCEL, 0);
            api::fire(rdram, ctx, api::Event::ObjectDrawGfx, (int32_t)p.addr);
            g_rdram = rdram;
            cancel = (int32_t)rd32(p.addr + o::CANCEL);
        }
        if (cancel) {
            jfg_interp_object_done(rdram, ctx);
            return;
        }
    }
    objPrintObject_orig(rdram, ctx);
    jfg_interp_object_done(rdram, ctx);
}

// amSndPlay(id, handle): jfg_on_sound_play (Hide Floyd mutes his sounds); a callback may change the sound's id.
JFG_WRAP(amSndPlay) {
    uint32_t id = (uint32_t)ctx->r4;
    if (!fire_value_cancel(rdram, ctx, jfg::api::Event::SoundPlay, id)) return;
    ctx->r4 = (gpr)(int32_t)id;
    amSndPlay_orig(rdram, ctx);
}

// frontFlashScreen(?, ?, colour, ...): jfg_on_screen_flash (Hit flash opacity); a callback may change the colour.
JFG_WRAP(frontFlashScreen) {
    uint32_t color = (uint32_t)ctx->r6;
    if (!fire_value_cancel(rdram, ctx, jfg::api::Event::ScreenFlash, color)) return;
    ctx->r6 = (gpr)(int32_t)color;
    frontFlashScreen_orig(rdram, ctx);
}

// sidekickControl(floyd, updateRate): jfg_on_sidekick_tick, then the game's code. Floyd's sounds are muted while it
// runs for a hidden Floyd, and the mouse steers him in Floyd missions (jfg_controls.cpp, through dAngle's wrapper).
JFG_WRAP(sidekickControl) {
    jfg::api::fire(rdram, ctx, jfg::api::Event::SidekickTick, (int32_t)ctx->r4, (int32_t)ctx->r5);
    jfg_sidekick_begin(rdram, ctx);
    sidekickControl_orig(rdram, ctx);
    jfg_sidekick_end();
    jfg_floyd_done();
}
