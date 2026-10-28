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
// own health for a hero draws its own (jfg_on_draw_hud).
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
// charge cut off by its shot). The first enabled mod to replace an id has it. jfg_sound_replace_enable turns one
// replacement off (the game's own sound back) and on again while the game runs; 1 if the id is replaced.
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
// The weapon in the first player's hand (0 the pistol .. 15), -1 if none. The shocker is 4, the flamethrower 6.
RECOMP_IMPORT("*", int jfg_hero_weapon(void));
// The first player's ammo for a weapon (0..15), -1 outside a level; and setting it.
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
// The hero's own voice (Juno's, Vela's or Lupus's, as the game picks them): 0 the jump, 1 the landing, 3 being hit.
#define JFG_SPEAK_JUMP 0
#define JFG_SPEAK_LAND 1
#define JFG_SPEAK_HIT  3
RECOMP_IMPORT("*", void jfg_hero_speak(void* hero, int type));
// The object drawn tinted rgba's colour (0xRRGGBBAA), the alpha its strength (0 removes it), as the game flashes the
// enemy it targets; kept until set again.
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
// it is in the middle 4:3 area: its left edge is then at x (overlay width - 320) / 2 in overlay units, else at 0.
RECOMP_IMPORT("*", int jfg_hud_in_corners(void));
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
// One of the game's texts (front_text[id], in the language shown) into out (size bytes, ended by 0); its length, 0 if
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
// object); weapon: what the game counts the kill under (0..15, the weapons' order); tribal: 1 when the victim was a
// Tribal. victim_name and killer_name are the objects' definition names from the player's game (up to 15 characters,
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

#endif
