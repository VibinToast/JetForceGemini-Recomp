// Jet Force Gemini port: kills for mods (mod_api/jfg_api.h, jfg_on_kill).
//
// Every squad member (enemies and Tribals) dies through SquaddiePassesAway (overlay 3), which also counts the kill in
// the game's weapon stats (mainIncreaseWeaponKills with its damage's low nibble: the weapon that killed it). Its
// wrapper fires jfg_on_kill with the victim, the weapon and who fired. The game keeps no shooter for most kills (the
// damage's last hitter, +0x3C, is set only by a decapitation), so the port remembers the last one each squad member
// was shot by: squadsPlayerShootsSquaddie(member, shooter), called by the hits (hitUpdate) and the flamethrower (its wrapper is in jfg_squad_cheat.cpp). Names
// are the objects' own definition names from the player's ROM (ObjDef +4, 16 bytes): the port and mods ship none.
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {
namespace {
constexpr int16_t KIND_HERO = 1, KIND_FLOYD = 2;
constexpr int KILLER_NONE = -1, KILLER_FLOYD = 3, KILLER_OBJECT = 4;

bool debug_kills() {
    static const bool on = std::getenv("JFG_DEBUG_KILLS") != nullptr;
    return on;
}

// The last shooter of the squad members shot lately (victim -> shooter), oldest overwritten.
struct Shot {
    uint32_t victim, shooter;
};
constexpr int SHOTS = 32;
Shot g_shots[SHOTS];
int g_next_shot = 0;

} // namespace

// From squadsPlayerShootsSquaddie's wrapper (jfg_squad_cheat.cpp).
void kills_remember_shot(uint32_t victim, uint32_t shooter) {
    for (Shot& s : g_shots) {
        if (s.victim == victim) { s.shooter = shooter; return; }
    }
    g_shots[g_next_shot] = { victim, shooter };
    g_next_shot = (g_next_shot + 1) % SHOTS;
}

namespace {
uint32_t take_shot(uint32_t victim) {
    for (Shot& s : g_shots) {
        if (s.victim == victim) { s.victim = 0; return s.shooter; }
    }
    return 0;
}

// The object's definition name (up to 15 characters), "" when it has none.
void def_name(uint32_t obj, char out[16]) {
    out[0] = 0;
    const uint32_t def = valid_ptr(obj) ? Obj::Def::get(obj) : 0;
    if (!valid_ptr(def)) return;
    for (int i = 0; i < 15; i++) {
        const uint8_t c = rd8(def + 4 + (uint32_t)i);
        if (c < 0x20 || c > 0x7E) { out[i] = 0; return; }
        out[i] = (char)c;
    }
    out[15] = 0;
}

// Whoever fired: a hero or Floyd, else the object behind the shot.
uint32_t shooter_of(uint32_t hit) {
    for (int depth = 0; depth < 3 && valid_ptr(hit); depth++) {
        const int16_t kind = Obj::Kind::get(hit);
        if (kind == KIND_HERO || kind == KIND_FLOYD) return hit;
        const uint32_t data = Obj::Data::get(hit);
        if (!valid_ptr(data)) return hit;
        uint32_t owner = BoltData::Owner::get(data);
        if (!valid_ptr(owner) || owner == hit) owner = rd32(data);
        if (!valid_ptr(owner) || owner == hit) return hit;
        hit = owner;
    }
    return hit;
}
} // namespace
} // namespace jfg

