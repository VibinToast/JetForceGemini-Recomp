// Jet Force Gemini port: the lighting tool (F12) for modern lighting (JFG-125, Recomp-Docs RENDERER_ROADMAP.md 3a).
//
// A window of its own (src/debug_ui/backend.cpp opens it with F12) for everything the modern lighting can do:
//   settings     every lighting setting, saved for the level (lighting/level_N.txt, "own settings") or as the defaults
//                every other level uses (lighting/default.txt); quality settings (shadow maps, light count) are global
//   placed lights  lights of one's own, saved with the level: with "place with fire" on, each shot (Z) puts the light
//                being made where the middle of the screen points (a crosshair shows it: the
//                game's own collision ray, trackNearestIntersection, from the camera), or above the player when it
//                hits nothing; each can be moved, recoloured, resized, made to flicker, switched off or removed
//   presets      named sets of settings (lighting/presets/<name>.txt) saved from the current ones and applied to any
//                level, all of them or only some groups (say the directional light and the surfaces)
//   level lights the game's own torches and lamps (its object lights), found again by where they stand: switched off,
//                recoloured, brighter or dimmer, larger or smaller, flickering; saved with the level
//   emission     a texture picker (any texture under the crosshair: its hash for naming its maps) and the lights
//                made where the level's textures with emission maps (materials/<hash>_e.png) are
// Changes apply at once and stay unsaved until Save (Discard goes back to the files): the defaults and the level are
// kept in memory and compared with what was last read or saved. Game thread only: the tool is drawn from jfg_debug_frame's UI
// frame, its tick runs every game tick, and the lighting reads the lights at rcpFast3d, all on the game thread.
#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern std::atomic<int> rt64_jfg_material_files;  // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
extern std::atomic<int> rt64_jfg_material_loaded;  // ...
void rt64_jfg_glow_ranges(const uint32_t* ranges, uint32_t count); // lib/rt64 src/hle/rt64_state.cpp
uint32_t rt64_jfg_glow_hashes(uint32_t start, uint32_t end, uint64_t* hashes, uint32_t max_hashes); // ...
bool rt64_jfg_emission_colour(uint64_t hash, float* rgb); // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
uint32_t rt64_jfg_material_kinds(uint64_t hash);          // ...
void rt64_jfg_material_rescan();                         // ...
void rt64_jfg_pick(bool on);                             // lib/rt64 src/hle/rt64_state.cpp
void rt64_jfg_emitters(bool on);                         // ...
uint32_t rt64_jfg_emitter_cells(uint64_t* hashes, float* xyza, uint32_t max_cells); // ...
bool rt64_jfg_picked(uint64_t* hash, uint32_t* width, uint32_t* height, uint32_t* address); // ...
void rt64_jfg_highlight(uint64_t hash, float amount);    // lib/rt64 src/render/rt64_framebuffer_renderer.cpp
void rt64_jfg_preview_request(uint64_t hash);             // lib/rt64 src/hle/rt64_state.cpp
bool rt64_jfg_preview(uint64_t hash, uint32_t* width, uint32_t* height, std::vector<uint8_t>& rgba); // ...
bool rt64_jfg_material_preview(uint64_t hash, int kind, uint32_t* width, uint32_t* height, std::vector<uint8_t>& rgba); // lib/rt64 src/render/rt64_framebuffer_renderer.cpp

namespace dino::debug_ui::backend {
bool lighting_tool_open();           // F12
bool lighting_tool_playing();        // placement mode: the game has the controls
void set_lighting_tool_playing(bool playing);
void set_lighting_tool_open(bool open);
}

