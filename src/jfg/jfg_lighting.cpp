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
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

void rt64_jfg_frame_lights(uint32_t dlStart, const float* data, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp
void rt64_jfg_level_vertices(const uint32_t* ranges, uint32_t count);            // lib/rt64 src/hle/rt64_rsp.cpp
void rt64_jfg_level_normals(const uint32_t* ranges, uint32_t rangeCount, const uint32_t* normals, uint32_t normalCount); // ...
void rt64_jfg_frame_points(uint32_t dlStart, const float* data, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp
void rt64_jfg_lighting_params(const float* params, uint32_t count); // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
void rt64_jfg_frame_objects(uint32_t dlStart, const uint32_t* ranges, uint32_t rangeCount, const uint32_t* normals,
                            uint32_t normalCount, const float* lights, uint32_t lightCount); // ...
extern std::atomic<int> rt64_jfg_pixel_lights; // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
extern std::atomic<int> rt64_jfg_debug_world;  // ...
extern std::atomic<int> rt64_jfg_debug_normal; // ...
extern std::atomic<int> rt64_jfg_debug_shadow; // ...
extern std::atomic<float> rt64_jfg_facing;     // ...
extern std::atomic<float> rt64_jfg_shadows;    // ...
extern std::atomic<int> rt64_jfg_shadow_lights; // ...
extern std::atomic<int> rt64_jfg_shadow_size;  // ...
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
        if (const char* env = std::getenv("JFG_SHADOW_LIGHTS")) settings.shadow_lights = std::clamp(std::atoi(env), 0, 4);
        if (const char* env = std::getenv("JFG_SHADOW_SIZE")) settings.shadow_size = std::clamp(std::atoi(env), 128, 2048);
        if (const char* env = std::getenv("JFG_LEVEL_AMBIENT")) settings.level_ambient = std::clamp(std::atoi(env), 0, 199);
        if (const char* env = std::getenv("JFG_PIXEL_LIGHTS_STRENGTH")) {
            settings.object_strength = std::clamp((int)std::lround(std::atof(env) * 100.0), 0, 200);
        }
        // On unless the switch says 0.
        auto on_unless_zero = [](const char* name, std::atomic<bool>& value) {
            if (const char* env = std::getenv(name)) value = std::atoi(env) != 0 || env[0] == 0;
        };
        on_unless_zero("JFG_NO_TRACK_LIGHTS", settings.no_game_lighting);
        on_unless_zero("JFG_NO_OBJECT_LIGHTING", settings.no_object_lighting);
        on_unless_zero("JFG_NO_GAME_SHADOWS", settings.no_game_shadows);
        on_unless_zero("JFG_CHARACTER_LIGHTING", settings.characters);
        if (const char* env = std::getenv("JFG_CHARACTER_LIGHTS")) settings.character_lights = std::clamp(std::atoi(env), 0, 100);
        settings.debug_world = std::getenv("RT64_JFG_DEBUG_WORLD") != nullptr;
        settings.debug_normal = std::getenv("RT64_JFG_DEBUG_NORMAL") != nullptr;
        settings.debug_shadow = std::getenv("RT64_JFG_DEBUG_SHADOW") != nullptr;
        // JFG_SUN="yaw,pitch,intensity" turns the directional light on; JFG_DETAIL (tenths of a unit), JFG_SHINE (%).
        if (const char* env = std::getenv("JFG_SUN")) {
            int yaw = 45, pitch = 50, intensity = 35;
            std::sscanf(env, "%d,%d,%d", &yaw, &pitch, &intensity);
            settings.sun = true;
            settings.sun_yaw = yaw;
            settings.sun_pitch = pitch;
            settings.sun_intensity = intensity;
        }
        if (const char* env = std::getenv("JFG_DETAIL")) settings.detail = std::clamp(std::atoi(env), 0, 100);
        if (const char* env = std::getenv("JFG_SHINE")) settings.shine = std::clamp(std::atoi(env), 0, 200);
        settings.log = std::getenv("JFG_DEBUG_LIGHTS") != nullptr;
    });
    return settings;
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
constexpr uint32_t SETTING_DIR = 0x00, SETTING_MAX = 0x15, SETTING_MIN = 0x17, SETTING_TABLE = 0x18;
constexpr uint32_t OBJ_LIGHT_TABLE = 0x70; // object light: its colour table
constexpr size_t MAX_OBJECTS = 254, MAX_OBJECT_VERTICES = 4096;

