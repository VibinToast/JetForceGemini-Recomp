// Jet Force Gemini port: "HUD in Screen Corners" (Graphics tab): in widescreen, the single-player HUD moves out of
// the middle 4:3 area to the edges of the window.
//
// RT64 widens perspective projections only; the HUD (front-end objects, numbers, text, the health meter, all drawn
// through frontgfx after camStandardOrtho) stays in the middle. RT64's extended GBI can measure 2D rectangles and
// viewports from the left or right edge of the window instead (gEXSetRectAlign, gEXSetViewportAlign, see
// lib/rt64/include/rt64_extended_gbi.h). While frontSingleInstruments draws, the HUD goes to the left edge (its panel
// - weapon, ammo, the box that opens for pick-ups, the weapon strip, health - is all on the left); a piece whose own
// position is clearly in the middle (the target, centred texts and bars) stays, and one right of the middle goes to the
// right edge. Those commands go into frontgfx in front of each piece; the game's own scissors are aligned the same way. A viewport alignment only applies when a viewport is set, so the
// HUD's orthographic viewport (camStandardOrtho's) is set again after each change. The lines the CPU draws into the
// picture (fxDrawLine: the target box) are not moved. The jetpack fuel meter's number is CPU-drawn too
// (fxDrawDigitalNumber), into the 4:3 picture, so it could not follow the meter to the edge: with the HUD at the edges
// the same digits are drawn as HUD rectangles instead (draw_fuel_digits).
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" int jfg_widescreen_active(void); // jfg_visuals.cpp
extern "C" void instShowBearBar(uint8_t* rdram, recomp_context* ctx); // overlay 14
extern "C" void frontDrawRectangles(uint8_t* rdram, recomp_context* ctx);

