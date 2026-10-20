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
    GameTick,     // jfg_on_game_tick(int frames)
    LevelChange,  // jfg_on_level_change(int level, int character)
    Count
};

using Handler = void (*)(uint8_t* rdram, recomp_context* ctx);

void add_internal(Event event, Handler handler);
// Runs the event's internal handlers, then mods' callbacks. ctx is left as it was.
void fire(uint8_t* rdram, recomp_context* ctx, Event event, int32_t arg0 = 0, int32_t arg1 = 0);
// Registers the events and helpers with the runtime; once, before mods load (jfg_register_mod_exports).
void register_api();

} // namespace jfg::api
