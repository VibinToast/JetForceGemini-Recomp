// Jet Force Gemini port: the CPU-drawn rain and reticles with in-between frames.
//
// Rain and most weapons' reticles (and their tiny "LOCK" text and numbers) are not drawn by the RDP: rainDraw and
// fxOutputLines add them straight into the finished framebuffer in RDRAM (decomp docs/PORT_FINDINGS.md, "CPU line
// drawing"). RT64 only notices such pixels when the next frame's display list arrives (checkRDRAM), and copies them
// into that one framebuffer's picture. With Smooth Motion the frames on screen are mostly in-between frames made from
// draw calls, so the reticles and the rain were missing.
//
// While RT64 adds in-between frames, the pixels these effects add are taken out of RDRAM again (so RT64 never sees
// them) and kept as a layer for the framebuffer they were drawn into. The render hook (src/debug_ui/backend.cpp) draws
// that layer on every frame presented for that framebuffer. The reticles' pixels are moved between the previous
// layer's place and this one's by the frame's weight, the way RT64 moves the scene; rain, new every frame, is not.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_rt64.h"

extern "C" int jfg_renderer_adds_frames(void); // src/renderer/renderer.cpp

namespace jfg {
namespace {

struct LayerPixel { uint16_t x, y; uint8_t r, g, b; bool still; }; // still: drawn where it is, never moved (rain)
struct Layer {
    uint32_t fb = 0;        // physical address, as RT64's VI reports it
    uint32_t seq = 0;       // 0 = empty slot
    uint32_t width = 0, height = 0;
    float cx = 0, cy = 0;   // centre of the lit pixels that move with blending (the reticle's lines and text)
    uint32_t moving = 0;    // how many of those
    std::vector<LayerPixel> pixels;
};

std::mutex g_layer_mutex;
// The most recent layers, one per framebuffer the game draws into: two buffers normally, three in the levels whose blur
// effect asks for triple buffering (viSetTrippleBuffer(levelGetBlurEffect(level)); seen in level 181, the slug boss
// tunnel: 0x10FF00, 0x135700, 0x15AF40). With only
// two kept, a frame presented a little later than the newest two (Smooth Motion above the game's rate) found no layer
// for its buffer: the reticle was missing or blinked.
constexpr int LAYER_SLOTS = 4;
Layer g_layers[LAYER_SLOTS];
uint32_t g_layer_seq = 0;

// JFG_DEBUG_CPU_LAYER=1: once a second, which framebuffers layers were captured from (and how many pixels) and which
// the presented frames showed, found or not (the reticle missing when a presented buffer has no layer).
const bool g_trace = std::getenv("JFG_DEBUG_CPU_LAYER") != nullptr;
struct TraceCount { uint32_t fb = 0, count = 0, pixels = 0; };
TraceCount g_trace_published[4], g_trace_found[4], g_trace_missing[4];
uint32_t g_trace_frames = 0;
void trace_add(TraceCount (&list)[4], uint32_t fb, uint32_t pixels) {
    for (TraceCount& t : list) {
        if (t.count == 0 || t.fb == fb) { t.fb = fb; t.count++; t.pixels += pixels; return; }
    }
}
std::string trace_text(const TraceCount (&list)[4]) {
    std::string out;
    char buf[64];
    for (const TraceCount& t : list) {
        if (t.count == 0) continue;
        std::snprintf(buf, sizeof(buf), " %06X x%u (%u px)", t.fb, t.count, t.pixels);
        out += buf;
    }
    return out.empty() ? std::string(" -") : out;
}

// Game thread only. mainCPUeffects draws rain (rainDraw) and then the lines (fxOutputLines); each is captured on its
// own (refractOutput, between them, moves pixels instead of adding them and is left as it is) into one pending layer,
// published once the lines are done.
bool g_capturing = false;
uint32_t g_capture_screen = 0, g_capture_width = 0, g_capture_height = 0;
std::vector<uint8_t> g_before;
Layer g_pending;

inline uint16_t pixel_at(const uint8_t* raw, uint32_t offset) {
    uint16_t v;
    std::memcpy(&v, raw + (offset ^ 2), 2); // halves are swizzled inside host-endian words
    return v;
}

// Keeps the framebuffer as it is before a CPU effect draws into it.
void capture_begin(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    g_capturing = false;
    if (!jfg_renderer_adds_frames()) return;
    const uint32_t screen = rd32(ADDR_currentScreen);
    if (!valid_ptr(screen)) return;
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // inside the gap call_game leaves below $sp
    call_game(viGetCurrentSize, ctx, { buf, buf + 4 });
    const uint32_t width = rd32(buf), height = rd32(buf + 4);
    if (width == 0 || width > 1024 || height == 0 || height > 1024) return;
    const uint32_t bytes = width * height * 2;
    if (!valid_ptr(screen + bytes - 1)) return;
    g_before.resize(bytes);
    std::memcpy(g_before.data(), rdram + (screen - 0x80000000u), bytes);
    g_capture_screen = screen;
    g_capture_width = width;
    g_capture_height = height;
    g_capturing = true;
}

// What the effect added goes into the pending layer, and RDRAM gets the frame back (so RT64 does not copy it in a
// frame late as well).
void capture_end(uint8_t* rdram, bool still) {
    if (!g_capturing) return;
    g_capturing = false;
    uint8_t* raw = rdram + (g_capture_screen - 0x80000000u);
    const uint32_t bytes = g_capture_width * g_capture_height * 2;
    const uint32_t fb = g_capture_screen & 0x1FFFFFFFu;
    if (g_pending.fb != fb || g_pending.width != g_capture_width || g_pending.height != g_capture_height) {
        g_pending = Layer();
        g_pending.fb = fb;
        g_pending.width = g_capture_width;
        g_pending.height = g_capture_height;
    }
    for (uint32_t word = 0; word < bytes; word += 4) {
        uint32_t a, b;
        std::memcpy(&a, raw + word, 4);
        std::memcpy(&b, g_before.data() + word, 4);
        if (a == b) continue;
        for (uint32_t half = 0; half < 4; half += 2) {
            const uint32_t offset = word + half;
            const uint16_t now = pixel_at(raw, offset), was = pixel_at(g_before.data(), offset);
            if (now == was) continue;
            // RGBA5551: the amounts the effect added (fxOutputLines only adds, with saturation; a few brushes also
            // subtract, those are left out; rain lightens).
            const int dr = std::max(0, ((now >> 11) & 31) - ((was >> 11) & 31));
            const int dg = std::max(0, ((now >> 6) & 31) - ((was >> 6) & 31));
            const int db = std::max(0, ((now >> 1) & 31) - ((was >> 1) & 31));
            if (dr == 0 && dg == 0 && db == 0) continue;
            const uint32_t index = offset / 2;
            const uint16_t x = (uint16_t)(index % g_capture_width), y = (uint16_t)(index / g_capture_width);
            g_pending.pixels.push_back({ x, y, (uint8_t)dr, (uint8_t)dg, (uint8_t)db, still });
            if (!still) {
                g_pending.cx += x;
                g_pending.cy += y;
                g_pending.moving++;
            }
        }
    }
    std::memcpy(raw, g_before.data(), bytes);
}

// The pending layer becomes the layer of its framebuffer.
void publish() {
    if (g_pending.fb == 0) return;
    Layer layer = std::move(g_pending);
    g_pending = Layer();
    if (layer.moving > 0) {
        layer.cx /= (float)layer.moving;
        layer.cy /= (float)layer.moving;
    }
    std::lock_guard<std::mutex> lock(g_layer_mutex);
    layer.seq = ++g_layer_seq;
    if (g_trace) trace_add(g_trace_published, layer.fb, (uint32_t)layer.pixels.size());
    if (g_trace && (g_layer_seq % 60) == 0 && !layer.pixels.empty()) {
        int x0 = 9999, y0 = 9999, x1 = -1, y1 = -1, still = 0;
        for (const LayerPixel& p : layer.pixels) { x0 = std::min(x0, (int)p.x); y0 = std::min(y0, (int)p.y); x1 = std::max(x1, (int)p.x); y1 = std::max(y1, (int)p.y); still += p.still ? 1 : 0; }
        std::fprintf(stderr, "[cpu-layer] layer %06X: %zu px (%d rain), box %d,%d - %d,%d\n", layer.fb, layer.pixels.size(), still, x0, y0, x1, y1);
    }
    Layer* slot = &g_layers[0]; // the oldest
    for (Layer& l : g_layers) if (l.seq < slot->seq) slot = &l;
    *slot = std::move(layer);
}

} // namespace

// mainCPUeffects (jfg.us.toml): before rainDraw is called ...
extern "C" void jfg_cpu_rain_before(uint8_t* rdram, recomp_context* ctx) {
    capture_begin(rdram, ctx);
}

// ... and after it (also reached when no rain is drawn: then nothing was captured).
extern "C" void jfg_cpu_rain_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    capture_end(rdram, true);
}

