// Jet Force Gemini port: kills for mods (mod_api/jfg_api.h, jfg_on_kill).
//
// The game ends a squad member (an enemy or a Tribal) in several places, and jfg_on_kill comes from each:
// - shot down: SquaddiePassesAway (overlay 3), which counts the kill in the game's weapon stats
//   (mainIncreaseWeaponKills with its damage's low nibble: the weapon that killed it);
// - blown apart by a blast (explosionHitTick), finished by some projectiles (hitUpdate) or beheaded by a shuriken:
//   counted by mainIncreaseWeaponKills straight from those;
// - a Tribal lost: counted by neither; the game marks it lost (mainSetTribalStatus) while SquaddieControl updates it.
// The game keeps no shooter for most kills (the damage's last hitter, +0x3C, is set only by a decapitation), so the
// port remembers the last one each squad member was shot by: squadsPlayerShootsSquaddie(member, shooter), called by
// the hits (hitUpdate), the flamethrower and decapitations (its wrapper is in jfg_squad_cheat.cpp). Names are the
// objects' own definition names from the player's ROM (ObjDef +4, 16 bytes): the port and mods ship none.
//
// Mods' weapons hurt in their own ways (setting health, blasts, the game's explosions), which the game counts under
// whatever weapon is in hand. A mod credits what its weapon hurt (jfg_weapon_credit: victim, weapon id, for how
// long); a death within that time reports that weapon, and the mod's hero as the killer when the game knows none.
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <chrono>
#include <cstring>
#include <unordered_map>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {
namespace squad_types { bool member_name(uint32_t obj, char out[16]); } // jfg_squad_types.cpp
bool drops_active(); // jfg_drops.cpp
void drops_roll(uint8_t* rdram, recomp_context* ctx, uint32_t victim, uint32_t killer, int killer_kind, bool tribal);
void register_drop_api();

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
uint32_t g_last_shot_victim = 0; // the squad member shot last (a shuriken's decapitation counts its kill after)
uint32_t g_squaddie = 0;          // the squad member SquaddieControl is updating
uint32_t g_blast_shooters[16] = {}; // the squad member whose projectile set off each blast record (explosionAddBlast)

} // namespace

// From SquaddieControl's wrapper (jfg_frame_rate.cpp): the member being updated; returns the one before.
uint32_t kills_set_squaddie(uint32_t obj) {
    const uint32_t before = g_squaddie;
    g_squaddie = obj;
    return before;
}

uint32_t kills_squaddie() { return g_squaddie; } // jfg_squad_multiplier.cpp

namespace {
void remember(uint32_t victim, uint32_t shooter) {
    for (Shot& s : g_shots) {
        if (s.victim == victim) { s.shooter = shooter; return; }
    }
    g_shots[g_next_shot] = { victim, shooter };
    g_next_shot = (g_next_shot + 1) % SHOTS;
}
uint32_t shooter_of(uint32_t hit);
} // namespace

// From squadsPlayerShootsSquaddie's wrapper (jfg_squad_cheat.cpp).
void kills_remember_shot(uint32_t victim, uint32_t shooter) {
    g_last_shot_victim = victim;
    remember(victim, shooter);
}

