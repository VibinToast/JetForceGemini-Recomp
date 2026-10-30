// Jet Force Gemini: Recompiled mod API. Include it in a mod (it needs the mod template's modding.h).
//
// Helpers are functions the game provides; events are moments a mod can react to, with
//     RECOMP_CALLBACK("*", jfg_on_game_tick) void my_tick(int frames) { ... }
// The game's own features use the same events and run first; mods' callbacks follow in mod-list order.
// Released helpers and events don't change; new ones come with a higher JFG_API_VERSION.
#ifndef JFG_API_H
#define JFG_API_H

#include "modding.h"

#define JFG_API_VERSION 6

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

// This tick the camera the hero steers by stands at (x, y, z) looking at (fx, fy, fz), rolled by roll (0x10000 a
// turn), with field of view fov (degrees; 0 = the game's). It is applied after the game's own camera code each tick it
// is set, so a mod with a camera of its own (a custom hero's, a boss fight's) sets it every tick; the tick it stops,
// the game's camera carries on from there. While it is set, the port's own camera options (the dual-stick and aiming
// views, the FOV option) leave the camera alone.
typedef struct {
    float x, y, z;
    float fx, fy, fz;
    int roll;
    float fov;
} JfgCameraSet;
RECOMP_IMPORT("*", void jfg_camera_set(const JfgCameraSet* camera));

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
// source, and gives the same to the breakable solid objects it touches (crates, breakable props: a stock crate takes
// about 3 units). targets: JFG_TARGET_HEALTH, JFG_TARGET_PROPS, or 0 for both. Returns how many objects with health it
// hit, and writes how many breakables to props_hit. Health stops at 0: an enemy dies the game's usual way. For an
// overkill (the enemy bursts, as from a big blast), set its health to -0x8000 with jfg_object_set_health.
#define JFG_TARGET_HEALTH 1
#define JFG_TARGET_PROPS 2
typedef struct {
    float x, y, z, radius;
    int damage;
    void* source;
    int targets;
    int props_hit; // out
} JfgDamageSphere;
RECOMP_IMPORT("*", int jfg_damage_sphere(JfgDamageSphere* sphere));

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

// The level's objects of one kind (their behaviour) that are shown, so not a pickup already taken: up to max of them
// into out; returns how many there are. The pickups' kinds (every kind, and which levels place what: Recomp-Docs
// LEVEL_OBJECTS.md):
#define JFG_KIND_WEAPON      43   // weapon pickups
#define JFG_KIND_COLLECTABLE 59   // ship parts, Floyd's parts, coins
#define JFG_KIND_KEY         60
#define JFG_KIND_HEALTH      61
#define JFG_KIND_AMMO        66   // ammo boxes (full, capacity, extra)
#define JFG_KIND_MULTI       79   // multiplayer pickups
#define JFG_KIND_POWERUP     81   // invincibility
#define JFG_KIND_CHEST       98
#define JFG_KIND_QUEST       102  // power cells, circuit breakers
#define JFG_KIND_PART        103  // parts to bring back (lubricant, DNA, ore, machine parts)
#define JFG_KIND_TOKEN       109
#define JFG_KIND_TOTEM       141  // totem poles
typedef struct {
    void* object;
    float x, y, z;
    int kind;
} JfgObjectInfo;
RECOMP_IMPORT("*", int jfg_objects_of_kind(int kind, JfgObjectInfo* out, int max));

// The collision triangles of the solid objects (crates, doors, lifts, props: the game's hit models) whose box overlaps
// the sphere, except source, in world space: up to max of them into out; returns how many there are. flags are their
// batch's (the same bits as JfgTriangle's). Call it each tick for moving ones.
typedef struct {
    float v[3][3];
    unsigned int flags;
    void* object;
} JfgObjectTriangle;
RECOMP_IMPORT("*", int jfg_object_triangles(const JfgNear* query, JfgObjectTriangle* out, int max));

// The first player's screen flashes this colour (RGBA) this tick: call it each tick of the flash. 0xFF0000FF is the
// heroes' red hit flash (they flash for 8 ticks); the player's Hit flash opacity option applies.
RECOMP_IMPORT("*", void jfg_screen_flash(unsigned int rgba));

// An object's health (256 per unit; the heroes' and enemies' alike), -1 if it has none; and setting it.
RECOMP_IMPORT("*", int jfg_object_health(void* object));
RECOMP_IMPORT("*", void jfg_object_set_health(void* object, int health));

// An enemy's type, -1 if the object is not one: a squad member's object id, the same for its whole squadron (BlueAnt
// 0x78, GreenAnt 0x8C, BigShieldAnt 0x9C, ...). A squad member's health below -(its full health / 8) (at least 2 units,
// at most 10) makes it burst on its next update (the game's overkill).
RECOMP_IMPORT("*", int jfg_enemy_type(void* object));

// The squad members (enemies, and the tribals: tribal is 1) whose hit sphere overlaps a sphere, except source,
// nearest first, with their hit centre and radius, health and type (as jfg_enemy_type): up to max of them into out;
// returns how many there are.
// Dying ones are listed too (health 0 or less, until the game removes them): a further big blow still bursts them.
typedef struct {
    void* object;
    float x, y, z; // its hit centre
    float radius;  // its hit radius
    int health;
    int type;
    int tribal;    // a tribal (squad members the heroes rescue), not an enemy
} JfgEnemy;
RECOMP_IMPORT("*", int jfg_enemies_near(const JfgNear* query, JfgEnemy* out, int max));

// The blast damage (grenades, rockets, mines, exploding barrels) an object took since the last call, 0 if none
// (256 = one unit of health); reading it clears it. The port keeps it for the 16 latest objects hit.
RECOMP_IMPORT("*", int jfg_object_blast_damage(void* object));

// While hidden, the game's health meter is not drawn for that hero (0 Vela, 1 Juno, 2 Lupus): a mod that keeps its
// own health for a hero draws its own (jfg_on_draw_hud). Hidden while any mod asks.
RECOMP_IMPORT("*", void jfg_hud_hide_health(int character, int hide));

// The heads the first player has collected as a hero (0 Vela, 1 Juno, 2 Lupus): tribal 1 the tribals', 0 the others'.
RECOMP_IMPORT("*", int jfg_hero_heads(int character, int tribal));

// Sound effects: the mod's own OGG or 16-bit PCM WAV files, named in its jfg_sounds.json
// ({ "sounds": { "<name>": "<file>.ogg" } }, the files among the mod's additional files). volume 1 = as recorded, pan
// -1 left to 1 right, pitch 1 = as recorded (2 an octave up); mixed at the game's sound effects volume. Play returns a
// voice (0: no such sound); a loop plays until stopped. jfg_sound_set changes a playing voice and returns 1 while it
// plays, 0 once it has ended.
// The same file can replace the game's own sound effects (since version 5): "replace": { "0xE1": "<name>",
// "0xFA": { "sound": "<name>", "loop": true, "volume": 0.8 } } by the game's sound id (JFG_DEBUG_SOUNDS=1 logs every id
// played). The game's sound is muted and the mod's plays in its place. A one-shot plays out; a "loop" (for the game's
// looping sounds) stops when the game stops its own. "stopped_by": [ "0xE1" ] ends it when one of those ids plays (a
// charge cut off by its shot). The first enabled mod to replace an id has it. jfg_sound_replace_enable turns one of
// the mod's own replacements off (the game's own sound back) and on again while the game runs; 1 if the id is
// replaced by this mod (another mod's replacement can't be turned off or swapped).
// jfg_sound_replace_use puts another of the mod's sounds (and volume) in for a replaced id from its next play on: one
// game sound shared by two weapons, told apart by jfg_hero_weapon. 1 if done, 0 if not replaced or no such sound.
RECOMP_IMPORT("*", int jfg_sound_replace_enable(int id, int on));
RECOMP_IMPORT("*", int jfg_sound_replace_use(int id, const char* name, float volume));
RECOMP_IMPORT("*", int jfg_sound_play(const char* name, float volume, float pan, float pitch));
RECOMP_IMPORT("*", int jfg_sound_loop(const char* name, float volume, float pan, float pitch));
RECOMP_IMPORT("*", int jfg_sound_set(int voice, float volume, float pan, float pitch));
RECOMP_IMPORT("*", void jfg_sound_stop(int voice));