namespace jfg {

namespace {
constexpr uint32_t ADDR_frontgfx = 0x800FF398;       // Gfx*, the HUD's display list
constexpr uint32_t ADDR_currentobjects = 0x800FF780; // FrontEndObject[], 0x20 bytes each: +0xC x (float, from the middle)
constexpr uint32_t ADDR_viewports = 0x800A2FB8;      // Vp[], camStandardOrtho's is [camera + 5]
constexpr uint32_t FRONT_OBJECT_SIZE = 0x20;

// rt64_extended_gbi.h
constexpr uint32_t RT64_HOOK_OPCODE = 0x00, RT64_HOOK_MAGIC_NUMBER = 0x525464, RT64_HOOK_OP_ENABLE = 0x1;
constexpr uint32_t RT64_EXTENDED_OPCODE = 0x64;
constexpr uint32_t G_EX_SETSCISSOR_V1 = 0x5, G_EX_SETRECTALIGN_V1 = 0x6, G_EX_SETVIEWPORTALIGN_V1 = 0x7,
                   G_EX_SETSCISSORALIGN_V1 = 0x8;
constexpr uint32_t G_EX_ORIGIN_NONE = 0x800, G_EX_ORIGIN_LEFT = 0x0, G_EX_ORIGIN_RIGHT = 0x400;

enum Side { MIDDLE, LEFT, RIGHT };
constexpr float SIDE_MARGIN = 40.0f; // a piece within this of the middle (in the game's 320-wide units) stays

bool g_on = false;        // between jfg_hud_begin and jfg_hud_end, with the option on in widescreen
Side g_current = MIDDLE;  // the alignment last written
Side g_stack[16];
int g_depth = 0;
bool g_air_region = false;
int g_block = 0;          // > 0: inside instDrawHealth, whose pieces go left as one block (the depth it started at)
int g_width = 320, g_height = 240;

void emit(uint32_t w0, uint32_t w1) {
    const uint32_t gfx = rd32(ADDR_frontgfx);
    if (!valid_ptr(gfx)) return;
    wr32(gfx, w0);
    wr32(gfx + 4, w1);
    wr32(ADDR_frontgfx, gfx + 8);
}

void full_window_scissor() { // gEXSetScissor from the window's left edge to its right edge
    emit((RT64_EXTENDED_OPCODE << 24) | G_EX_SETSCISSOR_V1, (G_EX_ORIGIN_LEFT << 2) | (G_EX_ORIGIN_RIGHT << 14));
    emit(0, (uint32_t)(g_height * 4) & 0xFFFF);
}

uint32_t ortho_viewport() {
    return ADDR_viewports + (rd32(ADDR_cameraNo) + 5) * 16;
}

void set_side(Side side) {
    if (side == g_current) return;
    g_current = side;
    const uint32_t origin = side == LEFT ? G_EX_ORIGIN_LEFT : side == RIGHT ? G_EX_ORIGIN_RIGHT : G_EX_ORIGIN_NONE;
    // Measured from the right edge, the game's x has its full width taken off, so its right edge lands on the window's.
    const uint32_t offset = side == RIGHT ? (uint32_t)(-(g_width * 4)) & 0xFFFF : 0;
    emit((RT64_EXTENDED_OPCODE << 24) | G_EX_SETRECTALIGN_V1, origin | (origin << 12));
    emit((offset << 16), (offset << 16));
    emit((RT64_EXTENDED_OPCODE << 24) | G_EX_SETVIEWPORTALIGN_V1, origin);
    emit((offset << 16), 0);
    emit(0x03800010, ortho_viewport() & 0x1FFFFFFF); // gSPViewport, as camStandardOrtho writes it
    // The scissors the game sets from here on are moved the same way (bounds: none). The one in force is kept: the box
    // that opens for pick-ups scrolls its text inside one, which a piece drawn in between (a tribal's head) must not undo.
    emit((RT64_EXTENDED_OPCODE << 24) | G_EX_SETSCISSORALIGN_V1, origin | (origin << 12));
    emit((offset << 16), (offset << 16));
    emit(0, 0xFFFFFFFF);
}

Side side_of(float x) {
    if (x < -SIDE_MARGIN) return LEFT;
    if (x > SIDE_MARGIN) return RIGHT;
    return MIDDLE;
}

void push(Side side) {
    if (!g_on) return;
    // In a block everything goes left with its panel: the objective panel's heads, their counts and its end-cap sit at any
    // x across it, right of the middle too.
    if (g_block > 0) side = LEFT;
    if (g_depth < (int)(sizeof(g_stack) / sizeof(g_stack[0]))) g_stack[g_depth] = side;
    g_depth++;
    set_side(side);
}

void pop() {
    if (!g_on || g_depth == 0) return;
    g_depth--;
    if (g_depth < g_block) g_block = 0;
    set_side(g_depth > 0 && g_depth <= (int)(sizeof(g_stack) / sizeof(g_stack[0])) ? g_stack[g_depth - 1] : MIDDLE);
}
}

// The single-player HUD goes to the window's edges (HUD in Screen Corners, in widescreen): jfg_hud_in_corners.
bool hud_in_corners() {
    static const bool env = std::getenv("JFG_HUD_CORNERS") != nullptr;
    return (g.hud_corners || env) && jfg_widescreen_active();
}

// frontSingleInstruments, right after its camStandardOrtho (the target and its box are drawn before and stay put).
extern "C" void jfg_hud_begin(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    // JFG_DEBUG_HUD_PANEL=1: the objective panel (tribals, gems) stays open, as after a pick-up, for window captures.
    static const bool panel = std::getenv("JFG_DEBUG_HUD_PANEL") != nullptr;
    if (panel) call_game(instShowBearBar, ctx, {});
    g_on = false;
    g_depth = 0;
    g_air_region = false;
    g_block = 0;
    g_current = MIDDLE;
    static const bool env = std::getenv("JFG_HUD_CORNERS") != nullptr; // for tests, like JFG_WIDESCREEN
    if (!(g.hud_corners || env) || !jfg_widescreen_active()) return;
    const uint32_t vp = ortho_viewport();
    g_width = (int16_t)rd16(vp + 8) / 2;   // vtrans: width * 2, height * 2 (camStandardOrtho)
    g_height = (int16_t)rd16(vp + 10) / 2;
    if (g_width <= 0 || g_height <= 0) return;
    g_on = true;
    emit((RT64_HOOK_OPCODE << 24) | RT64_HOOK_MAGIC_NUMBER, (RT64_HOOK_OP_ENABLE << 28) | RT64_EXTENDED_OPCODE); // gEXEnable
    push(LEFT); // the HUD's side unless a piece says otherwise
    full_window_scissor(); // until the game sets one: its own would still cut at the 4:3 area
}

// The return of frontInstruments: the HUD is done; alignment and scissor go back to normal.
extern "C" void jfg_hud_end(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_on) return;
    g_depth = 0;
    set_side(MIDDLE);
    emit((RT64_EXTENDED_OPCODE << 24) | G_EX_SETSCISSORALIGN_V1, G_EX_ORIGIN_NONE | (G_EX_ORIGIN_NONE << 12)); // no alignment
    emit(0, 0);
    emit(0, 0xFFFFFFFF);
    emit(0xED000000, ((uint32_t)(g_width * 4) << 12) | (uint32_t)(g_height * 4)); // gDPSetScissor, the whole screen
    g_on = false;
}

