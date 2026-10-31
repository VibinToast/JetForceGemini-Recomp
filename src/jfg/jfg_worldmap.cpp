// Jet Force Gemini port: mods' planets and galaxies on the planet-select map (the planet registry, MODDING_API_PLAN §4;
// JFG-Modding Docs/GALAXIES_PLAN.md).
//
// A mod lists them in jfg_world.json (format 1 = planets on the game's galaxy only):
//   { "format": 2,
//     "galaxies": [ { "id": "expanse", "name": "Zephyra Expanse", "home": "zephyra" } ],
//     "planets": [ {
//       "id": "zephyra", "name": "Zephyra",
//       "galaxy": "main" | "<a galaxy of this mod>" | "<mod id>:<galaxy>",
//       "levels": ["level_x", "level_x", "level_x"],      entry level for Vela, Juno, Lupus: a level the mod appends
//                                                         ("level_x", numbered at load) or "stock:N"
//       "disc": { "looks": 2, "x": 160, "y": 25, "radius": 14, "colour": [r, g, b] },
//                                                         looks: the stock disc whose textures it wears (0-10)
//       "unlock": "start" | "stock" | "script" | { "entered": "<planet>" } | { "completed": "<planet>" },
//       "routes": [ { "from": 0 | "planet:<planet>", "dir": 2, "back": 3, "corners": [[x, y], ...] } ] } ] }
//   from: a stock map point (the game's galaxy) or a planet of the same galaxy; dir: the stick direction from there
//   (0 left, 1 right, 2 up, 3 down, or the names), back: the way back. A planet named without "mod:" is the mod's own.
//   unlock: "stock" (the game's galaxy's default) = shown once the stock point a route leaves from is unlocked;
//   "start" (a mod galaxy's default) = always; "entered" / "completed" = once that planet was entered / completed;
//   "script" = once a script or mod unlocks it (jfg_world_unlock, jfg.unlock_planet). A planet is completed by a script
//   or mod (jfg_world_complete, jfg.complete_planet): the pack decides what finishing a world means.
//
// The map's tables live in overlay 9 and are full (the decomp's MOD_SUPPORT_REQUESTS, "the planet-select map's code"),
// so the game's own map code keeps running unchanged and the port adds the mods' planets around it (frontMap's
// wrapper):
// - before the game's frame: L / R switch galaxy; a stick move along a mod's route selects a planet; while one is
//   selected the port owns the stick and the A press (the game sees neither), follows the routes, and starts the
//   planet's level the way the map does (mainChangeLevel, or pauseSetNewLevel on the pause map);
// - in a mod's galaxy, the game's galaxy is hidden: the map shows only what the hero has unlocked, so for the length of
//   its frame the heroes' world masks and the all-levels cheat read as nothing unlocked (then they are put back);
// - during the frame, just before the map's frame and hero wheel are drawn (the wrapper of the step that draws the
//   route rectangles): the routes, discs (built by the game's own disc builder, drawn by its own disc drawer), names
//   and the galaxy line.
// What each save file has unlocked, entered and completed is kept in saves/<save name>_worlds.json, written when the
// game saves to that file (as Permanent Tribal Saves does). Map coordinates: x right, y down, the view centred on the
// selected point (scroll x, scroll y + 120).
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "ultramodern/ultramodern.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" int32_t* section_addresses; // librecomp: each code section's address this run (overlay 9 = section 9)

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
bool appended_entry(int data_asset, int entry, std::string* owner); // jfg_assets.cpp
}
extern "C" {
void amSndPlay(uint8_t* rdram, recomp_context* ctx);
void camStandardOrtho(uint8_t* rdram, recomp_context* ctx);
void texDPInit(uint8_t* rdram, recomp_context* ctx);
void fontUseFont(uint8_t* rdram, recomp_context* ctx);
void fontColour(uint8_t* rdram, recomp_context* ctx);
void fontPrintXY(uint8_t* rdram, recomp_context* ctx);
void fontStringWidth(uint8_t* rdram, recomp_context* ctx);
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
constexpr uint32_t CHARACTER_WORLDS = 0x64;          // MultiCharacter +0x64: u16, the worlds open to the hero (bit = world)

constexpr uint16_t SND_MOVE = 0x16B, SND_CHOOSE = 0x16D, SND_NO_ROUTE = 0x16E;
constexpr uint16_t BUTTON_A = 0x8000, BUTTON_L = 0x0020, BUTTON_R = 0x0010, BUTTON_Z = 0x2000;
constexpr uint16_t DPAD_LEFT = 0x0200, DPAD_RIGHT = 0x0100;
// Galaxy switch: R (RB, the right trigger, the R key) or D-pad right for the next galaxy; L (LB, the E key), Z (the left
// trigger) or D-pad left for the previous one. The map reads none of these (decomp MSR mod #6, "Buttons on the planet
// map opened from the pause menu").
constexpr uint16_t SWITCH_NEXT = BUTTON_R | DPAD_RIGHT, SWITCH_PREVIOUS = BUTTON_L | BUTTON_Z | DPAD_LEFT;
constexpr uint32_t BLOCK_COLOUR = 0x7F7F7FFF;
constexpr int SAVE_SLOTS = 6;
constexpr int MAIN = 0; // the game's galaxy

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

enum class Unlock { Stock, Start, Script, Entered, Completed };

struct Route {
    int from_stock = -1;       // a stock map point
    std::string from_ref;      // or a planet ("mod:id"), resolved to from_planet
    int from_planet = -1;
    int dir = 0, back = 0;
    std::vector<std::pair<int, int>> corners;
};

struct Galaxy {
    std::string key, name, home_ref; // key "mod:id" ("main": the game's)
    int home = -1;
};

struct Planet {
    std::string mod_id, key, name, galaxy_ref;
    int galaxy = MAIN;
    std::string levels[3];
    int looks = 0, x = 0, y = 0, radius = 12;
    bool has_colour = false;
    uint32_t colour = 0;
    Unlock unlock = Unlock::Stock;
    bool unlock_given = false;
    std::string unlock_ref; // Entered / Completed: that planet
    int unlock_planet = -1;
    std::vector<Route> routes;
    // While the map is open:
    uint32_t icon = 0, disc = 0, text = 0; // heap blocks (RDRAM)
};
std::vector<Galaxy> g_galaxies{ Galaxy{ "main", "Jet Force Gemini", "", -1 } };
std::vector<Planet> g_planets;
bool g_resolved = false;

// What a save file has: by the planets' and galaxies' keys, so a mod's update doesn't shift them.
struct SaveState {
    std::set<std::string> unlocked, entered, completed;
    std::string galaxy; // the galaxy the map was last on
};
std::map<int, SaveState> g_disk, g_memory; // by save slot; -1 = no file (a port warp from the title)
bool g_disk_loaded = false;

// While the map is open.
bool g_built = false;
int g_galaxy = MAIN;
int g_current = -1;    // the selected planet, -1 = a stock point (the game's galaxy)
int g_stock_point = 0; // the stock point the game's galaxy was left at
std::map<int, int> g_last; // galaxy -> its last selected planet
int g_rate = 1;        // this frame's update rate
int g_show = -1;       // jfg_galaxy_show: the galaxy the map opens on next
uint32_t g_cheat = 0;  // the all-levels cheat as it was before a hidden frame
std::atomic<bool> g_switching{ false }; // the map is up with more than one galaxy to go to (read by the input thread)

uint32_t o9(uint32_t offset) { return (uint32_t)section_addresses[MAP_SECTION] + offset; }

int dir_index(const nlohmann::json& v) {
    if (v.is_number_integer()) return std::clamp(v.get<int>(), -1, 3);
    static const char* names[] = { "left", "right", "up", "down" };
    for (int i = 0; i < 4; i++) {
        if (v.is_string() && v.get<std::string>() == names[i]) return i;
    }
    return -1;
}

std::string full_key(const std::string& mod_id, const std::string& ref) {
    return ref.find(':') == std::string::npos ? mod_id + ":" + ref : ref;
}

// --- loading ---------------------------------------------------------------------------------------------------------

void on_world_mod_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const std::string id = mod.manifest.mod_id;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    if (!j.is_object()) { log("worldmap: " + id + ": " + CONTENT_FILE + " isn't valid JSON - its planets are off"); return; }
    for (const auto& gx : j.value("galaxies", nlohmann::json::array())) {
        Galaxy g;
        g.key = full_key(id, gx.value("id", std::string("galaxy")));
        g.name = gx.value("name", g.key);
        g.home_ref = gx.contains("home") ? full_key(id, gx.value("home", std::string())) : "";
        log("worldmap: " + id + ": galaxy " + g.key + " (" + g.name + ")");
        g_galaxies.push_back(g);
    }
    for (const auto& p : j.value("planets", nlohmann::json::array())) {
        Planet planet;
        planet.mod_id = id;
        planet.key = full_key(id, p.value("id", std::string("planet")));
        planet.name = p.value("name", planet.key);
        const std::string galaxy = p.value("galaxy", std::string("main"));
        planet.galaxy_ref = galaxy == "main" ? "main" : full_key(id, galaxy);
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
        if (p.contains("unlock")) {
            const auto& u = p["unlock"];
            planet.unlock_given = true;
            if (u.is_string() && u.get<std::string>() == "start") planet.unlock = Unlock::Start;
            else if (u.is_string() && u.get<std::string>() == "stock") planet.unlock = Unlock::Stock;
            else if (u.is_string() && u.get<std::string>() == "script") planet.unlock = Unlock::Script;
            else if (u.is_object() && u.contains("entered")) { planet.unlock = Unlock::Entered; planet.unlock_ref = full_key(id, u.value("entered", std::string())); }
            else if (u.is_object() && u.contains("completed")) { planet.unlock = Unlock::Completed; planet.unlock_ref = full_key(id, u.value("completed", std::string())); }
            else { log("worldmap: " + id + ": planet " + planet.key + ": unknown unlock - it opens by script only"); planet.unlock = Unlock::Script; }
        }
        for (const auto& r : p.value("routes", nlohmann::json::array())) {
            Route route;
            const auto& from = r.value("from", nlohmann::json());
            if (from.is_number_integer()) route.from_stock = from.get<int>();
            else if (from.is_string() && from.get<std::string>().rfind("planet:", 0) == 0) route.from_ref = full_key(id, from.get<std::string>().substr(7));
            route.dir = dir_index(r.value("dir", nlohmann::json()));
            route.back = dir_index(r.value("back", nlohmann::json()));
            for (const auto& c : r.value("corners", nlohmann::json::array())) {
                if (c.is_array() && c.size() == 2) route.corners.emplace_back(c[0].get<int>(), c[1].get<int>());
            }
            if ((route.from_stock < 0 && route.from_ref.empty()) || route.dir < 0 || route.back < 0 || route.corners.size() < 2) {
                log("worldmap: " + id + ": planet " + planet.key + " has a route without from / dir / back / two corners - left out");
                continue;
            }
            planet.routes.push_back(route);
        }
        if (planet.looks < 0 || planet.looks > 10) {
            log("worldmap: " + id + ": planet " + planet.key + ": disc looks must be 0-10 - left out");
            continue;
        }
        log("worldmap: " + id + ": planet " + planet.key + " (" + planet.name + ")");
        g_planets.push_back(std::move(planet));
    }
}

