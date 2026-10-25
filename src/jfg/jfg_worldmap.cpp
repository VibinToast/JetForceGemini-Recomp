// Jet Force Gemini port: mods' planets on the planet-select map (the planet registry, MODDING_API_PLAN §4).
//
// A mod lists its planets in jfg_world.json:
//   { "format": 1, "planets": [ {
//       "id": "zephyra", "name": "Zephyra",
//       "levels": ["level_x", "level_x", "level_x"],      entry level for Vela, Juno, Lupus: a level the mod appends
//                                                         ("level_x", numbered at load) or "stock:N"
//       "disc": { "looks": 2, "x": 160, "y": 25, "radius": 14, "colour": [r, g, b] },
//                                                         looks: the stock disc whose textures it wears (0-10)
//       "routes": [ { "from": 0, "dir": 2, "back": 3, "corners": [[x, y], ...] } ] } ] }
//                                                         from: a stock map point; dir: the stick direction from it
//                                                         (0 left, 1 right, 2 up, 3 down), back: the way back
//
// The map's tables live in overlay 9 and are full (the decomp's MOD_SUPPORT_REQUESTS, "the planet-select map's code"),
// so the game's own map code keeps running unchanged and the port adds the mods' planets around it (frontMap's
// wrapper):
// - before the game's frame: a stick move along a mod's route from the current stock point selects that planet; while
//   one is selected the port owns the stick and the A press (the game sees neither), moves back along the route, and
//   starts the planet's level the way the map does (mainChangeLevel, or pauseSetNewLevel on the pause map);
// - during it, just before the map's frame and hero wheel are drawn (the wrapper of the step that draws the route
//   rectangles): the routes, discs (built by the game's own disc builder, drawn by its own disc drawer) and names.
// A planet shows when the stock point its route leaves from is unlocked. The map's map coordinates: x right, y down,
// the view centred on the selected point (scroll x, scroll y + 120).
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" int32_t* section_addresses; // librecomp: each code section's address this run (overlay 9 = section 9)

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}
extern "C" {
void amSndPlay(uint8_t* rdram, recomp_context* ctx);
void camStandardOrtho(uint8_t* rdram, recomp_context* ctx);
void texDPInit(uint8_t* rdram, recomp_context* ctx);
void fontUseFont(uint8_t* rdram, recomp_context* ctx);
void fontColour(uint8_t* rdram, recomp_context* ctx);
void fontPrintXY(uint8_t* rdram, recomp_context* ctx);
void pauseSetNewLevel(uint8_t* rdram, recomp_context* ctx);
void mmFree(uint8_t* rdram, recomp_context* ctx);
void func_overlay_9_00002880_1F03658(uint8_t* rdram, recomp_context* ctx); // select a stock map point
void func_overlay_9_00002978_1F03750(uint8_t* rdram, recomp_context* ctx); // scroll a disc's textures
void func_overlay_9_00002FB0_1F03D88(uint8_t* rdram, recomp_context* ctx); // fill a rectangle (route lines)
void func_overlay_9_000030E8_1F03EC0(uint8_t* rdram, recomp_context* ctx); // draw a disc
void func_overlay_9_00003D0C_1F04AE4(uint8_t* rdram, recomp_context* ctx); // build the next disc from an icon
}

