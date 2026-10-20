#include <cstdint>

#include "recomp.h"
#include "librecomp/helpers.hpp"

namespace {
constexpr int32_t PfsErrNoPack = 1;
}

extern "C" void osPfsIsPlug_recomp(uint8_t* rdram, recomp_context* ctx) {
    auto* pattern = _arg<1, uint8_t*>(rdram, ctx);
    *pattern = 0;
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
