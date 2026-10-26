// Jet Force Gemini port: fixes that let the game's cut enemies work (the enemy test hub pack, Recomp-Docs
// ENEMY_TEST_HUB.md). No stock level that can be reached uses them, so the game's own enemies are unchanged.
//
// Animation picker (overlay 3, func_overlay_3_00004614): it collects the entries of the baddy type's animation set
// that answer the wanted action (BaddyAnim Action, or AltAction) into a list of 16 on the stack, with no bound, and
// picks one by weight. A set with more than 16 entries on one action overwrites the stack: the cut insect (0xE0, 23
// entries on action 1) crashed on spawn, reading the table through a wild pointer. Its wrapper parks the entries past
// the 16th answering any one action on action 0xFF (nothing asks for it) for the call, so the picker sees 16 at most,
// and puts them back after. Sets of 16 or fewer, all of the stock enemies', are left alone.
//
// Orb creature (object 0xE6): func_80007528_8128 sets up an object's hit spheres and gives the creature's 40 orbs
// (its tentacles, placed by its own code: joint -1) a damage percent of 0, so every weapon and blast did nothing and
// it could never die. Its wrapper gives them ORB_DAMAGE_PERCENT. Its squadron's health (+0x34) then sets how many
// hits it takes.
#include <cstdint>
#include <cstdio>
#include <set>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"

extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace {
using namespace jfg;

// Overlay 3 (the squad code, ROM 0x01ED54E0): the animation sets (one BaddyAnimSet per baddy type) at +0x19278, the
// animations (BaddyAnim) at +0x17DB8, and the baddy type of the squad member being worked on (s16) at +0x1A1DC.
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0, ANIM_SETS = 0x19278, ANIMS = 0x17DB8, ANIM_TYPE = 0x1A1DC;
constexpr int PICKER_LIST = 16;   // the picker's list
constexpr uint8_t NO_ACTION = 0xFF;
constexpr int ORB_KIND = 0xE6;
constexpr uint8_t ORB_DAMAGE_PERCENT = 100;

struct Parked {
    uint32_t addr;
    uint8_t value;
};

// Parks the entries past the 16th answering one action; returns what it changed, to put back.
std::vector<Parked> park_overfull_actions(uint32_t base) {
    std::vector<Parked> parked;
    const int16_t type = (int16_t)rd16(base + ANIM_TYPE);
    if (type < 0) return parked;
    const uint32_t set = base + ANIM_SETS + (uint32_t)type * BaddyAnimSet::Size;
    const uint32_t first = BaddyAnimSet::First::get(set), count = BaddyAnimSet::Count::get(set);
    if (count <= PICKER_LIST) return parked;
    const uint32_t anims = base + ANIMS + first * BaddyAnim::Size;
    int answers[256] = {};
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t a = anims + i * BaddyAnim::Size;
        const uint8_t action = BaddyAnim::Action::get(a), alt = BaddyAnim::AltAction::get(a);
        answers[action]++;
        if (alt != action) answers[alt]++;
    }
    static std::set<int> logged;
    int seen[256] = {};
    for (uint32_t i = 0; i < count; i++) {
        const uint32_t a = anims + i * BaddyAnim::Size;
        const uint8_t action = BaddyAnim::Action::get(a), alt = BaddyAnim::AltAction::get(a);
        const int values = alt != action ? 2 : 1; // an entry answering one action through both counts once
        for (int k = 0; k < values; k++) {
            const uint8_t v = k == 0 ? action : alt;
            if (v == NO_ACTION || answers[v] <= PICKER_LIST) continue;
            if (++seen[v] <= PICKER_LIST) continue;
            if (BaddyAnim::Action::get(a) == v) {
                parked.push_back({ a + BaddyAnim::Action::offset, v });
                BaddyAnim::Action::set(a, NO_ACTION);
            }
            if (BaddyAnim::AltAction::get(a) == v) {
                parked.push_back({ a + BaddyAnim::AltAction::offset, v });
                BaddyAnim::AltAction::set(a, NO_ACTION);
            }
            if (logged.insert(type * 256 + v).second) {
                std::fprintf(stderr, "[jfg] cut enemies: baddy type %d has %d animations for action 0x%X; the picker takes the first %d\n",
                             (int)type, answers[v], (unsigned)v, PICKER_LIST);
            }
        }
    }
    return parked;
}
} // namespace

JFG_WRAP(func_overlay_3_00004614_1ED9AF4) {
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    const std::vector<Parked> parked = base != 0 ? park_overfull_actions(base) : std::vector<Parked>{};
    func_overlay_3_00004614_1ED9AF4_orig(rdram, ctx);
    for (const Parked& p : parked) wr8(p.addr, p.value);
}

JFG_WRAP(func_80007528_8128) {
    const uint32_t obj = (uint32_t)ctx->r4;
    const int kind = (int)(int32_t)ctx->r6;
    func_80007528_8128_orig(rdram, ctx);
    if (kind != ORB_KIND) return;
    const uint32_t damage = Obj::Damage::get(obj);
    if (damage == 0) return;
    const uint32_t spheres = Damage::Spheres::get(damage);
    const int16_t n = Damage::SphereCount::get(damage);
    for (int16_t i = 0; i < n; i++) {
        const uint32_t s = spheres + (uint32_t)i * HitSphere::Size;
        if (HitSphere::Joint::get(s) == -1 && HitSphere::DamagePercent::get(s) == 0) {
            HitSphere::DamagePercent::set(s, ORB_DAMAGE_PERCENT);
        }
    }
}
