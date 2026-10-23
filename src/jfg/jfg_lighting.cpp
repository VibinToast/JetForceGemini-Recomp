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
//
// Step 4: per-pixel level lighting (JFG_PIXEL_LIGHTS=1). With the lights, the port sends where the level's own vertex
// arrays are (rt64_jfg_level_vertices: every track segment's vertices, and both copies while the game double-buffers
// them for its lighting); RT64 marks the draws that load vertices from there and adds the lights to them per pixel in
// RasterPS, and trackLightAsm is skipped so the light isn't added twice. Characters and objects keep the game's own
// lighting.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <cstdlib>
#include <cstring>
#include <unordered_set>
#include <vector>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

void rt64_jfg_frame_lights(uint32_t dlStart, const float* data, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp
void rt64_jfg_level_vertices(const uint32_t* ranges, uint32_t count);            // lib/rt64 src/hle/rt64_rsp.cpp
void rt64_jfg_frame_casters(uint32_t dlStart, const float* data, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp
void rt64_jfg_frame_objects(uint32_t dlStart, const uint32_t* ranges, uint32_t rangeCount, const uint32_t* normals,
                            uint32_t normalCount, const float* lights, uint32_t lightCount); // ...
extern std::atomic<int> rt64_jfg_pixel_lights; // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
extern std::atomic<int> rt64_jfg_debug_world;  // ...
extern std::atomic<int> rt64_jfg_debug_normal; // ...
extern std::atomic<float> rt64_jfg_facing;     // ...
extern std::atomic<float> rt64_jfg_shadows;    // ...
extern std::atomic<float> rt64_jfg_level_ambient; // ...

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
constexpr uint32_t ADDR_track = 0x800A0D60;           // Track* (src/track.c)
constexpr uint32_t TRACK_SEGMENTS = 0x04, TRACK_SEGMENT_COUNT = 0x1A; // track segment*, s16
constexpr uint32_t SEGMENT_SIZE = 0x48, SEGMENT_VERTICES = 0x00, SEGMENT_VERTEX_COUNT = 0x24; // Vertex*, s16
constexpr uint32_t VERTEX_SIZE = 10;
constexpr uint32_t ADDR_trackLightBlocks = 0x800A0D80; // light block* (D_800A0D80_A1980): both vertex copies
constexpr uint32_t MAX_SEGMENTS = 512;
// Object lights (the game's lighting code): an array of pointers with a count. object light: u8 type (+0x03; 0x40 =
// lights objects only, no track light), Vec3f pos (+0x18), f32 radius (+0x24), u8 r, g, b, intensity (+0x40..0x43),
// its track light (+0x6C, NULL for type 0x40).
constexpr uint32_t ADDR_activeLights = 0x800A1008, ADDR_activeLightCount = 0x800A1004;
constexpr uint32_t OBJ_LIGHT_TYPE = 0x03, OBJ_LIGHT_POS = 0x18, OBJ_LIGHT_RADIUS = 0x24, OBJ_LIGHT_COLOUR = 0x40;
constexpr uint32_t OBJ_LIGHT_TRACK = 0x6C;
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

// The settings start from the test switches: JFG_PIXEL_LIGHTS (mode: 1 = the game's level lights per pixel; 2 = also
// the lights the game only uses on characters and objects, type 0x40: torches, lamps, which then light the level too,
// beyond the original game), JFG_PIXEL_LIGHTS_STRENGTH (0-1, the torches and lamps: the level already has their glow
// baked into its colours, so at full strength they light it twice), JFG_PIXEL_LIGHTS_FACING (0-100),
// JFG_NO_TRACK_LIGHTS, RT64_JFG_DEBUG_WORLD, RT64_JFG_DEBUG_NORMAL, JFG_DEBUG_LIGHTS.
LightingSettings& lighting_settings() {
    static LightingSettings settings;
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* env = std::getenv("JFG_PIXEL_LIGHTS")) settings.mode = std::clamp(std::atoi(env), 0, 2);
        if (const char* env = std::getenv("JFG_PIXEL_LIGHTS_FACING")) settings.facing = std::clamp(std::atoi(env), 0, 100);
        if (const char* env = std::getenv("JFG_PIXEL_SHADOWS")) settings.shadows = std::clamp(std::atoi(env), 0, 100);
        if (const char* env = std::getenv("JFG_LEVEL_AMBIENT")) settings.level_ambient = std::clamp(std::atoi(env), 0, 199);
        if (const char* env = std::getenv("JFG_PIXEL_LIGHTS_STRENGTH")) {
            settings.object_strength = std::clamp((int)std::lround(std::atof(env) * 100.0), 0, 200);
        }
        settings.no_game_lighting = std::getenv("JFG_NO_TRACK_LIGHTS") != nullptr;
        settings.no_object_lighting = std::getenv("JFG_NO_OBJECT_LIGHTING") != nullptr;
        settings.characters = std::getenv("JFG_CHARACTER_LIGHTING") != nullptr;
        if (const char* env = std::getenv("JFG_CHARACTER_LIGHTS")) settings.character_lights = std::clamp(std::atoi(env), 0, 100);
        settings.debug_world = std::getenv("RT64_JFG_DEBUG_WORLD") != nullptr;
        settings.debug_normal = std::getenv("RT64_JFG_DEBUG_NORMAL") != nullptr;
        settings.log = std::getenv("JFG_DEBUG_LIGHTS") != nullptr;
    });
    return settings;
}

