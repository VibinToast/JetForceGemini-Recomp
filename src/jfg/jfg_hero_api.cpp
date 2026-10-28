// Jet Force Gemini port: the hero part of the mod API (API version 4): a mod can take over a hero's control, read the
// controller and camera, read the level's collision triangles and floors, and hurt objects. Built for the custom-hero
// test (Recomp-Docs MARIO_TEST.md); the layouts are in mod_api/jfg_api.h. Version 5 added what the official mods use
// (Recomp-Docs OFFICIAL_MODS_PLAN.md): toggled buttons, a hero's dash and protection, Floyd and his shots, setting
// enemies alight or electrocuting them, ground fire, the flamethrower's and shocker's reach.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" void controlReadJoypad(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackNearestIntersection(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackPolyHeight(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackGetBlock(uint8_t* rdram, recomp_context* ctx);
extern "C" void mainGetGameCharacter(uint8_t* rdram, recomp_context* ctx);
extern "C" void squadsIsTribal(uint8_t* rdram, recomp_context* ctx);
extern "C" void frontFlashScreen(uint8_t* rdram, recomp_context* ctx);
extern "C" void camSetFOV(uint8_t* rdram, recomp_context* ctx);
extern "C" void controlSelectNextWeapon(uint8_t* rdram, recomp_context* ctx);
extern "C" void controlSelectPreviousWeapon(uint8_t* rdram, recomp_context* ctx);
extern "C" void objSetupObject(uint8_t* rdram, recomp_context* ctx);
extern "C" void objFreeObject(uint8_t* rdram, recomp_context* ctx);
extern "C" void explosionStart(uint8_t* rdram, recomp_context* ctx);
extern "C" void camStartShake(uint8_t* rdram, recomp_context* ctx);
extern "C" void explosionAddBlast(uint8_t* rdram, recomp_context* ctx);
extern "C" void shockwaveStart(uint8_t* rdram, recomp_context* ctx);
extern "C" void hitsCheckLineOfSight(uint8_t* rdram, recomp_context* ctx);
extern "C" void SetSquaddieOnFire(uint8_t* rdram, recomp_context* ctx);
extern "C" void squadsElectrocuteSquaddie(uint8_t* rdram, recomp_context* ctx);
extern "C" void squadsPlayerShootsSquaddie(uint8_t* rdram, recomp_context* ctx);
extern "C" void amSndPlay(uint8_t* rdram, recomp_context* ctx);
extern "C" void amSndPlayXYZ(uint8_t* rdram, recomp_context* ctx);
extern "C" void charSpeak(uint8_t* rdram, recomp_context* ctx);
extern "C" void func_overlay_15_00005180_1F18B00(uint8_t* rdram, recomp_context* ctx); // Vela: set her move (obj, data, move, frame)
extern "C" void func_overlay_16_00004F78_1F22F90(uint8_t* rdram, recomp_context* ctx); // Juno: the same
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp
namespace jfg { bool hud_in_corners(); } // jfg_hud_corners.cpp
namespace jfg { int asset_number(const std::string& ref); } // jfg_assets.cpp
namespace jfg { void overlay_size_now(float* out); } // jfg_overlay.cpp

namespace jfg::api {
// A camera a mod set this tick (jfg_camera_set), applied by the camera step's wrapper.
struct CameraSet { float x, y, z, fx, fy, fz; int16_t roll; float fov; bool pending; };
CameraSet g_camera_set;
int g_camera_ticks = 0; // camera steps left in which a mod's camera counts as active (set again each tick)
uint32_t camera_ptr();

namespace {
constexpr uint32_t ADDR_track = 0x800A0D60;      // Track*, the level's track (track.c)
constexpr uint32_t ADDR_controlcam = 0x800F6DC0; // Camera*, the camera the hero's control steers by
constexpr uint32_t ADDR_controlXjoy = 0x800F6DAC, ADDR_controlYjoy = 0x800F6DB0; // s32, filled by controlReadJoypad
constexpr int CHARACTERS = 3; // Vela, Juno, Lupus

// The hero's per-tick data (Obj::Data of a hero, ControlPlayer in the decomp's charControl.h).
namespace Hero {
using Speed    = Field<float, 0x04>;    // forward speed, read by the follow camera
using OldX     = Field<float, 0x3C>;    // the position before this tick's move
using OldY     = Field<float, 0x40>;
using OldZ     = Field<float, 0x44>;
using BodyYaw  = Field<uint16_t, 0x11C>;
using PushX      = Field<float, 0x6C>;     // a push (knockback): world x and z per 1/60 s tick, the move functions
using PushZ      = Field<float, 0x70>;     // add it while PushTicks runs, and walls stop it as they stop walking
using PushTicks  = Field<int8_t, 0x149>;
using State      = Field<uint8_t, 0x568>;  // low 4 bits: 0 walk/run, 1 stand, 2 crouch, 3 air, 4 swim, 5/11 aim, 10 dead
using SideMoving = Field<uint8_t, 0x56C>;  // set while the hero moves sideways (a strafe or a roll)
using Roll       = Field<uint8_t, 0x584>;  // a roll under way: 1 left, 2 right (3, 4 crouched); its speed follows a curve
using Protected  = Field<int16_t, 0x5C8>;  // 60 Hz ticks of protection: damage is dropped, enemies don't aim at it
using Floyd      = Field<uint32_t, 0x50C>; // Floyd's object while he follows this hero
using FloydFlown = Field<uint8_t, 0x10D>;  // bit 0: the player flies Floyd
using Beam       = Field<uint32_t, 0x560>; // the shocker's beam object
using Flames     = Field<uint32_t, 0x564>; // the flamethrower's particles
}
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0;         // the squads' code (overlay 3)
constexpr uint32_t ADDR_cooperativeGame = 0x800A4FC8;  // u8: a second player flies Floyd
constexpr int16_t KIND_SQUAD_MEMBER = 0x18;
// The shocker's beam (Obj::Data of object 0xB6, behaviour 0x98).
namespace Beam {
using EndX   = Field<float, 0xA48>;
using EndY   = Field<float, 0xA4C>;
using EndZ   = Field<float, 0xA50>;
using Active = Field<uint8_t, 0xA7D>;
}
// The flamethrower's block (Hero::Flames): 30 particles of 0x34 bytes from +0x08.
constexpr uint32_t FLAME_FIRST = 0x08, FLAME_SIZE = 0x34, FLAME_X = 0x00, FLAME_LIFE = 0x2C;
constexpr int FLAMES = 30;

uint32_t hero_data(uint32_t hero) {
    if (!valid_ptr(hero) || Obj::Kind::get(hero) != 1) return 0; // behaviour 1: a hero
    const uint32_t data = Obj::Data::get(hero);
    return valid_ptr(data) ? data : 0;
}

bool is_squad_member(uint32_t obj) {
    return valid_ptr(obj) && Obj::Kind::get(obj) == KIND_SQUAD_MEMBER && valid_ptr(Obj::Data::get(obj)) &&
           jfg_overlay_loaded_at(OVERLAY_3_ROM) != 0;
}

// A spawn entry for objSetupObject, in the gap call_game leaves below $sp (objSetupObject copies it).
uint32_t entry_at(recomp_context* ctx) { return ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; }

int16_t angle(float y, float x) { return (int16_t)(int32_t)std::lround(std::atan2(y, x) * (32768.0f / 3.14159265f)); }


bool g_takeover[CHARACTERS] = {};
bool g_game_control[CHARACTERS] = {}; // a taken-over hero runs the game's control code (jfg_hero_game_control)
bool g_hide_health[CHARACTERS] = {};

// Blast damage taken (func_8002AA7C_2B67C's wrapper), by damage data, until a mod reads it: the latest 16.
struct BlastTaken { uint32_t dmg; int damage; };
BlastTaken g_blasts[16];
int g_blast_count = 0;

// JfgInput
constexpr uint32_t IN_STICK_X = 0x00, IN_STICK_Y = 0x04, IN_RAW_X = 0x08, IN_RAW_Y = 0x0C, IN_HELD = 0x10,
                   IN_PRESSED = 0x14, IN_CAMERA_YAW = 0x18;
// JfgTriangle
constexpr uint32_t TRI_VERTS = 0x00, TRI_FLAGS = 0x14, TRI_TEXTURE = 0x18, TRI_SIZE = 0x1C;
// JfgFloor
constexpr uint32_t FL_X = 0x00, FL_Z = 0x04, FL_MASK = 0x08, FL_Y = 0x0C, FL_FLAGS = 0x10;
// JfgDamageSphere
constexpr uint32_t DS_X = 0x00, DS_Y = 0x04, DS_Z = 0x08, DS_RADIUS = 0x0C, DS_DAMAGE = 0x10, DS_SOURCE = 0x14,
                   DS_TARGETS = 0x18, DS_PROPS_HIT = 0x1C;
constexpr uint32_t TARGET_HEALTH = 1, TARGET_PROPS = 2; // JfgDamageSphere targets (0 = both)

} // namespace
uint32_t camera_ptr() {
    const uint32_t cam = rd32(ADDR_controlcam);
    if (valid_ptr(cam)) return cam;
    return ADDR_cameraActorArray + Camera::Size * (rd32(ADDR_cameraNo) & 3u);
}

namespace {
// jfg_hero_takeover(int character, int on): while on, the game's control code for that hero doesn't run;
// jfg_on_hero_control fires in its place. Only Juno (1) so far.
void hero_takeover(uint8_t*, recomp_context* ctx) {
    const int character = (int)ctx->r4;
    if (character >= 0 && character < CHARACTERS) g_takeover[character] = ctx->r5 != 0;
}

// jfg_hero_game_control(int character, int on): while on, a taken-over hero runs the game's own control code again
// (after jfg_on_hero_control, which still fires each tick, so the mod decides tick by tick): its aiming and weapons,
// its camera and its moves, from where the mod left the hero.
void hero_game_control(uint8_t*, recomp_context* ctx) {
    const int character = (int)ctx->r4;
    if (character >= 0 && character < CHARACTERS) g_game_control[character] = ctx->r5 != 0;
}

// jfg_hero_select_weapon(void* hero, int direction): the hero takes its next weapon (direction 1) or the one before
// (-1), as the game's weapon buttons do (controlSelectNextWeapon / controlSelectPreviousWeapon: the weapon list
// shows it).
void hero_select_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const int direction = (int)ctx->r5;
    const uint32_t data = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    if (!valid_ptr(data) || direction == 0) return;
    const recomp_context saved = *ctx;
    call_game(direction > 0 ? controlSelectNextWeapon : controlSelectPreviousWeapon, ctx, { obj, data });
    *ctx = saved;
    // The control code puts back the weapon it last chose (+0x585) when the one in hand differs, as its own buttons
    // set it: it is the new one now.
    g_rdram = rdram_;
    const int c = rd8(data + 1) & 3;
    const uint32_t ch = c < 3 ? character_ptr(c) : 0;
    if (valid_ptr(ch)) wr8(data + 0x585, Character::CurrentWeapon::get(ch));
}

// jfg_input_get(JfgInput* out): the first player's controller as the hero's control reads it this tick.
void input_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t out = (uint32_t)ctx->r4;
    if (out == 0) return;
    const uint32_t pad = ADDR_sControllerCurrData + 6u * (rd8(ADDR_sPlayerID) & 3u);
    wr32(out + IN_STICK_X, rd32(ADDR_controlXjoy));
    wr32(out + IN_STICK_Y, rd32(ADDR_controlYjoy));
    wr32(out + IN_RAW_X, (uint32_t)(int32_t)(int8_t)rd8(pad + 2));
    wr32(out + IN_RAW_Y, (uint32_t)(int32_t)(int8_t)rd8(pad + 3));
    wr32(out + IN_HELD, rd16(pad));
    wr32(out + IN_PRESSED, rd16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u)));
    wr32(out + IN_CAMERA_YAW, Camera::Yaw::get(camera_ptr()));
}

// jfg_camera_get(JfgCameraView* out): where the camera the hero steers by is, and its yaw and pitch.
void camera_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t out = (uint32_t)ctx->r4;
    if (out == 0) return;
    const uint32_t cam = camera_ptr();
    wrf(out + 0x00, Camera::PosX::get(cam));
    wrf(out + 0x04, Camera::PosY::get(cam));
    wrf(out + 0x08, Camera::PosZ::get(cam));
    wr32(out + 0x0C, Camera::Yaw::get(cam));
    wr32(out + 0x10, Camera::Pitch::get(cam));
}

