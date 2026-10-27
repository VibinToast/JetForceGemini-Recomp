// Jet Force Gemini port: the hero part of the mod API (API version 4): a mod can take over a hero's control, read the
// controller and camera, read the level's collision triangles and floors, and hurt objects. Built for the custom-hero
// test (Recomp-Docs MARIO_TEST.md); the layouts are in mod_api/jfg_api.h.
#include <algorithm>
#include <cmath>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" void controlReadJoypad(uint8_t* rdram, recomp_context* ctx);
extern "C" void trackPolyHeight(uint8_t* rdram, recomp_context* ctx);

namespace jfg::api {
namespace {
constexpr uint32_t ADDR_track = 0x800A0D60;      // Track*, the level's track (track.c)
constexpr uint32_t ADDR_controlcam = 0x800F6DC0; // Camera*, the camera the hero's control steers by
constexpr uint32_t ADDR_controlXjoy = 0x800F6DAC, ADDR_controlYjoy = 0x800F6DB0; // s32, filled by controlReadJoypad
constexpr uint32_t ADDR_ObjList = 0x800F2CA4, ADDR_ObjListCount = 0x800F2CA8;
constexpr int CHARACTERS = 3; // Vela, Juno, Lupus

// The hero's per-tick data (Obj::Data of a hero, ControlPlayer in the decomp's charControl.h).
namespace Hero {
using Speed    = Field<float, 0x04>;    // forward speed, read by the follow camera
using OldX     = Field<float, 0x3C>;    // the position before this tick's move
using OldY     = Field<float, 0x40>;
using OldZ     = Field<float, 0x44>;
using BodyYaw  = Field<uint16_t, 0x11C>;
}

bool g_takeover[CHARACTERS] = {};

// JfgInput
constexpr uint32_t IN_STICK_X = 0x00, IN_STICK_Y = 0x04, IN_RAW_X = 0x08, IN_RAW_Y = 0x0C, IN_HELD = 0x10,
                   IN_PRESSED = 0x14, IN_CAMERA_YAW = 0x18;
// JfgTriangle
constexpr uint32_t TRI_VERTS = 0x00, TRI_FLAGS = 0x14, TRI_TEXTURE = 0x18, TRI_SIZE = 0x1C;
// JfgFloor
constexpr uint32_t FL_X = 0x00, FL_Z = 0x04, FL_MASK = 0x08, FL_Y = 0x0C, FL_FLAGS = 0x10;
// JfgDamageSphere
constexpr uint32_t DS_X = 0x00, DS_Y = 0x04, DS_Z = 0x08, DS_RADIUS = 0x0C, DS_DAMAGE = 0x10, DS_SOURCE = 0x14;

uint32_t camera_ptr() {
    const uint32_t cam = rd32(ADDR_controlcam);
    if (valid_ptr(cam)) return cam;
    return ADDR_cameraActorArray + Camera::Size * (rd32(ADDR_cameraNo) & 3u);
}

// jfg_hero_takeover(int character, int on): while on, the game's control code for that hero doesn't run;
// jfg_on_hero_control fires in its place. Only Juno (1) so far.
void hero_takeover(uint8_t*, recomp_context* ctx) {
    const int character = (int)ctx->r4;
    if (character >= 0 && character < CHARACTERS) g_takeover[character] = ctx->r5 != 0;
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

// jfg_damage_sphere(const JfgDamageSphere* d): takes damage (256 = one unit of health) from every object with health
// whose hit centre is within the sphere (plus its own hit radius), except source; returns how many were hit.
void damage_sphere(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t d = (uint32_t)ctx->r4;
    if (d == 0) { ctx->r2 = 0; return; }
    const float x = rdf(d + DS_X), y = rdf(d + DS_Y), z = rdf(d + DS_Z), radius = rdf(d + DS_RADIUS);
    const int damage = (int)rd32(d + DS_DAMAGE);
    const uint32_t source = rd32(d + DS_SOURCE);
    const uint32_t list = rd32(ADDR_ObjList);
    const int count = (int)rd32(ADDR_ObjListCount);
    int hit = 0;
    if (valid_ptr(list) && count > 0 && count < 4096) {
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
} // namespace

bool hero_taken_over(int character) {
    return character >= 0 && character < CHARACTERS && g_takeover[character];
}

void register_hero_api() {
    recomp::overlays::register_base_export("jfg_hero_takeover", hero_takeover);
    recomp::overlays::register_base_export("jfg_input_get", input_get);
    recomp::overlays::register_base_export("jfg_camera_get", camera_get);
    recomp::overlays::register_base_export("jfg_level_triangles", level_triangles);
    recomp::overlays::register_base_export("jfg_level_floor", level_floor);
    recomp::overlays::register_base_export("jfg_damage_sphere", damage_sphere);
    recomp::overlays::register_base_export("jfg_objects_near", objects_near);
    recomp::overlays::register_base_export("jfg_object_health", object_health);
    recomp::overlays::register_base_export("jfg_object_set_health", object_set_health);
}

} // namespace jfg::api

// boyControl(obj, updateRate), Juno's control (overlay 16): while a mod has taken him over (jfg_hero_takeover), the
// controller is read as the game does, then jfg_on_hero_control runs in place of the game's code, and the follow
// camera's bookkeeping (old position, body yaw, speed) is filled from what the mod did. A dead hero (health under
// one unit) runs the game's code, so death and the restart are the game's own.
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
        return;
    }
    const int frames = (int)ctx->r5;
    call_game(controlReadJoypad, ctx, { hero });
    const float x0 = Obj::PosX::get(obj), y0 = Obj::PosY::get(obj), z0 = Obj::PosZ::get(obj);
    api::fire(rdram, ctx, api::Event::HeroControl, (int32_t)obj, frames, JUNO);
    g_rdram = rdram;
    namespace H = api::Hero;
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
}