namespace jfg {

void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp

namespace {
// The lighting files' folder: lighting/ next to the game, or JFG_LIGHTING_DIR (test runs on a copy).
const char* lighting_dir() {
    static const char* const dir = std::getenv("JFG_LIGHTING_DIR") ? std::getenv("JFG_LIGHTING_DIR") : "lighting";
    return dir;
}
#define LIGHTING_DIR lighting_dir()
constexpr uint32_t OBJ_POS = 0x0C;                     // ObjectTransform: f32 x, y, z
constexpr uint32_t ADDR_activeLights = 0x800A1008, ADDR_activeLightCount = 0x800A1004; // see jfg_lighting.cpp
constexpr uint32_t OBJ_LIGHT_TYPE = 0x03, OBJ_LIGHT_POS = 0x18, OBJ_LIGHT_RADIUS = 0x24, OBJ_LIGHT_COLOUR = 0x40;
constexpr uint16_t Z_BUTTON = 0x2000;                  // fire
constexpr float SAME_LIGHT = 2.0f;                     // a game light this close to an override's place is that one
// The level (src/track.c, see jfg_lighting.cpp): Track: texture entry* textures (+0x00, 8 bytes each: texture header*),
// segments (+0x04), texture count (s16 +0x18), segment count (s16 +0x1A). Segment (0x48 bytes): Vertex* (+0x00, 10
// bytes each, s16 x, y, z), Triangle* (+0x04, 16 bytes each: vertex indices at +1..+3, from the batch's first vertex),
// batches (+0x0C, 16 bytes each: texture index u8 +0x00, 0xFF = none; first vertex s16 +0x06; first triangle s16
// +0x08, the next batch's ends it), vertex count (s16 +0x24), batch count (s16 +0x28). texture header: u8 width,
// height (+0x00, +0x01), u16 frames x 256 (+0x12, animated textures: the frames follow each other), s16 size of one
// frame with its header (+0x16).
constexpr uint32_t ADDR_track = 0x800A0D60;
constexpr uint32_t TRACK_TEXTURES = 0x00, TRACK_SEGMENTS = 0x04, TRACK_TEXTURE_COUNT = 0x18, TRACK_SEGMENT_COUNT = 0x1A;
constexpr uint32_t SEGMENT_SIZE = 0x48, SEGMENT_VERTICES = 0x00, SEGMENT_TRIANGLES = 0x04, SEGMENT_BATCHES = 0x0C;
constexpr uint32_t SEGMENT_VERTEX_COUNT = 0x24, SEGMENT_BATCH_COUNT = 0x28, VERTEX_SIZE = 10;
constexpr uint32_t BATCH_SIZE = 0x10, BATCH_FIRST_VERTEX = 0x06, BATCH_FIRST_TRIANGLE = 0x08, TRIANGLE_SIZE = 0x10;
constexpr uint32_t TEXTURE_FRAMES = 0x12, TEXTURE_SIZE = 0x16;
constexpr uint32_t RDRAM_MASK = 0x007FFFFF;
constexpr float AIM_REACH = 4000.0f;

// --- settings -----------------------------------------------------------------------------------------------------
enum class Group { Lights, Shadows, Sun, Surfaces, Characters, Debug };
struct Setting {
    const char* key;
    const char* label;
    std::atomic<int>* i;
    std::atomic<bool>* b;
    int lo, hi;
    Group group;
    bool global; // quality: the same everywhere, saved with the defaults
    const char* help;
};

std::vector<Setting>& settings_table() {
    LightingSettings& s = lighting_settings();
    static std::vector<Setting> table = {
        { "mode", "Lights", &s.mode, nullptr, 0, 2, Group::Lights, false,
          "0: as the game. 1: the game's shots and flashes light the level per pixel. 2: its torches and lamps too." },
        { "level_ambient", "Level ambient (%)", &s.level_ambient, nullptr, 0, 199, Group::Lights, false,
          "The level's own colours, its ambient light. 100 is the original; lower is darker." },
        { "level_strength", "Shots and flashes (%)", &s.level_strength, nullptr, 0, 200, Group::Lights, false,
          "Brightness of the game's moving lights: shots, explosions, flashes." },
        { "object_strength", "Torches and lamps (%)", &s.object_strength, nullptr, 0, 200, Group::Lights, false,
          "Brightness of the game's torches and lamps on the level. Their glow is already painted into the level." },
        { "object_radius", "Torch and lamp reach (%)", &s.object_radius, nullptr, 25, 300, Group::Lights, false,
          "How far torches and lamps reach, as a share of their own radius (125 is what the game uses)." },
        { "off_glow", "Glow of lights switched off (%)", &s.off_glow, nullptr, 0, 200, Group::Lights, false,
          "A torch or lamp switched off in the Level lights tab also takes away the glow the level has painted in around "
          "it, this much (its own colour over its reach). 0 leaves the painted glow." },
        { "steady_all", "No flicker (every torch and lamp)", nullptr, &s.steady_all, 0, 1, Group::Lights, false,
          "The game makes torches, fires and lamps flicker. On: every one of them holds still at its average colour, "
          "brightness and size, as Steady does for one light. Set in the defaults for every level, or in a level's own "
          "settings for that level." },
        { "light_tint", "Lights keep the surface's colour (%)", &s.light_tint, nullptr, 0, 100, Group::Lights, false,
          "The level's own colours tint the light that falls on them: blue water lit by a white torch stays blue, even with "
          "the level ambient down. 0: lights add their own colour only (the surface's tint goes with the ambient)." },
        { "facing", "Facing the light (%)", &s.facing, nullptr, 0, 100, Group::Lights, false,
          "How much a surface turned away from a light gets less of it. 0 keeps the game's look." },
        { "max_lights", "Most lights per frame", &s.max_lights, nullptr, 0, 64, Group::Lights, true,
          "The nearest lights are kept. Every lit pixel adds up each light." },
        { "ray_shadows", "Ray-traced shadows", nullptr, &s.ray_shadows, 0, 1, Group::Shadows, false,
          "With ray tracing on: shadows traced with the GPU's ray tracing instead of shadow maps: every light that casts shadows gets them, "
          "with soft edges. Needs a GPU with ray tracing (DirectX 12 or Vulkan); without it the shadow maps stay." },
        { "ray_light_size", "Light size (units)", &s.ray_light_size, nullptr, 0, 100, Group::Shadows, false,
          "How big the lights are for ray-traced shadows: the bigger, the softer a shadow's edge, more so the further "
          "it falls from what casts it. 0: hard edges." },
        { "ray_sun_softness", "Directional light softness (0.1 degrees)", &s.ray_sun_softness, nullptr, 0, 100, Group::Shadows, false,
          "How wide the directional light is in the sky, for its ray-traced shadows: the sun is about 3 (0.3 degrees). "
          "0: hard edges." },
        { "ray_ao", "Ray-traced ambient occlusion", nullptr, &s.ray_ao, 0, 1, Group::Shadows, false,
          "With ray tracing on: corners, crevices and the ground under things are darkened, from rays traced "
          "around each pixel's surface (not only what the screen shows, as the screen-space AO does). It takes the "
          "screen-space AO's place and its smoothing; the Graphics menu's Ambient Occlusion need not be on." },
        { "ray_ao_reach", "Occlusion reach (units)", &s.ray_ao_reach, nullptr, 5, 500, Group::Shadows, false,
          "How far around a surface counts: larger darkens wider areas, smaller only tight corners." },
        { "ray_ao_strength", "Occlusion strength (%)", &s.ray_ao_strength, nullptr, 0, 200, Group::Shadows, false,
          "How much it darkens." },
        { "ray_sky", "Sky occlusion (%)", &s.ray_sky, nullptr, 0, 200, Group::Shadows, false,
          "With ray tracing on: what doesn't see the open sky (under roofs and overhangs, in tunnels and caves) "
          "is darkened, the open stays as it is. Rays traced upwards from each pixel's surface. 0 is off." },
        { "ray_sky_reach", "Sky distance (units)", &s.ray_sky_reach, nullptr, 200, 20000, Group::Shadows, false,
          "How far a ray must get without hitting anything to count as seeing the sky." },
        { "ray_gi", "Ray-traced indirect light", nullptr, &s.ray_gi, 0, 1, Group::Shadows, false,
          "With ray tracing on: light bounces. A grid of probes around the camera traces rays every frame; what "
          "the torches, placed lights and the directional light light up lights its surroundings in turn, and the "
          "sky's light comes in where the sky is seen. Smoothed over frames (a moving light's bounce follows in about "
          "half a second)." },
        { "ray_gi_strength", "Indirect light (%)", &s.ray_gi_strength, nullptr, 0, 400, Group::Shadows, false,
          "How bright the bounced light and the sky's light are." },
        { "ray_gi_spacing", "Probe spacing (units)", &s.ray_gi_spacing, nullptr, 32, 400, Group::Shadows, true,
          "How far apart the probes are: smaller follows walls and corners more closely, larger reaches further from "
          "the camera (the grid is 16 x 8 x 16 probes)." },
        { "ray_sky_light", "Sky light (%)", &s.ray_sky_light, nullptr, 0, 400, Group::Shadows, false,
          "The open sky's light on what sees it, in the sky colour below (with Ray-traced indirect light on). 0 is off." },
        { "sky_r", "Sky colour red", &s.sky_r, nullptr, 0, 255, Group::Shadows, false, "" },
        { "sky_g", "Sky colour green", &s.sky_g, nullptr, 0, 255, Group::Shadows, false, "" },
        { "sky_b", "Sky colour blue", &s.sky_b, nullptr, 0, 255, Group::Shadows, false, "" },
        { "ray_reflect", "Reflections on shiny surfaces (%)", &s.ray_reflect, nullptr, 0, 200, Group::Shadows, false,
          "With ray tracing on: surfaces with a specular map (materials/<hash>_s.png) mirror what is around them, "
          "as much as their map's strength says, more at a grazing angle. 0 is off." },
        { "ray_reflect_water", "Reflections on water (%)", &s.ray_reflect_water, nullptr, 0, 200, Group::Shadows, false,
          "With ray tracing on: water (the level's see-through surfaces facing up) mirrors its surroundings and the "
          "sky, little when looked down into, much at a grazing angle. 0 is off." },
        { "shadows", "Shadow darkness (%)", &s.shadows, nullptr, 0, 100, Group::Shadows, false,
          "How dark shadows from the lights are. 0 is off, as in the game." },
        { "glow_shadow", "Shadows on painted glow (%)", &s.glow_shadow, nullptr, 0, 100, Group::Shadows, false,
          "How much of the torch glow painted into the level a full shadow takes away." },
        { "no_game_shadows", "Game's shadows off", nullptr, &s.no_game_shadows, 0, 1, Group::Shadows, false,
          "Hides the dark blob the game draws under characters and objects." },
        { "shadow_lights", "Lights with shadows", &s.shadow_lights, nullptr, 0, 4, Group::Shadows, true,
          "The nearest lights that cast shadows. Each draws the scene six more times a frame." },
        { "shadow_size", "Shadow detail", &s.shadow_size, nullptr, 128, 1024, Group::Shadows, true,
          "Pixels per side of each shadow map: sharper edges, more memory and work." },
        { "sun", "Directional light", nullptr, &s.sun, 0, 1, Group::Sun, false,
          "A light from one direction over the whole level, like the sun or the moon." },
        { "sun_yaw", "Direction (degrees around)", &s.sun_yaw, nullptr, 0, 359, Group::Sun, false, "" },
        { "sun_pitch", "Height (degrees up)", &s.sun_pitch, nullptr, -30, 90, Group::Sun, false,
          "90 is straight down from above; 0 is level with the horizon." },
        { "sun_r", "Colour R", &s.sun_r, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_g", "Colour G", &s.sun_g, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_b", "Colour B", &s.sun_b, nullptr, 0, 255, Group::Sun, false, "" },
        { "sun_intensity", "Intensity (%)", &s.sun_intensity, nullptr, 0, 200, Group::Sun, false, "" },
        { "sun_characters", "On characters (%)", &s.sun_characters, nullptr, 0, 200, Group::Sun, false,
          "How much of it falls on characters and objects." },
        { "sun_shadows", "Casts shadows", nullptr, &s.sun_shadows, 0, 1, Group::Sun, false,
          "The level, characters and objects shadow it, in a square around where the camera looks. Uses the shadow "
          "darkness from the Shadows tab." },
        { "sun_shadow_area", "Shadow area (units)", &s.sun_shadow_area, nullptr, 250, 8000, Group::Sun, false,
          "Half the size of the square around the view that gets its shadows: larger reaches further, with softer, "
          "blockier edges." },
        { "smooth_surfaces", "Smooth surfaces", nullptr, &s.smooth_surfaces, 0, 1, Group::Surfaces, false,
          "Shine, detail and facing flow across neighbouring planes of the level instead of breaking at every triangle "
          "edge; real corners stay sharp. Off: one direction per triangle." },
        { "detail", "Surface detail", &s.detail, nullptr, 0, 100, Group::Surfaces, false,
          "Bumps from the textures (their light and dark taken as height) that catch the lights. Tenths of a unit." },
        { "normal_maps", "Normal maps", nullptr, &s.normal_maps, 0, 1, Group::Surfaces, false,
          "Authored normal maps: materials/<texture hash>_n.png next to the game (green up), or in a texture pack. Where "
          "a texture has one it replaces the surface detail worked out from it. Off: to compare." },
        { "normal_strength", "Normal map strength (%)", &s.normal_strength, nullptr, 0, 200, Group::Surfaces, false,
          "How deep the normal maps' bumps look." },
        { "specular_maps", "Specular maps", nullptr, &s.specular_maps, 0, 1, Group::Surfaces, false,
          "Authored specular maps: materials/<texture hash>_s.png (red: how strong the shine, green: how sharp). Where "
          "a texture has one it replaces the shine settings for it. Off: to compare." },
        { "specular_strength", "Specular map strength (%)", &s.specular_strength, nullptr, 0, 200, Group::Surfaces, false,
          "How strong the specular maps' shine is." },
        { "emission_maps", "Emission maps", nullptr, &s.emission_maps, 0, 1, Group::Surfaces, false,
          "Emission maps (materials/<texture hash>_e.png): their glow and the lights they make. Off: to compare." },
        { "glow", "Emission (%)", &s.glow, nullptr, 0, 200, Group::Surfaces, false,
          "How bright emission maps (materials/<texture hash>_e.png) are: their glow on the textures and the lights "
          "they make (with Light from emission on top). 0 is off." },
        { "emission_lights", "Emission maps light the level", nullptr, &s.emission_lights, 0, 1, Group::Surfaces, false,
          "Places lights where the level's textures with emission maps are, in their colour, so the glow falls on the "
          "walls, floor and characters around them. Nearby spots of one texture share a light." },
        { "emission_light", "Light from emission (%)", &s.emission_light, nullptr, 0, 400, Group::Surfaces, false,
          "How bright those lights are." },
        { "emission_reach", "Emission light reach (units)", &s.emission_reach, nullptr, 50, 1500, Group::Surfaces, false,
          "How far they reach from a 64 x 64 patch; larger glowing areas reach further (up to twice), smaller less." },
        { "emission_shadows", "Emission lights cast shadows", nullptr, &s.emission_shadows, 0, 1, Group::Surfaces, false,
          "They take shadow maps like other lights (only the nearest lights get one)." },
        { "emission_objects", "Objects' emission maps light too", nullptr, &s.emission_objects, 0, 1, Group::Surfaces, false,
          "Also places lights where objects, characters and effects with emission maps are drawn (doors, panels, "
          "pickups), following them as they move. Up to 24, the largest glowing areas first. Off: only the level's own "
          "textures light." },
        { "detail_characters", "Detail on characters (%)", &s.detail_characters, nullptr, 0, 200, Group::Surfaces, false,
          "How much of the surface detail characters and objects get. Their textures are mostly painted features (faces, "
          "clothes), which don't make good bumps; carved objects do." },
        { "shine", "Shine on the level (%)", &s.shine, nullptr, 0, 200, Group::Surfaces, false,
          "Highlights where a light reflects towards the camera; brighter parts of a texture shine more." },
        { "sharpness", "Level shine sharpness", &s.sharpness, nullptr, 1, 256, Group::Surfaces, false,
          "Small values: broad soft highlights; large values: small sharp ones." },
        { "shine_characters", "Shine on characters (%)", &s.shine_characters, nullptr, 0, 200, Group::Surfaces, false,
          "Highlights on characters and objects, apart from the level's. Needs New lighting for characters (Characters tab)." },
        { "sharpness_characters", "Character shine sharpness", &s.sharpness_characters, nullptr, 1, 256, Group::Surfaces, false,
          "Small values: broad soft highlights; large values: small sharp ones." },
        { "characters", "New lighting for characters", nullptr, &s.characters, 0, 1, Group::Characters, false,
          "Lights characters and objects per pixel with their models' own surface directions, from the same lights." },
        { "character_lights", "Frame's lights on characters (%)", &s.character_lights, nullptr, 0, 100, Group::Characters, false,
          "How much shots, torches and placed lights add on characters on top of the game's own light on them." },
        { "unlit_objects", "Light objects the game leaves unlit", nullptr, &s.unlit_objects, 0, 1, Group::Characters, false,
          "Objects the game draws without any lighting (bridges, crates, props) get the level's lighting: its ambient, "
          "the frame's lights and the directional light, by which way each flat face points (no smooth shading). Off: "
          "they keep the game's fixed look." },
        { "no_object_lighting", "Game's character lighting off", nullptr, &s.no_object_lighting, 0, 1, Group::Characters, false,
          "Characters and objects get no light from the game itself (on by default: the new lighting for characters replaces it)." },
        { "no_game_lighting", "Game's level lights off", nullptr, &s.no_game_lighting, 0, 1, Group::Characters, false,
          "The game's own per-vertex level lighting is skipped (on by default: the modern lights replace it)." },
        { "debug_world", "Show world position", nullptr, &s.debug_world, 0, 1, Group::Debug, true, "" },
        { "debug_normal", "Show surface directions", nullptr, &s.debug_normal, 0, 1, Group::Debug, true, "" },
        { "debug_shadow", "Show shadows in red", nullptr, &s.debug_shadow, 0, 1, Group::Debug, true, "" },
        { "log", "Log lights to jfg_lighting.log", nullptr, &s.log, 0, 1, Group::Debug, true, "" },
    };
    return table;
}

int get(const Setting& t) { return t.i ? t.i->load() : (t.b->load() ? 1 : 0); }
void put(const Setting& t, int v) {
    if (t.i) t.i->store(std::clamp(v, t.lo, t.hi));
    else t.b->store(v != 0);
}

// --- state --------------------------------------------------------------------------------------------------------
int g_level = -32768;          // the level whose file is loaded
bool g_level_own = false;      // it has its own settings (else the defaults)
std::vector<PlacedLight> g_placed;
std::vector<LightOverride> g_overrides;
std::vector<int> g_built_in;   // the settings as built in, for "back to built-in"
std::vector<int> g_defaults;        // the defaults as they are now (every setting but the debug views), unsaved or not
std::vector<int> g_saved_defaults;  // ... as default.txt has them
std::string g_saved_level;          // the level's file as it was read or last saved ("" = none)
bool g_confirm_discard = false;     // asking before Discard
PlacedLight g_template;        // the light placement makes
int g_selected = -1;           // placed light being edited
bool g_pulse = true;           // pulse the selected placed light
bool g_placing = false;        // each shot places a light
uint32_t g_selected_game = 0;  // game light being edited (its object light)
float g_aim[3] = { 0, 0, 0 };
uint32_t g_aim_tick = 0;
float g_player[3] = { 0, 0, 0 };
bool g_have_player = false;
std::string g_message;
uint32_t g_message_tick = 0;
bool g_confirm_clear = false;
bool g_confirm_reset = false;  // asking before Reset
uint32_t g_glow_tab_tick = 0;  // the Textures tab (the picker) was showing
bool g_open_glow_tab = false;  // test switch: show the Textures tab once
// The picker: a texture selected (locked, it stays shown wherever the crosshair goes) and whether the shown one pulses.
struct PickedTexture {
    uint64_t hash = 0;
    uint32_t width = 0, height = 0;
};
PickedTexture g_pick_selected;   // hash 0: none, the picker follows the crosshair
PickedTexture g_pick_shown;      // what the Textures tab shows: the selected one, else the one under the crosshair
bool g_pick_pulse = true;
bool g_fly_test = false;       // test switch: turn the fly camera on in the first level
// Lights from emission maps: the level textures found to have one (their average colour), patches of them grouped by
// texture in cells, and the lights made from those.
struct EmissionPatch {
    int texture;
    float x = 0, y = 0, z = 0; // area-weighted centre (sums while grouping)
    float area = 0;
};
std::vector<int> g_emission_checked;      // per level texture: 0 not yet, 1 has an emission map (patches made)
std::vector<std::array<float, 3>> g_emission_colour; // ... its map's average colour
std::vector<EmissionPatch> g_emission_patches;
std::vector<PlacedLight> g_emission_lights;
uint32_t g_emission_scan_tick = 0;
size_t g_object_emission_lights = 0; // of g_emission_lights, the last ones: from objects' emission maps (RT64 finds them)

bool use_files() { return std::getenv("JFG_LIGHTING_NO_FILES") == nullptr; }
// Test switch: JFG_LIGHTING_TOOL=open opens the tool at start, =place with "place with fire" on (JFG_DEBUG_INPUT can
// then fire), =mouse with the mouse and keyboard on the tool, =textures (or emission, glow) on the Textures tab, =fly on
// the Textures tab with the fly camera turned on two seconds into the first level.
void test_switch() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* env = std::getenv("JFG_LIGHTING_TOOL")) {
        dino::debug_ui::backend::set_lighting_tool_open(true);
        g_placing = std::strcmp(env, "place") == 0;
        g_open_glow_tab = std::strcmp(env, "textures") == 0 || std::strcmp(env, "emission") == 0 || std::strcmp(env, "glow") == 0 || std::strcmp(env, "fly") == 0;
        g_fly_test = std::strcmp(env, "fly") == 0;
        dino::debug_ui::backend::set_lighting_tool_playing(std::strcmp(env, "mouse") != 0);
    }
}
std::filesystem::path default_path() { return std::filesystem::path(LIGHTING_DIR) / "default.txt"; }
std::filesystem::path level_path(int level) { return std::filesystem::path(LIGHTING_DIR) / ("level_" + std::to_string(level) + ".txt"); }
void say(const std::string& text) { g_message = text; g_message_tick = g_tick_count; }