// jfg_world_to_screen(const float* world, float* out): where a point of the level shows on screen, in the overlay's
// units (jfg_overlay_size): out[0] x, out[1] y, out[2] its distance in front of the camera, out[3] overlay units per
// level unit at that distance (to size a picture drawn there). 1 if it is in front of the camera, else 0 (out not
// filled). It uses the game's view-projection matrix of the frame (D_800FAAF0, camera.c: view x perspective, row
// vectors), which maps the middle 4:3 of the picture: RT64's widescreen only widens it at the sides, so the 4:3 half
// width (160 units of 240 high) is the scale in both modes.
void world_to_screen(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    constexpr uint32_t VIEW_PROJ = 0x800FAAF0, PERSPECTIVE = 0x800FAA70;
    const uint32_t in = (uint32_t)ctx->r4, out = (uint32_t)ctx->r5;
    ctx->r2 = 0;
    if (in == 0 || out == 0 || rd32(ADDR_mainGameMode) != 0) return;
    const float v[4] = { rdf(in), rdf(in + 4), rdf(in + 8), 1.0f };
    float clip[4];
    for (int j = 0; j < 4; j++) {
        clip[j] = 0.0f;
        for (int i = 0; i < 4; i++) clip[j] += v[i] * rdf(VIEW_PROJ + (uint32_t)(i * 16 + j * 4));
    }
    if (clip[3] < 1.0f) return;
    float screen[2];
    jfg::overlay_size_now(screen);
    wrf(out + 0, screen[0] * 0.5f + clip[0] / clip[3] * 160.0f);
    wrf(out + 4, 120.0f - clip[1] / clip[3] * 120.0f);
    wrf(out + 8, clip[3]);
    wrf(out + 12, rdf(PERSPECTIVE + 5 * 4) * 120.0f / clip[3]); // [1][1]: the vertical scale
    ctx->r2 = 1;
}

// Calls fn(segment, batch flags, texture, first face, end face, first vertex) for every batch of the level's track.
template <typename F> void each_batch(F fn) {
    const uint32_t track = rd32(ADDR_track);
    if (!valid_ptr(track)) return;
    const uint32_t segments = Track::Segments::get(track);
    const int count = Track::SegmentCount::get(track);
    if (!valid_ptr(segments) || count <= 0 || count > 1024) return;
    for (int s = 0; s < count; s++) {
        const uint32_t seg = segments + TrackSegment::Size * (uint32_t)s;
        const uint32_t batches = TrackSegment::Batches::get(seg);
        const int n = TrackSegment::BatchCount::get(seg);
        if (!valid_ptr(batches) || n <= 0 || n > 4096) continue;
        for (int b = 0; b < n; b++) {
            const uint32_t batch = batches + TrackBatch::Size * (uint32_t)b;
            fn(seg, TrackBatch::Flags::get(batch), (uint32_t)TrackBatch::Texture::get(batch),
               TrackBatch::FaceOffset::get(batch), TrackBatch::FaceOffset::get(batch + TrackBatch::Size),
               TrackBatch::VertOffset::get(batch));
        }
    }
}

// jfg_level_triangles(JfgTriangle* out, int first, int max, int ignore_mask): copies the level's collision triangles
// (batches with none of ignore_mask's flags) from index `first`, at most `max`; returns how many there are in all.
// out = 0 only counts.
void level_triangles(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t out = (uint32_t)ctx->r4;
    const int first = (int)ctx->r5, max = (int)ctx->r6;
    const uint32_t ignore = (uint32_t)ctx->r7;
    int index = 0;
    each_batch([&](uint32_t seg, uint32_t flags, uint32_t texture, int face0, int face1, int vert0) {
        if (flags & ignore) return;
        const uint32_t faces = TrackSegment::Faces::get(seg), verts = TrackSegment::Verts::get(seg);
        if (!valid_ptr(faces) || !valid_ptr(verts) || face1 < face0 || face1 - face0 > 4096) return;
        for (int f = face0; f < face1; f++, index++) {
            if (out == 0 || index < first || index >= first + max) continue;
            const uint32_t face = faces + TrackFace::Size * (uint32_t)f;
            const uint32_t t = out + TRI_SIZE * (uint32_t)(index - first);
            for (uint32_t k = 0; k < 3; k++) {
                const uint32_t v = verts + TrackVertex::Size * (uint32_t)(vert0 + TrackFace::Vertex::get(face, k));
                wr16(t + TRI_VERTS + k * 6 + 0, (uint16_t)TrackVertex::X::get(v));
                wr16(t + TRI_VERTS + k * 6 + 2, (uint16_t)TrackVertex::Y::get(v));
                wr16(t + TRI_VERTS + k * 6 + 4, (uint16_t)TrackVertex::Z::get(v));
            }
            wr16(t + TRI_VERTS + 18, 0);
            wr32(t + TRI_FLAGS, flags);
            wr32(t + TRI_TEXTURE, texture);
        }
    });
    ctx->r2 = (gpr)index;
}

// jfg_level_floor(JfgFloor* q): the first track triangle under (x, z) whose batch has a flag of mask
// (trackPolyHeight): its height and batch flags, flags 0 when there is none.
void level_floor(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t q = (uint32_t)ctx->r4;
    if (q == 0) return;
    const recomp_context saved = *ctx;
    const uint32_t out_y = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
    ctx->f12.u32l = rd32(q + FL_X);
    ctx->f14.u32l = rd32(q + FL_Z);
    const uint32_t flags = call_game(trackPolyHeight, ctx, { 0u, 0u, out_y, rd32(q + FL_MASK) });
    // call_game puts the integer arguments in a0..a3; trackPolyHeight's are x ($f12), z ($f14), outY (a2), mask (a3).
    const float y = rdf(out_y);
    *ctx = saved;
    wrf(q + FL_Y, y);
    wr32(q + FL_FLAGS, flags);
}

// jfg_damage_sphere(JfgDamageSphere* d): takes damage (256 = one unit of health) from every object with health whose
// hit centre is within the sphere (plus its own hit radius), except source (targets 1), and gives it to the breakable
// solid objects the sphere touches (targets 2; 0 = both); returns how many objects with health were hit and writes
// the breakables hit to props_hit.
void damage_sphere(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t d = (uint32_t)ctx->r4;
    if (d == 0) { ctx->r2 = 0; return; }
    const float x = rdf(d + DS_X), y = rdf(d + DS_Y), z = rdf(d + DS_Z), radius = rdf(d + DS_RADIUS);
    const int damage = (int)rd32(d + DS_DAMAGE);
    const uint32_t source = rd32(d + DS_SOURCE);
    uint32_t targets = rd32(d + DS_TARGETS);
    if (targets == 0) targets = TARGET_HEALTH | TARGET_PROPS;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    int hit = 0, props = 0;
    if ((targets & TARGET_HEALTH) && valid_ptr(list) && count > 0 && count < 4096) {
        for (int i = 0; i < count; i++) {
            const uint32_t obj = rd32(list + 4u * (uint32_t)i);
            if (!valid_ptr(obj) || obj == source) continue;
            const uint32_t dmg = Obj::Damage::get(obj);
            if (!valid_ptr(dmg)) continue;
            const int health = Damage::Health::get(dmg);
            if (health < 0x100) continue;
            const float dx = Damage::CentreX::get(dmg) - x, dy = Damage::CentreY::get(dmg) - y,
                        dz = Damage::CentreZ::get(dmg) - z;
            const float reach = radius + std::max(0.0f, Damage::Radius::get(dmg));
            if (dx * dx + dy * dy + dz * dz > reach * reach) continue;
            Damage::Health::set(dmg, (int16_t)std::max(0, health - damage));
            hit++;
        }
    }
    // Solid objects (crates, breakable props: the game's hit models) keep their own damage count, which a blast adds to
    // (explosionHitTick: HitModelData +0x12A = 1, +0x130 += damage) and their code reads (a crate breaks a stage when
    // it passes the stage's strength): they get the same damage. (A stock crate: about 3 units of health.)
    constexpr uint32_t ADDR_hitModelCount = 0x801047E0, ADDR_hitModels = 0x801047E4;
    const int models = (int)rd32(ADDR_hitModelCount);
    const uint32_t mlist = rd32(ADDR_hitModels);
    if ((targets & TARGET_PROPS) && valid_ptr(mlist) && models > 0 && models < 1024) {
        for (int i = 0; i < models; i++) {
            const uint32_t obj = rd32(mlist + 4u * (uint32_t)i);
            if (!valid_ptr(obj) || obj == source) continue;
            const uint32_t data = rd32(obj + 0x5C);
            if (!valid_ptr(data)) continue;
            float d2 = 0.0f;
            const float p[3] = { x, y, z };
            for (uint32_t k = 0; k < 3; k++) { // the sphere against the object's box
                const float lo = rdf(data + 0x100 + 4 * k), hi = rdf(data + 0x10C + 4 * k);
                const float c = std::clamp(p[k], lo, hi);
                d2 += (p[k] - c) * (p[k] - c);
            }
            if (d2 > radius * radius) continue;
            wr8(data + 0x12A, 1);
            wr16(data + 0x130, (uint16_t)(int16_t)std::min(0x7FFF, (int)(int16_t)rd16(data + 0x130) + damage));
            props++;
        }
    }
    wr32(d + DS_PROPS_HIT, (uint32_t)props);
    ctx->r2 = (gpr)hit;
}

// jfg_objects_near(const JfgNear* q, JfgNearObject* out, int max): objects with health (an enemy, a crate, a hero)
// whose hit centre is within q's sphere, except q->source, nearest first; returns how many there are (at most max
// copied). JfgNear: x, y, z, radius, source. JfgNearObject: object, x, y, z (its hit centre), health.
void objects_near(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t q = (uint32_t)ctx->r4, out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    if (q == 0) { ctx->r2 = 0; return; }
    const float x = rdf(q + 0x00), y = rdf(q + 0x04), z = rdf(q + 0x08), radius = rdf(q + 0x0C);
    const uint32_t source = rd32(q + 0x10);
    struct Found { float d2; uint32_t obj, dmg; };
    Found found[64];
    int n = 0;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    if (valid_ptr(list) && count > 0 && count < 4096) {
        for (int i = 0; i < count && n < 64; i++) {
            const uint32_t obj = rd32(list + 4u * (uint32_t)i);
            if (!valid_ptr(obj) || obj == source) continue;
            const uint32_t dmg = Obj::Damage::get(obj);
            if (!valid_ptr(dmg) || Damage::Health::get(dmg) < 0x100) continue;
            const float dx = Damage::CentreX::get(dmg) - x, dy = Damage::CentreY::get(dmg) - y,
                        dz = Damage::CentreZ::get(dmg) - z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            if (d2 > radius * radius) continue;
            found[n++] = { d2, obj, dmg };
        }
    }
    std::sort(found, found + n, [](const Found& a, const Found& b) { return a.d2 < b.d2; });
    for (int i = 0; out != 0 && i < n && i < max; i++) {
        const uint32_t o = out + 0x14u * (uint32_t)i;
        wr32(o + 0x00, found[i].obj);
        wrf(o + 0x04, Damage::CentreX::get(found[i].dmg));
        wrf(o + 0x08, Damage::CentreY::get(found[i].dmg));
        wrf(o + 0x0C, Damage::CentreZ::get(found[i].dmg));
        wr32(o + 0x10, (uint32_t)(int32_t)Damage::Health::get(found[i].dmg));
    }
    ctx->r2 = (gpr)n;
}

