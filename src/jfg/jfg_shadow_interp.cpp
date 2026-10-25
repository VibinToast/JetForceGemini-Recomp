// Jet Force Gemini port: character shadows with in-between frames ("Smooth Shadows", Graphics tab).
//
// The game draws a character's shadow on copies of the ground triangles under it, which never move: once per game
// frame func_8001FA24 (called by shadowGenerate for each shadow) recomputes their texture coordinates so that the
// small shadow image sits under the character. For a corner at (x, z), with the object at (objX, objZ):
//   s = ((x - objX) cos a - (z - objZ) sin a + A) Su,   t = ((z - objZ) cos a + (x - objX) sin a + B) Sv
// (a: the object's or the light's angle; Su, Sv: image size over shadow size, larger with height; s10.5 texels).
// RT64 blends matrices between game frames but not texture coordinates, so the shadow stepped ahead of the character
// on in-between frames. On those frames the right image is the same mapping with the character's blended position:
// the whole batch shifted by (1 - weight) of what the last game frame's move did to s and t. That shift goes to RT64
// with the batch's triangle addresses (rt64_jfg_shadow_batch), which slides the batch's tile by it.
#include <cstdint>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

void rt64_jfg_shadow_batch(uint32_t start, uint32_t end, float dUls, float dUlt); // lib/rt64 src/gbi/rt64_gbi_f3ddkr.cpp

namespace jfg {

namespace {
// Where each shadowed object was at its last two game frames. Game thread only.
struct ShadowPlace {
    uint32_t object = 0;
    uint32_t tick = 0;
    float x = 0, z = 0, prev_x = 0, prev_z = 0;
    bool has_prev = false;
};
ShadowPlace places[64];

ShadowPlace& place_of(uint32_t object) {
    return places[((object >> 2) * 2654435761u) >> 26];
}

constexpr float MAX_STEP = 100.0f; // a larger move in one game frame is a jump (a warp, a respawn): not slid

// The batch being written by func_8001FA24.
bool batch_open = false;
uint32_t batch_start = 0;
float batch_uls = 0, batch_ult = 0;
}

// func_8001FA24, once the shadow's scales and the object's place are worked out and before its triangles are written:
// $s5 the shadow (+0: the object), $f18 / $f20 the object's x / z, $f14 / $f16 sin / cos of the angle, $f22 / $f24
// the s / t scales, $a3 where its first triangle goes.
extern "C" void jfg_shadow_begin(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    batch_open = false;
    const uint32_t shadow = (uint32_t)ctx->r21;
    if (!valid_ptr(shadow)) return;
    const uint32_t object = rd32(shadow);
    const float x = ctx->f18.fl, z = ctx->f20.fl;

    ShadowPlace& p = place_of(object);
    if (p.object != object) {
        p = ShadowPlace();
        p.object = object;
        p.tick = g_tick_count;
        p.x = x;
        p.z = z;
    }
    else if (p.tick != g_tick_count) {
        p.has_prev = (g_tick_count - p.tick) == 1;
        p.prev_x = p.x;
        p.prev_z = p.z;
        p.x = x;
        p.z = z;
        p.tick = g_tick_count;
    }

    float dx = 0, dz = 0;
    if (g.smooth_shadows && p.has_prev) {
        dx = x - p.prev_x;
        dz = z - p.prev_z;
        if (dx * dx + dz * dz > MAX_STEP * MAX_STEP) dx = dz = 0;
    }
    const float sin_a = ctx->f14.fl, cos_a = ctx->f16.fl;
    const float ds = (dx * cos_a - dz * sin_a) * ctx->f22.fl; // what the move added to s and t (s10.5)
    const float dt = (dz * cos_a + dx * sin_a) * ctx->f24.fl;
    // The tile origin one game frame back, in quarter texels: the image is sampled at s - uls / 4.
    batch_uls = -ds / 8.0f;
    batch_ult = -dt / 8.0f;
    batch_start = (uint32_t)ctx->r7 & 0x00FFFFFFu;
    batch_open = true;
}

// func_8001FA24 after its last triangle ($a3: the end). Batches are registered even with the option off (no shift),
// so an older shift never applies to new triangles at the same place.
extern "C" void jfg_shadow_end(uint8_t* rdram_, recomp_context* ctx) {
    (void)rdram_;
    if (!batch_open) return;
    batch_open = false;
    const uint32_t end = (uint32_t)ctx->r7 & 0x00FFFFFFu;
    if (end > batch_start) rt64_jfg_shadow_batch(batch_start, end, batch_uls, batch_ult);
}

} // namespace jfg