// While on, a taken-over hero (jfg_hero_takeover) runs the game's own control code again: its aiming and weapons, its
// camera and its moves carry on from where the mod left the hero object. jfg_on_hero_control still fires first each
// tick, so a mod can hand the hero over tick by tick (while the player aims, say).
RECOMP_IMPORT("*", void jfg_hero_game_control(int character, int on));

// The hero (its object) takes its next weapon (direction 1) or the one before (-1), as the game's weapon buttons do.
// While a mod moves a hero the mouse wheel changes its weapon this way (the buttons are the mod's then).
RECOMP_IMPORT("*", void jfg_hero_select_weapon(void* hero, int direction));

// The player's controls. modern: the port's dual-stick controls are in charge now (its Dual Stick or Mouse and
// Keyboard option, during gameplay): the right stick and the mouse turn the port's own camera, and the stick
// jfg_input_get gives is already turned to it (camera-relative to the camera the hero steers by). mouse: the mouse
// and keyboard play (the mouse is captured). buttons: the physical controls held, as a modern pad (JFG_PAD_*): a
// controller's, and while the mouse plays the keys and mouse buttons that stand for them (Space A, Left Ctrl or C B,
// F X, R Y, Q LB, E RB, right mouse button LT, left RT, Left Shift L3, Tab Back, Enter Start, 1-4 the D-pad: up,
// right, down, left). Triggers 0..1.
#define JFG_PAD_A      0x0001
#define JFG_PAD_B      0x0002
#define JFG_PAD_X      0x0004
#define JFG_PAD_Y      0x0008
#define JFG_PAD_LB     0x0010
#define JFG_PAD_RB     0x0020
#define JFG_PAD_LT     0x0040 // pulled over half way
#define JFG_PAD_RT     0x0080
#define JFG_PAD_L3     0x0100
#define JFG_PAD_R3     0x0200
#define JFG_PAD_BACK   0x0400
#define JFG_PAD_START  0x0800
#define JFG_PAD_UP     0x1000
#define JFG_PAD_DOWN   0x2000
#define JFG_PAD_LEFT   0x4000
#define JFG_PAD_RIGHT  0x8000
typedef struct {
    int modern;
    int mouse;
    unsigned int buttons;
    float left_trigger, right_trigger;
} JfgControls;
RECOMP_IMPORT("*", void jfg_controls_get(JfgControls* out));

// The port's dual-stick camera stands scale times as far out as usual (on top of the player's Camera Distance option):
// a hero of another size, say. 1 = as usual. Kept until set again. Each mod's request is kept: the camera stands as
// far out as the farthest asks, else as near as the nearest; 1 takes the mod's request back.
RECOMP_IMPORT("*", void jfg_camera_set_distance(float scale));

// --- helpers (since version 5) -------------------------------------------------------------------------------------

// Blood colours. Blood takes its colour from an index (0..7, JfgBloodSpurt.color, JfgBloodHit.color) into two tables,
// one for drops and splats and one for hit sprites. The game uses 0 (red: heroes, Floyd, Tribals), 1 (green: ground
// insects, most bosses), 2 (black: flyers, drones, turrets) and 4 (lime: the final boss); 3, 5, 6 and 7 are free for
// mods: take one with jfg_blood_color_alloc, fill it and hand it out. Sets index's colour, 0xRRGGBB each; the tables are drawn at different strengths (the
// game's red is 0x500000 for drops, 0xA00000 for hits). 0xFFFFFFFF = the game's own colour. Kept until set again.
RECOMP_IMPORT("*", void jfg_blood_set_color(int index, unsigned int drops, unsigned int hits));
// A colour index of the mod's own, so mods never fill the same one: the mod's nth (0 its first, up to 3); the same nth
// always gives the same index; -1 when the four free ones are taken. Since version 6.
RECOMP_IMPORT("*", int jfg_blood_color_alloc(int nth));
// The game's rainbow blood (a cheat): each drop takes a random colour index (hit sprites keep theirs). On while any
// mod asks; the same for jfg_limbs_keep and jfg_floyd_hide, and jfg_blood_keep keeps the most splats any mod asks for.
RECOMP_IMPORT("*", void jfg_blood_set_rainbow(int on));
// Blood stays: splats no longer fade, up to that many (64..4096; once reached, the oldest make room); 0 = the game's
// own. A level change clears them. Kept until set again.
RECOMP_IMPORT("*", void jfg_blood_keep(int splats));
// Body parts stay: the newest 24 severed limbs no longer fade (drone heads and other pickups are left alone).
RECOMP_IMPORT("*", void jfg_limbs_keep(int on));
// Hide Floyd: Floyd, his shadow and his sounds are gone while he follows the hero (not while a player flies him).
RECOMP_IMPORT("*", void jfg_floyd_hide(int on));

// The reticle: 0 = it no longer jumps onto an enemy the aim passes over (nor shows a lock); shots still go where it
// points, and homing weapons still lock on. 1 = the game's own. Kept until set again. To draw a reticle of your own,
// cancel the game's in jfg_on_reticle_draw and draw yours there (JfgReticle.gfx). Off while any mod asks.
RECOMP_IMPORT("*", void jfg_aim_lock(int on));

// Toggled buttons: while on, each press of those N64 buttons (libultra's, as in JfgInput) holds them down until the
// next press, for the first player (on any controls: pad, dual stick, mouse and keyboard; after Swap Triggers).
RECOMP_IMPORT("*", void jfg_input_toggle(unsigned int buttons, int on));
// Each mod's toggled and blocked buttons are kept apart: a button toggles (or reads as let go) while any mod asks.
// Lets go of those toggled buttons now (a death, a level change): the mod's own toggled buttons only.
RECOMP_IMPORT("*", void jfg_input_toggle_release(unsigned int buttons));
// The toggled buttons held now.
RECOMP_IMPORT("*", unsigned int jfg_input_toggled(void));
// While on, those N64 buttons read as let go for the first player (any controls). Kept until turned off.
RECOMP_IMPORT("*", void jfg_input_block(unsigned int buttons, int on));