// Key=value lines; "set.<key>" settings, "light.N" placed lights, "override.N" game light changes.
void save_default() {
    if (!use_files()) return;
    std::error_code ec;
    std::filesystem::create_directories(LIGHTING_DIR, ec);
    if (std::FILE* f = std::fopen(default_path().string().c_str(), "w")) {
        std::fputs("# Jet Force Gemini modern lighting: the defaults (lighting tool, F12)\n", f);
        const std::vector<Setting>& table = settings_table();
        for (size_t i = 0; i < table.size() && i < g_defaults.size(); i++) {
            if (table[i].group != Group::Debug) std::fprintf(f, "set.%s=%d\n", table[i].key, g_defaults[i]);
        }
        std::fclose(f);
    }
}

// Reads a lighting file: its settings (set.*) and, from a level's file, whether it has its own settings, its placed
// lights and its changed game lights.
void read_file(const std::filesystem::path& path, bool level_file, bool settings, bool lights = true) {
    std::FILE* f = std::fopen(path.string().c_str(), "r");
    if (!f) return;
    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || !eq) continue;
        *eq = 0;
        const char* key = line;
        const char* value = eq + 1;
        if (std::strncmp(key, "set.", 4) == 0) {
            if (!settings) continue;
            for (const Setting& t : settings_table()) {
                if (std::strcmp(t.key, key + 4) == 0 && !(level_file && t.global)) put(t, std::atoi(value));
            }
        }
        else if (level_file && std::strcmp(key, "own_settings") == 0) {
            g_level_own = std::atoi(value) != 0;
        }
        else if (level_file && lights && std::strncmp(key, "light.", 6) == 0) {
            PlacedLight p;
            int on = 1, shadows = 1;
            if (std::sscanf(value, "%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &p.x, &p.y, &p.z, &p.r, &p.g, &p.b, &p.intensity,
                            &p.radius, &p.inner, &p.flicker, &p.flicker_speed, &on, &shadows) >= 8) {
                p.on = on != 0;
                p.shadows = shadows != 0;
                g_placed.push_back(p);
            }
        }
        else if (level_file && lights && std::strncmp(key, "override.", 9) == 0) {
            LightOverride o;
            int on = 1, recolour = 0, shadows = 1, steady = 0;
            if (std::sscanf(value, "%f,%f,%f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d", &o.x, &o.y, &o.z, &on, &recolour, &o.r, &o.g, &o.b,
                            &o.intensity, &o.radius, &o.flicker, &o.flicker_speed, &shadows, &o.nth, &steady) >= 10) {
                o.on = on != 0;
                o.steady = steady != 0;
                o.shadows = shadows != 0;
                o.recolour = recolour != 0;
                g_overrides.push_back(o);
            }
        }
    }
    std::fclose(f);
}

// The level's file as it would be written now: whether it has its own settings (and them), its placed lights and its
// changed game lights; "" when it has none of those (no file).
std::string level_text() {
    if (g_level < 0 || (!g_level_own && g_placed.empty() && g_overrides.empty())) return "";
    std::string out;
    char line[320];
    std::snprintf(line, sizeof(line), "own_settings=%d\n", g_level_own ? 1 : 0);
    out += line;
    if (g_level_own) {
        for (const Setting& t : settings_table()) {
            if (t.group == Group::Debug || t.global) continue;
            std::snprintf(line, sizeof(line), "set.%s=%d\n", t.key, get(t));
            out += line;
        }
    }
    for (size_t i = 0; i < g_placed.size(); i++) {
        const PlacedLight& p = g_placed[i];
        std::snprintf(line, sizeof(line), "light.%zu=%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, p.x, p.y, p.z, p.r, p.g, p.b,
                      p.intensity, p.radius, p.inner, p.flicker, p.flicker_speed, p.on ? 1 : 0, p.shadows ? 1 : 0);
        out += line;
    }
    for (size_t i = 0; i < g_overrides.size(); i++) {
        const LightOverride& o = g_overrides[i];
        std::snprintf(line, sizeof(line), "override.%zu=%.2f,%.2f,%.2f,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n", i, o.x, o.y, o.z, o.on ? 1 : 0,
                      o.recolour ? 1 : 0, o.r, o.g, o.b, o.intensity, o.radius, o.flicker, o.flicker_speed, o.shadows ? 1 : 0, o.nth,
                      o.steady ? 1 : 0);
        out += line;
    }
    return out;
}