// Entry of frontDrawObj(item): the object's own x.
extern "C" void jfg_hud_object(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g_on) return;
    const uint32_t item = (uint32_t)ctx->r4;
    push(item < 64 ? side_of(rdf(ADDR_currentobjects + item * FRONT_OBJECT_SIZE + 0xC)) : MIDDLE);
}

// Entry of frontPrintNum(num, x, y, ...): x from the middle.
extern "C" void jfg_hud_number(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g_on) return;
    push(side_of((float)(int32_t)ctx->r5));
}

// Entry of fontPrintXY(dList, x, y, text, flags): x from the left of the screen; a centred text stays.
extern "C" void jfg_hud_text(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g_on) return;
    const int32_t x = (int32_t)ctx->r5;
    push(x < 0 || x > g_width ? MIDDLE : side_of((float)(x - g_width / 2)));
}

// Entry of frontDrawRectangles(dList, count, rects, ...): rectangles x1, y1, x2, y2 (s16, from the screen's left) and
// a colour, 12 bytes each; the side of the middle of all of them.
extern "C" void jfg_hud_rectangles(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g_on) return;
    const int count = (int)(int32_t)ctx->r5;
    const uint32_t rects = (uint32_t)ctx->r6;
    int left = 1 << 20, right = -(1 << 20);
    for (int i = 0; i < count && i < 64 && valid_ptr(rects + i * 12); i++) {
        const int x1 = (int16_t)rd16(rects + i * 12), x2 = (int16_t)rd16(rects + i * 12 + 4);
        if (x2 < 0) continue;
        left = std::min(left, x1);
        right = std::max(right, x2);
    }
    push(left > right ? g_stack[0] : side_of((float)((left + right) / 2 - g_width / 2)));
}

// Entries of instDrawHealth (the health meter) and of the weapon panel (func_overlay_14_0000292C: the gun, its charge
// bar and ammo, and through func_overlay_14_00001AD8 the HUD's frames - the message panel that slides out with a
// pick-up's name, the objective panel with the tribals' heads and counts): each one block at the left edge, see push.
extern "C" void jfg_hud_health(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_on) return;
    push(LEFT);
    if (g_block == 0) g_block = g_depth;
}

extern "C" void jfg_hud_weapon_panel(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_on) return;
    push(LEFT);
    if (g_block == 0) g_block = g_depth;
}

// Entries of func_overlay_14_00000220 (the target's panel, drawn where the target is on screen) and _00000074 (the
// arrow at either side of the screen): not the HUD's left; their pieces still take their own side.
extern "C" void jfg_hud_middle(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    push(MIDDLE);
}

// The returns of the functions above.
extern "C" void jfg_hud_pop(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    pop();
}

// frontSingleInstruments's jetpack fuel meter (drawn there, around (-50, -80): frame, "FUEL", its bar and number): its
// start, and the point after it that every path reaches. It goes to the left edge as one block: its bar's middle is
// near the screen's, so on its own it stayed while the frame moved.
extern "C" void jfg_hud_air_begin(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_on || g_air_region) return;
    g_air_region = true;
    push(LEFT);
    if (g_block == 0) g_block = g_depth;
}

extern "C" void jfg_hud_air_end(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_air_region) return;
    g_air_region = false;
    pop();
}

namespace {
// fxDrawDigitalNumber's digits (fx.c, func_8006DF90 / func_8006DD20): 9 x 17 seven-segment cells 10 apart, each pixel
// of a row a 3-bit segment number (0: none); the digit's mask says which segments are lit. A lit segment adds 16 (of
// 31) to the pixel's green (red when `red`), an unlit one 4. Here the same pixels are runs of HUD rectangles, blended
// in at those strengths.
constexpr uint32_t ADDR_digitRows = 0x800A6B64;  // D_800A6B64: s32[17], nine 3-bit segment numbers per row
constexpr uint32_t ADDR_digitMasks = 0x800A6BA8; // D_800A6BA8: u8[12], 0-9, minus, blank
constexpr uint32_t RECT_SIZE = 12;               // FrontRectangle: s16 x1, y1, x2, y2, u32 colour
constexpr int RECTS_PER_CALL = 64;
}