namespace {

using jfg::call_game;
using jfg::rd16;
using jfg::rd32;
using jfg::rd8;
using jfg::rdf;
using jfg::wr16;
using jfg::wr32;
using jfg::wr8;
using jfg::wrf;

constexpr char CONTENT_FILE[] = "jfg_world.json";
constexpr int MAP_SECTION = 9;

// Overlay 9's data (offsets in its section).
constexpr uint32_t O9_ICONS = 0x448C;      // MapWorldIcon[11], 0x30 each
constexpr uint32_t O9_POINTS = 0x469C;     // MapPoint[] (12 bytes; +3 unlocked), ended by 0xFF
constexpr uint32_t O9_ROUTES = 0x47DC;     // MapRoute[] (0x14: from, to, dirTo, dirBack), ended by from 0xFF
constexpr uint32_t O9_OPEN = 0x4908;       // s32: the map is set up
constexpr uint32_t O9_CURRENT = 0x490C;    // s32: the current point
constexpr uint32_t O9_HERO = 0x4914;       // s32: the selected hero (0 Vela, 1 Juno, 2 Lupus)
constexpr uint32_t O9_DISCS = 0x4990;      // MapLevel[11], 0x4E0 each
constexpr uint32_t O9_SCROLL_X = 0x9790, O9_SCROLL_Y = 0x9794;  // f32: the view
constexpr uint32_t O9_ORIGIN_X = 0x9798, O9_ORIGIN_Y = 0x979C;  // f32: eased towards the target
constexpr uint32_t O9_TARGET_X = 0x97A0, O9_TARGET_Y = 0x97A4;  // f32: the selected point
constexpr uint32_t O9_TIMER = 0x97A8;      // s32: move timer (15 after a move)
constexpr uint32_t O9_BUILT = 0x97B4;      // s32: discs built so far (the builder's slot)
constexpr uint32_t O9_MODE = 0x97C0;       // s32: 1 = the pause map
constexpr uint32_t O9_PLAYER = 0x97C4;     // s32: the controlling player
constexpr uint32_t O9_CLOSING = 0x97C8;    // s32
constexpr uint32_t ICON_SIZE = 0x30, DISC_SIZE = 0x4E0, POINT_SIZE = 12, ROUTE_SIZE = 0x14;

// The front end's globals.
constexpr uint32_t ADDR_frontgfx = 0x800FF398;   // Gfx*
constexpr uint32_t ADDR_frontmtx = 0x800FF39C;   // Mtx*
constexpr uint32_t ADDR_frontJoyDx = 0x800FF3D8; // s8[4]
constexpr uint32_t ADDR_frontJoyDy = 0x800FF3DC; // s8[4]
constexpr uint32_t ADDR_allLevelCheat = 0x800A5068;
constexpr uint32_t ADDR_racingCharacter = 0x800A4FC0; // u8

constexpr uint16_t SND_MOVE = 0x16B, SND_CHOOSE = 0x16D, SND_NO_ROUTE = 0x16E;
constexpr uint16_t BUTTON_A = 0x8000;
constexpr uint32_t BLOCK_COLOUR = 0x7F7F7FFF;

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

struct Route {
    int from = 0, dir = 0, back = 0;
    std::vector<std::pair<int, int>> corners;
};

struct Planet {
    std::string mod_id, id, name;
    std::string levels[3];
    int looks = 0, x = 0, y = 0, radius = 12;
    bool has_colour = false;
    uint32_t colour = 0;
    std::vector<Route> routes;
    // While the map is open:
    uint32_t icon = 0, disc = 0, text = 0; // heap blocks (RDRAM)
};
std::vector<Planet> g_planets;

// While the map is open.
bool g_built = false;
int g_current = -1;   // the selected planet, -1 = a stock point is selected
int g_route = 0;      // the route it was reached by
int g_from_point = 0; // that route's stock point
int g_rate = 1;       // this frame's update rate

uint32_t o9(uint32_t offset) { return (uint32_t)section_addresses[MAP_SECTION] + offset; }

int dir_index(const nlohmann::json& v) {
    if (v.is_number_integer()) return v.get<int>();
    static const char* names[] = { "left", "right", "up", "down" };
    for (int i = 0; i < 4; i++) {
        if (v.is_string() && v.get<std::string>() == names[i]) return i;
    }
    return -1;
}

void on_world_mod_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const std::string id = mod.manifest.mod_id;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!j.is_object()) { log("worldmap: " + id + ": " + CONTENT_FILE + " isn't valid JSON - its planets are off"); return; }
    for (const auto& p : j.value("planets", nlohmann::json::array())) {
        Planet planet;
        planet.mod_id = id;
        planet.id = p.value("id", std::string("planet"));
        planet.name = p.value("name", planet.id);
        const auto& levels = p.value("levels", nlohmann::json::array());
        for (int h = 0; h < 3; h++) planet.levels[h] = h < (int)levels.size() && levels[h].is_string() ? levels[h].get<std::string>() : "";
        const auto& disc = p.value("disc", nlohmann::json::object());
        planet.looks = disc.value("looks", 0);
        planet.x = disc.value("x", 0);
        planet.y = disc.value("y", 0);
        planet.radius = disc.value("radius", 12);
        if (disc.contains("colour") && disc["colour"].is_array() && disc["colour"].size() == 3) {
            planet.has_colour = true;
            planet.colour = ((uint32_t)disc["colour"][0].get<int>() << 24) | ((uint32_t)disc["colour"][1].get<int>() << 16)
                          | ((uint32_t)disc["colour"][2].get<int>() << 8) | 0xFF;
        }
        for (const auto& r : p.value("routes", nlohmann::json::array())) {
            Route route;
            route.from = r.value("from", -1);
            route.dir = dir_index(r.value("dir", nlohmann::json()));
            route.back = dir_index(r.value("back", nlohmann::json()));
            for (const auto& c : r.value("corners", nlohmann::json::array())) {
                if (c.is_array() && c.size() == 2) route.corners.emplace_back(c[0].get<int>(), c[1].get<int>());
            }
            if (route.from < 0 || route.dir < 0 || route.back < 0 || route.corners.size() < 2) {
                log("worldmap: " + id + ": planet " + planet.id + " has a route without from / dir / back / two corners - left out");
                continue;
            }
            planet.routes.push_back(route);
        }
        if (planet.looks < 0 || planet.looks > 10 || planet.routes.empty()) {
            log("worldmap: " + id + ": planet " + planet.id + " needs a disc (looks 0-10) and a route from a stock point - left out");
            continue;
        }
        log("worldmap: " + id + ": planet " + planet.id + " (" + planet.name + ")");
        g_planets.push_back(std::move(planet));
    }
}

