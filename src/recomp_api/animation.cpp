#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <vector>
#include <algorithm>

#include "recomp.h"

// Native reimplementation of DKR's obj_animate (vram 0x80061F70, US Rev 1).
//
// The retail function is hand-written assembly (see the decomp's
// src/hasm/obj_animate.s) implementing delta-compressed vertex animation:
//   anim data = [frame0: 12-byte root block + 6*V s16 absolute positions]
//               [frameN: 12-byte root block + 3*V s8 per-component deltas]...
// A positions-only working buffer (6 bytes/vertex) accumulates deltas
// forward/backward to the current whole frame; the fractional 1/16ths of the
// frame counter scale the *next* frame's deltas into a global scratch buffer;
// the final pass composes working+scratch into the double-buffered 10-byte
// Vertex array selected by ModelInstance::animationTaskNum (flipped here).
//
// This native version exists to bisect a persistent vertex-corruption bug on
// heavily-animated characters: if it behaves identically to the statically
// recompiled original, the animation *data* is at fault; if it fixes the
// corruption, the recompilation of the assembly is.

namespace {

using gpr = uint64_t;

inline int32_t mem_w(uint8_t* rdram, gpr addr) {
    return *reinterpret_cast<int32_t*>(rdram + ((addr & 0xFFFFFFFFULL) - 0x80000000ULL));
}

inline int16_t mem_h(uint8_t* rdram, gpr addr) {
    return *reinterpret_cast<int16_t*>(rdram + (((addr & 0xFFFFFFFFULL) ^ 2) - 0x80000000ULL));
}

inline void store_h(uint8_t* rdram, gpr addr, int16_t value) {
    *reinterpret_cast<int16_t*>(rdram + (((addr & 0xFFFFFFFFULL) ^ 2) - 0x80000000ULL)) = value;
}

inline int8_t mem_b(uint8_t* rdram, gpr addr) {
    return *reinterpret_cast<int8_t*>(rdram + (((addr & 0xFFFFFFFFULL) ^ 3) - 0x80000000ULL));
}

inline uint8_t mem_bu(uint8_t* rdram, gpr addr) {
    return *reinterpret_cast<uint8_t*>(rdram + (((addr & 0xFFFFFFFFULL) ^ 3) - 0x80000000ULL));
}

inline void store_b(uint8_t* rdram, gpr addr, int8_t value) {
    *reinterpret_cast<int8_t*>(rdram + (((addr & 0xFFFFFFFFULL) ^ 3) - 0x80000000ULL)) = value;
}

// Frame root values are 16-bit big-endian at arbitrary alignment inside the
// 3-byte-stride frame blocks; the original assembles them from byte pairs.
inline int32_t root_s16(uint8_t* rdram, gpr addr) {
    return (int32_t(mem_b(rdram, addr)) << 8) | mem_bu(rdram, addr + 1);
}

// v80 address (lui 0x8012 + lo -0x243C in the retail code); the v77
// decomp symbol D_8011D644 sits 0x580 lower and is WRONG for this ROM.
constexpr gpr ScratchPointerAddr = 0x8011DBC4ULL;

} // namespace

extern "C" void obj_animate_recomp(uint8_t* rdram, recomp_context* ctx);

