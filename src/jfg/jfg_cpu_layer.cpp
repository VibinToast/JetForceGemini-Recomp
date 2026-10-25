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
// Each connected group of lit pixels moves by its own step: the layer holds parts that move differently (the weapon's
// reticle at the aim point, its depth and heading digits that change every tick, the target boxes on enemies), and one
// step for all of them made the reticle shake while turning (issue #8).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <utility>
#include <vector>

#include "imgui/imgui.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_rt64.h"

extern "C" int jfg_renderer_adds_frames(void); // src/renderer/renderer.cpp

namespace jfg {
namespace {

constexpr uint16_t NO_GROUP = 0xFFFF;
struct LayerPixel { uint16_t x, y; uint8_t r, g, b; bool still; uint16_t group; }; // still: drawn where it is, never moved (rain)
// A connected (8-neighbour) group of the moving pixels: a reticle's corner or line, a digit, a target box.
struct Group {
    float cx = 0, cy = 0;   // centre
    uint32_t count = 0;
    uint16_t width = 0, height = 0;
    uint32_t shape = 0;     // the pixels' places inside the group's box and their colours: equal for the same drawing moved
    float ox = 0, oy = 0;   // step back to the previous layer's place (filled by match_groups)
};
struct Layer {
    uint32_t fb = 0;        // physical address, as RT64's VI reports it
    uint32_t seq = 0;       // 0 = empty slot
    uint32_t width = 0, height = 0;
    std::vector<LayerPixel> pixels;
    std::vector<Group> groups;
    uint32_t matched_seq = 0; // the previous layer the groups' steps were worked out against (0 = none)
    // Where this layer is drawn relative to where the game drew it (at most DEAD_ZONE pixels each way, see match_groups),
    // and the previous layer's, which the in-between frames start from.
    float bias_x = 0, bias_y = 0, prev_bias_x = 0, prev_bias_y = 0;
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
std::vector<int> g_grid; // pixel index per framebuffer pixel while grouping
std::vector<int> g_stack, g_members;
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
            g_pending.pixels.push_back({ x, y, (uint8_t)dr, (uint8_t)dg, (uint8_t)db, still, NO_GROUP });
        }
    }
    std::memcpy(raw, g_before.data(), bytes);
}

// Splits the layer's moving pixels into their connected groups (game thread, once per layer).
void make_groups(Layer& layer) {
    const uint32_t cells = layer.width * layer.height;
    if (g_grid.size() != cells) g_grid.assign(cells, -1);
    for (size_t i = 0; i < layer.pixels.size(); i++) {
        const LayerPixel& p = layer.pixels[i];
        if (!p.still) g_grid[p.y * layer.width + p.x] = (int)i;
    }
    for (size_t i = 0; i < layer.pixels.size(); i++) {
        LayerPixel& first = layer.pixels[i];
        if (first.still || first.group != NO_GROUP || layer.groups.size() >= NO_GROUP) continue;
        const uint16_t id = (uint16_t)layer.groups.size();
        Group g;
        int x0 = first.x, y0 = first.y, x1 = first.x, y1 = first.y;
        g_stack.clear();
        g_stack.push_back((int)i);
        g_grid[first.y * layer.width + first.x] = -1;
        g_members.clear(); // kept for the shape, which needs the box first
        while (!g_stack.empty()) {
            const int index = g_stack.back();
            g_stack.pop_back();
            LayerPixel& p = layer.pixels[index];
            p.group = id;
            g_members.push_back(index);
            g.cx += p.x; g.cy += p.y;
            x0 = std::min(x0, (int)p.x); y0 = std::min(y0, (int)p.y); x1 = std::max(x1, (int)p.x); y1 = std::max(y1, (int)p.y);
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    const int x = p.x + dx, y = p.y + dy;
                    if (x < 0 || y < 0 || x >= (int)layer.width || y >= (int)layer.height) continue;
                    int& cell = g_grid[y * layer.width + x];
                    if (cell >= 0) { g_stack.push_back(cell); cell = -1; }
                }
            }
        }
        g.count = (uint32_t)g_members.size();
        g.cx /= (float)g.count;
        g.cy /= (float)g.count;
        g.width = (uint16_t)(x1 - x0 + 1);
        g.height = (uint16_t)(y1 - y0 + 1);
        // Order-free sum of a hash per pixel (place in the box and colour).
        for (int index : g_members) {
            const LayerPixel& p = layer.pixels[index];
            uint32_t h = (uint32_t)(p.x - x0) | ((uint32_t)(p.y - y0) << 10) | ((uint32_t)(p.r ^ (p.g << 5) ^ (p.b << 10)) << 20);
            h ^= h >> 16; h *= 0x7FEB352Du; h ^= h >> 15; h *= 0x846CA68Bu; h ^= h >> 16;
            g.shape += h;
        }
        layer.groups.push_back(g);
    }
    for (const LayerPixel& p : layer.pixels) if (!p.still) g_grid[p.y * layer.width + p.x] = -1; // left clean for the next layer
}