// After the game's shot pass (hitUpdate's wrapper, jfg_squad_types.cpp): squadsPlayerShootsSquaddie only records the
// heroes' shots, so a squad member another one shot (enemies' friendly fire) had no shooter. A shot that crossed a
// member this pass is in its hit block (+0x3C, with +0x0A bit 0x40; bodies pressing together set bit 0x20 only):
// when it came from another squad member, that member is its shooter.
void kills_after_hit_pass() {
    constexpr int16_t KIND_SQUAD_MEMBER = 0x18;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    if (!valid_ptr(list) || count <= 0 || count > 4096) return;
    for (int i = 0; i < count; i++) {
        const uint32_t obj = rd32(list + 4u * (uint32_t)i);
        if (!valid_ptr(obj) || Obj::Kind::get(obj) != KIND_SQUAD_MEMBER) continue;
        const uint32_t dmg = Obj::Damage::get(obj);
        if (!valid_ptr(dmg) || !(rd16(dmg + 0x0A) & 0x40)) continue;
        const uint32_t shot = rd32(dmg + 0x3C);
        if (!valid_ptr(shot)) continue;
        const uint32_t shooter = shooter_of(shot);
        if (valid_ptr(shooter) && shooter != obj && Obj::Kind::get(shooter) == KIND_SQUAD_MEMBER) remember(obj, shooter);
    }
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
    if (squad_types::member_name(obj, out)) return; // a mod's enemy type: its own name, not the like type's
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

// Kills mods credit to their weapons (jfg_weapon_credit): victim -> weapon id, shooter, until when.
struct Credit {
    int weapon;
    uint32_t shooter;
    std::chrono::steady_clock::time_point until;
};
std::unordered_map<uint32_t, Credit> g_credits;
constexpr size_t MAX_CREDITS = 256;

uint32_t current_hero(recomp_context* ctx) {
    const uint32_t hero = player_object(ctx);
    return valid_ptr(hero) ? hero : 0;
}

// jfg_on_kill (and the debug log) for one death.
void report_kill(uint8_t* rdram, recomp_context* ctx, uint32_t victim, uint32_t killer, int weapon, bool tribal,
                 const char* where) {
    g_rdram = rdram;
    if (!valid_ptr(victim) || (!api::listened(api::Event::Kill) && !debug_kills() && !drops_active())) return;
    if (auto c = g_credits.find(victim); c != g_credits.end()) {
        if (std::chrono::steady_clock::now() <= c->second.until) {
            weapon = c->second.weapon;
            if (!valid_ptr(killer) || Obj::Kind::get(killer) != KIND_HERO) killer = c->second.shooter;
        }
        g_credits.erase(c);
    }
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
    if (!api::listened(api::Event::Kill)) {
        drops_roll(rdram, ctx, victim, killer, killer_kind, tribal);
        return;
    }
    const gpr a0 = ctx->r4;
    {
        api::Payload p(ctx, 0x38);
        wr32(p.addr + 0x00, victim);
        wr32(p.addr + 0x04, valid_ptr(killer) ? killer : 0);
        wr32(p.addr + 0x08, (uint32_t)killer_kind);
        wr32(p.addr + 0x0C, (uint32_t)weapon); // a mod's credited weapon: its id (16 up)
        wr32(p.addr + 0x10, tribal ? 1u : 0u);
        wr32(p.addr + 0x14, 0);
        for (uint32_t i = 0; i < 16; i++) {
            wr8(p.addr + 0x18 + i, (uint8_t)vname[i]);
            wr8(p.addr + 0x28 + i, (uint8_t)kname[i]);
        }
        api::fire(rdram, ctx, api::Event::Kill, (int32_t)p.addr);
    }
    ctx->r4 = a0;
    drops_roll(rdram, ctx, victim, killer, killer_kind, tribal); // mods' item drops (jfg_drops.cpp)
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
// squad member (never a Tribal: both callers skip those), the victim in the caller's $s2; or a shuriken took a squad
// member's head (weapon 9), right after squadsDecapitateSquaddie shot it: the member shot last. The shooter: a
// blast's (explosionHitTick: $s1 is the blast, explosionAddBlast's owner at +0x20; an enemy's grenade has its
// thrower or none, never the hero), else a projectile's (hitUpdate's $s5), else the hero.
JFG_WRAP(mainIncreaseWeaponKills) {
    using namespace jfg;
    const gpr a0 = ctx->r4;
    mainIncreaseWeaponKills_orig(rdram, ctx);
    if (g_in_passes) return;
    g_rdram = rdram;
    uint32_t victim = (uint32_t)ctx->r18;
    uint32_t killer = (uint32_t)ctx->r21;
    constexpr uint32_t ADDR_BLASTS = 0x800F6B60, BLAST_SIZE = 0x24, BLASTS = 16, BLAST_OWNER = 0x20;
    const uint32_t blast = (uint32_t)ctx->r17;
    const bool from_blast = blast >= ADDR_BLASTS && blast < ADDR_BLASTS + BLAST_SIZE * BLASTS &&
                            (blast - ADDR_BLASTS) % BLAST_SIZE == 0;
    if (from_blast && valid_ptr(victim) && Obj::Kind::get(victim) == 0x18) {
        uint32_t owner = rd32(blast + BLAST_OWNER);
        if (!valid_ptr(owner)) owner = g_blast_shooters[(blast - ADDR_BLASTS) / BLAST_SIZE]; // an enemy's (explosionAddBlast)
        take_shot(victim);
        report_kill(rdram, ctx, victim, valid_ptr(owner) ? shooter_of(owner) : 0u, (int)(int32_t)a0, false, "blast");
        return;
    }
    if (!valid_ptr(victim) || Obj::Kind::get(victim) != 0x18) {
        victim = g_last_shot_victim;
        killer = 0;
        if (!valid_ptr(victim) || Obj::Kind::get(victim) != 0x18) {
            if (debug_kills()) std::fprintf(stderr, "[kills] counted weapon %d, no victim\n", (int)a0);
            return;
        }
    }
    if (!valid_ptr(killer) || (Obj::Kind::get(killer) != KIND_HERO && Obj::Kind::get(killer) != KIND_FLOYD)) {
        killer = take_shot(victim);
        killer = valid_ptr(killer) ? shooter_of(killer) : current_hero(ctx);
    } else {
        take_shot(victim);
    }
    report_kill(rdram, ctx, victim, killer, (int)(int32_t)a0, false, "counted");
}

// explosionAddBlast(x $f12, y $f14, z a2, radius a3, ...; weapon sp+0x18, owner sp+0x1C) fills one of the 16 blast
// records (D_800F6B60). The game gives an owner only for the heroes' blasts; an enemy's grenade or rocket goes off
// with none. Its projectile is still in the object list while it explodes: the nearest object at the blast whose
// shooter is a squad member is noted as that record's thrower, for the kills the blast makes.
JFG_WRAP(explosionAddBlast) {
    using namespace jfg;
    g_rdram = rdram;
    const float x = ctx->f12.fl, y = ctx->f14.fl;
    float z;
    { const uint32_t bits = (uint32_t)ctx->r6; std::memcpy(&z, &bits, 4); }
    const uint32_t owner = rd32((uint32_t)ctx->r29 + 0x1C);
    explosionAddBlast_orig(rdram, ctx);
    g_rdram = rdram;
    constexpr uint32_t ADDR_BLASTS = 0x800F6B60, BLAST_SIZE = 0x24, BLASTS = 16;
    int record = -1;
    for (uint32_t i = 0; i < BLASTS && record < 0; i++) {
        const uint32_t b = ADDR_BLASTS + i * BLAST_SIZE;
        if (rdf(b) == x && rdf(b + 4) == y && rdf(b + 8) == z && rd32(b + 0x20) == owner) record = (int)i;
    }
    if (record < 0) return;
    g_blast_shooters[record] = 0;
    if (valid_ptr(owner)) return;
    constexpr float REACH = 90.0f;
    float best = REACH * REACH;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    if (!valid_ptr(list) || count <= 0 || count > 4096) return;
    for (int i = 0; i < count; i++) {
        const uint32_t o = rd32(list + 4u * (uint32_t)i);
        if (!valid_ptr(o)) continue;
        const int16_t kind = Obj::Kind::get(o);
        if (kind == 0x18 || kind == KIND_HERO || kind == KIND_FLOYD) continue;
        const float dx = Obj::PosX::get(o) - x, dy = Obj::PosY::get(o) - y, dz = Obj::PosZ::get(o) - z;
        const float d = dx * dx + dy * dy + dz * dz;
        if (d >= best) continue;
        const uint32_t s = shooter_of(o);
        if (valid_ptr(s) && s != o && Obj::Kind::get(s) == 0x18) { best = d; g_blast_shooters[record] = s; }
    }
}

// mainSetTribalStatus(tribal, status): status 2 is a Tribal lost. The game counts no kill for it; the Tribal is the
// squad member SquaddieControl is updating, and its shooter the one that shot it last.
JFG_WRAP(mainSetTribalStatus) {
    using namespace jfg;
    g_rdram = rdram;
    const int status = (int32_t)ctx->r5;
    const uint32_t game = rd32(ADDR_gameplay);
    const int lost = valid_ptr(game) ? rd8(game + 5) : 0;
    mainSetTribalStatus_orig(rdram, ctx);
    g_rdram = rdram;
    if (status != 2 || !valid_ptr(game) || rd8(game + 5) == lost) return;
    const uint32_t victim = g_squaddie;
    if (!valid_ptr(victim)) {
        if (debug_kills()) std::fprintf(stderr, "[kills] Tribal lost, no squad member being updated\n");
        return;
    }
    const uint32_t dmg = Obj::Damage::get(victim);
    const int weapon = valid_ptr(dmg) ? (rd8(dmg + 0x08) & 0xF) : -1;
    const uint32_t shot = take_shot(victim);
    const uint32_t hit = valid_ptr(shot) ? shot : (valid_ptr(dmg) ? rd32(dmg + 0x3C) : 0);
    report_kill(rdram, ctx, victim, shooter_of(hit), weapon, true, "Tribal lost");
}

namespace {
// jfg_weapon_credit(void* victim, int weapon, void* shooter, int ticks): if the victim (a squad member) dies within
// ticks (1/60 s, at most 10 s), jfg_on_kill reports that weapon (any id: a mod's own) and shooter (when the game
// knows no hero who did it). Credit again to renew it.
void weapon_credit(uint8_t* rdram_, recomp_context* ctx) {
    jfg::g_rdram = rdram_;
    const uint32_t victim = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r6;
    const int weapon = (int32_t)ctx->r5, ticks = std::clamp((int32_t)ctx->r7, 1, 600);
    if (!jfg::valid_ptr(victim) || weapon < 0) return;
    const auto now = std::chrono::steady_clock::now();
    if (jfg::g_credits.size() >= jfg::MAX_CREDITS) { // the oldest go first: expired ones
        for (auto it = jfg::g_credits.begin(); it != jfg::g_credits.end();) it = it->second.until < now ? jfg::g_credits.erase(it) : std::next(it);
        if (jfg::g_credits.size() >= jfg::MAX_CREDITS) jfg::g_credits.clear();
    }
    jfg::g_credits[victim] = { weapon, jfg::valid_ptr(shooter) ? shooter : 0u, now + std::chrono::milliseconds(ticks * 1000 / 60) };
}
} // namespace

namespace jfg {
void register_kill_api() {
    recomp::overlays::register_base_export("jfg_weapon_credit", weapon_credit);
    register_drop_api();
}
} // namespace jfg

namespace jfg {
// From C++ (jfg_pulls.cpp): as jfg_weapon_credit.
void credit_weapon(uint8_t* rdram, uint32_t victim, int weapon, uint32_t shooter, int ticks) {
    recomp_context c{};
    c.r4 = (gpr)(int32_t)victim; c.r5 = (gpr)weapon; c.r6 = (gpr)(int32_t)shooter; c.r7 = (gpr)ticks;
    weapon_credit(rdram, &c);
}
} // namespace jfg
