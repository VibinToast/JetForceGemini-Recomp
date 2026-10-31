// Small helpers for Jet Force Gemini: Recompiled mods, shared so mods don't each carry their own copy. All static
// inline: include it after jfg_api.h, and only what a mod uses ends up in it. Angles in the game's units are 0x10000
// a turn.
#ifndef JFG_UTIL_H
#define JFG_UTIL_H

#include "jfg_api.h"

// --- math ------------------------------------------------------------------------------------------------------------

// Sine of a game angle (0x10000 a turn), a fast approximation (about 0.001 off).
static inline float jfg_turn_sin(unsigned int a) {
    const float x = (float)(int)(short)(a & 0xFFFF) * (3.14159265f / 32768.0f);
    const float y = 1.27323954f * x - 0.405284735f * x * (x < 0 ? -x : x);
    return 0.225f * (y * (y < 0 ? -y : y) - y) + y;
}
static inline float jfg_turn_cos(unsigned int a) { return jfg_turn_sin(a + 0x4000); }
// The same in degrees.
static inline float jfg_deg_sin(float d) { return jfg_turn_sin((unsigned int)(int)(d * (65536.0f / 360.0f))); }
static inline float jfg_deg_cos(float d) { return jfg_deg_sin(d + 90.0f); }

// Square root (Newton's method, 16 steps: the same result in every mod).
static inline float jfg_sqrt(float v) {
    if (v <= 0.0f) return 0.0f;
    float r = v > 1.0f ? v : 1.0f;
    for (int i = 0; i < 16; i++) r = 0.5f * (r + v / r);
    return r;
}

// A random number 0..n-1 from the mod's own seed (any starting value).
static inline int jfg_rand(unsigned int* seed, int n) {
    *seed = *seed * 1103515245u + 12345u;
    return n > 0 ? (int)((*seed >> 16) % (unsigned int)n) : 0;
}

// An object's position (x, y, z) and speed (x, y, z: level units a frame).
static inline float* jfg_obj_pos(void* obj) { return (float*)((unsigned char*)obj + 0x0C); }
static inline float* jfg_obj_vel(void* obj) { return (float*)((unsigned char*)obj + 0x1C); }

// --- colours ---------------------------------------------------------------------------------------------------------

// 0xRRGGBB and an opacity 0..1 as the overlay's 0xRRGGBBAA.
static inline unsigned int jfg_rgba(unsigned int rgb, float a) {
    int v = (int)(a * 255.0f + 0.5f);
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return (rgb << 8) | (unsigned int)v;
}
// 0xRRGGBB a to b by t (0..1).
static inline unsigned int jfg_rgb_mix(unsigned int a, unsigned int b, float t) {
    const int r = (int)(((a >> 16) & 0xFF) + ((int)((b >> 16) & 0xFF) - (int)((a >> 16) & 0xFF)) * t);
    const int g = (int)(((a >> 8) & 0xFF) + ((int)((b >> 8) & 0xFF) - (int)((a >> 8) & 0xFF)) * t);
    const int bl = (int)((a & 0xFF) + ((int)(b & 0xFF) - (int)(a & 0xFF)) * t);
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)bl;
}

// --- overlay shapes and text (in jfg_on_draw_hud) -----------------------------------------------------------------------

#define JFG_DRAW_POINTS 512 // the most points one shape below draws (a full ring at 2 degrees a segment)
static float jfg_draw_xy[JFG_DRAW_POINTS * 2];

