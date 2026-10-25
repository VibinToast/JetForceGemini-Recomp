// Jet Force Gemini port: launch options for mod tools (JFG-Studio's "test" starts the game with these).
//
//   --start-level N         start straight in level N: the game boots, goes to the title screen as F5 does, then
//                           changes level the way JFG_DEBUG_WARP does (supported; the debug switch stays for agents)
//   --start-point N         the start point (entrance) in that level, default 0
//   --character NAME        vela, juno or lupus (or 0-2), default juno
//   --front-mode N          the front-end mode the level starts with, default 16 (in game with the HUD; 0 has none)
//   --mods-only ID[,ID...]  this run loads only these mods (and what they depend on); the player's Mods list and its
//                           saved order are left alone (N64ModernRuntime: set_run_only_mods)
//   --capabilities          prints what this build supports as JSON on stdout and exits (print_capabilities)
//
// --start-level implies --skip-launcher. Unknown values are refused at start with a message on stderr and exit code 2,
// so a tool never runs a game it didn't ask for. Results go to jfg_mods.log ("launch: ...").
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

#include "json/json.hpp"
#include "librecomp/mods.hpp"
#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_port.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

struct LaunchOptions {
    int start_level = -1;
    int start_point = 0;
    int character = 1; // Juno
    int front_mode = 16;
    bool capabilities = false;
    std::vector<std::string> mods_only;
    bool has_mods_only = false;
};
LaunchOptions g_launch;

void log(const std::string& text) { jfg::jfg_log_line("jfg_mods.log", text.c_str()); }

bool parse_int(const char* text, int& out, int lo, int hi) {
    char* end = nullptr;
    const long v = std::strtol(text, &end, 0);
    if (end == text || *end != '\0' || v < lo || v > hi) return false;
    out = (int)v;
    return true;
}

int parse_character(const char* text) {
    static const char* const names[] = { "vela", "juno", "lupus" };
    std::string lower = text;
    for (char& c : lower) c = (char)std::tolower((unsigned char)c);
    for (int i = 0; i < 3; i++) {
        if (lower == names[i]) return i;
    }
    int n;
    return parse_int(text, n, 0, 2) ? n : -1;
}

[[noreturn]] void refuse(const std::string& text) {
    std::fprintf(stderr, "[jfg] launch options: %s\n", text.c_str());
    std::exit(2);
}

// stdout of a Windows-subsystem build goes nowhere when started from a console; attach to the parent's console then
// (a tool reading a pipe already has a valid handle).
void attach_stdout() {
#ifdef _WIN32
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if ((out == nullptr || out == INVALID_HANDLE_VALUE) && AttachConsole(ATTACH_PARENT_PROCESS)) {
        std::freopen("CONOUT$", "w", stdout);
    }
#endif
}

void print_capabilities() {
    using nlohmann::json;
    json j;
    j["format"] = 1;
    j["port_version"] = JFG_VERSION_STRING;
    j["api_version"] = jfg::api::VERSION;
    j["content_types"] = {
        { "mod_binary.bin", { { "what", "code mod (RecompModTool)" } } },
        { "patch.bps", { { "what", "patch of the whole ROM, applied before the game starts" }, { "max_mods", 1 } } },
        { "jfg_assets.json", {
            { "what", "game data: whole assets, and entries of offset-table sets" },
            { "assets", true },
            { "entries", { { "entry", true }, { "append", true }, { "expect", true } } },
            { "patches", { { "set", true }, { "objlist", { "set", "delete", "append" } } } },
            { "fixups", { { "types", { "u8", "u16", "s16", "u32", "lohi" } }, { "packed", true }, { "other_mods", true } } },
            { "ids_kept_in", "mod_ids.json" },
            { "append_order", "next free number; mods from the top of the mod list, then file order" },
            { "numbers_logged_to", "jfg_mods.log" },
        } },
        { "rt64.json", { { "what", "texture pack" } } },
    };
    j["planet_map"] = { { "route", "patch.bps" }, { "in_place_only", true },
                        { "note", "overlay 9's data is read from the ROM image when it loads, so a patch.bps over its "
                          "tables in place reaches the game; its code is recompiled ahead of time, so moving or growing "
                          "the overlay does not" } };
    j["launch_options"] = {
        { "--start-level", { { "arg", "level" }, { "implies", "--skip-launcher" } } },
        { "--start-point", { { "arg", "start point" }, { "default", 0 } } },
        { "--character", { { "arg", "character" }, { "values", { "vela", "juno", "lupus" } }, { "default", "juno" } } },
        { "--front-mode", { { "arg", "front-end mode" }, { "default", 16 } } },
        { "--mods-only", { { "arg", "mod ids, comma separated" }, { "saves_mods_list", false } } },
        { "--capabilities", { { "arg", nullptr } } },
        { "--rom", { { "arg", "ROM path" } } },
        { "--skip-launcher", { { "arg", nullptr } } },
    };
    j["limits"] = {
        { "rom_bytes", 0x0FC00000 },        // the PI bus's cartridge window
        { "asset_count", 71 },              // the US ROM's asset table
        { "rom_patch_mods", 1 },            // the runtime refuses a second patch.bps mod
        { "level_number_max", 2047 },       // MultiCharacter +0x74 keeps 11 bits of the level
    };
    attach_stdout();
    std::printf("%s\n", j.dump(2).c_str());
    std::fflush(stdout);
}

} // namespace