// jfg_objects_of_kind(int kind, JfgObjectInfo* out, int max): the level's objects of that kind (Obj::Kind, their
// behaviour: 60 keys, 98 chests, 61 health, ...) that are shown (none of the transform's hidden flags, 0xC00, which
// objPrintObject skips: a pickup taken), with their place; returns how many there are (at most max copied).
// JfgObjectInfo: object, f32 x, y, z, s32 kind (0x14 bytes).
void objects_of_kind(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int kind = (int)(int32_t)ctx->r4;
    const uint32_t out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    int n = 0;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    if (valid_ptr(list) && count > 0 && count < 4096) {
        for (int i = 0; i < count; i++) {
            const uint32_t obj = rd32(list + 4u * (uint32_t)i);
            if (!valid_ptr(obj) || Obj::Kind::get(obj) != kind) continue;
            if (rd16(obj + 0x06) & 0xC00) continue; // hidden
            if (out != 0 && n < max) {
                const uint32_t o = out + 0x14u * (uint32_t)n;
                wr32(o + 0x00, obj);
                wrf(o + 0x04, Obj::PosX::get(obj));
                wrf(o + 0x08, Obj::PosY::get(obj));
                wrf(o + 0x0C, Obj::PosZ::get(obj));
                wr32(o + 0x10, (uint32_t)kind);
            }
            n++;
        }
    }
    ctx->r2 = (gpr)n;
}

// jfg_object_triangles(const JfgNear* q, JfgObjectTriangle* out, int max): the collision triangles of the solid
// objects (the game's hit models: crates, doors, lifts, props) whose box overlaps q's sphere, in world space, with
// their batch's flags and their object; returns how many there are (at most max copied). JfgObjectTriangle: f32 v[3][3],
// u32 flags, object (0x2C bytes). The game's own sweep (hitMakePolylist) reads the same models and matrices.
void object_triangles(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    constexpr uint32_t ADDR_hitModelCount = 0x801047E0, ADDR_hitModels = 0x801047E4;
    const uint32_t q = (uint32_t)ctx->r4, out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    if (q == 0) { ctx->r2 = 0; return; }
    const float qx = rdf(q + 0x00), qy = rdf(q + 0x04), qz = rdf(q + 0x08), r = rdf(q + 0x0C);
    const uint32_t source = rd32(q + 0x10);
    const int count = (int)rd32(ADDR_hitModelCount);
    const uint32_t list = rd32(ADDR_hitModels);
    int n = 0;
    if (!valid_ptr(list) || count <= 0 || count > 1024) { ctx->r2 = 0; return; }
    for (int i = 0; i < count; i++) {
        const uint32_t obj = rd32(list + 4u * (uint32_t)i);
        if (!valid_ptr(obj) || obj == source) continue;
        const uint32_t hit = Obj::Damage::get(obj);
        if (!valid_ptr(hit) || !(rd16(hit + 0x0A) & 1)) continue; // not solid now (as hitMakePolylist)
        const uint32_t data = rd32(obj + 0x5C);                   // HitModelData
        const uint32_t models = Obj::Models::get(obj);
        if (!valid_ptr(data) || !valid_ptr(models)) continue;
        // Its box (world space) against the sphere's.
        const float mn[3] = { rdf(data + 0x100), rdf(data + 0x104), rdf(data + 0x108) };
        const float mx[3] = { rdf(data + 0x10C), rdf(data + 0x110), rdf(data + 0x114) };
        if (mn[0] > qx + r || mx[0] < qx - r || mn[1] > qy + r || mx[1] < qy - r || mn[2] > qz + r || mx[2] < qz - r)
            continue;
        const uint32_t model = rd32(models + 4u * rd8(data + 0x133));
        if (!valid_ptr(model)) continue;
        const uint32_t header = rd32(model + 0x00), verts = rd32(model + 0x04);
        if (!valid_ptr(header) || !valid_ptr(verts)) continue;
        const int batches = (int16_t)rd16(header + 0x16);
        const uint32_t tris = rd32(header + 0x20), batch0 = rd32(header + 0x24);
        if (!valid_ptr(tris) || !valid_ptr(batch0) || batches <= 0 || batches > 256) continue;
        // Model to world: the second of its matrix pairs (the first takes world points into the model).
        const uint32_t m = data + 0x80 + 0x40u * (rd8(data + 0x12B) & 1);
        float M[16];
        for (uint32_t k = 0; k < 16; k++) M[k] = rdf(m + 4 * k);
        for (int b = 0; b < batches; b++) {
            const uint32_t batch = batch0 + 0x10u * (uint32_t)b;
            const int vert0 = (int16_t)rd16(batch + 0x06);
            const int t0 = (int16_t)rd16(batch + 0x08), t1 = (int16_t)rd16(batch + 0x18);
            const uint32_t flags = rd32(batch + 0x0C);
            if (t1 < t0 || t1 - t0 > 4096) continue;
            for (int t = t0; t < t1; t++, n++) {
                if (out == 0 || n >= max) continue;
                const uint32_t tri = tris + 0x10u * (uint32_t)t;
                const uint32_t o = out + 0x2Cu * (uint32_t)n;
                for (uint32_t k = 0; k < 3; k++) {
                    const uint32_t v = verts + 10u * (uint32_t)(vert0 + rd8(tri + 1 + k));
                    const float x = (int16_t)rd16(v), y = (int16_t)rd16(v + 2), z = (int16_t)rd16(v + 4);
                    for (uint32_t c = 0; c < 3; c++) {
                        wrf(o + k * 12 + c * 4, x * M[c] + y * M[4 + c] + z * M[8 + c] + M[12 + c]);
                    }
                }
                wr32(o + 0x24, flags);
                wr32(o + 0x28, obj);
            }
        }
    }
    ctx->r2 = (gpr)n;
}

// jfg_screen_flash(rgba): the first player's screen flashes that colour this tick, as the heroes' own code does each
// tick they are hurt (0xFF0000FF, the red hit flash; the Hit flash opacity option applies).
void screen_flash(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    call_game(frontFlashScreen, ctx, { 0u, 0u, (uint32_t)ctx->r4 });
}

// jfg_camera_set(const JfgCameraSet* s): this tick the camera the hero steers by stands at s's position, looking at
// its focus, rolled by roll (0x10000 a turn); applied after the game's own camera code (the wrapper below), so a mod
// sets it each tick it wants it.

void camera_set(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t s = (uint32_t)ctx->r4;
    if (s == 0) return;
    g_camera_set = { rdf(s + 0x00), rdf(s + 0x04), rdf(s + 0x08), rdf(s + 0x0C), rdf(s + 0x10), rdf(s + 0x14),
                     (int16_t)rd32(s + 0x18), rdf(s + 0x1C), true };
}

// jfg_object_health(obj): its health (256 per unit), -1 without damage data. jfg_object_set_health(obj, health).
void object_health(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t dmg = valid_ptr(obj) ? Obj::Damage::get(obj) : 0;
    ctx->r2 = valid_ptr(dmg) ? (gpr)Damage::Health::get(dmg) : (gpr)-1;
}

void object_set_health(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t dmg = valid_ptr(obj) ? Obj::Damage::get(obj) : 0;
    if (valid_ptr(dmg)) Damage::Health::set(dmg, (int16_t)std::clamp((int)ctx->r5, -0x8000, 0x7FFF));
}

// A squad member's type (its squadron's baddy type, the member's object id), -1 for anything else.
int squad_type(uint32_t obj) {
    const uint32_t data = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    const uint32_t squadron = valid_ptr(data) ? SquadMemberData::Squadron::get(data) : 0;
    const uint32_t sdata = valid_ptr(squadron) ? Obj::Data::get(squadron) : 0;
    if (!valid_ptr(sdata)) return -1;
    // Only a member of the squadron's own list (other kinds keep something else at +0x24).
    uint32_t member = SquadronData::FirstMember::get(sdata);
    for (int n = 0; n < 256 && valid_ptr(member); n++) {
        if (member == obj) return SquadronData::BaddyType::get(sdata);
        const uint32_t mdata = Obj::Data::get(member);
        if (!valid_ptr(mdata)) return -1;
        member = SquadMemberData::NextMember::get(mdata);
    }
    return -1;
}

// jfg_enemy_type(obj): squad_type.
void enemy_type(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->r2 = (gpr)squad_type((uint32_t)ctx->r4);
}

// jfg_enemies_near(const JfgNear* q, JfgEnemy* out, int max): the squad members whose hit sphere (centre, radius)
// overlaps q's sphere, except q's source, nearest first, dying ones too (health 0 or less: they stay until removed, and
// a further blow still makes them burst); returns how many there are (at most max copied). JfgEnemy: object, f32 x,
// y, z (hit centre), f32 radius (hit radius), s32 health, s32 type, s32 tribal (squadsIsTribal: a tribal to rescue,
// not an enemy) (0x20 bytes).
void enemies_near(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t q = (uint32_t)ctx->r4, out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    if (q == 0) { ctx->r2 = 0; return; }
    const float x = rdf(q + 0x00), y = rdf(q + 0x04), z = rdf(q + 0x08), radius = rdf(q + 0x0C);
    const uint32_t source = rd32(q + 0x10);
    struct Found { float d2; uint32_t obj, dmg; int type; };
    Found found[64];
    int n = 0;
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    if (valid_ptr(list) && count > 0 && count < 4096) {
        for (int i = 0; i < count && n < 64; i++) {
            const uint32_t obj = rd32(list + 4u * (uint32_t)i);
            if (!valid_ptr(obj) || obj == source) continue;
            const uint32_t dmg = Obj::Damage::get(obj);
            if (!valid_ptr(dmg)) continue;
            const float dx = Damage::CentreX::get(dmg) - x, dy = Damage::CentreY::get(dmg) - y,
                        dz = Damage::CentreZ::get(dmg) - z;
            const float d2 = dx * dx + dy * dy + dz * dz;
            const float reach = radius + std::max(0.0f, Damage::Radius::get(dmg));
            if (d2 > reach * reach) continue;
            const int type = squad_type(obj);
            if (type < 0) continue;
            found[n++] = { d2, obj, dmg, type };
        }
    }
    std::sort(found, found + n, [](const Found& a, const Found& b) { return a.d2 < b.d2; });
    const recomp_context saved = *ctx;
    for (int i = 0; out != 0 && i < n && i < max; i++) {
        const uint32_t o = out + 0x20u * (uint32_t)i;
        wr32(o + 0x00, found[i].obj);
        wrf(o + 0x04, Damage::CentreX::get(found[i].dmg));
        wrf(o + 0x08, Damage::CentreY::get(found[i].dmg));
        wrf(o + 0x0C, Damage::CentreZ::get(found[i].dmg));
        wrf(o + 0x10, std::max(0.0f, Damage::Radius::get(found[i].dmg)));
        wr32(o + 0x14, (uint32_t)(int32_t)Damage::Health::get(found[i].dmg));
        wr32(o + 0x18, (uint32_t)found[i].type);
        wr32(o + 0x1C, call_game(squadsIsTribal, ctx, { (uint32_t)found[i].type }) != 0 ? 1u : 0u);
    }
    *ctx = saved;
    ctx->r2 = (gpr)n;
}

// jfg_object_blast_damage(obj): the blast damage (grenades, rockets, mines, exploding barrels) it took since the last
// call, 0 if none; reading it clears it.
void object_blast_damage(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t dmg = valid_ptr(obj) ? Obj::Damage::get(obj) : 0;
    int total = 0, kept = 0;
    for (int i = 0; i < g_blast_count; i++) {
        if (dmg != 0 && g_blasts[i].dmg == dmg) total += g_blasts[i].damage;
        else g_blasts[kept++] = g_blasts[i];
    }
    g_blast_count = kept;
    ctx->r2 = (gpr)total;
}

// jfg_hero_heads(int character, int tribal): the heads the first player has collected as that hero (the game
// character's +0x06 tribals', +0x08 the others'; mainGetGameCharacter(player, character)).
void hero_heads(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int character = (int)ctx->r4;
    const bool tribal = ctx->r5 != 0;
    const recomp_context saved = *ctx;
    const uint32_t c = call_game(mainGetGameCharacter, ctx, { 0u, (uint32_t)character });
    *ctx = saved;
    ctx->r2 = valid_ptr(c) ? (gpr)rd16(c + (tribal ? 0x06 : 0x08)) : 0;
}

// jfg_hud_hide_health(int character, int hide): while hidden, the game's health meter is not drawn for that hero (a
// mod draws its own).
void hud_hide_health(uint8_t*, recomp_context* ctx) {
    const int character = (int)ctx->r4;
    if (character >= 0 && character < CHARACTERS) g_hide_health[character] = ctx->r5 != 0;
}