// 1 while the world stands still (the pause menu, the settings menu, the fly camera's freeze): jfg_on_game_tick still
// fires then, so a mod that acts in it should wait.
RECOMP_IMPORT("*", int jfg_game_paused(void));
// The first player's hero object while a level is played, else NULL.
RECOMP_IMPORT("*", void* jfg_hero_get(void));
// A hero's state, -1 if the object is not a hero.
#define JFG_HERO_MOVING   0
#define JFG_HERO_STANDING 1
#define JFG_HERO_CROUCHED 2
#define JFG_HERO_IN_AIR   3
#define JFG_HERO_SWIMMING 4
#define JFG_HERO_AIMING   5  // standing; 11 while moving
#define JFG_HERO_AIMING_MOVING 11
#define JFG_HERO_DEAD     10
RECOMP_IMPORT("*", int jfg_hero_state(void* hero));
// The weapon in the first player's hand (0 the pistol .. 14, 15 the proximity mine, 16 up modded weapons: see
// jfg_weapon_register), -1 if none. The shocker is 4, the flamethrower 6.
RECOMP_IMPORT("*", int jfg_hero_weapon(void));
// The first player's ammo for a weapon (any id; 15 up: -1 when not owned), -1 outside a level; and setting it.
RECOMP_IMPORT("*", int jfg_hero_ammo(int weapon));
RECOMP_IMPORT("*", void jfg_hero_set_ammo(int weapon, int ammo));
// Pushes the hero x, z level units per 1/60 s for ticks 1/60 s ticks (the game's knockback: walls stop it, and it
// eases off after). The hero walks a little over 2 units per 1/60 s.
RECOMP_IMPORT("*", void jfg_hero_push(void* hero, float x, float z, int ticks));
// The hero is hit as by an enemy's shot: one unit of health (units 1) or two (2), with the hit reaction, its sound and
// the red flash, unless it is protected (just hit, or jfg_hero_protect).
RECOMP_IMPORT("*", void jfg_hero_hit(void* hero, int units));
// The hero rolls to its left (right 0) or right (1), as the game's side buttons make it roll while it stands: the
// game plays the roll and moves the hero along it. Juno and Vela only; returns 1 if it rolled.
RECOMP_IMPORT("*", int jfg_hero_roll(void* hero, int right));
// Sets the hero's move (its animation, which the game carries on from) from its start: 0 walk, 1 run, 0x10 standing
// still (4 and 0x0E are crouched). The game's side roll ends crouched; a mod that rolls the hero can stand it up again
// with this. Juno and Vela only; returns 1 if set.
#define JFG_MOVE_WALK  0x00
#define JFG_MOVE_RUN   0x01
#define JFG_MOVE_STAND 0x10
RECOMP_IMPORT("*", int jfg_hero_move(void* hero, int move));
// The hero's aim: yaw and pitch (0x10000 a turn, pitch positive up), aiming (the aim view), pitched (aiming or with a
// target: then the pitch counts, else the game's shots go level), dir the unit direction (pitched, else level). 1, or
// 0 if it is not a hero. Since version 6.
typedef struct {
    int yaw, pitch;
    int aiming, pitched;
    float dir[3];
} JfgAim;
RECOMP_IMPORT("*", int jfg_hero_aim(void* hero, JfgAim* out));
// Where the hero's gun barrel is (x, y, z), as the game keeps it for its own shots: 1, or 0 when it isn't kept (the aim
// view draws no gun): start shots a little ahead of the hero then. Since version 6.
RECOMP_IMPORT("*", int jfg_hero_barrel(void* hero, float* out));
// The hero's own voice (Juno's, Vela's or Lupus's, as the game picks them): 0 the jump, 1 the landing, 3 being hit.
#define JFG_SPEAK_JUMP 0
#define JFG_SPEAK_LAND 1
#define JFG_SPEAK_HIT  3
RECOMP_IMPORT("*", void jfg_hero_speak(void* hero, int type));
// The object drawn tinted rgba's colour (0xRRGGBBAA), the alpha its strength (0 removes it), as the game flashes the
// enemy it targets; kept until set again or a level change. Each mod has its own tint on an object (removing yours
// leaves another mod's); the one set last shows.
RECOMP_IMPORT("*", void jfg_object_tint(void* object, unsigned int rgba));
// Memory for a mod's graphics data (textures, display lists, vertices) that the renderer can read: a mod's own memory
// is above what it reads. size bytes, zeroed, 16-byte aligned, kept for the whole run (allocate once); NULL when the
// 4 MB set aside for this is used up. A texture image's address in a display list is the pointer & 0x1FFFFFFF.
RECOMP_IMPORT("*", void* jfg_gfx_alloc(unsigned int size));

// The overlay: pictures, polygons and circles the port draws over the game at the window's own resolution,
// anti-aliased (not on the game's pixels). Places are in overlay units: the game's picture is 240 high and as wide as
// it is shown (jfg_overlay_size: 320 in 4:3, about 427 in 16:9), (0, 0) its top left. Send them each frame the game
// draws (from jfg_on_game_tick or jfg_on_draw_hud): they show from the end of that frame until the next, and go a
// quarter of a second after the game last drew a frame (menus, the title screen). Colours are 0xRRGGBBAA.
RECOMP_IMPORT("*", void jfg_overlay_size(float* width_height));
// A picture to draw (by id), w x h colours 0xRRGGBBAA, at most 1024 x 1024; kept until sent again. Send it once (or when
// it changes): it is copied and uploaded.
RECOMP_IMPORT("*", void jfg_overlay_image(int id, int w, int h, const unsigned int* rgba));
typedef struct {
    float x, y, w, h;   // where, in overlay units
    unsigned int tint;  // multiplies the picture's colours (0xFFFFFFFF as it is; the alpha fades it)
    int id;
} JfgOverlayImage;
RECOMP_IMPORT("*", void jfg_overlay_draw_image(const JfgOverlayImage* image));
typedef struct {
    const float* xy;    // count points: x, y, x, y, ... (at most 512)
    int count;
    unsigned int fill;  // 0: not filled (any polygon whose sides do not cross, convex or not)
    unsigned int line;  // 0: no outline
    float thickness;    // of the outline, in overlay units
} JfgOverlayPolygon;
RECOMP_IMPORT("*", void jfg_overlay_polygon(const JfgOverlayPolygon* polygon));
typedef struct {
    float x, y, radius;
    unsigned int fill, line;
    float thickness;
} JfgOverlayCircle;
RECOMP_IMPORT("*", void jfg_overlay_circle(const JfgOverlayCircle* circle));
// A line of text with the port's fonts, sharp at the window's resolution: its top at y, size units from the top of its
// capitals to the bottom of its descenders; x is its left (align 0), middle (1) or right (2). UTF-8, one line.
#define JFG_FONT_LATO      0   // Lato Bold, the menus' font
#define JFG_FONT_JFG       1   // JFG Bold, the launcher's lettering after the game's
#define JFG_FONT_LATO_THIN 2   // Lato Regular
typedef struct {
    float x, y, size;
    unsigned int color;   // 0xRRGGBBAA
    int align;
    int font;
    const char* text;
} JfgOverlayText;
RECOMP_IMPORT("*", void jfg_overlay_text(const JfgOverlayText* text));
// How wide that text is, in overlay units.
RECOMP_IMPORT("*", void jfg_overlay_text_width(const JfgOverlayText* text, float* width));
// 1 while the single-player HUD is at the window's edges (the HUD in Screen Corners option, in widescreen), 0 while
// it is in the middle 4:3 area.
RECOMP_IMPORT("*", int jfg_hud_in_corners(void));
// The single-player HUD's edges in overlay units: out[0] left, out[1] right. Line up with the game's HUD from these,
// not the overlay's edges: in the corners the HUD keeps to the HUD ratio's area (Graphics > HUD ratio: Clamp 16:9, the
// default, is a centred 16:9 strip in a wider window), else to the middle 320 units.
RECOMP_IMPORT("*", void jfg_hud_area(float* out));
// The screen corner (0 top left, 1 top right, 2 bottom left, 3 bottom right) for the mod's HUD piece: the one it wants
// unless another mod's piece is there, else the next free one round the screen. Ask each frame you draw: a mod holds
// its corner while it keeps asking. Since version 6.
RECOMP_IMPORT("*", int jfg_hud_corner(int wanted));
// A weapon's icon (0 the pistol .. 15), the game's own picture (texture packs replace it), centred at x, y in overlay
// units, scale 1 the size the game's weapon panel shows it (about 30 x 20 units), alpha 0..1. Only during
// jfg_on_weapon_hud: the port draws it with the game's HUD, so it is under the overlay's shapes.
typedef struct {
    int weapon;
    float x, y, scale, alpha;
} JfgWeaponIcon;
RECOMP_IMPORT("*", void jfg_hud_weapon_icon(const JfgWeaponIcon* icon));
// A dark disc in the game's HUD under the weapon icons (the game's own panels' translucent backing): centred at x, y,
// radius in overlay units, color 0xRRGGBBAA. Only during jfg_on_weapon_hud. The overlay is drawn over the game, icons
// included, so a backing that the icons sit on has to be drawn this way. Up to 24 a frame, for all mods together.
typedef struct {
    float x, y, radius;
    unsigned int color;
} JfgHudDisc;
RECOMP_IMPORT("*", void jfg_hud_disc(const JfgHudDisc* disc));
// One of the game's texts (by its id, in the language shown) into out (size bytes, ended by 0); its length, 0 if
// none. Weapon w's name is 0x39 + w; jfg_on_hud_message's msg is a text id too.
RECOMP_IMPORT("*", int jfg_game_text(int id, char* out, int size));
// The area's Tribals: saved, killed and left (the counts the game's objective panel shows). 0 outside a game.
RECOMP_IMPORT("*", int jfg_tribal_counts(int* saved, int* killed, int* left));
// Plays the game's own sound effect id at x, y, z (louder near the camera). Some: Juno's jump 0x4C and 0x51, landing
// 0x6, his side rolls 0x7A (left) and 0x79 (right) (JFG_DEBUG_SOUNDS=1 logs every sound the game starts).
RECOMP_IMPORT("*", void jfg_game_sound(int id, float x, float y, float z));
// Plays the game's own sound effect id, the same loudness wherever the camera is.
RECOMP_IMPORT("*", void jfg_game_sound_flat(int id));
// No damage reaches the hero for at least ticks 1/60 s ticks (8 at the least), and enemies don't aim at it.
RECOMP_IMPORT("*", void jfg_hero_protect(void* hero, int ticks));