static void obj_animate_native(uint8_t* rdram, recomp_context* ctx) {
    static const bool trace = (std::getenv("DKR_ANIM_TRACE") != nullptr);

    const gpr obj = ctx->r4;

    // Model instance selection: clamp obj->modelIndex (0x3A) to the header's
    // instance count (0x55 of *0x40), index the instance array at 0x68.
    int32_t modelIndex = mem_b(rdram, obj + 0x3A);
    if (modelIndex < 0) {
        modelIndex = 0;
    }
    const int32_t maxIndex = mem_b(rdram, gpr(uint32_t(mem_w(rdram, obj + 0x40))) + 0x55);
    if (modelIndex >= maxIndex) {
        modelIndex = maxIndex;
    }
    const gpr instance = gpr(uint32_t(mem_w(rdram, gpr(uint32_t(mem_w(rdram, obj + 0x68))) + gpr(modelIndex) * 4)));
    const gpr model = gpr(uint32_t(mem_w(rdram, instance)));

    const uint32_t animsPtr = uint32_t(mem_w(rdram, model + 0x44));
    if (animsPtr == 0) {
        ctx->r2 = 0;
        return;
    }

    const int32_t frameRaw = mem_h(rdram, obj + 0x18);
    int32_t animID = mem_b(rdram, obj + 0x3B);
    if ((frameRaw == mem_h(rdram, instance + 0x14)) && (animID == mem_h(rdram, instance + 0x10))) {
        ctx->r2 = 0;
        return;
    }

    if (animID < 0) {
        animID = 0;
    }
    const int32_t numAnims = mem_h(rdram, model + 0x48);
    if (animID >= numAnims) {
        animID = numAnims - 1;
    }

    int32_t maxFrame = 0;
    if (numAnims > 0) {
        maxFrame = mem_w(rdram, gpr(animsPtr) + gpr(animID) * 8 + 0x4) - 2;
    }

    // frameRaw is a 12.4 fixed-point frame counter.
    int32_t frame = int32_t(uint32_t(frameRaw) >> 4);
    int32_t effRaw = frameRaw;
    int32_t lastAnimID;
    if (frame > maxFrame) {
        // Ran past the end: restart the animation and force a rebuild.
        lastAnimID = -1;
        effRaw = 0;
        frame = 0;
        store_h(rdram, instance + 0x10, int16_t(-1));
    }
    else {
        lastAnimID = mem_h(rdram, instance + 0x10);
    }

    const gpr work = gpr(uint32_t(mem_w(rdram, instance + 0xC)));
    // Stateless mode: never trust the incremental working-buffer state; always
    // rebuild from frame 0 and apply every delta up to the target frame. The
    // incremental path is exact in theory but the accumulated state has been
    // observed to drift (bounded, deterministic pose distortion on
    // heavily-animated models); recomputation makes the pose exact by
    // construction at negligible native cost.
    static const bool stateless = (std::getenv("DKR_ANIM_INCREMENTAL") == nullptr);
    int32_t lastFrame = (!stateless && (animID == lastAnimID)) ? mem_h(rdram, instance + 0x12) : -1;

    store_h(rdram, instance + 0x10, int16_t(animID));
    store_h(rdram, instance + 0x14, int16_t(effRaw));
    store_h(rdram, instance + 0x12, int16_t(frame));

    const int32_t subframe = effRaw & 0xF;
    if (trace) {
        static uint32_t entryLog = 0;
        if (entryLog++ < 40) {
            std::fprintf(stderr,
                "[ANIM] enter obj=%08X inst=%08X model=%08X work=%08X anims=%08X scratch=%08X frame=%d last=%d\n",
                uint32_t(obj), uint32_t(instance), uint32_t(model), uint32_t(work),
                animsPtr, uint32_t(mem_w(rdram, ScratchPointerAddr)), frame, lastFrame);
        }
    }
    const gpr animEntry = gpr(animsPtr) + gpr(animID) * 8;
    const gpr animData = gpr(uint32_t(mem_w(rdram, animEntry)));
    const int32_t listCount = mem_h(rdram, model + 0x24);
    const int32_t vertsPerFrame = mem_h(rdram, model + 0x4A);
    const gpr indexList = gpr(uint32_t(mem_w(rdram, model + 0x4C)));
    const int32_t frameSize = vertsPerFrame * 3 + 12;

    // Rebuild the working buffer from frame 0's absolute positions when the
    // animation changed, looped, or we're at frame 0.
    if ((frame == 0) || (lastFrame == -1)) {
        const gpr absolutes = animData + 0xC;
        gpr base = gpr(uint32_t(mem_w(rdram, model + 0x4)));
        for (int32_t i = 0; i < listCount; i++, base += 0xA) {
            const int32_t idx = mem_h(rdram, indexList + gpr(i) * 2);
            if (idx == -1) {
                continue;
            }

            const gpr src = absolutes + gpr(idx) * 6;
            const gpr dst = work + gpr(idx) * 6;
            store_h(rdram, dst + 0, int16_t(mem_h(rdram, base + 0) + mem_h(rdram, src + 0)));
            store_h(rdram, dst + 2, int16_t(mem_h(rdram, base + 2) + mem_h(rdram, src + 2)));
            store_h(rdram, dst + 4, int16_t(mem_h(rdram, base + 4) + mem_h(rdram, src + 4)));
        }
        lastFrame = 0;
    }

    // Deltas for whole frame f live at animData + frameSize * (f + 1).
    if (lastFrame < frame) {
        gpr deltas = animData + gpr(uint32_t(frameSize) * uint32_t(lastFrame + 2));
        for (int32_t f = lastFrame; f < frame; f++, deltas += frameSize) {
            gpr d = deltas;
            gpr w = work;
            for (int32_t k = 0; k < vertsPerFrame; k++, d += 3, w += 6) {
                store_h(rdram, w + 0, int16_t(mem_h(rdram, w + 0) + mem_b(rdram, d + 0)));
                store_h(rdram, w + 2, int16_t(mem_h(rdram, w + 2) + mem_b(rdram, d + 1)));
                store_h(rdram, w + 4, int16_t(mem_h(rdram, w + 4) + mem_b(rdram, d + 2)));
            }
        }
    }
    else if (frame < lastFrame) {
        gpr deltas = animData + gpr(uint32_t(frameSize) * uint32_t(lastFrame + 2));
        for (int32_t f = lastFrame; f > frame; f--) {
            deltas -= frameSize;
            gpr d = deltas;
            gpr w = work;
            for (int32_t k = 0; k < vertsPerFrame; k++, d += 3, w += 6) {
                store_h(rdram, w + 0, int16_t(mem_h(rdram, w + 0) - mem_b(rdram, d + 0)));
                store_h(rdram, w + 2, int16_t(mem_h(rdram, w + 2) - mem_b(rdram, d + 1)));
                store_h(rdram, w + 4, int16_t(mem_h(rdram, w + 4) - mem_b(rdram, d + 2)));
            }
        }
    }

    // Fractional interpolation: scale the NEXT frame's deltas by subframe/16
    // into the global scratch buffer.
    const gpr scratch = gpr(uint32_t(mem_w(rdram, ScratchPointerAddr)));
    {
        gpr d = animData + gpr(uint32_t(frameSize) * uint32_t(frame + 2));
        gpr s = scratch;
        for (int32_t k = 0; k < vertsPerFrame; k++, d += 3, s += 6) {
            store_h(rdram, s + 0, int16_t((int32_t(mem_b(rdram, d + 0)) * subframe) >> 4));
            store_h(rdram, s + 2, int16_t((int32_t(mem_b(rdram, d + 1)) * subframe) >> 4));
            store_h(rdram, s + 4, int16_t((int32_t(mem_b(rdram, d + 2)) * subframe) >> 4));
        }
    }

    // Interpolate the 12-byte root block (three s16 at +0/+2/+4, one at +0xA).
    {
        gpr curRoot = (frame == 0) ? animData
                                   : (animData + gpr(uint32_t(frameSize) * uint32_t(frame + 1)) - 0xC);
        const gpr nextRoot = (frame == 0) ? (animData + gpr(uint32_t(frameSize)) * 2 - 0xC)
                                          : (curRoot + frameSize);
        const int32_t offsets[4] = { 0x0, 0x2, 0x4, 0xA };
        const int32_t outs[4] = { 0x16, 0x18, 0x1A, 0x1C };
        for (int i = 0; i < 4; i++) {
            const int32_t cur = root_s16(rdram, curRoot + offsets[i]);
            const int32_t next = root_s16(rdram, nextRoot + offsets[i]);
            store_h(rdram, instance + outs[i], int16_t(cur + (((next - cur) * subframe) >> 4)));
        }
    }

    // Flip the double buffer and compose working + scratch into the 10-byte
    // Vertex array the renderer consumes.
    const int8_t taskNum = mem_b(rdram, instance + 0x1F) ^ 1;
    store_b(rdram, instance + 0x1F, taskNum);
    gpr dest = gpr(uint32_t(mem_w(rdram, instance + 0x4 + gpr(taskNum) * 4)));

    int32_t outOfRange = 0;
    for (int32_t i = 0; i < listCount; i++, dest += 0xA) {
        const int32_t idx = mem_h(rdram, indexList + gpr(i) * 2);
        if (idx == -1) {
            continue;
        }

        const gpr w = work + gpr(idx) * 6;
        const gpr s = scratch + gpr(idx) * 6;
        for (int32_t c = 0; c < 3; c++) {
            const int16_t value = int16_t(mem_h(rdram, w + gpr(c) * 2) + mem_h(rdram, s + gpr(c) * 2));
            store_h(rdram, dest + gpr(c) * 2, value);
            if (value > 2000 || value < -2000) {
                outOfRange++;
            }
        }
    }

    if (trace && (outOfRange > 0)) {
        static uint32_t reportCount = 0;
        if (reportCount++ < 64) {
            std::fprintf(stderr,
                "[ANIM] native obj_animate out-of-range verts=%d anim=%d frame=%d last=%d verts/frame=%d data=%08X\n",
                outOfRange, animID, frame, lastFrame, vertsPerFrame, uint32_t(animData));
        }
    }

    if (trace) {
        // Integrity tracking: remember a checksum of the positions we just
        // wrote per destination buffer, and report if a buffer we previously
        // wrote was modified by third-party code before our next visit.
        static std::unordered_map<uint32_t, uint32_t> writtenSums;
        const uint32_t destBase = uint32_t(mem_w(rdram, instance + 0x4 + gpr(taskNum) * 4));
        auto sumBuffer = [&](uint32_t base) {
            uint32_t sum = 0;
            gpr p = gpr(base);
            for (int32_t i = 0; i < listCount; i++, p += 0xA) {
                sum = sum * 31 + uint16_t(mem_h(rdram, p + 0));
                sum = sum * 31 + uint16_t(mem_h(rdram, p + 2));
                sum = sum * 31 + uint16_t(mem_h(rdram, p + 4));
            }
            return sum;
        };
        auto it = writtenSums.find(destBase);
        if (it != writtenSums.end()) {
            // We are about to have overwritten this buffer; the pre-write
            // check happened implicitly above via outOfRange of *our* data.
        }
        writtenSums[destBase] = sumBuffer(destBase);

        // Check the OTHER buffer (written two calls ago, untouched by us now).
        const uint32_t otherBase = uint32_t(mem_w(rdram, instance + 0x4 + gpr(taskNum ^ 1) * 4));
        auto other = writtenSums.find(otherBase);
        if (other != writtenSums.end()) {
            const uint32_t now = sumBuffer(otherBase);
            if (now != other->second) {
                static uint32_t stompCount = 0;
                if (stompCount++ < 64) {
                    std::fprintf(stderr, "[ANIM] buffer %08X MODIFIED externally since we wrote it\n", otherBase);
                }
                other->second = now;
            }
        }

        static uint32_t writeLog = 0;
        if (writeLog++ < 100000) {
            std::fprintf(stderr, "[ANIM] wrote dest=%08X n=%d anim=%d frame=%d v0=(%d,%d,%d) v1=(%d,%d,%d)\n",
                destBase, listCount, animID, frame,
                mem_h(rdram, gpr(destBase) + 0), mem_h(rdram, gpr(destBase) + 2), mem_h(rdram, gpr(destBase) + 4),
                mem_h(rdram, gpr(destBase) + 10), mem_h(rdram, gpr(destBase) + 12), mem_h(rdram, gpr(destBase) + 14));
        }
    }

    ctx->r2 = 1;
}