// jfg_hud_in_corners(void): 1 while the single-player HUD is at the window's edges (HUD in Screen Corners, in
// widescreen), 0 while it is in the middle 4:3 area.
void hud_in_corners_export(uint8_t*, recomp_context* ctx) { ctx->r2 = jfg::hud_in_corners() ? 1 : 0; }
// --- toggled buttons (input thread) --------------------------------------------------------------------------------
std::atomic<uint16_t> g_toggle_mask{ 0 };    // jfg_input_toggle: buttons that toggle
std::atomic<uint16_t> g_toggle_latched{ 0 }; // ... and read as held now
std::atomic<uint16_t> g_toggle_release{ 0 }; // jfg_input_toggle_release: let go at the next poll
std::atomic<uint16_t> g_blocked{ 0 };        // jfg_input_block: buttons that read as let go

// jfg_input_block(unsigned int buttons, int on): while on, those N64 buttons read as not pressed (a dodge that may not
// fire, say).
void input_block(uint8_t*, recomp_context* ctx) {
    const uint16_t mask = (uint16_t)ctx->r4;
    if (ctx->r5 != 0) g_blocked.fetch_or(mask);
    else g_blocked.fetch_and((uint16_t)~mask);
}

// jfg_input_toggle(unsigned int buttons, int on): those N64 buttons toggle: a press holds them down, the next lets go.
void input_toggle(uint8_t*, recomp_context* ctx) {
    const uint16_t mask = (uint16_t)ctx->r4;
    if (ctx->r5 != 0) g_toggle_mask.fetch_or(mask);
    else { g_toggle_mask.fetch_and((uint16_t)~mask); g_toggle_latched.fetch_and((uint16_t)~mask); }
}

// jfg_input_toggle_release(unsigned int buttons): toggled buttons held now are let go.
void input_toggle_release(uint8_t*, recomp_context* ctx) { g_toggle_release.fetch_or((uint16_t)ctx->r4); }

// jfg_input_toggled(void): the toggled buttons held now.
void input_toggled(uint8_t*, recomp_context* ctx) { ctx->r2 = g_toggle_latched.load(); }

// --- heroes -------------------------------------------------------------------------------------------------------
// jfg_hero_get(void): the first player's hero object, or NULL.
void hero_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = rd32(ADDR_mainGameMode) == 0 ? player_object(ctx) : 0;
    ctx->r2 = (gpr)(int32_t)(hero_data(obj) != 0 ? obj : 0);
}

// jfg_hero_state(void* hero): the hero's state (low 4 bits), -1 if it is not a hero.
void hero_state(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    ctx->r2 = data != 0 ? (gpr)(Hero::State::get(data) & 0xF) : (gpr)-1;
}

// jfg_hero_weapon(void): the weapon in the first player's hand (the character's current weapon), -1 if none.
void hero_weapon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (rd32(ADDR_mainGameMode) == 0 && c >= 0 && c < 3) ? character_ptr(c) : 0;
    ctx->r2 = (gpr)(ch != 0 ? weapons::weapon_in_hand(ch, c) : -1); // modded weapons: their ids (16 up)
}

// jfg_game_paused(void): 1 while the world stands still: the pause menu, or the settings menu with Pause in the Settings
// Menu on (jfg_freeze_world: the game's own tick still runs then, drawing objects without moving them), or the fly camera
// frozen.
void game_paused(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    ctx->r2 = (jfg_freeze_world() != 0 || (int8_t)rd8(ADDR_pauseMode) != 0) ? 1 : 0;
}

// The first player's character (its saved state: health, weapons, ammo), 0 outside a level.
uint32_t player_character() {
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    return (rd32(ADDR_mainGameMode) == 0 && c >= 0 && c < 3) ? character_ptr(c) : 0;
}

// jfg_hero_ammo(int weapon): the first player's ammo for that weapon (0..14; 15 up: the proximity mine and modded
// weapons, -1 when not owned), -1 outside a level.
void hero_ammo(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t ch = player_character();
    const int w = (int)ctx->r4;
    if (w >= 15) { ctx->r2 = (gpr)(ch != 0 ? weapons::carried_ammo(w) : -1); return; }
    ctx->r2 = (ch != 0 && w >= 0) ? (gpr)Character::Ammo::get(ch, (uint32_t)w) : (gpr)-1;
}

// jfg_hero_set_ammo(int weapon, int ammo): sets it (0..0xFFFF; the game keeps it at the weapon's capacity itself).
void hero_set_ammo(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t ch = player_character();
    const int w = (int)ctx->r4;
    if (w >= 15) { if (ch != 0) weapons::set_carried_ammo(w, (int)ctx->r5); return; }
    if (ch != 0 && w >= 0) Character::Ammo::set(ch, (uint32_t)w, (uint16_t)std::clamp((int)ctx->r5, 0, 0xFFFF));
}

// jfg_enemy_dying(void* enemy): 1 when the squad member is dying: no health left (it plays its death), or left with the
// sliver of health the game gives the kinds that lie down and bleed out instead of dying at once (overlay 3's
// SquaddieControl: at most its squadron's member health / 8 (one unit at least, ten at most) plus one). 0 otherwise,
// -1 if it is not a squad member.
void enemy_dying(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    if (!valid_ptr(obj) || Obj::Kind::get(obj) != KIND_SQUAD_MEMBER) { ctx->r2 = (gpr)-1; return; }
    const uint32_t dmg = Obj::Damage::get(obj);
    const uint32_t data = Obj::Data::get(obj);
    const int health = valid_ptr(dmg) ? Damage::Health::get(dmg) : 0;
    if (health <= 0) { ctx->r2 = 1; return; }
    const uint32_t squadron = valid_ptr(data) ? SquadMemberData::Squadron::get(data) : 0;
    const uint32_t sdata = valid_ptr(squadron) ? Obj::Data::get(squadron) : 0;
    const int full = valid_ptr(sdata) ? SquadronData::MemberHealth::get(sdata) : 0;
    const int sliver = std::clamp(full >> 3, 0x100, 0xA00) + 1;
    ctx->r2 = (full > 0 && health <= sliver && health < full) ? 1 : 0;
}

// jfg_hero_push(void* hero, float x, float z, int ticks): the hero is pushed x, z level units per 1/60 s for that
// many 1/60 s ticks (the game's knockback: walls stop it); the game eases it off after.
void hero_push(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    if (data == 0) return;
    auto as_float = [](gpr bits) { const uint32_t v = (uint32_t)bits; float f; std::memcpy(&f, &v, 4); return f; };
    Hero::PushX::set(data, as_float(ctx->r5));
    Hero::PushZ::set(data, as_float(ctx->r6));
    Hero::PushTicks::set(data, (int8_t)std::clamp((int)ctx->r7, 0, 127));
}

// jfg_hero_hit(void* hero, int units): the hero is hit as by an enemy's shot: the hero's control code takes one unit of
// health (0x80 in the hero's data + 0x190) or two (0xC0), with its hit reaction, sound and red flash, unless the hero
// is protected (+0x5C8) (overlays 15-17's control code, e.g. overlay_16.c's boy control).
void hero_hit(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    if (data == 0) return;
    wr8(data + 0x190, (int)ctx->r5 >= 2 ? 0xC0 : 0x80);
}

// The hero's move setter (overlay 15 Vela's, 16 Juno's), or null for Lupus and outside a level.
static void (*move_setter())(uint8_t*, recomp_context*) {
    if (rd32(ADDR_mainGameMode) != 0) return nullptr;
    const int character = (int16_t)rd16(ADDR_currentCharacter);
    if (character == 1) return func_overlay_16_00004F78_1F22F90;
    if (character == 0) return func_overlay_15_00005180_1F18B00;
    return nullptr;
}

// jfg_hero_move(void* hero, int move): the hero's move (its animation, which the game's move picker carries on from:
// 0 walk, 1 run, 0x10 standing still; 4 and 0x0E are crouched) from its start. Juno and Vela only; returns 1 if set.
void hero_move(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = hero_data(obj);
    const auto set_move = move_setter();
    ctx->r2 = 0;
    if (data == 0 || set_move == nullptr) return;
    call_game(set_move, ctx, { obj, data, (uint32_t)ctx->r5 & 0xFF, 0 }); // frame 0.0f
    g_rdram = rdram_;
    Obj::AnimPhase::set(obj, 0.0f);
    ctx->r2 = 1;
}

// jfg_hero_roll(void* hero, int right): the hero rolls to its left (0) or right (1) as the game's side buttons start a
// roll while it stands (overlay 16's func_00004934: the roll state, the roll move from its start, the sideways flag): the
// game then plays the roll and moves the hero along the roll's speed curve. Juno and Vela only (Lupus has no roll);
// returns 1 if it rolled.
void hero_roll(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const bool right = ctx->r5 != 0;
    const uint32_t data = hero_data(obj);
    const int character = (int16_t)rd16(ADDR_currentCharacter);
    ctx->r2 = 0;
    if (data == 0 || rd32(ADDR_mainGameMode) != 0) return;
    void (*set_move)(uint8_t*, recomp_context*) = nullptr;
    uint32_t move = 0;
    if (character == 1) { set_move = func_overlay_16_00004F78_1F22F90; move = right ? 0x0B : 0x31; }      // Juno
    else if (character == 0) { set_move = func_overlay_15_00005180_1F18B00; move = right ? 0x0B : 0x32; } // Vela
    if (set_move == nullptr) return;
    Hero::Roll::set(data, right ? 2 : 1);
    call_game(set_move, ctx, { obj, data, move, 0 }); // frame 0.0f
    g_rdram = rdram_;
    Obj::AnimPhase::set(obj, 0.0f);
    Hero::SideMoving::set(data, 1);
    ctx->r2 = 1;
}

// jfg_game_sound(int id, float x, float y, float z): the game's sound effect id, placed at x, y, z (amSndPlayXYZ, as the
// game places its own: louder near the camera).
void game_sound(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t id = (uint32_t)ctx->r4 & 0xFFFF;
    call_game(amSndPlayXYZ, ctx, { id, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7, 4, 0 });
}

// jfg_game_sound_flat(int id): the game's sound effect id, not placed (amSndPlay, as the game plays its menu and HUD
// sounds): heard the same wherever the camera is.
void game_sound_flat(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    call_game(amSndPlay, ctx, { (uint32_t)ctx->r4 & 0xFFFF, 0 });
}

// jfg_hero_speak(void* hero, int type): the hero's own voice of that type (charSpeak: the sound set for the hero
// playing, Juno, Vela or Lupus; 0 the jump, 1 the landing, 3 being hit), as the game plays them.
void hero_speak(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t type = (uint32_t)ctx->r5 & 0xFF;
    if (hero_data(obj) == 0 || type > 15) return;
    call_game(charSpeak, ctx, { obj, type });
}

// jfg_hero_protect(void* hero, int ticks): no damage reaches the hero for at least that many 1/60 s ticks (8 at
// least: the game's own protection, as after a level exit).
void hero_protect(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4);
    if (data == 0) return;
    const int ticks = std::clamp((int)ctx->r5, 8, 0x7FFF);
    if (Hero::Protected::get(data) < ticks) Hero::Protected::set(data, (int16_t)ticks);
}

// --- Floyd --------------------------------------------------------------------------------------------------------
// jfg_floyd_get(int* state): Floyd's object while he is with the first player's hero, or NULL. state (may be NULL):
// 0 he follows the hero (the game's AI), 1 a player flies him (a co-op game, or the hero flying him), 2 a Floyd
// mission.
void floyd_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t state_out = (uint32_t)ctx->r4;
    const uint32_t hero = rd32(ADDR_mainGameMode) == 0 ? player_object(ctx) : 0;
    const uint32_t data = hero_data(hero);
    const uint32_t floyd = data != 0 ? Hero::Floyd::get(data) : 0;
    int state = 0;
    if (rd8(ADDR_robotMission) != 0) state = 2;
    else if (rd8(ADDR_cooperativeGame) != 0 || (data != 0 && (Hero::FloydFlown::get(data) & 1) != 0)) state = 1;
    if (state_out != 0) wr32(state_out, (uint32_t)state);
    ctx->r2 = (gpr)(int32_t)(valid_ptr(floyd) ? floyd : 0);
}

