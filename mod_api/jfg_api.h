// Jet Force Gemini: Recompiled mod API. Include it in a mod (it needs the mod template's modding.h).
//
// Helpers are functions the game provides; events are moments a mod can react to, with
//     RECOMP_CALLBACK("*", jfg_on_game_tick) void my_tick(int frames) { ... }
// The game's own features use the same events and run first; mods' callbacks follow in mod-list order.
// Released helpers and events don't change; new ones come with a higher JFG_API_VERSION.
#ifndef JFG_API_H
#define JFG_API_H

#include "modding.h"

#define JFG_API_VERSION 5

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
// own health for a hero draws its own (jfg_on_draw_hud).
RECOMP_IMPORT("*", void jfg_hud_hide_health(int character, int hide));

// The heads the first player has collected as a hero (0 Vela, 1 Juno, 2 Lupus): tribal 1 the tribals', 0 the others'.
RECOMP_IMPORT("*", int jfg_hero_heads(int character, int tribal));

// Sound effects: the mod's own OGG or 16-bit PCM WAV files, named in its jfg_sounds.json
// ({ "sounds": { "<name>": "<file>.ogg" } }, the files among the mod's additional files). volume 1 = as recorded, pan
// -1 left to 1 right, pitch 1 = as recorded (2 an octave up); mixed at the game's sound effects volume. Play returns a
// voice (0: no such sound); a loop plays until stopped. jfg_sound_set changes a playing voice and returns 1 while it
// plays, 0 once it has ended.
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
// a hero of another size, say. 1 = as usual. Kept until set again.
RECOMP_IMPORT("*", void jfg_camera_set_distance(float scale));

// --- helpers (since version 5) -------------------------------------------------------------------------------------

// Blood colours. Blood takes its colour from an index (0..7, JfgBloodSpurt.color, JfgBloodHit.color) into two tables,
// one for drops and splats and one for hit sprites. The game uses 0 (red: heroes, Floyd, Tribals), 1 (green: ground
// insects, most bosses), 2 (black: flyers, drones, turrets) and 4 (lime: the final boss); 3, 5, 6 and 7 are free for
// a mod to fill and hand out. Sets index's colour, 0xRRGGBB each; the tables are drawn at different strengths (the
// game's red is 0x500000 for drops, 0xA00000 for hits). 0xFFFFFFFF = the game's own colour. Kept until set again.
RECOMP_IMPORT("*", void jfg_blood_set_color(int index, unsigned int drops, unsigned int hits));
// The game's rainbow blood (a cheat): each drop takes a random colour index (hit sprites keep theirs).
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
// cancel the game's in jfg_on_reticle_draw and draw yours there (JfgReticle.gfx).
RECOMP_IMPORT("*", void jfg_aim_lock(int on));

// Toggled buttons: while on, each press of those N64 buttons (libultra's, as in JfgInput) holds them down until the
// next press, for the first player (on any controls: pad, dual stick, mouse and keyboard; after Swap Triggers).
RECOMP_IMPORT("*", void jfg_input_toggle(unsigned int buttons, int on));
// Lets go of those toggled buttons now (a death, a level change).
RECOMP_IMPORT("*", void jfg_input_toggle_release(unsigned int buttons));
// The toggled buttons held now.
RECOMP_IMPORT("*", unsigned int jfg_input_toggled(void));

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
// The weapon in the first player's hand (0 the pistol .. 15), -1 if none. The shocker is 4, the flamethrower 6.
RECOMP_IMPORT("*", int jfg_hero_weapon(void));
// The first player's ammo for a weapon (0..15), -1 outside a level; and setting it.
RECOMP_IMPORT("*", int jfg_hero_ammo(int weapon));
RECOMP_IMPORT("*", void jfg_hero_set_ammo(int weapon, int ammo));
// Pushes the hero x, z level units per 1/60 s for ticks 1/60 s ticks (the game's knockback: walls stop it, and it
// eases off after). The hero walks a little over 2 units per 1/60 s.
RECOMP_IMPORT("*", void jfg_hero_push(void* hero, float x, float z, int ticks));
// No damage reaches the hero for at least ticks 1/60 s ticks (8 at the least), and enemies don't aim at it.
RECOMP_IMPORT("*", void jfg_hero_protect(void* hero, int ticks));

// Floyd's object while he is with the first player's hero, else NULL; state (may be NULL): 0 the game's AI flies him
// after the hero, 1 a player flies him, 2 a Floyd mission.
RECOMP_IMPORT("*", void* jfg_floyd_get(int* state));
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

#endif