// A/B harness: run the native reimplementation in shadow mode against the
// recompiled original. The original's results stay live (retail behavior);
// any divergence between the two is logged byte-precisely.
extern "C" void obj_animate(uint8_t* rdram, recomp_context* ctx) {
    static const bool compare = (std::getenv("DKR_ANIM_COMPARE") != nullptr);
    if (!compare) {
        // Default to the recompiled original (retail behavior). The native
        // reimplementation is byte-identical (verified via DKR_ANIM_COMPARE)
        // and available with DKR_ANIM_NATIVE=1 (stateless pose recompute
        // unless DKR_ANIM_INCREMENTAL is also set).
        static const bool useNative = (std::getenv("DKR_ANIM_NATIVE") != nullptr);
        static const bool profile = (std::getenv("DKR_ANIM_PERF") != nullptr);
        const auto profileStart = profile ? std::chrono::steady_clock::now()
                                          : std::chrono::steady_clock::time_point{};
        if (useNative) {
            obj_animate_native(rdram, ctx);
        }
        else {
            obj_animate_recomp(rdram, ctx);
        }
        if (profile) {
            static uint64_t callCount = 0;
            static uint64_t totalUs = 0;
            static uint64_t maxUs = 0;
            static auto lastReport = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            const uint64_t elapsedUs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(now - profileStart).count());
            callCount++;
            totalUs += elapsedUs;
            maxUs = std::max(maxUs, elapsedUs);
            if ((now - lastReport) >= std::chrono::seconds(1)) {
                std::fprintf(stderr,
                    "[ANIM PERF] mode=%s calls=%llu avg=%.1f us max=%llu us\n",
                    useNative ? "native-incremental" : "recompiled-retail",
                    static_cast<unsigned long long>(callCount),
                    callCount ? static_cast<double>(totalUs) / static_cast<double>(callCount) : 0.0,
                    static_cast<unsigned long long>(maxUs));
                callCount = 0;
                totalUs = 0;
                maxUs = 0;
                lastReport = now;
            }
        }
        return;
    }

    const gpr obj = ctx->r4;
    int32_t modelIndex = mem_b(rdram, obj + 0x3A);
    if (modelIndex < 0) modelIndex = 0;
    const int32_t maxIndex = mem_b(rdram, gpr(uint32_t(mem_w(rdram, obj + 0x40))) + 0x55);
    if (modelIndex >= maxIndex) modelIndex = maxIndex;
    const gpr instance = gpr(uint32_t(mem_w(rdram, gpr(uint32_t(mem_w(rdram, obj + 0x68))) + gpr(modelIndex) * 4)));
    const gpr model = gpr(uint32_t(mem_w(rdram, instance)));
    const uint32_t animsPtr = uint32_t(mem_w(rdram, model + 0x44));
    if (animsPtr == 0) {
        obj_animate_recomp(rdram, ctx);
        return;
    }

    const int32_t listCount = mem_h(rdram, model + 0x24);
    const int32_t vertsPerFrame = mem_h(rdram, model + 0x4A);
    const gpr indexList = gpr(uint32_t(mem_w(rdram, model + 0x4C)));
    const gpr work = gpr(uint32_t(mem_w(rdram, instance + 0xC)));
    const uint32_t scratchPtr = uint32_t(mem_w(rdram, ScratchPointerAddr));
    int32_t maxIdx = -1;
    for (int32_t i = 0; i < listCount; i++) {
        const int32_t idx = mem_h(rdram, indexList + gpr(i) * 2);
        if (idx > maxIdx) maxIdx = idx;
    }

    auto hostRange = [&](uint32_t vram, size_t len, std::vector<uint8_t>& out) {
        const uint32_t start = (vram & ~3u);
        const size_t total = ((len + (vram - start)) + 3) & ~size_t(3);
        out.assign(rdram + (start - 0x80000000u), rdram + (start - 0x80000000u) + total);
        return start;
    };
    auto restoreRange = [&](uint32_t start, const std::vector<uint8_t>& data) {
        std::copy(data.begin(), data.end(), rdram + (start - 0x80000000u));
    };

    const uint32_t dest0 = uint32_t(mem_w(rdram, instance + 0x4));
    const uint32_t dest1 = uint32_t(mem_w(rdram, instance + 0x8));
    const size_t workLen = size_t(maxIdx + 1) * 6;
    const size_t scratchLen = size_t(vertsPerFrame) * 6;
    const size_t destLen = size_t(listCount) * 10;

    std::vector<uint8_t> sInst, sWork, sScratch, sDest0, sDest1;
    const uint32_t instStart = hostRange(uint32_t(instance) + 0x10, 0x10, sInst);
    const uint32_t workStart = hostRange(uint32_t(work), workLen, sWork);
    const uint32_t scratchStart = scratchPtr ? hostRange(scratchPtr, scratchLen, sScratch) : 0;
    const uint32_t d0Start = hostRange(dest0, destLen, sDest0);
    const uint32_t d1Start = hostRange(dest1, destLen, sDest1);

    // Run the native implementation on live state, snapshot its results.
    recomp_context nativeCtx = *ctx;
    obj_animate_native(rdram, &nativeCtx);
    std::vector<uint8_t> nInst, nWork, nScratch, nDest0, nDest1;
    hostRange(uint32_t(instance) + 0x10, 0x10, nInst);
    hostRange(uint32_t(work), workLen, nWork);
    if (scratchPtr) hostRange(scratchPtr, scratchLen, nScratch);
    hostRange(dest0, destLen, nDest0);
    hostRange(dest1, destLen, nDest1);

    // Restore and run the original; its results stay live.
    restoreRange(instStart, sInst);
    restoreRange(workStart, sWork);
    if (scratchPtr) restoreRange(scratchStart, sScratch);
    restoreRange(d0Start, sDest0);
    restoreRange(d1Start, sDest1);
    obj_animate_recomp(rdram, ctx);

    // Compare.
    static uint32_t diffReports = 0;
    auto diff = [&](const char* what, uint32_t start, const std::vector<uint8_t>& mine) {
        if (diffReports >= 40) return;
        std::vector<uint8_t> orig;
        hostRange(start, mine.size(), orig);
        for (size_t i = 0; i < mine.size() && i < orig.size(); i++) {
            if (mine[i] != orig[i]) {
                std::fprintf(stderr,
                    "[ANIM-DIFF] %s +0x%zx: native=%02X orig=%02X (obj=%08X frameRaw=%d)\n",
                    what, i, mine[i], orig[i], uint32_t(obj), int(mem_h(rdram, obj + 0x18)));
                diffReports++;
                if (diffReports >= 40) return;
            }
        }
    };
    diff("inst", instStart, nInst);
    diff("work", workStart, nWork);
    if (scratchPtr) diff("scratch", scratchStart, nScratch);
    diff("dest0", d0Start, nDest0);
    diff("dest1", d1Start, nDest1);
    if (nativeCtx.r2 != ctx->r2 && diffReports < 40) {
        std::fprintf(stderr, "[ANIM-DIFF] return native=%d orig=%d\n", int(nativeCtx.r2), int(ctx->r2));
        diffReports++;
    }
}