// jfg_fire_bolt(const JfgBolt* bolt): a laser bolt (Floyd's: object 0x50) from bolt's start towards its target.
// Returns the bolt object, NULL if none was made. JfgBolt: f32 x, y, z, tx, ty, tz; s32 damage (units of health,
// 1..127); f32 speed (Floyd's: 35); s32 life (ticks of 1/10 s; Floyd's: 30); owner (the bolts don't hit it); s32 sound
// (-1 none; Floyd's: 0xDA).
void fire_bolt(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t b = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (b == 0) return;
    const float x = rdf(b + 0x00), y = rdf(b + 0x04), z = rdf(b + 0x08);
    const float dx = x - rdf(b + 0x0C), dy = rdf(b + 0x10) - y, dz = z - rdf(b + 0x14);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x24; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, 0x50);
    wr8(e + 0x02, 0x24);
    wr16(e + 0x04, (uint16_t)(int16_t)std::clamp(x, -32768.0f, 32767.0f));
    wr16(e + 0x06, (uint16_t)(int16_t)std::clamp(y, -32768.0f, 32767.0f));
    wr16(e + 0x08, (uint16_t)(int16_t)std::clamp(z, -32768.0f, 32767.0f));
    wr8(e + 0x0A, (uint8_t)std::clamp((int)rd32(b + 0x18), 1, 127));
    wr8(e + 0x0B, (uint8_t)std::clamp((int)rd32(b + 0x20), 1, 255));
    wr16(e + 0x0C, (uint16_t)angle(dx, dz)); // as Floyd's code works them out (Arctanf)
    wr16(e + 0x0E, (uint16_t)angle(dy, std::sqrt(dx * dx + dz * dz)));
    wr8(e + 0x13, 0xFF);
    wrf(e + 0x14, rdf(b + 0x1C) > 0.0f ? rdf(b + 0x1C) : 35.0f);
    wr32(e + 0x18, rd32(b + 0x24));
    wr32(e + 0x1C, 0x800A1790); // Floyd's bolts' effect tables
    wr32(e + 0x20, 0x800A1850);
    const uint32_t bolt = call_game(objSetupObject, ctx, { e, 1 });
    if (valid_ptr(bolt)) wr32(bolt + 0x3C, 0);
    const int sound = (int)rd32(b + 0x28);
    if (sound >= 0) call_game(amSndPlay, ctx, { (uint32_t)sound, 0 });
    ctx->r2 = (gpr)(int32_t)(valid_ptr(bolt) ? bolt : 0);
}

// jfg_fire_plasma(const JfgPlasma* plasma): a plasma ball (the plasma shotgun's: object 0x13E, kind 0xA3, its code
// twisterInit / twisterControl in overlay 131) from plasma's start towards its target, as shoot_plasmashot spawns it.
// JfgPlasma: f32 x, y, z, tx, ty, tz; s32 size (its damage in units of health, 1..15; drawn and sized as the
// shotgun's 1..4 charge, larger ones as 4); f32 speed (0: it stays where it is); s32 time (1/10 s); owner (whose shot it
// is: its hits count as that hero's plasma shotgun). Returns the ball, NULL if none was made.
void fire_plasma(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t b = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (b == 0) return;
    const float x = rdf(b + 0x00), y = rdf(b + 0x04), z = rdf(b + 0x08);
    const float dx = x - rdf(b + 0x0C), dy = rdf(b + 0x10) - y, dz = z - rdf(b + 0x14);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x18; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, 0x13E);
    wr8(e + 0x02, 0x18);
    wr16(e + 0x04, (uint16_t)(int16_t)std::clamp(x, -32768.0f, 32767.0f));
    wr16(e + 0x06, (uint16_t)(int16_t)std::clamp(y, -32768.0f, 32767.0f));
    wr16(e + 0x08, (uint16_t)(int16_t)std::clamp(z, -32768.0f, 32767.0f));
    wr8(e + 0x0A, (uint8_t)std::clamp((int)rd32(b + 0x18), 1, 15));
    wr8(e + 0x0B, (uint8_t)std::clamp((int)rd32(b + 0x20), 1, 255));
    wr16(e + 0x0C, (uint16_t)angle(dx, dz));
    wr16(e + 0x0E, (uint16_t)angle(dy, std::sqrt(dx * dx + dz * dz)));
    wrf(e + 0x10, std::max(0.0f, rdf(b + 0x1C)));
    wr32(e + 0x14, rd32(b + 0x24));
    const uint32_t ball = call_game(objSetupObject, ctx, { e, 1 });
    if (valid_ptr(ball)) wr32(ball + 0x3C, 0);
    ctx->r2 = (gpr)(int32_t)(valid_ptr(ball) ? ball : 0);
}

// jfg_level_ray(const float* from, const float* to, float* out): the first point of the level (its track, as a plasma
// ball tests its flight: trackNearestIntersection) on the line from -> to. 1 if there is one, with out = x, y, z of it
// and the surface's normal (nx, ny, nz, facing from); 0 otherwise. The result (+0x04 the point, +0x20 its distance)
// carries no normal the port has found, so it is the plane of three such hits on lines a little apart.
void level_ray(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t a = (uint32_t)ctx->r4, b = (uint32_t)ctx->r5, out = (uint32_t)ctx->r6;
    ctx->r2 = 0;
    if (a == 0 || b == 0 || out == 0) return;
    const float ax = rdf(a), ay = rdf(a + 4), az = rdf(a + 8), bx = rdf(b), by = rdf(b + 4), bz = rdf(b + 8);
    float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len < 0.001f) return;
    dx /= len; dy /= len; dz /= len;
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // in the gap call_game leaves below $sp
    auto cast = [&](float ox, float oy, float oz, float* hit) -> bool {
        g_rdram = rdram_;
        wrf(buf + 0, ax + ox); wrf(buf + 4, ay + oy); wrf(buf + 8, az + oz);
        wrf(buf + 12, bx + ox); wrf(buf + 16, by + oy); wrf(buf + 20, bz + oz);
        const bool found = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
        g_rdram = rdram_;
        if (!found) return false;
        hit[0] = rdf(buf + 24 + 4); hit[1] = rdf(buf + 24 + 8); hit[2] = rdf(buf + 24 + 12);
        return std::isfinite(hit[0]) && std::isfinite(hit[1]) && std::isfinite(hit[2]);
    };
    float p[3];
    if (!cast(0, 0, 0, p)) return;
    // Four lines beside the first, 6 units off across it (right, up, left, down), each started 24 units further back
    // and run 24 further on: a start right at the surface can be behind it and miss. Hits more than 30 units from the
    // first are on another surface (an edge) and left out; the plane is that of the first hit and the two others that
    // span it best.
    float ux = 0, uy = 1, uz = 0;
    if (std::fabs(dy) > 0.9f) { ux = 1; uy = 0; }
    float sx = dy * uz - dz * uy, sy = dz * ux - dx * uz, sz = dx * uy - dy * ux; // side = d x up
    const float sl = std::sqrt(sx * sx + sy * sy + sz * sz);
    sx /= sl; sy /= sl; sz /= sl;
    const float tx = sy * dz - sz * dy, ty = sz * dx - sx * dz, tz = sx * dy - sy * dx; // across both
    constexpr float OFF = 6.0f, BACK = 24.0f;
    const float offs[4][3] = { { sx, sy, sz }, { tx, ty, tz }, { -sx, -sy, -sz }, { -tx, -ty, -tz } };
    float near[4][3];
    int count = 0;
    for (const auto& o : offs) {
        float h[3];
        // Back by BACK along the line at the start, on by BACK at the end: offset the start and end differently.
        g_rdram = rdram_;
        wrf(buf + 0, ax + o[0] * OFF - dx * BACK); wrf(buf + 4, ay + o[1] * OFF - dy * BACK); wrf(buf + 8, az + o[2] * OFF - dz * BACK);
        wrf(buf + 12, bx + o[0] * OFF + dx * BACK); wrf(buf + 16, by + o[1] * OFF + dy * BACK); wrf(buf + 20, bz + o[2] * OFF + dz * BACK);
        const bool found = (call_game(trackNearestIntersection, ctx, { 0u, buf, buf + 12, buf + 24, 0u, 0u }) & 0xFF) != 0;
        g_rdram = rdram_;
        if (!found) continue;
        h[0] = rdf(buf + 24 + 4); h[1] = rdf(buf + 24 + 8); h[2] = rdf(buf + 24 + 12);
        if (!std::isfinite(h[0]) || !std::isfinite(h[1]) || !std::isfinite(h[2])) continue;
        const float ex = h[0] - p[0], ey = h[1] - p[1], ez = h[2] - p[2];
        if (ex * ex + ey * ey + ez * ez > 30.0f * 30.0f) continue;
        near[count][0] = h[0]; near[count][1] = h[1]; near[count][2] = h[2];
        count++;
    }
    // If the plane can't be measured: a floor or ceiling for a line going steeply down or up, else a wall facing back.
    float nx, ny, nz;
    if (std::fabs(dy) > 0.5f) {
        nx = 0; ny = dy > 0 ? -1.0f : 1.0f; nz = 0;
    } else {
        const float hl = std::sqrt(dx * dx + dz * dz);
        nx = -dx / hl; ny = 0; nz = -dz / hl;
    }
    float best = 0.0f;
    for (int i = 0; i < count; i++) {
        for (int j = i + 1; j < count; j++) {
            const float e1x = near[i][0] - p[0], e1y = near[i][1] - p[1], e1z = near[i][2] - p[2];
            const float e2x = near[j][0] - p[0], e2y = near[j][1] - p[1], e2z = near[j][2] - p[2];
            float cx = e1y * e2z - e1z * e2y, cy = e1z * e2x - e1x * e2z, cz = e1x * e2y - e1y * e2x;
            const float cl = std::sqrt(cx * cx + cy * cy + cz * cz);
            if (cl <= best || cl < 1.0f) continue;
            best = cl;
            cx /= cl; cy /= cl; cz /= cl;
            if (cx * dx + cy * dy + cz * dz > 0) { cx = -cx; cy = -cy; cz = -cz; }
            nx = cx; ny = cy; nz = cz;
        }
    }
    wrf(out + 0, p[0]); wrf(out + 4, p[1]); wrf(out + 8, p[2]);
    wrf(out + 12, nx); wrf(out + 16, ny); wrf(out + 20, nz);
    ctx->r2 = 1;
}

// jfg_line_of_sight(void* from, void* to, const float* a, const float* b): 1 when nothing (the level, or an object
// with a hit model other than from and to) stands between points a and b (x, y, z each).
void line_of_sight(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t from = (uint32_t)ctx->r4, to = (uint32_t)ctx->r5, a = (uint32_t)ctx->r6, b = (uint32_t)ctx->r7;
    if (a == 0 || b == 0) { ctx->r2 = 0; return; }
    const uint32_t blocked = call_game(hitsCheckLineOfSight, ctx, { from, to, rd32(a), rd32(a + 4), rd32(a + 8), rd32(b), rd32(b + 4), rd32(b + 8) });
    ctx->r2 = blocked == 0 ? 1 : 0;
}

