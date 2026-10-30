// Jet Force Gemini port: support for code mods (.nrm).
//
// 1. Overlay hooks. To hook a function the runtime recompiles it again at start-up from the ROM it gets from
//    GameEntry::decompression_routine (librecomp's mods.cpp; nothing else uses it). The port recompiles overlays from
//    a copy of the ROM in which every overlay is pre-linked at a private address (tools/prepare_overlays.py), so the
//    live recompiler has to see that copy too, or it reads unlinked overlay code and crashes. jfg_prelink_rom makes it
//    from the player's ROM with the same algorithm (the game's runLink.c, as prepare_overlays.py implements it).
//    Functions whose code materialises another overlay's data address can't be regenerated correctly; the build
//    makes them unhookable (tools/refuse_mod_hooks.py). See Recomp-Docs/MOD_SUPPORT.md.
// 2. Functions mods can import ("*"): recomp_printf, written to jfg_mods.log and stderr.
// 3. Mods that replace a function a port feature needs as the caller: logged and shown in the Mods menu.
// (Mods that replace game data rather than code: jfg_assets.cpp.)
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"
#include "jfg_api.h"
#include "jfg_hooks.h"
#include "jfg_prelink.h"
void jfg_note_code_mods(); // below: logs where the code mods went

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

// --- recomp_printf ---------------------------------------------------------------------------------------------
// o32 varargs: argument slot i is $a0..$a3 for i < 4, then the stack at $sp + 4 * i. A double (and a long long)
// starts at an even slot and takes two, high word first.
struct VarArgs {
    uint8_t* rdram;
    recomp_context* ctx;
    unsigned slot = 1; // slot 0 is the format string
    uint32_t word(unsigned i) const {
        switch (i) {
            case 0: return (uint32_t)ctx->r4;
            case 1: return (uint32_t)ctx->r5;
            case 2: return (uint32_t)ctx->r6;
            case 3: return (uint32_t)ctx->r7;
            default: return (uint32_t)MEM_W(4 * i, ctx->r29);
        }
    }
    uint32_t next32() { return word(slot++); }
    uint64_t next64() {
        slot = (slot + 1) & ~1u;
        const uint64_t hi = word(slot), lo = word(slot + 1);
        slot += 2;
        return hi << 32 | lo;
    }
};

std::string read_guest_string(uint8_t* rdram, gpr addr, size_t limit = 4096) {
    std::string s;
    for (size_t i = 0; i < limit; i++) {
        const char c = (char)MEM_B(i, addr);
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

std::string guest_format(uint8_t* rdram, recomp_context* ctx) {
    const std::string fmt = read_guest_string(rdram, ctx->r4);
    VarArgs args{rdram, ctx};
    std::string out;
    char buf[512];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        // %[flags][width][.precision][length]conversion; '*' width/precision not supported
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) j++;
        while (j < fmt.size() && (std::isdigit((unsigned char)fmt[j]) || fmt[j] == '.')) j++;
        int longs = 0;
        bool half = false;
        while (j < fmt.size() && std::strchr("hlzjt", fmt[j])) { if (fmt[j] == 'l') longs++; if (fmt[j] == 'h') half = true; j++; }
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        std::string spec = fmt.substr(i, j - i);
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return std::strchr("hlzjt", c) != nullptr; }), spec.end());
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i':
                if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)args.next64());
                else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), half ? (int)(int16_t)args.next32() : (int)(int32_t)args.next32());
                out += buf; break;
            case 'u': case 'x': case 'X': case 'o':
                if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)args.next64());
                else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), half ? (unsigned)(uint16_t)args.next32() : (unsigned)args.next32());
                out += buf; break;
            case 'c': std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), (int)(char)args.next32()); out += buf; break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%08X", args.next32()); out += buf; break;
            case 's': {
                const uint32_t p = args.next32();
                const std::string s = p ? read_guest_string(rdram, (gpr)(int32_t)p) : std::string("(null)");
                std::snprintf(buf, sizeof buf, (spec + 's').c_str(), s.c_str());
                out += buf; break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                const uint64_t bits = args.next64();
                double d;
                std::memcpy(&d, &bits, sizeof d);
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                out += buf; break;
            }
            default: out += fmt.substr(i, j - i + 1); break;
        }
        i = j;
    }
    return out;
}

} // namespace

