// Jet Force Gemini port: gameplay options for the game hooks - no blood, damage taken, jetpack refuelling, the sniper
// rifle and the Tribal check.
#include <algorithm>
#include <cmath>
#include <cstdlib>

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
    const uint32_t hit = obj != 0 ? rd32(obj + 0x4C) : 0;
    if (!valid_ptr(hit)) { last_obj = 0; last_health = -1; return; }
    int health = (int16_t)rd16(hit + 6);
    if (obj == last_obj && last_health >= 0 && health < last_health && g.damage_taken != 100 && !g.invulnerable) {
        const int lost = last_health - health;
        const int scaled = (int)std::lround(lost * (double)g.damage_taken / 100.0);
        health = std::max(-0x7FFF, last_health - scaled);
        wr16(hit + 6, (uint16_t)(int16_t)health);
    }
    last_obj = obj;
    last_health = health;
}

// --- jetpack refuelling ----------------------------------------------------------------------------------------------
// Hook in refuelControl (overlay 100): the fuel a pad adds this tick.
extern "C" int jfg_refuel_amount(int amount) {
    if (g.refuel_speed == 100 || amount <= 0) return amount;
    return std::max(1, (int)std::lround(amount * (double)g.refuel_speed / 100.0));
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
    return g.skip_tribal_check && flag == 88 ? 1 : 0;
}

} // namespace jfg