// --- enemies ------------------------------------------------------------------------------------------------------
// jfg_enemy_alert(void* enemy, void* shooter): the enemy was shot by shooter (a hero): its squad turns on it, as
// after a hit by the hero's weapons. Squad members only.
void enemy_alert(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (is_squad_member(obj) && valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
}

// jfg_enemy_ignite(void* enemy, void* shooter): the enemy catches fire as from the flamethrower: it screams, runs for
// water and burns down (shooter, if not NULL, is who set it alight). Squad members only.
void enemy_ignite(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (!is_squad_member(obj)) return;
    if (valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
    call_game(SetSquaddieOnFire, ctx, { obj });
}

// jfg_enemy_electrocute(void* enemy, void* shooter): the enemy is electrocuted: held in place, shaking and losing
// health for about 2 seconds, with sparks. Squad members only; one already electrocuted is left alone.
void enemy_electrocute(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, shooter = (uint32_t)ctx->r5;
    if (!is_squad_member(obj)) return;
    if (valid_ptr(shooter)) call_game(squadsPlayerShootsSquaddie, ctx, { obj, shooter });
    call_game(squadsElectrocuteSquaddie, ctx, { obj });
}

// --- effects ------------------------------------------------------------------------------------------------------
// jfg_fx_fire(float x, float y, float z, int scale, int ticks): a fire on the ground (the one explosions leave:
// flames, smoke and light; it hurts nothing by itself) of scale 1..127 (32 a small one) burning for ticks of 1/10 s.
// Returns the fire object, NULL if none was made.
void fx_fire(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    auto as_float = [](gpr bits) { const uint32_t v = (uint32_t)bits; float f; std::memcpy(&f, &v, 4); return f; };
    const float x = ctx->f12.fl, y = ctx->f14.fl, z = as_float(ctx->r6);
    const int scale = (int)ctx->r7;
    const int ticks = (int)rd32((uint32_t)ctx->r29 + 0x10);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x10; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, 0x13D);
    wr8(e + 0x02, 0x0C);
    wr16(e + 0x04, (uint16_t)(int16_t)std::clamp(x, -32768.0f, 32767.0f));
    wr16(e + 0x06, (uint16_t)(int16_t)std::clamp(y, -32768.0f, 32767.0f));
    wr16(e + 0x08, (uint16_t)(int16_t)std::clamp(z, -32768.0f, 32767.0f));
    wr8(e + 0x0A, (uint8_t)std::clamp(scale, 1, 127));
    wr8(e + 0x0B, (uint8_t)std::clamp(ticks, 1, 127));
    const uint32_t fire = call_game(objSetupObject, ctx, { e, 1 });
    if (valid_ptr(fire)) wr32(fire + 0x3C, 0);
    ctx->r2 = (gpr)(int32_t)(valid_ptr(fire) ? fire : 0);
}

// --- objects and effects for mods' weapons (since version 6) ---------------------------------------------------------
constexpr uint32_t OBJ_SCALE = 0x08;   // the object's size (its transform's scale; 1.0 = its definition's)
constexpr uint32_t OBJ_PHYSICS = 0x7C; // ObjPhysics*, when its definition has a physics block (+0xD0); byte 0: flags
constexpr uint8_t PHYS_AT_REST = 2;

// A pointer a mod passed: the game's memory, or mods' own above 0x81000000 (docs/MEMORY.md).
bool mod_ptr(uint32_t p) { return p >= 0x80000400u && p < 0xA0000000u; }

uint32_t s16_arg(float v) { return (uint32_t)(int32_t)(int16_t)std::clamp(v, -32768.0f, 32767.0f); }
uint32_t float_bits(float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; }

// jfg_spawn(const JfgSpawn* s): { s32 object; f32 x, y, z, vx, vy, vz; s32 yaw; } an object of that id (any object
// type, a pack's included) at x, y, z, as a weapon's shot is made (objSetupObject, list 1: gone with the level). With
// a physics block (definition +0xD0) it is set flying at vx, vy, vz (units a tick) and falls, bounces and comes to
// rest by the game's own physics (decomp MSR 8c). The object, or NULL.
void spawn(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t s = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (!mod_ptr(s)) return;
    const int id = (int32_t)rd32(s);
    if (id <= 0 || id > 0x7FFF || rd32(ADDR_mainGameMode) != 0) return;
    const float x = rdf(s + 4), y = rdf(s + 8), z = rdf(s + 12);
    const uint32_t e = entry_at(ctx);
    for (uint32_t i = 0; i < 0x10; i += 4) wr32(e + i, 0);
    wr16(e + 0x00, (uint16_t)id);
    wr8(e + 0x02, 0x10);
    wr16(e + 0x04, (uint16_t)s16_arg(x));
    wr16(e + 0x06, (uint16_t)s16_arg(y));
    wr16(e + 0x08, (uint16_t)s16_arg(z));
    const uint32_t obj = call_game(objSetupObject, ctx, { e, 1 });
    g_rdram = rdram_;
    if (!valid_ptr(obj)) return;
    wr32(obj + 0x3C, 0);
    wrf(obj + 0x0C, x); // the entry only holds whole units
    wrf(obj + 0x10, y);
    wrf(obj + 0x14, z);
    Obj::Yaw::set(obj, (uint16_t)rd32(s + 28));
    wrf(obj + 0x1C, rdf(s + 16));
    wrf(obj + 0x20, rdf(s + 20));
    wrf(obj + 0x24, rdf(s + 24));
    const uint32_t phys = rd32(obj + OBJ_PHYSICS);
    if (valid_ptr(phys)) wr8(phys, rd8(phys) & (uint8_t)~PHYS_AT_REST);
    ctx->r2 = (gpr)(int32_t)obj;
}

// jfg_asset_number(const char* name): the number an appended entry or object type of a pack got this start
// ("mod_id:append name", as jfg_assets.json names it; jfg_assets.cpp), -1 if no loaded mod adds it. An object type's
// number is the object id to spawn.
void asset_number_api(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    std::string name;
    for (uint32_t i = 0; i < 128 && mod_ptr(p + i); i++) {
        const char c = (char)rd8(p + i);
        if (c == 0) break;
        name += c;
    }
    ctx->r2 = (gpr)jfg::asset_number(name);
}

// jfg_object_free(void* object): removes an object (objFreeObject), as its own code does when it is done.
void object_free(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    if (!valid_ptr(obj) || rd32(ADDR_mainGameMode) != 0) return;
    call_game(objFreeObject, ctx, { obj });
}

// jfg_object_set_scale(void* object, float scale): its size, 1.0 = as its definition makes it.
void object_set_scale(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    // o32: after a pointer argument a float comes in $a1, not $f14.
    float scale;
    const uint32_t bits = (uint32_t)ctx->r5;
    std::memcpy(&scale, &bits, 4);
    if (valid_ptr(obj) && scale == scale) wrf(obj + OBJ_SCALE, std::clamp(scale, 0.0f, 100.0f));
}

// jfg_object_scale(void* object): its size.
void object_scale(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    ctx->f0.fl = valid_ptr(obj) ? rdf(obj + OBJ_SCALE) : 0.0f;
}

// jfg_object_physics(void* object): its physics flags (1 in water, 2 at rest, 4 riding a moving object), -1 when it
// has no physics.
void object_physics(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t phys = valid_ptr(obj) ? rd32(obj + OBJ_PHYSICS) : 0;
    ctx->r2 = valid_ptr(phys) ? (gpr)rd8(phys) : (gpr)-1;
}

// jfg_explosion(const JfgExplosion* e): { s32 type; f32 x, y, z; void* owner; } one of the game's explosions
// (explosionStart: its look, sound, blast and damage; 0 the grenade's, 4 under water), credited to owner (a hero, or
// NULL).
void explosion(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t e = (uint32_t)ctx->r4;
    if (!mod_ptr(e) || rd32(ADDR_mainGameMode) != 0) return;
    const int type = (int32_t)rd32(e);
    const uint32_t owner = rd32(e + 16);
    if (type < 0) return;
    call_game(explosionStart, ctx, { (uint32_t)type, s16_arg(rdf(e + 4)), s16_arg(rdf(e + 8)), s16_arg(rdf(e + 12)), 8,
                                     valid_ptr(owner) ? owner : 0 });
}

// jfg_camera_shake(s32 magnitude, s32 ms): the first player's view jolts up and down by up to magnitude level units,
// fading out over ms (camStartShake(0, attack 0, sustain 0, decay s, magnitude), decomp MSR 10a: the game's strong hits
// use 15-25 over 2-4 s). A new call replaces the running shake; 0 stops it; a level change ends it.
void camera_shake(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (rd32(ADDR_mainGameMode) != 0) return;
    const int magnitude = std::clamp((int)(int32_t)ctx->r4, 0, 100);
    const float seconds = std::clamp((int)(int32_t)ctx->r5, 0, 10000) / 1000.0f;
    // o32: the first argument is an int, so the floats go in a1..a3 and magnitude on the stack.
    call_game(camStartShake, ctx, { 0, float_bits(0.0f), float_bits(0.0f), float_bits(seconds), (uint32_t)magnitude });
}

// jfg_blast(const JfgBlast* b): { f32 x, y, z, radius, damage, seconds; void* owner; } damage only (explosionAddBlast:
// strongest at the middle, less out to the radius, fading over the seconds), to whatever it reaches; 16 at a time.
void blast(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t b = (uint32_t)ctx->r4;
    if (!mod_ptr(b) || rd32(ADDR_mainGameMode) != 0) return;
    const float radius = std::max(1.0f, rdf(b + 12)), seconds = std::max(1.0f / 60.0f, rdf(b + 20));
    const uint32_t owner = rd32(b + 24);
    const recomp_context saved = *ctx;
    ctx->f12.fl = rdf(b);     // x and y go in $f12 / $f14 (o32: leading floats), the rest as words
    ctx->f14.fl = rdf(b + 4);
    call_game(explosionAddBlast, ctx, { 0, 0, rd32(b + 8), float_bits(radius), rd32(b + 16), float_bits(seconds), 0,
                                        valid_ptr(owner) ? owner : 0 });
    *ctx = saved;
}

// jfg_shockwave(const JfgShockwave* w): { f32 x, y, z, inner, outer, inner_speed, outer_speed; s32 hold, fade; } the
// game's flat ring effect (shockwaveStart, object 0x73): its inner and outer radius grow by their speeds each tick
// (negative: it shrinks, inward), it holds for hold ticks, then fades over fade ticks. Radii and speeds within +-100.
void shockwave(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t w = (uint32_t)ctx->r4;
    if (!mod_ptr(w) || rd32(ADDR_mainGameMode) != 0) return;
    const auto lim = [&](uint32_t at) { return float_bits(std::clamp(rdf(at), -100.0f, 100.0f)); };
    call_game(shockwaveStart, ctx, { s16_arg(rdf(w)), s16_arg(rdf(w + 4)), s16_arg(rdf(w + 8)) & 0xFFFF, lim(w + 12),
                                     lim(w + 16), lim(w + 20), lim(w + 24),
                                     (uint32_t)std::clamp((int32_t)rd32(w + 28), 0, 0x7FFF),
                                     (uint32_t)std::clamp((int32_t)rd32(w + 32), 0, 255) });
}

// jfg_enemy_traits(void* enemy, JfgEnemyTraits* out): { s32 full_health (256 a unit: the health its squadron gives
// each member), health; f32 radius (its hit sphere), scale, height (above the floor under it: flyers are high),
// floor; }. 1, or 0 if it is not a squad member.
void enemy_traits(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, out = (uint32_t)ctx->r5;
    ctx->r2 = 0;
    if (!valid_ptr(obj) || !mod_ptr(out) || Obj::Kind::get(obj) != KIND_SQUAD_MEMBER) return;
    const uint32_t data = Obj::Data::get(obj), dmg = Obj::Damage::get(obj);
    const uint32_t squadron = valid_ptr(data) ? SquadMemberData::Squadron::get(data) : 0;
    const uint32_t sdata = valid_ptr(squadron) ? Obj::Data::get(squadron) : 0;
    const int health = valid_ptr(dmg) ? Damage::Health::get(dmg) : 0;
    int full = valid_ptr(sdata) ? SquadronData::MemberHealth::get(sdata) : 0;
    if (full < health) full = health;
    const float x = Obj::PosX::get(obj), y = Obj::PosY::get(obj), z = Obj::PosZ::get(obj);
    float floor = y;
    // Straight down from a little above it: the first track surface below (the result's point at +0x04).
    const uint32_t q = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u;
    wrf(q, x); wrf(q + 4, y + 5.0f); wrf(q + 8, z);
    wrf(q + 12, x); wrf(q + 16, y - 3000.0f); wrf(q + 20, z);
    const bool found = (call_game(trackNearestIntersection, ctx, { 0u, q, q + 12, q + 24, 0u, 0u }) & 0xFF) != 0;
    g_rdram = rdram_;
    if (found) floor = rdf(q + 24 + 8);
    wr32(out + 0, (uint32_t)full);
    wr32(out + 4, (uint32_t)health);
    wrf(out + 8, valid_ptr(dmg) ? Damage::Radius::get(dmg) : 0.0f);
    wrf(out + 12, rdf(obj + OBJ_SCALE));
    wrf(out + 16, y - floor);
    wrf(out + 20, floor);
    ctx->r2 = 1;
}

// jfg_enemy_fling(void* enemy, const JfgFling* f): { f32 vx, vy, vz; s32 calls; } -> 1 if flung. The game's own
// launch (decomp MSR 10c): the member's velocity (units a squad call), track collision (b18) and b28, which keeps its
// AI off for `calls` calls (data +0x17) or until it lands. Not for a member with no collision data (definition +0x50),
// one the mover skips (b25), or one riding something (b9 without byte 3 b4): 0, and the caller moves it if it wants.
constexpr uint32_t SQ_FLAGS0 = 0x00, SQ_FLAGS1 = 0x01, SQ_FLAGS2 = 0x02, SQ_FLAGS3 = 0x03, SQ_CALLS = 0x17;
void enemy_fling(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4, f = (uint32_t)ctx->r5;
    ctx->r2 = 0;
    if (!valid_ptr(obj) || !mod_ptr(f) || Obj::Kind::get(obj) != KIND_SQUAD_MEMBER) return;
    const uint32_t sq = Obj::Data::get(obj), def = rd32(obj + 0x40);
    if (!valid_ptr(sq) || !valid_ptr(def) || rd32(def + 0x50) == 0) return;
    if ((rd8(sq + SQ_FLAGS0) & 0x02) || ((rd8(sq + SQ_FLAGS2) & 0x02) && !(rd8(sq + SQ_FLAGS3) & 0x10))) return;
    wrf(obj + 0x1C, rdf(f));
    wrf(obj + 0x20, rdf(f + 4));
    wrf(obj + 0x24, rdf(f + 8));
    wr8(sq + SQ_FLAGS1, (uint8_t)(rd8(sq + SQ_FLAGS1) | 0x04));
    wr8(sq + SQ_CALLS, (uint8_t)std::clamp((int32_t)rd32(f + 12), 1, 255));
    wr8(sq + SQ_FLAGS0, (uint8_t)(rd8(sq + SQ_FLAGS0) | 0x10));
    ctx->r2 = 1;
}

// jfg_enemy_flung(void* enemy): 1 while a squad member is launched (b28: flung by anyone, the game included).
void enemy_flung(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t sq = valid_ptr(obj) && Obj::Kind::get(obj) == KIND_SQUAD_MEMBER ? Obj::Data::get(obj) : 0;
    ctx->r2 = (gpr)(int32_t)(valid_ptr(sq) && (rd8(sq + SQ_FLAGS0) & 0x10) ? 1 : 0);
}

// jfg_shocker_end(void* hero, float* out): 1 while the hero's shocker beam is on, with where it ends (x, y, z: what
// it touches, or its full reach) in out.
void shocker_end(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4), out = (uint32_t)ctx->r5;
    const uint32_t beam = data != 0 ? Hero::Beam::get(data) : 0;
    const uint32_t bdata = valid_ptr(beam) ? Obj::Data::get(beam) : 0;
    if (!valid_ptr(bdata) || Beam::Active::get(bdata) == 0) { ctx->r2 = 0; return; }
    if (out != 0) {
        wrf(out + 0, Beam::EndX::get(bdata));
        wrf(out + 4, Beam::EndY::get(bdata));
        wrf(out + 8, Beam::EndZ::get(bdata));
    }
    ctx->r2 = 1;
}

