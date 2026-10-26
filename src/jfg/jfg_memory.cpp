// Jet Force Gemini port: more memory for levels than the N64 had (decomp PORT_FINDINGS request 15).
//
// - The main heap: the game ends it at 0x80400000 (mmExtendedRam is 0 in the ROM, so the expansion pak's second
//   4 MB went unused). It now ends at 0x80800000, twice the room. Past 0x80800000 is not possible: the RSP, RDP and
//   VI take 24-bit addresses, and the port's gore blocks start at 0x80C00000.
// - The track buffer: one heap block of 0x9F000 bytes (func_overlay_24_000001F8) that the level's track unpacks into,
//   followed by tables of about 44 bytes per triangle, with no check. A pack level bigger than the game's own wrote
//   past it into the heap and the game crashed. It is asked for at TRACK_BUFFER bytes instead; the loader shrinks the
//   block to what the track uses afterwards, so a stock level ends up the same. Only the tables gain the room: the
//   loader still reads the packed track to 0x9F000 - its size and unpacks in place, so the unpacked and packed
//   track together must still fit 0x9F000 (the constant at o24 +0x378/+0x380; not raised yet).
#include <cstdint>

#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
extern uint8_t* g_rdram; // jfg_game.cpp: what rd32 / wr32 read and write; every hook sets it
}

constexpr uint32_t ADDR_mmEndRam = 0x800FDDA8;  // s32: where the main heap ends (memory.c)
constexpr uint32_t HEAP_END = 0x80800000;       // the end of the expansion pak's 8 MB
constexpr uint32_t STOCK_TRACK_BUFFER = 0x9F000;
constexpr uint32_t TRACK_BUFFER = 0x140000;

// mempool_init(slots, size, slot count): when mmInit makes the main pool (it ends at mmEndRam), make it reach
// HEAP_END.
JFG_WRAP(mempool_init) {
    jfg::g_rdram = rdram;
    const uint32_t start = (uint32_t)ctx->r4, size = (uint32_t)ctx->r5;
    const uint32_t end = (uint32_t)jfg::rd32(ADDR_mmEndRam);
    if (start + size == end && end < HEAP_END) {
        ctx->r5 = (gpr)(int32_t)(HEAP_END - start);
        jfg::wr32(ADDR_mmEndRam, HEAP_END);
        static bool logged = false;
        if (!logged) {
            logged = true;
            jfg::jfg_log_line("jfg_memory.log", "memory: the main heap ends at 0x80800000 (the game's own end: "
                                                "0x80400000), twice the room for levels and their objects");
        }
    }
    mempool_init_orig(rdram, ctx);
}

// The size mmAlloc is asked for, raised for the track buffer (jfg_diagnostics.cpp's mmAlloc wrapper calls this).
extern "C" void jfg_memory_alloc_size(recomp_context* ctx) {
    if ((uint32_t)ctx->r4 == STOCK_TRACK_BUFFER) ctx->r4 = (gpr)(int32_t)TRACK_BUFFER;
}
