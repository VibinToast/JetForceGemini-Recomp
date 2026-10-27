// Jet Force Gemini port: gameplay options for the game hooks - no blood, damage taken, jetpack refuelling, the sniper
// rifle, unlimited continues and the Tribal check.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

// bloodSpurtAdd / bloodHitSpriteAdd (overlay 8) return immediately when this is non-zero.
extern "C" int jfg_no_blood(void) {
    static const bool env = std::getenv("JFG_NO_BLOOD") != nullptr;
    return (g.no_blood || env) ? 1 : 0;
}

// --- damage taken ----------------------------------------------------------------------------------------------------
// The character on screen keeps its health in its object's damage data (object + 0x4C, +6; 256 per unit). Whatever
// lowered it since the last tick (enemy fire, falls, explosions) is scaled here, before the game's own death check in
// the player's control code later in this tick. Rises (pickups, respawns) are left alone.
void apply_damage_taken(uint32_t obj) {
    static uint32_t last_obj = 0;
    static int last_health = -1;
    const uint32_t hit = obj != 0 ? Obj::Damage::get(obj) : 0;
    if (!valid_ptr(hit)) { last_obj = 0; last_health = -1; return; }
    int health = Damage::Health::get(hit);
    if (obj == last_obj && last_health >= 0 && health < last_health && g.damage_taken != 100 && !g.invulnerable) {
        const int lost = last_health - health;
        const int scaled = (int)std::lround(lost * (double)g.damage_taken / 100.0);
        health = std::max(-0x7FFF, last_health - scaled);
        Damage::Health::set(hit, (int16_t)health);
    }
    last_obj = obj;
    last_health = health;
}

// --- unlimited continues --------------------------------------------------------------------------------------------
// `continues` is set to 2 on entering a region and mainRestartAfterDeath takes one for each death there; with none left
// a death restarts the whole region from its entrance. With the option it is topped back up every tick, so the region
// title card after a death still shows 2.
void gameplay_tick() {
    if (!g.unlimited_continues || !valid_ptr(rd32(ADDR_gameplay)) || rd32(ADDR_mainGameMode) != 0) return;
    if ((int16_t)rd16(ADDR_continues) < 2) wr16(ADDR_continues, 2);
}

// --- jetpack refuelling ----------------------------------------------------------------------------------------------
// Hook in refuelControl (overlay 100): the fuel a pad adds this tick.
extern "C" int jfg_refuel_amount(int amount) {
    if (g.refuel_speed == 100 || amount <= 0) return amount;
    return std::max(1, (int)std::lround(amount * (double)g.refuel_speed / 100.0));
}

// --- Floyd's fuel ----------------------------------------------------------------------------------------------------
// jfg_on_sidekick_tick handler, before sidekickControl. In a Floyd mission Floyd's data (object + 0x68) holds his fuel
// at +0x1C and at +0x1E the frames left before he loses a unit of it: sidekickControl counts that down by one each
// frame of the tick and, at 0, takes a unit and starts again from 6. Giving back part of the frames makes the fuel last
// floyd_fuel percent as long; the gauge, the low-fuel beeps and the pickups that refill it are the game's.
extern "C" void jfg_floyd_fuel(uint8_t* rdram_, recomp_context* ctx) {
    static double owed = 0.0; // frames given back, below a whole one
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int frames = (int)ctx->r5;
    const uint32_t data = valid_ptr(obj) ? rd32(obj + 0x68) : 0;
    if (rd8(ADDR_robotMission) == 0 || !valid_ptr(data) || (int16_t)rd16(data + 0x1C) <= 0 || frames <= 0) {
        owed = 0.0;
        return;
    }
    owed += frames * (1.0 - 100.0 / g.floyd_fuel);
    const int back = (int)owed;
    owed -= back;
    if (back > 0) wr16(data + 0x1E, (uint16_t)((int16_t)rd16(data + 0x1E) + back));
}

// --- sniper zoom -----------------------------------------------------------------------------------------------------
// Hook in controlGetManualAim where it stores the zoom (1 = none .. 5): with the option the zoom is fixed.
extern "C" float jfg_sniper_zoom(float zoom) {
    if (!g.sniper_fixed_zoom) return zoom;
    return std::clamp((float)g.sniper_zoom / 100.0f, 1.0f, 5.0f);
}

// Hook in the characters' aim control (overlays 15-17), where it skips walking with the sniper rifle because the
// walk keys also zoom its scope. With the fixed zoom the weapon number is hidden there, so the sniper walks too.
extern "C" int jfg_sniper_walk(int weapon) {
    return (weapon == 7 && g.sniper_fixed_zoom) ? -1 : weapon;
}