int level_number(const Planet& p, int hero) {
    const std::string& ref = p.levels[hero >= 0 && hero < 3 ? hero : 1];
    if (ref.rfind("stock:", 0) == 0) return std::atoi(ref.c_str() + 6);
    return ref.empty() ? -1 : jfg::asset_number(p.mod_id + ":" + ref);
}

bool point_unlocked(int point) {
    for (int i = 0; i <= point; i++) {
        if (rd8(o9(O9_POINTS) + POINT_SIZE * i) == 0xFF) return false; // past the end
    }
    return rd8(o9(O9_POINTS) + POINT_SIZE * point + 3) != 0;
}

bool planet_shown(const Planet& p) {
    if (rd32(ADDR_allLevelCheat) != 0) return true;
    for (const Route& r : p.routes) {
        if (point_unlocked(r.from)) return true;
    }
    return false;
}

// A stock route already leaves `point` in direction `dir` (so a mod's route that way would never be taken).
bool stock_route_taken(int point, int dir) {
    for (uint32_t r = o9(O9_ROUTES); rd8(r) != 0xFF; r += ROUTE_SIZE) {
        if ((rd8(r) == point && rd8(r + 2) == dir) || (rd8(r + 1) == point && rd8(r + 3) == dir)) return true;
    }
    return false;
}

void sound(recomp_context* ctx, uint16_t id) { call_game(amSndPlay, ctx, { id, 0u }); }

void gfx(uint32_t w0, uint32_t w1) {
    const uint32_t g = rd32(ADDR_frontgfx);
    wr32(g, w0);
    wr32(g + 4, w1);
    wr32(ADDR_frontgfx, g + 8);
}

