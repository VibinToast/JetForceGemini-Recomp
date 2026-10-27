// Jet Force Gemini port: mod API version 5 helpers for the official mods (Recomp-Docs OFFICIAL_MODS_PLAN.md): the
// reticle (jfg_on_reticle_draw, the aim lock), toggled buttons, a hero's dash and protection, Floyd and his shots,
// setting enemies alight or electrocuting them, ground fire, and the flamethrower's and shocker's reach. Layouts in
// mod_api/jfg_api.h. The blood helpers are in jfg_gore.cpp, Hide Floyd in jfg_visuals.cpp.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" void objSetupObject(uint8_t* rdram, recomp_context* ctx);
extern "C" void hitsCheckLineOfSight(uint8_t* rdram, recomp_context* ctx);
extern "C" void SetSquaddieOnFire(uint8_t* rdram, recomp_context* ctx);
extern "C" void squadsElectrocuteSquaddie(uint8_t* rdram, recomp_context* ctx);
extern "C" void squadsPlayerShootsSquaddie(uint8_t* rdram, recomp_context* ctx);
extern "C" void amSndPlay(uint8_t* rdram, recomp_context* ctx);
extern "C" void hitPlayerAimCheck_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void frontDrawTarget_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace jfg::api {
namespace {
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0;         // the squads' code (overlay 3)
constexpr uint32_t ADDR_cooperativeGame = 0x800A4FC8;  // u8: a second player flies Floyd
constexpr int16_t KIND_SQUAD_MEMBER = 0x18;

// The hero's per-tick data (Obj::Data of a hero, ControlPlayer in the decomp's charControl.h).
namespace Hero {
using PushX      = Field<float, 0x6C>;     // a push (knockback): world x and z per 1/60 s tick, the move functions
using PushZ      = Field<float, 0x70>;     // add it while PushTicks runs, and walls stop it as they stop walking
using PushTicks  = Field<int8_t, 0x149>;
using State      = Field<uint16_t, 0x568>; // low 4 bits: 0 walk/run, 1 stand, 2 crouch, 3 air, 4 swim, 5/11 aim, 10 dead
using AimLock    = Field<uint8_t, 0x540>;  // bit 0: the reticle has locked on
using Protected  = Field<int16_t, 0x5C8>;  // 60 Hz ticks of protection: damage is dropped, enemies don't aim at it
using Floyd      = Field<uint32_t, 0x50C>; // Floyd's object while he follows this hero
using FloydFlown = Field<uint8_t, 0x10D>;  // bit 0: the player flies Floyd
using Beam       = Field<uint32_t, 0x560>; // the shocker's beam object
using Flames     = Field<uint32_t, 0x564>; // the flamethrower's particles
}
// The shocker's beam (Obj::Data of object 0xB6, behaviour 0x98).
namespace Beam {
using EndX   = Field<float, 0xA48>;
using EndY   = Field<float, 0xA4C>;
using EndZ   = Field<float, 0xA50>;
using Active = Field<uint8_t, 0xA7D>;
}
// The flamethrower's block (Hero::Flames): 30 particles of 0x34 bytes from +0x08.
constexpr uint32_t FLAME_FIRST = 0x08, FLAME_SIZE = 0x34, FLAME_X = 0x00, FLAME_LIFE = 0x2C;
constexpr int FLAMES = 30;

uint32_t hero_data(uint32_t hero) {
    if (!valid_ptr(hero) || Obj::Kind::get(hero) != 1) return 0; // behaviour 1: a hero
    const uint32_t data = Obj::Data::get(hero);
    return valid_ptr(data) ? data : 0;
}

bool is_squad_member(uint32_t obj) {
    return valid_ptr(obj) && Obj::Kind::get(obj) == KIND_SQUAD_MEMBER && valid_ptr(Obj::Data::get(obj)) &&
           jfg_overlay_loaded_at(OVERLAY_3_ROM) != 0;
}

// A spawn entry for objSetupObject, in the gap call_game leaves below $sp (objSetupObject copies it).
uint32_t entry_at(recomp_context* ctx) { return ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; }

int16_t angle(float y, float x) { return (int16_t)(int32_t)std::lround(std::atan2(y, x) * (32768.0f / 3.14159265f)); }

// --- the reticle --------------------------------------------------------------------------------------------------
bool g_aim_lock = true; // jfg_aim_lock

// jfg_aim_lock(int on): 0 = the reticle no longer jumps onto an enemy it passes over.
void aim_lock(uint8_t*, recomp_context* ctx) { g_aim_lock = ctx->r4 != 0; }

// --- toggled buttons (input thread) --------------------------------------------------------------------------------
std::atomic<uint16_t> g_toggle_mask{ 0 };    // jfg_input_toggle: buttons that toggle
std::atomic<uint16_t> g_toggle_latched{ 0 }; // ... and read as held now
std::atomic<uint16_t> g_toggle_release{ 0 }; // jfg_input_toggle_release: let go at the next poll

// jfg_input_toggle(unsigned int buttons, int on): those N64 buttons toggle: a press holds them down, the next lets go.
void input_toggle(uint8_t*, recomp_context* ctx) {
    const uint16_t mask = (uint16_t)ctx->r4;
    if (ctx->r5 != 0) g_toggle_mask.fetch_or(mask);
    else { g_toggle_mask.fetch_and((uint16_t)~mask); g_toggle_latched.fetch_and((uint16_t)~mask); }
}

// jfg_input_toggle_release(unsigned int buttons): toggled buttons held now are let go.
void input_toggle_release(uint8_t*, recomp_context* ctx) { g_toggle_release.fetch_or((uint16_t)ctx->r4); }

// jfg_input_toggled(void): the toggled buttons held now.
void input_toggled(uint8_t*, recomp_context* ctx) { ctx->r2 = g_toggle_latched.load(); }

// --- heroes -------------------------------------------------------------------------------------------------------
// jfg_hero_get(void): the first player's hero object, or NULL.
void hero_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = rd32(ADDR_mainGameMode) == 0 ? player_object(ctx) : 0;
    ctx->r2 = (gpr)(int32_t)(hero_data(obj) != 0 ? obj : 0);
}

// jfg_hero_state(void* hero): the hero's state (low 4 bits), -1 if it is not a hero.
void hero_state(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    ctx->r2 = data != 0 ? (gpr)(Hero::State::get(data) & 0xF) : (gpr)-1;
}

// jfg_hero_weapon(void): the weapon in the first player's hand (the character's current weapon), -1 if none.
void hero_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (rd32(ADDR_mainGameMode) == 0 && c >= 0 && c < 3) ? character_ptr(c) : 0;
    const int w = ch != 0 ? Character::CurrentWeapon::get(ch) : 0xFF;
    ctx->r2 = (gpr)(w < 16 ? w : -1);
}

