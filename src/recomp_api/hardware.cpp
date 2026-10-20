#include <cstdint>

#include "recomp.h"

extern "C" void drm_validate_imem_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 1;
}

extern "C" void drm_validate_dmem_recomp(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = 1;
}

extern "C" void drm_checksum_balloon_recomp(uint8_t* rdram, recomp_context* ctx) {
    // The retail function checksums obj_loop_goldenballoon's code bytes and
    // sets gAntiPiracyHeadroll = 0x800 on mismatch, which visibly corrupts
    // every racer's head/tail vertex group while on a vehicle. The port
    // patches game code in RDRAM by design, so the retail sum cannot match.
    // The check is anti-tamper, not game logic; skip it entirely.
}
