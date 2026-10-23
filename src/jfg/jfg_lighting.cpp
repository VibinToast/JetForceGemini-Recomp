// Jet Force Gemini port: modern lighting (Recomp-Docs RENDERER_ROADMAP.md, 3a). Work in progress for 1.4.
//
// The game lights its levels on the CPU: trackUpdateLighting keeps two copies of the level's vertices (10-byte
// Vertex: s16 x, y, z; u8 r, g, b, a), resets the one it is about to draw (func_8001400C_14C0C) and adds every
// active track light to its vertex colours (trackLightAsm, once per light). Track lights are spheres with a linear
// falloff by distance and no facing; every object light (muzzle flashes, flares, glowing pickups) mirrors itself as
// one. The plan is to skip trackLightAsm and light the level per pixel in RT64 from the same list of lights.
//
// Step 1, the experiment: JFG_NO_TRACK_LIGHTS=1 makes trackLightAsm do nothing, so the levels should show their plain
// baked colours (if func_8001400C_14C0C resets them as expected) with no dynamic light. JFG_DEBUG_LIGHTS=1 logs each
// light trackLightAsm is given (position, radius, inner radius) to jfg_lighting.log, the first time it sees it.
//
// Step 2: every frame the game submits (rcpFast3d, main task), the active track lights go to RT64 with the address of
// that frame's display list (rt64_jfg_frame_lights), which puts them in the frame's workload as point lights.
// RT64_JFG_DEBUG_LIGHTS=1 logs what arrives there.
#include <cstdio>
#include <cstdlib>
#include <unordered_set>

#include "jfg_game.h"
#include "jfg_hooks.h"

void rt64_jfg_frame_lights(uint32_t dlStart, const float* data, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp

namespace jfg {

void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp

// level light (src/track.c in the decomp, 0x80 bytes): f32 x, y, z, radius (0 = free slot), inner radius, ...
constexpr uint32_t TRACK_LIGHT_X = 0x00, TRACK_LIGHT_RADIUS = 0x0C, TRACK_LIGHT_INNER = 0x10;
// ... then a table of 32 RGB steps (s8, read unsigned), step i = colour x (i + 1) / 32 (trackLightColour): the last
// step is the light's full colour, intensity included.
constexpr uint32_t TRACK_LIGHT_FULL_COLOUR = 0x20 + 31 * 3, TRACK_LIGHT_SIZE = 0x80;
constexpr uint32_t ADDR_trackLights = 0x800A0D78;     // level light* (D_800A0D78_A1978)
constexpr uint32_t ADDR_trackLightSlots = 0x800A0D70; // s32, slots in that array (D_800A0D70_A1970)
constexpr uint32_t MAX_FRAME_LIGHTS = 64;
constexpr uint32_t RDRAM_MASK = 0x007FFFFF;           // as RT64 gets the task's display list (src/renderer/renderer.cpp)

static void log_light(uint32_t light) {
    static std::unordered_set<uint32_t> seen;
    if (seen.size() >= 256 || !seen.insert(light).second) return;
    char line[160];
    std::snprintf(line, sizeof(line), "level %d light %08X at %.0f %.0f %.0f radius %.0f inner %.0f",
                  (int)(int16_t)rd16(ADDR_currentLevel), light, rdf(light + TRACK_LIGHT_X),
                  rdf(light + TRACK_LIGHT_X + 4),
                  rdf(light + TRACK_LIGHT_X + 8), rdf(light + TRACK_LIGHT_RADIUS), rdf(light + TRACK_LIGHT_INNER));
    jfg_log_line("jfg_lighting.log", line);
}

// Entry of rcpFast3d(dlStart, dlEnd, taskType, framebuffer): the frame's display list is about to be sent.
extern "C" void jfg_frame_lights(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if ((uint32_t)ctx->r6 != 0) return; // side tasks (clone, refraction, blur) are not frames
    float data[MAX_FRAME_LIGHTS * 8];
    uint32_t count = 0;
    const uint32_t lights = rd32(ADDR_trackLights);
    const int32_t slots = (int32_t)rd32(ADDR_trackLightSlots);
    if (valid_ptr(lights)) {
        for (int32_t i = 0; i < slots && count < MAX_FRAME_LIGHTS; i++) {
            const uint32_t l = lights + (uint32_t)i * TRACK_LIGHT_SIZE;
            const float radius = rdf(l + TRACK_LIGHT_RADIUS);
            if (radius <= 0.0f) continue;
            float* d = &data[count++ * 8];
            d[0] = rdf(l + TRACK_LIGHT_X);
            d[1] = rdf(l + TRACK_LIGHT_X + 4);
            d[2] = rdf(l + TRACK_LIGHT_X + 8);
            d[3] = radius;
            d[4] = rdf(l + TRACK_LIGHT_INNER);
            for (uint32_t c = 0; c < 3; c++) d[5 + c] = rd8(l + TRACK_LIGHT_FULL_COLOUR + c) / 255.0f;
        }
    }
    rt64_jfg_frame_lights((uint32_t)ctx->r4 & RDRAM_MASK, data, count);
}

// Entry of trackLightAsm(track, light, visible). Returns non-zero to skip it.
extern "C" int jfg_track_light(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    static const bool log = std::getenv("JFG_DEBUG_LIGHTS") != nullptr;
    static const bool skip = std::getenv("JFG_NO_TRACK_LIGHTS") != nullptr;
    if (log) log_light((uint32_t)ctx->r5);
    return skip ? 1 : 0;
}

} // namespace jfg
