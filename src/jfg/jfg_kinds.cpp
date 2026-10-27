// Jet Force Gemini port: mods' new object types ("objtypes" in jfg_assets.json, jfg_assets.cpp) in the squad code.
//
// A pack can add an enemy made from a stock one: a copy of its definition pointing at a copy of its model with the
// pack's own textures, under a new object id. The squad code picks its per-type data (the baddy row, animation sets,
// sounds) through BaddyTypeToIndex, a switch on the stock object ids; for a mod's id it is asked about the stock id the
// type was made from, so the new enemy moves, fights and dies like that one.
//
// The level loader asks the same of func_800050A8 (is this object id a squad member?) for each list entry: members are
// not spawned there but kept for their squadron, which spawns them. Without the stock id a mod's members were spawned
// on their own at load - outside the squad, idle, at the wrong size - and the squadron made one member of its own
// (play-tested 2026-10-25; the level then crashed while drawing them).
#include <cstdint>
#include <set>
#include <string>

#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

JFG_WRAP(BaddyTypeToIndex) {
    const int id = (int)(int32_t)ctx->r4;
    const int like = jfg::baddy_like(id);
    if (like >= 0) {
        static std::set<int> seen;
        if (seen.insert(id).second) {
            const std::string line = "kinds: the squad code met object type " + std::to_string(id) + " (as " +
                                     std::to_string(like) + ")";
            jfg::jfg_log_line("jfg_mods.log", line.c_str());
        }
        ctx->r4 = (gpr)like;
    }
    BaddyTypeToIndex_orig(rdram, ctx);
}

JFG_WRAP(func_800050A8_5CA8) {
    const int like = jfg::baddy_like((int)(int32_t)ctx->r4);
    if (like >= 0) ctx->r4 = (gpr)like;
    func_800050A8_5CA8_orig(rdram, ctx);
}
