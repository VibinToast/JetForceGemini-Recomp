// Jet Force Gemini port: mods' patches of the squad code's animation table (jfg_assets.json "baddies").
//
// Each baddy type (a row: the index BaddyTypeToIndex gives a member object id) has a run of 8-byte animation entries in
// overlay 3's data (BaddyAnimSet, BaddyAnim in jfg_fields.h): the action each plays for, a second action it also
// answers, its weight in the pick, and the fire set it shoots with. This is code data, not an asset, so the asset
// patches can't reach it. A mod changes entries by field:
//     "baddies": [ { "row": 17, "anim": 16, "expect": { "action": 1 }, "set": { "action": 53 } },
//                  { "row": 32, "anim": 18, "set": { "fireSet": 32 } } ]
// "anim" is the entry's index in the row's set. Fields: action, altAction, weight (0-31), stance (0-7), fireSet (0-126,
// or null for none), loops (0-15). "expect" (optional) names the values the mod was made for: when an entry holds
// others (other data, or another mod changed it first), the patch is skipped and logged. Patches apply every time
// overlay 3 is copied in, before any of its code runs, mods from the top of the mod list down, each mod's in file
// order. Several rows can share entries (the game's own sets overlap): a change shows in every row using that entry.
// jfgformats.baddies (the decomp's tools) reads the same table and names the same fields.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "jfg_fields.h"
#include "jfg_game.h"
#include "json/json.hpp"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp

namespace {
// Overlay 3 (the squad code, ROM 0x01ED54E0): the animation sets (one BaddyAnimSet per row) at +0x19278 and the
// animations (BaddyAnim) at +0x17DB8. 54 rows in the US game.
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0, ANIM_SETS = 0x19278, ANIMS = 0x17DB8;
constexpr int ROWS = 54;
constexpr uint8_t NO_FIRE_SET = 0x7F;

// A field of an entry: the action bytes, or bits of the word at +4 (from bit 31 down: weight 5, two flags, stance 3,
// fire set 7, loops 4, 11 more).
struct AnimField {
    const char* name;
    int byte;       // +2 / +3 for the action bytes, -1 for the word
    int shift, bits;
};
constexpr AnimField FIELDS[] = {
    { "action", 2, 0, 8 }, { "altAction", 3, 0, 8 },
    { "weight", -1, 27, 5 }, { "stance", -1, 22, 3 }, { "fireSet", -1, 15, 7 }, { "loops", -1, 11, 4 },
};

struct Change {
    const AnimField* field;
    uint32_t value;
};

struct BaddyPatch {
    size_t order;
    std::string mod_id;
    int row = -1, anim = -1;
    std::vector<Change> expect, set;
};

std::vector<BaddyPatch> g_read;          // filled by enabled mods (load_mods)
std::vector<BaddyPatch> g_baddy_patches; // this game's, from activate_baddy_patches

void log(const std::string& text) { jfg_log_line("jfg_mods.log", text.c_str()); }

uint32_t get_field(uint32_t entry, const AnimField& f) {
    if (f.byte >= 0) return rd8(entry + (uint32_t)f.byte);
    return (rd32(entry + 4) >> f.shift) & ((1u << f.bits) - 1);
}

void set_field(uint32_t entry, const AnimField& f, uint32_t value) {
    if (f.byte >= 0) { wr8(entry + (uint32_t)f.byte, (uint8_t)value); return; }
    const uint32_t mask = ((1u << f.bits) - 1) << f.shift;
    wr32(entry + 4, (rd32(entry + 4) & ~mask) | ((value << f.shift) & mask));
}

// {"field": value, ...} -> changes; false when a name or value doesn't fit.
bool read_changes(const nlohmann::json& j, std::vector<Change>& out) {
    if (!j.is_object()) return false;
    for (const auto& [name, v] : j.items()) {
        const AnimField* f = nullptr;
        for (const AnimField& c : FIELDS) if (name == c.name) f = &c;
        if (f == nullptr) return false;
        uint32_t value;
        if (v.is_null() && std::string(f->name) == "fireSet") value = NO_FIRE_SET;
        else if (v.is_number_integer() && v.get<int64_t>() >= 0 && v.get<int64_t>() < (int64_t(1) << f->bits)) value = (uint32_t)v.get<int64_t>();
        else return false;
        if (std::string(f->name) == "fireSet" && v.is_number_integer() && value == NO_FIRE_SET) return false; // null is "none"
        out.push_back({ f, value });
    }
    return true;
}
} // namespace

