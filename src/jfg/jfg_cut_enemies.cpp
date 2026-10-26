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
// Proximity mine (weapon 15): the fire code (func_800386B0_392B0) sends weapon 15 to the default shot, a laser bolt.
// Its wrapper fires it as a mine instead: for that one shot the hero holds weapon 11 (the mine case of the switch,
// throwing mine object 0x127, uncharged), with weapon 15's ammo in 11's slot and the charge at its uncharged value;
// then everything is put back, the ammo used and the shot counted on weapon 15. The shot's rate, spread and power
// are still weapon 15's (the caller passes its WeaponDef). The stock game never gives weapon 15.
//
// Light show (behaviour 0x7F): the object update's switch (func_8000FBF0_107F0, objControl) runs blastdoorControl for
// it, which writes through the light show's missing hit block and crashes. The switch's calls into overlays are jal
// instructions the overlays' relocations fill in as they load (TrapDanglingJump until then), so which case reaches
// which function is read from the code itself: the wrapper finds the case whose call goes to lightShowControl and
// hands the light show to the switch as that behaviour for the call. With none, it is handed over as 0x7E (a case
// that runs nothing), so it draws but doesn't move, and the targets are logged.
//
// Player bodies (jfg_assets.json "playerBodies"): a mod can play a hero in another body in its own levels, the cut
// Tribal man (0x4) and yellow ant (0x21) for example (multiplayer skins with the heroes' rigs):
//     "playerBodies": [ { "level": "level_my_room", "juno": 4, "vela": 33 } ]
// objSetupPlayers takes each hero's object id from a table (skin 0, D_800A08A0: Vela, Juno, Lupus, ship, then the
// same in the power suit); its wrapper puts the mod's ids in (both suits) while it sets up the players of that level,
// and puts the stock ids back. Only the body changes: the hero keeps their code, animations, weapons and voice.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "json/json.hpp"

extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
int asset_number(const std::string& ref);              // jfg_assets.cpp
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
constexpr uint8_t PROXIMITY_MINE = 15, TIMED_MINE = 11;
constexpr int16_t MINE_UNCHARGED = 0x20; // the timed mine's charge for mine type 0 ((charge - 0x20) >> 4 == 0)
constexpr int16_t LIGHT_SHOW = 0x7F, NO_CONTROL = 0x7E;
constexpr uint32_t LIGHT_SHOW_CONTROL = 0x823E08E0; // overlay 64
// The switch's call for each behaviour 0x7F..0x81 (a jal, 0x10 bytes apart; case = behaviour - 2).
constexpr uint32_t CONTROL_CALLS = 0x80010370;
constexpr int16_t CONTROL_CALLS_FIRST = 0x7F, CONTROL_CALLS_COUNT = 3;

uint32_t jal_target(uint32_t at) {
    const uint32_t w = rd32(at);
    return (w >> 26) == 3 ? 0x80000000u | ((w & 0x3FFFFFFu) << 2) : 0;
}
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

JFG_WRAP(func_800386B0_392B0) {
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (rd32(ADDR_mainGameMode) == 0 && c >= 0 && c < 3) ? character_ptr(c) : 0;
    if (ch == 0 || Character::CurrentWeapon::get(ch) != PROXIMITY_MINE) {
        func_800386B0_392B0_orig(rdram, ctx);
        return;
    }
    const uint32_t player = (uint32_t)ctx->r5;
    const uint32_t game = rd32(ADDR_gameplay);
    const uint16_t mine_ammo = Character::Ammo::get(ch, TIMED_MINE);
    const uint32_t mine_shots = Game::WeaponShots::get(game, TIMED_MINE);
    const int16_t charge = PlayerData::Charge::get(player);
    Character::Ammo::set(ch, TIMED_MINE, Character::Ammo::get(ch, PROXIMITY_MINE));
    Character::CurrentWeapon::set(ch, TIMED_MINE);
    PlayerData::Charge::set(player, MINE_UNCHARGED);
    func_800386B0_392B0_orig(rdram, ctx);
    Character::Ammo::set(ch, PROXIMITY_MINE, Character::Ammo::get(ch, TIMED_MINE));
    Character::Ammo::set(ch, TIMED_MINE, mine_ammo);
    Character::CurrentWeapon::set(ch, PROXIMITY_MINE);
    PlayerData::Charge::set(player, charge);
    const uint32_t shots = Game::WeaponShots::get(game, TIMED_MINE);
    if (shots != mine_shots) {
        Game::WeaponShots::set(game, PROXIMITY_MINE, Game::WeaponShots::get(game, PROXIMITY_MINE) + (shots - mine_shots));
        Game::WeaponShots::set(game, TIMED_MINE, mine_shots);
    }
}

JFG_WRAP(func_8000FBF0_107F0) {
    const uint32_t obj = (uint32_t)ctx->r4;
    if (Obj::Kind::get(obj) != LIGHT_SHOW) {
        func_8000FBF0_107F0_orig(rdram, ctx);
        return;
    }
    int16_t as = NO_CONTROL;
    for (int16_t i = 0; i < CONTROL_CALLS_COUNT; i++) {
        if (jal_target(CONTROL_CALLS + 0x10u * (uint32_t)i) == LIGHT_SHOW_CONTROL) as = (int16_t)(CONTROL_CALLS_FIRST + i);
    }
    static bool logged = false;
    if (!logged) {
        std::fprintf(stderr, "[jfg] cut objects: light show: the update's calls for 0x7F..0x81 go to %08X %08X %08X; run as 0x%X\n",
                     jal_target(CONTROL_CALLS), jal_target(CONTROL_CALLS + 0x10), jal_target(CONTROL_CALLS + 0x20), (unsigned)as);
        logged = true;
    }
    Obj::Kind::set(obj, as);
    func_8000FBF0_107F0_orig(rdram, ctx);
    Obj::Kind::set(obj, LIGHT_SHOW);
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
