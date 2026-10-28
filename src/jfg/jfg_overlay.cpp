// Jet Force Gemini port: the mod overlay (mod_api/jfg_api.h, "overlay"): pictures, polygons and circles a mod sends
// each frame, drawn by the port over the game at the window's own resolution, anti-aliased (ImGui draw lists, on every
// presented frame by the render hook, src/debug_ui/backend.cpp), rather than on the game's pixels.
//
// Places are in overlay units: the game's picture is 240 high and as wide as it is shown (320 in 4:3, about 427 in
// 16:9; jfg_overlay_size). What mods send while the game draws a frame (from its game tick or jfg_on_draw_hud) is shown
// from the end of that frame (prnBorder) until the next; shapes stop being shown a quarter of a second after the game
// last drew a frame with them (menus, the title screen). Pictures are kept by id until replaced: RGBA, up to
// 1024 x 1024, uploaded once as ImGui textures (both of the port's renderer backends update them).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include "imgui/imgui.h"
#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_rt64.h"

namespace jfg::overlay {
namespace {
constexpr float HEIGHT = 240.0f;
constexpr int MAX_IMAGE = 1024;
constexpr size_t MAX_SHAPES = 4096, MAX_POINTS = 64;
constexpr auto STALE = std::chrono::milliseconds(250);

enum class Kind : uint8_t { Image, Polygon, Circle };
struct Shape {
    Kind kind;
    int image = 0;
    uint32_t fill = 0, line = 0; // 0xRRGGBBAA
    float thickness = 0.0f;
    float x = 0, y = 0, w = 0, h = 0, r = 0;
    std::vector<ImVec2> points;
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> pixels; // RGBA, as ImGui stores it
    bool changed = false;
    ImTextureData* tex = nullptr;
};

std::mutex g_mutex;
std::vector<Shape> g_building;  // game thread
std::vector<Shape> g_shown;     // under g_mutex
std::chrono::steady_clock::time_point g_shown_at;
std::map<int, Image> g_images;  // under g_mutex
std::vector<ImTextureData*> g_retired; // textures replaced by a new size, destroyed by the backend first

ImU32 to_im(uint32_t rgba) {
    return IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, rgba & 0xFF);
}

float rdf(uint32_t a) {
    const uint32_t v = rd32(a);
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

void add(Shape&& s) {
    if (g_building.size() < MAX_SHAPES) g_building.push_back(std::move(s));
}

// --- the exports ---------------------------------------------------------------------------------------------------
// jfg_overlay_size(float* out): out[0] the overlay's width, out[1] its height (240).
void overlay_size(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t out = (uint32_t)ctx->r4;
    const RT64JfgPresentInfo& info = rt64_jfg_present_info;
    float w = HEIGHT * 4.0f / 3.0f;
    if (info.viewHeight > 0.0f && info.viewWidth > 0.0f) w = HEIGHT * info.viewWidth / info.viewHeight;
    uint32_t bits;
    std::memcpy(&bits, &w, 4);
    wr32(out, bits);
    const float h = HEIGHT;
    std::memcpy(&bits, &h, 4);
    wr32(out + 4, bits);
}

// jfg_overlay_image(int id, int w, int h, const unsigned int* rgba): picture id, w x h words 0xRRGGBBAA.
void overlay_image(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int)ctx->r4, w = (int)ctx->r5, h = (int)ctx->r6;
    const uint32_t src = (uint32_t)ctx->r7;
    if (w <= 0 || h <= 0 || w > MAX_IMAGE || h > MAX_IMAGE || src == 0) return;
    std::vector<uint32_t> pixels((size_t)w * h);
    for (size_t i = 0; i < pixels.size(); i++) {
        const uint32_t c = rd32(src + (uint32_t)i * 4);
        // ImGui's RGBA32 is bytes R, G, B, A in memory: on a little-endian host, A << 24 | B << 16 | G << 8 | R.
        pixels[i] = (c >> 24) | ((c >> 8) & 0xFF00) | ((c << 8) & 0xFF0000) | (c << 24);
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    Image& img = g_images[id];
    if (img.tex != nullptr && (img.w != w || img.h != h)) {
        img.tex->SetStatus(ImTextureStatus_WantDestroy);
        img.tex->UnusedFrames = 8; // the backends destroy a texture once no frame in flight uses it
        g_retired.push_back(img.tex);
        img.tex = nullptr;
    }
    img.w = w;
    img.h = h;
    img.pixels = std::move(pixels);
    img.changed = true;
}

// jfg_overlay_draw_image(const JfgOverlayImage* image): { float x, y, w, h; unsigned int tint; int id; }
void overlay_draw_image(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    if (p == 0) return;
    Shape s{ Kind::Image };
    s.x = rdf(p); s.y = rdf(p + 4); s.w = rdf(p + 8); s.h = rdf(p + 12);
    s.fill = rd32(p + 16);
    s.image = (int)rd32(p + 20);
    add(std::move(s));
}

// jfg_overlay_polygon(const JfgOverlayPolygon* polygon): { const float* xy; int count; unsigned int fill, line;
// float thickness; } A polygon (sides not crossing), filled and/or outlined.
void overlay_polygon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    if (p == 0) return;
    const uint32_t xy = rd32(p);
    const int n = (int)rd32(p + 4);
    if (xy == 0 || n < 2 || n > (int)MAX_POINTS) return;
    Shape s{ Kind::Polygon };
    s.fill = rd32(p + 8);
    s.line = rd32(p + 12);
    s.thickness = rdf(p + 16);
    for (int i = 0; i < n; i++) s.points.emplace_back(rdf(xy + i * 8), rdf(xy + i * 8 + 4));
    add(std::move(s));
}

// jfg_overlay_circle(const JfgOverlayCircle* circle): { float x, y, radius; unsigned int fill, line; float thickness; }
void overlay_circle(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    if (p == 0) return;
    Shape s{ Kind::Circle };
    s.x = rdf(p); s.y = rdf(p + 4); s.r = rdf(p + 8);
    s.fill = rd32(p + 12);
    s.line = rd32(p + 16);
    s.thickness = rdf(p + 20);
    add(std::move(s));
}

ImTextureData* g_white = nullptr; // a white texture: shapes take its colour (the list's own texture)
std::atomic<bool> g_menu_shown{ false };
} // namespace

