// Jet Force Gemini port: visual options, for the game hooks and the renderer - widescreen, field of view, the levels'
// own fog (and the hidden depth fog), LOD and draw distance, texture and picture filtering, the red flash on a hit.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_api.h"
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
    // Leaving a planet (mainChangeLevel with 0x11) shows the region stats paused, then loads the level with the pause
    // mode cleared and the planet map as front-end mode 17 in game mode 0: that map gets the same treatment.
    const bool paused = g_rdram != nullptr &&
        (rd8(ADDR_pauseMode) != 0 || (rd32(ADDR_mainGameMode) == 0 && rd8(ADDR_frontEndMode) == FRONT_MODE_MAP));
    rt64_jfg_paused.store(paused ? 1 : 0, std::memory_order_relaxed);
    if (!(g.widescreen || env) || g_rdram == nullptr) return 0;
    return rd32(ADDR_mainGameMode) == 0 ? 1 : 0;
}

// "Shadow Opacity": the alpha func_8001B8E0 draws a shadow batch with (gDPSetPrimColor; the object's fade times the
// batch's vertex alpha, >> 8), scaled by the option. 0 skips the batch, as the game does when a shadow fades out.
extern "C" int jfg_shadow_alpha(int alpha) {
    if (g.shadow_opacity == 100 || alpha <= 0) return alpha;
    return std::min(255, (alpha * g.shadow_opacity + 50) / 100);
}

namespace {
constexpr uint32_t ADDR_shadowGfx = 0x800F2F70; // D_800F2F70: Gfx*, the list func_8001B8E0 draws the shadows into
}

// func_8001B8E0's wrapper: after the game's code, each batch's gDPSetPrimColor(0, 0, 0, 0, 0, alpha) in what it wrote
// gets jfg_shadow_alpha's alpha (the one after each batch, all 0xFF, puts the colour back and is left). At 0 the batch
// is drawn fully transparent.
JFG_WRAP(func_8001B8E0_1C4E0) {
    using namespace jfg;
    g_rdram = rdram;
    const uint32_t start = rd32(ADDR_shadowGfx);
    func_8001B8E0_1C4E0_orig(rdram, ctx);
    g_rdram = rdram;
    if (g.shadow_opacity == 100) return;
    const uint32_t end = rd32(ADDR_shadowGfx);
    if (!valid_ptr(start) || !valid_ptr(end) || end < start || end - start > 0x10000) return;
    for (uint32_t cmd = start; cmd < end; cmd += 8) {
        const uint32_t alpha = rd32(cmd + 4);
        if (rd32(cmd) != 0xFA000000u || alpha == 0 || alpha > 0xFF) continue;
        wr32(cmd + 4, (uint32_t)jfg_shadow_alpha((int)alpha));
    }
}

// "Hit flash opacity": entry of frontFlashScreen. The characters' control code (overlays 15-17) calls it with red
// (0xFF0000FF) for a few ticks after a hit; fxScreenEffect then draws the picture again multiplied by that colour
// (combiner TEXEL0 * PRIMITIVE), which leaves only its red. Raising green and blue towards white weakens the tint
// evenly. Returns the colour to use; 0 skips the flash.
extern "C" uint32_t jfg_hit_flash(uint32_t color) {
    constexpr uint32_t HIT_RED = 0xFF0000FFu;
    if (color != HIT_RED || g.hit_flash >= 100) return color;
    if (g.hit_flash <= 0) return 0;
    const uint32_t gb = (uint32_t)std::lround(255.0 * (100 - g.hit_flash) / 100.0);
    return 0xFF0000FFu | (gb << 16) | (gb << 8);
}

// "Hide Floyd". sidekickControl(obj, ...) updates Floyd; its entry notes his object kind (behaviorId, +0x48), and
// objPrintObject(dList, mtx, vtx, obj) draws nothing for an object of that kind while the option is on. His shadow is
// made in shadowGenerate, from the object's shadow data (+0x50): the loop skips an object without any. His sounds
// (among them a looping one, 0x138, whose volume the code works out each tick) are all started by sidekickControl
// itself, so amSndPlay does nothing while it runs for a hidden Floyd. The kind is kept rather than the object, so he stays hidden while
// paused and a reused object address hides nothing else. Not in a Floyd mission or a co-op game, where a player
// flies him.
static int g_floyd_kind = -1;
static uint32_t g_floyd_obj = 0; // his object as last seen by sidekickControl
static bool g_floyd_muted = false;
constexpr uint32_t ADDR_cooperativeGame = 0x800A4FC8; // u8

static bool floyd_hidden(uint32_t obj) {
    if (!g.hide_floyd || g_floyd_kind < 0 || !valid_ptr(obj) || Obj::Kind::get(obj) != g_floyd_kind) return false;
    return rd8(ADDR_robotMission) == 0 && rd8(ADDR_cooperativeGame) == 0;
}

