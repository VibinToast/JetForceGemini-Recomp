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
//
// Frog (behaviour 0x15): the object setup's data-size switch (func_8000E680_F280) has no case for it, so a frog got no
// data block (Obj::Data 0) and frogInit wrote its 0x40 bytes through a null pointer. Its wrapper gives it the block
// the setup had ready: the data pointer, and 0x44 bytes taken (0x40, rounded and plus 4, as the switch returns).
//
// Weapon slot 15 (the cut proximity mine, and the mods' weapons): jfg_weapon_registry.cpp.
//
// Heavy brute's beam (SpannerHand, laser beam 0xF4): squadsFireWeapon aims its end once, at the target's hit centre,
// and the beam never moves it (BTSBSpannerHand only moves the start to the hand); its hit sphere is radius 4 at the end
// (LaserBeamInit). So it only hit a hero standing still. Before BTSBSpannerHand the end now eases 1/8 of the way to
// the current target's hit centre each tick (overlay 3's target, D_overlay_3_0001A1E4), so it sweeps after the hero
// and can be dodged; the original then puts the beam object (and its hit) on the end. After LaserBeamInit the hit is
// widened to BEAM_RADIUS. Stock enemies with this beam: only the cut SpannerHand (decomp MSR "Hub code survey", bot2).
//
// Player bodies (jfg_assets.json "playerBodies"): a mod can play a hero in another body in its own levels, the cut
// Tribal man (0x4) and yellow ant (0x21) for example (multiplayer skins with the heroes' rigs):
//     "playerBodies": [ { "level": "level_my_room", "juno": 4, "vela": 33 } ]
// objSetupPlayers takes each hero's object id from a table (skin 0, D_800A08A0: Vela, Juno, Lupus, ship, then the
// same in the power suit); its wrapper puts the mod's ids in (both suits) while it sets up the players of that level,
// and puts the stock ids back. Only the body changes: the hero keeps their code, animations, weapons and voice.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <set>
#include <string>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "json/json.hpp"

extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace recomp { void* alloc(uint8_t* rdram, size_t size); }

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
int asset_number(const std::string& ref);              // jfg_assets.cpp
namespace weapons { void level_load(uint8_t* rdram); } // jfg_weapon_registry.cpp
}