namespace jfg {
namespace {
bool g_in_passes = false; // inside SquaddiePassesAway: its own kill count is reported by its wrapper

uint32_t current_hero(recomp_context* ctx) {
    const uint32_t hero = player_object(ctx);
    return valid_ptr(hero) ? hero : 0;
}

// jfg_on_kill (and the debug log) for one death.
void report_kill(uint8_t* rdram, recomp_context* ctx, uint32_t victim, uint32_t killer, int weapon, bool tribal,
                 const char* where) {
    g_rdram = rdram;
    if (!valid_ptr(victim) || (!api::listened(api::Event::Kill) && !debug_kills())) return;
    int killer_kind = KILLER_NONE;
    if (valid_ptr(killer)) {
        const int16_t kind = Obj::Kind::get(killer);
        const uint32_t data = Obj::Data::get(killer);
        if (kind == KIND_HERO && valid_ptr(data)) killer_kind = PlayerData::Character::get(data) & 3;
        else if (kind == KIND_FLOYD) killer_kind = KILLER_FLOYD;
        else killer_kind = KILLER_OBJECT;
    }
    char vname[16], kname[16];
    def_name(victim, vname);
    def_name(killer, kname);
    if (debug_kills()) {
        std::fprintf(stderr, "[kills] %s: victim %08X kind %d '%s' weapon %d tribal %d | killer %08X kind %d '%s' -> %d\n",
                     where, victim, Obj::Kind::get(victim), vname, weapon, tribal ? 1 : 0, killer,
                     valid_ptr(killer) ? Obj::Kind::get(killer) : -1, kname, killer_kind);
    }
    if (!api::listened(api::Event::Kill)) return;
    const gpr a0 = ctx->r4;
    {
        api::Payload p(ctx, 0x38);
        wr32(p.addr + 0x00, victim);
        wr32(p.addr + 0x04, valid_ptr(killer) ? killer : 0);
        wr32(p.addr + 0x08, (uint32_t)killer_kind);
        wr32(p.addr + 0x0C, (uint32_t)weapon);
        wr32(p.addr + 0x10, tribal ? 1u : 0u);
        wr32(p.addr + 0x14, 0);
        for (uint32_t i = 0; i < 16; i++) {
            wr8(p.addr + 0x18 + i, (uint8_t)vname[i]);
            wr8(p.addr + 0x28 + i, (uint8_t)kname[i]);
        }
        api::fire(rdram, ctx, api::Event::Kill, (int32_t)p.addr);
    }
    ctx->r4 = a0;
}
} // namespace
} // namespace jfg

// SquaddiePassesAway(obj): jfg_on_kill after the game's code. JfgKill: void* victim, void* killer (0 when unknown),
// s32 killer_kind (-1 unknown, 0 Vela, 1 Juno, 2 Lupus, 3 Floyd, 4 another object), weapon (0..15), tribal (1: the
// victim is a Tribal), frames (0), char victim_name[16], killer_name[16].
JFG_WRAP(SquaddiePassesAway) {
    using namespace jfg;
    g_rdram = rdram;
    const uint32_t victim = (uint32_t)ctx->r4;
    const uint32_t dmg = valid_ptr(victim) ? Obj::Damage::get(victim) : 0;
    const int weapon = valid_ptr(dmg) ? (rd8(dmg + 0x08) & 0xF) : -1;
    const uint32_t shot = take_shot(victim);
    const uint32_t hit = valid_ptr(shot) ? shot : (valid_ptr(dmg) ? rd32(dmg + 0x3C) : 0);
    const uint32_t game = rd32(ADDR_gameplay);
    const int tribals_killed = valid_ptr(game) ? rd8(game + 5) : 0;
    const bool was_in = g_in_passes;
    g_in_passes = true;
    SquaddiePassesAway_orig(rdram, ctx);
    g_in_passes = was_in;
    g_rdram = rdram;
    // A Tribal: its squad member flag (bit 3 of its data's first word, squadsIsTribal of its squadron), or the game
    // counting a Tribal lost during the call.
    const uint32_t vdata = valid_ptr(victim) ? Obj::Data::get(victim) : 0;
    const bool tribal = (valid_ptr(vdata) && (rd32(vdata) & 0x8)) || (valid_ptr(game) && rd8(game + 5) != tribals_killed);
    report_kill(rdram, ctx, victim, shooter_of(hit), weapon, tribal, "passes away");
}

// mainIncreaseWeaponKills(weapon), called from elsewhere than SquaddiePassesAway: a blast or a projectile finished a
// squad member (never a Tribal: both callers skip those). The victim is the caller's $s2; the shooter, when the caller
// has one, its $s5, else the hero.
JFG_WRAP(mainIncreaseWeaponKills) {
    using namespace jfg;
    const gpr a0 = ctx->r4;
    mainIncreaseWeaponKills_orig(rdram, ctx);
    if (g_in_passes) return;
    g_rdram = rdram;
    const uint32_t victim = (uint32_t)ctx->r18;
    if (!valid_ptr(victim) || Obj::Kind::get(victim) != 0x18) {
        if (debug_kills()) std::fprintf(stderr, "[kills] counted weapon %d, no victim in $s2 (%08X)\n", (int)a0, victim);
        return;
    }
    uint32_t killer = (uint32_t)ctx->r21;
    if (!valid_ptr(killer) || (Obj::Kind::get(killer) != KIND_HERO && Obj::Kind::get(killer) != KIND_FLOYD)) {
        killer = take_shot(victim);
        killer = valid_ptr(killer) ? shooter_of(killer) : current_hero(ctx);
    } else {
        take_shot(victim);
    }
    report_kill(rdram, ctx, victim, killer, (int)(int32_t)a0, false, "counted");
}