// jfg_hero_push(void* hero, float x, float z, int ticks): the hero is pushed x, z level units per 1/60 s for that
// many 1/60 s ticks (the game's knockback: walls stop it); the game eases it off after.
void hero_push(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    if (data == 0) return;
    auto as_float = [](gpr bits) { const uint32_t v = (uint32_t)bits; float f; std::memcpy(&f, &v, 4); return f; };
    Hero::PushX::set(data, as_float(ctx->r5));
    Hero::PushZ::set(data, as_float(ctx->r6));
    Hero::PushTicks::set(data, (int8_t)std::clamp((int)ctx->r7, 0, 127));
}

// jfg_hero_protect(void* hero, int ticks): no damage reaches the hero for at least that many 1/60 s ticks (8 at
// least: the game's own protection, as after a level exit).
void hero_protect(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    if (data == 0) return;
    const int ticks = std::clamp((int)ctx->r5, 8, 0x7FFF);
    if (Hero::Protected::get(data) < ticks) Hero::Protected::set(data, (int16_t)ticks);
}

// --- Floyd --------------------------------------------------------------------------------------------------------
// jfg_floyd_get(int* state): Floyd's object while he is with the first player's hero, or NULL. state (may be NULL):
// 0 he follows the hero (the game's AI), 1 a player flies him (a co-op game, or the hero flying him), 2 a Floyd
// mission.
void floyd_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t state_out = (uint32_t)ctx->r4;
    const uint32_t hero = rd32(ADDR_mainGameMode) == 0 ? player_object(ctx) : 0;
    const uint32_t data = hero_data(hero);
    const uint32_t floyd = data != 0 ? Hero::Floyd::get(data) : 0;
    int state = 0;
    if (rd8(ADDR_robotMission) != 0) state = 2;
    else if (rd8(ADDR_cooperativeGame) != 0 || (data != 0 && (Hero::FloydFlown::get(data) & 1) != 0)) state = 1;
    if (state_out != 0) wr32(state_out, (uint32_t)state);
    ctx->r2 = (gpr)(int32_t)(valid_ptr(floyd) ? floyd : 0);
}