// The pending layer becomes the layer of its framebuffer.
void publish() {
    if (g_pending.fb == 0) return;
    Layer layer = std::move(g_pending);
    g_pending = Layer();
    make_groups(layer);
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

// Works out each group's step back to the previous layer (present thread, under the layer lock, once per pair). Groups
// drawn the same way in the previous layer (same shape) within MAX_STEP pixels are candidate partners. The step shared by
// the most pixels wins first (a reticle's lines all move by the same step, so it moves as one piece even when it swings
// further than the gap between its own equal-looking parts, as at Game 30), then the next (a target box), and so on. A
// group with no partner (a digit that changed, a box that is growing or just appeared) moves with the nearest group that
// has one, within NEAR pixels (the digits ride along with their reticle), or is drawn where it is.
void match_groups(Layer& cur, const Layer* prev) {
    const uint32_t prev_seq = (prev != nullptr) ? prev->seq : 0;
    if (cur.matched_seq == prev_seq && prev_seq != 0) return;
    cur.matched_seq = prev_seq;
    constexpr float MAX_STEP = 128.0f, NEAR = 40.0f;
    std::vector<uint8_t> partnered(cur.groups.size(), 0), taken(prev != nullptr ? prev->groups.size() : 0, 0);
    for (Group& g : cur.groups) g.ox = g.oy = 0;
    cur.bias_x = cur.bias_y = cur.prev_bias_x = cur.prev_bias_y = 0;
    if (prev == nullptr) return;
    struct Pair { float dx, dy, d; uint32_t c, p; };
    std::vector<Pair> pairs;
    for (uint32_t c = 0; c < cur.groups.size(); c++) {
        const Group& a = cur.groups[c];
        for (uint32_t p = 0; p < prev->groups.size(); p++) {
            const Group& b = prev->groups[p];
            if (a.shape != b.shape || a.count != b.count || a.width != b.width || a.height != b.height) continue;
            const float dx = b.cx - a.cx, dy = b.cy - a.cy, d = std::hypot(dx, dy);
            if (d <= MAX_STEP) pairs.push_back({ dx, dy, d, c, p });
        }
    }
    // Closest first, so of the pairs with one step the nearer ones are taken.
    // Pairs with the same step next to each other (equal shapes moved by whole pixels: the rounded key is exact).
    auto key = [](const Pair& x) { return std::make_pair(std::lround(x.dx * 4.0f), std::lround(x.dy * 4.0f)); };
    std::sort(pairs.begin(), pairs.end(), [&](const Pair& x, const Pair& y) { return key(x) < key(y); });
    bool first = true;
    for (;;) {
        // The open step with the most pixels (a group has at most one partner at a given step); the shorter one on a tie.
        size_t best = 0, best_end = 0;
        uint32_t best_pixels = 0;
        for (size_t start = 0; start < pairs.size(); ) {
            size_t end = start;
            uint32_t pixels = 0;
            while (end < pairs.size() && key(pairs[end]) == key(pairs[start])) {
                if (!partnered[pairs[end].c] && !taken[pairs[end].p]) pixels += cur.groups[pairs[end].c].count;
                end++;
            }
            if (pixels > best_pixels || (pixels > 0 && pixels == best_pixels && pairs[start].d < pairs[best].d)) {
                best = start; best_end = end; best_pixels = pixels;
            }
            start = end;
        }
        if (best_pixels == 0) break;
        if (first) {
            // Dead zone on the main step (the reticle's): a move of a pixel or so is taken up by the bias instead of being
            // shown, and shown once the bias is used up. While aiming and turning, the game moves the reticle up and down
            // by a pixel from tick to tick (it follows the view pitch, which the mouse nudges); with in-between frames
            // that read as a shake against the smooth picture. Real moves still show, at most DEAD_ZONE pixels late.
            constexpr float DEAD_ZONE = 1.0f;
            first = false;
            cur.prev_bias_x = prev->bias_x;
            cur.prev_bias_y = prev->bias_y;
            cur.bias_x = std::clamp(prev->bias_x + pairs[best].dx, -DEAD_ZONE, DEAD_ZONE);
            cur.bias_y = std::clamp(prev->bias_y + pairs[best].dy, -DEAD_ZONE, DEAD_ZONE);
        }
        for (size_t i = best; i < best_end; i++) {
            const Pair& y = pairs[i];
            if (partnered[y.c] || taken[y.p]) continue;
            partnered[y.c] = taken[y.p] = 1;
            cur.groups[y.c].ox = y.dx;
            cur.groups[y.c].oy = y.dy;
        }
    }
    // A small group (a digit or letter) goes with the nearest larger partnered group (its reticle's lines) when that is
    // within NEAR: the same digit elsewhere in the readout (125 -> 152) would otherwise give it a false step.
    // Then a group with no partner goes with the nearest partnered group.
    constexpr uint32_t SMALL = 20;
    auto follow_nearest = [&](uint32_t c, uint32_t min_count) {
        Group& g = cur.groups[c];
        float best = NEAR;
        for (uint32_t o = 0; o < cur.groups.size(); o++) {
            if (o == c || !partnered[o] || cur.groups[o].count < min_count) continue;
            const float d = std::hypot(cur.groups[o].cx - g.cx, cur.groups[o].cy - g.cy);
            if (d < best) { best = d; g.ox = cur.groups[o].ox; g.oy = cur.groups[o].oy; }
        }
    };
    for (uint32_t c = 0; c < cur.groups.size(); c++) {
        if (partnered[c] && cur.groups[c].count < SMALL) follow_nearest(c, SMALL);
    }
    for (uint32_t c = 0; c < cur.groups.size(); c++) {
        if (!partnered[c]) follow_nearest(c, 0);
    }
}

} // namespace