// The discs, built once per opening of the map by the game's own builder in disc slot 0 (saved and put back).
void build(recomp_context* ctx) {
    g_built = true;
    g_current = -1;
    std::vector<uint8_t> slot0(DISC_SIZE);
    const uint32_t slot = o9(O9_DISCS);
    for (uint32_t i = 0; i < DISC_SIZE; i++) slot0[i] = rd8(slot + i);
    const uint32_t built = rd32(o9(O9_BUILT));
    for (Planet& p : g_planets) {
        p.icon = call_game(mmAlloc, ctx, { ICON_SIZE, BLOCK_COLOUR });
        p.disc = call_game(mmAlloc, ctx, { DISC_SIZE, BLOCK_COLOUR });
        p.text = call_game(mmAlloc, ctx, { 0x40, BLOCK_COLOUR });
        if (!jfg::valid_ptr(p.icon) || !jfg::valid_ptr(p.disc) || !jfg::valid_ptr(p.text)) {
            log("worldmap: no heap for planet " + p.id);
            p.disc = 0;
            continue;
        }
        const uint32_t like = o9(O9_ICONS) + ICON_SIZE * (uint32_t)p.looks;
        for (uint32_t i = 0; i < ICON_SIZE; i++) wr8(p.icon + i, rd8(like + i));
        wr8(p.icon + 0x04, (uint8_t)p.radius);
        wr16(p.icon + 0x06, (uint16_t)(int16_t)p.x);
        wr16(p.icon + 0x08, (uint16_t)(int16_t)p.y);
        if (p.has_colour) wr32(p.icon + 0x14, p.colour);
        wr8(p.icon + 0x18, 0xFF); // a planet, not a moon
        wr32(o9(O9_BUILT), 0);
        call_game(func_overlay_9_00003D0C_1F04AE4, ctx, { p.icon });
        for (uint32_t i = 0; i < DISC_SIZE; i++) wr8(p.disc + i, rd8(slot + i));
        wr32(p.disc + 0x4C8, p.icon);
        const std::string name = p.name.substr(0, 0x3F);
        for (size_t i = 0; i <= name.size(); i++) wr8(p.text + (uint32_t)i, i < name.size() ? (uint8_t)name[i] : 0);
    }
    for (uint32_t i = 0; i < DISC_SIZE; i++) wr8(slot + i, slot0[i]);
    wr32(o9(O9_BUILT), built);

    // The hero is in one of the planets' levels: the map opens on it.
    const int hero = (int)rd32(o9(O9_HERO));
    const uint32_t ch = jfg::character_ptr(hero);
    if (ch != 0) {
        const int level = jfg::Character::Resume::get(ch) >> 5;
        for (int k = 0; k < (int)g_planets.size(); k++) {
            if (g_planets[k].disc && level_number(g_planets[k], hero) == level) {
                g_current = k;
                g_route = 0;
                g_from_point = g_planets[k].routes[0].from;
                wrf(o9(O9_TARGET_X), (float)g_planets[k].x);
                wrf(o9(O9_TARGET_Y), (float)g_planets[k].y);
                wrf(o9(O9_ORIGIN_X), (float)g_planets[k].x);
                wrf(o9(O9_ORIGIN_Y), (float)g_planets[k].y);
                wrf(o9(O9_SCROLL_X), (float)g_planets[k].x);
                wrf(o9(O9_SCROLL_Y), (float)g_planets[k].y - 120.0f);
                wr32(o9(O9_CURRENT), (uint32_t)g_from_point);
                log("worldmap: opens on " + g_planets[k].id);
            }
        }
    }
    for (const Planet& p : g_planets) {
        for (const Route& r : p.routes) {
            if (stock_route_taken(r.from, r.dir))
                log("worldmap: planet " + p.id + ": map point " + std::to_string(r.from) + " already has a route that way");
        }
    }
}

void free_blocks(recomp_context* ctx) {
    for (Planet& p : g_planets) {
        for (uint32_t* b : { &p.icon, &p.disc, &p.text }) {
            if (*b) call_game(mmFree, ctx, { *b });
            *b = 0;
        }
    }
    g_built = false;
    g_current = -1;
}