namespace {
using namespace jfg;

// Overlay 3 (the squad code, ROM 0x01ED54E0): the animation sets (one BaddyAnimSet per baddy type) at +0x19278, the
// animations (BaddyAnim) at +0x17DB8, and the baddy type of the squad member being worked on (s16) at +0x1A1DC.
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0, ANIM_SETS = 0x19278, ANIMS = 0x17DB8, ANIM_TYPE = 0x1A1DC;
constexpr int PICKER_LIST = 16;   // the picker's list
constexpr uint8_t NO_ACTION = 0xFF;
constexpr int ORB_KIND = 0xE6;
constexpr uint8_t ORB_DAMAGE_PERCENT = 100;
constexpr int16_t FROG = 0x15;
constexpr uint32_t FROG_DATA_TAKEN = 0x44;
constexpr uint32_t SQUAD_TARGET = 0x1A1E4;        // overlay 3: the object the current squad member aims at
constexpr uint32_t SPANNER_BEAM = 0xAC;           // SquadMemberData: its beam object, while it fires
constexpr uint32_t BEAM_END = 0x10;               // LaserBeamData: Vec3f end
constexpr float BEAM_SWEEP = 1.0f / 8.0f, BEAM_RADIUS = 14.0f;
constexpr uint32_t ADDR_PLAYER_BODIES = 0x800A08A0; // s16[8]: skin 0's object id per character & 7
constexpr int VELA = 0, JUNO = 1, POWER_SUIT = 4;
constexpr int16_t STOCK_BODIES[8] = { 0x0000, 0x007E, 0x0006, 0x0044, 0x001C, 0x001D, 0x0032, 0x0044 };

struct PlayerBodies {
    std::string mod_id, level; // level: "mod:level_x", or "stock:21"
    int vela = -1, juno = -1;  // object ids, -1 = the hero's own
};
std::vector<PlayerBodies> g_player_bodies;

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

JFG_WRAP(func_8000E680_F280) {
    const uint32_t obj = (uint32_t)ctx->r4, data = (uint32_t)ctx->r5;
    func_8000E680_F280_orig(rdram, ctx);
    const uint32_t def = Obj::Def::get(obj);
    if (def == 0 || ObjDef::Behaviour::get(def) != FROG || Obj::Data::get(obj) != 0) return;
    Obj::Data::set(obj, data);
    ctx->r2 = FROG_DATA_TAKEN;
}

namespace jfg {
// The "playerBodies" of one mod's jfg_assets.json (jfg_assets.cpp, on_asset_mod_enabled).
void read_player_bodies(const nlohmann::json& j, const std::string& id) {
    for (const nlohmann::json& e : j.value("playerBodies", nlohmann::json::array())) {
        PlayerBodies b;
        b.mod_id = id;
        if (e.contains("level") && e["level"].is_number_integer()) b.level = "stock:" + std::to_string(e["level"].get<int>());
        else b.level = e.value("level", "");
        if (!b.level.empty() && b.level.find(':') == std::string::npos) b.level = id + ":" + b.level;
        const auto body = [&](const char* key) {
            return e.contains(key) && e[key].is_number_integer() && e[key].get<int>() >= 0 && e[key].get<int>() < 0x8000
                       ? e[key].get<int>() : -1;
        };
        b.vela = body("vela");
        b.juno = body("juno");
        if (b.level.empty() || (b.vela < 0 && b.juno < 0)) {
            const std::string line = "assets: " + id + ": a player body needs \"level\" and an object id for \"vela\" or "
                                     "\"juno\": " + e.dump();
            jfg_log_line("jfg_mods.log", line.c_str());
            continue;
        }
        g_player_bodies.push_back(std::move(b));
    }
}
} // namespace jfg

JFG_WRAP(objSetupPlayers) {
    jfg::weapons::level_load(rdram); // the weapon registry's slot 15 (jfg_weapon_registry.cpp)
    const int level = (int16_t)rd16(ADDR_currentLevel);
    const PlayerBodies* found = nullptr;
    for (const PlayerBodies& b : g_player_bodies) {
        const int n = b.level.rfind("stock:", 0) == 0 ? std::atoi(b.level.c_str() + 6) : jfg::asset_number(b.level);
        if (n == level) found = &b; // the last mod in the list wins
    }
    bool stock = found != nullptr;
    for (int i = 0; stock && i < 8; i++) stock = (int16_t)rd16(ADDR_PLAYER_BODIES + 2 * i) == STOCK_BODIES[i];
    if (found != nullptr && !stock) {
        static bool logged = false;
        if (!logged) jfg::jfg_log_line("jfg_mods.log", "player bodies: the body table isn't the stock one - left alone");
        logged = true;
    }
    if (!stock) {
        objSetupPlayers_orig(rdram, ctx);
        return;
    }
    const auto put = [](int character, int id) {
        if (id < 0) return;
        wr16(ADDR_PLAYER_BODIES + 2 * character, (uint16_t)id);
        wr16(ADDR_PLAYER_BODIES + 2 * (character + POWER_SUIT), (uint16_t)id);
    };
    put(VELA, found->vela);
    put(JUNO, found->juno);
    objSetupPlayers_orig(rdram, ctx);
    for (int i = 0; i < 8; i++) wr16(ADDR_PLAYER_BODIES + 2 * i, (uint16_t)STOCK_BODIES[i]);
}

JFG_WRAP(BTSBSpannerHand) {
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = Obj::Data::get(obj);
    const uint32_t beam = valid_ptr(data) ? rd32(data + SPANNER_BEAM) : 0;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    const uint32_t target = base != 0 ? rd32(base + SQUAD_TARGET) : 0;
    const uint32_t beam_data = valid_ptr(beam) ? Obj::Data::get(beam) : 0;
    const uint32_t hit = valid_ptr(target) ? Obj::Damage::get(target) : 0;
    if (valid_ptr(beam_data) && valid_ptr(hit)) {
        const float to[3] = { Damage::CentreX::get(hit), Damage::CentreY::get(hit), Damage::CentreZ::get(hit) };
        for (uint32_t k = 0; k < 3; k++) {
            const float now = rdf(beam_data + BEAM_END + 4 * k);
            wrf(beam_data + BEAM_END + 4 * k, now + (to[k] - now) * BEAM_SWEEP);
        }
    }
    BTSBSpannerHand_orig(rdram, ctx);
}

JFG_WRAP(LaserBeamInit) {
    const uint32_t obj = (uint32_t)ctx->r4;
    LaserBeamInit_orig(rdram, ctx);
    const uint32_t hit = Obj::Damage::get(obj);
    if (!valid_ptr(hit)) return;
    Damage::Radius::set(hit, BEAM_RADIUS);
    Damage::Radius2::set(hit, BEAM_RADIUS);
}