static inline void jfg_draw_rect(float x, float y, float w, float h, unsigned int fill) {
    float xy[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    JfgOverlayPolygon p = { xy, 4, fill, 0, 0 };
    jfg_overlay_polygon(&p);
}

// A box with corners of radius r, each corner steps segments (a pill when r is half the height).
static inline void jfg_draw_rounded(float x, float y, float w, float h, float r, int steps, unsigned int fill) {
    if (steps < 1) steps = 1;
    if (steps > JFG_DRAW_POINTS / 4 - 1) steps = JFG_DRAW_POINTS / 4 - 1;
    const float corners[4][3] = { { x + w - r, y + r, -90.0f }, { x + w - r, y + h - r, 0.0f },
                                  { x + r, y + h - r, 90.0f }, { x + r, y + r, 180.0f } };
    int k = 0;
    for (int c = 0; c < 4; c++) {
        for (int i = 0; i <= steps; i++) {
            const float a = corners[c][2] + 90.0f * i / steps;
            jfg_draw_xy[k++] = corners[c][0] + r * jfg_deg_cos(a);
            jfg_draw_xy[k++] = corners[c][1] + r * jfg_deg_sin(a);
        }
    }
    JfgOverlayPolygon p = { jfg_draw_xy, k / 2, fill, 0, 0 };
    jfg_overlay_polygon(&p);
}

// A band of a ring around cx, cy from angle a0 to a1 (degrees, clockwise from the right) between radii r0 and r1, a
// segment every step degrees.
static inline void jfg_draw_band(float cx, float cy, float a0, float a1, float r0, float r1, float step,
                                 unsigned int fill) {
    if (a1 - a0 < 0.5f) return;
    int n = (int)((a1 - a0) / (step > 0.1f ? step : 0.1f)) + 2;
    if (n > JFG_DRAW_POINTS / 2 - 1) n = JFG_DRAW_POINTS / 2 - 1;
    int k = 0;
    for (int i = 0; i <= n; i++) {
        const float a = a0 + (a1 - a0) * i / n;
        jfg_draw_xy[k++] = cx + r1 * jfg_deg_cos(a);
        jfg_draw_xy[k++] = cy + r1 * jfg_deg_sin(a);
    }
    for (int i = n; i >= 0; i--) {
        const float a = a0 + (a1 - a0) * i / n;
        jfg_draw_xy[k++] = cx + r0 * jfg_deg_cos(a);
        jfg_draw_xy[k++] = cy + r0 * jfg_deg_sin(a);
    }
    JfgOverlayPolygon p = { jfg_draw_xy, k / 2, fill, 0, 0 };
    jfg_overlay_polygon(&p);
}

static inline void jfg_draw_text(float x, float y, float size, unsigned int color, int align, int font, const char* s) {
    JfgOverlayText t = { x, y, size, color, align, font, s };
    jfg_overlay_text(&t);
}
static inline float jfg_text_width(float size, int font, const char* s) {
    JfgOverlayText t = { 0, 0, size, 0, 0, font, s };
    float w = 0.0f;
    jfg_overlay_text_width(&t, &w);
    return w;
}

// --- the HUD's pick-up messages (jfg_on_hud_message) -----------------------------------------------------------------

#define JFG_PICKUP_NONE     0
#define JFG_PICKUP_WEAPON   1 // a new weapon: its name
#define JFG_PICKUP_AMMO     2
#define JFG_PICKUP_CLIP     3
#define JFG_PICKUP_FULL     4 // full ammo
#define JFG_PICKUP_CAPACITY 5 // more capacity
// What a pick-up message says, and of which stock weapon (0..14).
static inline int jfg_pickup_message(int msg, int* weapon) {
    static const int first[5] = { 0x39, 0x48, 0x57, 0x66, 0x75 };
    for (int k = 0; k < 5; k++) {
        if (msg >= first[k] && msg < first[k] + 15) {
            if (weapon) *weapon = msg - first[k];
            return k + 1;
        }
    }
    if (weapon) *weapon = -1;
    return JFG_PICKUP_NONE;
}

// --- a debug line (in jfg_on_draw_hud) ---------------------------------------------------------------------------------

typedef struct {
    char text[256];
    int n;
} JfgLine;
static inline void jfg_line_str(JfgLine* l, const char* s) {
    while (*s && l->n < (int)sizeof(l->text) - 1) l->text[l->n++] = *s++;
    l->text[l->n] = 0;
}
static inline void jfg_line_num(JfgLine* l, int v) {
    char buf[12];
    int n = 0;
    if (v < 0) { jfg_line_str(l, "-"); v = -v; }
    do { buf[n++] = (char)('0' + v % 10); v /= 10; } while (v > 0 && n < 11);
    while (n > 0 && l->n < (int)sizeof(l->text) - 1) l->text[l->n++] = buf[--n];
    l->text[l->n] = 0;
}
// Drawn at the top left, as the official mods' Debug Line options do.
static inline void jfg_line_draw(const JfgLine* l) {
    jfg_draw_text(10.0f, 80.0f, 11.0f, 0x80FFFFFF, 0, 0, l->text);
}

#endif