// Shadow casters: the objects the game draws a shadow for this frame, as spheres resting on the ground at their place.
// A shadow image's scale is its size in texels (s10.5) over its size in the world, so the radius is CASTER_UNITS over
// it (calibrated on the player, JFG_DEBUG_LIGHTS logs the scales), times the caster size setting.
struct Caster {
    uint32_t object;
    float x, y, z, radius;
};
std::vector<Caster> g_casters; // game thread: added by func_8001FA24, sent and cleared at rcpFast3d
constexpr float CASTER_UNITS = 384.0f; // the player: a radius of about 20
constexpr uint32_t OBJ_Y = 0x10; // ObjectTransform: f32 x, y, z at +0x0C
constexpr size_t MAX_CASTERS = 64;

void lighting_add_caster(uint32_t object, float x, float z, float scale) {
    if (!valid_ptr(object) || scale <= 0.0f || g_casters.size() >= MAX_CASTERS) return;
    const float radius = CASTER_UNITS / scale * (lighting_settings().caster_size.load() / 100.0f);
    const float y = rdf(object + OBJ_Y);
    g_casters.push_back({ object, x, y + radius, z, radius });
}

// --- characters and objects ---------------------------------------------------------------------------------------
// The game lights a model per vertex (lightSingleLight, lightTwoLights, lightMultiLight, called by lightObject) from
// two of its tables (read from the recompiled lightSingleLight):
//   model + 0x6C: groups of [u8 count, u8 joint], then count records; a count of 0 ends it. A record is a big-endian
//                 word: the offset of a table entry (top 16 bits), n more vertices sharing it (4 bits), the vertex
//                 index (12 bits); then the n extra indices, 12 bits each, packed from the top; 4 + (12n + 7) / 8
//                 bytes in all.
//   model + 0x68: the table: per entry a normal (s8 x, y, z, in the joint's space) and the base colour (u8 r, g, b).
// Brightness = min + (max - min) x how much the normal faces the light (one direction per joint), colour = base x
// brightness, written into the instance's vertices (modInst + 0x04, 10 bytes each, colour at +6).
// With the new character lighting, the port writes the base colour itself, sends each vertex's normal (with the
// object's slot) and the object's settings to RT64, and the game's three functions are skipped.
constexpr uint32_t MODINST_MODEL = 0x00, MODINST_VERTICES = 0x04;
constexpr uint32_t MODEL_NORMAL_TABLE = 0x68, MODEL_NORMAL_LIST = 0x6C;
// Object lighting (the game's lighting code): f32 fade (+0x00), s16 directions used (+0x0E), light setting[4] (+0x10, 0x20
// each): Vec3f direction (+0x00), u8 max (+0x15), u8 min (+0x17).
constexpr uint32_t LIGHTING_FADE = 0x00, LIGHTING_COUNT = 0x0E, LIGHTING_SETTINGS = 0x10, SETTING_SIZE = 0x20;
constexpr uint32_t SETTING_DIR = 0x00, SETTING_MAX = 0x15, SETTING_MIN = 0x17;
constexpr size_t MAX_OBJECTS = 254, MAX_OBJECT_VERTICES = 4096;