// Floyd's object while he is with the first player's hero, else NULL; state (may be NULL): 0 the game's AI flies him
// after the hero, 1 a player flies him, 2 a Floyd mission.
RECOMP_IMPORT("*", void* jfg_floyd_get(int* state));
// Floyd's lights show texture frame (0..255) of his model's light parts while he follows a hero (0 red, 1 blue, 2 green,
// 3 dark): the game shows 3, 1 at times, and blinks 0 while the hero's aim is locked on an enemy (that warning still wins).
// -1 gives them back to the game. Kept until set again.
RECOMP_IMPORT("*", void jfg_floyd_light(int frame));
// A laser bolt (Floyd's) from x, y, z towards tx, ty, tz. damage in units of health (1..127; an ant soldier has a few);
// speed in level units per 1/30 s (Floyd's 35); life in 1/10 s (Floyd's 30); owner is never hit by it; sound -1 = none
// (Floyd's 0xDA). Returns the bolt object, NULL if none was made.
typedef struct {
    float x, y, z;
    float tx, ty, tz;
    int damage;
    float speed;
    int life;
    void* owner;
    int sound;
} JfgBolt;
RECOMP_IMPORT("*", void* jfg_fire_bolt(const JfgBolt* bolt));
// A plasma ball (the plasma shotgun's) from x, y, z towards tx, ty, tz. size: its damage in units of health (1..15), and
// how big it is (the shotgun's charge 1..4; larger ones are drawn as 4); speed in level units per 1/30 s (0: it stays
// put); time in 1/10 s; owner: whose shot it is (its hits and kills count as that hero's plasma shotgun). Returns the
// ball object (its kind is JFG_KIND_PLASMA), NULL if none was made. Since version 5.
#define JFG_KIND_PLASMA 163
typedef struct {
    float x, y, z;
    float tx, ty, tz;
    int size;
    float speed;
    int time;
    void* owner;
} JfgPlasma;
RECOMP_IMPORT("*", void* jfg_fire_plasma(const JfgPlasma* plasma));
// Objects a mod drives as its own (a plasma ball it bounces or pulls, a grenade it takes as its weapon's): claim one
// before changing it, and leave alone those another mod has. jfg_object_claim: 1 if it is now (or already was) yours, 0
// if another mod has it. jfg_object_claimed: 0 no mod's, 1 yours, 2 another mod's. A claim goes when you unclaim it,
// when the object is freed and at a level change. Balls a mod makes with jfg_fire_plasma are already its own.
RECOMP_IMPORT("*", int jfg_object_claim(void* obj));
// A short-lived effect (a dust puff, a spark, an impact flash, a shock wave: one of the pack's object types) the port
// runs to its end: spawned at x, y, z turned to yaw, moved vx, vy, vz level units a tick (1/60 s; no collision) with
// keep of its speed kept each tick (1 or 0: all), scaled from scale_from to scale_to over its ticks (ease_out 1: fast
// at first), and freed when they are up (or at a level change). opacity (0..255; 0 leaves it) is for the fading kinds
// (behaviour 0x7E, which are not drawn until it is set): from fade_from of its life (0..1; 1 never) it fades to
// nothing. The effect's object, or NULL. jfg_object_set_opacity sets a fading kind's opacity (others are left).
typedef struct {
    const char* type;
    float x, y, z;
    int yaw;
    float vx, vy, vz;
    float keep;
    int ticks;
    float scale_from, scale_to;
    int ease_out;
    int opacity;
    float fade_from;
} JfgFx;
RECOMP_IMPORT("*", void* jfg_fx_spawn(const JfgFx* fx));
RECOMP_IMPORT("*", void jfg_object_set_opacity(void* obj, int opacity));
// A pull field (a vortex, a black hole), run by the port: the enemies around x, y, z within reach, with nothing between,
// are caught (tinted tint, 0 none; alerted to owner, or stunned with JFG_PULL_STUN) and slide toward it: strength
// level units a tick at the middle, edge times that at the reach's edge, stopping hold from the middle (JFG_PULL_MASS:
// heavier enemies slower, flyers drawn up or down too). Every hurt_ticks (1/60 s) those within core x reach + hold
// lose damage (256 a unit; never past dying), credited to weapon (-1 none) and owner. Tribals only with
// JFG_PULL_TRIBALS, and never hurt. All mods' pulls run together: an enemy two pulls catch moves by the strongest.
// jfg_pull_set: id -1 makes a new pull and returns its id; your pull's id moves or changes it (call it each tick to
// move it). jfg_pull_end stops it and takes its tints off; a level change ends them all. jfg_pull_caught copies what
// it holds (up to max) and returns how many.
#define JFG_PULL_TRIBALS 1
#define JFG_PULL_MASS    2
#define JFG_PULL_STUN    4
typedef struct {
    float x, y, z;
    float reach, strength, edge, hold, core;
    int damage;
    int hurt_ticks;
    unsigned int tint;
    int flags;
    int weapon;
    void* owner;
} JfgPull;
typedef struct {
    void* enemy;
    int tribal;
} JfgPullCaught;
RECOMP_IMPORT("*", int jfg_pull_set(int id, const JfgPull* pull));
RECOMP_IMPORT("*", void jfg_pull_end(int id));
RECOMP_IMPORT("*", int jfg_pull_caught(int id, JfgPullCaught* out, int max));
// Objects coming and going: while on, the mod gets jfg_on_object_spawn and jfg_on_object_free for objects of that kind
// (JFG_KIND_*), instead of scanning for them each tick. Only the mods watching a kind are told.
RECOMP_IMPORT("*", void jfg_object_watch(int kind, int on));
RECOMP_IMPORT("*", int jfg_object_claimed(void* obj));
RECOMP_IMPORT("*", void jfg_object_unclaim(void* obj));
// The first point of the level (walls, floors, ceilings: the track, as shots test their flight) on the line from a to
// b (x, y, z each): 1 if there is one, with out = its x, y, z and the surface's normal (nx, ny, nz, facing a); 0 if the
// line is clear. Since version 5.
RECOMP_IMPORT("*", int jfg_level_ray(const float* a, const float* b, float* out));
// 1 when nothing (the level, or an object with a hit model other than from and to) stands between a and b (x, y, z).
RECOMP_IMPORT("*", int jfg_line_of_sight(void* from, void* to, const float* a, const float* b));

