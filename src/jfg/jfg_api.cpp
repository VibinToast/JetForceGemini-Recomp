// Jet Force Gemini port: the JFG mod API (mod_api/jfg_api.h for mods, jfg_api.h for the port).
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" void recomp_trigger_event(uint8_t* rdram, recomp_context* ctx, uint32_t event_index);
extern "C" void camGetNo(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackGetFog(uint8_t* rdram, recomp_context* ctx);
std::string jfg_locale_shown(); // jfg_locale.cpp
namespace jfg::overlay { void register_api(); void frame_done(); } // jfg_overlay.cpp
namespace jfg { void register_weapon_hud_api(); }                 // jfg_weapon_hud.cpp

namespace jfg::api {
namespace {
// Event names for mods, in the order of Event.
const char* const g_event_names[] = {
    "jfg_on_game_tick", "jfg_on_level_change", "jfg_on_boss_fight_control", "jfg_on_blood_spurt", "jfg_on_blood_hit",
    "jfg_on_severed_limb_tick", "jfg_on_object_draw", "jfg_on_sound_play", "jfg_on_screen_flash", "jfg_on_sidekick_tick",
    "jfg_on_language_change", "jfg_on_map_open", "jfg_on_galaxy_change", "jfg_on_planet_select", "jfg_on_hero_control",
    "jfg_on_object_draw_gfx", "jfg_on_draw_hud", "jfg_on_hud_message", "jfg_on_reticle_draw",
    "jfg_on_health_draw", "jfg_on_weapon_hud", "jfg_on_kill", nullptr
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

// jfg_aim_lock(int on): 0 = the reticle no longer jumps onto an enemy it passes over (hitPlayerAimCheck's wrapper).
bool g_aim_lock = true;
void aim_lock(uint8_t*, recomp_context* ctx) { g_aim_lock = ctx->r4 != 0; }

// jfg_gfx_alloc(unsigned int size): a block for a mod's graphics data (textures, display lists, vertices) in the
// free 4 MB at 0x80800000 (docs/MEMORY.md): below the 16 MB line, so the renderer reads it, unlike mod memory. Zeroed,
// 16-byte aligned, kept for the whole run (a mod allocates once); NULL when the region is full.
constexpr uint32_t GFX_REGION = 0x80800000u, GFX_REGION_END = 0x80C00000u;
uint32_t g_gfx_next = GFX_REGION;
uint32_t gfx_take(uint32_t bytes) {
    const uint32_t size = (bytes + 15u) & ~15u;
    if (size == 0 || size > GFX_REGION_END - g_gfx_next) return 0;
    const uint32_t block = g_gfx_next;
    g_gfx_next += size;
    for (uint32_t a = block; a < block + size; a += 4) wr32(a, 0);
    std::fprintf(stderr, "[jfg] jfg_gfx_alloc: %u bytes at 0x%08X (%u KB of 4096 used)\n", size, block, (g_gfx_next - GFX_REGION) / 1024);
    return block;
}
void gfx_alloc(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->r2 = (gpr)(int32_t)gfx_take((uint32_t)ctx->r4);
}

// jfg_object_tint(void* object, unsigned int rgba): the object drawn tinted that colour, alpha its strength (0 none),
// through the fog colour as the game flashes the enemy it targets (objPrintModelObject); kept until set again.
struct Tint {
    uint32_t object, rgba;
};
std::vector<Tint> g_tints;
void object_tint(uint8_t*, recomp_context* ctx) {
    const uint32_t object = (uint32_t)ctx->r4, rgba = (uint32_t)ctx->r5;
    auto it = std::find_if(g_tints.begin(), g_tints.end(), [&](const Tint& t) { return t.object == object; });
    if (it != g_tints.end()) {
        if ((rgba & 0xFF) == 0) g_tints.erase(it);
        else it->rgba = rgba;
    } else if ((rgba & 0xFF) != 0 && object != 0 && g_tints.size() < 64) {
        g_tints.push_back({ object, rgba });
    }
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
    // Blood stays and Body parts stay, for a mod (jfg_blood_keep, jfg_limbs_keep; jfg_gore.cpp).
    add_internal(Event::LevelChange, jfg_gore_level_change, [] { return jfg_gore_keep_blood(); }); // kept splats go
    add_internal(Event::SeveredLimbTick, jfg_gore_limb, [] { return jfg_gore_keep_limbs(); });
    add_internal(Event::SidekickTick, jfg_floyd_seen);       // Hide Floyd: his object kind (jfg_visuals.cpp)
    add_internal(Event::SidekickTick, jfg_floyd_fuel, [] { return g.floyd_fuel != 100; }); // Floyd's Fuel (jfg_gameplay.cpp)
    add_internal(Event::ObjectDraw, jfg_hide_object, [] { return jfg_floyd_hide_on(); }); // Hide Floyd, for a mod
    add_internal(Event::SoundPlay, jfg_floyd_mute, [] { return jfg_floyd_hide_on(); });   // ... his sounds
    add_internal(Event::ScreenFlash, jfg_hit_flash_event, [] { return g.hit_flash < 100; }); // Hit flash opacity
    recomp::overlays::register_base_events(g_event_names);
    recomp::overlays::register_base_export("jfg_api_version", api_version);
    recomp::overlays::register_base_export("jfg_get_tick_scale", get_tick_scale);
    recomp::overlays::register_base_export("jfg_get_level", get_level);
    recomp::overlays::register_base_export("jfg_get_language", get_language);
    jfg::register_world_api(); // planets and galaxies (jfg_worldmap.cpp)
    jfg::register_blood_api(); // blood colours, rainbow, Blood stays, Body parts stay (jfg_gore.cpp)
    recomp::overlays::register_base_export("jfg_floyd_hide", jfg_api_floyd_hide); // Hide Floyd (jfg_visuals.cpp)
    recomp::overlays::register_base_export("jfg_aim_lock", aim_lock);
    recomp::overlays::register_base_export("jfg_object_tint", object_tint);
    recomp::overlays::register_base_export("jfg_gfx_alloc", gfx_alloc);
    jfg::overlay::register_api(); // pictures and shapes over the game at the window's resolution (jfg_overlay.cpp)
    jfg::register_weapon_hud_api(); // the weapon panel's icons, the game's texts, Tribal counts (jfg_weapon_hud.cpp)
    register_hero_api();       // heroes, input, level collision, damage (jfg_hero_api.cpp)
}

// A block of the graphics region for the port's own use (the weapon panel's discs, jfg_weapon_hud.cpp), as
// jfg_gfx_alloc gives mods: 0 when full.
uint32_t gfx_block(uint32_t bytes) { return gfx_take(bytes); }

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

static float as_float(uint32_t bits) { float f; std::memcpy(&f, &bits, 4); return f; }

// bloodSpurtAdd(x, y, z, vx, vy, vz, brightness, color, scale): jfg_on_blood_spurt with the arguments in a
// JfgBloodSpurt, and the creature bleeding (jfg_blood_source). x and y are in $f12/$f14, z and vx in $a2/$a3, the rest in
// the caller's argument slots ($sp + 0x10..). Callbacks may change them, cancel the spurt, or set amount: drops in
// percent, the extra ones spread a little (jfg_gore.cpp).
JFG_WRAP(bloodSpurtAdd) {
    using namespace jfg;
    namespace b = jfg::api::blood_spurt;
    g_rdram = rdram;
    jfg_blood_colors_apply();
    if (!api::listened(api::Event::BloodSpurt)) { // no payload to build; fire only counts it
        api::fire(rdram, ctx, api::Event::BloodSpurt);
        jfg_gore_make_room(rdram, 100);
        bloodSpurtAdd_orig(rdram, ctx);
        return;
    }
    const uint32_t sp = (uint32_t)ctx->r29;
    int32_t amount, cancel;
    int group = 0;
    const int game_color = (int)rd32(sp + 0x1C);
    const uint32_t source = jfg_blood_source(ctx->f12.fl, ctx->f14.fl, as_float((uint32_t)ctx->r6), game_color, &group);
    {
        api::Payload p(ctx, b::SIZE);
        wr32(p.addr + b::SOURCE, source);
        wr32(p.addr + b::GROUP, (uint32_t)group);
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
    static const bool log = std::getenv("JFG_DEBUG_BLOOD") != nullptr; // the first 40 spurts, for tests
    static int logged = 0;
    if (log && logged < 40) {
        logged++;
        std::fprintf(stderr, "[jfg] blood spurt from %08X (kind %d), group %d: colour %d -> %d, amount %d%s\n", source,
                     source != 0 ? (int)Obj::Kind::get(source) : -1, group, game_color, (int)rd32(sp + 0x1C), amount,
                     cancel ? ", cancelled" : "");
    }
    if (cancel) return;
    jfg_gore_make_room(rdram, amount);
    jfg_blood_extra_drops(rdram, ctx, amount);
    bloodSpurtAdd_orig(rdram, ctx);
}

// bloodHitSpriteAdd(x, y, z, brightness, color): jfg_on_blood_hit, with the creature bleeding (jfg_blood_source);
// callbacks may change brightness and color ($a3 and the caller's $sp + 0x10), or cancel it.
JFG_WRAP(bloodHitSpriteAdd) {
    using namespace jfg;
    namespace b = jfg::api::blood_hit;
    g_rdram = rdram;
    jfg_blood_colors_apply();
    if (!api::listened(api::Event::BloodHit)) { // no payload to build; fire only counts it
        api::fire(rdram, ctx, api::Event::BloodHit);
        bloodHitSpriteAdd_orig(rdram, ctx);
        return;
    }
    const uint32_t sp = (uint32_t)ctx->r29;
    int32_t cancel;
    {
        int group = 0;
        const uint32_t source = jfg_blood_source(ctx->f12.fl, ctx->f14.fl, as_float((uint32_t)ctx->r6), (int)rd32(sp + 0x10), &group);
        api::Payload p(ctx, b::SIZE);
        wr32(p.addr + b::X, ctx->f12.u32l);
        wr32(p.addr + b::Y, ctx->f14.u32l);
        wr32(p.addr + b::Z, (uint32_t)ctx->r6);
        wr32(p.addr + b::CANCEL, 0);
        wr32(p.addr + b::BRIGHTNESS, (uint32_t)ctx->r7);
        wr32(p.addr + b::COLOR, rd32(sp + 0x10));
        wr32(p.addr + b::SOURCE, source);
        wr32(p.addr + b::GROUP, (uint32_t)group);
        api::fire(rdram, ctx, api::Event::BloodHit, (int32_t)p.addr);
        g_rdram = rdram;
        cancel = (int32_t)rd32(p.addr + b::CANCEL);
        ctx->r7 = (gpr)(int32_t)rd32(p.addr + b::BRIGHTNESS);
        wr32(sp + 0x10, rd32(p.addr + b::COLOR));
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
    // jfg_object_tint: the fog set to the tint's colour, at the same strength wherever the object is (multiplier 0,
    // offset the strength), and the level's own fog put back after it.
    uint32_t tint = 0;
    for (const auto& t : jfg::api::g_tints) if (t.object == object) tint = t.rgba;
    const uint32_t dlist = (uint32_t)ctx->r4;
    if (tint != 0) {
        using namespace jfg;
        g_rdram = rdram;
        uint32_t cmd = rd32(dlist);
        wr32(cmd, 0xF8000000u); wr32(cmd + 4, tint | 0xFF);               // gDPSetFogColor
        wr32(cmd + 8, 0xBC000008u); wr32(cmd + 12, tint & 0xFF);         // gSPFogPosition's word: multiplier 0, offset
        wr32(dlist, cmd + 16);
    }
    objPrintObject_orig(rdram, ctx);
    if (tint != 0) {
        using namespace jfg;
        g_rdram = rdram;
        const uint32_t out = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
        const uint32_t cam = call_game(camGetNo, ctx, {}) & 3;
        call_game(trackGetFog, ctx, { cam, out, out + 2, out + 4, out + 6, out + 7, out + 8, out + 9 });
        int near_pos = (int16_t)rd16(out), far_pos = (int16_t)rd16(out + 2);
        jfg_level_fog_options(near_pos, far_pos);
        const int span = std::max(far_pos - near_pos, 1);
        const int fm = 128000 / span, fo = (500 - near_pos) * 256 / span;
        uint32_t cmd = rd32(dlist);
        wr32(cmd, 0xF8000000u);
        wr32(cmd + 4, ((uint32_t)rd8(out + 6) << 24) | ((uint32_t)rd8(out + 7) << 16) | ((uint32_t)rd8(out + 8) << 8) | 0xFF);
        wr32(cmd + 8, 0xBC000008u); wr32(cmd + 12, ((uint32_t)(fm & 0xFFFF) << 16) | (uint32_t)(fo & 0xFFFF));
        wr32(dlist, cmd + 16);
    }
    jfg_interp_object_done(rdram, ctx);
}

namespace jfg { void sound_replaced_play(recomp_context* ctx, uint32_t id, uint32_t handle_addr, bool was_playing, bool placed, float x, float y, float z); } // jfg_music.cpp

// amSndPlay(id, handle): jfg_on_sound_play (Hide Floyd mutes his sounds); a callback may change the sound's id. Then a
// sound a mod replaces plays the mod's in its place (jfg_music.cpp).
JFG_WRAP(amSndPlay) {
    uint32_t id = (uint32_t)ctx->r4;
    static const bool log = std::getenv("JFG_DEBUG_SOUNDS") != nullptr; // tests: every sound effect started, by id
    if (log) std::fprintf(stderr, "[jfg] sound 0x%X (tick %u)\n", id & 0xFFFF, jfg::g_tick_count);
    if (!fire_value_cancel(rdram, ctx, jfg::api::Event::SoundPlay, id)) return;
    ctx->r4 = (gpr)(int32_t)id;
    const uint32_t handle_addr = (uint32_t)ctx->r5;
    amSndPlay_orig(rdram, ctx);
    jfg::g_rdram = rdram;
    jfg::sound_replaced_play(ctx, id, handle_addr, false, false, 0.0f, 0.0f, 0.0f);
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

// prnBorder(dList), the frame's last drawing (the screen border, after the scene, the HUD and the hint text):
// jfg_on_draw_hud first, so a mod draws over the HUD (a custom hero's own icons and meters).
JFG_WRAP(prnBorder) {
    using namespace jfg;
    if (api::listened(api::Event::DrawHud)) {
        namespace h = jfg::api::hud_gfx;
        g_rdram = rdram;
        const uint32_t size = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
        call_game(viGetCurrentSize, ctx, { size, size + 4 });
        g_rdram = rdram;
        const uint32_t width = rd32(size), height = rd32(size + 4);
        api::Payload p(ctx, h::SIZE);
        wr32(p.addr + h::GFX, (uint32_t)ctx->r4);
        wr32(p.addr + h::WIDTH, width);
        wr32(p.addr + h::HEIGHT, height);
        api::fire(rdram, ctx, api::Event::DrawHud, (int32_t)p.addr);
    }
    prnBorder_orig(rdram, ctx);
    jfg::overlay::frame_done(); // what mods sent for this frame's overlay is shown from now on
}

// instQueueMsg(player, msg): a message for a player's HUD panel (a pick-up's name, a hit): jfg_on_hud_message first.
// player is the hero's data (+0x00 the player, +0x01 the character).
JFG_WRAP(instQueueMsg) {
    using namespace jfg;
    if (api::listened(api::Event::HudMessage)) {
        g_rdram = rdram;
        const gpr a0 = ctx->r4, a1 = ctx->r5;
        const uint32_t pl = (uint32_t)a0;
        const int player = valid_ptr(pl) ? (int8_t)rd8(pl) : -1;
        const int character = valid_ptr(pl) ? rd8(pl + 1) & 3 : -1;
        api::fire(rdram, ctx, api::Event::HudMessage, (int32_t)a1, character, player);
        ctx->r4 = a0;
        ctx->r5 = a1;
    }
    instQueueMsg_orig(rdram, ctx);
}

// hitPlayerAimCheck(obj, dist, outDist): with the aim lock off (jfg_aim_lock), the game's check still runs (homing
// weapons' targets, the box on an enemy), but the reticle stays where the aim ray meets the level (outDist keeps its
// value) and shows no lock (it returns 0).
JFG_WRAP(hitPlayerAimCheck) {
    using namespace jfg;
    if (api::g_aim_lock) { hitPlayerAimCheck_orig(rdram, ctx); return; }
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4, out = (uint32_t)ctx->r6;
    const uint32_t kept = out != 0 ? rd32(out) : 0;
    hitPlayerAimCheck_orig(rdram, ctx);
    g_rdram = rdram;
    if (out != 0) wr32(out, kept);
    // The aim is on something: keep frontPlayerTarget's countdown (the hero's data + 0x541, high nibble) full, as a
    // lock does. Run out, it forgets the target (+0x1A8[5]) and the next check finds it again as new, replaying the
    // scan sound (0x1D) over and over while the reticle rests on a Tribal.
    const uint32_t data = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    if (ctx->r2 != 0 && valid_ptr(data)) wr8(data + 0x541, (uint8_t)((rd8(data + 0x541) & 0x0F) | 0xA0));
    ctx->r2 = 0; // frontPlayerTarget sets the lock (the hero's data + 0x540, bit 0) from what this returns
}

// frontDrawTarget(player, x, y, trans, depth)'s wrapper (jfg_interpolation.cpp), before the game's code:
// jfg_on_reticle_draw with where the reticle goes. Returns false when a callback cancelled the game's reticle.
// JfgReticle: s32 x, y (screen pixels), f32 depth (0 near .. 1 at the aim's reach), f32 scale, s32 locked, s32 player,
// s32 weapon, s32 cancel, Gfx** gfx (the HUD's display list, frontgfx, where the game draws it), s32 width, height (the
// screen in pixels).
bool jfg_reticle_event(uint8_t* rdram_, recomp_context* ctx) {
    using namespace jfg;
    if (!api::listened(api::Event::ReticleDraw)) return true;
    g_rdram = rdram_;
    const uint32_t player = (uint32_t)ctx->r4, trans = (uint32_t)ctx->r7;
    const uint32_t depth_bits = rd32((uint32_t)ctx->r29 + 0x10);
    int32_t cancel;
    {
        constexpr uint32_t ADDR_frontgfx = 0x800FF398; // Gfx*, the HUD's display list
        const uint32_t size = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
        call_game(viGetCurrentSize, ctx, { size, size + 4 });
        g_rdram = rdram_;
        const uint32_t width = rd32(size), height = rd32(size + 4);
        api::Payload p(ctx, 0x2C);
        wr32(p.addr + 0x00, (uint32_t)(int32_t)(int16_t)ctx->r5);
        wr32(p.addr + 0x04, (uint32_t)(int32_t)(int16_t)ctx->r6);
        wr32(p.addr + 0x08, depth_bits);
        wr32(p.addr + 0x0C, valid_ptr(trans) ? rd32(trans + 0x08) : 0x3F800000u);
        wr32(p.addr + 0x10, valid_ptr(player) ? (rd8(player + 0x540) & 1u) : 0u);
        wr32(p.addr + 0x14, valid_ptr(player) ? rd8(player + 0x00) : 0u);
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
        const int w = ch != 0 ? Character::CurrentWeapon::get(ch) : 0xFF;
        wr32(p.addr + 0x18, (uint32_t)(w < 16 ? w : -1));
        wr32(p.addr + 0x1C, 0);
        wr32(p.addr + 0x20, ADDR_frontgfx);
        wr32(p.addr + 0x24, width);
        wr32(p.addr + 0x28, height);
        api::fire(rdram_, ctx, api::Event::ReticleDraw, (int32_t)p.addr);
        g_rdram = rdram_;
        cancel = (int32_t)rd32(p.addr + 0x1C);
    }
    return cancel == 0;
}

// instDrawHealth(hero, health, segments, updateRate)'s wrapper (jfg_hud_corners.cpp), before the game's code:
// jfg_on_health_draw. Returns false when a callback cancelled the game's meter.
// JfgHealth: void* hero, s32 health (units), segments (6, +1 per health upgrade, up to 12), max (5 * segments + 4),
// character, players (1 in the single-player game), frames, cancel.
bool jfg_health_event(uint8_t* rdram_, recomp_context* ctx) {
    using namespace jfg;
    if (!api::listened(api::Event::HealthDraw)) return true;
    g_rdram = rdram_;
    constexpr uint32_t ADDR_numberOfPlayers = 0x800A4FD0; // u8
    const uint32_t hero = (uint32_t)ctx->r4;
    const uint32_t data = valid_ptr(hero) ? Obj::Data::get(hero) : 0;
    const int segments = (int32_t)ctx->r6;
    const gpr a0 = ctx->r4, a1 = ctx->r5, a2 = ctx->r6, a3 = ctx->r7;
    int32_t cancel;
    {
        api::Payload p(ctx, 0x20);
        wr32(p.addr + 0x00, hero);
        wr32(p.addr + 0x04, (uint32_t)(int32_t)a1);
        wr32(p.addr + 0x08, (uint32_t)segments);
        wr32(p.addr + 0x0C, (uint32_t)(5 * segments + 4));
        wr32(p.addr + 0x10, valid_ptr(data) ? (PlayerData::Character::get(data) & 3u) : 0u);
        wr32(p.addr + 0x14, rd8(ADDR_numberOfPlayers));
        wr32(p.addr + 0x18, (uint32_t)(int32_t)a3);
        wr32(p.addr + 0x1C, 0);
        api::fire(rdram_, ctx, api::Event::HealthDraw, (int32_t)p.addr);
        g_rdram = rdram_;
        cancel = (int32_t)rd32(p.addr + 0x1C);
    }
    ctx->r4 = a0;
    ctx->r5 = a1;
    ctx->r6 = a2;
    ctx->r7 = a3;
    return cancel == 0;
}
