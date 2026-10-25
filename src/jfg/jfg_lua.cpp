// Jet Force Gemini port: mods' Lua scripts (the Modding API plan §6, first set). Lua 5.4, vendored in lib/lua.
//
// A mod lists its scripts in jfg_scripts.json:  { "scripts": ["scripts/main.lua", ...] }
// Each mod gets a Lua state of its own with the base, string, table, math, utf8 and coroutine libraries (no io, os,
// package or loading of files or bytecode). A script registers handlers with jfg.on(event, function) and calls the
// helpers below. An error, or a handler that runs more than 2,000,000 instructions in one call, is logged to
// jfg_mods.log and that handler is turned off; the game goes on.
//
// Events (game thread):
//   "level"  (level)          the first tick in a level (its objects are set up)
//   "tick"   (level, seconds) every game tick in a level; seconds since it started
// Helpers:
//   jfg.log(...)                            a line in jfg_mods.log
//   jfg.level()                             the current level number
//   jfg.level_id(name)                      "stock:21" -> 21, "level_x" (this mod's) or "mod:level_x" -> its number
//   jfg.player()                            x, y, z of the first player, or nil
//   jfg.flag(n), jfg.set_flag(n, on)        the game's flags (mainGetGameFlag / mainSetGameFlag)
//   jfg.unlock_door(id)                     as a squadron's death does (doorUnlock)
//   jfg.fog(r, g, b, near, far, ticks)      fades the level's fog (trackFadeFog)
//   jfg.message(text [, seconds])           a line on screen (the port's overlay), 3 s by default
//   jfg.music(name), jfg.music_stop()       one of this mod's OGG tracks (jfg_music.json names) in this level
//   jfg.warp(level, start_point)            changes level as the port's warp does
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "lua.hpp"
#include "jfg_game.h"
#include "jfg_port.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}
extern "C" {
void doorUnlock(uint8_t* rdram, recomp_context* ctx);
void trackFadeFog(uint8_t* rdram, recomp_context* ctx);
}

