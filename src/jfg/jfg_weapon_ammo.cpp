// Jet Force Gemini port: ammo for mods' weapons (mod API jfg_weapon_ammo_sources). The game has no ammo for a weapon a
// mod adds, so the mod names where more comes from and the port runs it, the same way for every weapon:
// - Drops: a cell (the pack's object type) may drop where an enemy dies, as one of the shared drop pool's drops
//   (jfg_drops.cpp: one roll per kill across all mods). It falls to the floor under it (the level's, as the
//   crosshair tests it) and rests with its middle cell_height above it, so one from a flyer comes down where the hero
//   can reach it; it spins, and walking into it gives its ammo (the game's pick-up sound). It blinks out after a
//   while. A pickup magnet (jfg_drop_magnet) draws cells within its reach to the hero.
// - Refill: picking up a stock weapon's ammo, or the weapon itself, gives some too (the pick-up's HUD message).
// - Pickups: a share of the level's pickups of a stock weapon give the mod's weapon instead (their model stays). The
//   share is counted as pickups appear, so it holds over a level; when several weapons share the same stock weapon's
//   pickups, a pickup turns into at most one of them.
// - Recharge: one comes back every so many ticks while the hero has fewer than the weapon's capacity.
// Ammo is given with jfg_hero_give_weapon's rule: the weapon too if the hero hasn't it, never past its capacity.
// Cells are claimed (jfg_object_claims.cpp), so one the game frees is never touched again.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

// jfg_hero_api.cpp, jfg_weapon_registry.cpp, jfg_object_claims.cpp, jfg_drops.cpp
uint32_t spawn_object(uint8_t* rdram, recomp_context* ctx, int id, float x, float y, float z, float vx, float vy,
                      float vz, uint16_t yaw);
void free_object(uint8_t* rdram, recomp_context* ctx, uint32_t obj);
void set_object_scale(uint32_t obj, float scale);
void play_flat_sound(uint8_t* rdram, recomp_context* ctx, int id);
uint32_t first_hero(uint8_t* rdram, recomp_context* ctx);
bool game_is_paused(uint8_t* rdram, recomp_context* ctx);
int hero_ammo_of(uint8_t* rdram, recomp_context* ctx, int weapon);
namespace weapons {
void give(uint8_t* rdram, recomp_context* ctx, int id, int ammo);
bool owned(uint8_t* rdram, recomp_context* ctx, int id);
int capacity(int id);
int pickup_get(uint8_t* rdram, recomp_context* ctx, uint32_t obj);
void pickup_set(uint8_t* rdram, recomp_context* ctx, uint32_t obj, int id);
}
void claim_for_mod(uint32_t obj, size_t mod_index);
bool claim_alive(uint32_t obj);
void unclaim(uint32_t obj);
int drop_register_named(const std::string& name, int chance, int flags);
float drop_magnet(); // jfg_drops.cpp
int asset_number(const std::string& ref); // jfg_assets.cpp