std::vector<uint32_t> g_object_ranges;  // start, vertex count, offset into g_object_normals
std::vector<uint32_t> g_object_normals; // per vertex: s8 x, y, z, slot + 1
std::vector<float> g_object_lights;     // 20 floats per slot (RT64's JfgObjectLight)

static uint32_t be32(uint32_t a) {
    return ((uint32_t)rd8(a) << 24) | ((uint32_t)rd8(a + 1) << 16) | ((uint32_t)rd8(a + 2) << 8) | rd8(a + 3);
}

// Entry of lightObject(trans, modInst, lighting, mtx).
extern "C" void jfg_light_object(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    LightingSettings& s = lighting_settings();
    if (!s.characters.load()) return;
    const uint32_t inst = (uint32_t)ctx->r5, lighting = (uint32_t)ctx->r6;
    if (!valid_ptr(inst) || !valid_ptr(lighting) || g_object_lights.size() / 20 >= MAX_OBJECTS) return;
    const uint32_t model = rd32(inst + MODINST_MODEL), verts = rd32(inst + MODINST_VERTICES);
    if (!valid_ptr(model) || !valid_ptr(verts)) return;
    const uint32_t list = rd32(model + MODEL_NORMAL_LIST), table = rd32(model + MODEL_NORMAL_TABLE);
    if (!valid_ptr(list) || !valid_ptr(table)) return;

    const uint32_t slot = (uint32_t)(g_object_lights.size() / 20) + 1;
    const size_t first = g_object_normals.size();
    uint32_t vertex_count = 0;
    auto set = [&](uint32_t idx, uint32_t entry) {
        if (idx >= MAX_OBJECT_VERTICES) return;
        if (idx >= vertex_count) {
            vertex_count = idx + 1;
            g_object_normals.resize(first + vertex_count, 0);
        }
        g_object_normals[first + idx] = rd8(entry) | ((uint32_t)rd8(entry + 1) << 8) | ((uint32_t)rd8(entry + 2) << 16) | (slot << 24);
        const uint32_t v = verts + idx * 10;
        wr8(v + 6, rd8(entry + 3));
        wr8(v + 7, rd8(entry + 4));
        wr8(v + 8, rd8(entry + 5));
        wr8(v + 9, 0xFF);
    };
    uint32_t p = list;
    for (int guard = 0; guard < 4096; guard++) {
        const uint32_t count = rd8(p);
        if (count == 0) break;
        p += 2; // the joint byte: the game turns its light direction per joint, RT64 turns the normals instead
        for (uint32_t k = 0; k < count; k++) {
            const uint32_t w = be32(p);
            const uint32_t entry = table + (w >> 16), extra = (w >> 12) & 0xF;
            set(w & 0xFFF, entry);
            for (uint32_t j = 0; j < extra; j++) {
                const uint32_t bit = 12 * j; // from the top of the bytes after the word
                const uint32_t b = p + 4 + bit / 8;
                const uint32_t two = ((uint32_t)rd8(b) << 8) | rd8(b + 1);
                set((bit % 8 == 0) ? (two >> 4) : (two & 0xFFF), entry);
            }
            p += extra ? (12 * extra + 39) / 8 : 4;
        }
    }
    if (vertex_count == 0) {
        g_object_normals.resize(first);
        return;
    }
    g_object_ranges.push_back(verts & RDRAM_MASK);
    g_object_ranges.push_back(vertex_count);
    g_object_ranges.push_back((uint32_t)first);

    // Its settings, with the fade lightObject applies to the first one.
    float light[20] = {};
    const int32_t used = std::clamp((int)(int16_t)rd16(lighting + LIGHTING_COUNT), 0, 4);
    const float fade = rdf(lighting + LIGHTING_FADE);
    for (int32_t k = 0; k < std::max(used, 1); k++) {
        const uint32_t setting = lighting + LIGHTING_SETTINGS + (uint32_t)k * SETTING_SIZE;
        float lo = rd8(setting + SETTING_MIN), hi = rd8(setting + SETTING_MAX);
        if (k == 0 && fade != 1.0f) {
            lo = std::floor(lo * fade);
            hi = std::floor(hi * fade);
        }
        if (k == 0) light[16] = lo / 255.0f;
        if (k < used) {
            light[k * 4] = rdf(setting + SETTING_DIR);
            light[k * 4 + 1] = rdf(setting + SETTING_DIR + 4);
            light[k * 4 + 2] = rdf(setting + SETTING_DIR + 8);
            light[k * 4 + 3] = std::max(hi - lo, 0.0f) / 255.0f;
        }
    }
    light[17] = (float)used;
    light[18] = s.character_lights.load() / 100.0f;
    g_object_lights.insert(g_object_lights.end(), light, light + 20);
}