// int recomp_printf(const char* fmt, ...): mods import it from "*" (recomputils.h in the mod templates).
extern "C" void jfg_recomp_printf(uint8_t* rdram, recomp_context* ctx) {
    std::string text = guest_format(rdram, ctx);
    const int len = (int)text.size();
    // One log line per line of output; the logger adds the time stamp and the newline.
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        if (nl > start) jfg::jfg_log_line("jfg_mods.log", text.substr(start, nl - start).c_str());
        start = nl + 1;
    }
    ctx->r2 = len;
}

namespace {
// Each overlay's code at its private link address (0x82000000+), where code a mod hook regenerated runs.
struct PrivateCode { uint32_t link, size, rom; };
std::vector<PrivateCode> g_private_code;
} // namespace

// Called from jfg_on_init (jfg_glue.cpp): after the runtime has built its function table, before mods are loaded.
// A regenerated (hooked) overlay function calls the other functions of its own overlay by address lookup
// (librecomp's apply_regenlist sets use_lookup_for_all_function_calls), and in the pre-linked code those addresses
// are the private link addresses (0x82000000 upward), which the runtime's table doesn't have: the lookup failed and
// the game quit. So every overlay function is also entered under its private address. That's safe: the recompiled
// code finds its section's real load address at run time, whichever address it was called through, and it never
// collides with a real one (RDRAM ends at 0x80800000).
extern "C" void jfg_mods_alias_overlays(void) {
    const std::vector<jfg::prelink::Overlay> overlays = jfg::prelink::overlays(recomp::get_rom());
    const auto& by_rom = recomp::overlays::get_vrom_to_section_map();
    int sections = 0, funcs = 0;
    for (const jfg::prelink::Overlay& ov : overlays) {
        auto it = by_rom.find(ov.rom);
        if (ov.text == 0 || it == by_rom.end()) {
            continue;
        }
        if (recomp::overlays::get_section_ram_addr(it->second) != ov.link) {
            continue; // not laid out the way prepare_overlays.py did it: leave this section alone
        }
        sections++;
        g_private_code.push_back(PrivateCode{ ov.link, ov.text, ov.rom });
        for (uint32_t off = 0; off < ov.text; off += 4) {
            if (recomp_func_t* f = recomp::overlays::get_func_by_section_index_function_offset(it->second, off)) {
                recomp::overlays::add_loaded_function((int32_t)(ov.link + off), f);
                funcs++;
            }
        }
    }
    char line[128];
    std::snprintf(line, sizeof line, "mods: %d functions of %d overlays callable at their private addresses", funcs,
                  sections);
    jfg::jfg_log_line("jfg_mods.log", line);
}

// Where each code mod's code and data went this run: logged, and kept for the crash report (jfg_mods_at). A crash's game
// callstack holds a mod function's address as linked (every mod links at the same base, 0x81000000), not where it was
// loaded, so an address names the mods whose linked image covers it: look it up in each one's build/mod.map.
namespace {
struct ModRange { char id[64]; uint32_t start, size, link; };
ModRange g_mod_ranges[256];
int g_mod_range_count = 0;
} // namespace

void jfg_note_code_mods() {
    g_mod_range_count = 0;
    for (const recomp::mods::CodeModRange& r : recomp::mods::get_code_mod_ranges()) {
        if (g_mod_range_count >= (int)(sizeof(g_mod_ranges) / sizeof(g_mod_ranges[0]))) break;
        ModRange& m = g_mod_ranges[g_mod_range_count++];
        std::snprintf(m.id, sizeof m.id, "%s", r.mod_id.c_str());
        m.start = r.start;
        m.size = r.size;
        m.link = r.link_base;
        char line[160];
        std::snprintf(line, sizeof line, "mods: %s at 0x%08X-0x%08X (linked at 0x%08X)", m.id, m.start, m.start + m.size,
                      m.link);
        jfg::jfg_log_line("jfg_mods.log", line);
    }
}

extern "C" int jfg_mods_at(uint32_t address, char* out, int out_size) {
    int n = 0, used = 0;
    if (out_size > 0) out[0] = 0;
    for (int i = 0; i < g_mod_range_count; i++) {
        const ModRange& m = g_mod_ranges[i];
        if (address < m.link || address >= m.link + m.size) continue;
        const int w = std::snprintf(out + used, out_size > used ? (size_t)(out_size - used) : 0, "%s%s", n ? ", " : "", m.id);
        if (w > 0 && used + w < out_size) used += w;
        n++;
    }
    return n;
}