void save_level() {
    if (!use_files() || g_level < 0) return;
    std::error_code ec;
    const std::string text = level_text();
    if (text.empty()) {
        std::filesystem::remove(level_path(g_level), ec); // nothing of its own left
        return;
    }
    std::filesystem::create_directories(LIGHTING_DIR, ec);
    if (std::FILE* f = std::fopen(level_path(g_level).string().c_str(), "w")) {
        std::fprintf(f, "# Jet Force Gemini modern lighting: level %d (lighting tool, F12)\n", g_level);
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
}

// The working defaults follow the settings while the level uses them (and the quality settings, the same everywhere,
// always), every tick.
void sync_defaults() {
    const std::vector<Setting>& table = settings_table();
    if (g_defaults.size() != table.size()) return;
    for (size_t i = 0; i < table.size(); i++) {
        if (table[i].group == Group::Debug) continue;
        if (!g_level_own || table[i].global) g_defaults[i] = get(table[i]);
    }
}

bool defaults_unsaved() { return use_files() && g_defaults != g_saved_defaults; }
bool level_unsaved() { return use_files() && g_level >= 0 && level_text() != g_saved_level; }

// The defaults from the files: built in, then default.txt (into the settings, then the working copy).
void load_defaults() {
    std::vector<Setting>& table = settings_table();
    for (size_t i = 0; i < table.size() && i < g_built_in.size(); i++) {
        if (table[i].group != Group::Debug) put(table[i], g_built_in[i]);
    }
    read_file(default_path(), false, true);
    g_defaults.assign(table.size(), 0);
    for (size_t i = 0; i < table.size(); i++) g_defaults[i] = get(table[i]);
    g_saved_defaults = g_defaults;
}

void save_all() {
    sync_defaults();
    if (defaults_unsaved()) {
        save_default();
        g_saved_defaults = g_defaults;
    }
    if (level_unsaved()) {
        save_level();
        g_saved_level = level_text();
    }
}

// A new level: the defaults as they are now (unsaved changes to them carry over), then the level's own file. Unsaved
// changes to the level before are dropped.
void load_level(int level) {
    if (level_unsaved() && level != g_level) {
        say("Unsaved changes to level " + std::to_string(g_level) + " were dropped (not saved).");
    }
    sync_defaults();
    g_level = level;
    g_level_own = false;
    g_placed.clear();
    g_overrides.clear();
    g_emission_checked.clear();
    g_emission_colour.clear();
    g_emission_patches.clear();
    g_emission_lights.clear();
    g_pick_selected = {};
    g_pick_shown = {};
    g_selected = -1;
    g_selected_game = 0;
    g_confirm_reset = false; // a pending Reset was about the level before
    g_confirm_discard = false;
    if (!use_files()) return;
    std::vector<Setting>& table = settings_table();
    for (size_t i = 0; i < table.size() && i < g_defaults.size(); i++) {
        if (table[i].group != Group::Debug) put(table[i], g_defaults[i]);
    }
    read_file(level_path(level), true, false, true);
    if (g_level_own) read_file(level_path(level), true, true, false);
    g_saved_level = level_text();
}

// Discard: the defaults and the level back to their files.
void discard_all() {
    load_defaults();
    const int level = g_level;
    g_level = -32768; // nothing of the level before counts as unsaved
    g_saved_level = "";
    load_level(level);
}

bool aim_fresh() { return g_aim_tick != 0 && g_tick_count - g_aim_tick <= 3; }

// Where a light goes: the aim point (pulled back a little from the surface it hit), else in front of the player.
bool placement_point(float* out) {
    if (aim_fresh()) {
        float dx = 0, dy = 0, dz = 0;
        if (g_have_player) {
            dx = g_player[0] - g_aim[0];
            dy = g_player[1] + 40.0f - g_aim[1];
            dz = g_player[2] - g_aim[2];
        }
        const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float back = len > 1.0f ? std::min(12.0f, len * 0.5f) / len : 0.0f;
        out[0] = g_aim[0] + dx * back;
        out[1] = g_aim[1] + dy * back;
        out[2] = g_aim[2] + dz * back;
        return true;
    }
    if (g_have_player) {
        out[0] = g_player[0];
        out[1] = g_player[1] + 60.0f;
        out[2] = g_player[2];
        return true;
    }
    return false;
}

void place_light() {
    float p[3];
    if (!placement_point(p)) {
        say("No aim point or player to place the light at.");
        return;
    }
    PlacedLight light = g_template;
    light.x = p[0];
    light.y = p[1];
    light.z = p[2];
    g_placed.push_back(light);
    g_selected = (int)g_placed.size() - 1;
    char text[96];
    std::snprintf(text, sizeof(text), "Light %d placed at %.0f, %.0f, %.0f.", g_selected + 1, p[0], p[1], p[2]);
    say(text);
}

LightOverride* override_for(float x, float y, float z, int nth) {
    for (LightOverride& o : g_overrides) {
        const float dx = o.x - x, dy = o.y - y, dz = o.z - z;
        if (o.nth == nth && dx * dx + dy * dy + dz * dz <= SAME_LIGHT * SAME_LIGHT) return &o;
    }
    return nullptr;
}

// Which of the game's torches and lamps at a light's spot it is: how many come before it in the game's list there
// (the character select room has two at the same place).
int light_nth(uint32_t light) {
    const float x = rdf(light + OBJ_LIGHT_POS), y = rdf(light + OBJ_LIGHT_POS + 4), z = rdf(light + OBJ_LIGHT_POS + 8);
    const uint32_t objects = rd32(ADDR_activeLights);
    const int32_t count = (int32_t)rd32(ADDR_activeLightCount);
    int nth = 0;
    for (int32_t i = 0; valid_ptr(objects) && i < count && i < 256; i++) {
        const uint32_t l = rd32(objects + (uint32_t)i * 4);
        if (l == light) break;
        if (!valid_ptr(l) || !(rd8(l + OBJ_LIGHT_TYPE) & 0x40)) continue;
        const float dx = rdf(l + OBJ_LIGHT_POS) - x, dy = rdf(l + OBJ_LIGHT_POS + 4) - y, dz = rdf(l + OBJ_LIGHT_POS + 8) - z;
        if (dx * dx + dy * dy + dz * dz <= SAME_LIGHT * SAME_LIGHT) nth++;
    }
    return nth;
}

// --- UI helpers ---------------------------------------------------------------------------------------------------
void help(const char* text) {
    if (!text || !text[0]) return;
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

bool slider_int(const char* label, int* v, int lo, int hi) {
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    return ImGui::SliderInt(label, v, lo, hi);
}

void draw_setting(const Setting& t) {
    if (t.b) {
        bool v = t.b->load();
        if (ImGui::Checkbox(t.label, &v)) {
            t.b->store(v);
        }
    }
    else {
        int v = t.i->load();
        if (slider_int(t.label, &v, t.lo, t.hi)) {
            put(t, v);
        }
    }
    help(t.help);
}

void draw_group(Group group) {
    for (const Setting& t : settings_table()) {
        if (t.group != group) continue;
        if (group == Group::Sun && (std::strcmp(t.key, "sun_g") == 0 || std::strcmp(t.key, "sun_b") == 0)) continue;
        if (group == Group::Sun && std::strcmp(t.key, "sun_r") == 0) {
            LightingSettings& s = lighting_settings();
            float c[3] = { s.sun_r.load() / 255.0f, s.sun_g.load() / 255.0f, s.sun_b.load() / 255.0f };
            if (ImGui::ColorEdit3("Colour", c)) {
                s.sun_r = (int)std::lround(c[0] * 255.0f);
                s.sun_g = (int)std::lround(c[1] * 255.0f);
                s.sun_b = (int)std::lround(c[2] * 255.0f);
            }
            continue;
        }
        draw_setting(t);
    }
}

bool colour_edit(const char* label, int* r, int* g, int* b) {
    float c[3] = { *r / 255.0f, *g / 255.0f, *b / 255.0f };
    if (!ImGui::ColorEdit3(label, c)) return false;
    *r = (int)std::lround(c[0] * 255.0f);
    *g = (int)std::lround(c[1] * 255.0f);
    *b = (int)std::lround(c[2] * 255.0f);
    return true;
}

// The fields a placed light has; true when one changed.
bool edit_placed(PlacedLight& p, bool with_position) {
    bool changed = false;
    ImGui::PushID(&p);
    if (with_position) {
        float pos[3] = { p.x, p.y, p.z };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::DragFloat3("Position", pos, 1.0f)) {
            p.x = pos[0];
            p.y = pos[1];
            p.z = pos[2];
            changed = true;
        }
        changed |= ImGui::Checkbox("On", &p.on);
    }
    changed |= ImGui::Checkbox("Casts shadows", &p.shadows);
    help("Off: the light shines through characters and the level without throwing shadows (and leaves its shadow "
         "map to another light).");
    changed |= colour_edit("Colour", &p.r, &p.g, &p.b);
    changed |= slider_int("Intensity (%)", &p.intensity, 0, 400);
    changed |= slider_int("Reach (units)", &p.radius, 10, 2000);
    help("How far the light reaches; it fades to nothing at this distance.");
    changed |= slider_int("Full-strength core (%)", &p.inner, 0, 95);
    help("The share of its reach where it is at full strength before fading.");
    changed |= slider_int("Flicker (%)", &p.flicker, 0, 100);
    help("How much it dims at most as it flickers, like a flame. 0 is steady.");
    changed |= slider_int("Flicker speed (%)", &p.flicker_speed, 10, 400);
    ImGui::PopID();
    return changed;
}

void draw_placed() {
    LightingSettings& s = lighting_settings();
    if (s.mode.load() < 1) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Lights is 0 (off) in the Lights tab: placed lights don't show.");
    ImGui::TextWrapped("With \"Place with fire\" on, a crosshair shows in the middle of the screen: turn the camera so it is "
                       "on a spot and fire (Z) to place the light just in front of it, or above the player when it points "
                       "at nothing. Shots then place lights instead of only shooting.");
    ImGui::Checkbox("Place with fire", &g_placing);
    ImGui::SameLine();
    ImGui::BeginDisabled(!aim_fresh() && !g_have_player);
    if (ImGui::Button(aim_fresh() ? "Place at the screen's middle" : "Place above the player")) place_light();
    ImGui::EndDisabled();
    ImGui::Checkbox("Pulse the selected light", &g_pulse);
    if (ImGui::CollapsingHeader("Light to place", ImGuiTreeNodeFlags_DefaultOpen)) edit_placed(g_template, false);

    ImGui::SeparatorText("Placed lights");
    if (g_placed.empty()) ImGui::TextDisabled("None on this level yet.");
    for (int i = 0; i < (int)g_placed.size(); i++) {
        const PlacedLight& p = g_placed[i];
        char label[128];
        std::snprintf(label, sizeof(label), "%d: %.0f, %.0f, %.0f   reach %d   %s%s##placed%d", i + 1, p.x, p.y, p.z, p.radius,
                      p.on ? "" : "(off) ", p.flicker > 0 ? "flickers" : "", i);
        ImVec4 colour(p.r / 255.0f, p.g / 255.0f, p.b / 255.0f, 1.0f);
        ImGui::ColorButton("##c", colour, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        if (ImGui::Selectable(label, g_selected == i)) g_selected = (g_selected == i) ? -1 : i;
    }
    if (g_selected >= 0 && g_selected < (int)g_placed.size()) {
        ImGui::SeparatorText("Selected light");
        PlacedLight& p = g_placed[g_selected];
        edit_placed(p, true);
        float pt[3];
        ImGui::BeginDisabled(!placement_point(pt));
        if (ImGui::Button(aim_fresh() ? "Move to the screen's middle" : "Move above the player")) {
            placement_point(pt);
            p.x = pt[0];
            p.y = pt[1];
            p.z = pt[2];
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Duplicate")) {
            PlacedLight copy = p;
            copy.x += 20.0f;
            g_placed.push_back(copy);
            g_selected = (int)g_placed.size() - 1;
        }
        ImGui::SameLine();
        if (ImGui::Button("Use for new lights")) g_template = p;
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            g_placed.erase(g_placed.begin() + g_selected);
            g_selected = -1;
        }
    }
    if (!g_placed.empty()) {
        ImGui::Spacing();
        if (!g_confirm_clear) {
            if (ImGui::Button("Remove all placed lights")) g_confirm_clear = true;
        }
        else {
            if (ImGui::Button("Yes, remove them all")) {
                g_placed.clear();
                g_selected = -1;
                g_confirm_clear = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Keep them")) g_confirm_clear = false;
        }
    }
}

void draw_level_lights() {
    ImGui::TextWrapped("The game's own torches and lamps here (nearest to the player first). Changes are kept for this "
                       "level and found again by where the light stands. Their glow painted into the level stays.");
    if (lighting_settings().mode.load() < 2) ImGui::TextColored(ImVec4(1, 0.7f, 0.3f, 1), "Lights is below 2: torches and lamps don't light the level per pixel.");
    struct Row { uint32_t l; float x, y, z, d2; };
    std::vector<Row> rows;
    const uint32_t objects = rd32(ADDR_activeLights);
    const int32_t count = (int32_t)rd32(ADDR_activeLightCount);
    for (int32_t i = 0; valid_ptr(objects) && i < count && i < 256; i++) {
        const uint32_t l = rd32(objects + (uint32_t)i * 4);
        if (!valid_ptr(l) || !(rd8(l + OBJ_LIGHT_TYPE) & 0x40)) continue;
        Row r{ l, rdf(l + OBJ_LIGHT_POS), rdf(l + OBJ_LIGHT_POS + 4), rdf(l + OBJ_LIGHT_POS + 8), 0 };
        if (g_have_player) {
            const float dx = r.x - g_player[0], dy = r.y - g_player[1], dz = r.z - g_player[2];
            r.d2 = dx * dx + dy * dy + dz * dz;
        }
        rows.push_back(r);
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.d2 < b.d2; });
    if (rows.empty()) ImGui::TextDisabled("No torches or lamps active here.");
    const Row* selected = nullptr;
    for (const Row& r : rows) {
        const int nth = light_nth(r.l);
        const LightOverride* o = override_for(r.x, r.y, r.z, nth);
        char label[180];
        char which[16] = "";
        if (nth > 0) std::snprintf(which, sizeof(which), " (#%d here)", nth + 1);
        std::snprintf(label, sizeof(label), "%.0f, %.0f, %.0f%s   %.0f away   reach %.0f%s##game%08X", r.x, r.y, r.z, which, std::sqrt(r.d2),
                      rdf(r.l + OBJ_LIGHT_RADIUS), o ? (o->on ? "   (changed)" : "   (off)") : "", r.l);
        ImVec4 colour(rd8(r.l + OBJ_LIGHT_COLOUR) / 255.0f, rd8(r.l + OBJ_LIGHT_COLOUR + 1) / 255.0f, rd8(r.l + OBJ_LIGHT_COLOUR + 2) / 255.0f, 1.0f);
        ImGui::ColorButton("##c", colour, ImGuiColorEditFlags_NoTooltip, ImVec2(ImGui::GetFontSize(), ImGui::GetFontSize()));
        ImGui::SameLine();
        if (ImGui::Selectable(label, g_selected_game == r.l)) g_selected_game = (g_selected_game == r.l) ? 0 : r.l;
        if (g_selected_game == r.l) selected = &r;
    }
    if (!selected) return;
    ImGui::SeparatorText("Selected torch or lamp");
    const int nth = light_nth(selected->l);
    LightOverride* o = override_for(selected->x, selected->y, selected->z, nth);
    LightOverride edit = o ? *o : LightOverride{};
    if (!o) {
        edit.nth = nth;
        edit.x = selected->x;
        edit.y = selected->y;
        edit.z = selected->z;
        edit.r = rd8(selected->l + OBJ_LIGHT_COLOUR);
        edit.g = rd8(selected->l + OBJ_LIGHT_COLOUR + 1);
        edit.b = rd8(selected->l + OBJ_LIGHT_COLOUR + 2);
    }
    bool changed = ImGui::Checkbox("On", &edit.on);
    help("Off: the light lights nothing (the level, characters, objects). The glow the level has painted in around it stays.");
    changed |= ImGui::Checkbox("Casts shadows", &edit.shadows);
    changed |= ImGui::Checkbox("Own colour", &edit.recolour);
    ImGui::BeginDisabled(!edit.recolour);
    changed |= colour_edit("Colour", &edit.r, &edit.g, &edit.b);
    ImGui::EndDisabled();
    changed |= slider_int("Intensity (% of its own)", &edit.intensity, 0, 400);
    changed |= slider_int("Reach (% of its own)", &edit.radius, 0, 400);
    help("0 turns it off, as unticking On does.");
    changed |= ImGui::Checkbox("Steady (the game's flicker off)", &edit.steady);
    help("The game makes torches and fires flicker on its own. Steady holds this light at its average colour, brightness and size "
         "instead. Flicker below adds the tool's own flicker on top (0: none).");
    changed |= slider_int("Flicker (%)", &edit.flicker, 0, 100);
    changed |= slider_int("Flicker speed (%)", &edit.flicker_speed, 10, 400);
    if (changed) {
        if (o) *o = edit;
        else g_overrides.push_back(edit);
    }
    if (o && ImGui::Button("Back to the game's own")) {
        g_overrides.erase(g_overrides.begin() + (o - g_overrides.data()));
    }
}
// --- glowing textures --------------------------------------------------------------------------------------------
int level_texture_count() {
    const uint32_t track = rd32(ADDR_track);
    return valid_ptr(track) ? std::max(0, (int)(int16_t)rd16(track + TRACK_TEXTURE_COUNT)) : 0;
}

uint32_t level_texture(int index) {
    if (index < 0 || index >= level_texture_count()) return 0;
    const uint32_t list = rd32(rd32(ADDR_track) + TRACK_TEXTURES);
    if (!valid_ptr(list)) return 0;
    const uint32_t header = rd32(list + (uint32_t)index * 8);
    return valid_ptr(header) ? header : 0;
}

// The memory a level texture loads from: all its frames.
bool level_texture_range(int index, uint32_t* start, uint32_t* end) {
    const uint32_t header = level_texture(index);
    if (header == 0) return false;
    const uint32_t size = rd16(header + TEXTURE_SIZE);
    const uint32_t frames = std::clamp((uint32_t)(rd16(header + TEXTURE_FRAMES) >> 8), 1u, 64u);
    if (size == 0) return false;
    *start = header & RDRAM_MASK;
    *end = *start + size * frames;
    return true;
}

// Every level texture not yet found with an emission map to RT64 when that changes, so it notes their hashes (the
// lights from emission maps find them by those).
void send_glow() {
    static std::vector<uint32_t> sent;
    std::vector<uint32_t> ranges;
    if (lighting_settings().emission_lights.load() && lighting_active()) {
        for (int t = 0; t < level_texture_count() && t < 1024; t++) {
            uint32_t start, end;
            if ((t < (int)g_emission_checked.size() && g_emission_checked[t]) || !level_texture_range(t, &start, &end)) continue;
            ranges.push_back(start);
            ranges.push_back(end);
            ranges.push_back(0);
        }
    }
    if (ranges == sent) return;
    sent = ranges;
    rt64_jfg_glow_ranges(ranges.data(), (uint32_t)(ranges.size() / 3));
}

void draw_crosshair(bool on_something);

// A level texture's emission map colour (the average over the hashes RT64 has seen for it), false when none has one.
bool texture_emission(int texture, float* rgb) {
    uint32_t start, end;
    uint64_t hashes[64];
    if (!level_texture_range(texture, &start, &end)) return false;
    const uint32_t count = std::min(rt64_jfg_glow_hashes(start, end, hashes, 64), 64u);
    float sum[3] = { 0, 0, 0 };
    int found = 0;
    for (uint32_t i = 0; i < count; i++) {
        float c[3];
        if (!rt64_jfg_emission_colour(hashes[i], c)) continue;
        for (int k = 0; k < 3; k++) sum[k] += c[k];
        found++;
    }
    if (found == 0) return false;
    for (int k = 0; k < 3; k++) rgb[k] = sum[k] / found;
    return true;
}

// The patches of a texture: its level triangles grouped in 128-unit cells, each with its area-weighted centre.
void add_emission_patches(int texture) {
    constexpr float CELL = 128.0f;
    const uint32_t track = rd32(ADDR_track);
    if (!valid_ptr(track)) return;
    const uint32_t segments = rd32(track + TRACK_SEGMENTS);
    const int32_t segment_count = std::min((int32_t)(int16_t)rd16(track + TRACK_SEGMENT_COUNT), (int32_t)1024);
    if (!valid_ptr(segments)) return;
    std::vector<std::pair<int64_t, EmissionPatch>> cells;
    for (int32_t i = 0; i < segment_count; i++) {
        const uint32_t seg = segments + (uint32_t)i * SEGMENT_SIZE;
        const uint32_t verts = rd32(seg + SEGMENT_VERTICES), tris = rd32(seg + SEGMENT_TRIANGLES), batches = rd32(seg + SEGMENT_BATCHES);
        const int32_t vertex_count = (int16_t)rd16(seg + SEGMENT_VERTEX_COUNT);
        const int32_t batch_count = (int16_t)rd16(seg + SEGMENT_BATCH_COUNT);
        if (!valid_ptr(verts) || !valid_ptr(tris) || !valid_ptr(batches)) continue;
        for (int32_t b = 0; b < batch_count && b < 4096; b++) {
            const uint32_t batch = batches + (uint32_t)b * BATCH_SIZE;
            if (rd8(batch) != texture) continue;
            const int32_t first_vertex = (int16_t)rd16(batch + BATCH_FIRST_VERTEX);
            const int32_t t0 = (int16_t)rd16(batch + BATCH_FIRST_TRIANGLE);
            const int32_t t1 = (int16_t)rd16(batch + BATCH_SIZE + BATCH_FIRST_TRIANGLE);
            for (int32_t t = t0; t < t1 && t - t0 < 4096; t++) {
                const uint32_t tri = tris + (uint32_t)t * TRIANGLE_SIZE;
                float v[3][3];
                bool ok = true;
                for (uint32_t k = 0; k < 3 && ok; k++) {
                    const int32_t index = first_vertex + rd8(tri + 1 + k);
                    if (index < 0 || index >= vertex_count) ok = false;
                    else for (uint32_t c = 0; c < 3; c++) v[k][c] = (float)(int16_t)rd16(verts + (uint32_t)index * VERTEX_SIZE + c * 2);
                }
                if (!ok) continue;
                const float e1[3] = { v[1][0] - v[0][0], v[1][1] - v[0][1], v[1][2] - v[0][2] };
                const float e2[3] = { v[2][0] - v[0][0], v[2][1] - v[0][1], v[2][2] - v[0][2] };
                const float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
                const float area = 0.5f * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                if (area < 1.0f) continue;
                const float c[3] = { (v[0][0] + v[1][0] + v[2][0]) / 3.0f, (v[0][1] + v[1][1] + v[2][1]) / 3.0f, (v[0][2] + v[1][2] + v[2][2]) / 3.0f };
                const int64_t key = ((int64_t)std::floor(c[0] / CELL) & 0xFFFFF) | (((int64_t)std::floor(c[1] / CELL) & 0xFFFFF) << 20) |
                                    (((int64_t)std::floor(c[2] / CELL) & 0xFFFFF) << 40);
                auto it = std::find_if(cells.begin(), cells.end(), [key](const auto& e) { return e.first == key; });
                if (it == cells.end()) {
                    cells.push_back({ key, EmissionPatch{ texture } });
                    it = cells.end() - 1;
                }
                it->second.x += c[0] * area;
                it->second.y += c[1] * area;
                it->second.z += c[2] * area;
                it->second.area += area;
            }
        }
    }
    for (auto& e : cells) {
        if (g_emission_patches.size() >= 512) break;
        EmissionPatch p = e.second;
        p.x /= p.area;
        p.y /= p.area;
        p.z /= p.area;
        g_emission_patches.push_back(p);
    }
}

// Looks for the level textures' emission maps now and then (RT64 knows a texture's hash once it has been drawn, and
// its map's colour once that is loaded), and makes the lights from the patches with the settings.
void update_emission_lights() {
    LightingSettings& s = lighting_settings();
    const int count = std::min(level_texture_count(), 1024);
    if ((int)g_emission_checked.size() != count) {
        g_emission_checked.assign((size_t)count, 0);
        g_emission_colour.assign((size_t)count, { 0, 0, 0 });
        g_emission_patches.clear();
    }
    if (s.emission_lights.load() && lighting_active() && g_tick_count - g_emission_scan_tick >= 30) {
        g_emission_scan_tick = g_tick_count;
        for (int t = 0; t < count; t++) {
            float rgb[3];
            if (g_emission_checked[t] || !texture_emission(t, rgb)) continue;
            g_emission_checked[t] = 1;
            g_emission_colour[t] = { rgb[0], rgb[1], rgb[2] };
            add_emission_patches(t);
        }
    }
    g_emission_lights.clear();
    g_object_emission_lights = 0;
    // Emission (%) is over both: the maps' glow and the lights from them (0: neither).
    const int emission = s.emission_maps.load() ? std::clamp(s.glow.load(), 0, 200) : 0;
    const bool lights = s.emission_lights.load() && emission > 0;
    rt64_jfg_emitters(lights && s.emission_objects.load() && lighting_active());
    if (!lights) return;
    // A light in the map's colour over a patch of `area` (square world units).
    auto add_light = [&](float x, float y, float z, float area, const float* c) {
        const float peak = std::max({ c[0], c[1], c[2] });
        if (peak < 0.002f) return;
        // The hue of the map at full, as bright as how much of it glows (a map a quarter lit gives a full light).
        const float lum = std::min(1.0f, (0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]) * 4.0f);
        PlacedLight l;
        l.x = x;
        l.y = y;
        l.z = z;
        l.r = (int)std::lround(c[0] / peak * 255.0f);
        l.g = (int)std::lround(c[1] / peak * 255.0f);
        l.b = (int)std::lround(c[2] / peak * 255.0f);
        l.intensity = (int)std::lround(lum * s.emission_light.load() * emission / 100.0f);
        l.radius = (int)std::lround(s.emission_reach.load() * std::clamp(std::sqrt(area) / 64.0f, 0.5f, 2.0f));
        l.inner = 10;
        l.shadows = s.emission_shadows.load();
        g_emission_lights.push_back(l);
    };
    for (const EmissionPatch& p : g_emission_patches) add_light(p.x, p.y, p.z, p.area, g_emission_colour[(size_t)p.texture].data());
    // Objects, characters and effects with emission maps, where RT64 saw them drawn last frame (they move).
    if (!s.emission_objects.load()) return;
    constexpr uint32_t MAX_OBJECT_LIGHTS = 24;
    uint64_t hashes[MAX_OBJECT_LIGHTS];
    float xyza[MAX_OBJECT_LIGHTS * 4];
    const uint32_t cells = rt64_jfg_emitter_cells(hashes, xyza, MAX_OBJECT_LIGHTS);
    const size_t before = g_emission_lights.size();
    for (uint32_t i = 0; i < cells; i++) {
        float c[3];
        if (rt64_jfg_emission_colour(hashes[i], c)) add_light(xyza[i * 4], xyza[i * 4 + 1], xyza[i * 4 + 2], xyza[i * 4 + 3], c);
    }
    g_object_emission_lights = g_emission_lights.size() - before;
}

