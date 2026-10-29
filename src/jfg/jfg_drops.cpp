// Jet Force Gemini port: one shared pool of mods' item drops (mod API jfg_drop_register, jfg_on_drop).
//
// Mods that drop their own items from enemies (a weapon's ammo cells, say) register a drop with its chance per kill
// instead of rolling on jfg_on_kill each. The port rolls once per kill, so a kill drops at most one mod's item:
//   - only enemies the heroes or Floyd killed (no Tribals);
//   - a drop with JFG_DROP_BIG has twice its chance from a big enemy (hit sphere radius 30 or more, or 10 health units);
//   - from a squad the enemy multiplier made bigger (jfg_squad_multiplier.cpp), each chance is scaled by its stock size
//     over its size now, so 3x the enemies give about the same drops in all;
//   - when the chances add up to more than 100%, they are scaled down to share it.
// The drop that comes up is fired as jfg_on_drop to every mod, with its id: the mod whose drop it is spawns its item.
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

float squad_mult_drop_scale(uint32_t member); // jfg_squad_multiplier.cpp

namespace {
constexpr int MAX_DROPS = 32;
constexpr int FLAG_BIG = 1;
constexpr float BIG_RADIUS = 30.0f;
constexpr int BIG_HEALTH = 10 * 256;
constexpr int KILLER_FLOYD = 3;

struct Drop {
    std::string name;
    int chance;     // percent a kill
    int flags;
};
std::vector<Drop> g_drops;
uint32_t g_seed = 0x2545F491u;

float random01() {
    g_seed = g_seed * 1103515245u + 12345u;
    return (float)((g_seed >> 8) & 0xFFFFFF) / 16777216.0f;
}

bool big_enemy(uint32_t victim) {
    const uint32_t dmg = Obj::Damage::get(victim), data = Obj::Data::get(victim);
    const uint32_t squadron = valid_ptr(data) ? SquadMemberData::Squadron::get(data) : 0;
    const uint32_t sdata = valid_ptr(squadron) ? Obj::Data::get(squadron) : 0;
    const int full = valid_ptr(sdata) ? SquadronData::MemberHealth::get(sdata) : 0;
    const float radius = valid_ptr(dmg) ? Damage::Radius::get(dmg) : 0.0f;
    return radius >= BIG_RADIUS || full >= BIG_HEALTH;
}

std::string read_name(uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 64; i++) {
        const char c = (char)rd8(addr + i);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

// jfg_drop_register(const char* name, int chance, int flags) -> its id (0 up), or -1. Registering the name again
// changes its chance and flags and gives the same id (call it whenever the mod's option may have changed).
void drop_register(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t name_at = (uint32_t)ctx->r4;
    ctx->r2 = (gpr)(int32_t)-1;
    if (name_at < 0x80000400u || name_at >= 0xA0000000u) return;
    const std::string name = read_name(name_at);
    if (name.empty()) return;
    const int chance = std::clamp((int)(int32_t)ctx->r5, 0, 100), flags = (int32_t)ctx->r6;
    for (size_t i = 0; i < g_drops.size(); i++) {
        if (g_drops[i].name != name) continue;
        g_drops[i].chance = chance;
        g_drops[i].flags = flags;
        ctx->r2 = (gpr)(int32_t)i;
        return;
    }
    if ((int)g_drops.size() >= MAX_DROPS) return;
    g_drops.push_back({ name, chance, flags });
    ctx->r2 = (gpr)(int32_t)(g_drops.size() - 1);
}
} // namespace

bool drops_active() {
    return api::listened(api::Event::Drop) && !g_drops.empty();
}

// From report_kill (jfg_kills.cpp), after jfg_on_kill: at most one drop for the kill.
void drops_roll(uint8_t* rdram_, recomp_context* ctx, uint32_t victim, uint32_t killer, int killer_kind, bool tribal) {
    g_rdram = rdram_;
    if (!drops_active() || tribal || killer_kind < 0 || killer_kind > KILLER_FLOYD || !valid_ptr(victim)) return;
    const bool big = big_enemy(victim);
    const float scale = squad_mult_drop_scale(victim);
    float chances[MAX_DROPS], total = 0.0f;
    for (size_t i = 0; i < g_drops.size(); i++) {
        float c = (float)g_drops[i].chance;
        if (big && (g_drops[i].flags & FLAG_BIG)) c *= 2.0f;
        chances[i] = c * scale;
        total += chances[i];
    }
    if (total <= 0.0f) return;
    const float roll = random01() * std::max(100.0f, total);
    int picked = -1;
    float at = 0.0f;
    for (size_t i = 0; i < g_drops.size() && picked < 0; i++) {
        at += chances[i];
        if (roll < at) picked = (int)i;
    }
    if (picked < 0) return;
    const gpr a0 = ctx->r4;
    {
        api::Payload p(ctx, 0x20);
        wr32(p.addr + 0x00, (uint32_t)picked);
        wr32(p.addr + 0x04, victim);
        wr32(p.addr + 0x08, valid_ptr(killer) ? killer : 0);
        wr32(p.addr + 0x0C, (uint32_t)killer_kind);
        wrf(p.addr + 0x10, Obj::PosX::get(victim));
        wrf(p.addr + 0x14, Obj::PosY::get(victim));
        wrf(p.addr + 0x18, Obj::PosZ::get(victim));
        wr32(p.addr + 0x1C, big ? 1u : 0u);
        api::fire(rdram_, ctx, api::Event::Drop, (int32_t)p.addr);
    }
    ctx->r4 = a0;
}

void register_drop_api() {
    recomp::overlays::register_base_export("jfg_drop_register", drop_register);
}

} // namespace jfg
