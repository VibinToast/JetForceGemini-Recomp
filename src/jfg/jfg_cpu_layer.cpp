// Jet Force Gemini port: the CPU-drawn reticles with in-between frames.
//
// Most weapons' reticles (and their tiny "LOCK" text and numbers) are not drawn by the RDP: fxOutputLines adds them
// straight into the finished framebuffer in RDRAM (decomp docs/PORT_FINDINGS.md, "CPU line drawing"). RT64 only notices
// such pixels when the next frame's display list arrives (checkRDRAM), and copies them into that one framebuffer's
// picture. With the Renderer Framerate above 30 the frames on screen are mostly in-between frames made from draw calls,
// so the reticle was missing (or, at best, moved at 30).
//
// While RT64 adds in-between frames, the pixels fxOutputLines adds are taken out of RDRAM again (so RT64 never sees
// them) and kept as a layer for the framebuffer they were drawn into. The render hook (src/debug_ui/backend.cpp) draws
// that layer on every frame presented for that framebuffer, moved between the previous layer's place and this one's by
// the frame's weight, the way RT64 moves the scene.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_rt64.h"

extern "C" int jfg_renderer_adds_frames(void); // src/renderer/renderer.cpp

namespace jfg {
namespace {

struct LayerPixel { uint16_t x, y; uint8_t r, g, b; };
struct Layer {
    uint32_t fb = 0;        // physical address, as RT64's VI reports it
    uint32_t seq = 0;       // 0 = empty slot
    uint32_t width = 0, height = 0;
    float cx = 0, cy = 0;   // centre of the lit pixels
    std::vector<LayerPixel> pixels;
};

std::mutex g_layer_mutex;
Layer g_layers[2];         // the two most recent, for the double-buffered screen
uint32_t g_layer_seq = 0;

// Game thread only.
bool g_capturing = false;
uint32_t g_capture_screen = 0, g_capture_width = 0, g_capture_height = 0;
std::vector<uint8_t> g_before;

inline uint16_t pixel_at(const uint8_t* raw, uint32_t offset) {
    uint16_t v;
    std::memcpy(&v, raw + (offset ^ 2), 2); // halves are swizzled inside host-endian words
    return v;
}

} // namespace

// Entry of fxOutputLines (jfg.us.toml): keep the framebuffer as it is before the lines are added.
extern "C" void jfg_cpu_lines_before(uint8_t* rdram, recomp_context* ctx) {
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

// mainCPUeffects, right after fxOutputLines returns: what it added becomes the layer, and RDRAM gets the frame back.
extern "C" void jfg_cpu_lines_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (!g_capturing) return;
    g_capturing = false;
    uint8_t* raw = rdram + (g_capture_screen - 0x80000000u);
    const uint32_t bytes = g_capture_width * g_capture_height * 2;
    Layer layer;
    layer.fb = g_capture_screen & 0x1FFFFFFFu;
    layer.width = g_capture_width;
    layer.height = g_capture_height;
    double sx = 0, sy = 0;
    for (uint32_t word = 0; word < bytes; word += 4) {
        uint32_t a, b;
        std::memcpy(&a, raw + word, 4);
        std::memcpy(&b, g_before.data() + word, 4);
        if (a == b) continue;
        for (uint32_t half = 0; half < 4; half += 2) {
            const uint32_t offset = word + half;
            const uint16_t now = pixel_at(raw, offset), was = pixel_at(g_before.data(), offset);
            if (now == was) continue;
            // RGBA5551: the amounts fxOutputLines added (it only adds, with saturation; a few brushes also subtract,
            // those are left out).
            const int dr = std::max(0, ((now >> 11) & 31) - ((was >> 11) & 31));
            const int dg = std::max(0, ((now >> 6) & 31) - ((was >> 6) & 31));
            const int db = std::max(0, ((now >> 1) & 31) - ((was >> 1) & 31));
            if (dr == 0 && dg == 0 && db == 0) continue;
            const uint32_t index = offset / 2;
            const uint16_t x = (uint16_t)(index % g_capture_width), y = (uint16_t)(index / g_capture_width);
            layer.pixels.push_back({ x, y, (uint8_t)dr, (uint8_t)dg, (uint8_t)db });
            sx += x;
            sy += y;
        }
    }
    // The frame without the lines, so RT64 does not copy them in a frame late as well.
    std::memcpy(raw, g_before.data(), bytes);
    if (!layer.pixels.empty()) {
        layer.cx = (float)(sx / layer.pixels.size());
        layer.cy = (float)(sy / layer.pixels.size());
    }
    std::lock_guard<std::mutex> lock(g_layer_mutex);
    layer.seq = ++g_layer_seq;
    Layer& slot = (g_layers[0].seq <= g_layers[1].seq) ? g_layers[0] : g_layers[1];
    slot = std::move(layer);
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
    if (cur == nullptr || cur->pixels.empty() || cur->width != info.fbWidth) return false;
    const Layer* prev = nullptr;
    for (const Layer& l : g_layers) {
        if (l.seq + 1 == cur->seq) prev = &l;
    }
    // Moved back toward the previous layer's place by the part of the step this frame has not reached yet. Only when
    // the two look alike (a reticle that appears, or whose shape changes, is drawn where it is).
    float ox = 0, oy = 0;
    if (prev != nullptr && !prev->pixels.empty()) {
        const float a = (float)prev->pixels.size(), b = (float)cur->pixels.size();
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
        const float x0 = left + ((float)p.x + ox) * sx, y0 = info.viewY + ((float)p.y + oy) * sy;
        list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sx, y0 + sy), col);
    }
    return true;
}

} // namespace jfg
