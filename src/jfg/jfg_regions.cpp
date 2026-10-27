// Jet Force Gemini port: the names of mods' regions (jfg_assets.json "regions", numbered 31-39 by the content linker in
// jfg_assets.cpp).
//
// The game names region r with front_text[0x197 + r] and world w with frontGetWorldName(w) (front_text by a table of
// the 15 worlds that have names). Both are read in two places: the area title shown on entering a region
// (frontDisplayRegion: region, world, continues) and the pause menu's Tribal screen (tribalScreen). For the length of
// those two calls a mod region's front_text entry points at its name instead (31-39 index the game's other menu text,
// which neither call prints), and frontGetWorldName of a world without a name (15 and up) gives the world text of the
// region being played. The strings live in the runtime's heap, outside the game's memory.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "recomp.h"
#include "jfg_game.h"
#include "jfg_hooks.h"

namespace recomp { void* alloc(uint8_t* rdram, size_t size); }
namespace jfg { bool mod_region(int region, std::string* name, std::string* world); }

namespace {

using namespace jfg;

constexpr uint32_t ADDR_front_text = 0x800A51C8;     // char**, NULL until initFront
constexpr uint32_t ADDR_currentRegion = 0x800FD7A4;  // s16: the region being played (0 / 0xFF: none)
constexpr int REGION_NAMES = 0x197;                  // front_text index of region 0's name
constexpr int FIRST_MOD_REGION = 31, LAST_MOD_REGION = 39;
constexpr int NAMED_WORLDS = 15;                     // worlds 0-14 have names (frontGetWorldName)
constexpr size_t TEXT_ROOM = 0x40;

struct Texts {
    uint32_t name = 0, world = 0; // RDRAM, 0 = none
};
std::map<int, Texts> g_texts;

uint32_t put_string(uint8_t* rdram, const std::string& text) {
    void* block = recomp::alloc(rdram, TEXT_ROOM);
    if (block == nullptr) return 0;
    const uint32_t at = 0x80000000u + (uint32_t)((uint8_t*)block - rdram);
    const std::string s = text.substr(0, TEXT_ROOM - 1);
    for (size_t i = 0; i < TEXT_ROOM; i++) wr8(at + (uint32_t)i, i < s.size() ? (uint8_t)s[i] : 0);
    return at;
}

// The texts of a mod region, made on first use; nullptr when `region` isn't one.
const Texts* texts(uint8_t* rdram, int region) {
    if (region < FIRST_MOD_REGION || region > LAST_MOD_REGION) return nullptr;
    auto it = g_texts.find(region);
    if (it != g_texts.end()) return &it->second;
    std::string name, world;
    if (!mod_region(region, &name, &world)) return nullptr;
    Texts t;
    t.name = put_string(rdram, name);
    if (!world.empty()) t.world = put_string(rdram, world);
    return &g_texts.emplace(region, t).first->second;
}

// Points the mod regions' front_text entries at their names until it goes out of scope.
struct NamesShown {
    explicit NamesShown(uint8_t* rdram) {
        const uint32_t table = rd32(ADDR_front_text);
        if (!valid_ptr(table)) return;
        for (int r = FIRST_MOD_REGION; r <= LAST_MOD_REGION; r++) {
            const Texts* t = texts(rdram, r);
            if (t == nullptr || t->name == 0) continue;
            const uint32_t entry = table + 4 * (uint32_t)(REGION_NAMES + r);
            saved.push_back({ entry, rd32(entry) });
            wr32(entry, t->name);
        }
    }
    ~NamesShown() {
        for (auto it = saved.rbegin(); it != saved.rend(); ++it) wr32(it->first, it->second);
    }
    std::vector<std::pair<uint32_t, uint32_t>> saved;
};

} // namespace

JFG_WRAP(frontDisplayRegion) {
    NamesShown names(rdram);
    frontDisplayRegion_orig(rdram, ctx);
}

JFG_WRAP(tribalScreen) {
    NamesShown names(rdram);
    tribalScreen_orig(rdram, ctx);
}

JFG_WRAP(frontGetWorldName) {
    if ((int32_t)ctx->r4 >= NAMED_WORLDS) {
        const Texts* t = texts(rdram, (int16_t)rd16(ADDR_currentRegion));
        if (t != nullptr && t->world != 0) {
            ctx->r2 = (gpr)(int32_t)t->world;
            return;
        }
    }
    frontGetWorldName_orig(rdram, ctx);
}
