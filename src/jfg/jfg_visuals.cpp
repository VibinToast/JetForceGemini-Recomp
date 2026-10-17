// Jet Force Gemini port: visual options, for the game hooks and the renderer - widescreen, field of view, the levels'
// own fog (and the hidden depth fog), LOD and draw distance, texture and picture filtering.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_rt64.h"

namespace jfg {

// Read by the renderer thread (src/renderer/renderer.cpp): 1 = render at the window's aspect ratio instead of 4:3.
// RT64 widens perspective projections only, so 2D (HUD, menus, text) stays in the middle 4:3 area. Only in the game
// proper (game mode 0, which includes its pause menus and cutscenes): the front end masks its 3D backdrops with
// 4:3 overlays (the title screen's oval), and the uncovered sides look broken.
extern "C" int jfg_widescreen_active(void) {
    static const bool env = std::getenv("JFG_WIDESCREEN") != nullptr; // for tests
    // The renderer keeps the sides of a widescreen picture empty while the pause screen is up (see FramebufferRenderer).
    rt64_jfg_paused.store((g_rdram != nullptr && rd8(ADDR_pauseMode) != 0) ? 1 : 0, std::memory_order_relaxed);
    if (!(g.widescreen || env) || g_rdram == nullptr) return 0;
    return rd32(ADDR_mainGameMode) == 0 ? 1 : 0;
}

// "Disable LOD". objPrintModelObject draws an object with its next, simpler model (model index + 1) when the object
// is further from the camera than the distance in its object definition (float at header + 0xCC; 0 = no LOD). With the
// option, that distance is set to 0 in the definition of every object in play; the old values are kept and put back
// when the option is turned off (definitions are reloaded with each level, so the record is dropped on a level change).
// Nothing else reads header + 0xCC except the reflection drawing (overlay 137), which makes the same choice.
void apply_no_lod(recomp_context* ctx) {
    // Two fields of an object definition, each set to 0 while its option is on (originals kept to put back):
    //   + 0xCC float: LOD distance (objPrintModelObject); + 0x16 s16: draw distance (track.c func_80015ED8 skips the
    //   object beyond it, and fades it out just before; 0 = always drawn).
    struct Field { uint32_t offset; uint32_t size; const bool* option; std::unordered_map<uint32_t, uint32_t> saved; };
    static Field fields[2] = { { 0xCC, 4, &g.no_lod, {} }, { 0x16, 2, &g.far_objects, {} } };
    static int saved_level = -1;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != saved_level) { for (Field& f : fields) f.saved.clear(); saved_level = level; }
    bool work = false;
    for (const Field& f : fields) work |= *f.option || !f.saved.empty();
    if (!work) return;
    const uint32_t scratch = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u;
    wr32(scratch, 0); wr32(scratch + 4, 0);
    const uint32_t list = call_game(objGetObjList, ctx, { scratch, scratch + 4 });
    const int first = (int32_t)rd32(scratch), last = (int32_t)rd32(scratch + 4);
    if (!valid_ptr(list) || first < 0 || last > 2000) return;
    static const bool log = std::getenv("JFG_DEBUG_LOD") != nullptr;
    for (int i = first; i < last; i++) {
        const uint32_t obj = rd32(list + 4 * i);
        if (!valid_ptr(obj)) continue;
        const uint32_t def = rd32(obj + 0x40);
        if (!valid_ptr(def)) continue;
        for (Field& f : fields) {
            const uint32_t value = f.size == 4 ? rd32(def + f.offset) : rd16(def + f.offset);
            auto put = [&](uint32_t v) { if (f.size == 4) wr32(def + f.offset, v); else wr16(def + f.offset, (uint16_t)v); };
            if (*f.option) {
                if (value != 0 && f.saved.find(def) == f.saved.end()) {
                    f.saved[def] = value; put(0);
                    if (log) std::fprintf(stderr, "[jfg] object %d (definition %08X): field +0x%X was %08X, now 0\n", (int)(int16_t)rd16(obj + 0x48), def, f.offset, value);
                }
            }
            else {
                auto it = f.saved.find(def);
                if (it != f.saved.end() && value == 0) put(it->second);
            }
        }
    }
    for (Field& f : fields) if (!*f.option) f.saved.clear();
}