namespace {

using jfg::ADDR_currentCharacter;
using jfg::ADDR_currentLevel;
using jfg::ADDR_mainGameMode;
using jfg::call_game;
using jfg::rd16;
using jfg::rd32;
using jfg::rdf;

constexpr char CONTENT_FILE[] = "jfg_scripts.json";
constexpr int BUDGET = 2000000;  // instructions per handler call
constexpr int HOOK_STEP = 10000;

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

struct Script {
    std::string mod_id;
    lua_State* L = nullptr;
    std::map<std::string, std::vector<int>> handlers; // event -> registry refs
    std::vector<std::pair<std::string, std::string>> sources; // (file, text), run once the game starts
    bool started = false;
    int budget = 0;
};
std::vector<std::unique_ptr<Script>> g_scripts;
recomp_context* g_ctx = nullptr; // the game thread's context while handlers run

// On-screen message (drawn by jfg_tick.cpp's overlay through lua_overlay_text).
std::mutex g_msg_mutex;
std::string g_msg;
std::chrono::steady_clock::time_point g_msg_until{};

Script* self(lua_State* L) { return (Script*)lua_touserdata(L, lua_upvalueindex(1)); }

void count_hook(lua_State* L, lua_Debug*) {
    Script* s = nullptr;
    lua_getfield(L, LUA_REGISTRYINDEX, "jfg_script");
    s = (Script*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    if (s && (s->budget -= HOOK_STEP) < 0) luaL_error(L, "a handler ran too long (more than %d instructions)", BUDGET);
}

std::string args_text(lua_State* L, int from) {
    std::string out;
    for (int i = from; i <= lua_gettop(L); i++) {
        if (i > from) out += " ";
        size_t n = 0;
        const char* t = luaL_tolstring(L, i, &n);
        out.append(t, n);
        lua_pop(L, 1);
    }
    return out;
}

int l_log(lua_State* L) {
    log("script " + self(L)->mod_id + ": " + args_text(L, 1));
    return 0;
}

int l_on(lua_State* L) {
    Script* s = self(L);
    const std::string event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (event != "level" && event != "tick") return luaL_error(L, "unknown event '%s' (level, tick)", event.c_str());
    lua_pushvalue(L, 2);
    s->handlers[event].push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

int l_level(lua_State* L) {
    lua_pushinteger(L, (int16_t)rd16(ADDR_currentLevel));
    return 1;
}

int l_level_id(lua_State* L) {
    if (lua_isinteger(L, 1)) { lua_pushinteger(L, lua_tointeger(L, 1)); return 1; }
    std::string name = luaL_checkstring(L, 1);
    if (name.rfind("stock:", 0) == 0) { lua_pushinteger(L, std::atoi(name.c_str() + 6)); return 1; }
    if (name.find(':') == std::string::npos) name = self(L)->mod_id + ":" + name;
    const int n = jfg::asset_number(name);
    if (n < 0) lua_pushnil(L); else lua_pushinteger(L, n);
    return 1;
}

int l_player(lua_State* L) {
    const uint32_t obj = g_ctx ? jfg::player_object(g_ctx) : 0;
    if (!obj) { lua_pushnil(L); return 1; }
    lua_pushnumber(L, rdf(obj + 0x0C));
    lua_pushnumber(L, rdf(obj + 0x10));
    lua_pushnumber(L, rdf(obj + 0x14));
    return 3;
}

int l_flag(lua_State* L) {
    const int n = (int)luaL_checkinteger(L, 1);
    lua_pushboolean(L, g_ctx && call_game(mainGetGameFlag, g_ctx, { (uint32_t)n }) != 0);
    return 1;
}

int l_set_flag(lua_State* L) {
    const int n = (int)luaL_checkinteger(L, 1);
    const bool on = lua_toboolean(L, 2);
    if (g_ctx) call_game(mainSetGameFlag, g_ctx, { (uint32_t)n, on ? 1u : 0u });
    return 0;
}

int l_unlock_door(lua_State* L) {
    const int id = (int)luaL_checkinteger(L, 1);
    if (g_ctx) call_game(doorUnlock, g_ctx, { (uint32_t)id, 0u });
    return 0;
}

int l_fog(lua_State* L) {
    const uint32_t r = (uint32_t)luaL_checkinteger(L, 1), g = (uint32_t)luaL_checkinteger(L, 2);
    const uint32_t b = (uint32_t)luaL_checkinteger(L, 3), near_ = (uint32_t)luaL_checkinteger(L, 4);
    const uint32_t far_ = (uint32_t)luaL_checkinteger(L, 5);
    const float ticks = (float)luaL_optnumber(L, 6, 60.0);
    uint32_t tbits;
    std::memcpy(&tbits, &ticks, 4);
    if (g_ctx) call_game(trackFadeFog, g_ctx, { 0u, r, g, b, near_, far_, tbits });
    return 0;
}

int l_message(lua_State* L) {
    const std::string text = luaL_checkstring(L, 1);
    const double seconds = luaL_optnumber(L, 2, 3.0);
    log("script " + self(L)->mod_id + ": message: " + text);
    std::lock_guard<std::mutex> lock(g_msg_mutex);
    g_msg = text;
    g_msg_until = std::chrono::steady_clock::now() + std::chrono::milliseconds((int)(seconds * 1000));
    return 0;
}

int l_music(lua_State* L) {
    jfg::music_force(self(L)->mod_id, luaL_checkstring(L, 1));
    return 0;
}

int l_music_stop(lua_State* L) {
    jfg::music_force(self(L)->mod_id, "");
    return 0;
}

int l_warp(lua_State* L) {
    const int level = (int)luaL_checkinteger(L, 1);
    const int start = (int)luaL_optinteger(L, 2, 0);
    jfg::request_warp(level, (int16_t)rd16(ADDR_currentCharacter), start, 16);
    return 0;
}

int l_print(lua_State* L) { return l_log(L); }

void open_state(Script& s) {
    lua_State* L = luaL_newstate();
    s.L = L;
    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    luaL_requiref(L, LUA_COLIBNAME, luaopen_coroutine, 1);
    lua_settop(L, 0);
    for (const char* name : { "dofile", "loadfile", "load", "require" }) {  // no files, no bytecode
        lua_pushnil(L);
        lua_setglobal(L, name);
    }
    lua_pushlightuserdata(L, &s);
    lua_setfield(L, LUA_REGISTRYINDEX, "jfg_script");
    lua_pushlightuserdata(L, &s);
    lua_pushcclosure(L, l_print, 1);
    lua_setglobal(L, "print");
    static const luaL_Reg fns[] = {
        { "log", l_log }, { "on", l_on }, { "level", l_level }, { "level_id", l_level_id }, { "player", l_player },
        { "flag", l_flag }, { "set_flag", l_set_flag }, { "unlock_door", l_unlock_door }, { "fog", l_fog },
        { "message", l_message }, { "music", l_music }, { "music_stop", l_music_stop }, { "warp", l_warp },
        { nullptr, nullptr } };
    lua_newtable(L);
    lua_pushlightuserdata(L, &s);
    luaL_setfuncs(L, fns, 1);
    lua_setglobal(L, "jfg");
    lua_sethook(L, count_hook, LUA_MASKCOUNT, HOOK_STEP);
}

// Runs a function on the stack top with `nargs` arguments under the budget; logs and returns false on an error.
bool run(Script& s, int nargs, const std::string& what) {
    s.budget = BUDGET;
    if (lua_pcall(s.L, nargs, 0, 0) != LUA_OK) {
        log("script " + s.mod_id + ": " + what + ": " + (lua_tostring(s.L, -1) ? lua_tostring(s.L, -1) : "error"));
        lua_pop(s.L, 1);
        return false;
    }
    return true;
}

void start(Script& s) {
    s.started = true;
    open_state(s);
    for (const auto& [file, text] : s.sources) {
        if (luaL_loadbufferx(s.L, text.data(), text.size(), ("@" + file).c_str(), "t") != LUA_OK) {
            log("script " + s.mod_id + ": " + (lua_tostring(s.L, -1) ? lua_tostring(s.L, -1) : file));
            lua_pop(s.L, 1);
            continue;
        }
        run(s, 0, file);
    }
    size_t n = 0;
    for (const auto& [_, list] : s.handlers) n += list.size();
    log("script " + s.mod_id + ": " + std::to_string(s.sources.size()) + " scripts, " + std::to_string(n) + " handlers");
}

void fire(const char* event, std::initializer_list<double> args) {
    for (auto& sp : g_scripts) {
        Script& s = *sp;
        auto it = s.handlers.find(event);
        if (it == s.handlers.end()) continue;
        std::vector<int>& list = it->second;
        for (size_t k = 0; k < list.size();) {
            lua_rawgeti(s.L, LUA_REGISTRYINDEX, list[k]);
            for (double a : args) {
                if (a == (double)(lua_Integer)a) lua_pushinteger(s.L, (lua_Integer)a); // levels are integers
                else lua_pushnumber(s.L, a);
            }
            if (run(s, (int)args.size(), std::string("\"") + event + "\" handler")) {
                k++;
            } else {
                luaL_unref(s.L, LUA_REGISTRYINDEX, list[k]);
                list.erase(list.begin() + k);
                log("script " + s.mod_id + ": that \"" + event + "\" handler is turned off");
            }
        }
    }
}

void on_scripts_enabled(recomp::mods::ModContext&, const recomp::mods::ModHandle& mod) {
    bool exists = false;
    const std::vector<char> text = mod.manifest.file_handle->read_file(CONTENT_FILE, exists);
    if (!exists) return;
    const nlohmann::json j = nlohmann::json::parse(text.begin(), text.end(), nullptr, false);
    const std::string id = mod.manifest.mod_id;
    if (!j.is_object()) {
        log("script " + id + ": " + CONTENT_FILE + " isn't valid JSON - its scripts are off");
        return;
    }
    auto s = std::make_unique<Script>();
    s->mod_id = id;
    for (const auto& f : j.value("scripts", nlohmann::json::array())) {
        if (!f.is_string()) continue;
        bool ok = false;
        std::vector<char> src = mod.manifest.file_handle->read_file(f.get<std::string>(), ok);
        if (!ok) { log("script " + id + ": " + f.get<std::string>() + " is not in the mod"); continue; }
        s->sources.emplace_back(f.get<std::string>(), std::string(src.begin(), src.end()));
    }
    if (!s->sources.empty()) g_scripts.push_back(std::move(s));
}

} // namespace

extern "C" void jfg_register_script_mods(void) {
    recomp::mods::ModContentType type{
        .content_filename = CONTENT_FILE,
        .allow_runtime_toggle = false,
        .on_enabled = on_scripts_enabled,
        .on_disabled = nullptr,
        .on_reordered = nullptr,
    };
    recomp::mods::register_mod_content_type(type);
}

namespace jfg {

// Every game tick: starts the scripts once, then fires "level" on the first tick in a level and "tick" after it.
void lua_tick(recomp_context* ctx) {
    if (g_scripts.empty()) return;
    static int last_level = -1;
    static auto level_start = std::chrono::steady_clock::now();
    g_ctx = ctx;
    for (auto& s : g_scripts) {
        if (!s->started) start(*s);
    }
    const int mode = (int)rd32(ADDR_mainGameMode);
    const int level = (int)(int16_t)rd16(ADDR_currentLevel);
    if (mode == 0) {
        if (level != last_level) {
            last_level = level;
            level_start = std::chrono::steady_clock::now();
            fire("level", { (double)level });
        }
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - level_start).count();
        fire("tick", { (double)level, t });
    } else if (mode != 0 && level != last_level) {
        last_level = -1;  // between levels: the next one fires "level" again
    }
    g_ctx = nullptr;
}

// The on-screen message, if one is showing: (text, alpha 0-1). UI thread.
bool lua_overlay_text(std::string& text, float& alpha) {
    std::lock_guard<std::mutex> lock(g_msg_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (g_msg.empty() || now >= g_msg_until) return false;
    text = g_msg;
    const float left = std::chrono::duration<float>(g_msg_until - now).count();
    alpha = std::min(1.0f, left / 0.5f);
    return true;
}

} // namespace jfg