// jfg_fire_bolt(const JfgBolt* bolt): a laser bolt (Floyd's: object 0x50) from bolt's start towards its target.
// Returns the bolt object, NULL if none was made. JfgBolt: f32 x, y, z, tx, ty, tz; s32 damage (units of health,
// 1..127); f32 speed (Floyd's: 35); s32 life (ticks of 1/10 s; Floyd's: 30); owner (the bolts don't hit it); s32 sound
// (-1 none; Floyd's: 0xDA).
void fire_bolt(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t b = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (b == 0) return;
    const float x = rdf(b + 0x00), y = rdf(b + 0x04), z = rdf(b + 0x08);
    const float dx = x - rdf(b + 0x0C), dy = rdf(b + 0x10) - y, dz = z - rdf(b + 0x14);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x24; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, 0x50);
    wr8(e + 0x02, 0x24);
    wr16(e + 0x04, (uint16_t)(int16_t)std::clamp(x, -32768.0f, 32767.0f));
    wr16(e + 0x06, (uint16_t)(int16_t)std::clamp(y, -32768.0f, 32767.0f));
    wr16(e + 0x08, (uint16_t)(int16_t)std::clamp(z, -32768.0f, 32767.0f));
    wr8(e + 0x0A, (uint8_t)std::clamp((int)rd32(b + 0x18), 1, 127));
    wr8(e + 0x0B, (uint8_t)std::clamp((int)rd32(b + 0x20), 1, 255));
    wr16(e + 0x0C, (uint16_t)angle(dx, dz)); // as Floyd's code works them out (Arctanf)
    wr16(e + 0x0E, (uint16_t)angle(dy, std::sqrt(dx * dx + dz * dz)));
    wr8(e + 0x13, 0xFF);
    wrf(e + 0x14, rdf(b + 0x1C) > 0.0f ? rdf(b + 0x1C) : 35.0f);
    wr32(e + 0x18, rd32(b + 0x24));
    wr32(e + 0x1C, 0x800A1790); // Floyd's bolts' effect tables
    wr32(e + 0x20, 0x800A1850);
    const uint32_t bolt = call_game(objSetupObject, ctx, { e, 1 });
    if (valid_ptr(bolt)) wr32(bolt + 0x3C, 0);
    const int sound = (int)rd32(b + 0x28);
    if (sound >= 0) call_game(amSndPlay, ctx, { (uint32_t)sound, 0 });
    ctx->r2 = (gpr)(int32_t)(valid_ptr(bolt) ? bolt : 0);
}

// jfg_line_of_sight(void* from, void* to, const float* a, const float* b): 1 when nothing (the level, or an object
// with a hit model other than from and to) stands between points a and b (x, y, z each).
void line_of_sight(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t from = (uint32_t)ctx->r4, to = (uint32_t)ctx->r5, a = (uint32_t)ctx->r6, b = (uint32_t)ctx->r7;
    if (a == 0 || b == 0) { ctx->r2 = 0; return; }
    const uint32_t blocked = call_game(hitsCheckLineOfSight, ctx, { from, to, rd32(a), rd32(a + 4), rd32(a + 8), rd32(b), rd32(b + 4), rd32(b + 8) });
    ctx->r2 = blocked == 0 ? 1 : 0;
}

