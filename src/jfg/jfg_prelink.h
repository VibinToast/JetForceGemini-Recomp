// Jet Force Gemini port: pre-linking the game's overlays at private addresses (the game's runLink.c, with no other
// overlay loaded), on a copy of the ROM.
//
// The build recompiles overlays from such a copy (tools/prepare_overlays.py), and mods' hooks regenerate functions
// from one too (jfg_prelink_rom, jfg_mods.cpp), so the two must be byte for byte the same. tools/prelink_check.cpp,
// run by both build scripts, checks it. Only the standard library here: the check is built without the game.
#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace jfg::prelink {

constexpr uint32_t LINK_BASE = 0x82000000; // the first overlay's private address; must match prepare_overlays.py

// An overlay as the ROM's overlay table describes it, with its private address (link).
struct Overlay {
    uint32_t num, rom, text, data, bss, rel1, rel2, link;
};

// The overlays, in the order of the ROM's table.
std::vector<Overlay> overlays(std::span<const uint8_t> rom);

// rom with every overlay pre-linked, in out. Returns the number of overlays linked, or -1 if the ROM doesn't look like
// the US ROM (out is then a copy of rom).
int link(std::span<const uint8_t> rom, std::vector<uint8_t>& out);

} // namespace jfg::prelink
