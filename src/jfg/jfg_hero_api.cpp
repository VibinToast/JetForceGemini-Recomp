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
extern "C" void frontFlashScreen(uint8_t* rdram, recomp_context* ctx);
extern "C" void camSetFOV(uint8_t* rdram, recomp_context* ctx);

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
} // namespace

// While a mod sets the camera (jfg_camera_set): the port's own camera features (the dual-stick and over-the-shoulder
// views, the FOV option) leave it alone; mod_camera_fov is its field of view, 0 = the game's.
bool mod_camera_active() {
    return g_camera_ticks > 0;
}

float mod_camera_fov() {
    return g_camera_ticks > 0 ? g_camera_set.fov : 0.0f;
}

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
    recomp::overlays::register_base_export("jfg_object_triangles", object_triangles);
    recomp::overlays::register_base_export("jfg_screen_flash", screen_flash);
    recomp::overlays::register_base_export("jfg_camera_set", camera_set);
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
    api::g_camera_ticks = 2;
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