// --- enemies ------------------------------------------------------------------------------------------------------
// jfg_enemy_alert(void* enemy, void* shooter): the enemy was shot by shooter (a hero): its squad turns on it, as
// after a hit by the hero's weapons. Squad members only.
void enemy_alert(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (is_squad_member(obj) && valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
}

// jfg_enemy_ignite(void* enemy, void* shooter): the enemy catches fire as from the flamethrower: it screams, runs for
// water and burns down (shooter, if not NULL, is who set it alight). Squad members only.
void enemy_ignite(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (!is_squad_member(obj)) return;
    if (valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
    call_game(SetSquaddieOnFire, ctx, { obj });
}

// jfg_enemy_electrocute(void* enemy, void* shooter): the enemy is electrocuted: held in place, shaking and losing
// health for about 2 seconds, with sparks. Squad members only; one already electrocuted is left alone.
void enemy_electrocute(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (!is_squad_member(obj)) return;
    if (valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
    call_game(squadsElectrocuteSquaddie, ctx, { obj });
}

// --- effects ------------------------------------------------------------------------------------------------------
// jfg_fx_fire(float x, float y, float z, int scale, int ticks): a fire on the ground (the one explosions leave:
// flames, smoke and light; it hurts nothing by itself) of scale 1..127 (32 a small one) burning for ticks of 1/10 s.
// Returns the fire object, NULL if none was made.
void fx_fire(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    auto as_float = [](gpr bits) { const uint32_t v = (uint32_t)bits; float f; std::memcpy(&f, &v, 4); return f; };
    const float x = ctx->f12.fl, y = ctx->f14.fl, z = as_float(ctx->r6);
    const int scale = (int)ctx->r7;
    const int ticks = (int)rd32((uint32_t)ctx->r29 + 0x10);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x10; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, 0x13D);
    wr8(e + 0x02, 0x0C);
    wr16(e + 0x04, (uint16_t)(int16_t)std::clamp(x, -32768.0f, 32767.0f));
    wr16(e + 0x06, (uint16_t)(int16_t)std::clamp(y, -32768.0f, 32767.0f));
    wr16(e + 0x08, (uint16_t)(int16_t)std::clamp(z, -32768.0f, 32767.0f));
    wr8(e + 0x0A, (uint8_t)std::clamp(scale, 1, 127));
    wr8(e + 0x0B, (uint8_t)std::clamp(ticks, 1, 127));
    const uint32_t fire = call_game(objSetupObject, ctx, { e, 1 });
    if (valid_ptr(fire)) wr32(fire + 0x3C, 0);
    ctx->r2 = (gpr)(int32_t)(valid_ptr(fire) ? fire : 0);
}

// jfg_shocker_end(void* hero, float* out): 1 while the hero's shocker beam is on, with where it ends (x, y, z: what
// it touches, or its full reach) in out.
void shocker_end(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4), out = (uint32_t)ctx->r5;
    const uint32_t beam = data != 0 ? Hero::Beam::get(data) : 0;
    const uint32_t bdata = valid_ptr(beam) ? Obj::Data::get(beam) : 0;
    if (!valid_ptr(bdata) || Beam::Active::get(bdata) == 0) { ctx->r2 = 0; return; }
    if (out != 0) {
        wrf(out + 0, Beam::EndX::get(bdata));
        wrf(out + 4, Beam::EndY::get(bdata));
        wrf(out + 8, Beam::EndZ::get(bdata));
    }
    ctx->r2 = 1;
}

// jfg_flames_get(void* hero, float* out, int max): where the hero's flamethrower flames are now (x, y, z each, up to
// max of them); returns how many there are (0 while it is not firing).
void flames_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4), out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    const uint32_t block = data != 0 ? Hero::Flames::get(data) : 0;
    int n = 0;
    if (valid_ptr(block)) {
        for (int i = 0; i < FLAMES; i++) {
            const uint32_t p = block + FLAME_FIRST + (uint32_t)i * FLAME_SIZE;
            if (rd8(p + FLAME_LIFE) == 0) continue;
            if (out != 0 && n < max) {
                for (uint32_t k = 0; k < 3; k++) wr32(out + (uint32_t)n * 12 + k * 4, rd32(p + FLAME_X + k * 4));
            }
            n++;
        }
    }
    ctx->r2 = (gpr)n;
}
} // namespace