// --- field of view ---------------------------------------------------------------------------------------------------
// "Field of view %" option. Every FOV change goes through camSetFOV(fov, force) (src/camera.c): it stores the game's
// value in camera->fov, divides it for a zoomed scope, and rebuilds the projection when the result differs from the
// FOV the projection was last built with (D_800FB078). jfg_fov (hook where its zoomed and unzoomed paths meet, $f12 =
// the FOV about to be used) scales that value, so camera->fov keeps the game's own and the game never sees ours.
// camSetProjMtx only calls camSetFOV when D_800FB078 differs from camera->fov; while the scale is not 1 they always
// differ (so leaving gameplay restores the game's FOV at once), but going back to a scale needs a nudge: fov_refresh.
constexpr uint32_t ADDR_camFovApplied = 0x800FB078; // f32: the FOV the projection was last built with
constexpr float CAMERA_MAX_FOV = 90.0f;              // camSetFOV ignores a FOV of 90 or more
float g_fov_applied_scale = 1.0f;                    // the scale wanted when camSetFOV last ran

float fov_scale_wanted() {
    if (g.fov == 100 || g_rdram == nullptr) return 1.0f;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || in_cutscene()) return 1.0f;
    return (float)g.fov / 100.0f;
}

// Once per drawn tick, before the scene is drawn (jfg_camera_override).
void fov_refresh() {
    if (fov_scale_wanted() != g_fov_applied_scale) wrf(ADDR_camFovApplied, 0.0f); // camSetProjMtx calls camSetFOV again
}

extern "C" float jfg_fov(uint8_t* rdram, float fov, uint32_t zoomed) {
    g_rdram = rdram;
    const float scale = fov_scale_wanted();
    g_fov_applied_scale = scale;
    // A zoomed scope keeps its magnification: the zoom is relative to the game's own view.
    if (!(zoomed != 0 || scale == 1.0f || !(fov > 0.0f && fov < CAMERA_MAX_FOV))) fov = std::min(fov * scale, CAMERA_MAX_FOV - 0.5f);
    // The ambient occlusion pass rebuilds positions from depth with the projection's field of view (SsaoPS.hlsl).
    if (fov > 0.0f && fov < CAMERA_MAX_FOV) rt64_jfg_fov_tan.store(std::tan(fov * (PI_F / 360.0f)), std::memory_order_relaxed);
    return fov;
}

// --- the game's own fog -----------------------------------------------------------------------------------------------
// Each frame the level's fog goes to the RDP from func_8001BF9C (src/track.c): trackGetFog(player, &near, &far, ...) and
// then gSPFogPosition(near, far). jfg_game_fog (hook right after trackGetFog returns, $sp = its frame: near at +0x3E,
// far at +0x3C, both s16) moves or removes it, and tells the renderer whether the level has fog at all: the depth-based
// fog (experimental) stays off in levels that have their own. Fog positions are 0..1000 over the depth range, as
// gSPFogPosition takes them; trackSetFogOff uses 1018 / 1023, beyond the far plane, for "no fog".
constexpr float FOG_NEAR_PLANE = 10.0f, FOG_FAR_PLANE = 15000.0f; // the game's projection (src/camera.h)
constexpr int FOG_POSITION_FAR = 1000, FOG_OFF_NEAR = 1018, FOG_OFF_FAR = 1023;

float fog_position_to_distance(int v) {
    const float ndc = (float)std::clamp(v, 0, FOG_POSITION_FAR) / 500.0f - 1.0f;
    return (2.0f * FOG_NEAR_PLANE * FOG_FAR_PLANE) / ((FOG_FAR_PLANE + FOG_NEAR_PLANE) - ndc * (FOG_FAR_PLANE - FOG_NEAR_PLANE));
}