// The settings menu or a prompt is open: the overlay is left out, so it never covers it (F10's tools bar leaves it
// up). The game draws on behind the menu, so what mods send keeps coming.
void set_menu_shown(bool shown) { g_menu_shown.store(shown, std::memory_order_relaxed); }

// The game finished a frame: what was sent for it is shown from now on.
void frame_done() {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_shown.swap(g_building);
    g_building.clear();
    g_shown_at = std::chrono::steady_clock::now();
}

void register_api() {
    recomp::overlays::register_base_export("jfg_overlay_size", overlay_size);
    recomp::overlays::register_base_export("jfg_overlay_image", overlay_image);
    recomp::overlays::register_base_export("jfg_overlay_draw_image", overlay_draw_image);
    recomp::overlays::register_base_export("jfg_overlay_polygon", overlay_polygon);
    recomp::overlays::register_base_export("jfg_overlay_circle", overlay_circle);
}

// Present thread (render hook): the overlay in window pixels into list, the textures to create or update into
// textures. False if there is nothing to draw.
bool build(ImDrawList* list, ImVector<ImTextureData*>& textures) {
    const RT64JfgPresentInfo& info = rt64_jfg_present_info;
    if (info.fbWidth == 0 || info.viewWidth <= 0.0f || info.viewHeight <= 0.0f) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    for (ImTextureData* t : g_retired) textures.push_back(t);
    g_retired.erase(std::remove_if(g_retired.begin(), g_retired.end(), [](ImTextureData* t) {
        if (t->Status != ImTextureStatus_Destroyed) return false;
        IM_DELETE(t);
        return true;
    }), g_retired.end());
    if (g_shown.empty() || g_menu_shown.load(std::memory_order_relaxed) ||
        std::chrono::steady_clock::now() - g_shown_at > STALE) return !textures.empty();
    if (g_white == nullptr) {
        g_white = IM_NEW(ImTextureData)();
        g_white->Create(ImTextureFormat_RGBA32, 4, 4);
        std::memset(g_white->GetPixels(), 0xFF, (size_t)g_white->GetSizeInBytes());
    }
    textures.push_back(g_white);
    for (auto& [id, img] : g_images) {
        if (!img.changed) {
            if (img.tex != nullptr) textures.push_back(img.tex);
            continue;
        }
        if (img.tex == nullptr) {
            img.tex = IM_NEW(ImTextureData)();
            img.tex->Create(ImTextureFormat_RGBA32, img.w, img.h);
        } else if (img.tex->Status == ImTextureStatus_OK) {
            img.tex->UpdateRect = ImTextureRect{ 0, 0, (unsigned short)img.w, (unsigned short)img.h };
            img.tex->Updates.resize(0);
            img.tex->Updates.push_back(img.tex->UpdateRect);
            img.tex->SetStatus(ImTextureStatus_WantUpdates);
        }
        std::memcpy(img.tex->GetPixels(), img.pixels.data(), img.pixels.size() * 4);
        img.changed = false;
        textures.push_back(img.tex);
    }
    const float scale = info.viewHeight / HEIGHT;
    auto at = [&](float x, float y) { return ImVec2(info.viewX + x * scale, info.viewY + y * scale); };
    list->_FringeScale = 1.0f;
    list->_InvFringeScale = 1.0f;
    list->Flags = ImDrawListFlags_AntiAliasedLines | ImDrawListFlags_AntiAliasedFill;
    list->PushTexture(g_white->GetTexRef());
    for (const Shape& s : g_shown) {
        switch (s.kind) {
        case Kind::Image: {
            auto it = g_images.find(s.image);
            if (it == g_images.end() || it->second.tex == nullptr) break;
            list->AddImage(it->second.tex->GetTexRef(), at(s.x, s.y), at(s.x + s.w, s.y + s.h), ImVec2(0, 0), ImVec2(1, 1), to_im(s.fill));
            break;
        }
        case Kind::Polygon: {
            std::vector<ImVec2> pts;
            for (const ImVec2& p : s.points) pts.push_back(at(p.x, p.y));
            if (s.fill & 0xFF) list->AddConcavePolyFilled(pts.data(), (int)pts.size(), to_im(s.fill)); // convex or not
            if ((s.line & 0xFF) && s.thickness > 0.0f) list->AddPolyline(pts.data(), (int)pts.size(), to_im(s.line), ImDrawFlags_Closed, s.thickness * scale);
            break;
        }
        case Kind::Circle: {
            const ImVec2 c = at(s.x, s.y);
            const float r = s.r * scale;
            const int seg = std::clamp((int)(r * 1.5f), 12, 64);
            if (s.fill & 0xFF) list->AddCircleFilled(c, r, to_im(s.fill), seg);
            if ((s.line & 0xFF) && s.thickness > 0.0f) list->AddCircle(c, r, to_im(s.line), seg, s.thickness * scale);
            break;
        }
        }
    }
    list->PopTexture();
    return true;
}

} // namespace jfg::overlay
