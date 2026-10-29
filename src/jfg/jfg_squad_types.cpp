// Jet Force Gemini port: new squad enemy types run by mods (mod API 6; decomp MSR 12).
//
// A mod's pack appends an object type that the squad code treats like a stock baddy ("objtypes" with "like" in
// jfg_assets.json: its row, animation set, voices and per-type routine come with it) and registers it here by the
// pack's definition name (jfg_squad_type_register). Its members are then run by the mod:
// - jfg_on_squad_tick: the per-type tick. The stock types' routine (the BTSB table, step 11 of SquaddieControl, where
//   every overlay-3 global is fresh and steering hasn't run yet) is wrapped; for a member of a registered type the
//   event runs in its place (or before it, with run_stock). Members are told apart by their definition (the pointer
//   objGetObjdef returns for the pack's definition).
// - Helpers for that tick: jfg_squad_set_mission / jfg_squad_clear_mission (TryToSetMission / ClearMissionFields: a
//   mod holds a member with mission 7 under its own group), jfg_object_anim_set (objAnimSetMove: any animation of the
//   model's list, advanced by the mod in animation state 7, which nothing in overlay 3 advances).
// - Armour: jfg_squad_set_armour(member, mode). hitUpdate tests each shot with hitVectorCheck; around that call the
//   member's hit spheres get the armour bit (byte +0x30 bit 0x20: the shot stops, no damage) unless the shot comes
//   from behind (mode 1) or always (mode 2), by the shot's travel against the member's yaw as hitUpdate decides "from
//   behind". After hitUpdate the bits are cleared again (blasts skip armoured spheres too). A shot stopped by armour
//   is counted: jfg_squad_deflects(member).
// - jfg_hero_shot: a mod's projectile hits the hero as a stock shot does (decomp MSR 12e2): the hero's spheres along
//   the segment, protection and noHit spheres skipped, the hit flags and damage written for the hero code to react.
// - Its own baddy row (decomp MSR 12i part 2): the like type's row names that type's joints (head, leans, aim) and its
//   body parts, which would cut a new model's batches off on a killing hit. GetBaddyData is wrapped: for a registered
//   type's member it returns (and sets as overlay 3's current row) a copy of the like row with no body parts and the
//   mod's head, lean and aim joints. Overlay 3 reads the row only through that call and its global.
// - Its own size: a squad member's scale is its squadron's percent (+0x02) alone (the member setup,
//   func_overlay_3_0000D360), not its definition's +0x00. For a registered type the setup runs with the percent times
//   the definition's scale, so the model, its hit box (set up there from that scale) and spheres all agree.
// - Its own voices: the hurt and death voices are per type index (the like type's), read only in SquaddieControl. Around
//   a registered type's SquaddieControl call that index's voice record is zeroed (silence: the mod plays its own), and
//   put back after, so stock members of the like type keep theirs.

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" void TryToSetMission(uint8_t* rdram, recomp_context* ctx);
extern "C" void ClearMissionFields(uint8_t* rdram, recomp_context* ctx);
extern "C" void objAnimSetMove(uint8_t* rdram, recomp_context* ctx);
extern "C" void Arctanf(uint8_t* rdram, recomp_context* ctx);
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp
namespace jfg::api { uint32_t gfx_block(uint32_t bytes); } // jfg_api.cpp

