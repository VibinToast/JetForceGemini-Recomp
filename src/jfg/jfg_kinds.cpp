// Jet Force Gemini port: mods' new object types ("objtypes" in jfg_assets.json, jfg_assets.cpp) in the squad code.
//
// A pack can add an enemy made from a stock one: a copy of its definition pointing at a copy of its model with the
// pack's own textures, under a new object id. The squad code picks its per-type data (the baddy row, animation sets,
// sounds) through BaddyTypeToIndex, a switch on the stock object ids; for a mod's id it is asked about the stock id the
// type was made from, so the new enemy moves, fights and dies like that one.
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