// jfg_flames_get(void* hero, float* out, int max): where the hero's flamethrower flames are now (x, y, z each, up to
// max of them); returns how many there are (0 while it is not firing).
void flames_get(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t data = hero_data((uint32_t)ctx->r4), out = (uint32_t)ctx->r5;
    const int max = (int)ctx->r6;
    const uint32_t block = data != 0 ? Hero::Flames::get(data) : 0;
    int n = 0;
    if (valid_ptr(block)) {
        for (int i = 0; i < FLAMES; i++) {
            const uint32_t p = block + FLAME_FIRST + (uint32_t)i * FLAME_SIZE;
            if (rd8(p + FLAME_LIFE) == 0) continue;
            if (out != 0 && n < max) {
                for (uint32_t k = 0; k < 3; k++) wr32(out + (uint32_t)n * 12 + k * 4, rd32(p + FLAME_X + k * 4));
            }
            n++;
        }
    }
    ctx->r2 = (gpr)n;
}
} // namespace

// The input thread, after the controller is read (src/input/controls.cpp, jfg_dual_stick after Swap Triggers): the
// mods' toggled buttons (jfg_input_toggle) flip on each press and read as held while on.
extern "C" void jfg_input_toggles(uint16_t* buttons) {
    const uint16_t mask = g_toggle_mask.load(std::memory_order_relaxed);
    const uint16_t release = g_toggle_release.exchange(0, std::memory_order_relaxed);
    static uint16_t prev = 0;
    const uint16_t b = *buttons;
    uint16_t latched = (uint16_t)(g_toggle_latched.load(std::memory_order_relaxed) & ~release & mask);
    latched ^= (uint16_t)(b & ~prev & mask); // presses flip
    prev = b;
    g_toggle_latched.store(latched, std::memory_order_relaxed);
    *buttons = (uint16_t)(((b & ~mask) | latched) & ~g_blocked.load(std::memory_order_relaxed));
}

void note_blast_damage(uint32_t dmg, int damage) {
    for (int i = 0; i < g_blast_count; i++) {
        if (g_blasts[i].dmg == dmg) { g_blasts[i].damage += damage; return; }
    }
    if (g_blast_count == 16) { // the oldest goes
        for (int i = 1; i < 16; i++) g_blasts[i - 1] = g_blasts[i];
        g_blast_count--;
    }
    g_blasts[g_blast_count++] = { dmg, damage };
}

// While a mod sets the camera (jfg_camera_set): the port's own camera features (the dual-stick and over-the-shoulder
// views, the FOV option) leave it alone; mod_camera_fov is its field of view, 0 = the game's.
bool mod_camera_active() {
    return g_camera_ticks > 0;
}

float mod_camera_fov() {
    return g_camera_ticks > 0 ? g_camera_set.fov : 0.0f;
}

// The hero's camera step counts a mod's camera down, but on the title screen and in the front end no hero runs: a
// custom hero's camera (its field of view) stayed on there. Each game tick counts it down too.
void mod_camera_tick() {
    if (g_camera_ticks > 0) g_camera_ticks--;
}

bool hero_taken_over(int character) {
    return character >= 0 && character < CHARACTERS && g_takeover[character];
}

// Weapon steps from the mouse wheel for a hero a mod moves (jfg_controls.cpp), made in its control tick.
int g_weapon_steps = 0;
void queue_weapon_step(int direction) { g_weapon_steps += direction; }

// jfg_camera_set_distance(float scale): the port's dual-stick camera stands scale times as far out (on top of the
// player's Camera Distance option); 1 = as usual. Kept until set again.
float g_camera_distance = 1.0f;
void camera_set_distance(uint8_t*, recomp_context* ctx) {
    const float scale = ctx->f12.fl;
    g_camera_distance = (scale > 0.1f && scale < 10.0f) ? scale : 1.0f;
}

bool hero_mod_controlled(int character) {
    return hero_taken_over(character) && !g_game_control[character];
}

bool hero_health_hidden(int character) {
    return character >= 0 && character < CHARACTERS && g_hide_health[character];
}

void register_hero_api() {
    recomp::overlays::register_base_export("jfg_hero_takeover", hero_takeover);
    recomp::overlays::register_base_export("jfg_input_get", input_get);
    recomp::overlays::register_base_export("jfg_camera_get", camera_get);
    recomp::overlays::register_base_export("jfg_level_triangles", level_triangles);
    recomp::overlays::register_base_export("jfg_level_floor", level_floor);
    recomp::overlays::register_base_export("jfg_damage_sphere", damage_sphere);
    recomp::overlays::register_base_export("jfg_objects_near", objects_near);
    recomp::overlays::register_base_export("jfg_object_triangles", object_triangles);
    recomp::overlays::register_base_export("jfg_screen_flash", screen_flash);
    recomp::overlays::register_base_export("jfg_camera_set", camera_set);
    recomp::overlays::register_base_export("jfg_object_health", object_health);
    recomp::overlays::register_base_export("jfg_object_set_health", object_set_health);
    recomp::overlays::register_base_export("jfg_enemy_type", enemy_type);
    recomp::overlays::register_base_export("jfg_enemies_near", enemies_near);
    recomp::overlays::register_base_export("jfg_object_blast_damage", object_blast_damage);
    recomp::overlays::register_base_export("jfg_hud_hide_health", hud_hide_health);
    recomp::overlays::register_base_export("jfg_hud_in_corners", hud_in_corners_export);
    recomp::overlays::register_base_export("jfg_hero_heads", hero_heads);
    recomp::overlays::register_base_export("jfg_hero_game_control", hero_game_control);
    recomp::overlays::register_base_export("jfg_hero_select_weapon", hero_select_weapon);
    recomp::overlays::register_base_export("jfg_controls_get", controls_get);
    recomp::overlays::register_base_export("jfg_camera_set_distance", camera_set_distance);
    recomp::overlays::register_base_export("jfg_input_toggle", input_toggle);
    recomp::overlays::register_base_export("jfg_input_toggle_release", input_toggle_release);
    recomp::overlays::register_base_export("jfg_input_toggled", input_toggled);
    recomp::overlays::register_base_export("jfg_input_block", input_block);
    recomp::overlays::register_base_export("jfg_hero_get", hero_get);
    recomp::overlays::register_base_export("jfg_hero_state", hero_state);
    recomp::overlays::register_base_export("jfg_hero_weapon", hero_weapon);
    recomp::overlays::register_base_export("jfg_hero_ammo", hero_ammo);
    recomp::overlays::register_base_export("jfg_game_paused", game_paused);
    recomp::overlays::register_base_export("jfg_hero_set_ammo", hero_set_ammo);
    recomp::overlays::register_base_export("jfg_enemy_dying", enemy_dying);
    recomp::overlays::register_base_export("jfg_hero_push", hero_push);
    recomp::overlays::register_base_export("jfg_hero_protect", hero_protect);
    recomp::overlays::register_base_export("jfg_hero_roll", hero_roll);
    recomp::overlays::register_base_export("jfg_hero_move", hero_move);
    recomp::overlays::register_base_export("jfg_hero_speak", hero_speak);
    recomp::overlays::register_base_export("jfg_objects_of_kind", objects_of_kind);
    recomp::overlays::register_base_export("jfg_game_sound", game_sound);
    recomp::overlays::register_base_export("jfg_game_sound_flat", game_sound_flat);
    recomp::overlays::register_base_export("jfg_hero_hit", hero_hit);
    recomp::overlays::register_base_export("jfg_floyd_get", floyd_get);
    recomp::overlays::register_base_export("jfg_fire_bolt", fire_bolt);
    recomp::overlays::register_base_export("jfg_fire_plasma", fire_plasma);
    recomp::overlays::register_base_export("jfg_level_ray", level_ray);
    recomp::overlays::register_base_export("jfg_line_of_sight", line_of_sight);
    recomp::overlays::register_base_export("jfg_enemy_alert", enemy_alert);
    recomp::overlays::register_base_export("jfg_enemy_ignite", enemy_ignite);
    recomp::overlays::register_base_export("jfg_enemy_electrocute", enemy_electrocute);
    recomp::overlays::register_base_export("jfg_fx_fire", fx_fire);
    recomp::overlays::register_base_export("jfg_shocker_end", shocker_end);
    recomp::overlays::register_base_export("jfg_flames_get", flames_get);
    recomp::overlays::register_base_export("jfg_spawn", spawn);
    recomp::overlays::register_base_export("jfg_asset_number", asset_number_api);
    recomp::overlays::register_base_export("jfg_world_to_screen", world_to_screen);
    recomp::overlays::register_base_export("jfg_object_free", object_free);
    recomp::overlays::register_base_export("jfg_object_set_scale", object_set_scale);
    recomp::overlays::register_base_export("jfg_object_scale", object_scale);
    recomp::overlays::register_base_export("jfg_object_physics", object_physics);
    recomp::overlays::register_base_export("jfg_explosion", explosion);
    recomp::overlays::register_base_export("jfg_camera_shake", camera_shake);
    recomp::overlays::register_base_export("jfg_blast", blast);
    recomp::overlays::register_base_export("jfg_shockwave", shockwave);
    recomp::overlays::register_base_export("jfg_enemy_traits", enemy_traits);
    recomp::overlays::register_base_export("jfg_enemy_fling", enemy_fling);
    recomp::overlays::register_base_export("jfg_enemy_flung", enemy_flung);
}

} // namespace jfg::api

