// Jet Force Gemini port: short-lived effects for mods (mod API jfg_fx_spawn, jfg_object_set_opacity). A dust puff, a
// spark, an impact flash or a shock wave is one of a pack's object types, spawned, then grown, moved, faded and
// freed over its life; the port runs that life for every mod the same way, so a mod spawns it and forgets it.
// - Moved by its speed each tick (no collision), the speed eased off by `keep` a tick.
// - Scaled from scale_from to scale_to over its life, evenly or easing out (fast at first).
// - Its opacity (the fading kinds only: behaviour 0x7E, whose draw copies object +0x8C, decomp MSR 11; they start at
//   0, not drawn) set at the start and, past fade_from of its life, faded to nothing.
// - Freed at the end of its life; one the game frees first is left alone (claimed, jfg_object_claims.cpp). A level
//   change ends them all.
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

uint32_t spawn_object(uint8_t* rdram, recomp_context* ctx, int id, float x, float y, float z, float vx, float vy,
                      float vz, uint16_t yaw);                              // jfg_hero_api.cpp
void free_object(uint8_t* rdram, recomp_context* ctx, uint32_t obj);       // jfg_hero_api.cpp
void set_object_scale(uint32_t obj, float scale);                          // jfg_hero_api.cpp
bool game_is_paused(uint8_t* rdram, recomp_context* ctx);                  // jfg_hero_api.cpp
void claim_for_mod(uint32_t obj, size_t mod_index);                        // jfg_object_claims.cpp
bool claim_alive(uint32_t obj);
void unclaim(uint32_t obj);
int asset_number(const std::string& ref);                                  // jfg_assets.cpp

namespace {
constexpr int16_t KIND_FADING = 0x7E;
constexpr uint32_t OBJ_OPACITY = 0x8C;
constexpr size_t MAX_FX = 64;
constexpr size_t PORT_CLAIM = SIZE_MAX - 1;

struct Fx {
    uint32_t obj;
    int age, ticks;
    float vx, vy, vz, keep;
    float scale_from, scale_to;
    bool ease_out;
    int opacity;
    float fade_from;
};
std::vector<Fx> g_fx;
std::map<std::string, int> g_types; // a pack's type name -> its object id this start

bool fading_kind(uint32_t obj) { return valid_ptr(obj) && Obj::Kind::get(obj) == KIND_FADING; }

std::string read_name(uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 64 && addr >= 0x80000400u && addr < 0xA0000000u; i++) {
        const char c = (char)rd8(addr + i);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// jfg_fx_spawn(const JfgFx* fx) -> the effect's object, or NULL (no such type, or too many effects).
void fx_spawn(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t f = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (f < 0x80000400u || f >= 0xA0000000u || g_fx.size() >= MAX_FX) return;
    const std::string name = read_name(rd32(f + 0x00));
    if (name.empty()) return;
    auto t = g_types.find(name);
    if (t == g_types.end()) t = g_types.emplace(name, asset_number(name)).first;
    if (t->second < 0) return;
    Fx fx;
    fx.ticks = std::max(1, (int)(int32_t)rd32(f + 0x24));
    fx.vx = rdf(f + 0x14); fx.vy = rdf(f + 0x18); fx.vz = rdf(f + 0x1C);
    fx.keep = rdf(f + 0x20) > 0.0f ? std::min(rdf(f + 0x20), 1.0f) : 1.0f;
    fx.scale_from = rdf(f + 0x28);
    fx.scale_to = rdf(f + 0x2C);
    fx.ease_out = rd32(f + 0x30) != 0;
    fx.opacity = std::clamp((int)(int32_t)rd32(f + 0x34), 0, 255);
    fx.fade_from = rdf(f + 0x38);
    fx.age = 0;
    fx.obj = spawn_object(rdram_, ctx, t->second, rdf(f + 0x04), rdf(f + 0x08), rdf(f + 0x0C), 0.0f, 0.0f, 0.0f,
                          (uint16_t)rd32(f + 0x10));
    if (!valid_ptr(fx.obj)) return;
    set_object_scale(fx.obj, fx.scale_from);
    if (fx.opacity > 0 && fading_kind(fx.obj)) wr32(fx.obj + OBJ_OPACITY, (uint32_t)fx.opacity);
    claim_for_mod(fx.obj, PORT_CLAIM);
    g_fx.push_back(fx);
    ctx->r2 = (gpr)(int32_t)fx.obj;
}

// jfg_object_set_opacity(void* obj, int opacity): 0..255, for the fading kinds (behaviour 0x7E); others are left.
void object_set_opacity(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    if (fading_kind(obj)) wr32(obj + OBJ_OPACITY, (uint32_t)std::clamp((int)(int32_t)ctx->r5, 0, 255));
}

bool active() { return !g_fx.empty(); }

void on_tick(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (game_is_paused(rdram_, ctx)) return;
    const int frames = std::max(1, (int)(int32_t)ctx->r4);
    for (size_t i = 0; i < g_fx.size();) {
        Fx& e = g_fx[i];
        bool gone = !claim_alive(e.obj); // freed by the game
        if (!gone) {
            e.age += frames;
            if (e.age >= e.ticks) {
                unclaim(e.obj);
                free_object(rdram_, ctx, e.obj);
                gone = true;
            }
        }
        if (gone) {
            g_fx[i] = g_fx.back();
            g_fx.pop_back();
            continue;
        }
        Obj::PosX::set(e.obj, Obj::PosX::get(e.obj) + e.vx * frames);
        Obj::PosY::set(e.obj, Obj::PosY::get(e.obj) + e.vy * frames);
        Obj::PosZ::set(e.obj, Obj::PosZ::get(e.obj) + e.vz * frames);
        for (int f = 0; f < frames; f++) { e.vx *= e.keep; e.vy *= e.keep; e.vz *= e.keep; }
        const float t = (float)e.age / (float)e.ticks;
        const float grow = e.ease_out ? 1.0f - (1.0f - t) * (1.0f - t) : t;
        set_object_scale(e.obj, e.scale_from + (e.scale_to - e.scale_from) * grow);
        if (e.opacity > 0 && e.fade_from < 1.0f && fading_kind(e.obj)) {
            const float left = t < e.fade_from ? 1.0f : 1.0f - (t - e.fade_from) / (1.0f - e.fade_from);
            wr32(e.obj + OBJ_OPACITY, (uint32_t)std::clamp((int)(e.opacity * left), 0, 255));
        }
        i++;
    }
}

void on_level_change(uint8_t*, recomp_context*) { g_fx.clear(); } // gone with the level
} // namespace

void register_fx_api() {
    recomp::overlays::register_base_export("jfg_fx_spawn", fx_spawn);
    recomp::overlays::register_base_export("jfg_object_set_opacity", object_set_opacity);
    api::add_internal(api::Event::GameTick, on_tick, active);
    api::add_internal(api::Event::LevelChange, on_level_change);
}

} // namespace jfg