std::vector<uint32_t> g_object_ranges;  // start, vertex count, offset into g_object_normals
std::vector<uint32_t> g_object_normals; // per vertex: s8 x, y, z, slot + 1
constexpr size_t OBJECT_LIGHT_FLOATS = 52; // RT64's JfgObjectLight: dirs, colors, positions, params
std::vector<float> g_object_lights;     // OBJECT_LIGHT_FLOATS per slot

static uint32_t be32(uint32_t a) {
    return ((uint32_t)rd8(a) << 24) | ((uint32_t)rd8(a + 1) << 16) | ((uint32_t)rd8(a + 2) << 8) | rd8(a + 3);
}

// Entry of lightObject(trans, modInst, lighting, mtx).
extern "C" void jfg_light_object(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    LightingSettings& s = lighting_settings();
    if (!s.characters.load() || !lighting_active()) return;
    const uint32_t inst = (uint32_t)ctx->r5, lighting = (uint32_t)ctx->r6;
    if (!valid_ptr(inst) || !valid_ptr(lighting) || g_object_lights.size() / OBJECT_LIGHT_FLOATS >= MAX_OBJECTS) return;
    const uint32_t model = rd32(inst + MODINST_MODEL), verts = rd32(inst + MODINST_VERTICES);
    if (!valid_ptr(model) || !valid_ptr(verts)) return;
    const uint32_t list = rd32(model + MODEL_NORMAL_LIST), table = rd32(model + MODEL_NORMAL_TABLE);
    if (!valid_ptr(list) || !valid_ptr(table)) return;

    const uint32_t slot = (uint32_t)(g_object_lights.size() / OBJECT_LIGHT_FLOATS) + 1;
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

    // Its settings, with the fade lightObject applies to the first one. Each points at its light's colour table
    // (+0x18, the object light's +0x70, made by lightCreateLightTable from its colour; the first one's is white):
    // the colour is that object light's (as the lighting tool changed it), and where it is, so the character is
    // shadowed from it when it has a shadow map.
    float light[OBJECT_LIGHT_FLOATS] = {};
    auto colour_of = [](uint32_t table, float* rgb, float* pos) {
        rgb[0] = rgb[1] = rgb[2] = 1.0f;
        const uint32_t objects = rd32(ADDR_activeLights);
        const int32_t count = (int32_t)rd32(ADDR_activeLightCount);
        for (int32_t i = 0; valid_ptr(table) && valid_ptr(objects) && i < count; i++) {
            const uint32_t l = rd32(objects + (uint32_t)i * 4);
            if (valid_ptr(l) && rd32(l + OBJ_LIGHT_TABLE) == table) {
                for (uint32_t c = 0; c < 3; c++) {
                    rgb[c] = rd8(l + OBJ_LIGHT_COLOUR + c) / 255.0f;
                    pos[c] = rdf(l + OBJ_LIGHT_POS + c * 4);
                }
                pos[3] = 1.0f;
                if (const LightOverride* o = lighting_override_for(l)) {
                    const float scale = (o->on && o->radius > 0) ? o->intensity / 100.0f * lighting_flicker(l, o->flicker, o->flicker_speed) : 0.0f;
                    if (o->recolour) {
                        rgb[0] = o->r / 255.0f;
                        rgb[1] = o->g / 255.0f;
                        rgb[2] = o->b / 255.0f;
                    }
                    for (uint32_t c = 0; c < 3; c++) rgb[c] *= scale;
                }
                return;
            }
        }
    };
    const int32_t used = std::clamp((int)(int16_t)rd16(lighting + LIGHTING_COUNT), 0, 4);
    const float fade = rdf(lighting + LIGHTING_FADE);
    for (int32_t k = 0; k < std::max(used, 1); k++) {
        const uint32_t setting = lighting + LIGHTING_SETTINGS + (uint32_t)k * SETTING_SIZE;
        float lo = rd8(setting + SETTING_MIN), hi = rd8(setting + SETTING_MAX);
        if (k == 0 && fade != 1.0f) {
            lo = std::floor(lo * fade);
            hi = std::floor(hi * fade);
        }
        if (k == 0) light[48] = lo / 255.0f;
        if (k < used) {
            light[k * 4] = rdf(setting + SETTING_DIR);
            light[k * 4 + 1] = rdf(setting + SETTING_DIR + 4);
            light[k * 4 + 2] = rdf(setting + SETTING_DIR + 8);
            light[k * 4 + 3] = std::max(hi - lo, 0.0f) / 255.0f;
            colour_of(rd32(setting + SETTING_TABLE), &light[16 + k * 4], &light[32 + k * 4]);
        }
    }
    light[49] = (float)used;
    light[50] = s.character_lights.load() / 100.0f;
    g_object_lights.insert(g_object_lights.end(), light, light + OBJECT_LIGHT_FLOATS);
}