// boyControl(obj, updateRate), Juno's control (overlay 16): while a mod has taken him over (jfg_hero_takeover), the
// controller is read as the game does, then jfg_on_hero_control runs in place of the game's code, and the follow
// camera's bookkeeping (old position, body yaw, speed) is filled from what the mod did. A dead hero (health under
// one unit) runs the game's code, so death and the restart are the game's own; so does a hero the mod hands back
// (jfg_hero_game_control), from where the mod left it.
// Where the game's own control code left the hero last (see the hand-over below).
// (In this level, for this hero object: the game sets the points up afresh at a level's start.)
static float g_game_left[3];
static bool g_game_left_valid = false;
static uint32_t g_game_left_obj = 0;
static int g_game_left_level = -1;

static void note_game_left(uint32_t obj) {
    using namespace jfg;
    g_game_left_obj = obj;
    g_game_left_level = (int16_t)rd16(ADDR_currentLevel);
    g_game_left[0] = Obj::PosX::get(obj);
    g_game_left[1] = Obj::PosY::get(obj);
    g_game_left[2] = Obj::PosZ::get(obj);
    g_game_left_valid = true;
}

JFG_WRAP(boyControl) {
    using namespace jfg;
    constexpr int JUNO = 1;
    g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t hero = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    const uint32_t dmg = valid_ptr(obj) ? Obj::Damage::get(obj) : 0;
    const bool dead = valid_ptr(dmg) && Damage::Health::get(dmg) < 0x100;
    if (!api::hero_taken_over(JUNO) || !valid_ptr(hero) || dead) {
        boyControl_orig(rdram, ctx);
        g_rdram = rdram;
        if (valid_ptr(obj)) note_game_left(obj);
        return;
    }
    const int frames = (int)ctx->r5;
    if (!g_game_left_valid || g_game_left_obj != obj || g_game_left_level != (int16_t)rd16(ADDR_currentLevel)) {
        note_game_left(obj); // new level: the game set its points up where it put the hero
    }
    call_game(controlReadJoypad, ctx, { hero });
    const float x0 = Obj::PosX::get(obj), y0 = Obj::PosY::get(obj), z0 = Obj::PosZ::get(obj);
    // A weapon change shows the weapon list once: the game's control code clears its request bits (+0x540, 0x30) at
    // the start of each tick, before its weapon buttons may set them again; the HUD scrolls (and ticks) while they are
    // set. Here likewise, then the wheel's and the mod's changes.
    if (!api::g_game_control[JUNO]) {
        wr8(hero + 0x540, (uint8_t)(rd8(hero + 0x540) & ~0x30));
        for (; api::g_weapon_steps != 0; api::g_weapon_steps += api::g_weapon_steps > 0 ? -1 : 1) {
            call_game(api::g_weapon_steps > 0 ? controlSelectNextWeapon : controlSelectPreviousWeapon, ctx, { obj, hero });
        }
    }
    api::g_weapon_steps = 0;
    api::fire(rdram, ctx, api::Event::HeroControl, (int32_t)obj, frames, JUNO);
    g_rdram = rdram;
    namespace H = api::Hero;
    if (!api::g_game_control[JUNO]) {
        // The weapon the control code holds the hero to (+0x585: it changes back to it when it next runs) is the one in
        // hand, so a weapon changed meanwhile stays.
        const uint32_t ch = character_ptr(JUNO);
        if (ch != 0) wr8(hero + 0x585, (uint8_t)Character::CurrentWeapon::get(ch));
    }
    static bool game_ran = false; // the game's code ran the hero last tick
    if (api::g_game_control[JUNO]) {
        if (!game_ran) {
            // Handed back: the copies of the facing the control code keeps (and writes back each tick) take the
            // object's, which the mod has been turning.
            const uint16_t yaw = Obj::Yaw::get(obj);
            for (uint32_t k : { 0x106u, 0x11Cu, 0x122u, 0x1C6u, 0x1CAu }) wr16(hero + k, yaw);
            H::OldX::set(hero, x0);
            H::OldY::set(hero, y0);
            H::OldZ::set(hero, z0);
            // On its feet and at rest: the speeds the control code keeps (forward, sideways, vertical) and the
            // object's velocity are whatever the mod or an earlier run of the code left, and a jump or a fall from then
            // would carry on (an upward speed launched the hero into the air).
            for (uint32_t k : { 0x1Cu, 0x20u, 0x24u }) wrf(obj + k, 0.0f);
            for (uint32_t k : { 0x04u, 0x08u, 0x10u, 0x74u }) wrf(hero + k, 0.0f);
            wr8(hero + 0x185, 0); // ticks without ground
            // Lift: +0x594 scales gravity down ((1 - lift) x gravity), up to 1.5 from the water's buoyancy or the
            // jetpack's hover (+0x14A), and is only cleared in some states: left over from the last swim or hover, the
            // hero fell upwards, faster and faster. +0x57C: where a fall began (the fall damage measures from it).
            wrf(hero + 0x594, 0.0f);
            wr8(hero + 0x14A, 0);
            wrf(hero + 0x57C, y0);
            // The collision sweeps the hero's spheres from where they were at the end of its last tick under the
            // game's code (+0x424, entries 8-15, world space) to where they are now (+0x364, 8-15): from where the
            // game last left it, across whatever lies between, so it was pushed out of slopes (high into the air) or
            // judged stuck and put back far away. Both move along with the hero to where the mod left it.
            if (g_game_left_valid && g_game_left_obj == obj && g_game_left_level == (int16_t)rd16(ADDR_currentLevel)) {
                const float d[3] = { x0 - g_game_left[0], y0 - g_game_left[1], z0 - g_game_left[2] };
                for (uint32_t i = 8; i < 16; i++) {
                    for (uint32_t base : { 0x364u, 0x424u }) {
                        const uint32_t p = hero + base + 12u * i;
                        for (uint32_t c = 0; c < 3; c++) wrf(p + 4 * c, rdf(p + 4 * c) + d[c]);
                    }
                }
            }
            const uint8_t state = PlayerData::State::get(hero);
            if ((state & ~0x20u) == PlayerData::StateJumping || state == PlayerData::StateAiming) PlayerData::State::set(hero, 0);
        }
        const bool first = !game_ran;
        game_ran = true;
        ctx->r4 = (gpr)(int32_t)obj;
        ctx->r5 = (gpr)frames;
        boyControl_orig(rdram, ctx);
        g_rdram = rdram;
        note_game_left(obj);
        if (first && Obj::VelY::get(obj) > 0.0f) {
            // The control code takes the hero's speed from how far it moved this tick. On uneven ground the hero the
            // mod left stands a little into the slope for the game's own collision, which pushes it out (130 units
            // up, seen on a forest slope): that push is not a jump, and as a speed it launched the hero high into the
            // air (the same rule gives the game's jumps from a slope their extra height).
            Obj::VelY::set(obj, 0.0f);
        }
        return;
    }
    if (game_ran && PlayerData::State::get(hero) == PlayerData::StateAiming) {
        // Taken back while the game's code had it aiming: it is out of its aim (the next hand-over starts one
        // afresh), so the port's aiming options leave the stick alone.
        PlayerData::State::set(hero, 0);
    }
    game_ran = false;
    // The hero's hit spheres sit on its posed joints (ModelInstance::Positions, written when the game's code poses the
    // model), so they would stay where the hero last stood: move them with the hero. If they are not near it (never
    // posed), stand them in a column from its feet up.
    const uint32_t models = Obj::Models::get(obj);
    const uint32_t instance = valid_ptr(models) ? rd32(models) : 0;
    const uint32_t points = valid_ptr(instance) ? ModelInstance::Positions::get(instance) : 0;
    const int spheres = valid_ptr(dmg) ? Damage::SphereCount::get(dmg) : 0;
    if (valid_ptr(points) && spheres > 0 && spheres <= 32) {
        const float x = Obj::PosX::get(obj), y = Obj::PosY::get(obj), z = Obj::PosZ::get(obj);
        const float mx = rdf(points) + x - x0, mz = rdf(points + 8) + z - z0;
        const bool near = (mx - x) * (mx - x) + (mz - z) * (mz - z) < 80.0f * 80.0f;
        for (int i = 0; i < spheres; i++) {
            const uint32_t p = points + 12u * (uint32_t)i;
            if (near) {
                wrf(p + 0, rdf(p + 0) + x - x0);
                wrf(p + 4, rdf(p + 4) + y - y0);
                wrf(p + 8, rdf(p + 8) + z - z0);
            } else {
                wrf(p + 0, x);
                wrf(p + 4, y + 8.0f + 48.0f * (float)i / (float)std::max(1, spheres - 1));
                wrf(p + 8, z);
            }
        }
    }
    H::OldX::set(hero, x0);
    H::OldY::set(hero, y0);
    H::OldZ::set(hero, z0);
    H::BodyYaw::set(hero, Obj::Yaw::get(obj));
    const float dx = Obj::PosX::get(obj) - x0, dz = Obj::PosZ::get(obj) - z0;
    H::Speed::set(hero, std::sqrt(dx * dx + dz * dz) / (float)std::max(1, frames));
    // The hero's track segment: the game's move code keeps it (objMoveObject: trackGetBlock when the object leaves its
    // segment's box), and objects are drawn only in the segments seen from the camera's, so a hero the mod moved into
    // another segment would vanish. trackGetBlock(x $f12, y $f14, z a2): the box holding x/z with the nearest y centre.
    {
        const recomp_context saved = *ctx;
        ctx->f12.u32l = rd32(obj + 0x0C);
        ctx->f14.u32l = rd32(obj + 0x10);
        const int32_t block = (int32_t)call_game(trackGetBlock, ctx, { 0u, 0u, rd32(obj + 0x14) });
        *ctx = saved;
        if (block >= 0) Obj::Segment::set(obj, (int16_t)block);
    }
}

// The hero's camera step (controlPlayer, after the hero's control: func_8002B418_2C018(obj, player, rate)): the game's
// own camera code, then a camera a mod set this tick (jfg_camera_set), in the game's angles: with d = position - focus,
// yaw 0x8000 - atan2(dx, dz) and pitch atan2(dy, |dxz|), as the game's follow camera aims (charControl.c).
JFG_WRAP(func_8002B418_2C018) {
    using namespace jfg;
    func_8002B418_2C018_orig(rdram, ctx);
    api::CameraSet& s = api::g_camera_set;
    if (!s.pending) {
        if (api::g_camera_ticks > 0) api::g_camera_ticks--;
        return;
    }
    s.pending = false;
    api::g_camera_ticks = 3; // (the game tick counts it down too: mod_camera_tick)
    g_rdram = rdram;
    const uint32_t cam = api::camera_ptr();
    const float dx = s.x - s.fx, dy = s.y - s.fy, dz = s.z - s.fz;
    constexpr float TO_ANGLE = 32768.0f / PI_F;
    Camera::PosX::set(cam, s.x);
    Camera::PosY::set(cam, s.y);
    Camera::PosZ::set(cam, s.z);
    Camera::Yaw::set(cam, (uint16_t)(int32_t)(0x8000 - (int32_t)std::lround(std::atan2(dx, dz) * TO_ANGLE)));
    Camera::Pitch::set(cam, (uint16_t)(int32_t)std::lround(std::atan2(dy, std::sqrt(dx * dx + dz * dz)) * TO_ANGLE));
    Camera::Roll::set(cam, (uint16_t)s.roll);
    if (s.fov > 0.0f) {
        Camera::Fov::set(cam, s.fov);
        const recomp_context saved = *ctx;
        ctx->f12.fl = s.fov;
        call_game(camSetFOV, ctx, { 0u, 1u }); // camSetFOV(fov in $f12, force): the wrapper passes it unscaled
        *ctx = saved;
    }
}

// func_8002AA7C_2B67C(damage data, damage): a blast's damage to one object (the blast rule: spread over its hit
// spheres); returns what it dealt. Noted for jfg_object_blast_damage.
JFG_WRAP(func_8002AA7C_2B67C) {
    using namespace jfg;
    const uint32_t dmg = (uint32_t)ctx->r4;
    func_8002AA7C_2B67C_orig(rdram, ctx);
    g_rdram = rdram;
    const int dealt = (int)(int32_t)ctx->r2;
    if (dealt > 0 && valid_ptr(dmg)) api::note_blast_damage(dmg, dealt);
}
