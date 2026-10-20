// Jet Force Gemini port: the JFG mod API, the port's side (mods include mod_api/jfg_api.h).
//
// Events fire from the port's wrappers and hooks. The port's own features are "internal mods": they subscribe with
// add_internal and run first, then mods' RECOMP_CALLBACK functions in mod-list order. A handler gets the event's
// arguments in ctx->r4.. as a mod does.
#pragma once

#include <cstdint>

#include "recomp.h"

namespace jfg::api {

constexpr int VERSION = 1;

// The events, in the order they are registered: a released event keeps its place, new ones go before Count.
enum class Event : uint32_t {
    GameTick,         // jfg_on_game_tick(int frames)
    LevelChange,      // jfg_on_level_change(int level, int character)
    BossFightControl, // jfg_on_boss_fight_control(Object* player, int frames, int character)
    BloodSpurt,       // jfg_on_blood_spurt(JfgBloodSpurt* blood)
    BloodHit,         // jfg_on_blood_hit(JfgBloodHit* hit)
    SeveredLimbTick,  // jfg_on_severed_limb_tick(Object* limb, int frames)
    Count
};

using Handler = void (*)(uint8_t* rdram, recomp_context* ctx);

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
constexpr uint32_t X = 0x00, Y = 0x04, Z = 0x08, VX = 0x0C, VY = 0x10, VZ = 0x14, ALPHA = 0x18, COUNT = 0x1C, SCALE = 0x20,
                   AMOUNT = 0x24, CANCEL = 0x28, SIZE = 0x2C;
}
namespace blood_hit {
constexpr uint32_t X = 0x00, Y = 0x04, Z = 0x08, CANCEL = 0x0C, SIZE = 0x10;
}

void add_internal(Event event, Handler handler);
// Runs the event's internal handlers, then mods' callbacks. ctx is left as it was.
void fire(uint8_t* rdram, recomp_context* ctx, Event event, int32_t arg0 = 0, int32_t arg1 = 0, int32_t arg2 = 0,
          int32_t arg3 = 0);
// Registers the events and helpers with the runtime; once, before mods load (jfg_register_mod_exports).
void register_api();

} // namespace jfg::api