// Enemies (squad members; the others are left alone): shot by shooter (a hero) as by its weapons, its squad turns on
// it; set alight as by the flamethrower (it screams, runs for water and burns down; Tribals too); electrocuted as by
// the shocker on some foes (held in place and drained for about 2 s, with sparks; not again while it lasts). shooter
// may be NULL.
RECOMP_IMPORT("*", void jfg_enemy_alert(void* enemy, void* shooter));
RECOMP_IMPORT("*", void jfg_enemy_ignite(void* enemy, void* shooter));
RECOMP_IMPORT("*", void jfg_enemy_electrocute(void* enemy, void* shooter));
// 1 when an enemy is dying: no health left, or the sliver some kinds keep while they lie on the ground and bleed out
// (they die by themselves); 0 otherwise; -1 if it is not a squad member.
RECOMP_IMPORT("*", int jfg_enemy_dying(void* enemy));

// A fire on the ground (the one explosions leave: flames, smoke and light; it hurts nothing itself: use
// jfg_damage_sphere) of scale 1..127 (about 32 a small one) for ticks 1/10 s. Returns its object, NULL if none.
RECOMP_IMPORT("*", void* jfg_fx_fire(float x, float y, float z, int scale, int ticks));
// 1 while the hero's shocker beam is on, with where it ends (what it touches, or its full reach) in out[3].
RECOMP_IMPORT("*", int jfg_shocker_end(void* hero, float* out));
// Where the hero's flamethrower flames are now (x, y, z each into out, at most max); returns how many (0: not firing).
RECOMP_IMPORT("*", int jfg_flames_get(void* hero, float* out, int max));

// --- weapons past the game's own (since version 6) -------------------------------------------------------------------
// The game has 16 weapon slots: 0..14 the heroes' weapons, 15 the cut proximity mine. A mod adds weapons by name and
// gets an id for this run (16, 17, ... in the order mods register, any number of them): keep the name, not the
// number. Whichever modded weapon is in hand takes slot 15 (its ammo, its row of weapon stats and its names), so the
// game's own code shows, fires and throws it; next / previous weapon step through all of them. Owned weapons and
// their ammo are saved with the save file, by name. A randomizer lists every weapon (jfg_weapon_count, jfg_weapon_info),
// gives or takes them (jfg_hero_give_weapon, jfg_hero_take_weapon) and points pickups at any of them
// (jfg_weapon_pickup_set).
typedef struct {
    const char* name;         // unique, "mod_id:weapon" (no spaces)
    const char* display;      // its name in the HUD and pick-up message ("" = name)
    const char* ammo_display; // plural, for "more ammo" messages ("" = display)
    int host;         // the stock weapon (0..14) whose shot it fires and how it is held; -1 the default shot (a laser
                      // bolt) or cancel it in jfg_on_weapon_fire and fire your own
    int charge;       // the charge given to the host's shot (as the plasma shotgun's or a mine's type), -1 as it is
    int thrown;       // 1: thrown (the hero's throw), 0: as the host
    int row_from;     // the stock weapon whose stats row it uses (refire, speed, spread, damage): 0..14
    int max_ammo;     // -1 = the row's
    int start_ammo;   // given with the weapon, -1 = the row's
    int pickup_ammo;  // a pickup of it when owned, -1 = the row's
} JfgWeaponDef;
// Its id (16 up; the same id again for a name registered before), -1 if the definition is bad. Register on your first
// jfg_on_game_tick: a weapon saved in the file in play is given back when it registers.
RECOMP_IMPORT("*", int jfg_weapon_register(const JfgWeaponDef* def));
// A weapon's id by name ("stock:pistol" .. "stock:cluster_bombs", "port:proximity_mine", "mod_id:weapon"), -1 none.
RECOMP_IMPORT("*", int jfg_weapon_find(const char* name));
// A modded weapon's HUD icon: a 2D sprite of your pack (its number from jfg_asset_number; a picture of about 32 x 22 as
// the game's, made with jfgformats' sprites.write and HUD_ICON_ANCHOR), shown in the weapon panel, the weapon strip and
// the pause screen while the weapon is in hand. Set it after jfg_weapon_register. jfg_hud_weapon_icon takes modded ids.
RECOMP_IMPORT("*", void jfg_weapon_set_icon(int id, int sprite));
// A modded weapon's model in the hero's hand: a pack's model (its number from jfg_asset_number, "mod:model_gun"), static
// (no joints), drawn on the hand where the stock guns are, at their size and axes; -1 = none of its own (an empty
// hand, a grenade for Lupus). Since version 6.
RECOMP_IMPORT("*", void jfg_weapon_set_held_model(int id, int model));
// A modded weapon's big icon: w x h colours 0xRRGGBBAA (at most 512 x 512; it is copied). Where mods draw the weapon's
// icon (jfg_hud_weapon_icon: Ammo Ring, ...) it is drawn instead of the game's sprite, sharp at the window's resolution,
// about 44 units wide at scale 1 (the game's are about 32).
RECOMP_IMPORT("*", void jfg_weapon_set_icon_image(int id, int w, int h, const unsigned int* rgba));
// How many weapon ids there are: 0 .. count - 1 are all weapons.
RECOMP_IMPORT("*", int jfg_weapon_count(void));
typedef struct {
    char name[48];    // as jfg_weapon_find takes it
    char display[40]; // in the language shown for stock weapons
    int modded;       // 1 for a mod's weapon (16 up)
    int host;         // stock weapons: their own number
    int thrown;       // -1: the game's own rule (stock weapons)
    int max_ammo;
} JfgWeaponInfo;
// 1 and out filled, 0 for no such weapon.
RECOMP_IMPORT("*", int jfg_weapon_info(int id, JfgWeaponInfo* out)); // display: a weapon's name to show (any id)
// The first player's hero: owns it?; give it (ammo -1 = its start ammo; owned already: added up to its capacity);
// take it away (not the pistol; in hand: the hero holds the next weapon).
RECOMP_IMPORT("*", int jfg_hero_has_weapon(int id));
RECOMP_IMPORT("*", void jfg_hero_give_weapon(int id, int ammo));
RECOMP_IMPORT("*", void jfg_hero_take_weapon(int id));
// A weapon pickup (an object of kind JFG_KIND_WEAPON) gives that weapon from now on (its model stays); and the weapon
// it gives. Pickups are the level's: point them again after each level change.
RECOMP_IMPORT("*", void jfg_weapon_pickup_set(void* pickup, int id));
RECOMP_IMPORT("*", int jfg_weapon_pickup_get(void* pickup));
// Credits what your weapon hurts: if the victim (a squad member) dies within ticks (1/60 s, at most 600), jfg_on_kill
// reports that weapon (your weapon's id) and shooter (when the game knows no hero who did it), so kill feeds and stats
// show your weapon. Call it as you hurt it (setting its health, a blast, an explosion you set off); again to renew.
RECOMP_IMPORT("*", void jfg_weapon_credit(void* victim, int weapon, void* shooter, int ticks));

