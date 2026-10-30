// Jet Force Gemini port: settings that moved from the port into the official mods (owner, 2026-10-27), and profile
// keys that set a mod's options.
//
// The blood options went to the Blood mod (jfg_blood) and Hide Floyd to Better Floyd (jfg_better_floyd). An options
// file from an older version, or a profile (the Gore profile), still names the old keys; apply_option hands them here,
// and they become the mod's own options, with the mod turned on when one of them is not the original game's. A key
// "mod:<id>:<option>" sets any mod option the same way, and "mod:<id>:enabled" turns a mod on or off. The mods ship on
// their own, so a mod that isn't installed is skipped (the values wait until the mods have been scanned and the Mods
// list read). The next save of the options file no longer has the old keys. A mod can only be
// turned on before the game starts (with the launcher skipped, the game may start first): a turn-on that didn't take
// waits in jfg_mod_enable.txt for the next start.
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "librecomp/mods.hpp"
#include "jfg_options.h"

namespace jfg {

namespace {
constexpr char BLOOD[] = "jfg_blood", FLOYD[] = "jfg_better_floyd";

struct OldKey {
    const char* key;
    const char* mod;
    const char* option;
    int original; // the original game's value: anything else turns the mod on
};
constexpr OldKey OLD_KEYS[] = {
    { "no_blood", BLOOD, "no_blood", 0 },
    { "gore_keep_blood", BLOOD, "keep_blood", 0 },
    { "gore_keep_limbs", BLOOD, "keep_limbs", 0 },
    { "blood_amount", BLOOD, "amount", 100 },
    { "blood_splats", BLOOD, "splats", -1 }, // a limit only: on its own it turns nothing on
    { "hide_floyd", FLOYD, "hide", 0 },
};

struct Pending {
    std::string mod, option;
    int value;
    bool enable; // turn the mod on (or, for "enabled", the value says)
};
std::mutex g_mutex;
std::vector<Pending> g_pending;
constexpr char ENABLE_FILE[] = "jfg_mod_enable.txt"; // mod ids to turn on at the next start, one a line
std::vector<std::string> g_enable_later;

void enable(const std::string& mod) {
    if (recomp::mods::is_mod_enabled(mod)) return;
    recomp::mods::enable_mod(mod, true);
    const bool on = recomp::mods::is_mod_enabled(mod);
    std::fprintf(stderr, "[jfg] mod %s turned on%s\n", mod.c_str(), on ? "" : ": not now (the game has started)");
    if (!on) g_enable_later.push_back(mod);
}

void set_option(const Pending& p) {
    if (!recomp::mods::get_details_for_mod(p.mod).has_value()) {
        std::fprintf(stderr, "[jfg] mod option %s:%s=%d: the mod isn't installed\n", p.mod.c_str(), p.option.c_str(), p.value);
        return;
    }
    if (p.option == "enabled") {
        if (p.value != 0) enable(p.mod);
        else recomp::mods::enable_mod(p.mod, false);
        return;
    }
    const recomp::mods::ConfigSchema& schema = recomp::mods::get_mod_config_schema(p.mod);
    const auto it = schema.options_by_id.find(p.option);
    if (it == schema.options_by_id.end()) return;
    const recomp::mods::ConfigOption& o = schema.options[it->second];
    if (o.type == recomp::mods::ConfigOptionType::Enum) recomp::mods::set_mod_config_value(p.mod, p.option, (uint32_t)p.value);
    else if (o.type == recomp::mods::ConfigOptionType::Number) recomp::mods::set_mod_config_value(p.mod, p.option, (double)p.value);
    if (p.enable) enable(p.mod);
}
} // namespace

bool apply_mod_option(const char* key, int value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const OldKey& k : OLD_KEYS) {
        if (std::strcmp(key, k.key) != 0) continue;
        g_pending.push_back({ k.mod, k.option, value, k.original >= 0 && value != k.original });
        return true;
    }
    if (std::strncmp(key, "mod:", 4) != 0) return false;
    const char* id = key + 4;
    const char* colon = std::strchr(id, ':');
    if (colon == nullptr || colon == id || colon[1] == '\0') return true;
    g_pending.push_back({ std::string(id, colon), colon + 1, value, false });
    return true;
}

// Every UI frame (ui_state.cpp): once the mods have been scanned, the waiting values go to them.
void mod_options_frame() {
    std::lock_guard<std::mutex> lock(g_mutex);
    static bool read_enable_file = false;
    if (!read_enable_file) {
        read_enable_file = true;
        if (FILE* f = std::fopen(ENABLE_FILE, "r")) {
            char id[128];
            while (std::fscanf(f, "%127s", id) == 1) g_pending.push_back({ id, "enabled", 1, false });
            std::fclose(f);
            std::remove(ENABLE_FILE);
        }
    }
    if (g_pending.empty() || !recomp::mods::is_mods_config_loaded()) return; // the Mods list would undo a turn-on
    for (const Pending& p : g_pending) set_option(p);
    g_pending.clear();
    if (g_enable_later.empty()) return;
    if (FILE* f = std::fopen(ENABLE_FILE, "w")) {
        for (const std::string& mod : g_enable_later) std::fprintf(f, "%s\n", mod.c_str());
        std::fclose(f);
    }
    std::fprintf(stderr, "[jfg] %zu mod(s) to turn on at the next start (%s)\n", g_enable_later.size(), ENABLE_FILE);
    g_enable_later.clear();
}

} // namespace jfg