void choose(recomp_context* ctx, const Planet& p) {
    int hero = (int)rd32(o9(O9_HERO));
    const int level = level_number(p, hero);
    if (level < 0) {
        log("worldmap: planet " + p.id + " has no level for this hero");
        sound(ctx, SND_NO_ROUTE);
        return;
    }
    sound(ctx, SND_CHOOSE);
    const uint32_t ch = jfg::character_ptr(hero);
    if (ch != 0) wr16(ch + 0x04, rd16(ch + 0x04) & ~0x1Fu); // a new level: no saved state for it
    log("worldmap: " + p.id + " chosen: level " + std::to_string(level));
    if (rd32(o9(O9_MODE)) == 1) {
        if ((hero & 3) == 3) hero = rd8(ADDR_racingCharacter);
        call_game(pauseSetNewLevel, ctx, { (uint32_t)level, (uint32_t)hero, 0u, 0x10u });
    } else {
        call_game(mainChangeLevel, ctx, { (uint32_t)level, (uint32_t)hero, 0u, 0x10u, 1u, 0u });
    }
}

// Before the game's frame: the stick and A for the mods' planets.
void input(recomp_context* ctx) {
    const uint32_t player = rd32(o9(O9_PLAYER)) & 3u;
    const int dx = (int8_t)rd8(ADDR_frontJoyDx + player), dy = (int8_t)rd8(ADDR_frontJoyDy + player);
    int dir = -1;
    if (dx < -32) dir = 0;
    else if (dx > 32) dir = 1;
    else if (dy < -32) dir = 3;
    else if (dy > 32) dir = 2;
    const bool idle = rd32(o9(O9_TIMER)) == 0 && rd32(o9(O9_CLOSING)) == 0;
    auto hold_stick = [&]() { wr8(ADDR_frontJoyDx + player, 0); wr8(ADDR_frontJoyDy + player, 0); };

    if (g_current < 0) {
        if (!idle || dir < 0) return;
        const int point = (int)rd32(o9(O9_CURRENT));
        for (int k = 0; k < (int)g_planets.size(); k++) {
            const Planet& p = g_planets[k];
            if (!p.disc || !planet_shown(p)) continue;
            for (int r = 0; r < (int)p.routes.size(); r++) {
                if (p.routes[r].from != point || p.routes[r].dir != dir || !point_unlocked(point)) continue;
                g_current = k;
                g_route = r;
                g_from_point = point;
                sound(ctx, SND_MOVE);
                wrf(o9(O9_TARGET_X), (float)p.x);
                wrf(o9(O9_TARGET_Y), (float)p.y);
                wr32(o9(O9_TIMER), 15);
                hold_stick();
                log("worldmap: on " + p.id);
                return;
            }
        }
        return;
    }

    const Planet& p = g_planets[g_current];
    hold_stick();
    wrf(o9(O9_TARGET_X), (float)p.x);
    wrf(o9(O9_TARGET_Y), (float)p.y);
    if (idle && dir >= 0) {
        if (dir == p.routes[g_route].back) {
            g_current = -1;
            sound(ctx, SND_MOVE);
            call_game(func_overlay_9_00002880_1F03658, ctx, { (uint32_t)g_from_point });
            log("worldmap: back to map point " + std::to_string(g_from_point));
            return;
        }
        sound(ctx, SND_NO_ROUTE);
    }
    const uint32_t pressed = jfg::ADDR_buttonsPressed + 2u * (rd8(jfg::ADDR_sPlayerID + player) & 3u);
    if ((rd16(pressed) & BUTTON_A) && rd32(o9(O9_CLOSING)) == 0) {
        wr16(pressed, rd16(pressed) & ~BUTTON_A); // the game's map must not also choose its own point
        choose(ctx, p);
    }
}