// The level's vertex arrays as physical [start, end) pairs, as RT64 sees vertex loads.
static void send_level_vertices() {
    static uint32_t ranges[MAX_SEGMENTS * 3 * 2];
    uint32_t count = 0;
    const uint32_t track = rd32(ADDR_track);
    if (valid_ptr(track)) {
        const uint32_t segments = rd32(track + TRACK_SEGMENTS);
        const int32_t segment_count = (int16_t)rd16(track + TRACK_SEGMENT_COUNT);
        const uint32_t blocks = rd32(ADDR_trackLightBlocks);
        for (int32_t i = 0; valid_ptr(segments) && i < segment_count && i < (int32_t)MAX_SEGMENTS; i++) {
            const uint32_t seg = segments + (uint32_t)i * SEGMENT_SIZE;
            const uint32_t bytes = (uint32_t)(int16_t)rd16(seg + SEGMENT_VERTEX_COUNT) * VERTEX_SIZE;
            uint32_t arrays[3] = { rd32(seg + SEGMENT_VERTICES), 0, 0 };
            if (valid_ptr(blocks)) {
                arrays[1] = rd32(blocks + (uint32_t)i * 8);
                arrays[2] = rd32(blocks + (uint32_t)i * 8 + 4);
            }
            for (uint32_t a : arrays) {
                if (!valid_ptr(a) || bytes == 0) continue;
                ranges[count * 2] = (a & RDRAM_MASK) & ~7u; // RT64 masks the low 3 bits of a load's address
                ranges[count * 2 + 1] = (a & RDRAM_MASK) + bytes;
                count++;
            }
        }
    }
    rt64_jfg_level_vertices(ranges, count);
}

// JFG_DEBUG_LIGHTS: every 120 frames, the object lights and whether each lights the level too.
static void log_object_lights() {
    static uint32_t frames = 0, logged = 0;
    if ((frames++ % 120) != 0 || logged >= 60) return;
    logged++;
    const uint32_t lights = rd32(ADDR_activeLights);
    const int32_t count = (int32_t)rd32(ADDR_activeLightCount);
    char line[160];
    std::snprintf(line, sizeof(line), "level %d: %d object lights", (int)(int16_t)rd16(ADDR_currentLevel), count);
    jfg_log_line("jfg_lighting.log", line);
    for (int32_t i = 0; valid_ptr(lights) && i < count && i < 32; i++) {
        const uint32_t l = rd32(lights + (uint32_t)i * 4);
        if (!valid_ptr(l)) continue;
        std::snprintf(line, sizeof(line), "  type %02X at %.0f %.0f %.0f radius %.0f colour %u %u %u x %u track %08X",
                      rd8(l + OBJ_LIGHT_TYPE), rdf(l + OBJ_LIGHT_POS), rdf(l + OBJ_LIGHT_POS + 4), rdf(l + OBJ_LIGHT_POS + 8),
                      rdf(l + OBJ_LIGHT_RADIUS), rd8(l + OBJ_LIGHT_COLOUR), rd8(l + OBJ_LIGHT_COLOUR + 1),
                      rd8(l + OBJ_LIGHT_COLOUR + 2), rd8(l + OBJ_LIGHT_COLOUR + 3), rd32(l + OBJ_LIGHT_TRACK));
        jfg_log_line("jfg_lighting.log", line);
    }
}

