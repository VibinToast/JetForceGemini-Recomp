// Jet Force Gemini: Recompiled mod API. Include it in a mod (it needs the mod template's modding.h).
//
// Helpers are functions the game provides; events are moments a mod can react to, with
//     RECOMP_CALLBACK("*", jfg_on_game_tick) void my_tick(int frames) { ... }
// The game's own features use the same events and run first; mods' callbacks follow in mod-list order.
// Released helpers and events don't change; new ones come with a higher JFG_API_VERSION.
#ifndef JFG_API_H
#define JFG_API_H

#include "modding.h"

#define JFG_API_VERSION 3

// --- helpers -------------------------------------------------------------------------------------------------------

// The API version the running game provides. A mod that needs a newer one should check it.
RECOMP_IMPORT("*", int jfg_api_version(void));

// How much game time this tick covers compared with the original game: 1.0 at 30 fps, 0.5 at 60 fps. Multiply
// per-tick changes (movement, timers, counters) by it so a mod behaves the same at both frame rates.
RECOMP_IMPORT("*", float jfg_get_tick_scale(void));

// The current level number.
RECOMP_IMPORT("*", int jfg_get_level(void));

// (Since version 2.) The language shown: copies the locale's id into `id` (0 ended, cut to `size`) and returns 0 for
// English, 1 for any other. Ids: "builtin.1" to "builtin.4" for the game's own language slots, "mod.<mod id>" for a
// locale mod (a mod with jfg_locale.json; see the port's locale notes).
RECOMP_IMPORT("*", int jfg_get_language(char* id, int size));

// (Since version 3.) Planets and galaxies that mods add to the planet-select map (jfg_world.json). A planet or galaxy
// is named "<mod id>:<id>" ("main" is the game's own galaxy); the helpers take the index jfg_world_find /
// jfg_galaxy_find return (-1 = not loaded). Opening, entering and completing are kept per save file.
RECOMP_IMPORT("*", int jfg_world_find(const char* planet));
// Bits: 1 open (shown on the map), 2 entered (its level was played), 4 completed. -1: no such planet.
RECOMP_IMPORT("*", int jfg_world_state(int planet));
// Opens a planet ("unlock": "script" planets wait for this).
RECOMP_IMPORT("*", void jfg_world_unlock(int planet));
// Marks a planet completed: planets that open after it ("unlock": {"completed": ...}) show on the map.
RECOMP_IMPORT("*", void jfg_world_complete(int planet));
RECOMP_IMPORT("*", int jfg_galaxy_find(const char* galaxy));
// The galaxy the map shows, -1 while the map is closed.
RECOMP_IMPORT("*", int jfg_galaxy_current(void));
// The next time the map opens, it opens on this galaxy (when one of its planets is open).
RECOMP_IMPORT("*", void jfg_galaxy_show(int galaxy));

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

// jfg_on_blood_hit: a blood hit sprite. Set cancel to 1 to drop it.
typedef struct {
    float x, y, z;
    int cancel;
} JfgBloodHit;

// jfg_on_object_draw: an object is about to be drawn. Set cancel to 1 to draw nothing for it.
typedef struct {
    void* object;
    int cancel;
} JfgObjectDraw;

// jfg_on_sound_play: a sound effect is about to start. Change id to play another; set cancel to 1 to play nothing.
typedef struct {
    int id;
    int cancel;
} JfgSoundPlay;

// jfg_on_screen_flash: the screen is about to flash (the red hit flash). Change color (RGBA) or set cancel to 1.
typedef struct {
    unsigned int color;
    int cancel;
} JfgScreenFlash;

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
// jfg_on_object_draw(JfgObjectDraw* draw)
//     An object is about to be drawn (after the game's own option, Hide Floyd, has applied).
// jfg_on_sound_play(JfgSoundPlay* sound)
//     A sound effect is about to start.
// jfg_on_screen_flash(JfgScreenFlash* flash)
//     The screen is about to flash; the Hit flash opacity option has already applied.
// jfg_on_sidekick_tick(void* floyd, int frames)
//     Each tick Floyd is updated, before the game's code for him.

// --- events (since version 2) --------------------------------------------------------------------------------------

// jfg_on_language_change(void)
//     The player chose another language; the new one is shown from this tick on (jfg_get_language).

// --- events (since version 3) --------------------------------------------------------------------------------------

// jfg_on_map_open(int galaxy)
//     The planet-select map opened (from the title or the pause menu) on that galaxy.
// jfg_on_galaxy_change(int galaxy)
//     The player switched galaxy on the map (L / R).
// jfg_on_planet_select(int planet)
//     The player chose a mod's planet; its level starts next.

#endif