// After the game's frame: the mods' routes, discs and names.
void draw(recomp_context* ctx, int rate) {
    const float sx = rdf(o9(O9_SCROLL_X)), sy = rdf(o9(O9_SCROLL_Y));
    call_game(texDPInit, ctx, { ADDR_frontgfx });
    gfx(0xE7000000, 0);           // pipe sync
    gfx(0xBA001402, 0);           // fill rectangles, as the map's own routes
    gfx(0xFCFFFFFF, 0xFFFDF6FB);
    gfx(0xFA000000, 0x00FF0080);  // the routes' green
    for (const Planet& p : g_planets) {
        if (!p.disc || !planet_shown(p)) continue;
        for (const Route& r : p.routes) {
            if (!point_unlocked(r.from) && rd32(ADDR_allLevelCheat) == 0) continue;
            for (size_t i = 0; i + 1 < r.corners.size(); i++) {
                const int x1 = r.corners[i].first - (int)sx + 160, y1 = r.corners[i].second - (int)sy;
                const int x2 = r.corners[i + 1].first - (int)sx + 160, y2 = r.corners[i + 1].second - (int)sy;
                call_game(func_overlay_9_00002FB0_1F03D88, ctx, { (uint32_t)(int16_t)x1, (uint32_t)(int16_t)y1, (uint32_t)(int16_t)x2, (uint32_t)(int16_t)y2 });
            }
        }
    }
    call_game(texDPInit, ctx, { ADDR_frontgfx });
    for (const Planet& p : g_planets) {
        if (!p.disc || !planet_shown(p)) continue;
        call_game(func_overlay_9_00002978_1F03750, ctx, { p.disc, (uint32_t)rate });
        call_game(camStandardOrtho, ctx, { ADDR_frontgfx, ADDR_frontmtx });
        call_game(func_overlay_9_000030E8_1F03EC0, ctx, { p.disc });
    }
    for (int k = 0; k < (int)g_planets.size(); k++) {
        const Planet& p = g_planets[k];
        if (!p.disc || !planet_shown(p)) continue;
        const int x = p.x - (int)sx + 160, y = p.y - (int)sy;
        call_game(fontUseFont, ctx, { 2u });
        if (k == g_current) call_game(fontColour, ctx, { 0xFFu, 0xFFu, 0u, 0xFFu, 0xFFu });
        else call_game(fontColour, ctx, { 0u, 0xFFu, 0u, 0xFFu, 0xFFu });
        call_game(fontPrintXY, ctx, { ADDR_frontgfx, (uint32_t)(x + 30), (uint32_t)(y - 15), p.text, 0u });
    }
}

} // namespace

JFG_WRAP(frontMap) {
    if (g_planets.empty() || rd32(o9(O9_OPEN)) == 0) { frontMap_orig(rdram, ctx); return; }
    if (!g_built) build(ctx);
    const int before = (int)rd32(o9(O9_CURRENT));
    g_rate = (int)ctx->r4;
    input(ctx);
    frontMap_orig(rdram, ctx);
    if (rd32(o9(O9_OPEN)) == 0) return;
    if (g_current >= 0) {
        // The hero wheel moved the view to the new hero's world: back on the stock map.
        const Planet& p = g_planets[g_current];
        if ((int)rd32(o9(O9_CURRENT)) != before || rdf(o9(O9_TARGET_X)) != (float)p.x || rdf(o9(O9_TARGET_Y)) != (float)p.y)
            g_current = -1;
    }
}

// The map's last drawing step before its frame (the route rectangles): the mods' planets go in under the frame.
JFG_WRAP(func_overlay_9_000039F0_1F047C8) {
    if (g_built && rd32(o9(O9_OPEN)) != 0) draw(ctx, g_rate);
    func_overlay_9_000039F0_1F047C8_orig(rdram, ctx);
}

JFG_WRAP(frontCleanupMap) {
    if (g_built) free_blocks(ctx);
    frontCleanupMap_orig(rdram, ctx);
}

extern "C" void jfg_register_world_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_world_mod_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
}
