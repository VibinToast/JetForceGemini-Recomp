// Jet Force Gemini: Recompiled mod API. Include it in a mod (it needs the mod template's modding.h).
//
// Helpers are functions the game provides; events are moments a mod can react to, with
//     RECOMP_CALLBACK("*", jfg_on_game_tick) void my_tick(int frames) { ... }
// The game's own features use the same events and run first; mods' callbacks follow in mod-list order.
// Released helpers and events don't change; new ones come with a higher JFG_API_VERSION.
#ifndef JFG_API_H
#define JFG_API_H

#include "modding.h"

#define JFG_API_VERSION 4

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

// (Since version 4.) Heroes, the controller, the level's collision and damage: what a custom hero needs.

// While on, the game's own control code for that hero (1 = Juno; the only one so far) doesn't run:
// jfg_on_hero_control fires in its place each tick, and the mod moves the hero object itself (position at +0x0C,
// velocity at +0x1C, yaw at +0x00). A dead hero (health under 256) runs the game's code, so death and the restart are
// the game's own.
RECOMP_IMPORT("*", void jfg_hero_takeover(int character, int on));

// The first player's controller, as the hero's control reads it this tick. stick: after the game's own handling (and
// the port's mouse and dual-stick options); raw: the pad's own -80..80. Buttons are libultra's (A 0x8000, B 0x4000,
// Z 0x2000, START 0x1000, L 0x20, R 0x10, C 0x0F). camera_yaw: the camera the hero steers by.
typedef struct {
    int stick_x, stick_y;
    int raw_x, raw_y;
    unsigned int held;
    unsigned int pressed;
    unsigned int camera_yaw;
} JfgInput;
RECOMP_IMPORT("*", void jfg_input_get(JfgInput* out));

// The camera the hero steers by: its position, yaw and pitch (the game's angles, 0x10000 a turn).
typedef struct {
    float x, y, z;
    unsigned int yaw, pitch;
} JfgCameraView;
RECOMP_IMPORT("*", void jfg_camera_get(JfgCameraView* out));

// One of the level's collision triangles: three corners in level units, the flags of its batch (0x2000 water,
// 0x8000 hurts, 0x80000000 kills; others not named yet) and its texture.
typedef struct {
    short v[3][3];
    short pad;
    unsigned int flags;
    unsigned int texture;
} JfgTriangle;
// Copies the triangles of batches with none of ignore_mask's flags, from index first, at most max; returns how many
// there are in all (out = 0 only counts). The level's render triangles are its collision.
RECOMP_IMPORT("*", int jfg_level_triangles(JfgTriangle* out, int first, int max, unsigned int ignore_mask));

// The first triangle under (x, z) whose batch has one of mask's flags: its height (y) and its batch's flags (0 = none).
typedef struct {
    float x, z;
    unsigned int mask;
    float y;
    unsigned int flags;
} JfgFloor;
RECOMP_IMPORT("*", void jfg_level_floor(JfgFloor* q));

// Takes damage (256 = one unit of health) from every object with health whose hit centre is in the sphere, except
// source; returns how many it hit.
typedef struct {
    float x, y, z, radius;
    int damage;
    void* source;
} JfgDamageSphere;
RECOMP_IMPORT("*", int jfg_damage_sphere(const JfgDamageSphere* sphere));

// The objects with health (enemies, crates, heroes) whose hit centre is within a sphere, except source, nearest
// first: up to max of them into out; returns how many there are.
typedef struct {
    float x, y, z, radius;
    void* source;
} JfgNear;
typedef struct {
    void* object;
    float x, y, z; // its hit centre
    int health;
} JfgNearObject;
RECOMP_IMPORT("*", int jfg_objects_near(const JfgNear* query, JfgNearObject* out, int max));

// An object's health (256 per unit; the heroes' and enemies' alike), -1 if it has none; and setting it.
RECOMP_IMPORT("*", int jfg_object_health(void* object));
RECOMP_IMPORT("*", void jfg_object_set_health(void* object, int health));

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

// jfg_on_object_draw_gfx: an object is about to be drawn, with where its drawing goes: gfx, mtx and vtx point at the
// game's own cursors (Gfx**, Mtx**, Vertex**). A callback may write commands at *gfx (advancing it), and set cancel to
// 1 to skip the game's own drawing of the object. The game's camPushModelMtx / camPopModelMtx and texDPTextureX can be
// called with them.
typedef struct {
    void* object;
    void** gfx;
    void** mtx;
    void** vtx;
    int cancel;
} JfgObjectGfx;

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

// --- events (since version 4) --------------------------------------------------------------------------------------

// jfg_on_hero_control(void* hero, int frames, int character)
//     Each tick a taken-over hero (jfg_hero_takeover) is controlled, in place of the game's code. frames: ticks of
//     1/60 s the tick covers (2 at 30 fps). The controller has been read (jfg_input_get).
// jfg_on_object_draw_gfx(JfgObjectGfx* draw)
//     An object is about to be drawn (after jfg_on_object_draw): write your own commands, or cancel the game's.

#endif