// The input thread, after the controller is read (src/input/controls.cpp, jfg_dual_stick after Swap Triggers): the
// mods' toggled buttons (jfg_input_toggle) flip on each press and read as held while on.
extern "C" void jfg_input_toggles(uint16_t* buttons) {
    const uint16_t mask = g_toggle_mask.load(std::memory_order_relaxed);
    const uint16_t release = g_toggle_release.exchange(0, std::memory_order_relaxed);
    static uint16_t prev = 0;
    const uint16_t b = *buttons;
    uint16_t latched = (uint16_t)(g_toggle_latched.load(std::memory_order_relaxed) & ~release & mask);
    latched ^= (uint16_t)(b & ~prev & mask); // presses flip
    prev = b;
    g_toggle_latched.store(latched, std::memory_order_relaxed);
    *buttons = (uint16_t)((b & ~mask) | latched);
}

void register_api5() {
    recomp::overlays::register_base_export("jfg_aim_lock", aim_lock);
    recomp::overlays::register_base_export("jfg_input_toggle", input_toggle);
    recomp::overlays::register_base_export("jfg_input_toggle_release", input_toggle_release);
    recomp::overlays::register_base_export("jfg_input_toggled", input_toggled);
    recomp::overlays::register_base_export("jfg_hero_get", hero_get);
    recomp::overlays::register_base_export("jfg_hero_state", hero_state);
    recomp::overlays::register_base_export("jfg_hero_weapon", hero_weapon);
    recomp::overlays::register_base_export("jfg_hero_push", hero_push);
    recomp::overlays::register_base_export("jfg_hero_protect", hero_protect);
    recomp::overlays::register_base_export("jfg_floyd_get", floyd_get);
    recomp::overlays::register_base_export("jfg_fire_bolt", fire_bolt);
    recomp::overlays::register_base_export("jfg_line_of_sight", line_of_sight);
    recomp::overlays::register_base_export("jfg_enemy_alert", enemy_alert);
    recomp::overlays::register_base_export("jfg_enemy_ignite", enemy_ignite);
    recomp::overlays::register_base_export("jfg_enemy_electrocute", enemy_electrocute);
    recomp::overlays::register_base_export("jfg_fx_fire", fx_fire);
    recomp::overlays::register_base_export("jfg_shocker_end", shocker_end);
    recomp::overlays::register_base_export("jfg_flames_get", flames_get);
}

bool aim_lock_on() { return g_aim_lock; }

} // namespace jfg::api

// hitPlayerAimCheck(obj, dist, outDist): with the aim lock off (jfg_aim_lock), the game's check still runs (homing
// weapons' targets, the box on an enemy), but the reticle stays where the aim ray meets the level (outDist keeps its
// value) and shows no lock.
JFG_WRAP(hitPlayerAimCheck) {
    using namespace jfg;
    if (api::aim_lock_on()) { hitPlayerAimCheck_orig(rdram, ctx); return; }
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4, out = (uint32_t)ctx->r6;
    const uint32_t kept = out != 0 ? rd32(out) : 0;
    hitPlayerAimCheck_orig(rdram, ctx);
    g_rdram = rdram;
    if (out != 0) wr32(out, kept);
    const uint32_t data = api::hero_data(obj);
    if (data != 0) api::Hero::AimLock::set(data, (uint8_t)(api::Hero::AimLock::get(data) & ~1u));
}

// frontDrawTarget(player, x, y, trans, depth)'s wrapper (jfg_interpolation.cpp), before the game's code:
// jfg_on_reticle_draw with where the reticle goes. Returns false when a callback cancelled the game's reticle.
// JfgReticle: s32 x, y (screen pixels, as jfg_on_draw_hud's), f32 depth (0 near .. 1 at the aim's reach), f32 scale,
// s32 locked, s32 player, s32 weapon, s32 cancel.
bool jfg_reticle_event(uint8_t* rdram_, recomp_context* ctx) {
    using namespace jfg;
    if (!api::listened(api::Event::ReticleDraw)) return true;
    g_rdram = rdram_;
    const uint32_t player = (uint32_t)ctx->r4, trans = (uint32_t)ctx->r7;
    const uint32_t depth_bits = rd32((uint32_t)ctx->r29 + 0x10);
    int32_t cancel;
    {
        api::Payload p(ctx, 0x20);
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
        api::fire(rdram_, ctx, api::Event::ReticleDraw, (int32_t)p.addr);
        g_rdram = rdram_;
        cancel = (int32_t)rd32(p.addr + 0x1C);
    }
    return cancel == 0;
}