// --- objects and effects for mods' weapons (since version 6) ---------------------------------------------------------
// An object of any type (a pack's own included) at x, y, z, as a shot is made (gone with the level). With a physics
// block in its definition it flies at vx, vy, vz (units a tick) and falls, bounces and comes to rest by the game's own
// physics; yaw 0..0xFFFF. The object, or NULL.
typedef struct {
    int object;
    float x, y, z;
    float vx, vy, vz;
    int yaw;
} JfgSpawn;
RECOMP_IMPORT("*", void* jfg_spawn(const JfgSpawn* spawn));
// The number a pack's appended entry or object type got this start ("mod_id:append name", as its jfg_assets.json
// names it), -1 if no loaded mod adds it. An object type's number is the object id to give jfg_spawn.
RECOMP_IMPORT("*", int jfg_asset_number(const char* name));
// Removes an object, as its own code does when it is done.
RECOMP_IMPORT("*", void jfg_object_free(void* object));
// An object's size (1.0 = as its definition makes it), and setting it.
RECOMP_IMPORT("*", float jfg_object_scale(void* object));
RECOMP_IMPORT("*", void jfg_object_set_scale(void* object, float scale));
// Its physics flags: 1 in water, 2 at rest (landed), 4 riding a moving object; -1 when it has no physics.
RECOMP_IMPORT("*", int jfg_object_physics(void* object));
// One of the game's explosions (its look, sound, blast and damage; type 0 the grenade's, 4 under water), credited to
// owner (a hero, or NULL).
typedef struct {
    int type;
    float x, y, z;
    void* owner;
} JfgExplosion;
RECOMP_IMPORT("*", void jfg_explosion(const JfgExplosion* explosion));
// The first player's view jolts up and down by up to magnitude level units, fading out over ms milliseconds (the
// game's strong hits: 15-25 over 2-4 s; 5-8 a light thump). A new call replaces the running shake unless that one is
// still stronger as it fades (a light thump doesn't cut a big shake short), 0 stops it, a level change ends it. It shakes whichever camera is on: the game's, the port's dual-stick and aiming views, and a mod's own
// (jfg_camera_set). Since version 6.
RECOMP_IMPORT("*", void jfg_camera_shake(int magnitude, int ms));
// Damage only, no look: strongest at the middle, less out to the radius, fading over the seconds; damage 256 a unit
// of health. At most 16 at a time (the oldest gives way).
typedef struct {
    float x, y, z;
    float radius;
    float damage;
    float seconds;
    void* owner;
} JfgBlast;
RECOMP_IMPORT("*", void jfg_blast(const JfgBlast* blast));
// The game's flat ring effect: its inner and outer radius grow by their speeds each tick (negative: it shrinks
// inward), it holds for hold ticks, then fades over fade ticks. Radii and speeds within -100..100.
typedef struct {
    float x, y, z;
    float inner, outer;
    float inner_speed, outer_speed;
    int hold, fade;
} JfgShockwave;
RECOMP_IMPORT("*", void jfg_shockwave(const JfgShockwave* wave));
// What a squad member is like, for weighing it: full_health (256 a unit, what its squadron gives each member) and
// health now, radius (its hit sphere), scale, height above the floor under it (flyers are high up) and that floor.
// 1, or 0 if it is not a squad member.
typedef struct {
    int full_health;
    int health;
    float radius;
    float scale;
    float height;
    float floor;
} JfgEnemyTraits;
RECOMP_IMPORT("*", int jfg_enemy_traits(void* enemy, JfgEnemyTraits* out));
// Knockback for enemies (the hero's is jfg_hero_push). Flings a squad member the game's own way: its velocity (level
// units a squad call: squads move once per 1/30 s, so twice a 1/60 s speed), track collision, and its AI off for
// `calls` calls or until it lands; walls stop it (a fast stop is a slam a mod can look for). 1 if flung, 0 if the
// game can't move it so (no collision data, held still, riding): the mod moves it itself if it wants.
typedef struct {
    float vx, vy, vz;
    int calls;           // walkers about 12, flyers about 14
} JfgFling;
RECOMP_IMPORT("*", int jfg_enemy_fling(void* enemy, const JfgFling* f));
// 1 while a squad member is flung (by jfg_enemy_fling, another mod or the game). A mod that drives its own enemy's
// velocity leaves it alone meanwhile, so the throw isn't undone.
RECOMP_IMPORT("*", int jfg_enemy_flung(void* enemy));
// Ammo for a mod's weapon, run by the port (the same for every weapon; call again whenever your options may have
// changed, each tick is fine; zeros turn a source off). Ammo is given as jfg_hero_give_weapon gives it: the weapon too
// if the hero hasn't it, never past its capacity (jfg_hero_give_weapon(id, n) is also the way to add n yourself).
// - Drops: cell (the pack's object type, "mod:objtype_x"; NULL for none) may drop where an enemy dies, drop_chance
//   percent a kill, from the shared drop pool (jfg_drop_register: one drop a kill across mods, twice the chance from a
//   big enemy). It appears cell_height above where the enemy died, at cell_scale, and hovers (cell_flags
//   JFG_CELL_FALLS: it falls and bounces by the game's physics); it spins, and walking into it gives cell_ammo with
//   the game's pick-up sound. It blinks out after 30 seconds.
// - Refill: picking up refill_from's ammo or the weapon itself (a stock weapon, 0..14; -1 none) gives refill_ammo.
// - Pickups: pickup_share percent of the level's pickups of pickup_from (stock, -1 none) give your weapon instead
//   (their model stays); with several weapons sharing one stock weapon's pickups, a pickup becomes at most one.
// - Recharge: one comes back every recharge_ticks (1/60 s; 0 none) while the hero has fewer than its capacity.
#define JFG_CELL_FALLS 1
typedef struct {
    const char* cell;
    int drop_chance;
    int cell_ammo;
    float cell_scale;
    float cell_height;
    int cell_flags;
    int refill_from;
    int refill_ammo;
    int pickup_from;
    int pickup_share;
    int recharge_ticks;
} JfgAmmoSources;
RECOMP_IMPORT("*", void jfg_weapon_ammo_sources(int weapon, const JfgAmmoSources* sources));
// Item drops from enemies, shared by all mods: register a drop with its chance per kill (0..100) instead of rolling on
// jfg_on_kill, and spawn your item in jfg_on_drop when its id is yours. The port rolls once per kill, so a kill drops
// at most one mod's item: only enemies the heroes or Floyd killed (no Tribals); JFG_DROP_BIG doubles the chance from a
// big enemy (hit sphere radius 30 or more, or 10 health units); squads made bigger by jfg_squad_multiplier drop as
// often in all as at their normal size; when the chances add up to more than 100% they are scaled down to share it.
// Registering the same name again changes its chance and flags and returns the same id: call it whenever your option
// may have changed. -1 if the name is empty or 32 drops are registered.
#define JFG_DROP_BIG 1
RECOMP_IMPORT("*", int jfg_drop_register(const char* name, int chance, int flags));
// Every registered drop's chance times percent / 100 (0..300; 100 normal). Each mod's request is kept: above 100 the
// largest applies, else the smallest. Since version 6.
RECOMP_IMPORT("*", void jfg_drop_rate(int percent));
// Mods' drops (weapons' ammo cells) within reach units of the hero drift to it and are picked up; 0 none. The largest
// request applies. Since version 6.
RECOMP_IMPORT("*", void jfg_drop_magnet(float reach));
// More enemies: squads set up from now on (the next level loaded) have percent / 100 times their members, 100 to 300
// (rounded per squad). flags 1: sentries too (they stand in a clump at their post). Tribals, bosses and big enemies keep
// their count. The extra members save no kills: killed squads stay killed at any setting, and a level never runs out
// of kill flags. They are made for the stock members still alive, and not when the level's object list is nearly full.
// Flying squads (drones, flying insects: their members don't land) take flyer_percent instead; extra flyers spread
// along their formation, which can put them where the hero can't reach.
RECOMP_IMPORT("*", void jfg_squad_multiplier(int percent, int flags, int flyer_percent));
// Where a point of the level (x, y, z) shows on screen this frame, in the overlay's units (jfg_overlay_size): out[0]
// x, out[1] y, out[2] its distance in front of the camera, out[3] overlay units per level unit there (to size a
// picture drawn at it). 1 if it is in front of the camera, else 0. Overlay pictures cover the game: to hide one behind
// a wall, test the line from the camera (jfg_camera_get) with jfg_level_ray. Picture ids (jfg_overlay_image) are
// each mod's own: any numbers, they never clash with another mod's.
RECOMP_IMPORT("*", int jfg_world_to_screen(const float* world, float* out));