// Entry of fxOutputLines.
extern "C" void jfg_cpu_lines_before(uint8_t* rdram, recomp_context* ctx) {
    capture_begin(rdram, ctx);
}

// mainCPUeffects, right after fxOutputLines returns: the frame's layer is complete.
extern "C" void jfg_cpu_lines_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    capture_end(rdram, false);
    publish();
}

// Present thread (render hook): the layer for the frame being presented, in window pixels. False if there is none.
bool cpu_layer_build(ImDrawList* list) {
    const RT64JfgPresentInfo& info = rt64_jfg_present_info;
    if (info.fbWidth == 0 || info.viewWidth <= 0.0f || info.viewHeight <= 0.0f) return false;
    if (!jfg_renderer_adds_frames()) return false;
    std::lock_guard<std::mutex> lock(g_layer_mutex);
    const Layer* cur = nullptr;
    for (const Layer& l : g_layers) {
        if (l.seq != 0 && l.fb == info.fbAddress && (cur == nullptr || l.seq > cur->seq)) cur = &l;
    }
    if (g_trace) {
        trace_add((cur != nullptr && cur->width == info.fbWidth) ? g_trace_found : g_trace_missing, info.fbAddress,
                  (cur != nullptr) ? (uint32_t)cur->pixels.size() : 0);
        if (++g_trace_frames % 120 == 0) {
            std::fprintf(stderr, "[cpu-layer] captured:%s | presented with layer:%s | without:%s | fb %ux%u\n",
                         trace_text(g_trace_published).c_str(), trace_text(g_trace_found).c_str(), trace_text(g_trace_missing).c_str(),
                         info.fbWidth, info.fbHeight);
            for (auto* list : { &g_trace_published, &g_trace_found, &g_trace_missing }) for (TraceCount& t : *list) t = TraceCount();
        }
    }
    if (cur == nullptr || cur->pixels.empty() || cur->width != info.fbWidth) return false;
    const Layer* prev = nullptr;
    for (const Layer& l : g_layers) {
        if (l.seq + 1 == cur->seq) prev = &l;
    }
    // Moved back toward the previous layer's place by the part of the step this frame has not reached yet. Only when
    // the two look alike (a reticle that appears, or whose shape changes, is drawn where it is).
    float ox = 0, oy = 0;
    if (prev != nullptr && prev->moving > 0 && cur->moving > 0) {
        const float a = (float)prev->moving, b = (float)cur->moving;
        if (std::fabs(a - b) <= 0.25f * std::max(a, b)) {
            const float back = 1.0f - std::clamp(info.weight, 0.0f, 1.0f);
            ox = (prev->cx - cur->cx) * back;
            oy = (prev->cy - cur->cy) * back;
        }
    }
    // Square pixels, centred: in widescreen RT64 stretches the frame across the wider window, but pastes CPU-drawn pixels
    // at the frame's height scale in the middle (RenderTarget::copyFromChanges, "pillarBox"), where the 4:3 picture sits.
    const float sy = info.viewHeight / (float)info.fbHeight, sx = sy;
    const float left = info.viewX + 0.5f * info.viewWidth - 0.5f * (float)info.fbWidth * sx;
    for (const LayerPixel& p : cur->pixels) {
        // Additive in the game: shown as the added colour over the picture, covering it by the amount of the brightest
        // channel added (result = added + (1 - that) x picture).
        const int m = std::max({ (int)p.r, (int)p.g, (int)p.b });
        const float alpha = (float)m / 31.0f;
        const ImU32 col = IM_COL32(p.r * 255 / m, p.g * 255 / m, p.b * 255 / m, (int)(alpha * 255.0f + 0.5f));
        const float px = (float)p.x + (p.still ? 0.0f : ox), py = (float)p.y + (p.still ? 0.0f : oy);
        const float x0 = left + px * sx, y0 = info.viewY + py * sy;
        list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sx, y0 + sy), col);
    }
    return true;
}

} // namespace jfg