// Calls from one overlay into another go through the game's lazy linker, TrapDanglingJump, which finds the call site
// from $ra - 8. Code a mod hook regenerated runs at its overlay's private address, so its $ra is private too and the
// linker read outside RDRAM (a crash in the Lurg fight with a hook on boyBossControl). Such a $ra is moved to where
// that overlay is loaded; the recompiled code returns natively, without it.
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom); // jfg_glue.cpp

JFG_WRAP(TrapDanglingJump) {
    const uint32_t ra = (uint32_t)ctx->r31;
    if (ra >= 0x82000000u) {
        for (const PrivateCode& code : g_private_code) {
            if (ra - code.link <= code.size) {
                if (const uint32_t ram = jfg_overlay_loaded_at(code.rom)) {
                    ctx->r31 = (gpr)(int32_t)(ram + (ra - code.link));
                    static bool logged = false;
                    if (!logged) {
                        logged = true;
                        char line[128];
                        std::snprintf(line, sizeof line, "mods: a regenerated function called into another overlay "
                                      "(return address 0x%08X, loaded at 0x%08X)", ra, ram + (ra - code.link));
                        jfg::jfg_log_line("jfg_mods.log", line);
                    }
                }
                break;
            }
        }
    }
    TrapDanglingJump_orig(rdram, ctx);
}

extern "C" void jfg_register_asset_mods(void); // jfg_assets.cpp
extern "C" void jfg_register_music_mods(void); // jfg_music.cpp
extern "C" void jfg_register_script_mods(void); // jfg_lua.cpp
extern "C" void jfg_register_world_mods(void); // jfg_worldmap.cpp

// Called from src/main.cpp before the runtime starts.
// RecompiledFuncs/jfg_wrapped.c (N64Recomp, [wrappers] in jfg.us.toml): every game function the port wraps, with its
// original.
extern "C" void jfg_register_wrapped(void (*reg)(recomp_func_t* wrapped, recomp_func_t* original));

extern "C" void jfg_register_mod_exports(void) {
    recomp::overlays::register_base_export("recomp_printf", jfg_recomp_printf);
    jfg_register_wrapped(recomp::overlays::register_base_wrapper); // mods patch and hook the originals, not the wrappers
    jfg::api::register_api(); // the JFG mod API's events and helpers (jfg_api.cpp)
    jfg_register_asset_mods(); // mods that replace game data (jfg_assets.json)
    jfg_register_music_mods(); // mods' OGG music for levels (jfg_music.json)
    jfg_register_script_mods(); // mods' Lua scripts (jfg_scripts.json)
    jfg_register_world_mods(); // mods' planets on the planet-select map (jfg_world.json)
}

// --- mods replacing a function a port feature needs as the caller ---------------------------------------------------
// Some port features act on one game function's own call of another (JfgCaller in jfg_hooks.h): the caller's wrapper
// notes its stack frame, the callee's wrapper acts only when called from that frame. A mod that replaces such a caller
// (RECOMP_PATCH) runs its own code with its own frame, so those features turn off for as long as the mod is loaded.
// They can't be kept, but the player and the mod's author are told: jfg_mods.log, and the mod's page in the Mods menu.
extern "C" {
void func_80044FAC_45BAC(uint8_t* rdram, recomp_context* ctx);
void func_8001BF9C_1CB9C(uint8_t* rdram, recomp_context* ctx);
void func_80045F00_46B00(uint8_t* rdram, recomp_context* ctx);
void controlPlatform(uint8_t* rdram, recomp_context* ctx);
void refuelControl(uint8_t* rdram, recomp_context* ctx);
void func_overlay_15_00004658_1F17FD8(uint8_t* rdram, recomp_context* ctx);
void func_overlay_16_00004440_1F22458(uint8_t* rdram, recomp_context* ctx);
void func_overlay_17_00004614_1F2C15C(uint8_t* rdram, recomp_context* ctx);
void squadsFireWeapon(uint8_t* rdram, recomp_context* ctx);
void initFront(uint8_t* rdram, recomp_context* ctx);
void func_8006FD98_70998(uint8_t* rdram, recomp_context* ctx);
}