int planet_index(const std::string& key) {
    for (int k = 0; k < (int)g_planets.size(); k++) {
        if (g_planets[k].key == key) return k;
    }
    return -1;
}

int galaxy_index(const std::string& key) {
    for (int k = 0; k < (int)g_galaxies.size(); k++) {
        if (g_galaxies[k].key == key) return k;
    }
    return -1;
}

// Names to indices, once every mod is loaded. Planets in an unknown galaxy and routes from unknown planets drop out.
void resolve() {
    if (g_resolved) return;
    g_resolved = true;
    for (Planet& p : g_planets) {
        p.galaxy = galaxy_index(p.galaxy_ref);
        if (p.galaxy < 0) {
            log("worldmap: planet " + p.key + ": there is no galaxy " + p.galaxy_ref + " (is its mod on?) - left out");
            continue;
        }
        if (!p.unlock_given) p.unlock = p.galaxy == MAIN ? Unlock::Stock : Unlock::Start;
        if (p.unlock == Unlock::Entered || p.unlock == Unlock::Completed) {
            p.unlock_planet = planet_index(p.unlock_ref);
            if (p.unlock_planet < 0) log("worldmap: planet " + p.key + ": unlocks with " + p.unlock_ref + ", which isn't loaded");
        }
        for (auto it = p.routes.begin(); it != p.routes.end();) {
            if (!it->from_ref.empty()) {
                it->from_planet = planet_index(it->from_ref);
                if (it->from_planet < 0 || g_planets[it->from_planet].galaxy_ref != p.galaxy_ref) {
                    log("worldmap: planet " + p.key + ": a route from " + it->from_ref + ", not a planet of its galaxy - left out");
                    it = p.routes.erase(it);
                    continue;
                }
            } else if (p.galaxy != MAIN) {
                log("worldmap: planet " + p.key + ": a route from a stock map point, but it is not on the game's galaxy - left out");
                it = p.routes.erase(it);
                continue;
            }
            ++it;
        }
    }
    for (Galaxy& g : g_galaxies) {
        if (!g.home_ref.empty()) g.home = planet_index(g.home_ref);
    }
}