// fxDrawDigitalNumber(x, y, value, digits, red) from frontSingleInstruments with the HUD at the edges: the number drawn
// through frontDrawRectangles (so in the fuel meter's block). False: leave it to the game.
bool draw_fuel_digits(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    if (!g_on || !g_air_region) return false;
    int x = (int)(int32_t)ctx->r4;
    const int y = (int)(int32_t)ctx->r5, value = (int)(int32_t)ctx->r6, width = (int)(int32_t)ctx->r7;
    const bool red = rd32((uint32_t)ctx->r29 + 0x10) != 0;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", value);
    int len = (int)std::strlen(buf);
    if (len > width) return true; // the game draws nothing either
    std::vector<uint8_t> masks;
    for (int i = 0; len < width || buf[i] != '\0';) {
        uint8_t mask = rd8(ADDR_digitMasks + 11);
        if (len < width) len++;
        else {
            const char c = buf[i++];
            if (c == '-') mask = rd8(ADDR_digitMasks + 10);
            else if (c >= '0' && c <= '9') mask = rd8(ADDR_digitMasks + (uint32_t)(c - '0'));
        }
        masks.push_back(mask);
    }
    const uint32_t tint = red ? 0xFF000000u : 0x00FF0000u, lit = tint | 132u, unlit = tint | 33u; // 16 and 4 of 31

    // The rectangles go below the stack (call_game's frame goes below them).
    recomp_context c = *ctx;
    c.r29 = (gpr)(int32_t)(((uint32_t)ctx->r29 - 0x800u) & ~0xFu);
    const uint32_t rects = (uint32_t)c.r29;
    int count = 0;
    auto flush = [&] {
        if (count > 0) call_game(frontDrawRectangles, &c, { ADDR_frontgfx, (uint32_t)count, rects, 0 });
        count = 0;
    };
    for (const uint8_t mask : masks) {
        for (int row = 0; row < 17; row++) {
            uint32_t bits = rd32(ADDR_digitRows + (uint32_t)row * 4);
            int run_start = -1;
            uint32_t run_colour = 0;
            for (int col = 0; col <= 9; col++) {
                const uint32_t segment = col < 9 ? (bits & 7) : 0;
                bits >>= 3;
                const uint32_t colour = segment == 0 ? 0 : (mask & (1u << segment)) ? lit : unlit;
                if (run_start >= 0 && colour != run_colour) {
                    const uint32_t r = rects + (uint32_t)count * RECT_SIZE;
                    wr16(r + 0, (uint16_t)(x + run_start));
                    wr16(r + 2, (uint16_t)(y + row));
                    wr16(r + 4, (uint16_t)(x + col));
                    wr16(r + 6, (uint16_t)(y + row + 1));
                    wr32(r + 8, run_colour);
                    if (++count == RECTS_PER_CALL) flush();
                    run_start = -1;
                }
                if (colour != 0 && run_start < 0) { run_start = col; run_colour = colour; }
            }
        }
        x += 10;
    }
    flush();
    return true;
}

}

// --- the wrappers (tools/wrapped_funcs.txt) --------------------------------------------------------------------------
// The HUD is marked from frontSingleInstruments's own camStandardOrtho (the first; the weapon strip's function before
// it sets one of its own) to the return of frontInstruments; the air meter from frontSingleInstruments's first own
// texDPInit to its first own sprSetTextureFilter, which every path reaches after it.
namespace {
JfgCaller g_single;    // frontSingleInstruments (frame 0xB0)
bool g_hud_begun = false;
}

JFG_WRAP(frontSingleInstruments) {
    // JFG_DEBUG_HUD_FUEL=1: the jetpack fuel meter stays shown, as while flying (player data + 0x592: its timer).
    static const bool fuel = std::getenv("JFG_DEBUG_HUD_FUEL") != nullptr;
    if (fuel && jfg::valid_ptr((uint32_t)ctx->r4)) {
        jfg::g_rdram = rdram;
        const uint32_t player = jfg::Obj::Data::get((uint32_t)ctx->r4);
        if (jfg::valid_ptr(player)) jfg::wr16(player + 0x592, 30);
    }
    JfgCaller::Scope scope(g_single, ctx, 0xB0);
    g_hud_begun = false;
    frontSingleInstruments_orig(rdram, ctx);
}