// Smooth surface directions for the level. The level stores no directions, so RT64 otherwise works out one per
// triangle from its shape (flat), and shine and surface detail break at every triangle edge. Here each segment's
// triangles (track segment, src/track.c: Triangle* at +0x04, 16 bytes each with the vertex indices at +1..+3, relative to
// their batch's first vertex; batches at +0x0C, 16 bytes each: first vertex s16 at +6, first triangle s16 at +8,
// count at +0x28, the next batch's first triangle ends a batch) give every corner the average of the directions of
// the triangles meeting there, weighted by their size and welded by position across segments. RasterPS blends it with
// the flat direction and keeps the flat one where they differ a lot (real corners). Worked out once per level and
// sent with the vertex arrays they belong to (both copies the game's lighting keeps too).
constexpr uint32_t SEGMENT_TRIANGLES = 0x04, SEGMENT_BATCHES = 0x0C, SEGMENT_BATCH_COUNT = 0x28;
constexpr uint32_t BATCH_SIZE = 0x10, BATCH_FIRST_VERTEX = 0x06, BATCH_FIRST_TRIANGLE = 0x08, TRIANGLE_SIZE = 0x10;

static void send_level_normals() {
    static uint32_t key[4] = { 0, 0, 0, 0 };
    static std::vector<uint32_t> normals, ranges;
    const uint32_t track = rd32(ADDR_track);
    const uint32_t segments = valid_ptr(track) ? rd32(track + TRACK_SEGMENTS) : 0;
    const int32_t segment_count = valid_ptr(track) ? (int16_t)rd16(track + TRACK_SEGMENT_COUNT) : 0;
    const uint32_t blocks = rd32(ADDR_trackLightBlocks);
    const uint32_t first_block = valid_ptr(blocks) ? rd32(blocks) : 0;
    const uint32_t now[4] = { track, segments, (uint32_t)segment_count, first_block };
    if (std::memcmp(now, key, sizeof(key)) == 0) return;
    std::memcpy(key, now, sizeof(key));
    normals.clear();
    ranges.clear();
    if (!valid_ptr(segments) || segment_count <= 0) {
        rt64_jfg_level_normals(nullptr, 0, nullptr, 0);
        return;
    }

    // Each corner's position (welded) gets the sum of the directions of the triangles meeting there.
    struct Sum { float x = 0, y = 0, z = 0; };
    std::unordered_map<uint64_t, Sum> sums;
    auto pos_key = [](int16_t x, int16_t y, int16_t z) {
        return ((uint64_t)(uint16_t)x << 32) | ((uint64_t)(uint16_t)y << 16) | (uint64_t)(uint16_t)z;
    };
    auto vertex = [](uint32_t verts, uint32_t i, int16_t* v) {
        for (uint32_t c = 0; c < 3; c++) v[c] = (int16_t)rd16(verts + i * VERTEX_SIZE + c * 2);
    };
    const int32_t count = std::min(segment_count, (int32_t)MAX_SEGMENTS);
    for (int pass = 0; pass < 2; pass++) {
        for (int32_t i = 0; i < count; i++) {
            const uint32_t seg = segments + (uint32_t)i * SEGMENT_SIZE;
            const uint32_t verts = rd32(seg + SEGMENT_VERTICES), tris = rd32(seg + SEGMENT_TRIANGLES);
            const uint32_t batches = rd32(seg + SEGMENT_BATCHES);
            const int32_t vertex_count = (int16_t)rd16(seg + SEGMENT_VERTEX_COUNT);
            const int32_t batch_count = (int16_t)rd16(seg + SEGMENT_BATCH_COUNT);
            if (!valid_ptr(verts) || vertex_count <= 0) continue;
            if (pass == 0) {
                if (!valid_ptr(tris) || !valid_ptr(batches)) continue;
                for (int32_t b = 0; b < batch_count && b < 4096; b++) {
                    const uint32_t batch = batches + (uint32_t)b * BATCH_SIZE;
                    const int32_t first_vertex = (int16_t)rd16(batch + BATCH_FIRST_VERTEX);
                    const int32_t t0 = (int16_t)rd16(batch + BATCH_FIRST_TRIANGLE);
                    const int32_t t1 = (int16_t)rd16(batch + BATCH_SIZE + BATCH_FIRST_TRIANGLE);
                    for (int32_t t = t0; t < t1 && t - t0 < 4096; t++) {
                        const uint32_t tri = tris + (uint32_t)t * TRIANGLE_SIZE;
                        int16_t v[3][3];
                        bool ok = true;
                        for (uint32_t k = 0; k < 3; k++) {
                            const int32_t index = first_vertex + rd8(tri + 1 + k);
                            if (index < 0 || index >= vertex_count) { ok = false; break; }
                            vertex(verts, (uint32_t)index, v[k]);
                        }
                        if (!ok) continue;
                        const float ax = (float)(v[1][0] - v[0][0]), ay = (float)(v[1][1] - v[0][1]), az = (float)(v[1][2] - v[0][2]);
                        const float bx = (float)(v[2][0] - v[0][0]), by = (float)(v[2][1] - v[0][1]), bz = (float)(v[2][2] - v[0][2]);
                        const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
                        for (uint32_t k = 0; k < 3; k++) {
                            Sum& sum = sums[pos_key(v[k][0], v[k][1], v[k][2])];
                            sum.x += nx;
                            sum.y += ny;
                            sum.z += nz;
                        }
                    }
                }
            }
            else {
                // Every vertex gets its corner's direction (s8 x, y, z; 0 when the triangles there cancel out), for the
                // segment's own array and the game's two lighting copies of it.
                const uint32_t offset = (uint32_t)normals.size();
                for (int32_t k = 0; k < vertex_count; k++) {
                    int16_t v[3];
                    vertex(verts, (uint32_t)k, v);
                    const auto it = sums.find(pos_key(v[0], v[1], v[2]));
                    uint32_t packed = 0;
                    if (it != sums.end()) {
                        const float len = std::sqrt(it->second.x * it->second.x + it->second.y * it->second.y + it->second.z * it->second.z);
                        if (len > 1e-3f) {
                            const int nx = (int)std::lround(it->second.x / len * 127.0f), ny = (int)std::lround(it->second.y / len * 127.0f);
                            const int nz = (int)std::lround(it->second.z / len * 127.0f);
                            packed = (uint32_t)(uint8_t)nx | ((uint32_t)(uint8_t)ny << 8) | ((uint32_t)(uint8_t)nz << 16);
                        }
                    }
                    normals.push_back(packed);
                }
                uint32_t arrays[3] = { verts, 0, 0 };
                if (valid_ptr(blocks)) {
                    arrays[1] = rd32(blocks + (uint32_t)i * 8);
                    arrays[2] = rd32(blocks + (uint32_t)i * 8 + 4);
                }
                for (uint32_t a : arrays) {
                    if (!valid_ptr(a)) continue;
                    ranges.push_back(a & RDRAM_MASK);
                    ranges.push_back((uint32_t)vertex_count);
                    ranges.push_back(offset);
                }
            }
        }
    }
    rt64_jfg_level_normals(ranges.data(), (uint32_t)(ranges.size() / 3), normals.data(), (uint32_t)normals.size());
    if (lighting_settings().log.load()) {
        char line[160];
        std::snprintf(line, sizeof(line), "level normals: %zu vertices, %zu corners, %zu arrays", normals.size(), sums.size(), ranges.size() / 3);
        jfg_log_line("jfg_lighting.log", line);
    }
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
    const bool active = lighting_active(); // off: the game's original look, nothing of ours applied
    const int mode = active ? s.mode.load() : 0;
    rt64_jfg_pixel_lights.store(mode > 0 ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_debug_world.store(s.debug_world.load() ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_debug_normal.store(s.debug_normal.load() ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_debug_shadow.store(s.debug_shadow.load() ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_facing.store(active ? std::clamp(s.facing.load(), 0, 100) / 100.0f : 0.0f, std::memory_order_relaxed);
    rt64_jfg_shadows.store(active ? std::clamp(s.shadows.load(), 0, 100) / 100.0f : 0.0f, std::memory_order_relaxed);
    rt64_jfg_shadow_lights.store(std::clamp(s.shadow_lights.load(), 0, 4), std::memory_order_relaxed);
    rt64_jfg_shadow_size.store(std::clamp(s.shadow_size.load(), 128, 2048), std::memory_order_relaxed);
    rt64_jfg_level_ambient.store(active ? std::clamp(s.level_ambient.load(), 0, 199) / 100.0f : 1.0f, std::memory_order_relaxed);
    if (s.log.load()) log_object_lights();
    send_level_vertices();
    send_level_normals();
    // Every candidate light (id = its slot's address, so RT64 can match it with the last frame's for interpolation),
    // then the nearest to the camera up to the limit.
    struct Light {
        float d[10]; // x, y, z, radius, inner, r, g, b, id, flags (1 = casts no shadows)
        float dist2;
        bool level;
    };
    static std::vector<Light> found;
    found.clear();
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float cx = rdf(cam + 0x0C), cy = rdf(cam + 0x10), cz = rdf(cam + 0x14);
    auto add = [&](uint32_t id, float x, float y, float z, float radius, float inner, const float* colour, bool level,
                   bool shadows = true) {
        Light light;
        light.d[0] = x;
        light.d[1] = y;
        light.d[2] = z;
        light.d[3] = radius;
        light.d[4] = inner;
        for (uint32_t c = 0; c < 3; c++) light.d[5 + c] = colour[c];
        std::memcpy(&light.d[8], &id, 4);
        light.d[9] = shadows ? 0.0f : 1.0f;
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
        float radius = rdf(l + OBJ_LIGHT_RADIUS);
        if (intensity == 0 || radius <= 0.0f) continue;
        const float x = rdf(l + OBJ_LIGHT_POS), y = rdf(l + OBJ_LIGHT_POS + 4), z = rdf(l + OBJ_LIGHT_POS + 8);
        float colour[3];
        for (uint32_t c = 0; c < 3; c++) {
            colour[c] = (float)((rd8(l + OBJ_LIGHT_COLOUR + c) * intensity) >> 8) / 255.0f * object_strength;
        }
        // As the lighting tool changed it: off, another colour (at the light's own brightness), brighter or dimmer,
        // larger or smaller, flickering.
        const LightOverride* own = lighting_override_for(l);
        if (const LightOverride* o = own) {
            if (!o->on || o->radius <= 0) continue; // reach 0 is off too
            if (o->recolour) {
                const float level = (float)intensity / 256.0f * object_strength;
                colour[0] = o->r / 255.0f * level;
                colour[1] = o->g / 255.0f * level;
                colour[2] = o->b / 255.0f * level;
            }
            const float scale = o->intensity / 100.0f * lighting_flicker(l, o->flicker, o->flicker_speed);
            for (uint32_t c = 0; c < 3; c++) colour[c] *= scale;
            radius *= o->radius / 100.0f;
        }
        add(l, x, y, z, radius * object_radius, radius * 0.3f, colour, false, own == nullptr || own->shadows);
    }

    // The lights placed with the lighting tool (pulsing while the tool shows which one is selected).
    const std::vector<PlacedLight>& placed = lighting_placed_lights();
    int placed_sent = 0;
    for (size_t i = 0; mode >= 1 && i < placed.size(); i++) {
        const PlacedLight& p = placed[i];
        if (!p.on || p.radius <= 0) continue;
        float scale = p.intensity / 100.0f * lighting_flicker(0xC0DE0000u + (uint32_t)i, p.flicker, p.flicker_speed);
        if (lighting_pulse_light() == (int)i) scale *= 0.6f + 0.4f * std::sin(g_tick_count * 0.25f);
        const float colour[3] = { p.r / 255.0f * scale, p.g / 255.0f * scale, p.b / 255.0f * scale };
        add(0xC0DE0000u + (uint32_t)i, p.x, p.y, p.z, (float)p.radius, p.radius * std::clamp(p.inner, 0, 95) / 100.0f, colour, false,
            p.shadows);
        placed_sent++;
    }
    // Lights where the level's emission-mapped textures are (the lighting tool's Glow tab).
    const std::vector<PlacedLight>& emitted = lighting_emission_lights();
    for (size_t i = 0; mode >= 1 && i < emitted.size(); i++) {
        const PlacedLight& p = emitted[i];
        const float scale = p.intensity / 100.0f;
        const float colour[3] = { p.r / 255.0f * scale, p.g / 255.0f * scale, p.b / 255.0f * scale };
        add(0xE0000000u + (uint32_t)i, p.x, p.y, p.z, (float)p.radius, p.radius * std::clamp(p.inner, 0, 95) / 100.0f, colour, false,
            p.shadows);
        placed_sent++;
    }
    s.sent_placed_lights = placed_sent;

    const size_t max_lights = (size_t)std::clamp(s.max_lights.load(), 0, (int)MAX_FRAME_LIGHTS);
    // Nearest first (RT64 gives the first ones that cast shadows a shadow map).
    std::sort(found.begin(), found.end(), [](const Light& a, const Light& b) { return a.dist2 < b.dist2; });
    if (found.size() > max_lights) found.resize(max_lights);
    float data[MAX_FRAME_LIGHTS * 10];
    int level_lights = 0;
    for (size_t i = 0; i < found.size(); i++) {
        std::memcpy(&data[i * 10], found[i].d, sizeof(found[i].d));
        level_lights += found[i].level ? 1 : 0;
    }
    s.sent_level_lights = level_lights;
    s.sent_object_lights = (int)found.size() - level_lights;
    rt64_jfg_frame_lights((uint32_t)ctx->r4 & RDRAM_MASK, data, (uint32_t)found.size());

    // The camera (for the shine), placed on in-between frames as the lights are, and the settings the shaders take.
    // [1]: where the camera looks, a little ahead (the directional light's shadows are made around it).
    const float heading = -angle_from_s16(rd16(cam)), pitch_cam = angle_from_s16(rd16(cam + 2));
    const float ahead = s.sun_shadow_area.load() * 0.6f;
    float points[10] = { cx, cy, cz, 0.0f, 0.0f,
                         cx + std::sin(heading) * std::cos(pitch_cam) * ahead, cy - std::sin(pitch_cam) * ahead,
                         cz + std::cos(heading) * std::cos(pitch_cam) * ahead, 0.0f, 0.0f };
    const uint32_t camera_id = 0xCA3E7A00u, focus_id = 0xCA3E7A01u;
    std::memcpy(&points[4], &camera_id, 4);
    std::memcpy(&points[9], &focus_id, 4);
    rt64_jfg_frame_points((uint32_t)ctx->r4 & RDRAM_MASK, points, 2);
    const bool on = mode > 0;
    const float yaw = s.sun_yaw.load() * 3.14159265f / 180.0f, pitch = s.sun_pitch.load() * 3.14159265f / 180.0f;
    const float sun = s.sun_intensity.load() / 100.0f;
    float params[24] = {};
    params[0] = std::cos(pitch) * std::sin(yaw);
    params[1] = std::sin(pitch);
    params[2] = std::cos(pitch) * std::cos(yaw);
    params[3] = (on && s.sun.load()) ? 1.0f : 0.0f;
    params[4] = s.sun_r.load() / 255.0f * sun;
    params[5] = s.sun_g.load() / 255.0f * sun;
    params[6] = s.sun_b.load() / 255.0f * sun;
    params[7] = std::clamp(s.sun_characters.load(), 0, 200) / 100.0f;
    params[8] = on ? s.detail.load() / 10.0f : 0.0f;
    params[9] = on ? s.shine.load() / 100.0f : 0.0f;
    params[10] = (float)std::clamp(s.sharpness.load(), 1, 256);
    params[11] = std::clamp(s.glow_shadow.load(), 0, 100) / 100.0f;
    params[12] = std::clamp(s.detail_characters.load(), 0, 200) / 100.0f;
    params[13] = s.sun_shadows.load() ? 1.0f : 0.0f;
    params[14] = (float)std::clamp(s.sun_shadow_area.load(), 250, 8000);
    params[15] = (on && s.normal_maps.load()) ? std::clamp(s.normal_strength.load(), 0, 200) / 100.0f : 0.0f;
    params[16] = s.smooth_surfaces.load() ? 1.0f : 0.0f;
    params[17] = on ? s.shine_characters.load() / 100.0f : 0.0f;
    params[18] = (float)std::clamp(s.sharpness_characters.load(), 1, 256);
    params[19] = (on && s.emission_maps.load()) ? std::clamp(s.glow.load(), 0, 200) / 100.0f : 0.0f;
    params[20] = (on && s.specular_maps.load()) ? std::clamp(s.specular_strength.load(), 0, 200) / 100.0f : 0.0f;
    rt64_jfg_lighting_params(params, 24);
    rt64_jfg_frame_objects((uint32_t)ctx->r4 & RDRAM_MASK, g_object_ranges.data(), (uint32_t)(g_object_ranges.size() / 3),
                           g_object_normals.data(), (uint32_t)g_object_normals.size(), g_object_lights.data(),
                           (uint32_t)(g_object_lights.size() / OBJECT_LIGHT_FLOATS));
    g_object_ranges.clear();
    g_object_normals.clear();
    g_object_lights.clear();
}

// Entries of lightSingleLight, lightTwoLights and lightMultiLight (called by lightObject): they write a model's vertex
// colours from the lights near it. Returns non-zero to skip them; the model then keeps whatever its vertex colours
// held before (lightEnviroMap, the shiny models' texture coordinates, still runs).
extern "C" int jfg_object_light(void) {
    const LightingSettings& s = lighting_settings();
    return (s.no_object_lighting.load() || (s.characters.load() && lighting_active())) ? 1 : 0;
}

// Entry of trackLightAsm(track, light, visible). Returns non-zero to skip it.
extern "C" int jfg_track_light(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const LightingSettings& s = lighting_settings();
    if (s.log.load()) log_light((uint32_t)ctx->r5);
    return (s.no_game_lighting.load() || (s.mode.load() > 0 && lighting_active())) ? 1 : 0;
}

} // namespace jfg
