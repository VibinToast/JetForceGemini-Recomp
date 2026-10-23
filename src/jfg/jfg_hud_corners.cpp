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
// HUD's orthographic viewport (camStandardOrtho's) is set again after each change. The lines and digits the CPU
// draws into the picture (fxDrawLine, fxDrawDigitalNumber: the target box, the air meter's number) are not moved.
#include <algorithm>
#include <cstdint>
#include <cstdlib>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

extern "C" int jfg_widescreen_active(void); // jfg_visuals.cpp
extern "C" void instShowBearBar(uint8_t* rdram, recomp_context* ctx); // overlay 14

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

// frontSingleInstruments's air meter (drawn there, around (-50, -80)): its start, and the point after it that every
// path reaches.
extern "C" void jfg_hud_air_begin(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_on || g_air_region) return;
    g_air_region = true;
    push(side_of(-50.0f));
}

extern "C" void jfg_hud_air_end(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (!g_air_region) return;
    g_air_region = false;
    pop();
}

}
