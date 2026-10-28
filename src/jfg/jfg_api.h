// Jet Force Gemini port: the JFG mod API, the port's side (mods include mod_api/jfg_api.h).
//
// Events fire from the port's wrappers and hooks. The port's own features are "internal mods": they subscribe with
// add_internal and run first, then mods' RECOMP_CALLBACK functions in mod-list order. A handler gets the event's
// arguments in ctx->r4.. as a mod does.
#pragma once

#include <cstddef>
#include <cstdint>

#include "recomp.h"

namespace jfg::api {

constexpr int VERSION = 6;

// The events, in the order they are registered: a released event keeps its place, new ones go before Count.
enum class Event : uint32_t {
    GameTick,         // jfg_on_game_tick(int frames)
    LevelChange,      // jfg_on_level_change(int level, int character)
    BossFightControl, // jfg_on_boss_fight_control(Object* player, int frames, int character)
    BloodSpurt,       // jfg_on_blood_spurt(JfgBloodSpurt* blood)
    BloodHit,         // jfg_on_blood_hit(JfgBloodHit* hit)
    SeveredLimbTick,  // jfg_on_severed_limb_tick(Object* limb, int frames)
    ObjectDraw,       // jfg_on_object_draw(JfgObjectDraw* draw)
    SoundPlay,        // jfg_on_sound_play(JfgSoundPlay* sound)
    ScreenFlash,      // jfg_on_screen_flash(JfgScreenFlash* flash)
    SidekickTick,     // jfg_on_sidekick_tick(Object* floyd, int frames)
    LanguageChange,   // jfg_on_language_change(void), since version 2
    MapOpen,          // jfg_on_map_open(int galaxy), since version 3
    GalaxyChange,     // jfg_on_galaxy_change(int galaxy), since version 3
    PlanetSelect,     // jfg_on_planet_select(int planet), since version 3
    HeroControl,      // jfg_on_hero_control(Object* hero, int frames, int character), since version 4
    ObjectDrawGfx,    // jfg_on_object_draw_gfx(JfgObjectGfx* draw), since version 4
    DrawHud,          // jfg_on_draw_hud(JfgHudGfx* hud), since version 4
    HudMessage,       // jfg_on_hud_message(int msg, int character, int player), since version 4
    ReticleDraw,      // jfg_on_reticle_draw(JfgReticle* reticle), since version 5
    HealthDraw,       // jfg_on_health_draw(JfgHealth* health), since version 5
    WeaponHud,        // jfg_on_weapon_hud(JfgWeaponHud* hud), since version 5
    Kill,             // jfg_on_kill(JfgKill* kill), since version 5
    WeaponFire,       // jfg_on_weapon_fire(JfgWeaponFire* fire), since version 6
    Count
};

using Handler = void (*)(uint8_t* rdram, recomp_context* ctx);
// Whether an internal handler has anything to do right now (its option is on). Lets fire() skip an event entirely.
using Active = bool (*)();

// Events whose callbacks may change values or cancel get a struct in game memory (layout in mod_api/jfg_api.h), just
// below the game's stack pointer; the event's a0 points at it. While a Payload lives, ctx's $sp is below the struct,
// so callbacks' frames don't overwrite it.
struct Payload {
    Payload(recomp_context* ctx, uint32_t size);
    ~Payload();
    recomp_context* ctx;
    gpr saved_sp;
    uint32_t addr;
};

// JfgBloodSpurt and JfgBloodHit (mod_api/jfg_api.h).
namespace blood_spurt {
constexpr uint32_t X = 0x00, Y = 0x04, Z = 0x08, VX = 0x0C, VY = 0x10, VZ = 0x14, BRIGHTNESS = 0x18, COLOR = 0x1C, SCALE = 0x20,
                   AMOUNT = 0x24, CANCEL = 0x28, SOURCE = 0x2C, GROUP = 0x30, SIZE = 0x34; // SOURCE, GROUP since version 5
}
namespace blood_hit {
constexpr uint32_t X = 0x00, Y = 0x04, Z = 0x08, CANCEL = 0x0C, BRIGHTNESS = 0x10, COLOR = 0x14, SOURCE = 0x18, GROUP = 0x1C,
                   SIZE = 0x20; // BRIGHTNESS.. since version 5
}
// JfgObjectDraw, JfgSoundPlay, JfgScreenFlash: one value and cancel.
namespace value_cancel {
constexpr uint32_t VALUE = 0x00, CANCEL = 0x04, SIZE = 0x08;
}
// JfgObjectGfx: the object, where its draw writes (the game's Gfx**, Mtx**, Vertex** cursors) and cancel.
namespace object_gfx {
constexpr uint32_t OBJECT = 0x00, GFX = 0x04, MTX = 0x08, VTX = 0x0C, CANCEL = 0x10, SIZE = 0x14;
}
// JfgHudGfx: where the frame's display list goes on (Gfx**), and the screen's size in pixels.
namespace hud_gfx {
constexpr uint32_t GFX = 0x00, WIDTH = 0x04, HEIGHT = 0x08, SIZE = 0x0C;
}

// The hero API (jfg_hero_api.cpp): helpers, and whether a mod controls a hero (boyControl's wrapper asks).
void register_hero_api();
bool hero_taken_over(int character);
// Taken over and not handed back to the game's code (jfg_hero_game_control): the mod moves it this tick.
bool hero_mod_controlled(int character);
extern float g_camera_distance; // jfg_camera_set_distance: the dual-stick camera's distance, times the option
void queue_weapon_step(int direction); // the mouse wheel, for a hero a mod moves: done in its next control tick
// jfg_controls_get (jfg_controls.cpp): the player's controls, for a mod hero.
void controls_get(uint8_t* rdram, recomp_context* ctx);
// A mod shows its own health for this hero (jfg_hud_hide_health): instDrawHealth's wrapper leaves the meter out.
bool hero_health_hidden(int character);
// A mod sets the camera (jfg_camera_set) these ticks: the port's own camera features stand down; its FOV (0 = none).
bool mod_camera_active();
float mod_camera_fov();
void mod_camera_tick(); // each game tick: a mod's camera lapses once it stops setting it, heroes or not

// active: nullptr = always runs.
void add_internal(Event event, Handler handler, Active active = nullptr);
// True when firing the event would run anything: an active internal handler or a mod's callback. Events with a Payload
// check it first, so an event nobody listens to costs no game-memory writes either.
bool listened(Event event);
// Runs the event's active internal handlers, then mods' callbacks. ctx is left as it was. Returns at once when
// nothing listens.
void fire(uint8_t* rdram, recomp_context* ctx, Event event, int32_t arg0 = 0, int32_t arg1 = 0, int32_t arg2 = 0,
          int32_t arg3 = 0);
// Registers the events and helpers with the runtime; once, before mods load (jfg_register_mod_exports).
void register_api();

// For the F9 window's Mod API tab: an event's name, how often it fired and how often something listened, since start
// (read on the UI thread), and the number of mods' callbacks.
struct Stats {
    uint64_t fired;
    uint64_t ran;
};
const char* event_name(Event event);
Stats stats(Event event);
size_t mod_callbacks(Event event);
// JFG_DEBUG_STATS: one line with each event fired since the last call, as fired/listened.
void log_stats();

} // namespace jfg::api