// --- save state --------------------------------------------------------------------------------------------------

std::filesystem::path state_path() {
    std::filesystem::path save = ultramodern::get_save_file_path();
    return save.parent_path() / (save.stem().u8string() + u8"_worlds.json");
}

void load_disk() {
    if (g_disk_loaded) return;
    g_disk_loaded = true;
    g_disk.clear();
    std::ifstream f(state_path());
    if (!f) return;
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (!j.is_object()) return;
    const nlohmann::json slots = j.value("slots", nlohmann::json::object()); // named: items() doesn't keep a temporary alive
    for (const auto& [slot, s] : slots.items()) {
        SaveState st;
        for (const auto& v : s.value("unlocked", nlohmann::json::array())) if (v.is_string()) st.unlocked.insert(v);
        for (const auto& v : s.value("entered", nlohmann::json::array())) if (v.is_string()) st.entered.insert(v);
        for (const auto& v : s.value("completed", nlohmann::json::array())) if (v.is_string()) st.completed.insert(v);
        st.galaxy = s.value("galaxy", std::string());
        g_disk[std::atoi(slot.c_str())] = st;
    }
}

void write_disk() {
    nlohmann::json slots = nlohmann::json::object();
    for (const auto& [slot, st] : g_disk) {
        if (slot < 0) continue;
        slots[std::to_string(slot)] = { { "unlocked", st.unlocked }, { "entered", st.entered },
                                        { "completed", st.completed }, { "galaxy", st.galaxy } };
    }
    const std::filesystem::path path = state_path();
    std::filesystem::path tmp = path;
    tmp += u8".tmp";
    {
        std::ofstream f(tmp, std::ios::trunc);
        if (!f) return;
        f << nlohmann::json{ { "format", 1 }, { "slots", slots } }.dump(1);
        if (!f) return;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
}

int current_slot() {
    if (!jfg::auto_save_file_in_play()) return -1;
    const uint32_t game = rd32(jfg::ADDR_gameplay);
    if (!jfg::valid_ptr(game)) return -1;
    const int slot = jfg::Game::SaveSlot::get(game);
    return (slot >= 0 && slot < SAVE_SLOTS) ? slot : -1;
}

SaveState& state() { return g_memory[current_slot()]; }

// --- rules -----------------------------------------------------------------------------------------------------------

bool point_unlocked(int point) {
    for (int i = 0; i <= point; i++) {
        if (rd8(o9(O9_POINTS) + POINT_SIZE * i) == 0xFF) return false; // past the end
    }
    return rd8(o9(O9_POINTS) + POINT_SIZE * point + 3) != 0;
}

bool cheat_on() { return (g_galaxy == MAIN ? rd32(ADDR_allLevelCheat) : g_cheat) != 0; }

// Whether a planet is open (shown and selectable). Stock-point rules read the map's own table, so they need the map.
bool open_planet(int k, int depth = 0) {
    const Planet& p = g_planets[k];
    if (p.galaxy < 0 || !p.disc) return false;
    if (cheat_on()) return true;
    const SaveState& st = state();
    if (st.unlocked.count(p.key)) return true;
    switch (p.unlock) {
    case Unlock::Start: return true;
    case Unlock::Script: return false;
    case Unlock::Entered: return p.unlock_planet >= 0 && st.entered.count(g_planets[p.unlock_planet].key) > 0;
    case Unlock::Completed: return p.unlock_planet >= 0 && st.completed.count(g_planets[p.unlock_planet].key) > 0;
    case Unlock::Stock:
        for (const Route& r : p.routes) {
            if (r.from_stock >= 0 && g_galaxy == MAIN && point_unlocked(r.from_stock)) return true;
            if (r.from_planet >= 0 && r.from_planet != k && depth < 8 && open_planet(r.from_planet, depth + 1)) return true;
        }
        return false;
    }
    return false;
}

bool galaxy_open(int g) {
    if (g == MAIN) return true;
    for (int k = 0; k < (int)g_planets.size(); k++) {
        if (g_planets[k].galaxy == g && open_planet(k)) return true;
    }
    return false;
}

int level_number(const Planet& p, int hero) {
    const std::string& ref = p.levels[hero >= 0 && hero < 3 ? hero : 1];
    if (ref.rfind("stock:", 0) == 0) return std::atoi(ref.c_str() + 6);
    return ref.empty() ? -1 : jfg::asset_number(p.mod_id + ":" + ref);
}

// The galaxy a level belongs to: the game's for its own levels, else the galaxy of the planets of the mod that added it
// (when they are all on one). -1: unknown (a mod with no planets, or planets on several galaxies).
constexpr int LEVEL_HEADERS = 0x1F; // the level headers' data asset
int galaxy_of_level(int level) {
    std::string owner;
    if (!jfg::appended_entry(LEVEL_HEADERS, level, &owner)) return MAIN;
    int g = -1;
    for (const Planet& p : g_planets) {
        if (p.mod_id != owner || p.galaxy < 0) continue;
        if (g >= 0 && g != p.galaxy) return -1;
        g = p.galaxy;
    }
    return g;
}

// A stock route already leaves `point` in direction `dir`.
bool stock_route_taken(int point, int dir) {
    for (uint32_t r = o9(O9_ROUTES); rd8(r) != 0xFF; r += ROUTE_SIZE) {
        if ((rd8(r) == point && rd8(r + 2) == dir) || (rd8(r + 1) == point && rd8(r + 3) == dir)) return true;
    }
    return false;
}

// Two routes leaving the same place the same way: the later one moves to a free direction there, or drops out. Done at
// each opening of the map (the stock routes are only readable then); the same mods give the same result.
void settle_routes() {
    std::set<std::tuple<int, int, int>> used; // (stock point or -1 - planet, galaxy, direction)
    auto node = [](const Route& r) { return r.from_stock >= 0 ? r.from_stock : -1 - r.from_planet; };
    for (int k = 0; k < (int)g_planets.size(); k++) { // the ways back from each planet are taken first
        for (const Route& r : g_planets[k].routes) used.insert({ -1 - k, g_planets[k].galaxy, r.back });
    }
    for (Planet& p : g_planets) {
        if (p.galaxy < 0) continue;
        for (auto it = p.routes.begin(); it != p.routes.end();) {
            auto taken = [&](int d) {
                return used.count({ node(*it), p.galaxy, d }) > 0 || (it->from_stock >= 0 && stock_route_taken(it->from_stock, d));
            };
            if (taken(it->dir)) {
                int free = -1;
                for (int d = 0; d < 4 && free < 0; d++) if (!taken(d)) free = d;
                const std::string from = it->from_stock >= 0 ? "map point " + std::to_string(it->from_stock) : it->from_ref;
                if (free < 0) {
                    log("worldmap: planet " + p.key + ": every direction from " + from + " is taken - that route is left out");
                    it = p.routes.erase(it);
                    continue;
                }
                log("worldmap: planet " + p.key + ": the route from " + from + " moves from direction " + std::to_string(it->dir) +
                    " to " + std::to_string(free) + " (another route already goes that way)");
                it->dir = free;
            }
            used.insert({ node(*it), p.galaxy, it->dir });
            ++it;
        }
    }
}

void sound(recomp_context* ctx, uint16_t id) { call_game(amSndPlay, ctx, { id, 0u }); }

void gfx(uint32_t w0, uint32_t w1) {
    const uint32_t g = rd32(ADDR_frontgfx);
    wr32(g, w0);
    wr32(g + 4, w1);
    wr32(ADDR_frontgfx, g + 8);
}

void select_planet(int k, bool instant) {
    const Planet& p = g_planets[k];
    g_current = k;
    g_last[p.galaxy] = k;
    wrf(o9(O9_TARGET_X), (float)p.x);
    wrf(o9(O9_TARGET_Y), (float)p.y);
    if (instant) {
        wrf(o9(O9_ORIGIN_X), (float)p.x);
        wrf(o9(O9_ORIGIN_Y), (float)p.y);
        wrf(o9(O9_SCROLL_X), (float)p.x);
        wrf(o9(O9_SCROLL_Y), (float)p.y - 120.0f);
    } else {
        wr32(o9(O9_TIMER), 15);
    }
}

// The planet a galaxy opens on: the last one used there, its home, or its first open planet.
int galaxy_entry(int g) {
    auto it = g_last.find(g);
    if (it != g_last.end() && open_planet(it->second)) return it->second;
    if (g_galaxies[g].home >= 0 && open_planet(g_galaxies[g].home)) return g_galaxies[g].home;
    for (int k = 0; k < (int)g_planets.size(); k++) {
        if (g_planets[k].galaxy == g && open_planet(k)) return k;
    }
    return -1;
}

// instant: cut to the galaxy's planet (a switch is a cut, not a flight across: the maps are separate).
void switch_galaxy(recomp_context* ctx, int g, bool instant) {
    if (g == g_galaxy) return;
    if (g_galaxy == MAIN) g_stock_point = (int)rd32(o9(O9_CURRENT));
    g_galaxy = g;
    state().galaxy = g_galaxies[g].key;
    if (g == MAIN) {
        g_current = -1;
        call_game(func_overlay_9_00002880_1F03658, ctx, { (uint32_t)g_stock_point });
        // Another map: cut to it rather than fly across (the point's position is the target the game just set).
        const float x = rdf(o9(O9_TARGET_X)), y = rdf(o9(O9_TARGET_Y));
        wrf(o9(O9_ORIGIN_X), x);
        wrf(o9(O9_ORIGIN_Y), y);
        wrf(o9(O9_SCROLL_X), x);
        wrf(o9(O9_SCROLL_Y), y - 120.0f);
    } else {
        const int k = galaxy_entry(g);
        if (k >= 0) select_planet(k, instant);
    }
    log("worldmap: galaxy " + g_galaxies[g].key);
    jfg::api::fire(jfg::g_rdram, ctx, jfg::api::Event::GalaxyChange, g);
}

// The discs, built once per opening of the map by the game's own builder in disc slot 0 (saved and put back).
void build(recomp_context* ctx) {
    resolve();
    g_built = true;
    g_current = -1;
    g_galaxy = MAIN;
    std::vector<uint8_t> slot0(DISC_SIZE);
    const uint32_t slot = o9(O9_DISCS);
    for (uint32_t i = 0; i < DISC_SIZE; i++) slot0[i] = rd8(slot + i);
    const uint32_t built = rd32(o9(O9_BUILT));
    for (Planet& p : g_planets) {
        if (p.galaxy < 0) continue;
        p.icon = call_game(mmAlloc, ctx, { ICON_SIZE, BLOCK_COLOUR });
        p.disc = call_game(mmAlloc, ctx, { DISC_SIZE, BLOCK_COLOUR });
        p.text = call_game(mmAlloc, ctx, { 0x40, BLOCK_COLOUR });
        if (!jfg::valid_ptr(p.icon) || !jfg::valid_ptr(p.disc) || !jfg::valid_ptr(p.text)) {
            log("worldmap: no heap for planet " + p.key);
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
    settle_routes();

    // Where the map opens: on the planet whose level the hero is in; else the galaxy asked for (jfg_galaxy_show); else
    // the galaxy of the level the hero is in (the game's for its own levels); else the one this file was last on.
    const int hero = (int)rd32(o9(O9_HERO));
    const uint32_t ch = jfg::character_ptr(hero);
    const int level = ch != 0 ? jfg::Character::Resume::get(ch) >> 5 : -1;
    int open_on = -1;
    for (int k = 0; k < (int)g_planets.size() && open_on < 0; k++) {
        if (g_planets[k].disc && level_number(g_planets[k], hero) == level && open_planet(k)) open_on = k;
    }
    if (open_on >= 0) {
        const Planet& p = g_planets[open_on];
        if (p.galaxy != MAIN) switch_galaxy(ctx, p.galaxy, true);
        if (p.galaxy == MAIN) {
            g_stock_point = (int)rd32(o9(O9_CURRENT));
            for (const Route& r : p.routes) if (r.from_stock >= 0) { g_stock_point = r.from_stock; break; }
            wr32(o9(O9_CURRENT), (uint32_t)g_stock_point);
        }
        select_planet(open_on, true);
        log("worldmap: opens on " + p.key);
    } else {
        int g = g_show >= 0 ? g_show : level >= 0 ? galaxy_of_level(level) : -1;
        if (g < 0) g = galaxy_index(state().galaxy);
        if (g > MAIN && g < (int)g_galaxies.size() && galaxy_open(g)) switch_galaxy(ctx, g, true);
    }
    g_show = -1;
    jfg::api::fire(jfg::g_rdram, ctx, jfg::api::Event::MapOpen, g_galaxy);
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
    g_galaxy = MAIN;
}

void choose(recomp_context* ctx, int k) {
    const Planet& p = g_planets[k];
    int hero = (int)rd32(o9(O9_HERO));
    const int level = level_number(p, hero);
    if (level < 0) {
        log("worldmap: planet " + p.key + " has no level for this hero");
        sound(ctx, SND_NO_ROUTE);
        return;
    }
    sound(ctx, SND_CHOOSE);
    jfg::api::fire(jfg::g_rdram, ctx, jfg::api::Event::PlanetSelect, k);
    const uint32_t ch = jfg::character_ptr(hero);
    if (ch != 0) wr16(ch + 0x04, rd16(ch + 0x04) & ~0x1Fu); // a new level: no saved state for it
    log("worldmap: " + p.key + " chosen: level " + std::to_string(level));
    if (rd32(o9(O9_MODE)) == 1) {
        if ((hero & 3) == 3) hero = rd8(ADDR_racingCharacter);
        call_game(pauseSetNewLevel, ctx, { (uint32_t)level, (uint32_t)hero, 0u, 0x10u });
    } else {
        call_game(mainChangeLevel, ctx, { (uint32_t)level, (uint32_t)hero, 0u, 0x10u, 1u, 0u });
    }
}

// The planet a stick direction leads to from planet k (its routes and the routes into it), -2 = back to the stock
// point it was reached from, -1 = none.
int neighbour(int k, int dir) {
    const Planet& p = g_planets[k];
    for (int q = 0; q < (int)g_planets.size(); q++) {
        if (g_planets[q].galaxy != p.galaxy || !open_planet(q)) continue;
        for (const Route& r : g_planets[q].routes) {
            if (r.from_planet == k && r.dir == dir) return q;
        }
    }
    for (const Route& r : p.routes) {
        if (r.back != dir) continue;
        if (r.from_planet >= 0 && open_planet(r.from_planet)) return r.from_planet;
        if (r.from_stock >= 0 && p.galaxy == MAIN && point_unlocked(r.from_stock)) {
            g_stock_point = r.from_stock;
            return -2;
        }
    }
    return -1;
}

// Before the game's frame: L / R, the stick and A for the mods' planets.
void input(recomp_context* ctx) {
    const uint32_t player = rd32(o9(O9_PLAYER)) & 3u;
    const int dx = (int8_t)rd8(ADDR_frontJoyDx + player), dy = (int8_t)rd8(ADDR_frontJoyDy + player);
    int dir = -1;
    if (dx < -32) dir = 0;
    else if (dx > 32) dir = 1;
    else if (dy < -32) dir = 3;
    else if (dy > 32) dir = 2;
    const bool closing = rd32(o9(O9_CLOSING)) != 0;
    const bool idle = rd32(o9(O9_TIMER)) == 0 && !closing;
    auto hold_stick = [&]() { wr8(ADDR_frontJoyDx + player, 0); wr8(ADDR_frontJoyDy + player, 0); };
    const uint32_t pressed_at = jfg::ADDR_buttonsPressed + 2u * (rd8(jfg::ADDR_sPlayerID + player) & 3u);
    const uint16_t pressed = rd16(pressed_at);
    static const bool debug = std::getenv("JFG_DEBUG_WORLDMAP") != nullptr; // the map's buttons, when they change
    if (debug) {
        static uint32_t last = 0;
        const uint32_t pad = jfg::ADDR_sControllerCurrData + jfg::Pad::Size * (rd8(jfg::ADDR_sPlayerID + player) & 3u);
        const uint32_t now = ((uint32_t)rd16(pad) << 16) | pressed;
        if (now != last) {
            last = now;
            log("worldmap debug: buttons " + std::to_string(now >> 16) + " pressed " + std::to_string(pressed));
        }
    }

    // L / R: the previous / next galaxy with an open planet.
    if ((pressed & (SWITCH_NEXT | SWITCH_PREVIOUS)) && !closing) {
        const int n = (int)g_galaxies.size(), step = (pressed & SWITCH_NEXT) ? 1 : n - 1;
        for (int g = (g_galaxy + step) % n; g != g_galaxy; g = (g + step) % n) {
            if (galaxy_open(g)) {
                sound(ctx, SND_MOVE);
                switch_galaxy(ctx, g, true);
                hold_stick();
                return;
            }
        }
    }

    if (g_current < 0) {
        if (g_galaxy != MAIN || !idle || dir < 0) return;
        const int point = (int)rd32(o9(O9_CURRENT));
        for (int k = 0; k < (int)g_planets.size(); k++) {
            const Planet& p = g_planets[k];
            if (p.galaxy != MAIN || !open_planet(k)) continue;
            for (const Route& r : p.routes) {
                if (r.from_stock != point || r.dir != dir || !point_unlocked(point)) continue;
                g_stock_point = point;
                sound(ctx, SND_MOVE);
                select_planet(k, false);
                hold_stick();
                log("worldmap: on " + p.key);
                return;
            }
        }
        return;
    }

    hold_stick();
    wrf(o9(O9_TARGET_X), (float)g_planets[g_current].x); // the view stays on the planet
    wrf(o9(O9_TARGET_Y), (float)g_planets[g_current].y);
    if (idle && dir >= 0) {
        const int next = neighbour(g_current, dir);
        if (next == -2) {
            g_current = -1;
            sound(ctx, SND_MOVE);
            call_game(func_overlay_9_00002880_1F03658, ctx, { (uint32_t)g_stock_point });
            log("worldmap: back to map point " + std::to_string(g_stock_point));
            return;
        }
        if (next >= 0) {
            sound(ctx, SND_MOVE);
            select_planet(next, false);
            log("worldmap: on " + g_planets[next].key);
        } else {
            sound(ctx, SND_NO_ROUTE);
        }
    }
    if ((pressed & BUTTON_A) && !closing && g_current >= 0) {
        wr16(pressed_at, rd16(pressed_at) & ~BUTTON_A); // the game's map must not also choose its own point
        choose(ctx, g_current);
    }
}

void print(recomp_context* ctx, int x, int y, uint32_t text) {
    call_game(fontPrintXY, ctx, { ADDR_frontgfx, (uint32_t)x, (uint32_t)y, text, 0u });
}

// After the game's map content, before its frame: the galaxy's routes, discs and names, and the galaxy line.
void draw(recomp_context* ctx, int rate) {
    const float sx = rdf(o9(O9_SCROLL_X)), sy = rdf(o9(O9_SCROLL_Y));
    auto line = [&](int ax, int ay, int bx, int by) {
        const int x1 = ax - (int)sx + 160, y1 = ay - (int)sy, x2 = bx - (int)sx + 160, y2 = by - (int)sy;
        call_game(func_overlay_9_00002FB0_1F03D88, ctx, { (uint32_t)(int16_t)x1, (uint32_t)(int16_t)y1, (uint32_t)(int16_t)x2, (uint32_t)(int16_t)y2 });
    };
    call_game(texDPInit, ctx, { ADDR_frontgfx });
    gfx(0xE7000000, 0);           // pipe sync
    gfx(0xBA001402, 0);           // fill rectangles, as the map's own routes
    gfx(0xFCFFFFFF, 0xFFFDF6FB);
    gfx(0xFA000000, 0x00FF0080);  // the routes' green
    for (int k = 0; k < (int)g_planets.size(); k++) {
        const Planet& p = g_planets[k];
        if (p.galaxy != g_galaxy || !open_planet(k)) continue;
        for (const Route& r : p.routes) {
            if (r.from_stock >= 0 && !point_unlocked(r.from_stock) && !cheat_on()) continue;
            if (r.from_planet >= 0 && !open_planet(r.from_planet)) continue;
            for (size_t i = 0; i + 1 < r.corners.size(); i++)
                line(r.corners[i].first, r.corners[i].second, r.corners[i + 1].first, r.corners[i + 1].second);
        }
    }
    call_game(texDPInit, ctx, { ADDR_frontgfx });
    for (int k = 0; k < (int)g_planets.size(); k++) {
        const Planet& p = g_planets[k];
        if (p.galaxy != g_galaxy || !open_planet(k)) continue;
        call_game(func_overlay_9_00002978_1F03750, ctx, { p.disc, (uint32_t)rate });
        call_game(camStandardOrtho, ctx, { ADDR_frontgfx, ADDR_frontmtx });
        call_game(func_overlay_9_000030E8_1F03EC0, ctx, { p.disc });
    }
    for (int k = 0; k < (int)g_planets.size(); k++) {
        const Planet& p = g_planets[k];
        if (p.galaxy != g_galaxy || !open_planet(k)) continue;
        const int x = p.x - (int)sx + 160, y = p.y - (int)sy;
        call_game(fontUseFont, ctx, { 2u });
        if (k == g_current) call_game(fontColour, ctx, { 0xFFu, 0xFFu, 0u, 0xFFu, 0xFFu });
        else call_game(fontColour, ctx, { 0u, 0xFFu, 0u, 0xFFu, 0xFFu });
        print(ctx, x + 30, y - 15, p.text);
    }

    // The galaxy line, when there is more than one galaxy to go to: the name at the top, with the L and R buttons in the
    // selected planet's yellow well clear of it on either side.
    int open_count = 0;
    for (int g = 0; g < (int)g_galaxies.size(); g++) open_count += galaxy_open(g) ? 1 : 0;
    g_switching.store(open_count > 1, std::memory_order_relaxed);
    if (open_count > 1) {
        static uint32_t text = 0;
        if (!text) text = call_game(mmAlloc, ctx, { 0x60, BLOCK_COLOUR }); // kept for the whole run
        if (jfg::valid_ptr(text)) {
            const uint32_t name = text, left = text + 0x48, right = text + 0x50;
            auto put = [](uint32_t at, const std::string& s) {
                for (size_t i = 0; i <= s.size(); i++) wr8(at + (uint32_t)i, i < s.size() ? (uint8_t)s[i] : 0);
            };
            put(name, g_galaxies[g_galaxy].name.substr(0, 0x40));
            put(left, "L");
            put(right, "R");
            // The game's font has no arrows, so the buttons are drawn: a frame around each letter and a small arrow
            // pointing out from it, filled like the routes.
            constexpr int GAP = 22, Y = 34, PAD = 3, TOP = Y - 3, BOTTOM = Y + 11, MID = (TOP + BOTTOM) / 2;
            call_game(fontUseFont, ctx, { 2u });
            const int half = (int)call_game(fontStringWidth, ctx, { name, 2u, 1u }) / 2;
            const int lw = (int)call_game(fontStringWidth, ctx, { left, 2u, 1u });
            const int rw = (int)call_game(fontStringWidth, ctx, { right, 2u, 1u });
            const int lx2 = 160 - half - GAP, lx1 = lx2 - lw - 2 * PAD; // L's frame
            const int rx1 = 160 + half + GAP, rx2 = rx1 + rw + 2 * PAD; // R's frame
            auto rect = [&](int x1, int y1, int x2, int y2) {
                call_game(func_overlay_9_00002FB0_1F03D88, ctx, { (uint32_t)(int16_t)x1, (uint32_t)(int16_t)y1,
                                                                 (uint32_t)(int16_t)x2, (uint32_t)(int16_t)y2 });
            };
            auto frame = [&](int x1, int x2) {
                rect(x1, TOP, x2, TOP);
                rect(x1, BOTTOM, x2, BOTTOM);
                rect(x1, TOP, x1, BOTTOM);
                rect(x2, TOP, x2, BOTTOM);
            };
            call_game(texDPInit, ctx, { ADDR_frontgfx });
            gfx(0xE7000000, 0);
            gfx(0xBA001402, 0);
            gfx(0xFCFFFFFF, 0xFFFDF6FB);
            gfx(0xFA000000, 0xFFFF00FF); // the selected planet's yellow
            frame(lx1, lx2);
            frame(rx1, rx2);
            for (int c = 0; c < 4; c++) { // columns of an arrow, the tallest by the frame
                rect(lx1 - 3 - c, MID - (3 - c), lx1 - 3 - c, MID + (3 - c));
                rect(rx2 + 3 + c, MID - (3 - c), rx2 + 3 + c, MID + (3 - c));
            }
            call_game(texDPInit, ctx, { ADDR_frontgfx });
            call_game(fontUseFont, ctx, { 2u });
            call_game(fontColour, ctx, { 0xFFu, 0xFFu, 0xFFu, 0xFFu, 0xFFu });
            print(ctx, 160 - half, Y, name);
            call_game(fontColour, ctx, { 0xFFu, 0xFFu, 0u, 0xFFu, 0xFFu });
            print(ctx, lx1 + PAD + 1, Y, left);
            print(ctx, rx1 + PAD + 1, Y, right);
        }
    }
}

// Lets the game's map show nothing of its own galaxy for one frame: every hero's world mask and the cheat read as
// nothing unlocked (they are put back right after).
struct HideStockGalaxy {
    bool on;
    uint16_t masks[3] = {};
    explicit HideStockGalaxy(bool on_) : on(on_) {
        if (!on) return;
        g_cheat = rd32(ADDR_allLevelCheat);
        wr32(ADDR_allLevelCheat, 0);
        for (int h = 0; h < 3; h++) {
            const uint32_t ch = jfg::character_ptr(h);
            masks[h] = ch ? rd16(ch + CHARACTER_WORLDS) : 0;
            if (ch) wr16(ch + CHARACTER_WORLDS, 0);
        }
    }
    ~HideStockGalaxy() {
        if (!on) return;
        wr32(ADDR_allLevelCheat, g_cheat);
        for (int h = 0; h < 3; h++) {
            const uint32_t ch = jfg::character_ptr(h);
            if (ch) wr16(ch + CHARACTER_WORLDS, masks[h]);
        }
    }
};

// --- API (mods' helpers, jfg_api.h; Lua, jfg_lua.cpp) -----------------------------------------------------------------

std::string read_string(uint32_t addr) {
    std::string s;
    for (int i = 0; i < 128 && jfg::valid_ptr(addr); i++) {
        const char c = (char)rd8(addr + (uint32_t)i);
        if (!c) break;
        s += c;
    }
    return s;
}

void api_world_find(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    resolve();
    ctx->r2 = (gpr)(int32_t)planet_index(read_string((uint32_t)ctx->r4));
}
void api_world_state(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    ctx->r2 = (gpr)(int32_t)jfg::world_state((int)(int32_t)ctx->r4);
}
void api_world_unlock(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    jfg::world_unlock((int)(int32_t)ctx->r4);
}
void api_world_complete(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    jfg::world_complete((int)(int32_t)ctx->r4);
}
void api_galaxy_find(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    resolve();
    const std::string key = read_string((uint32_t)ctx->r4);
    ctx->r2 = (gpr)(int32_t)galaxy_index(key);
}
void api_galaxy_current(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    ctx->r2 = (gpr)(int32_t)jfg::galaxy_current();
}
void api_galaxy_show(uint8_t* rdram, recomp_context* ctx) {
    jfg::g_rdram = rdram;
    jfg::galaxy_show((int)(int32_t)ctx->r4);
}

} // namespace

namespace jfg {

int world_find(const std::string& key) { resolve(); return planet_index(key); }
int galaxy_find(const std::string& key) { resolve(); return galaxy_index(key); }
std::string galaxy_key(int g) { return g >= 0 && g < (int)g_galaxies.size() ? g_galaxies[g].key : ""; }

// Bits: 1 open (as the map would show it now; stock-point rules count only while the map is open), 2 entered,
// 4 completed. -1: no such planet.
int world_state(int k) {
    resolve();
    if (k < 0 || k >= (int)g_planets.size()) return -1;
    const SaveState& st = state();
    const Planet& p = g_planets[k];
    int out = 0;
    bool open = st.unlocked.count(p.key) > 0;
    if (!open && p.galaxy >= 0) {
        switch (p.unlock) {
        case Unlock::Start: open = true; break;
        case Unlock::Entered: open = p.unlock_planet >= 0 && st.entered.count(g_planets[p.unlock_planet].key) > 0; break;
        case Unlock::Completed: open = p.unlock_planet >= 0 && st.completed.count(g_planets[p.unlock_planet].key) > 0; break;
        case Unlock::Stock: open = g_built && open_planet(k); break;
        case Unlock::Script: break;
        }
    }
    if (open) out |= 1;
    if (st.entered.count(p.key)) out |= 2;
    if (st.completed.count(p.key)) out |= 4;
    return out;
}

void world_unlock(int k) {
    resolve();
    if (k < 0 || k >= (int)g_planets.size()) return;
    if (state().unlocked.insert(g_planets[k].key).second) log("worldmap: " + g_planets[k].key + " unlocked");
}

void world_complete(int k) {
    resolve();
    if (k < 0 || k >= (int)g_planets.size()) return;
    if (state().completed.insert(g_planets[k].key).second) log("worldmap: " + g_planets[k].key + " completed");
}

int galaxy_current() { return g_built ? g_galaxy : -1; }

void galaxy_show(int g) {
    resolve();
    if (g < 0 || g >= (int)g_galaxies.size()) return;
    g_show = g;
    state().galaxy = g_galaxies[g].key;
}

// Game thread, each tick (jfg_tick.cpp): a planet whose level the hero is in counts as entered.
void worldmap_tick() {
    if (g_planets.empty() || rd32(ADDR_mainGameMode) != 0) return;
    static int last_level = -1;
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level == last_level) return;
    last_level = level;
    resolve();
    for (const Planet& p : g_planets) {
        for (int h = 0; h < 3; h++) {
            if (!p.levels[h].empty() && level_number(p, h) == level) {
                if (state().entered.insert(p.key).second) log("worldmap: " + p.key + " entered");
                break;
            }
        }
    }
}

// The save file hooks (jfg_auto_save.cpp), as Permanent Tribal Saves.
void worldmap_file_chosen() {
    load_disk();
    g_memory = g_disk;
}

void worldmap_on_save(int slot) {
    if (slot < 0 || slot >= SAVE_SLOTS) return;
    load_disk();
    g_disk[slot] = g_memory[slot];
    write_disk();
}

void worldmap_forget(int slot) {
    load_disk();
    for (int i = 0; i < SAVE_SLOTS; i++) {
        if (slot >= 0 && i != slot) continue;
        g_disk.erase(i);
        g_memory.erase(i);
    }
    write_disk();
}

void register_world_api() {
    recomp::overlays::register_base_export("jfg_world_find", api_world_find);
    recomp::overlays::register_base_export("jfg_world_state", api_world_state);
    recomp::overlays::register_base_export("jfg_world_unlock", api_world_unlock);
    recomp::overlays::register_base_export("jfg_world_complete", api_world_complete);
    recomp::overlays::register_base_export("jfg_galaxy_find", api_galaxy_find);
    recomp::overlays::register_base_export("jfg_galaxy_current", api_galaxy_current);
    recomp::overlays::register_base_export("jfg_galaxy_show", api_galaxy_show);
}

} // namespace jfg

JFG_WRAP(frontMap) {
    if (g_planets.empty() || rd32(o9(O9_OPEN)) == 0) { frontMap_orig(rdram, ctx); return; }
    if (!g_built) build(ctx);
    g_rate = (int)ctx->r4;
    const int before = (int)rd32(o9(O9_CURRENT));
    input(ctx);
    {
        HideStockGalaxy hide(g_galaxy != MAIN);
        frontMap_orig(rdram, ctx);
    }
    if (rd32(o9(O9_OPEN)) == 0 || g_current < 0) return;
    const Planet& p = g_planets[g_current];
    const bool moved = (int)rd32(o9(O9_CURRENT)) != before || rdf(o9(O9_TARGET_X)) != (float)p.x || rdf(o9(O9_TARGET_Y)) != (float)p.y;
    if (!moved) return;
    if (g_galaxy == MAIN) {
        g_current = -1; // the hero wheel moved the view to the new hero's world: back on the stock map
    } else {
        wrf(o9(O9_TARGET_X), (float)p.x); // a mod's galaxy keeps its planet
        wrf(o9(O9_TARGET_Y), (float)p.y);
    }
}

// The map's last drawing step before its frame (the route rectangles): the mods' planets go in under the frame.
JFG_WRAP(func_overlay_9_000039F0_1F047C8) {
    if (g_built && rd32(o9(O9_OPEN)) != 0) draw(ctx, g_rate);
    func_overlay_9_000039F0_1F047C8_orig(rdram, ctx);
}

JFG_WRAP(frontCleanupMap) {
    g_switching.store(false, std::memory_order_relaxed);
    if (g_built) free_blocks(ctx);
    frontCleanupMap_orig(rdram, ctx);
}

// Input thread (src/input/controls.cpp): 1 while the shoulder buttons switch galaxy.
extern "C" int jfg_worldmap_switching(void) {
    return g_switching.load(std::memory_order_relaxed) ? 1 : 0;
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