int fog_distance_to_position(float d) {
    d = std::clamp(d, FOG_NEAR_PLANE, FOG_FAR_PLANE);
    const float ndc = ((FOG_FAR_PLANE + FOG_NEAR_PLANE) - (2.0f * FOG_NEAR_PLANE * FOG_FAR_PLANE) / d) / (FOG_FAR_PLANE - FOG_NEAR_PLANE);
    return (int)std::lround((ndc + 1.0f) * 500.0f);
}

std::atomic<int> g_level_fog_near{ -1 }, g_level_fog_far{ -1 }; // the level's fog positions last sent (F9 status line)

extern "C" void jfg_game_fog(uint8_t* rdram, uint32_t sp) {
    g_rdram = rdram;
    const int near_pos = (int16_t)rd16(sp + 0x3E), far_pos = (int16_t)rd16(sp + 0x3C);
    g_level_fog_near.store(near_pos, std::memory_order_relaxed);
    g_level_fog_far.store(far_pos, std::memory_order_relaxed);
    const bool level_fog = near_pos < FOG_POSITION_FAR;
    rt64_jfg_fog_blocked.store(level_fog ? 1 : 0, std::memory_order_relaxed);
    if (!level_fog) return;
    if (g.no_level_fog) {
        wr16(sp + 0x3E, (uint16_t)FOG_OFF_NEAR);
        wr16(sp + 0x3C, (uint16_t)FOG_OFF_FAR);
        return;
    }
    if (g.level_fog_distance == 100) return;
    // Scaled as distances from the camera, so 200% puts the fog twice as far away.
    const float scale = (float)g.level_fog_distance / 100.0f;
    int new_far = far_pos >= FOG_POSITION_FAR ? far_pos : std::min(fog_distance_to_position(fog_position_to_distance(far_pos) * scale), FOG_OFF_FAR);
    int new_near = std::min(fog_distance_to_position(fog_position_to_distance(near_pos) * scale), new_far - 5);
    wr16(sp + 0x3E, (uint16_t)(int16_t)new_near);
    wr16(sp + 0x3C, (uint16_t)(int16_t)new_far);
}

// The depth fog's status line in the F9 menu: the level's own fog, and what the renderer did over the last half second.
std::string depth_fog_status() {
    static uint32_t last_drawn = 0, last_game = 0;
    static std::chrono::steady_clock::time_point last_check;
    static bool drawn = false, game = false;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_check > std::chrono::milliseconds(500)) {
        const uint32_t d = rt64_jfg_fog_drawn.load(std::memory_order_relaxed), gs = rt64_jfg_fog_game_scenes.load(std::memory_order_relaxed);
        drawn = d != last_drawn;
        game = gs != last_game;
        last_drawn = d;
        last_game = gs;
        last_check = now;
    }
    std::string out;
    char buf[160];
    const int n = g_level_fog_near.load(std::memory_order_relaxed), f = g_level_fog_far.load(std::memory_order_relaxed);
    if (n < 0 && f < 0) out = "Level fog: not seen yet.";
    else if (n >= FOG_POSITION_FAR) { std::snprintf(buf, sizeof(buf), "Level fog: off (%d-%d).", n, f); out = buf; }
    else {
        std::snprintf(buf, sizeof(buf), "Level fog: %d-%d (about %.0f to %.0f units).", n, f, fog_position_to_distance(n),
                      f >= FOG_POSITION_FAR ? FOG_FAR_PLANE : fog_position_to_distance(f));
        out = buf;
    }
    out += game ? " The scenery uses it, so depth fog stays off." : drawn ? " Depth fog is being drawn." : " Depth fog is not being drawn.";
    return out;
}

// Read by the renderer (src/renderer/renderer.cpp): the texture and output filtering options.
extern "C" int jfg_texture_three_point(void) { return g.texture_filter == 0 ? 1 : 0; }
extern "C" int jfg_output_filtering(void) { return g.output_filter; }

} // namespace jfg