// Entry of rcpFast3d(dlStart, dlEnd, taskType, framebuffer): the frame's display list is about to be sent.
extern "C" void jfg_frame_lights(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if ((uint32_t)ctx->r6 != 0) return; // side tasks (clone, refraction, blur) are not frames
    LightingSettings& s = lighting_settings();
    const int mode = s.mode.load();
    rt64_jfg_pixel_lights.store(mode > 0 ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_debug_world.store(s.debug_world.load() ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_debug_normal.store(s.debug_normal.load() ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_facing.store(std::clamp(s.facing.load(), 0, 100) / 100.0f, std::memory_order_relaxed);
    rt64_jfg_shadows.store(std::clamp(s.shadows.load(), 0, 100) / 100.0f, std::memory_order_relaxed);
    rt64_jfg_level_ambient.store(std::clamp(s.level_ambient.load(), 0, 199) / 100.0f, std::memory_order_relaxed);
    if (s.log.load()) log_object_lights();
    send_level_vertices();
    // Every candidate light (id = its slot's address, so RT64 can match it with the last frame's for interpolation),
    // then the nearest to the camera up to the limit.
    struct Light {
        float d[9];
        float dist2;
        bool level;
    };
    static std::vector<Light> found;
    found.clear();
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float cx = rdf(cam + 0x0C), cy = rdf(cam + 0x10), cz = rdf(cam + 0x14);
    auto add = [&](uint32_t id, float x, float y, float z, float radius, float inner, const float* colour, bool level) {
        Light light;
        light.d[0] = x;
        light.d[1] = y;
        light.d[2] = z;
        light.d[3] = radius;
        light.d[4] = inner;
        for (uint32_t c = 0; c < 3; c++) light.d[5 + c] = colour[c];
        std::memcpy(&light.d[8], &id, 4);
        light.dist2 = (x - cx) * (x - cx) + (y - cy) * (y - cy) + (z - cz) * (z - cz);
        light.level = level;
        found.push_back(light);
    };

    const float level_strength = s.level_strength.load() / 100.0f;
    const uint32_t lights = rd32(ADDR_trackLights);
    const int32_t slots = (int32_t)rd32(ADDR_trackLightSlots);
    for (int32_t i = 0; valid_ptr(lights) && i < slots; i++) {
        const uint32_t l = lights + (uint32_t)i * TRACK_LIGHT_SIZE;
        const float radius = rdf(l + TRACK_LIGHT_RADIUS);
        if (radius <= 0.0f) continue;
        float colour[3];
        for (uint32_t c = 0; c < 3; c++) colour[c] = rd8(l + TRACK_LIGHT_FULL_COLOUR + c) / 255.0f * level_strength;
        add(l, rdf(l + TRACK_LIGHT_X), rdf(l + TRACK_LIGHT_X + 4), rdf(l + TRACK_LIGHT_X + 8), radius,
            rdf(l + TRACK_LIGHT_INNER), colour, true);
    }

    // Mode 2: the object-only lights as well, sized as the game sizes a track light from an object light
    // (func_80020D84_21984: radius x 1.25, inner radius x 0.3, colour x intensity / 256); the radius scale and the
    // strength are settings.
    const float object_strength = s.object_strength.load() / 100.0f;
    const float object_radius = s.object_radius.load() / 100.0f;
    const uint32_t objects = rd32(ADDR_activeLights);
    const int32_t object_count = (int32_t)rd32(ADDR_activeLightCount);
    for (int32_t i = 0; mode >= 2 && valid_ptr(objects) && i < object_count; i++) {
        const uint32_t l = rd32(objects + (uint32_t)i * 4);
        if (!valid_ptr(l) || !(rd8(l + OBJ_LIGHT_TYPE) & 0x40)) continue;
        const uint32_t intensity = rd8(l + OBJ_LIGHT_COLOUR + 3);
        const float radius = rdf(l + OBJ_LIGHT_RADIUS);
        if (intensity == 0 || radius <= 0.0f) continue;
        float colour[3];
        for (uint32_t c = 0; c < 3; c++) {
            colour[c] = (float)((rd8(l + OBJ_LIGHT_COLOUR + c) * intensity) >> 8) / 255.0f * object_strength;
        }
        add(l, rdf(l + OBJ_LIGHT_POS), rdf(l + OBJ_LIGHT_POS + 4), rdf(l + OBJ_LIGHT_POS + 8), radius * object_radius,
            radius * 0.3f, colour, false);
    }

    const size_t max_lights = (size_t)std::clamp(s.max_lights.load(), 0, (int)MAX_FRAME_LIGHTS);
    if (found.size() > max_lights) {
        std::partial_sort(found.begin(), found.begin() + max_lights, found.end(),
                          [](const Light& a, const Light& b) { return a.dist2 < b.dist2; });
        found.resize(max_lights);
    }
    float data[MAX_FRAME_LIGHTS * 9];
    int level_lights = 0;
    for (size_t i = 0; i < found.size(); i++) {
        std::memcpy(&data[i * 9], found[i].d, sizeof(found[i].d));
        level_lights += found[i].level ? 1 : 0;
    }
    s.sent_level_lights = level_lights;
    s.sent_object_lights = (int)found.size() - level_lights;
    rt64_jfg_frame_lights((uint32_t)ctx->r4 & RDRAM_MASK, data, (uint32_t)found.size());

    float casters[MAX_CASTERS * 5];
    for (size_t i = 0; i < g_casters.size(); i++) {
        const Caster& c = g_casters[i];
        casters[i * 5] = c.x;
        casters[i * 5 + 1] = c.y;
        casters[i * 5 + 2] = c.z;
        casters[i * 5 + 3] = c.radius;
        std::memcpy(&casters[i * 5 + 4], &c.object, 4);
    }
    if (s.log.load()) {
        static uint32_t frames = 0, logged = 0;
        if ((frames++ % 120) == 0 && logged < 40 && !g_casters.empty()) {
            logged++;
            char line[200];
            const Caster& c = g_casters[0];
            std::snprintf(line, sizeof(line), "%zu casters; first: object %08X at %.0f %.0f %.0f radius %.1f", g_casters.size(),
                          c.object, c.x, c.y - c.radius, c.z, c.radius);
            jfg_log_line("jfg_lighting.log", line);
        }
    }
    rt64_jfg_frame_casters((uint32_t)ctx->r4 & RDRAM_MASK, casters, (uint32_t)g_casters.size());
    g_casters.clear();
    rt64_jfg_frame_objects((uint32_t)ctx->r4 & RDRAM_MASK, g_object_ranges.data(), (uint32_t)(g_object_ranges.size() / 3),
                           g_object_normals.data(), (uint32_t)g_object_normals.size(), g_object_lights.data(),
                           (uint32_t)(g_object_lights.size() / 20));
    g_object_ranges.clear();
    g_object_normals.clear();
    g_object_lights.clear();
}

// Entries of lightSingleLight, lightTwoLights and lightMultiLight (called by lightObject): they write a model's vertex
// colours from the lights near it. Returns non-zero to skip them; the model then keeps whatever its vertex colours
// held before (lightEnviroMap, the shiny models' texture coordinates, still runs).
extern "C" int jfg_object_light(void) {
    const LightingSettings& s = lighting_settings();
    return (s.no_object_lighting.load() || s.characters.load()) ? 1 : 0;
}

// Entry of trackLightAsm(track, light, visible). Returns non-zero to skip it.
extern "C" int jfg_track_light(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const LightingSettings& s = lighting_settings();
    if (s.log.load()) log_light((uint32_t)ctx->r5);
    return (s.no_game_lighting.load() || s.mode.load() > 0) ? 1 : 0;
}

} // namespace jfg