// --- skip the Tribal check -------------------------------------------------------------------------------------------
// Hook at the entry of mainGetGameFlag: 1 = report the flag as set.
extern "C" int jfg_flag_override(int flag) {
    static const bool floyd = std::getenv("JFG_DEBUG_FLOYD") != nullptr; // tests: Floyd (game flag 3) has joined
    return (g.skip_tribal_check && flag == 88) || (floyd && flag == 3) ? 1 : 0;
}

} // namespace jfg

JFG_WRAP(mainGetGameFlag) {
    if (jfg_flag_override((int)ctx->r4)) {
        ctx->r2 = 1;
        return;
    }
    mainGetGameFlag_orig(rdram, ctx);
}

// Refuel speed: refuelControl's fuel per tick is objTvTimes(updateRate * 6) (its only call of it); scaled here.
static JfgCaller g_refuel;

JFG_WRAP(refuelControl) {
    JfgCaller::Scope scope(g_refuel, ctx, 0x80);
    refuelControl_orig(rdram, ctx);
}

JFG_WRAP(objTvTimes) {
    const bool refuel = g_refuel.direct(ctx);
    objTvTimes_orig(rdram, ctx);
    if (!refuel) return;
    ctx->r2 = (gpr)(int32_t)jfg_refuel_amount((int32_t)ctx->r2);
}

// Sniper rifle walking. Each character's aim control (overlays 15-17) skips walking with the sniper (weapon 7, the
// character's +0x70) because the zoom keys walk; with the fixed zoom it walks like any weapon. The weapon is read once,
// right after frontGetTargetControl returns; until the aim control's next call (Powerf, or controlGetManualAim, or its
// return) the weapon reads as none. The character is at 0x4C($sp) of the aim control (0x50 for Lupus, overlay 17).
static JfgCaller g_aim;
static uint32_t g_aim_char_slot = 0, g_walk_char = 0;

static void sniper_walk_restore(uint8_t* rdram) {
    if (g_walk_char == 0) return;
    jfg::Character::CurrentWeapon::set(rdram, g_walk_char, 7);
    g_walk_char = 0;
}

static void aim_control(uint8_t* rdram, recomp_context* ctx, recomp_func_t* orig, uint32_t frame, uint32_t slot) {
    const uint32_t prev = g_aim_char_slot;
    g_aim_char_slot = slot;
    {
        JfgCaller::Scope scope(g_aim, ctx, frame);
        orig(rdram, ctx);
    }
    g_aim_char_slot = prev;
    sniper_walk_restore(rdram);
}

JFG_WRAP(func_overlay_15_00004658_1F17FD8) { aim_control(rdram, ctx, func_overlay_15_00004658_1F17FD8_orig, 0x58, 0x4C); }
JFG_WRAP(func_overlay_16_00004440_1F22458) { aim_control(rdram, ctx, func_overlay_16_00004440_1F22458_orig, 0x58, 0x4C); }
JFG_WRAP(func_overlay_17_00004614_1F2C15C) { aim_control(rdram, ctx, func_overlay_17_00004614_1F2C15C_orig, 0x60, 0x50); }

JFG_WRAP(frontGetTargetControl) {
    const bool aim = g_aim.direct(ctx);
    frontGetTargetControl_orig(rdram, ctx);
    const uint32_t slot = g_aim_char_slot;
    if (!aim || (uint32_t)ctx->r2 == 0) return;
    const uint32_t ch = (uint32_t)MEM_W(slot, ctx->r29);
    if (ch < 0x80000400u || ch >= 0x80800000u) return;
    if (jfg::Character::CurrentWeapon::get(rdram, ch) == 7 && jfg_sniper_walk(7) != 7) {
        jfg::Character::CurrentWeapon::set(rdram, ch, jfg::Character::NoWeapon);
        g_walk_char = ch;
    }
}

JFG_WRAP(Powerf) {
    if (g_aim.direct(ctx)) sniper_walk_restore(rdram);
    Powerf_orig(rdram, ctx);
}

// Sniper zoom: controlGetManualAim(?, data) stores the zoom (data +0x1EC, 1..5) and its rate (+0x1F0); a fixed zoom
// replaces both (rate 0) after it. Nothing after that store in it reads them.
JFG_WRAP(controlGetManualAim) {
    if (g_aim.direct(ctx)) sniper_walk_restore(rdram);
    const uint32_t data = (uint32_t)ctx->r5;
    controlGetManualAim_orig(rdram, ctx);
    if (data < 0x80000400u || data >= 0x80800000u) return;
    const float zoom = jfg::PlayerData::AimZoom::get(rdram, data);
    const float fixed = jfg_sniper_zoom(zoom);
    if (fixed == zoom) return;
    jfg::PlayerData::AimZoom::set(rdram, data, fixed);
    jfg::PlayerData::AimZoomRate::set(rdram, data, 0.0f);
}