namespace {
struct CallerFeatures {
    recomp_func_t* func;
    const char* name;
    const char* features; // what stops working while a mod replaces it
};

// Every JfgCaller::Scope in src/jfg (keep in step with them).
const CallerFeatures g_caller_features[] = {
    { func_80044FAC_45BAC, "func_80044FAC_45BAC (the main loop tick)",
      "60 fps frame pacing, the dual-stick and mouse camera controls, cutscene skipping and F5" },
    { func_8001BF9C_1CB9C, "func_8001BF9C_1CB9C (level fog)", "the fog options" },
    { func_80045F00_46B00, "func_80045F00_46B00 (the world tick)",
      "the fly camera's freeze, Pause in the Settings Menu, the camera override and the squads' smoothing at 60 fps" },
    { controlPlatform, "controlPlatform", "Movement speed on moving platforms" },
    { refuelControl, "refuelControl", "Refuel speed" },
    { func_overlay_15_00004658_1F17FD8, "func_overlay_15_00004658_1F17FD8 (aim control)", "walking with the sniper rifle" },
    { func_overlay_16_00004440_1F22458, "func_overlay_16_00004440_1F22458 (aim control)", "walking with the sniper rifle" },
    { func_overlay_17_00004614_1F2C15C, "func_overlay_17_00004614_1F2C15C (aim control)", "walking with the sniper rifle" },
    { squadsFireWeapon, "squadsFireWeapon",
      "the fix for the crash when an enemy's weapon can't be created (low memory)" },
    { initFront, "initFront", "room for menu text longer than the original (translations)" },
    { func_8006FD98_70998, "func_8006FD98_70998 (text drawing)", "locales with their own character pages (Japanese)" },
};

// Mod id -> the text for its page in the Mods menu.
std::vector<std::pair<std::string, std::string>> g_mod_warnings;
} // namespace

// Once mods are loaded, before the game's first instruction (src/main.cpp, custom_recomp_entrypoint).
extern "C" void jfg_check_mod_replacements(void) {
    g_mod_warnings.clear();
    const std::vector<recomp::mods::ModDetails> details = recomp::mods::get_all_mod_details("jet-force-gemini");
    for (const CallerFeatures& c : g_caller_features) {
        const std::string mod_id = recomp::mods::get_func_replacement_mod(c.func);
        if (mod_id.empty()) continue;
        std::string mod_name = mod_id;
        for (const recomp::mods::ModDetails& d : details) {
            if (d.mod_id == mod_id) mod_name = d.display_name;
        }
        const std::string line = "mods: " + mod_name + " (" + mod_id + ") replaces " + c.name +
                                 "; off while it's loaded: " + c.features;
        jfg::jfg_log_line("jfg_mods.log", line.c_str());
        auto it = std::find_if(g_mod_warnings.begin(), g_mod_warnings.end(), [&](const auto& w) { return w.first == mod_id; });
        if (it == g_mod_warnings.end()) {
            g_mod_warnings.emplace_back(mod_id, "This mod replaces game code that Jet Force Gemini: Recompiled's own "
                                                "features rely on, so these are off while it's loaded: ");
            it = g_mod_warnings.end() - 1;
        }
        else {
            it->second += "; ";
        }
        it->second += std::string(c.features) + " (" + c.name + ")"; // one line: labels don't keep line breaks
    }
}

// The Mods menu's details panel (src/ui/ui_mod_details_panel.cpp): the warning for a mod, or "" (none, or the game
// hasn't started: mods load with the game).
extern "C" const char* jfg_mod_warning(const char* mod_id) {
    for (const auto& w : g_mod_warnings) {
        if (w.first == mod_id) return w.second.c_str();
    }
    return "";
}

// GameEntry::decompression_routine (src/main.cpp): the ROM the runtime regenerates hooked functions from.
std::vector<uint8_t> jfg_prelink_rom(std::span<const uint8_t> rom) {
    std::vector<uint8_t> out;
    const int n = jfg::prelink::link(rom, out);
    if (n < 0) {
        jfg::jfg_log_line("jfg_mods.log", "mods: the ROM's overlay tables don't fit; hooks use the ROM as it is");
        return out;
    }
    char line[128];
    std::snprintf(line, sizeof line, "mods: pre-linked %d overlays for hooks (private addresses from 0x%08X)", n,
                  jfg::prelink::LINK_BASE);
    jfg::jfg_log_line("jfg_mods.log", line);
    return out;
}