JFG_WRAP(camStandardOrtho) {
    const bool begin = g_single.direct(ctx) && !g_hud_begun;
    recomp_context args = *ctx;
    camStandardOrtho_orig(rdram, ctx);
    if (begin) {
        g_hud_begun = true;
        jfg::jfg_hud_begin(rdram, &args);
    }
}

JFG_WRAP(frontInstruments) {
    frontInstruments_orig(rdram, ctx);
    jfg::jfg_hud_end(rdram, ctx);
}

JFG_WRAP(texDPInit) {
    if (g_single.direct(ctx)) jfg::jfg_hud_air_begin(rdram, ctx);
    texDPInit_orig(rdram, ctx);
}

JFG_WRAP(fxDrawDigitalNumber) {
    if (g_single.direct(ctx) && jfg::draw_fuel_digits(rdram, ctx)) return;
    fxDrawDigitalNumber_orig(rdram, ctx);
}

JFG_WRAP(sprSetTextureFilter) {
    if (g_single.direct(ctx)) jfg::jfg_hud_air_end(rdram, ctx);
    sprSetTextureFilter_orig(rdram, ctx);
}

// The HUD's pieces: each entry takes its side, each return goes back to the one before.
#define JFG_HUD_PIECE(name, push)          \
    JFG_WRAP(name) {                       \
        push(rdram, ctx);                  \
        name##_orig(rdram, ctx);           \
        jfg::jfg_hud_pop(rdram, ctx);      \
    }
JFG_HUD_PIECE(frontDrawObj, jfg::jfg_hud_object)
JFG_HUD_PIECE(frontPrintNum, jfg::jfg_hud_number)
JFG_HUD_PIECE(fontPrintXY, jfg::jfg_hud_text)
JFG_HUD_PIECE(frontDrawRectangles, jfg::jfg_hud_rectangles)
JFG_HUD_PIECE(func_overlay_14_00000220_1F0DB78, jfg::jfg_hud_middle)       // the target's panel
JFG_HUD_PIECE(func_overlay_14_00000074_1F0D9CC, jfg::jfg_hud_middle)       // the arrows at the screen's sides
#undef JFG_HUD_PIECE

// The weapon panel (func_overlay_14_0000292C, with the message, objective and selector panels it draws): a HUD piece on
// the left, which a mod can take over (jfg_on_weapon_hud) and whose weapon icons it can place (jfg_weapon_hud.cpp).
bool jfg_weapon_hud_event(uint8_t* rdram, recomp_context* ctx); // jfg_weapon_hud.cpp
void jfg_weapon_hud_icons(uint8_t* rdram, recomp_context* ctx);
JFG_WRAP(func_overlay_14_0000292C_1F10284) {
    jfg::jfg_hud_weapon_panel(rdram, ctx);
    if (jfg_weapon_hud_event(rdram, ctx)) func_overlay_14_0000292C_1F10284_orig(rdram, ctx);
    jfg_weapon_hud_icons(rdram, ctx);
    jfg::jfg_hud_pop(rdram, ctx);
}

// instDrawHealth(player, health, segments, updateRate), the health meter: left out for a hero whose health a mod shows
// itself (jfg_hud_hide_health) or draws its own (jfg_on_health_draw), else a HUD piece as above.
bool jfg_health_event(uint8_t* rdram, recomp_context* ctx); // jfg_api.cpp
JFG_WRAP(instDrawHealth) {
    jfg::g_rdram = rdram;
    const uint32_t obj = (uint32_t)ctx->r4;
    const uint32_t data = jfg::valid_ptr(obj) ? jfg::Obj::Data::get(obj) : 0;
    if (jfg::valid_ptr(data) && jfg::api::hero_health_hidden(jfg::PlayerData::Character::get(data) & 3)) return;
    if (!jfg_health_event(rdram, ctx)) return; // a mod draws its own meter
    jfg::g_rdram = rdram;
    jfg::jfg_hud_health(rdram, ctx);
    instDrawHealth_orig(rdram, ctx);
    jfg::jfg_hud_pop(rdram, ctx);
}