// A level texture's hash as texture dumps and material maps name it (<16 hex digits>, the first of an animated one's
// frames RT64 has seen), "" until it has been drawn while the lights from emission maps ask RT64 to note it.
std::string texture_hash(int texture, int* frames = nullptr) {
    uint32_t start, end;
    uint64_t hashes[64];
    if (!level_texture_range(texture, &start, &end)) return "";
    const uint32_t count = rt64_jfg_glow_hashes(start, end, hashes, 64);
    if (frames) *frames = (int)count;
    if (count == 0) return "";
    std::sort(hashes, hashes + std::min(count, 64u));
    char text[24];
    std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)hashes[0]);
    return text;
}

// A picture in the Textures tab: one 256 x 256 ImGui texture (registered once), the picture in its top-left corner,
// replaced when its key changes.
constexpr int PREVIEW_SIZE = 256;
struct Preview {
    ImTextureData* tex = nullptr;
    uint64_t key = 0;
    uint32_t w = 0, h = 0;

    void set(uint64_t new_key, const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
        if (tex == nullptr) {
            tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, PREVIEW_SIZE, PREVIEW_SIZE);
            ImGui::RegisterUserTexture(tex);
        }
        if (width > (uint32_t)PREVIEW_SIZE || height > (uint32_t)PREVIEW_SIZE) return;
        for (uint32_t y = 0; y < height; y++) std::memcpy(tex->GetPixelsAt(0, (int)y), &rgba[(size_t)y * width * 4], (size_t)width * 4);
        ImTextureDataQueueUpload(tex, 0, 0, (int)width, (int)height);
        key = new_key;
        w = width;
        h = height;
    }

    // Drawn at most `longest` pixels on its long side (whole-number enlargement when `sharp`), over a checkerboard.
    void draw(float longest, bool sharp) const {
        const float unit = ImGui::GetFontSize() / 13.0f;
        const float fit = longest * unit / (float)std::max(std::max(w, h), 1u);
        const float scale = sharp ? std::max(1.0f, std::floor(fit)) : fit;
        const ImVec2 size(w * scale, h * scale);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float cell = 8.0f * unit;
        for (float y = 0; y < size.y; y += cell) {
            for (float x = 0; x < size.x; x += cell) {
                const bool dark = ((int)(x / cell) + (int)(y / cell)) & 1;
                dl->AddRectFilled(ImVec2(at.x + x, at.y + y), ImVec2(at.x + std::min(x + cell, size.x), at.y + std::min(y + cell, size.y)),
                                  dark ? IM_COL32(70, 70, 70, 255) : IM_COL32(110, 110, 110, 255));
            }
        }
        ImGuiPlatformIO& pio = ImGui::GetPlatformIO();
        if (sharp && pio.DrawCallback_SetSamplerNearest) dl->AddCallback(pio.DrawCallback_SetSamplerNearest, nullptr);
        ImGui::Image(tex->GetTexRef(), size, ImVec2(0, 0), ImVec2(w / (float)PREVIEW_SIZE, h / (float)PREVIEW_SIZE));
        if (sharp && pio.DrawCallback_SetSamplerLinear) dl->AddCallback(pio.DrawCallback_SetSamplerLinear, nullptr);
    }
};
Preview g_preview;      // the texture: the original as RT64 decodes it from TMEM (not a pack's replacement)
Preview g_map_preview[3]; // its normal, specular and emission maps (RT64 keeps a small copy of each map it loads)