namespace {
constexpr int KIND_WEAPON_PICKUP = 43;
constexpr int SOUND_PICKUP = 0x8A;
constexpr int MSG_NAME = 0x39, MSG_AMMO = 0x48, MSG_CLIP = 0x57, MSG_FULL = 0x66, MSG_CAPACITY = 0x75; // + weapon
constexpr int MAX_CELLS = 24, MAX_SEEN = 96, SCAN_TICKS = 60;
constexpr int CELL_LIFE = 30 * 60, CELL_BLINK = 5 * 60, CELL_SPIN = 1500;
constexpr float CELL_REACH = 38.0f;
constexpr int CELL_FALLS = 1; // JfgAmmoSources.cell_flags: every cell falls now; the flag is kept for mods
constexpr float CELL_GRAVITY = 0.45f, CELL_FALL_MAX = 9.0f, MAGNET_SPEED = 7.0f; // units a 1/60 s tick
constexpr int DROP_BIG = 1;   // as JFG_DROP_BIG
constexpr size_t PORT_CLAIM = SIZE_MAX - 1; // the claims of the port's own cells

struct Source {
    int weapon = -1;
    std::string cell;      // the pack's object type ("mod:objtype_x"), empty: no drops
    int cell_type = -2;    // its object id this start (-2 not looked up)
    int drop_chance = 0, cell_ammo = 1, cell_flags = 0;
    float cell_scale = 1.0f, cell_height = 20.0f;
    int refill_from = -1, refill_ammo = 0;
    int pickup_from = -1, pickup_share = 0;
    int recharge_ticks = 0;
    int drop_id = -1;
    int recharge_left = 0;
    int share_sum = 50;    // the first pickup a little sooner
};
std::vector<Source> g_sources;

struct Cell {
    uint32_t obj;
    int source, age;
    float scale;
    float rest;      // the y it rests at (the floor under it + cell_height), NAN when no floor was found
    float vy;        // falling speed
};

// The floor under (x, y, z): the first level surface on a line straight down (trackNearestIntersection, the crosshair's
// test of the level), NAN if none within 4000.
float floor_under(recomp_context* ctx, float x, float y, float z) {
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // in the gap call_game leaves below $sp
    wrf(buf + 0, x); wrf(buf + 4, y + 10.0f); wrf(buf + 8, z);
    wrf(buf + 12, x); wrf(buf + 16, y - 4000.0f); wrf(buf + 20, z);
    const recomp_context saved = *ctx;
    const bool hit = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
    *ctx = saved;
    const float fy = rdf(buf + 32);
    return hit && std::isfinite(fy) ? fy : NAN;
}
std::vector<Cell> g_cells;
std::vector<uint32_t> g_seen_pickups;
int g_scan_wait = 0;
uint32_t g_seed = 0xA77E5EEDu;

int rnd(int n) {
    g_seed = g_seed * 1103515245u + 12345u;
    return n > 0 ? (int)((g_seed >> 16) % (uint32_t)n) : 0;
}

Source* source_of(int weapon) {
    for (Source& s : g_sources) if (s.weapon == weapon) return &s;
    return nullptr;
}

std::string read_name(uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 64 && addr >= 0x80000400u && addr < 0xA0000000u; i++) {
        const char c = (char)rd8(addr + i);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// jfg_weapon_ammo_sources(int weapon, const JfgAmmoSources* s): sets (or changes) where that weapon's ammo comes from;
// call it again whenever the mod's options may have changed (each tick is fine), with zeros to turn sources off.
void weapon_ammo_sources(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int weapon = (int32_t)ctx->r4;
    const uint32_t s = (uint32_t)ctx->r5;
    if (weapon < 0 || s < 0x80000400u || s >= 0xA0000000u) return;
    Source* src = source_of(weapon);
    if (src == nullptr) {
        g_sources.push_back({});
        src = &g_sources.back();
        src->weapon = weapon;
    }
    const std::string cell = rd32(s + 0x00) != 0 ? read_name(rd32(s + 0x00)) : "";
    if (cell != src->cell) { src->cell = cell; src->cell_type = -2; }
    src->drop_chance = std::clamp((int)(int32_t)rd32(s + 0x04), 0, 100);
    src->cell_ammo = std::max(1, (int)(int32_t)rd32(s + 0x08));
    src->cell_scale = rdf(s + 0x0C) > 0.0f ? rdf(s + 0x0C) : 1.0f;
    src->cell_height = rdf(s + 0x10);
    src->cell_flags = (int32_t)rd32(s + 0x14);
    src->refill_from = (int32_t)rd32(s + 0x18);
    src->refill_ammo = std::max(0, (int)(int32_t)rd32(s + 0x1C));
    src->pickup_from = (int32_t)rd32(s + 0x20);
    src->pickup_share = std::clamp((int)(int32_t)rd32(s + 0x24), 0, 100);
    src->recharge_ticks = std::max(0, (int)(int32_t)rd32(s + 0x28));
    src->drop_id = drop_register_named("weapon_ammo:" + std::to_string(weapon), cell.empty() ? 0 : src->drop_chance,
                                       DROP_BIG);
}

bool active() { return !g_sources.empty(); }

// A shared drop came up: a cell of the weapon whose drop it is.
void on_drop(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t d = (uint32_t)ctx->r4;
    const int id = (int32_t)rd32(d + 0x00);
    for (Source& s : g_sources) {
        if (s.drop_id != id || s.cell.empty() || s.drop_chance <= 0) continue;
        if (s.cell_type == -2) s.cell_type = asset_number(s.cell);
        if (s.cell_type < 0 || (int)g_cells.size() >= MAX_CELLS) return;
        // Made still (no game physics: it would rest the cell's middle on the floor, half in it); the port drops it.
        const float x = rdf(d + 0x10), y = rdf(d + 0x14) + s.cell_height, z = rdf(d + 0x18);
        const uint32_t cell = spawn_object(rdram_, ctx, s.cell_type, x, y, z, 0.0f, 0.0f, 0.0f, (uint16_t)rnd(0x10000));
        if (!valid_ptr(cell)) return;
        set_object_scale(cell, s.cell_scale);
        claim_for_mod(cell, PORT_CLAIM);
        const float floor = floor_under(ctx, x, y, z);
        const float rest = std::isfinite(floor) ? std::min(y, floor + s.cell_height) : NAN;
        g_cells.push_back({ cell, (int)(&s - g_sources.data()), 0, s.cell_scale, rest, 0.0f });
        return;
    }
}

// The pick-up's HUD message: a refill from the stock weapon it names.
void on_hud_message(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int msg = (int32_t)ctx->r4, player = (int32_t)ctx->r6;
    if (player != 0) return;
    for (const Source& s : g_sources) {
        const int w = s.refill_from;
        if (w < 0 || s.refill_ammo <= 0) continue;
        if (msg == MSG_NAME + w || msg == MSG_AMMO + w || msg == MSG_CLIP + w || msg == MSG_FULL + w || msg == MSG_CAPACITY + w)
            weapons::give(rdram_, ctx, s.weapon, s.refill_ammo);
    }
}

void tick_cells(uint8_t* rdram_, recomp_context* ctx, uint32_t hero, int frames) {
    for (size_t i = 0; i < g_cells.size();) {
        Cell& c = g_cells[i];
        bool gone = !claim_alive(c.obj); // freed by the game
        if (!gone) {
            c.age += frames;
            Obj::Yaw::set(c.obj, (uint16_t)(Obj::Yaw::get(c.obj) + CELL_SPIN * frames));
            // Falling to its resting height.
            if (std::isfinite(c.rest) && Obj::PosY::get(c.obj) > c.rest) {
                c.vy = std::min(CELL_FALL_MAX, c.vy + CELL_GRAVITY * (float)frames);
                Obj::PosY::set(c.obj, std::max(c.rest, Obj::PosY::get(c.obj) - c.vy * (float)frames));
            }
            // The pickup magnet: within its reach the cell drifts to the hero's middle.
            if (const float reach = drop_magnet(); reach > 0.0f) {
                const float mx = Obj::PosX::get(hero) - Obj::PosX::get(c.obj), my = Obj::PosY::get(hero) + 30.0f - Obj::PosY::get(c.obj),
                            mz = Obj::PosZ::get(hero) - Obj::PosZ::get(c.obj);
                const float md = std::sqrt(mx * mx + my * my + mz * mz);
                if (md < reach && md > 1.0f) {
                    const float step = std::min(md, MAGNET_SPEED * (float)frames) / md;
                    Obj::PosX::set(c.obj, Obj::PosX::get(c.obj) + mx * step);
                    Obj::PosY::set(c.obj, Obj::PosY::get(c.obj) + my * step);
                    Obj::PosZ::set(c.obj, Obj::PosZ::get(c.obj) + mz * step);
                    c.rest = NAN; // drawn in: no more falling
                }
            }
            const float dx = Obj::PosX::get(c.obj) - Obj::PosX::get(hero), dy = Obj::PosY::get(c.obj) - Obj::PosY::get(hero),
                        dz = Obj::PosZ::get(c.obj) - Obj::PosZ::get(hero);
            const Source& s = g_sources[(size_t)c.source];
            if (dx * dx + dz * dz < CELL_REACH * CELL_REACH && dy > -40.0f && dy < 70.0f) {
                weapons::give(rdram_, ctx, s.weapon, s.cell_ammo);
                play_flat_sound(rdram_, ctx, SOUND_PICKUP);
                gone = true;
            } else if (c.age >= CELL_LIFE) {
                gone = true;
            } else {
                const bool blink = c.age >= CELL_LIFE - CELL_BLINK && (c.age / 8) % 2 == 1; // every other eighth of a second
                set_object_scale(c.obj, blink ? 0.01f : c.scale);
            }
            if (gone) {
                unclaim(c.obj);
                free_object(rdram_, ctx, c.obj);
            }
        }
        if (gone) {
            g_cells[i] = g_cells.back();
            g_cells.pop_back();
        } else {
            i++;
        }
    }
}

void scan_pickups(uint8_t* rdram_, recomp_context* ctx) {
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = std::min((int)(int32_t)rd32(ADDR_ObjListCount), 512);
    if (!valid_ptr(list)) return;
    for (int i = 0; i < count; i++) {
        const uint32_t obj = rd32(list + 4u * (uint32_t)i);
        if (!valid_ptr(obj) || Obj::Kind::get(obj) != KIND_WEAPON_PICKUP) continue;
        if (std::find(g_seen_pickups.begin(), g_seen_pickups.end(), obj) != g_seen_pickups.end()) continue;
        if ((int)g_seen_pickups.size() >= MAX_SEEN) return;
        g_seen_pickups.push_back(obj);
        const int stock = weapons::pickup_get(rdram_, ctx, obj);
        Source* pick = nullptr;
        for (Source& s : g_sources) {
            if (s.pickup_from != stock || s.pickup_share <= 0) continue;
            s.share_sum += s.pickup_share; // every (100 / share)th of those pickups
            if (s.share_sum >= 100 && (pick == nullptr || s.share_sum > pick->share_sum)) pick = &s;
        }
        if (pick != nullptr) {
            pick->share_sum -= 100;
            weapons::pickup_set(rdram_, ctx, obj, pick->weapon);
        }
    }
}

void tick_recharge(uint8_t* rdram_, recomp_context* ctx, int frames) {
    for (Source& s : g_sources) {
        if (s.recharge_ticks <= 0 || !weapons::owned(rdram_, ctx, s.weapon) ||
            hero_ammo_of(rdram_, ctx, s.weapon) >= weapons::capacity(s.weapon)) {
            s.recharge_left = s.recharge_ticks;
            continue;
        }
        s.recharge_left -= frames;
        if (s.recharge_left <= 0) {
            weapons::give(rdram_, ctx, s.weapon, 1);
            s.recharge_left = s.recharge_ticks;
        }
    }
}

void on_tick(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int frames = std::max(1, (int)(int32_t)ctx->r4);
    const uint32_t hero = first_hero(rdram_, ctx);
    if (!valid_ptr(hero) || game_is_paused(rdram_, ctx)) return;
    tick_cells(rdram_, ctx, hero, frames);
    tick_recharge(rdram_, ctx, frames);
    if ((g_scan_wait -= frames) <= 0) {
        g_scan_wait = SCAN_TICKS;
        scan_pickups(rdram_, ctx);
    }
}

void on_level_change(uint8_t*, recomp_context*) {
    g_cells.clear(); // gone with the level
    g_seen_pickups.clear();
    g_scan_wait = 0;
    for (Source& s : g_sources) s.share_sum = 50;
}
} // namespace

void register_weapon_ammo_api() {
    recomp::overlays::register_base_export("jfg_weapon_ammo_sources", weapon_ammo_sources);
    api::add_internal(api::Event::GameTick, on_tick, active);
    api::add_internal(api::Event::Drop, on_drop, active);
    api::add_internal(api::Event::HudMessage, on_hud_message, active);
    api::add_internal(api::Event::LevelChange, on_level_change, active);
}

} // namespace jfg