extern "C" void jfg_floyd_seen(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    if (!valid_ptr(obj)) return;
    const int kind = Obj::Kind::get(obj);
    if (kind != g_floyd_kind && std::getenv("JFG_DEBUG_FLOYD")) std::fprintf(stderr, "[jfg] Floyd: behaviorId %d\n", kind);
    g_floyd_kind = kind;
    g_floyd_obj = obj;
    g_floyd_muted = floyd_hidden(obj);
}

// After sidekickControl (its wrapper, jfg_api.cpp).
extern "C" void jfg_floyd_done(void) { g_floyd_muted = false; }

// jfg_on_sound_play handler: nothing plays while sidekickControl runs for a hidden Floyd.
extern "C" void jfg_floyd_mute(uint8_t* rdram_, recomp_context* ctx) {
    if (!g_floyd_muted) return;
    g_rdram = rdram_;
    const uint32_t sound = (uint32_t)ctx->r4;
    static const bool log = std::getenv("JFG_DEBUG_FLOYD") != nullptr;
    if (log) std::fprintf(stderr, "[jfg] Floyd: sound 0x%X muted\n", rd32(sound + api::value_cancel::VALUE) & 0xFFFF);
    wr32(sound + api::value_cancel::CANCEL, 1);
}

extern "C" void jfg_hide_object(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t draw = (uint32_t)ctx->r4;
    if (floyd_hidden(rd32(draw + api::value_cancel::VALUE))) wr32(draw + api::value_cancel::CANCEL, 1);
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
    static Field fields[2] = { { ObjDef::LodDistance::offset, 4, &g.no_lod, {} }, { ObjDef::DrawRange::offset, 2, &g.far_objects, {} } };
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
        const uint32_t def = Obj::Def::get(obj);
        if (!valid_ptr(def)) continue;
        for (Field& f : fields) {
            const uint32_t value = f.size == 4 ? rd32(def + f.offset) : rd16(def + f.offset);
            auto put = [&](uint32_t v) { if (f.size == 4) wr32(def + f.offset, v); else wr16(def + f.offset, (uint16_t)v); };
            if (*f.option) {
                if (value != 0 && f.saved.find(def) == f.saved.end()) {
                    f.saved[def] = value; put(0);
                    if (log) std::fprintf(stderr, "[jfg] object %d (definition %08X): field +0x%X was %08X, now 0\n", (int)Obj::Kind::get(obj), def, f.offset, value);
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

// jfg_on_screen_flash handler: the hit flash's colour (jfg_hit_flash); 0 cancels it.
extern "C" void jfg_hit_flash_event(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t flash = (uint32_t)ctx->r4;
    const uint32_t color = jfg_hit_flash(rd32(flash + api::value_cancel::VALUE));
    if (color == 0) wr32(flash + api::value_cancel::CANCEL, 1);
    else wr32(flash + api::value_cancel::VALUE, color);
}

// Floyd's shadow: shadowGenerate makes shadows from each object's shadow data (+0x50) and skips an object without any.
// For a hidden Floyd it's blanked while shadowGenerate runs.
JFG_WRAP(shadowGenerate) {
    g_rdram = rdram;
    const uint32_t floyd = g_floyd_obj;
    const uint32_t shadow = floyd_hidden(floyd) ? Obj::Shadow::get(floyd) : 0;
    if (shadow != 0) {
        static bool logged = false;
        if (!logged && std::getenv("JFG_DEBUG_FLOYD")) { std::fprintf(stderr, "[jfg] Floyd: shadow skipped\n"); logged = true; }
        Obj::Shadow::set(floyd, 0);
    }
    shadowGenerate_orig(rdram, ctx);
    if (shadow != 0) {
        g_rdram = rdram;
        Obj::Shadow::set(floyd, shadow);
    }
}

// camSetFOV(fov, force): the camera keeps the game's FOV (camera +0x2C); a zoomed scope divides it by its zoom. Unzoomed,
// the FOV option scales what is applied (jfg_fov): the game's code runs with the scaled FOV and the camera's own value
// is put back. Zoomed, nothing changes (jfg_fov only notes the FOV for the ambient occlusion pass).
JFG_WRAP(camSetFOV) {
    g_rdram = rdram;
    const float fov = ctx->f12.fl;
    const uint32_t cam = rd32(ADDR_cameraNo) & 3;
    const uint32_t zoomed = rd8(0x800A3174u + cam);
    float applied = fov;
    if (zoomed != 0) applied *= 1.0f / ((0.25f * (rdf(0x800A317Cu + cam * 4) - 1.0f)) + 1.0f);
    const float wanted = jfg_fov(rdram, applied, zoomed);
    if (zoomed != 0 || wanted == applied) {
        camSetFOV_orig(rdram, ctx);
        return;
    }
    ctx->f12.fl = wanted;
    camSetFOV_orig(rdram, ctx);
    g_rdram = rdram;
    Camera::Fov::set(ADDR_cameraActorArray + cam * Camera::Size, fov);
}

} // namespace jfg