// Called from main's argument loop (src/main.cpp) with argv[*i]: 1 when it was one of these options (*i then points
// at its last word), 0 when it's another option.
extern "C" int jfg_launch_arg(int argc, char** argv, int* i) {
    const char* arg = argv[*i];
    auto value = [&]() -> const char* {
        if (*i + 1 >= argc) refuse(std::string(arg) + " needs a value");
        return argv[++*i];
    };
    if (std::strcmp(arg, "--start-level") == 0) {
        const char* v = value();
        if (!parse_int(v, g_launch.start_level, 0, 2047)) refuse(std::string("--start-level ") + v + ": not a level number");
    } else if (std::strcmp(arg, "--start-point") == 0) {
        const char* v = value();
        if (!parse_int(v, g_launch.start_point, 0, 255)) refuse(std::string("--start-point ") + v + ": not 0-255");
    } else if (std::strcmp(arg, "--character") == 0) {
        const char* v = value();
        g_launch.character = parse_character(v);
        if (g_launch.character < 0) refuse(std::string("--character ") + v + ": vela, juno or lupus");
    } else if (std::strcmp(arg, "--front-mode") == 0) {
        const char* v = value();
        if (!parse_int(v, g_launch.front_mode, 0, 255)) refuse(std::string("--front-mode ") + v + ": not 0-255");
    } else if (std::strcmp(arg, "--mods-only") == 0) {
        const std::string list = value();
        g_launch.has_mods_only = true;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t comma = std::min(list.find(',', start), list.size());
            if (comma > start) g_launch.mods_only.push_back(list.substr(start, comma - start));
            start = comma + 1;
        }
    } else if (std::strcmp(arg, "--capabilities") == 0) {
        g_launch.capabilities = true;
    } else {
        return 0;
    }
    return 1;
}

// Called from main once the arguments are read: -1 to go on, otherwise the exit code (--capabilities). Sets
// *skip_launcher for --start-level and limits this run's mods for --mods-only.
extern "C" int jfg_launch_ready(bool* skip_launcher) {
    if (g_launch.capabilities) {
        print_capabilities();
        return 0;
    }
    if (g_launch.start_level >= 0) *skip_launcher = true;
    if (g_launch.has_mods_only) recomp::mods::set_run_only_mods(g_launch.mods_only);
    return -1;
}

namespace jfg {

// Every game tick (jfg_debug_frame): carries out --start-level, and reports --mods-only ids that aren't there.
void launch_tick(recomp_context* ctx) {
    enum class Step { Boot, Title, Arrive, Done };
    static Step step = Step::Boot;
    static int ticks = 0, steady = 0;
    static bool mods_checked = false;
    if (!mods_checked) {
        mods_checked = true;
        if (g_launch.has_mods_only) {
            std::string line = "launch: --mods-only, this run's mods:";
            for (const std::string& id : g_launch.mods_only) {
                const bool on = recomp::mods::is_mod_enabled(id);
                line += " " + id + (on ? "" : " (not in the mods folder)");
            }
            log(line);
        }
    }
    if (g_launch.start_level < 0 || step == Step::Done) return;
    ticks++;
    const int mode = (int)rd32(ADDR_mainGameMode);
    switch (step) {
    case Step::Boot:
        // F5's way to the title screen, once the start-up screens run (a level change from those doesn't stay).
        if (ticks >= 60 && (mode == 0 || mode == 1)) {
            g_reset_pending = true;
            step = Step::Title;
        }
        break;
    case Step::Title:
        // Change level once the title screen (front-end mode 3) has run for a moment.
        if (g_reset_pending) break;
        steady = (call_game(frontGetMode, ctx, {}) & 0xFFu) == 3 ? steady + 1 : 0;
        if (steady >= 30) {
            request_warp(g_launch.start_level, g_launch.character, g_launch.start_point, g_launch.front_mode);
            step = Step::Arrive;
            ticks = 0;
        }
        break;
    case Step::Arrive: {
        char line[160];
        if ((int16_t)rd16(ADDR_currentLevel) == g_launch.start_level && mode == 0) {
            std::snprintf(line, sizeof line, "launch: started in level %d, start point %d, as %s (front-end mode %d)",
                          g_launch.start_level, g_launch.start_point, character_names[g_launch.character], g_launch.front_mode);
            log(line);
            step = Step::Done;
        } else if (ticks > 900) {
            std::snprintf(line, sizeof line, "launch: level %d did not start (the game is in level %d)", g_launch.start_level,
                          (int)(int16_t)rd16(ADDR_currentLevel));
            log(line);
            step = Step::Done;
        }
        break;
    }
    case Step::Done:
        break;
    }
}

} // namespace jfg