// The shown texture, enlarged with sharp pixels, and beside it each material map it has, labelled.
void draw_texture_preview(uint64_t hash) {
    rt64_jfg_preview_request(hash);
    if (hash != g_preview.key) {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
        if (rt64_jfg_preview(hash, &w, &h, rgba)) g_preview.set(hash, rgba, w, h);
    }
    const uint32_t kinds = rt64_jfg_material_kinds(hash);
    const char* const names[3] = { "Normal (_n)", "Specular (_s)", "Emission (_e)" };
    bool any_map = false;
    for (int k = 0; k < 3; k++) {
        if (!(kinds & (1u << k))) continue;
        any_map = true;
        if (hash != g_map_preview[k].key) {
            uint32_t w = 0, h = 0;
            std::vector<uint8_t> rgba;
            if (rt64_jfg_material_preview(hash, k, &w, &h, rgba)) g_map_preview[k].set(hash, rgba, w, h);
        }
    }
    if (hash == 0) return;
    ImGui::BeginGroup();
    ImGui::TextDisabled("Texture");
    if (hash == g_preview.key) g_preview.draw(192.0f, true);
    else ImGui::TextDisabled("(shows once it is drawn again)");
    ImGui::EndGroup();
    if (!any_map) return;
    for (int k = 0; k < 3; k++) {
        if (!(kinds & (1u << k))) continue;
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::TextDisabled("%s", names[k]);
        if (hash == g_map_preview[k].key) g_map_preview[k].draw(128.0f, false);
        else ImGui::TextDisabled("(loads when drawn)");
        ImGui::EndGroup();
    }
}

