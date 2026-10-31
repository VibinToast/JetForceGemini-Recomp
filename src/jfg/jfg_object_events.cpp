// Jet Force Gemini port: objects coming and going, for mods (mod API jfg_object_watch, jfg_on_object_spawn,
// jfg_on_object_free). A mod that follows objects of a kind (plasma balls, grenades, squad members) watches that kind
// instead of scanning the object list each tick and guessing which ones went (a freed object's address is soon given
// to a new one).
//
// Each object objSetupObject makes of a watched kind is noted; after each objDoFrees, noted objects no longer in either
// object list are gone. Both are told at the start of the next game tick (before jfg_on_game_tick), frees first, and
// only to the mods watching that kind: an object made and freed in between is never told. At a level change every
// object goes without events.
#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

namespace {
constexpr uint32_t ADDR_NoAddObjList = 0x800F2CB4, ADDR_NoAddObjListCount = 0x800F2CB8;
constexpr int OBJECTS_MAX = 512, NO_ADD_MAX = 64;

struct Seen {
    uint32_t obj;
    int kind;
};
std::map<int, std::set<size_t>> g_watchers;  // kind -> the mods watching it
std::unordered_map<uint32_t, int> g_live;    // watched objects made and not yet freed -> their kind
std::vector<Seen> g_spawned, g_freed;        // to tell at the next tick

// jfg_object_watch(int kind, int on): the calling mod is told when objects of that kind are made and freed.
void object_watch(uint8_t*, recomp_context* ctx, size_t mod_index) {
    const int kind = (int32_t)ctx->r4;
    if (ctx->r5 != 0) {
        g_watchers[kind].insert(mod_index);
    } else if (auto it = g_watchers.find(kind); it != g_watchers.end()) {
        it->second.erase(mod_index);
        if (it->second.empty()) g_watchers.erase(it);
    }
}

bool active() { return !g_watchers.empty(); }

void tell(uint8_t* rdram_, recomp_context* ctx, api::Event event, const Seen& s) {
    const auto it = g_watchers.find(s.kind);
    if (it == g_watchers.end()) return;
    for (size_t mod : it->second) api::fire_for_mod(rdram_, ctx, event, mod, (int32_t)s.obj, s.kind);
}

void on_tick(uint8_t* rdram_, recomp_context* ctx) {
    const std::vector<Seen> freed = std::move(g_freed), spawned = std::move(g_spawned);
    g_freed.clear();
    g_spawned.clear();
    for (const Seen& s : freed) tell(rdram_, ctx, api::Event::ObjectFree, s);
    for (const Seen& s : spawned) {
        if (g_live.count(s.obj)) tell(rdram_, ctx, api::Event::ObjectSpawn, s);
    }
}

void on_level_change(uint8_t*, recomp_context*) {
    g_live.clear();
    g_spawned.clear();
    g_freed.clear();
}
} // namespace

// From objSetupObject's wrapper (jfg_diagnostics.cpp): an object it made.
void object_events_made(uint8_t* rdram_, uint32_t obj) {
    if (g_watchers.empty() || !valid_ptr(obj)) return;
    g_rdram = rdram_;
    const int kind = Obj::Kind::get(obj);
    if (!g_watchers.count(kind)) return;
    g_live[obj] = kind;
    g_spawned.push_back({ obj, kind });
}

// From objDoFrees' wrapper (jfg_frame_rate.cpp): the noted objects no longer listed are gone.
void object_events_after_frees(uint8_t* rdram_) {
    if (g_live.empty()) return;
    g_rdram = rdram_;
    std::unordered_map<uint32_t, int> alive;
    const uint32_t lists[2][3] = { { ADDR_ObjList, ADDR_ObjListCount, OBJECTS_MAX },
                                   { ADDR_NoAddObjList, ADDR_NoAddObjListCount, NO_ADD_MAX } };
    for (const auto& l : lists) {
        const uint32_t list = rd32(l[0]);
        const int count = std::min((int)(int32_t)rd32(l[1]), (int)l[2]);
        if (!valid_ptr(list)) continue;
        for (int i = 0; i < count; i++) {
            const auto it = g_live.find(rd32(list + 4u * (uint32_t)i));
            if (it != g_live.end()) alive.insert(*it);
        }
    }
    for (const auto& entry : g_live) {
        const uint32_t obj = entry.first;
        const int kind = entry.second;
        if (alive.count(obj)) continue;
        const auto made = std::find_if(g_spawned.begin(), g_spawned.end(), [&](const Seen& s) { return s.obj == obj; });
        if (made != g_spawned.end()) g_spawned.erase(made); // made and freed before anyone was told: neither is told
        else g_freed.push_back({ obj, kind });
    }
    g_live.swap(alive);
}

void register_object_events_api() {
    recomp::overlays::register_ext_base_export("jfg_object_watch", object_watch);
    api::add_internal(api::Event::GameTick, on_tick, active);
    api::add_internal(api::Event::LevelChange, on_level_change);
}

} // namespace jfg