// Before rainDraw (its wrapper, below; called only from mainCPUeffects) ...
extern "C" void jfg_cpu_rain_before(uint8_t* rdram, recomp_context* ctx) {
    capture_begin(rdram, ctx);
}

// ... and after it (also reached when no rain is drawn: then nothing was captured).
extern "C" void jfg_cpu_rain_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    capture_end(rdram, true);
}

// Before fxOutputLines (its wrapper, below).
extern "C" void jfg_cpu_lines_before(uint8_t* rdram, recomp_context* ctx) {
    capture_begin(rdram, ctx);
}

// After fxOutputLines (its wrapper; mainCPUeffects is its only caller): the frame's layer is complete.
extern "C" void jfg_cpu_lines_after(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    capture_end(rdram, false);
    publish();
}

// mainCPUeffects, right after its fxInitLines returns (the wrappers below): during a fade (going through a loading zone, a warp) the game draws
// no lines and only clears its list. The frame's buffer gets an empty layer, so the last reticle drawn into it is not
// shown again on every frame presented for it until lines are drawn there again.
extern "C" void jfg_cpu_lines_faded(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    g_rdram = rdram;
    g_capturing = false;
    if (!jfg_renderer_adds_frames()) return;
    const uint32_t screen = rd32(ADDR_currentScreen);
    if (!valid_ptr(screen)) return;
    const uint32_t fb = screen & 0x1FFFFFFFu;
    if (g_pending.fb != fb) {
        g_pending = Layer();
        g_pending.fb = fb;
        g_pending.width = g_capture_width;
        g_pending.height = g_capture_height;
    }
    publish();
}

// Present thread (render hook): the layer for the frame being presented, in window pixels. False if there is none.
bool cpu_layer_build(ImDrawList* list) {
    const RT64JfgPresentInfo& info = rt64_jfg_present_info;
    if (info.fbWidth == 0 || info.viewWidth <= 0.0f || info.viewHeight <= 0.0f) return false;
    if (!jfg_renderer_adds_frames()) return false;
    std::lock_guard<std::mutex> lock(g_layer_mutex);
    Layer* cur = nullptr;
    for (Layer& l : g_layers) {
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
        if (l.seq + 1 == cur->seq && l.width == cur->width && l.height == cur->height) prev = &l;
    }
    match_groups(*cur, prev);
    // Moved back toward the previous layer's place by the part of the step this frame has not reached yet.
    const float back = 1.0f - std::clamp(info.frameWeight, 0.0f, 1.0f);
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
        float px = (float)p.x, py = (float)p.y;
        if (!p.still) {
            // From the previous layer's drawn place to this one's (each where the game drew it plus its bias).
            const float ox = (p.group != NO_GROUP) ? cur->groups[p.group].ox : 0.0f, oy = (p.group != NO_GROUP) ? cur->groups[p.group].oy : 0.0f;
            px += cur->bias_x + (ox + cur->prev_bias_x - cur->bias_x) * back;
            py += cur->bias_y + (oy + cur->prev_bias_y - cur->bias_y) * back;
        }
        const float x0 = left + px * sx, y0 = info.viewY + py * sy;
        list->AddRectFilled(ImVec2(x0, y0), ImVec2(x0 + sx, y0 + sy), col);
    }
    return true;
}

} // namespace jfg

// CPU-drawn rain and reticles with in-between frames (Smooth Motion): what rainDraw and fxOutputLines add to the finished
// frame is taken out again and drawn on every presented frame by the render hook. rainDraw is reached only from
// mainCPUeffects (through TrapDanglingJump); its paths without rain capture nothing.
JFG_WRAP(rainDraw) {
    jfg_cpu_rain_before(rdram, ctx);
    rainDraw_orig(rdram, ctx);
    jfg_cpu_rain_after(rdram, ctx);
}

JFG_WRAP(fxOutputLines) {
    jfg_cpu_lines_before(rdram, ctx);
    fxOutputLines_orig(rdram, ctx);
    jfg_cpu_lines_after(rdram, ctx);
}

// During a fade mainCPUeffects calls fxInitLines instead of fxOutputLines; after its own call, the empty layer.
static JfgCaller g_cpu_effects; // mainCPUeffects (frame 0x40)

JFG_WRAP(mainCPUeffects) {
    JfgCaller::Scope scope(g_cpu_effects, ctx, 0x40);
    mainCPUeffects_orig(rdram, ctx);
}

JFG_WRAP(fxInitLines) {
    const bool faded = g_cpu_effects.direct(ctx);
    fxInitLines_orig(rdram, ctx);
    if (faded) jfg_cpu_lines_faded(rdram, ctx);
}