// Any texture under the crosshair (level, characters, objects, effects drawn in 3D; RT64 finds it): its hash, size,
// which material maps it has, buttons to copy the names, and a rescan for new files.
void draw_picked_texture() {
    ImGui::SeparatorText("Texture under the crosshair");
    uint64_t hash = 0;
    uint32_t width = 0, height = 0, address = 0;
    const bool picked = rt64_jfg_picked(&hash, &width, &height, &address);
    draw_crosshair(picked);
    const bool selected = g_pick_selected.hash != 0;
    if (selected) {
        hash = g_pick_selected.hash;
        width = g_pick_selected.width;
        height = g_pick_selected.height;
    }
    g_pick_shown = (selected || picked) ? PickedTexture{ hash, width, height } : PickedTexture{};
    if (!selected) {
        ImGui::BeginDisabled(!picked);
        if (ImGui::Button("Select")) g_pick_selected = g_pick_shown;
        ImGui::EndDisabled();
        help("Locks the texture under the crosshair: it stays shown (and pulses) wherever you look, until Clear.");
    }
    else {
        if (ImGui::Button("Clear")) g_pick_selected = {};
        help("Back to the texture under the crosshair.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Pulse it", &g_pick_pulse);
    help("The shown texture pulses brighter everywhere it is drawn, so you can see which one it is.");
    if (g_pick_shown.hash == 0) {
        ImGui::TextDisabled("Nothing textured in 3D under the crosshair.");
    }
    else {
        char text[24];
        std::snprintf(text, sizeof(text), "%016llX", (unsigned long long)hash);
        ImGui::Text("%s%s  (%ux%u)", selected ? "Selected: " : "", text, width, height);
        const uint32_t kinds = rt64_jfg_material_kinds(hash);
        std::string has;
        if (kinds & 1) has += " normal";
        if (kinds & 2) has += " specular";
        if (kinds & 4) has += " emission";
        ImGui::SameLine();
        ImGui::TextDisabled("maps:%s", has.empty() ? " none" : has.c_str());
        if (ImGui::SmallButton("Copy _e.png name")) {
            ImGui::SetClipboardText((std::string(text) + "_e.png").c_str());
            say(std::string("Copied ") + text + "_e.png");
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy hash")) {
            ImGui::SetClipboardText(text);
            say(std::string("Copied ") + text);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy _n / _s names")) {
            ImGui::SetClipboardText((std::string(text) + "_n.png\n" + text + "_s.png").c_str());
            say("Copied the normal and specular map names.");
        }
        draw_texture_preview(hash);
    }
    if (ImGui::Button(g_fly.active ? "Fly camera: ON" : "Fly camera: off")) g_fly_toggle_request = true;
    help("Fly anywhere to aim at textures (F7; WASD / left stick to fly, mouse / right stick to look). Alt gives the "
         "mouse back to the tool for its buttons.");
    ImGui::SameLine();
    if (ImGui::Button("Rescan material maps")) {
        rt64_jfg_material_rescan();
        for (Preview& p : g_map_preview) p.key = 0; // changed maps show again once reloaded
        say("Material maps scanned again: new and changed files load the next time their texture is drawn.");
    }
    help("After adding or changing files in materials/ (or a texture pack): finds them without restarting. Each "
         "rescan loads the maps again, up to 512 maps a session.");
    ImGui::Spacing();
}

// A setting by its key (the settings table's own widget and help).
void draw_key(const char* key) {
    for (const Setting& t : settings_table()) {
        if (std::strcmp(t.key, key) == 0) draw_setting(t);
    }
}

// The Surfaces tab, in sections: the level's surface directions, the surface detail and shine worked out from the
// textures, and the authored material maps (normal, specular, emission, with the lights emission maps make).
void draw_surfaces() {
    draw_key("smooth_surfaces");
    ImGui::SeparatorText("Surface detail (from the textures)");
    draw_key("detail");
    draw_key("detail_characters");
    ImGui::SeparatorText("Shine (textures without a specular map)");
    draw_key("shine");
    draw_key("sharpness");
    draw_key("shine_characters");
    draw_key("sharpness_characters");
    ImGui::SeparatorText("Material maps");
    ImGui::TextDisabled("materials/<texture hash>_n.png, _s.png, _e.png next to the game or in a texture pack (the "
                        "Textures tab names a texture). %d textures have them, %d maps loaded.",
                        rt64_jfg_material_files.load(), rt64_jfg_material_loaded.load());
    draw_key("normal_maps");
    ImGui::BeginDisabled(!lighting_settings().normal_maps.load());
    draw_key("normal_strength");
    ImGui::EndDisabled();
    draw_key("specular_maps");
    ImGui::BeginDisabled(!lighting_settings().specular_maps.load());
    draw_key("specular_strength");
    ImGui::EndDisabled();
    draw_key("emission_maps");
    ImGui::BeginDisabled(!lighting_settings().emission_maps.load());
    draw_key("glow");
    ImGui::Indent();
    draw_key("emission_lights");
    ImGui::BeginDisabled(!lighting_settings().emission_lights.load());
    draw_key("emission_light");
    draw_key("emission_reach");
    draw_key("emission_shadows");
    draw_key("emission_objects");
    ImGui::EndDisabled();
    int textures = 0;
    for (int c : g_emission_checked) textures += c;
    ImGui::TextDisabled("%d level textures here have emission maps (found once drawn): %zu lights from them", textures,
                        g_emission_lights.size() - g_object_emission_lights);
    if (lighting_settings().emission_objects.load())
        ImGui::TextDisabled("%zu lights from objects' emission maps in view", g_object_emission_lights);
    ImGui::Unindent();
    ImGui::EndDisabled();
}

// The Textures tab: the texture picker.
void draw_textures() {
    g_glow_tab_tick = g_tick_count;
    ImGui::TextWrapped("Point the crosshair at any texture (level, characters, objects) to get its hash: the name for its "
                       "material maps. Select keeps it while you look around; it pulses where it is drawn.");
    draw_picked_texture();
}

// --- presets -----------------------------------------------------------------------------------------------------
// A preset is the settings of the groups chosen when it was saved (set.<key>=<value> lines, as the level files), in
// lighting/presets/<name>.txt. Applying it sets the groups chosen then, for the level's own settings or the defaults,
// whichever the level uses. Quality settings (the same everywhere) and the debug views are never part of one.
const char* const PRESET_GROUP_NAMES[] = { "Lights", "Shadows", "Directional", "Surfaces", "Characters" };
constexpr int PRESET_GROUPS = 5;
bool g_preset_save_groups[PRESET_GROUPS] = { true, true, true, true, true };
bool g_preset_apply_groups[PRESET_GROUPS] = { true, true, true, true, true };
char g_preset_name[64] = "";
std::vector<std::string> g_presets;
bool g_presets_listed = false;
std::string g_preset_delete; // asking to confirm

std::filesystem::path presets_dir() { return std::filesystem::path(LIGHTING_DIR) / "presets"; }

void list_presets() {
    g_presets.clear();
    std::error_code ec;
    for (std::filesystem::directory_iterator it(presets_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() == ".txt") g_presets.push_back(it->path().stem().string());
    }
    std::sort(g_presets.begin(), g_presets.end());
    g_presets_listed = true;
}

// A name usable as a file name: letters, digits, space, - and _.
std::string preset_file_name(const char* name) {
    std::string out;
    for (const char* c = name; *c; c++) {
        const unsigned char ch = (unsigned char)*c;
        if (std::isalnum(ch) || ch == ' ' || ch == '-' || ch == '_') out += (char)ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    return out;
}

void save_preset(const std::string& name) {
    std::error_code ec;
    std::filesystem::create_directories(presets_dir(), ec);
    std::FILE* f = std::fopen((presets_dir() / (name + ".txt")).string().c_str(), "w");
    if (!f) {
        say("Could not write the preset.");
        return;
    }
    std::fprintf(f, "# Jet Force Gemini modern lighting preset (lighting tool, F12)\n");
    for (const Setting& t : settings_table()) {
        const int g = (int)t.group;
        if (t.global || g >= PRESET_GROUPS || !g_preset_save_groups[g]) continue;
        std::fprintf(f, "set.%s=%d\n", t.key, get(t));
    }
    std::fclose(f);
    list_presets();
    say("Preset \"" + name + "\" saved.");
}

void apply_preset(const std::string& name) {
    std::FILE* f = std::fopen((presets_dir() / (name + ".txt")).string().c_str(), "r");
    if (!f) {
        say("Could not read the preset.");
        return;
    }
    int applied = 0;
    char line[256];
    while (std::fgets(line, sizeof(line), f)) {
        char* eq = std::strchr(line, '=');
        if (line[0] == '#' || !eq || std::strncmp(line, "set.", 4) != 0) continue;
        *eq = 0;
        for (const Setting& t : settings_table()) {
            const int g = (int)t.group;
            if (std::strcmp(t.key, line + 4) != 0 || t.global || g >= PRESET_GROUPS || !g_preset_apply_groups[g]) continue;
            put(t, std::atoi(eq + 1));
            applied++;
        }
    }
    std::fclose(f);
    say("Preset \"" + name + "\" applied to " + (g_level_own ? "this level's own settings" : "the defaults") + " (" +
        std::to_string(applied) + " settings).");
}

// The presets bar, always at the top of the tool: pick a preset to apply or delete, save the current settings under a
// name; which groups a save keeps and an apply takes are chosen in the "Groups" popup.
std::string g_preset_selected;

void draw_presets() {
    if (!g_presets_listed) list_presets();
    if (!g_preset_selected.empty() && std::find(g_presets.begin(), g_presets.end(), g_preset_selected) == g_presets.end()) {
        g_preset_selected.clear();
    }
    const float unit = ImGui::GetFontSize();

    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Preset");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(unit * 11.0f);
    if (ImGui::BeginCombo("##preset", g_preset_selected.empty() ? (g_presets.empty() ? "(none saved)" : "(choose)") : g_preset_selected.c_str())) {
        for (const std::string& p : g_presets) {
            if (ImGui::Selectable(p.c_str(), p == g_preset_selected)) {
                g_preset_selected = p;
                g_preset_delete.clear();
                std::snprintf(g_preset_name, sizeof(g_preset_name), "%s", p.c_str());
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(g_preset_selected.empty());
    if (ImGui::Button("Apply")) apply_preset(g_preset_selected);
    help(g_level_own ? "Sets this level's own settings from the preset (the groups ticked under Groups)."
                     : "Sets the defaults from the preset (this level has no settings of its own; the groups ticked under Groups).");
    ImGui::SameLine();
    if (g_preset_delete == g_preset_selected && !g_preset_selected.empty()) {
        if (ImGui::Button("Yes, delete")) {
            std::error_code ec;
            std::filesystem::remove(presets_dir() / (g_preset_selected + ".txt"), ec);
            g_preset_delete.clear();
            g_preset_selected.clear();
            list_presets();
            say("Preset deleted.");
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep")) g_preset_delete.clear();
    }
    else if (ImGui::Button("Delete")) g_preset_delete = g_preset_selected;
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(unit * 11.0f);
    ImGui::InputTextWithHint("##presetname", "name to save as", g_preset_name, sizeof(g_preset_name));
    const std::string name = preset_file_name(g_preset_name);
    const bool exists = std::find(g_presets.begin(), g_presets.end(), name) != g_presets.end();
    ImGui::SameLine();
    ImGui::BeginDisabled(name.empty());
    if (ImGui::Button(exists ? "Overwrite" : "Save current")) {
        save_preset(name);
        g_preset_selected = name;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Groups...")) ImGui::OpenPopup("preset_groups");
    if (ImGui::BeginPopup("preset_groups")) {
        ImGui::TextDisabled("A save keeps:");
        for (int g = 0; g < PRESET_GROUPS; g++) {
            ImGui::PushID(g);
            ImGui::Checkbox(PRESET_GROUP_NAMES[g], &g_preset_save_groups[g]);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Apply takes:");
        for (int g = 0; g < PRESET_GROUPS; g++) {
            ImGui::PushID(100 + g);
            ImGui::Checkbox(PRESET_GROUP_NAMES[g], &g_preset_apply_groups[g]);
            ImGui::PopID();
        }
        ImGui::Separator();
        ImGui::TextDisabled("Files: %s", presets_dir().string().c_str());
        ImGui::EndPopup();
    }
}
} // namespace

// --- what the lighting reads --------------------------------------------------------------------------------------
const std::vector<PlacedLight>& lighting_placed_lights() { return g_placed; }
const std::vector<PlacedLight>& lighting_emission_lights() { return g_emission_lights; }

const LightOverride* lighting_override_for(uint32_t light) {
    if (g_overrides.empty()) return nullptr;
    return override_for(rdf(light + OBJ_LIGHT_POS), rdf(light + OBJ_LIGHT_POS + 4), rdf(light + OBJ_LIGHT_POS + 8), light_nth(light));
}

float lighting_flicker(uint32_t seed, int amount, int speed) {
    if (amount <= 0) return 1.0f;
    static const auto start = std::chrono::steady_clock::now();
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() * std::max(speed, 1) / 100.0f;
    const float phase = (float)((seed * 2654435761u) >> 8) / (float)(1u << 24) * 6.2831853f;
    // A flame: a slow sway and quicker flutters, 0 to 1.
    const float n = 0.5f + 0.22f * std::sin(t * 5.1f + phase) + 0.16f * std::sin(t * 11.7f + phase * 1.7f) +
                    0.12f * std::sin(t * 23.3f + phase * 2.9f);
    return 1.0f - std::clamp(amount, 0, 100) / 100.0f * std::clamp(n, 0.0f, 1.0f);
}

int lighting_pulse_light() {
    return (g_pulse && dino::debug_ui::backend::lighting_tool_open()) ? g_selected : -1;
}

// The test switches turn it on for the session without touching the saved option; the tool's button then switches
// that instead.
int g_forced_lighting = -1; // -1: follow the option, 0 / 1: off / on for this session

bool lighting_active() {
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (std::getenv("JFG_PIXEL_LIGHTS") != nullptr) g_forced_lighting = 1;
        if (const char* env = std::getenv("JFG_MODERN_LIGHTING")) g_forced_lighting = (std::atoi(env) != 0) ? 1 : 0; // 0: off
    }
    return (g_forced_lighting >= 0) ? (g_forced_lighting != 0) : g.modern_lighting;
}

// The same for ray tracing: JFG_RAY_TRACING=1 / 0 for this session.
int g_forced_ray_tracing = -1;

bool ray_tracing_active() {
    static bool checked = false;
    if (!checked) {
        checked = true;
        if (const char* env = std::getenv("JFG_RAY_TRACING")) g_forced_ray_tracing = (std::atoi(env) != 0) ? 1 : 0;
    }
    return (g_forced_ray_tracing >= 0) ? (g_forced_ray_tracing != 0) : g.ray_tracing;
}

namespace {
// Modern lighting on / off, from the button at the top of the tool (the Graphics option, saved; or for this session
// when a test switch turned it on).
void toggle_lighting() {
    if (g_forced_lighting >= 0) {
        g_forced_lighting = g_forced_lighting ? 0 : 1;
        return;
    }
    g.modern_lighting = !g.modern_lighting;
    save_options();
}

// ... and every ray-traced feature (the Ray Tracing option), from the button below it.
void toggle_ray_tracing() {
    if (g_forced_ray_tracing >= 0) {
        g_forced_ray_tracing = g_forced_ray_tracing ? 0 : 1;
        return;
    }
    g.ray_tracing = !g.ray_tracing;
    save_options();
}
} // namespace

namespace {
// Where the middle of the screen points: the camera's view ray (Camera: s16 yaw at +0, s16 pitch at +2, position at
// +0x0C; the fly camera's conventions, jfg_camera.cpp) against the level with the game's trackNearestIntersection(0,
// &start, &end, &result, 0x2000, 0), as frontPlayerTarget calls it for the aim: non-zero on a hit, the distance to
// it at result + 0x20. The vectors live in the gap below $sp that call_game leaves.
void update_aim(recomp_context* ctx) {
    constexpr float REACH = AIM_REACH;
    const uint32_t cam = ADDR_cameraActorArray + Camera::Size * (rd32(ADDR_cameraNo) & 3u);
    const bool fly = g_fly.active && g_fly.started; // the fly camera's view: it is put in the game's camera only to draw
    const float heading = fly ? g_fly.heading : -angle_from_s16(Camera::Yaw::get(cam)), pitch = fly ? g_fly.pitch : angle_from_s16(Camera::Pitch::get(cam));
    const float dir[3] = { std::sin(heading) * std::cos(pitch), -std::sin(pitch), std::cos(heading) * std::cos(pitch) };
    const float from[3] = { fly ? g_fly.x : Camera::PosX::get(cam), fly ? g_fly.y : Camera::PosY::get(cam), fly ? g_fly.z : Camera::PosZ::get(cam) };
    const uint32_t base = ((uint32_t)ctx->r29 - 0xA0u) & ~0xFu;
    const uint32_t start = base, end = base + 0x10, result = base + 0x20;
    for (uint32_t c = 0; c < 3; c++) {
        wrf(start + c * 4, from[c]);
        wrf(end + c * 4, from[c] + dir[c] * REACH);
    }
    for (uint32_t i = 0; i < 0x40; i += 4) wr32(result + i, 0);
    const uint32_t hit = call_game(trackNearestIntersection, ctx, { 0, start, end, result, 0x2000, 0 });
    if (hit == 0) return;
    const float distance = rdf(result + 0x20);
    if (!(distance > 0.0f && distance <= REACH)) return;
    for (uint32_t c = 0; c < 3; c++) g_aim[c] = from[c] + dir[c] * distance;
    g_aim_tick = g_tick_count;
}
} // namespace

void lighting_tool_tick(recomp_context* ctx) {
    if (g_built_in.empty()) {
        for (const Setting& t : settings_table()) g_built_in.push_back(get(t));
        if (use_files()) load_defaults(); // the defaults' global settings at once
        else g_defaults = g_saved_defaults = g_built_in;
    }
    test_switch();
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != g_level) {
        load_level(level);
        // Test switch: JFG_LIGHTING_PRESET=<name> applies that preset at every level load (scripted runs).
        static const char* const preset = std::getenv("JFG_LIGHTING_PRESET");
        if (preset != nullptr) apply_preset(preset);
    }

    g_have_player = false;
    if (valid_ptr(rd32(ADDR_gameplay))) {
        const uint32_t player = player_object(ctx);
        if (valid_ptr(player)) {
            for (uint32_t c = 0; c < 3; c++) g_player[c] = rdf(player + OBJ_POS + c * 4);
            g_have_player = true;
        }
    }

    if (dino::debug_ui::backend::lighting_tool_open() && g_have_player) update_aim(ctx);

    // JFG_LIGHTING_LOG_AIM: logs the texture under the crosshair as it changes, and the level's textures (scripted runs).
    static const bool log_aim = std::getenv("JFG_LIGHTING_LOG_AIM") != nullptr;
    update_emission_lights();
    if (log_aim && g_tick_count % 300 == 0 && g_object_emission_lights > 0) {
        const PlacedLight& l = g_emission_lights[g_emission_lights.size() - g_object_emission_lights];
        char line[160];
        std::snprintf(line, sizeof(line), "object emission lights: %zu (largest at %.0f, %.0f, %.0f, colour %d %d %d, %d%%, reach %d)",
                      g_object_emission_lights, l.x, l.y, l.z, l.r, l.g, l.b, l.intensity, l.radius);
        jfg_log_line("jfg_lighting.log", line);
    }
    send_glow();
    const bool emission_tab = dino::debug_ui::backend::lighting_tool_open() && g_tick_count - g_glow_tab_tick <= 3;
    rt64_jfg_pick(emission_tab || log_aim);
    // The shown texture pulses while the Textures tab shows: brighter and back, about twice a second.
    const bool pulse = emission_tab && g_pick_pulse && g_pick_shown.hash != 0;
    rt64_jfg_highlight(pulse ? g_pick_shown.hash : 0, pulse ? 0.5f + 0.5f * std::sin(g_tick_count * 0.4f) : 0.0f);
    if (log_aim) {
        static uint64_t logged = 0;
        uint64_t hash = 0;
        uint32_t w = 0, h = 0, address = 0;
        if (rt64_jfg_picked(&hash, &w, &h, &address) && hash != logged) {
            logged = hash;
            char line[160];
            int level_texture = -1;
            for (int t = 0; t < std::min(level_texture_count(), 1024) && level_texture < 0; t++) {
                uint32_t start, end;
                if (level_texture_range(t, &start, &end) && (address & RDRAM_MASK) >= start && (address & RDRAM_MASK) < end) level_texture = t;
            }
            std::snprintf(line, sizeof(line), "pick: %016llX %ux%u from %08X (level texture %d; %s)", (unsigned long long)hash, w, h,
                          address, level_texture, level_texture < 0 ? "not the level's: a character, object or effect" : "the level's");
            jfg_log_line("jfg_lighting.log", line);
        }
    }

    // JFG_LIGHTING_LOG_AIM also lists the level's textures once, ten seconds into a level: hash, area and centre, for
    // picking one to give an emission map in scripted runs.
    static int listed_level = -32768;
    static uint32_t level_tick = 0;
    if (g_level != listed_level) {
        listed_level = g_level;
        level_tick = g_tick_count;
    }
    if (g_fly_test && g_have_player && level_tick != 0 && g_tick_count - level_tick == 60) {
        g_fly_test = false;
        g_fly_toggle_request = true;
    }
    if (log_aim && level_tick != 0 && g_tick_count - level_tick == 300) {
        const size_t before = g_emission_patches.size();
        for (int t = 0; t < std::min(level_texture_count(), 1024); t++) {
            add_emission_patches(t);
            float area = 0, x = 0, y = 0, z = 0;
            const size_t cells = g_emission_patches.size() - before;
            for (size_t i = before; i < g_emission_patches.size(); i++) {
                const EmissionPatch& p = g_emission_patches[i];
                area += p.area;
                x += p.x * p.area;
                y += p.y * p.area;
                z += p.z * p.area;
            }
            g_emission_patches.resize(before);
            if (area <= 0) continue;
            char line[200];
            std::snprintf(line, sizeof(line), "texture %d: hash %s, %zu cells, area %.0f, centre %.0f, %.0f, %.0f", t,
                          texture_hash(t).c_str(), cells, area, x / area, y / area, z / area);
            jfg_log_line("jfg_lighting.log", line);
        }
    }

    // "Place with fire": each shot places the light being made.
    if (dino::debug_ui::backend::lighting_tool_open() && g_placing) {
        if (rd16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u)) & Z_BUTTON) place_light();
    }

    sync_defaults();
}

namespace {
// The crosshair: the middle of the screen, where the ray goes; green on something, orange on nothing.
void draw_crosshair(bool on_something) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImVec2 c(ImGui::GetIO().DisplaySize.x * 0.5f, ImGui::GetIO().DisplaySize.y * 0.5f);
    const float r = std::max(8.0f, ImGui::GetIO().DisplaySize.y / 90.0f);
    const ImU32 col = on_something ? IM_COL32(120, 255, 140, 230) : IM_COL32(255, 170, 80, 230);
    dl->AddCircle(c, r, col, 24, 2.0f);
    dl->AddLine(ImVec2(c.x - r * 1.8f, c.y), ImVec2(c.x - r * 0.5f, c.y), col, 2.0f);
    dl->AddLine(ImVec2(c.x + r * 0.5f, c.y), ImVec2(c.x + r * 1.8f, c.y), col, 2.0f);
    dl->AddLine(ImVec2(c.x, c.y - r * 1.8f), ImVec2(c.x, c.y - r * 0.5f), col, 2.0f);
    dl->AddLine(ImVec2(c.x, c.y + r * 0.5f), ImVec2(c.x, c.y + r * 1.8f), col, 2.0f);
}
} // namespace

void draw_lighting_tool(recomp_context* ctx) {
    (void)ctx;
    LightingSettings& s = lighting_settings();
    ImGui::GetIO().FontGlobalScale = 1.0f;
    ImGui::SetNextWindowSize(ImVec2(620, 760), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 640, 20), ImGuiCond_FirstUseEver);
    const bool playing = dino::debug_ui::backend::lighting_tool_playing();
    if (playing) ImGui::SetNextWindowBgAlpha(0.55f);
    if (ImGui::Begin("Lighting tool (F12)")) {
        ImGui::SetWindowFontScale(std::max(1.0f, ImGui::GetIO().DisplaySize.y / 900.0f));
        if (playing) ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "The game has the controls. Alt: mouse and keyboard to the tool.");
        else ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.4f, 1.0f), "The tool has the mouse and keyboard. Alt: back to the game.");
        if (g_placing) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.5f, 1.0f), "PLACING: point the crosshair and fire (Z) to place a light.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Stop placing")) g_placing = false;
            if (aim_fresh()) ImGui::Text("Crosshair on %.0f, %.0f, %.0f", g_aim[0], g_aim[1], g_aim[2]);
            else ImGui::Text("Crosshair on nothing: the light goes above the player.");
            draw_crosshair(aim_fresh());
        }
        // Modern / original lighting, to compare (the same as the Graphics option).
        {
            const bool on = lighting_active();
            ImGui::PushStyleColor(ImGuiCol_Button, on ? ImVec4(0.15f, 0.55f, 0.25f, 1.0f) : ImVec4(0.45f, 0.35f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, on ? ImVec4(0.2f, 0.65f, 0.3f, 1.0f) : ImVec4(0.55f, 0.45f, 0.2f, 1.0f));
            if (ImGui::Button(on ? "Lighting: MODERN" : "Lighting: ORIGINAL", ImVec2(ImGui::GetFontSize() * 11.0f, 0.0f))) toggle_lighting();
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextDisabled(on ? "click for the game's original lighting" : "click for the modern lighting");
            // Every ray-traced feature on / off (the Ray Tracing option); which ones, in the Shadows tab.
            const bool rays = ray_tracing_active();
            ImGui::PushStyleColor(ImGuiCol_Button, rays ? ImVec4(0.15f, 0.55f, 0.25f, 1.0f) : ImVec4(0.45f, 0.35f, 0.15f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, rays ? ImVec4(0.2f, 0.65f, 0.3f, 1.0f) : ImVec4(0.55f, 0.45f, 0.2f, 1.0f));
            if (ImGui::Button(rays ? "Ray tracing: ON" : "Ray tracing: OFF", ImVec2(ImGui::GetFontSize() * 11.0f, 0.0f))) toggle_ray_tracing();
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            ImGui::TextDisabled(rays ? "click to turn every ray-traced feature off" : "click for the ray-traced features (Shadows tab)");
        }
        load_level_names();
        const std::string name = (g_level >= 0 && g_level < (int)g_level_names.size()) ? g_level_names[g_level] : "";
        ImGui::Text("Level %d %s", g_level, name.c_str());
        ImGui::SameLine();
        ImGui::TextDisabled(g_level_own ? "(its own settings)" : "(the defaults)");
        if (!g_level_own) {
            if (ImGui::Button("Give this level its own settings")) {
                g_level_own = true;
                say("This level now keeps its own settings; the defaults stay as they were.");
            }
            help("Copies the current settings into this level's own settings (saved with Save). Changes then stay with this level only.");
        }
        else {
            if (ImGui::Button("Back to the defaults for this level")) {
                g_level_own = false;
                std::vector<Setting>& table = settings_table();
                for (size_t i = 0; i < table.size() && i < g_defaults.size(); i++) {
                    if (table[i].group != Group::Debug && !table[i].global) put(table[i], g_defaults[i]);
                }
                say("This level uses the defaults again (its placed lights and changed torches stay). Save to keep it.");
            }
            help("Forgets this level's own settings; it uses the defaults again.");
        }
        ImGui::SameLine();
        // Reset: the settings back to how the port ships them, in whichever the level uses, after a confirmation that
        // says which; on a level without its own settings that is the defaults, shared by every such level. Quality
        // settings (the same everywhere) and the debug views are left alone.
        if (!g_confirm_reset) {
            if (ImGui::Button("Reset...")) g_confirm_reset = true;
            help("Puts the settings back to how the port ships them, for this level's own settings or the defaults "
                 "(whichever it uses). Asks first.");
        }
        else {
            ImGui::NewLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "%s", g_level_own
                ? "Reset this level's own settings to the built-in ones?"
                : "Reset the DEFAULTS to the built-in ones? Every level without its own settings changes.");
            if (ImGui::Button("Yes, reset")) {
                std::vector<Setting>& table = settings_table();
                for (size_t i = 0; i < table.size() && i < g_built_in.size(); i++) {
                    if (table[i].group != Group::Debug && !table[i].global) put(table[i], g_built_in[i]);
                }
                g_confirm_reset = false;
                say(g_level_own ? "This level's settings are back to the built-in ones." : "The defaults are back to the built-in ones.");
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) g_confirm_reset = false;
        }
        if (!g_message.empty() && g_tick_count - g_message_tick < 60 * 6) ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f), "%s", g_message.c_str());
        // Unsaved changes: what they are about, Save, Discard (asks first).
        {
            const bool defaults = defaults_unsaved(), level = level_unsaved();
            if (defaults || level) {
                std::string what = level ? "this level" : "";
                if (defaults) what += level ? " and the defaults" : "the defaults";
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.25f, 1.0f), "Unsaved changes to %s.", what.c_str());
                if (defaults && !g_level_own) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("(the defaults: every level without its own settings)");
                }
                if (!g_confirm_discard) {
                    if (ImGui::Button("Save")) {
                        save_all();
                        say("Saved.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Discard...")) g_confirm_discard = true;
                    help("Unsaved changes stay while you play; changing level drops this level's unsaved changes (the "
                         "defaults' stay), and closing the game drops them all.");
                }
                else {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.35f, 1.0f), "Throw away the unsaved changes and go back to the saved files?");
                    if (ImGui::Button("Yes, discard")) {
                        discard_all();
                        g_confirm_discard = false;
                        say("Back to the saved settings.");
                    }
                    ImGui::SameLine();
                    if (ImGui::Button("Keep them")) g_confirm_discard = false;
                }
            }
            else g_confirm_discard = false;
        }
        ImGui::TextDisabled("Last frame: %d level lights, %d torches and lamps, %d placed", s.sent_level_lights.load(),
                            s.sent_object_lights.load(), s.sent_placed_lights.load());
        ImGui::Separator();
        draw_presets();
        ImGui::Separator();
        if (ImGui::BeginTabBar("lighting_tabs")) {
            if (ImGui::BeginTabItem("Lights")) { draw_group(Group::Lights); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Shadows")) { draw_group(Group::Shadows); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Directional")) { draw_group(Group::Sun); ImGui::EndTabItem(); }
            // Test switch: JFG_LIGHTING_TAB=<tab name> shows that tab first (scripted runs).
            static const char* first_tab = std::getenv("JFG_LIGHTING_TAB");
            auto tab_flags = [](const char* name) {
                return (first_tab != nullptr && std::strcmp(first_tab, name) == 0 && ImGui::GetFrameCount() < 120) ? ImGuiTabItemFlags_SetSelected : 0;
            };
            if (ImGui::BeginTabItem("Surfaces", nullptr, tab_flags("Surfaces"))) { draw_surfaces(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Textures", nullptr, g_open_glow_tab ? ImGuiTabItemFlags_SetSelected : 0)) { draw_textures(); ImGui::EndTabItem(); }
            g_open_glow_tab = false;
            if (ImGui::BeginTabItem("Characters")) { draw_group(Group::Characters); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Placed lights")) { draw_placed(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Level lights")) { draw_level_lights(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Debug")) {
                draw_group(Group::Debug);
                ImGui::TextDisabled("Files: %s, %s", default_path().string().c_str(), level_path(g_level).string().c_str());
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }
    ImGui::End();
}

} // namespace jfg