// The "baddies" of one mod's jfg_assets.json (jfg_assets.cpp, on_asset_mod_enabled).
void read_baddy_patches(const nlohmann::json& j, const std::string& id, size_t order) {
    for (const nlohmann::json& e : j.value("baddies", nlohmann::json::array())) {
        BaddyPatch p;
        p.order = order;
        p.mod_id = id;
        p.row = e.value("row", -1);
        p.anim = e.value("anim", -1);
        const bool ok = p.row >= 0 && p.row < ROWS && p.anim >= 0 && e.contains("set") &&
                        read_changes(e["set"], p.set) && !p.set.empty() &&
                        (!e.contains("expect") || read_changes(e["expect"], p.expect));
        if (!ok) {
            log("baddies: " + id + ": a patch needs \"row\" (0-53), \"anim\" and \"set\" with known fields and values "
                "in range - skipped: " + e.dump());
            continue;
        }
        g_read.push_back(std::move(p));
    }
}

// The mods' patches go live (jfg_apply_asset_mods, after the runtime has loaded mods and before the game runs).
void activate_baddy_patches() {
    g_baddy_patches = std::move(g_read);
    g_read.clear();
    std::stable_sort(g_baddy_patches.begin(), g_baddy_patches.end(),
                     [](const BaddyPatch& a, const BaddyPatch& b) { return a.order < b.order; });
}

// Overlay 3 was just copied to ram (romCopy's wrapper, jfg_engine_wrappers.cpp): applies the patches to its data.
void apply_baddy_patches(uint32_t rom, uint32_t ram) {
    if (rom != OVERLAY_3_ROM || g_baddy_patches.empty()) return;
    static bool logged = false; // the same lines would repeat at every load
    char line[320];
    int applied = 0;
    for (const BaddyPatch& p : g_baddy_patches) {
        const uint32_t set = ram + ANIM_SETS + (uint32_t)p.row * BaddyAnimSet::Size;
        const int count = BaddyAnimSet::Count::get(set);
        if (p.anim >= count) {
            if (!logged) {
                std::snprintf(line, sizeof line, "baddies: %s: row %d has %d animations, no %d - skipped", p.mod_id.c_str(),
                              p.row, count, p.anim);
                log(line);
            }
            continue;
        }
        const uint32_t entry = ram + ANIMS + (BaddyAnimSet::First::get(set) + (uint32_t)p.anim) * BaddyAnim::Size;
        bool fits = true;
        for (const Change& c : p.expect) {
            const uint32_t now = get_field(entry, *c.field);
            if (now == c.value) continue;
            fits = false;
            if (!logged) {
                std::snprintf(line, sizeof line, "baddies: %s: row %d animation %d: %s is %u, not %u - skipped (the mod was "
                              "made for other data, or another mod changed it first)", p.mod_id.c_str(), p.row, p.anim,
                              c.field->name, now, c.value);
                log(line);
            }
        }
        if (!fits) continue;
        for (const Change& c : p.set) set_field(entry, *c.field, c.value);
        applied++;
    }
    if (!logged) {
        std::snprintf(line, sizeof line, "baddies: %d of %zu patches applied to overlay 3's animation table", applied,
                      g_baddy_patches.size());
        log(line);
    }
    logged = true;
}
} // namespace jfg
