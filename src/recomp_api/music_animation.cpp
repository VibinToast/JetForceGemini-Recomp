#include <bit>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "recomp.h"
#include "ultramodern/ultra64.h"

namespace {

// US 1.1 RDRAM offsets. Words are directly addressable in N64Recomp's
// word-swapped RDRAM; halfwords and bytes retain the usual XOR adjustment.
constexpr uint32_t AudioPrevCount = 0x000DCBBC;
constexpr uint32_t MusicTempo = 0x001162B0;
constexpr uint32_t MusicAnimationTick = 0x001162B4;
constexpr uint32_t MusicPlaying = 0x001162C0;
constexpr uint32_t CurrentMenuId = 0x000DF9F0;
constexpr uint32_t SpTaskNum = 0x00123A68;
constexpr uint32_t GameMode = 0x00123A6C;

constexpr int32_t GameModeMenu = 1;
constexpr int32_t MenuCharacterSelect = 3;
constexpr float CounterTicksPerMillisecond = 46875.0f;

uint32_t load_u32(const uint8_t* rdram, uint32_t offset) {
    return *reinterpret_cast<const uint32_t*>(rdram + offset);
}

void store_u32(uint8_t* rdram, uint32_t offset, uint32_t value) {
    *reinterpret_cast<uint32_t*>(rdram + offset) = value;
}

int16_t load_s16(const uint8_t* rdram, uint32_t offset) {
    return *reinterpret_cast<const int16_t*>(rdram + (offset ^ 2U));
}

void store_s16(uint8_t* rdram, uint32_t offset, int16_t value) {
    *reinterpret_cast<int16_t*>(rdram + (offset ^ 2U)) = value;
}

uint8_t load_u8(const uint8_t* rdram, uint32_t offset) {
    return rdram[offset ^ 3U];
}

float load_float(const uint8_t* rdram, uint32_t offset) {
    return std::bit_cast<float>(load_u32(rdram, offset));
}

void store_float(uint8_t* rdram, uint32_t offset, float value) {
    store_u32(rdram, offset, std::bit_cast<uint32_t>(value));
}

struct CharacterSelectPhaseCache {
    uint8_t* rdram = nullptr;
    uint32_t spTaskNum = UINT32_MAX;
    float fraction = 0.0f;
    bool valid = false;
    uint64_t cacheHits = 0;
    std::chrono::steady_clock::time_point computedAt{};
    std::chrono::steady_clock::time_point lastReport{};
};

CharacterSelectPhaseCache phaseCache;

} // namespace

// DKR's retail helper mutates audioPrevCount and gMusicAnimationTick on every
// query. Character Select calls it once for each dancer during one object
// update pass. That is harmless on the N64, where the calls are effectively
// simultaneous, but an optimized native renderer can spread them across a
// meaningful part of the frame and give each model a different pose.
//
// Advance the retail clock once on the first query of a Character Select game
// frame, then return the identical phase to the remaining dancers. gSPTaskNum
// flips at the start of each normal game frame and is stable throughout the
// object pass. The age guard prevents a stale hit if a skipped frame leaves the
// two-state token unchanged. All non-Character-Select callers retain one clock
// advance per call.
extern "C" void music_animation_fraction(uint8_t* rdram, recomp_context* ctx) {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::now();

    const bool characterSelect =
        static_cast<int32_t>(load_u32(rdram, GameMode)) == GameModeMenu &&
        static_cast<int32_t>(load_u32(rdram, CurrentMenuId)) == MenuCharacterSelect;
    const uint32_t spTaskNum = load_u32(rdram, SpTaskNum);
    const bool sameCharacterSelectFrame =
        characterSelect && phaseCache.valid && phaseCache.rdram == rdram &&
        phaseCache.spTaskNum == spTaskNum &&
        (now - phaseCache.computedAt) < std::chrono::milliseconds(50);

    if (sameCharacterSelectFrame) {
        phaseCache.cacheHits++;
        ctx->f0.fl = phaseCache.fraction;
        return;
    }

    const uint32_t count = osGetCount();
    const uint32_t previousCount = load_u32(rdram, AudioPrevCount);
    // Unsigned subtraction is the exact modulo-2^32 counter delta. It handles
    // equality as zero and a genuine COUNT rollover without a special branch.
    const uint32_t delta = count - previousCount;

    float tick = load_float(rdram, MusicAnimationTick);
    tick += static_cast<float>(delta) / CounterTicksPerMillisecond;

    int16_t tempo = load_s16(rdram, MusicTempo);
    if (load_u8(rdram, MusicPlaying) == 0) {
        tempo = 182;
        store_s16(rdram, MusicTempo, tempo);
    }
    // A sequence normally resolves its tempo before this helper is used. Keep
    // malformed or transitional state finite instead of dividing by zero.
    if (tempo <= 0) {
        tempo = 182;
    }

    const float period = 120000.0f / static_cast<float>(tempo);
    while (period < tick) {
        tick -= period;
    }

    const float fraction = tick / period;
    store_float(rdram, MusicAnimationTick, tick);
    store_u32(rdram, AudioPrevCount, count);

    if (characterSelect) {
        if (phaseCache.rdram != rdram) {
            phaseCache.lastReport = now;
            phaseCache.cacheHits = 0;
        }
        phaseCache.rdram = rdram;
        phaseCache.spTaskNum = spTaskNum;
        phaseCache.fraction = fraction;
        phaseCache.valid = true;
        phaseCache.computedAt = now;

        static const bool trace = std::getenv("DKR_MUSIC_ANIM_TRACE") != nullptr;
        if (trace && (now - phaseCache.lastReport) >= std::chrono::seconds(1)) {
            std::fprintf(stderr,
                "[MUSIC ANIM] task=%u delta=%u tempo=%d tick=%.3f phase=%.6f cached=%llu\n",
                spTaskNum, delta, static_cast<int>(tempo), tick, fraction,
                static_cast<unsigned long long>(phaseCache.cacheHits));
            phaseCache.cacheHits = 0;
            phaseCache.lastReport = now;
        }
    }
    else {
        phaseCache.valid = false;
    }

    ctx->f0.fl = fraction;
}
