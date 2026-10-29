// Jet Force Gemini port: pull fields for mods (mod API jfg_pull_set, jfg_pull_end, jfg_pull_caught). A vortex, a black
// hole: a point that draws the enemies around it in, catching them (tinted, alerted or stunned), sliding them toward
// it faster the closer they are, and hurting those near its middle every so often. The port runs every mod's pulls
// together, so an enemy two pulls catch moves once a tick, by the strongest of them, not by both added up.
// - Only enemies with nothing between them and the point are pulled (Tribals only with JFG_PULL_TRIBALS, never hurt).
// - The step: strength level units a tick at the middle, edge times that at the reach's edge; it stops at hold from
//   the middle. With JFG_PULL_MASS heavier enemies (health, size) slide slower and flyers, half as heavy, are drawn up
//   or down too.
// - Every hurt_ticks those within core x reach + hold lose damage (never past dying), credited to weapon and owner.
// - Each mod's tint is its own (jfg_object_tint); ending a pull takes its tints off.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

struct NearEnemy { uint32_t obj; float x, y, z; int health; bool tribal; };      // jfg_hero_api.cpp
int near_enemies(uint8_t* rdram, recomp_context* ctx, float x, float y, float z, float radius, NearEnemy* out, int max);
bool clear_line(uint8_t* rdram, recomp_context* ctx, uint32_t from, uint32_t to, const float* a, const float* b);
void alert_enemy(uint8_t* rdram, recomp_context* ctx, uint32_t enemy, uint32_t shooter, bool stun);
float enemy_mass(uint8_t* rdram, recomp_context* ctx, uint32_t enemy, float flyer_height, bool* flyer);
bool game_is_paused(uint8_t* rdram, recomp_context* ctx);
void credit_weapon(uint8_t* rdram, uint32_t victim, int weapon, uint32_t shooter, int ticks); // jfg_kills.cpp
namespace api { void tint_for_mod(uint32_t object, uint32_t rgba, size_t mod_index); }         // jfg_api.cpp

