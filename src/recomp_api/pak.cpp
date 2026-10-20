#include <cstdint>

#include "recomp.h"
#include "librecomp/helpers.hpp"
#include "input/input.hpp"

namespace {
constexpr int32_t PfsErrNoPack = 1;
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    const gpr pattern = _arg<1, PTR(u8)>(rdram, ctx);
    // JFG PORT: report a pak in every port that has a controller. Jet Force Gemini only tries osMotorInit (the
    // Rumble Pak) for ports this reports as occupied; osPfsInit below still says there is no Controller Pak.
    uint8_t bits = 0;
    for (int i = 0; i < 4; i++) {
        const auto info = dino::input::get_connected_device_info(i);
        if (info.connected_device == ultramodern::input::Device::Controller && info.connected_pak != ultramodern::input::Pak::None) {
            bits |= uint8_t(1u << i);
        }
    }
    MEM_B(0, pattern) = int8_t(bits); // a single byte in big-endian RDRAM
    _return<int32_t>(ctx, 0);
}

extern "C" void osPfsInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osContRamRead_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osContRamWrite_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osGetId_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osPfsGetStatus_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osPfsSelectBank_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<int32_t>(ctx, PfsErrNoPack);
}

extern "C" void __osSiGetAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}

extern "C" void __osSiRelAccess_recomp(uint8_t* rdram, recomp_context* ctx) {
}

extern "C" void rmonPrintf_recomp(uint8_t* rdram, recomp_context* ctx) {
}
