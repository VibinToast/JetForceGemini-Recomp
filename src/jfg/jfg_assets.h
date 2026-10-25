// Jet Force Gemini port: the game's data assets (jfg_assets.cpp).
//
// Two layers, both under the game's asset table (src/pi.c in the decomp: 71 assets read through piRomLoad,
// piRomLoadSection and piRomGetFileSize):
//   - Boot assets: asset mods (jfg_assets.json), laid into the ROM once before the game starts.
//   - Runtime assets: a port feature serves its own version of an asset while the game runs, and can change or drop
//     it at any time; the loaders' wrappers read it instead of the ROM. Each runtime asset has one owner, so two
//     features can't fight over an asset unnoticed: a second owner is refused and logged. The locale system
//     (jfg_locale.cpp) is the first user.
// Runtime assets are read from the game thread; set and clear them there too (a tick, a wrapper).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace jfg::assets {

constexpr int COUNT = 71; // assets in the US ROM's table

// The asset as the game reads it now: the runtime version when there is one, else the ROM's (boot assets applied).
std::span<const uint8_t> read(int asset);
// The ROM's version, ignoring runtime assets.
std::span<const uint8_t> read_rom(int asset);
// Serves `data` for `asset` from now on. False (and nothing changed) when another owner holds it.
bool set_runtime(int asset, std::vector<uint8_t> data, const std::string& owner);
// Drops owner's runtime version of `asset`; the game reads the ROM's again.
void clear_runtime(int asset, const std::string& owner);

} // namespace jfg::assets