// --- event data ---------------------------------------------------------------------------------------------------

// Who bleeds (JfgBloodSpurt.group, JfgBloodHit.group; since version 5).
#define JFG_BLOOD_HERO   0 // a hero or Floyd
#define JFG_BLOOD_TRIBAL 1
#define JFG_BLOOD_GREEN  2 // creatures with the game's green blood (ground insects, most bosses)
#define JFG_BLOOD_DARK   3 // with its black blood (flyers, drones, turrets)
#define JFG_BLOOD_BOSS   4 // the final boss
#define JFG_BLOOD_OTHER  5

// jfg_on_blood_spurt: a spurt of blood drops. Change any field; set cancel to 1 to drop it; amount is the drops in
// percent (100 = the game's own, 200 = twice as many; the extra ones spread a little). brightness and color were
// called alpha and count before version 5 (the names still work). Since version 5: source, the creature bleeding (the
// nearest to the blood with that colour; NULL if none), and its group (JFG_BLOOD_*), both only to read.
typedef struct {
    float x, y, z;
    float vx, vy, vz;
    union { int brightness; int alpha; };  // 0..255
    union { int color; int count; };       // colour index (jfg_blood_set_color)
    float scale;
    int amount;
    int cancel;
    void* source;
    int group;
} JfgBloodSpurt;