namespace {
constexpr int FLAG_TRIBALS = 1, FLAG_MASS = 2, FLAG_STUN = 4;
constexpr int NEARBY = 24, MAX_CAUGHT = 24, MAX_PULLS = 32;
constexpr float FLYER_HEIGHT = 35.0f;
constexpr int CREDIT_TICKS = 600; // a pulled enemy that dies within this long is the weapon's kill

struct Caught { uint32_t obj; bool tribal; };
struct Pull {
    int id;
    size_t mod;
    float x, y, z, reach, strength, edge, hold, core;
    int damage, hurt_ticks, hurt_in;
    uint32_t tint;
    int flags, weapon;
    uint32_t owner;
    std::vector<Caught> caught;
};
std::vector<Pull> g_pulls;
int g_next_id = 1;

Pull* find(int id, size_t mod) {
    for (Pull& p : g_pulls) if (p.id == id && p.mod == mod) return &p;
    return nullptr;
}

void untint(Pull& p) {
    if (p.tint == 0) return;
    for (const Caught& c : p.caught) api::tint_for_mod(c.obj, 0, p.mod);
}

// jfg_pull_set(int id, const JfgPull* pull) -> its id: a new pull (id < 0), or one of the mod's moved or changed.
void pull_set(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4;
    const uint32_t s = (uint32_t)ctx->r5;
    ctx->r2 = (gpr)(int32_t)-1;
    if (s < 0x80000400u || s >= 0xA0000000u) return;
    Pull* p = id >= 0 ? find(id, mod_index) : nullptr;
    if (p == nullptr) {
        if (id >= 0 || (int)g_pulls.size() >= MAX_PULLS) return;
        g_pulls.push_back({});
        p = &g_pulls.back();
        p->id = g_next_id++;
        p->mod = mod_index;
        p->hurt_in = 0;
    }
    p->x = rdf(s + 0x00); p->y = rdf(s + 0x04); p->z = rdf(s + 0x08);
    p->reach = std::max(0.0f, rdf(s + 0x0C));
    p->strength = std::max(0.0f, rdf(s + 0x10));
    p->edge = std::clamp(rdf(s + 0x14), 0.0f, 1.0f);
    p->hold = std::max(0.0f, rdf(s + 0x18));
    p->core = std::clamp(rdf(s + 0x1C), 0.0f, 1.0f);
    p->damage = std::max(0, (int)(int32_t)rd32(s + 0x20));
    p->hurt_ticks = std::max(1, (int)(int32_t)rd32(s + 0x24));
    const uint32_t tint = rd32(s + 0x28);
    if (tint != p->tint) {
        untint(*p);
        p->tint = tint;
        if (tint != 0) for (const Caught& c : p->caught) api::tint_for_mod(c.obj, tint, p->mod);
    }
    p->flags = (int32_t)rd32(s + 0x2C);
    p->weapon = (int32_t)rd32(s + 0x30);
    p->owner = rd32(s + 0x34);
    ctx->r2 = (gpr)p->id;
}

// jfg_pull_end(int id): the pull stops and lets go (its tints taken off).
void pull_end(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_rdram = rdram_;
    const int id = (int32_t)ctx->r4;
    for (size_t i = 0; i < g_pulls.size(); i++) {
        if (g_pulls[i].id != id || g_pulls[i].mod != mod_index) continue;
        untint(g_pulls[i]);
        g_pulls.erase(g_pulls.begin() + (long)i);
        return;
    }
}

// jfg_pull_caught(int id, JfgPullCaught* out, int max) -> how many the pull holds ({ void* enemy; int tribal; }).
void pull_caught(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_rdram = rdram_;
    const Pull* p = find((int32_t)ctx->r4, mod_index);
    const uint32_t out = (uint32_t)ctx->r5;
    const int max = (int32_t)ctx->r6;
    ctx->r2 = 0;
    if (p == nullptr) return;
    for (int i = 0; i < (int)p->caught.size() && i < max && out >= 0x80000400u && out < 0xA0000000u; i++) {
        wr32(out + 8u * (uint32_t)i, p->caught[(size_t)i].obj);
        wr32(out + 8u * (uint32_t)i + 4, p->caught[(size_t)i].tribal ? 1u : 0u);
    }
    ctx->r2 = (gpr)(int32_t)p->caught.size();
}

bool active() { return !g_pulls.empty(); }

struct Move { float x, z, y, step; };

void on_tick(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (game_is_paused(rdram_, ctx)) return;
    const int frames = std::max(1, (int)(int32_t)ctx->r4);
    std::map<uint32_t, Move> moves; // each enemy's strongest step this tick
    NearEnemy found[NEARBY];
    for (Pull& p : g_pulls) {
        const int n = near_enemies(rdram_, ctx, p.x, p.y, p.z, p.reach, found, NEARBY);
        const float at[3] = { p.x, p.y, p.z };
        p.hurt_in -= frames;
        const bool hurt_now = p.hurt_in <= 0;
        if (hurt_now) p.hurt_in = p.hurt_ticks;
        for (int i = 0; i < n; i++) {
            const NearEnemy& e = found[i];
            if ((e.tribal && !(p.flags & FLAG_TRIBALS)) || e.health <= 0) continue;
            const float from[3] = { e.x, e.y, e.z };
            if (!clear_line(rdram_, ctx, e.obj, 0, from, at)) continue;
            const bool known = std::any_of(p.caught.begin(), p.caught.end(), [&](const Caught& c) { return c.obj == e.obj; });
            if (!known && (int)p.caught.size() < MAX_CAUGHT) {
                p.caught.push_back({ e.obj, e.tribal });
                if (valid_ptr(p.owner) && !e.tribal) alert_enemy(rdram_, ctx, e.obj, p.owner, (p.flags & FLAG_STUN) != 0);
                if (p.tint != 0) api::tint_for_mod(e.obj, p.tint, p.mod);
            }
            const float dx = p.x - Obj::PosX::get(e.obj), dz = p.z - Obj::PosZ::get(e.obj), dy = p.y - Obj::PosY::get(e.obj);
            const float d = std::sqrt(dx * dx + dz * dz);
            if (d > p.hold) {
                float mass = 1.0f;
                bool flyer = false;
                if (p.flags & FLAG_MASS) mass = enemy_mass(rdram_, ctx, e.obj, FLYER_HEIGHT, &flyer);
                const float near = 1.0f - std::min(1.0f, p.reach > 0.0f ? d / p.reach : 1.0f);
                const float step = std::min(p.strength * (p.edge + (1.0f - p.edge) * near) * frames / mass, d - p.hold);
                const float up = flyer ? std::clamp(dy * 0.03f * frames, -step, step) : 0.0f;
                Move& m = moves[e.obj];
                if (step > m.step) m = { dx / d * step, dz / d * step, up, step };
            }
            if (hurt_now && !e.tribal && p.damage > 0 && d < p.reach * p.core + p.hold) {
                if (p.weapon >= 0) credit_weapon(rdram_, e.obj, p.weapon, p.owner, CREDIT_TICKS);
                const uint32_t dmg = Obj::Damage::get(e.obj);
                if (valid_ptr(dmg)) {
                    const int health = Damage::Health::get(dmg);
                    if (health > 0) Damage::Health::set(dmg, (int16_t)std::max(0, health - p.damage));
                }
            }
        }
    }
    for (const auto& entry : moves) {
        const uint32_t obj = entry.first;
        const Move& m = entry.second;
        Obj::PosX::set(obj, Obj::PosX::get(obj) + m.x);
        Obj::PosZ::set(obj, Obj::PosZ::get(obj) + m.z);
        if (m.y != 0.0f) Obj::PosY::set(obj, Obj::PosY::get(obj) + m.y);
    }
}

void on_level_change(uint8_t*, recomp_context*) { g_pulls.clear(); } // the tints go with the level (jfg_api.cpp)
} // namespace

void register_pull_api() {
    recomp::overlays::register_ext_base_export("jfg_pull_set", pull_set);
    recomp::overlays::register_ext_base_export("jfg_pull_end", pull_end);
    recomp::overlays::register_ext_base_export("jfg_pull_caught", pull_caught);
    api::add_internal(api::Event::GameTick, on_tick, active);
    api::add_internal(api::Event::LevelChange, on_level_change);
}

} // namespace jfg