namespace jfg::squad_types {
namespace {
constexpr uint32_t OBJ_DEF = 0x40, OBJ_HIT = 0x4C, OBJ_DATA = 0x68;
constexpr uint32_t HIT_HEALTH = 0x06, HIT_FLAGS8 = 0x08, HIT_FLAGSA = 0x0A, HIT_PREV = 0x18, HIT_PART = 0x31,
                   HIT_SPHERES_N = 0x32, HIT_LAST = 0x3C, HIT_SPHERES = 0x40;
constexpr uint32_t SPH_SIZE = 0x34, SPH_FLAGSA = 0x0A, SPH_X = 0x0C, SPH_RADIUS = 0x24, SPH_DEALT = 0x2C,
                   SPH_PERCENT = 0x2F, SPH_BITS = 0x30;
constexpr uint8_t SPH_ARMOUR = 0x20, SPH_NOHIT = 0x40;
constexpr uint32_t HERO_PROTECT = 0x5C8, HERO_STATE = 0x568;
// Overlay 3 (decomp MSR 12i): the current row (GetBaddyData), its type index, the voice records (8 bytes an index).
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0, O3_ROW = 0x1A1E0, O3_INDEX = 0x1A1DC, O3_VOICES = 0x19460,
                   O3_MEMBER = 0x1A1CC, O3_SQUADRON = 0x1A1D8, SQUADRON_SCALE = 0x02;
// The baddy row (0x80 bytes): body part count, head joint and its offset, lean joints and weights, aim joint.
constexpr uint32_t ROW_SIZE = 0x80, ROW_PARTS = 0x29, ROW_HEAD = 0x30, ROW_LEAN = 0x34, ROW_LEAN_WEIGHT = 0x37,
                   ROW_AIM = 0x3A;
// JfgSquadTypeDef flags.
constexpr int KEEP_VOICES = 1, KEEP_BODY_PARTS = 2;

struct Type {
    std::string name, def_name; // "mod:type", "mod:objdef_x"
    size_t mod_index = SIZE_MAX; // the mod that registered it: only it gets the type's jfg_on_squad_tick
    int def_index = -2;         // the definition's number this start (-2 not looked up yet)
    uint32_t def_ptr = 0;       // where objGetObjdef has it loaded this level
    int flags = 0;
    int head = -1, head_offset[3] = {}, lean[3] = { -1, -1, -1 }, lean_weight[3] = {}, aim = -1; // -1 keeps the like's
    uint32_t row = 0;           // its row copy (the graphics region: kept for the run)
    uint32_t row_from = 0;      // the like row it was copied from
    int index = -1;             // the like type's index (overlay 3's, for the voices)
};
std::vector<Type> g_types;

struct Armoured { uint32_t obj; int mode; int deflects; int16_t health; };
std::vector<Armoured> g_armoured;
// A mod's shot on the hero (jfg_hero_shot) between ticks: hitUpdate clears the hit flags at the start of the object
// pass, before controlPlayer reads them for the hurt reaction (flash, shake, voice), so they are written again after it,
// where hitUpdate itself marks a stock shot's hit.
struct HeroHit { uint32_t hit = 0, sphere = 0, source = 0; uint8_t part = 0, flags8 = 0; };
std::vector<HeroHit> g_hero_hits;
uint32_t g_tick_member = 0; // the member whose jfg_on_squad_tick is running (the helpers' overlay globals are its)
int g_frames = 2;           // its SquaddieControl update rate

int type_of_def(uint32_t def) {
    if (def == 0) return -1;
    for (size_t i = 0; i < g_types.size(); i++) if (g_types[i].def_ptr == def) return (int)i;
    return -1;
}

std::string read_str(uint32_t addr) {
    std::string s;
    for (uint32_t i = 0; i < 128; i++) {
        const char c = (char)rd8(addr + i);
        if (c == 0) break;
        s.push_back(c);
    }
    return s;
}

bool mod_ptr(uint32_t p) { return p >= 0x80000400u && p < 0xA0000000u; }

Armoured* armoured(uint32_t obj) {
    for (Armoured& a : g_armoured) if (a.obj == obj) return &a;
    return nullptr;
}

// Sets or clears the armour bit on every hit sphere of obj.
void set_sphere_armour(uint32_t obj, bool on) {
    const uint32_t hit = rd32(obj + OBJ_HIT);
    if (!valid_ptr(hit)) return;
    const int n = (int16_t)rd16(hit + HIT_SPHERES_N);
    const uint32_t sph = rd32(hit + HIT_SPHERES);
    if (n <= 0 || n > 64 || !valid_ptr(sph)) return;
    for (int i = 0; i < n; i++) {
        const uint32_t s = sph + SPH_SIZE * (uint32_t)i;
        const uint8_t b = rd8(s + SPH_BITS);
        wr8(s + SPH_BITS, on ? (uint8_t)(b | SPH_ARMOUR) : (uint8_t)(b & ~SPH_ARMOUR));
    }
}

uint16_t game_arctan(recomp_context* ctx, float dx, float dz) {
    const recomp_context saved = *ctx;
    ctx->f12.fl = dx;
    ctx->f14.fl = dz;
    Arctanf(g_rdram, ctx);
    const uint16_t r = (uint16_t)ctx->r2;
    *ctx = saved;
    return r;
}
} // namespace

// objGetObjdef(index) returned def: a registered type's definition, loaded for this level.
void def_loaded(int index, uint32_t def) {
    for (Type& t : g_types) {
        if (t.def_index == -2) t.def_index = jfg::asset_number(t.def_name);
        if (t.def_index == index && valid_ptr(def)) t.def_ptr = def;
    }
}

void set_frames(int frames) { g_frames = frames > 0 ? frames : 1; }

// The type's row copy from the like row: no body parts (unless kept), the mod's joints.
void make_row(Type& t, uint32_t like_row) {
    for (uint32_t i = 0; i < ROW_SIZE; i += 4) wr32(t.row + i, rd32(like_row + i));
    if (!(t.flags & KEEP_BODY_PARTS)) wr8(t.row + ROW_PARTS, 0);
    if (t.head >= 0) {
        wr8(t.row + ROW_HEAD, (uint8_t)t.head);
        for (int k = 0; k < 3; k++) wr8(t.row + ROW_HEAD + 1 + k, (uint8_t)(int8_t)std::clamp(t.head_offset[k], -128, 127));
    }
    for (int k = 0; k < 3; k++) {
        if (t.lean[k] < 0) continue;
        wr8(t.row + ROW_LEAN + k, (uint8_t)t.lean[k]);
        wr8(t.row + ROW_LEAN_WEIGHT + k, (uint8_t)std::clamp(t.lean_weight[k], 0, 255));
    }
    if (t.aim >= 0) wr8(t.row + ROW_AIM, (uint8_t)t.aim);
    t.row_from = like_row;
}

// A registered type's member's name for kill reports (jfg_on_kill): the type's name after the mod's id, its first
// letter capital ("jfg_shellback:shellback" -> "Shellback"), in place of its definition's, which is the like type's.
bool member_name(uint32_t obj, char out[16]) {
    const int type = valid_ptr(obj) ? type_of_def(rd32(obj + OBJ_DEF)) : -1;
    if (type < 0) return false;
    const std::string& name = g_types[type].name;
    const size_t colon = name.find(':');
    const std::string own = colon == std::string::npos ? name : name.substr(colon + 1);
    int n = 0;
    for (size_t i = 0; i < own.size() && n < 15; i++) {
        char c = own[i];
        if (c == '_') continue;
        if (n == 0 && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        out[n++] = c;
    }
    out[n] = 0;
    return n > 0;
}

// Around a member's SquaddieControl: its type's voices silenced (on), or the like type's put back (off).
uint8_t g_voice_saved[8];
uint32_t g_voice_at = 0;
void voices(uint32_t obj, bool on) {
    if (!on) {
        if (g_voice_at != 0) for (uint32_t i = 0; i < 8; i++) wr8(g_voice_at + i, g_voice_saved[i]);
        g_voice_at = 0;
        return;
    }
    const int type = valid_ptr(obj) ? type_of_def(rd32(obj + OBJ_DEF)) : -1;
    if (type < 0 || (g_types[type].flags & KEEP_VOICES) || g_types[type].index < 0) return;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    if (base == 0) return;
    g_voice_at = base + O3_VOICES + 8u * (uint32_t)g_types[type].index;
    for (uint32_t i = 0; i < 8; i++) {
        g_voice_saved[i] = rd8(g_voice_at + i);
        wr8(g_voice_at + i, 0);
    }
}

void level_change() {
    for (Type& t : g_types) { t.def_ptr = 0; t.row_from = 0; }
    g_armoured.clear();
    g_hero_hits.clear();
    g_tick_member = 0;
}
} // namespace jfg::squad_types

using namespace jfg;
using namespace jfg::squad_types;

extern "C" void jfg_squad_types_level_change(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram; (void)ctx;
    jfg::squad_types::level_change();
}

// The per-type routine of the stock "like" bases (index 20 and its kin): a registered type's members get the mod's
// tick. JfgSquadTick: s32 type; void* member; void* data; s32 frames; s32 run_stock.
JFG_WRAP(BTSBStandardFire) {
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int type = valid_ptr(obj) ? type_of_def(rd32(obj + OBJ_DEF)) : -1;
    if (type < 0 || !api::listened(api::Event::SquadTick)) {
        BTSBStandardFire_orig(rdram, ctx);
        return;
    }
    const gpr a0 = ctx->r4;
    int32_t run_stock;
    {
        api::Payload p(ctx, 0x14);
        wr32(p.addr + 0x00, (uint32_t)type);
        wr32(p.addr + 0x04, obj);
        wr32(p.addr + 0x08, rd32(obj + OBJ_DATA));
        wr32(p.addr + 0x0C, (uint32_t)g_frames);
        wr32(p.addr + 0x10, 0);
        g_tick_member = obj;
        if (g_types[type].mod_index != SIZE_MAX) api::fire_for_mod(rdram, ctx, api::Event::SquadTick, g_types[type].mod_index, (int32_t)p.addr);
        else api::fire(rdram, ctx, api::Event::SquadTick, (int32_t)p.addr);
        g_tick_member = 0;
        g_rdram = rdram;
        run_stock = (int32_t)rd32(p.addr + 0x10);
    }
    ctx->r4 = a0;
    if (run_stock != 0) BTSBStandardFire_orig(rdram, ctx);
}

// GetBaddyData(member): the row of its type, also overlay 3's current row. A registered type's member gets its copy.
JFG_WRAP(GetBaddyData) {
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4;
    GetBaddyData_orig(rdram, ctx);
    g_rdram = rdram;
    const uint32_t like_row = (uint32_t)ctx->r2;
    const int type = valid_ptr(obj) && valid_ptr(like_row) ? type_of_def(rd32(obj + OBJ_DEF)) : -1;
    const uint32_t base = type >= 0 ? jfg_overlay_loaded_at(OVERLAY_3_ROM) : 0;
    if (base == 0) return;
    Type& t = g_types[type];
    t.index = (int32_t)rd32(base + O3_INDEX);
    if (t.row == 0) t.row = jfg::api::gfx_block(ROW_SIZE);
    if (t.row == 0) return;
    if (t.row_from != like_row) make_row(t, like_row);
    wr32(base + O3_ROW, t.row);
    ctx->r2 = (gpr)(int32_t)t.row;
}

// The member setup (once a member): a registered type's scale is its squadron's percent times its definition's scale.
JFG_WRAP(func_overlay_3_0000D360_1EE2840) {
    g_rdram = rdram;
    const uint32_t base = jfg_overlay_loaded_at(OVERLAY_3_ROM);
    const uint32_t member = base != 0 ? rd32(base + O3_MEMBER) : 0, sqd = base != 0 ? rd32(base + O3_SQUADRON) : 0;
    const int type = valid_ptr(member) && valid_ptr(sqd) ? type_of_def(rd32(member + OBJ_DEF)) : -1;
    const float scale = type >= 0 ? rdf(g_types[type].def_ptr) : 1.0f;
    if (type < 0 || !(scale > 0.0f) || scale == 1.0f) {
        func_overlay_3_0000D360_1EE2840_orig(rdram, ctx);
        return;
    }
    const uint8_t old = rd8(sqd + SQUADRON_SCALE);
    wr8(sqd + SQUADRON_SCALE, (uint8_t)std::clamp((int)std::lround((old != 0 ? old : 100) * scale), 1, 255));
    func_overlay_3_0000D360_1EE2840_orig(rdram, ctx);
    g_rdram = rdram;
    wr8(sqd + SQUADRON_SCALE, old);
}

// hitVectorCheck(shot, shotHit, n, targets, &results, owner), once per shot from hitUpdate: armoured members take the
// shot on their armour unless it comes from behind (as hitUpdate decides it: the shot's travel against their yaw).
JFG_WRAP(hitVectorCheck) {
    g_rdram = rdram;
    const uint32_t shot = (uint32_t)ctx->r4, shot_hit = (uint32_t)ctx->r5;
    if (g_armoured.empty() || !valid_ptr(shot) || !valid_ptr(shot_hit)) {
        hitVectorCheck_orig(rdram, ctx);
        return;
    }
    const recomp_context saved = *ctx;
    const uint16_t travel = game_arctan(ctx, rdf(shot + 0x0C) - rdf(shot_hit + HIT_PREV),
                                        rdf(shot + 0x14) - rdf(shot_hit + HIT_PREV + 8));
    *ctx = saved;
    for (Armoured& a : g_armoured) {
        if (!valid_ptr(a.obj) || a.mode == 0) continue;
        const int16_t diff = (int16_t)(uint16_t)(rd16(a.obj) - travel);
        const bool back = std::abs((int)diff) >= 0x4000;
        set_sphere_armour(a.obj, a.mode == 2 || !back);
    }
    hitVectorCheck_orig(rdram, ctx);
}

// The shot pass: armour off again afterwards (blasts skip armoured spheres too), and the shots it stopped counted.
JFG_WRAP(hitUpdate) {
    g_rdram = rdram;
    for (Armoured& a : g_armoured) {
        const uint32_t hit = valid_ptr(a.obj) ? rd32(a.obj + OBJ_HIT) : 0;
        a.health = valid_ptr(hit) ? (int16_t)rd16(hit + HIT_HEALTH) : 0;
        if (valid_ptr(hit)) wr32(hit + HIT_LAST, 0);
    }
    hitUpdate_orig(rdram, ctx);
    g_rdram = rdram;
    for (const HeroHit& h : g_hero_hits) {
        if (!valid_ptr(h.hit) || !valid_ptr(h.sphere)) continue;
        wr16(h.sphere + SPH_FLAGSA, (uint16_t)(rd16(h.sphere + SPH_FLAGSA) | 0x40));
        wr16(h.hit + HIT_FLAGSA, (uint16_t)(rd16(h.hit + HIT_FLAGSA) | 0x20));
        wr32(h.hit + HIT_LAST, h.source);
        wr8(h.hit + HIT_PART, h.part);
        wr8(h.hit + HIT_FLAGS8, (uint8_t)((rd8(h.hit + HIT_FLAGS8) & ~0x3F) | h.flags8));
    }
    g_hero_hits.clear();
    for (Armoured& a : g_armoured) {
        if (!valid_ptr(a.obj)) continue;
        set_sphere_armour(a.obj, false);
        const uint32_t hit = rd32(a.obj + OBJ_HIT);
        // A shot crossed it (hit +0x0A bit 0x40, with hit +0x3C the shot) and took no health: the armour stopped it.
        // Bodies pressing together (the hero against it) also set +0x3C, with bit 0x20 only: not a deflect.
        if (valid_ptr(hit) && rd32(hit + HIT_LAST) != 0 && (rd16(hit + HIT_FLAGSA) & 0x40) &&
            (int16_t)rd16(hit + HIT_HEALTH) == a.health && a.mode != 0)
            a.deflects++;
    }
}

// --- the helpers mods import -------------------------------------------------------------------------------------
namespace {
// jfg_squad_type_register(const JfgSquadTypeDef* def) -> its type number, or -1. JfgSquadTypeDef: { const char* name,
// *def_name; s32 flags, head, head_offset[3], lean[3], lean_weight[3], aim; } (-1 joints keep the like type's).
void api_squad_type_register(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_rdram = rdram_;
    const uint32_t d = (uint32_t)ctx->r4;
    ctx->r2 = (gpr)(int32_t)-1;
    if (!mod_ptr(d)) return;
    const std::string name = mod_ptr(rd32(d)) ? read_str(rd32(d)) : "";
    const std::string def = mod_ptr(rd32(d + 4)) ? read_str(rd32(d + 4)) : "";
    if (name.empty() || def.find(':') == std::string::npos) return;
    for (size_t i = 0; i < g_types.size(); i++) {
        if (g_types[i].name == name) { ctx->r2 = (gpr)(int32_t)i; return; }
    }
    Type t;
    t.name = name;
    t.mod_index = mod_index;
    t.def_name = def;
    t.flags = (int32_t)rd32(d + 0x08);
    t.head = (int32_t)rd32(d + 0x0C);
    for (int k = 0; k < 3; k++) {
        t.head_offset[k] = (int32_t)rd32(d + 0x10 + 4 * k);
        t.lean[k] = (int32_t)rd32(d + 0x1C + 4 * k);
        t.lean_weight[k] = (int32_t)rd32(d + 0x28 + 4 * k);
    }
    t.aim = (int32_t)rd32(d + 0x34);
    t.def_index = jfg::asset_number(def);
    g_types.push_back(t);
    ctx->r2 = (gpr)(int32_t)(g_types.size() - 1);
}

// jfg_squad_set_mission(void* member, int group, int mission, int a, int b, int c) -> 1 if set. Only in its tick.
void api_squad_set_mission(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t sp = (uint32_t)ctx->r29;
    const uint32_t a = rd32(sp + 0x10), b = rd32(sp + 0x14);
    if (obj == 0 || obj != g_tick_member) { ctx->r2 = 0; return; }
    const uint32_t r = call_game(TryToSetMission, ctx, { (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7, a, b });
    ctx->r2 = (gpr)(int32_t)(r != 0);
}

// jfg_squad_clear_mission(void* member, int group, int cooldown). Only in its tick.
void api_squad_clear_mission(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    if (obj == 0 || obj != g_tick_member) return;
    call_game(ClearMissionFields, ctx, { (uint32_t)ctx->r5, (uint32_t)ctx->r6 });
}

// jfg_object_anim_set(void* obj, int anim, int phase_1000): plays animation `anim` of the object's model list from
// phase phase_1000 / 1000 (objAnimSetMove).
void api_object_anim_set(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int anim = (int32_t)ctx->r5;
    const float phase = std::clamp((int32_t)ctx->r6, 0, 1000) / 1000.0f;
    if (!valid_ptr(obj) || anim < 0 || anim > 255) return;
    uint32_t bits;
    std::memcpy(&bits, &phase, 4);
    call_game(objAnimSetMove, ctx, { obj, (uint32_t)anim, bits });
}

// jfg_squad_set_armour(void* member, int mode): 0 none, 1 all but from behind, 2 all.
void api_squad_set_armour(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int mode = std::clamp((int32_t)ctx->r5, 0, 2);
    if (!valid_ptr(obj)) return;
    Armoured* a = armoured(obj);
    if (mode == 0) {
        if (a != nullptr) {
            set_sphere_armour(obj, false);
            a->mode = 0;
        }
        return;
    }
    if (a == nullptr) { g_armoured.push_back({ obj, mode, 0, 0 }); return; }
    a->mode = mode;
}

// jfg_squad_deflects(void* member): shots its armour stopped since the last call.
void api_squad_deflects(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    Armoured* a = armoured((uint32_t)ctx->r4);
    ctx->r2 = (gpr)(int32_t)(a != nullptr ? a->deflects : 0);
    if (a != nullptr) a->deflects = 0;
}

// jfg_squad_forget(void* member): drops it from the armour list (it died or was freed).
void api_squad_forget(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    g_armoured.erase(std::remove_if(g_armoured.begin(), g_armoured.end(), [&](const Armoured& a) { return a.obj == obj; }),
                     g_armoured.end());
}

// jfg_hero_shot(const JfgHeroShot* s): { f32 from[3], to[3], radius; s32 damage (256 a unit), weapon; void* source; }
// -> 1 when it hit the first player's hero (decomp MSR 12e2).
void api_hero_shot(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t s = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (!mod_ptr(s)) return;
    const uint32_t hero = player_object(ctx);
    g_rdram = rdram_;
    if (!valid_ptr(hero) || (int16_t)rd16(hero + 0x48) != 1) return;
    const uint32_t data = rd32(hero + OBJ_DATA), hit = rd32(hero + OBJ_HIT);
    if (!valid_ptr(data) || !valid_ptr(hit)) return;
    if ((int32_t)rd32(data + HERO_PROTECT) > 0 || rd8(data + HERO_STATE) == 10) return;
    if ((int16_t)rd16(hit + HIT_HEALTH) < 0x100) return;
    const float ax = rdf(s), ay = rdf(s + 4), az = rdf(s + 8), bx = rdf(s + 12), by = rdf(s + 16), bz = rdf(s + 20);
    const float pr = rdf(s + 24);
    const int damage = (int32_t)rd32(s + 28), weapon = (int32_t)rd32(s + 32) & 0xF;
    const uint32_t source = rd32(s + 36);
    const int n = (int16_t)rd16(hit + HIT_SPHERES_N);
    const uint32_t sph = rd32(hit + HIT_SPHERES);
    if (n <= 0 || n > 64 || !valid_ptr(sph)) return;
    const float dx = bx - ax, dy = by - ay, dz = bz - az, len2 = dx * dx + dy * dy + dz * dz;
    int best = -1;
    float best_t = 2.0f;
    for (int i = 0; i < n; i++) {
        const uint32_t sp = sph + SPH_SIZE * (uint32_t)i;
        if (rd8(sp + SPH_BITS) & SPH_NOHIT) continue;
        const float cx = rdf(sp + SPH_X), cy = rdf(sp + SPH_X + 4), cz = rdf(sp + SPH_X + 8);
        const float r = rdf(sp + SPH_RADIUS) + pr;
        float t = len2 > 0.0f ? ((cx - ax) * dx + (cy - ay) * dy + (cz - az) * dz) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        const float px = ax + dx * t - cx, py = ay + dy * t - cy, pz = az + dz * t - cz;
        if (px * px + py * py + pz * pz <= r * r && t < best_t) { best = i; best_t = t; }
    }
    if (best < 0) return;
    const uint32_t sp = sph + SPH_SIZE * (uint32_t)best;
    const uint16_t yaw = rd16(hero);
    const uint16_t travel = game_arctan(ctx, dx, dz);
    g_rdram = rdram_;
    const bool back = std::abs((int)(int16_t)(uint16_t)(yaw - travel)) >= 0x4000;
    wr16(sp + SPH_FLAGSA, (uint16_t)(rd16(sp + SPH_FLAGSA) | 0x40));
    wr16(hit + HIT_FLAGSA, (uint16_t)(rd16(hit + HIT_FLAGSA) | 0x20));
    wr32(hit + HIT_LAST, valid_ptr(source) ? source : 0);
    wr8(hit + HIT_PART, (uint8_t)best);
    wr8(hit + HIT_FLAGS8, (uint8_t)((rd8(hit + HIT_FLAGS8) & ~0x3F) | weapon | (back ? 0x20 : 0)));
    const int health = (int16_t)rd16(hit + HIT_HEALTH);
    const int percent = rd8(sp + SPH_PERCENT);
    const int dealt = percent == 100 ? damage : percent == 0xFF ? health : damage * percent / 100;
    wr16(sp + SPH_DEALT, (uint16_t)((int16_t)rd16(sp + SPH_DEALT) + dealt));
    wr16(hit + HIT_HEALTH, (uint16_t)(int16_t)std::max(-0x8000, health - dealt));
    // Marked again after the next hitUpdate, for the hero code to react (it clears them before the hero reads them).
    HeroHit h;
    h.hit = hit;
    h.sphere = sp;
    h.source = valid_ptr(source) ? source : 0;
    h.part = (uint8_t)best;
    h.flags8 = (uint8_t)(weapon | (back ? 0x20 : 0));
    g_hero_hits.push_back(h);
    ctx->r2 = 1;
}
} // namespace

namespace jfg {
void register_squad_type_api() {
    recomp::overlays::register_ext_base_export("jfg_squad_type_register", api_squad_type_register);
    recomp::overlays::register_base_export("jfg_squad_set_mission", api_squad_set_mission);
    recomp::overlays::register_base_export("jfg_squad_clear_mission", api_squad_clear_mission);
    recomp::overlays::register_base_export("jfg_object_anim_set", api_object_anim_set);
    recomp::overlays::register_base_export("jfg_squad_set_armour", api_squad_set_armour);
    recomp::overlays::register_base_export("jfg_squad_deflects", api_squad_deflects);
    recomp::overlays::register_base_export("jfg_squad_forget", api_squad_forget);
    recomp::overlays::register_base_export("jfg_hero_shot", api_hero_shot);
}
} // namespace jfg