// jfg_on_blood_hit: a blood hit sprite. Set cancel to 1 to drop it. Since version 5: its brightness and colour index
// (change them as in JfgBloodSpurt), source and group.
typedef struct {
    float x, y, z;
    int cancel;
    int brightness;
    int color;
    void* source;
    int group;
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

// jfg_on_draw_hud: the frame's drawing is nearly done (the scene, the HUD and the hint text are in): a callback may
// write 2D commands at *gfx (advancing it), such as texture rectangles over the HUD. width and height: the screen in
// pixels (320 x 240, or 448 x 336 in the game's hi-res mode). Set the RDP's state you need yourself.
typedef struct {
    void** gfx;
    int width, height;
} JfgHudGfx;

// jfg_on_reticle_draw: the aiming reticle is about to be drawn at x, y (screen pixels), depth 0 near .. 1 at the aim's
// reach, scale its size, locked on an enemy or not, by player (0 the first), for weapon. Set cancel to 1 to draw none;
// to draw your own, write 2D commands at *gfx (the HUD's display list, advancing it; set the RDP state you need and
// put the cycle type back to 1-cycle after). width, height: the screen in pixels (since version 5).
typedef struct {
    int x, y;
    float depth;
    float scale;
    int locked;
    int player;
    int weapon;
    int cancel;
    void** gfx;
    int width, height;
} JfgReticle;

// jfg_on_health_draw: the hero's health meter is about to be drawn. health in units (0 .. max; jfg_object_health is
// 256 per unit), segments: the meter's segments (6, one more per health upgrade, up to 12), max = 5 * segments + 4 (the
// meter fills its segments five times over: health / (segments + 1) full rounds, then health % (segments + 1)
// segments of the next). players: 1 in the single-player game. frames: video frames the tick covers. Set cancel to 1
// to draw none (and draw your own, e.g. with the overlay).
typedef struct {
    void* hero;
    int health;
    int segments;
    int max;
    int character;
    int players;
    int frames;
    int cancel;
} JfgHealth;

// jfg_on_weapon_hud: the weapon panel is about to be drawn: the weapon in hand (0..15), its ammo and max ammo, charge
// (the charge bar: 0..1, the pistol's heat or a charging weapon's charge; -1 for a weapon without one), owned (bit w:
// weapon w), character (0 Vela, 1 Juno, 2 Lupus), frames. Set cancel to 1 to draw none of it (the weapon frame, the
// pick-up message panel, the Tribal panel, the weapon selector) and draw your own, placing the weapon icons with
// jfg_hud_weapon_icon. The pick-up messages still come as jfg_on_hud_message, the counts from jfg_tribal_counts.
typedef struct {
    int weapon;
    int ammo;
    int max_ammo;
    float charge;
    unsigned int owned;
    int character;
    int frames;
    int cancel;
} JfgWeaponHud;

// jfg_on_kill: a squad member (an enemy, or a Tribal) died. victim: its object; killer: whoever fired the shot (a hero,
// Floyd, or another object; NULL when unknown) and killer_kind (-1 unknown, 0 Vela, 1 Juno, 2 Lupus, 3 Floyd, 4 another
// object); weapon: what the game counts the kill under (0..15, the weapons' order), or a modded weapon's id (16 up)
// when its mod credited the kill (jfg_weapon_credit; its name and icon: jfg_weapon_info, jfg_hud_weapon_icon); tribal:
// 1 when the victim was a Tribal. victim_name and killer_name are the objects' definition names from the player's game (up to 15 characters,
// "" when there is none). Read only; frames is 0.
typedef struct {
    void* victim;
    void* killer;
    int killer_kind;
    int weapon;
    int tribal;
    int frames;
    char victim_name[16];
    char killer_name[16];
} JfgKill;

// jfg_on_weapon_fire (since version 6): the first player's hero fires: weapon (any id, modded ones included),
// character (0 Vela, 1 Juno, 2 Lupus), hero (its object), charge (the plasma shotgun's charge as the game keeps it).
// Set cancel to 1 to fire nothing of the game's (fire your own); then take_ammo (1) says whether one ammo is used.
typedef struct {
    int weapon;
    int character;
    void* hero;
    int charge;
    int cancel;
    int take_ammo;
} JfgWeaponFire;

// --- new squad enemy types (since version 6) -----------------------------------------------------------------------
// A pack appends an object type the squad code treats like a stock enemy ("objtypes" with "like" in jfg_assets.json,
// its definition a copy of that enemy's with the pack's model) and the mod registers it by the definition's name.
// Squadrons placed with that object id (Studio, or a level's data) then spawn members the mod runs in
// jfg_on_squad_tick, in place of the like type's own fire routine.
// The type gets its own copy of the like type's baddy row: no body parts (the like type's would cut the new model's
// batches off on a killing hit) and the joints below, which are the new model's. Its hurt and death voices are silent
// (the like type's would play); the mod plays its own.
typedef struct {
    const char* name;      // "mod_id:type"
    const char* def_name;  // the pack's appended definition, "mod_id:objdef_x"
    int flags;             // JFG_SQUAD_KEEP_VOICES, JFG_SQUAD_KEEP_BODY_PARTS: keep the like type's
    // The model's joints (-1 keeps the like type's number, which may name another bone on a new model):
    int head;              // the head: the eye point (aim, sight) is head_offset in its frame
    int head_offset[3];    // -128..127
    int lean[3];           // up to three joints that bend into turns (lean_weight 0..255 each); 0 = none
    int lean_weight[3];
    int aim;               // the joint that turns toward the hero; 0 = none
} JfgSquadTypeDef;
#define JFG_SQUAD_KEEP_VOICES 1
#define JFG_SQUAD_KEEP_BODY_PARTS 2
// Its type number (the same again for a name registered before), -1 if the definition is bad.
RECOMP_IMPORT("*", int jfg_squad_type_register(const JfgSquadTypeDef* def));

// jfg_on_drop: a kill's shared drop came up (jfg_drop_register). id: the drop's (spawn your item only when it is
// yours); victim, killer and killer_kind as in jfg_on_kill; x, y, z where the victim died; big: 1 from a big enemy.
typedef struct {
    int id;
    void* victim;
    void* killer;
    int killer_kind;
    float x, y, z;
    int big;
} JfgDrop;

// jfg_on_squad_tick: a member of a registered type, at the point of its update where the game runs its type's own
// routine (before steering and the move). data is its squad control data (object +0x68), frames the update rate of
// this call (squad members move once per call). Set run_stock to 1 to run the like type's routine after yours. Only the
// mod that registered the type gets its members' ticks.
typedef struct {
    int type;
    void* member;
    void* data;
    int frames;
    int run_stock;
} JfgSquadTick;

// Only inside jfg_on_squad_tick, for that member. A mod holds a member with mission 7 under its own group (0x20 up):
// nothing in the game replaces it except burning, hit-stop and electrocution. 1 if set.
RECOMP_IMPORT("*", int jfg_squad_set_mission(void* member, int group, int mission, int a, int b, int c));
// Clears the member's mission when it is the group's; cooldown: ticks before it may fire again.
RECOMP_IMPORT("*", void jfg_squad_clear_mission(void* member, int group, int cooldown));
// Plays animation `anim` of the object's model list from phase / 1000 (0..1000). The mod advances the phase
// (object +0x28, 0..1; a loop wraps below 1.0, a one-shot holds at 1.0) itself. For a squad member, set its animation
// state (data +0x4B) to 7 first, which nothing in the game advances, and its set entry (data +0x19) to an entry of the
// like type's set whose flags it should run with (0, an idle: no firing, stands; 19 for the ground soldier's walk),
// never the anim number. Drive it only while data +0x4B is still 7: a hit reaction, burning or death replaces it.
// Handing back: jfg_squad_clear_mission, data +0x4B = 0 and data +0x0A = 0 (so the game picks its own move again).
RECOMP_IMPORT("*", void jfg_object_anim_set(void* object, int anim, int phase));
// Shell armour: shots hitting the member stop on its hit spheres with no damage: mode 1 all but those from behind,
// 2 all, 0 none. Blasts always hurt. jfg_squad_deflects: shots stopped since the last call. jfg_squad_forget: drop
// a member (dead or gone) from the armour list.
RECOMP_IMPORT("*", void jfg_squad_set_armour(void* member, int mode));
RECOMP_IMPORT("*", int jfg_squad_deflects(void* member));
RECOMP_IMPORT("*", void jfg_squad_forget(void* member));

// A mod's projectile hits the first player's hero as an enemy's shot does: the segment from `from` to `to` (its last
// and current position) with radius, against the hero's hit spheres; damage 256 a unit; weapon (0..15) for what the
// game records; source the projectile. The hero's protection and dodges count; the hero code reacts (flash, voice,
// death). 1 if it hit.
typedef struct {
    float from[3], to[3];
    float radius;
    int damage;
    int weapon;
    void* source;
} JfgHeroShot;
RECOMP_IMPORT("*", int jfg_hero_shot(const JfgHeroShot* shot));

// --- events (since version 1) --------------------------------------------------------------------------------------

// jfg_on_game_tick(int frames)
//     Once per game tick, before the game runs it. frames: video frames the tick covers (2 at 30 fps, 1 at 60 fps).
// jfg_on_level_change(int level, int character)
//     The game starts changing to `level`, with `character` playing.
// jfg_on_boss_fight_control(void* player, int frames, int character)
//     Each tick the player is controlled in a boss fight, before the game's control code runs. player: the player's
//     object; frames: video frames the tick covers; character: 0 Vela, 1 Juno, 2 Lupus.
// jfg_on_blood_spurt(JfgBloodSpurt* blood)
//     Blood drops are about to be added.
// jfg_on_blood_hit(JfgBloodHit* hit)
//     A blood hit sprite is about to be added.
// jfg_on_severed_limb_tick(void* limb, int frames)
//     Each tick a severed body part (an object) is updated, before the game's code for it.
// jfg_on_object_draw(JfgObjectDraw* draw)
//     An object is about to be drawn (after Hide Floyd, jfg_floyd_hide, has applied).
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
// jfg_on_draw_hud(JfgHudGfx* hud)
//     Each frame, after the HUD: draw over it.
// jfg_on_hud_message(int msg, int character, int player)
//     A message is queued for a player's HUD panel (text msg). Pick-ups: weapon w's ammo 0x48 + w (extra ammo, or a
//     dropped ammo part), 0x57 + w (a clip), 0x66 + w (full ammo), 0x75 + w (more capacity); keys 0x170 + key.

// --- events (since version 5) --------------------------------------------------------------------------------------

// jfg_on_reticle_draw(JfgReticle* reticle)
//     Each frame the aiming reticle is drawn, before the game draws it.
// jfg_on_weapon_hud(JfgWeaponHud* hud)
//     Each frame the weapon panel is drawn (single player, not in menus or cutscenes), before the game draws it.
// jfg_on_health_draw(JfgHealth* health)
//     Each frame the health meter is drawn (not in the pause menu, cutscenes or other times the HUD is hidden),
//     before the game draws it.
// jfg_on_kill(JfgKill* kill)
//     A squad member (an enemy or a Tribal) died, after the game counted it in the weapon stats.

// --- events (since version 6) --------------------------------------------------------------------------------------

// jfg_on_weapon_fire(JfgWeaponFire* fire)
//     The first player's hero is about to fire a shot (or release a throw), before the game makes it.
// jfg_on_squad_tick(JfgSquadTick* tick)
//     A member of a squad type a mod registered is updated (jfg_squad_type_register).
// jfg_on_drop(JfgDrop* drop)
//     A kill dropped one of the registered drops (jfg_drop_register), after jfg_on_kill.
// jfg_on_object_spawn(void* obj, int kind)
//     An object of a kind the mod watches (jfg_object_watch) was made: told at the start of the next game tick,
//     before jfg_on_game_tick.
// jfg_on_object_free(void* obj, int kind)
//     A watched object was freed: told at the start of the next tick, before spawns; don't read it any more (its
//     address may already be a new object's). One made and freed between two ticks is never told. At a level change
//     every object goes without these events.

#endif
