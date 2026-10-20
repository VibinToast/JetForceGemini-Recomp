// Jet Force Gemini: Recompiled mod API. Include it in a mod (it needs the mod template's modding.h).
//
// Helpers are functions the game provides; events are moments a mod can react to, with
//     RECOMP_CALLBACK("*", jfg_on_game_tick) void my_tick(int frames) { ... }
// The game's own features use the same events and run first; mods' callbacks follow in mod-list order.
// Released helpers and events don't change; new ones come with a higher JFG_API_VERSION.
#ifndef JFG_API_H
#define JFG_API_H

#include "modding.h"

#define JFG_API_VERSION 1

// --- helpers -------------------------------------------------------------------------------------------------------

// The API version the running game provides. A mod that needs a newer one should check it.
RECOMP_IMPORT("*", int jfg_api_version(void));

// How much game time this tick covers compared with the original game: 1.0 at 30 fps, 0.5 at 60 fps. Multiply
// per-tick changes (movement, timers, counters) by it so a mod behaves the same at both frame rates.
RECOMP_IMPORT("*", float jfg_get_tick_scale(void));

// The current level number.
RECOMP_IMPORT("*", int jfg_get_level(void));

// --- event data ---------------------------------------------------------------------------------------------------

// jfg_on_blood_spurt: a spurt of blood drops. Change any field; set cancel to 1 to drop it; amount is the drops in
// percent (100 = the game's own, 200 = twice as many; the extra ones spread a little).
typedef struct {
    float x, y, z;
    float vx, vy, vz;
    int alpha;
    int count;
    float scale;
    int amount;
    int cancel;
} JfgBloodSpurt;

// --- events (since version 1) --------------------------------------------------------------------------------------

// jfg_on_game_tick(int frames)
//     Once per game tick, before the game runs it. frames: video frames the tick covers (2 at 30 fps, 1 at 60 fps).
// jfg_on_level_change(int level, int character)
//     The game starts changing to `level`, with `character` playing.
// jfg_on_boss_fight_control(void* player, int frames, int character)
//     Each tick the player is controlled in a boss fight, before the game's control code runs. player: the player's
//     object; frames: video frames the tick covers; character: 0 Vela, 1 Juno, 2 Lupus.
// jfg_on_blood_spurt(JfgBloodSpurt* blood)
//     Blood drops are about to be added. The game's own options (No blood, Blood amount) have already applied.
// jfg_on_blood_hit(JfgBloodHit* hit)
//     A blood hit sprite is about to be added.
// jfg_on_severed_limb_tick(void* limb, int frames)
//     Each tick a severed body part (an object) is updated, before the game's code for it.

#endif
