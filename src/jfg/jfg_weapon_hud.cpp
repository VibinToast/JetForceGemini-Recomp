// Jet Force Gemini port: the weapon panel for mods (mod_api/jfg_api.h, "the weapon panel").
//
// The game's weapon panel (func_overlay_14_0000292C, from frontSingleInstruments; overlay 14) draws the weapon frame,
// the message panel that slides out for pick-ups (front-end item 5, msgQueues' text), the objective panel that slides
// down when a Tribal is saved or lost (item 6: saved / killed / left), the selector that slides out on a weapon change
// with the owned weapons' icons (item 4), then the gun icon (front-end item 10 + weapon, drawn through currentobjects[1]),
// the charge bar (6 segments) and the ammo and max ammo numbers. Its wrapper (jfg_hud_corners.cpp) fires
// jfg_on_weapon_hud first: a mod that cancels it draws its own panel, all of it, with the overlay. The weapon icons stay
// the game's pictures (texture packs replace them): the mod asks for them with jfg_hud_weapon_icon and the port draws
// them after the event through the game's own frontDrawObj, at the place the mod gave, in overlay units.
//
// Also here: the game's texts (front_text, in the language shown: jfg_game_text) and the Tribal counts
// (jfg_tribal_counts).
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

extern "C" void frontDrawObj_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void loadFrontEndItem(uint8_t* rdram, recomp_context* ctx);
extern "C" void sprSetTextureFilter_orig(uint8_t* rdram, recomp_context* ctx);
extern "C" void controlGetMaxCharge(uint8_t* rdram, recomp_context* ctx);
extern "C" void texDPTextureX(uint8_t* rdram, recomp_context* ctx);
extern "C" void camCopyOrthoMatrix(uint8_t* rdram, recomp_context* ctx);
extern "C" void mathMtxF2L(uint8_t* rdram, recomp_context* ctx);
namespace jfg { bool hud_in_corners(); void hud_align_right(bool right); } // jfg_hud_corners.cpp
namespace jfg::api { uint32_t gfx_block(uint32_t bytes); } // jfg_api.cpp
namespace jfg::overlay { float overlay_width(); }        // jfg_overlay.cpp

namespace jfg {
namespace {
constexpr uint32_t ADDR_currentobjects = 0x800FF780; // FrontEndObject[], 0x20 bytes each
constexpr uint32_t ADDR_frontendptrs = 0x800FF3F8;   // void*[]: front-end items, NULL until loaded
constexpr uint32_t ADDR_objtrans = 0x800A51CC;       // s32: the opacity frontDrawObj draws with
constexpr uint32_t ADDR_front_text = 0x800A51C8;     // char**, NULL until initFront
constexpr uint32_t ADDR_frontgfx = 0x800FF398;       // Gfx*, the HUD's display list
// A FrontEndObject's item (s16), scale, x (from the screen's middle), y (from the middle, up).
constexpr uint32_t FO_ITEM = 0x06, FO_SCALE = 0x08, FO_X = 0x0C, FO_Y = 0x10;
constexpr int ICON_ITEM = 10;                         // front-end items 10..25: the weapons' icons
constexpr int WEAPONS = 16;

struct Icon {
    int weapon;
    float x, y, scale, alpha;
};
std::vector<Icon> g_icons; // asked for during jfg_on_weapon_hud, drawn after it

// Discs drawn into the HUD under the icons (jfg_hud_disc): a fan of SLICES triangles, CHUNK at a time (the microcode
// takes 16 triangles a command), in the graphics region (the renderer reads it), one set of buffers a frame of two.
struct Disc {
    float x, y, r;
    uint32_t rgba;
};
std::vector<Disc> g_discs;
constexpr int MAX_DISCS = 24, CHUNK = 16, SLICES = 64, CHUNKS = SLICES / CHUNK;
constexpr uint32_t VTX_SIZE = 10, CHUNK_VTX = CHUNK + 2, CHUNK_BYTES = 184; // centre + 17 rim points, 8-aligned
constexpr float FIX = 8.0f; // vertices in eighths of a unit (the matrix scales them back): a round edge
// [2 frames] { float mtx[16], Mtx (64) scaled, Mtx (64) as it was, vertices [MAX_DISCS][CHUNKS] }, then triangles
uint32_t g_buffers = 0;
constexpr uint32_t FRAME_BYTES = 64 + 64 + 64 + MAX_DISCS * CHUNKS * CHUNK_BYTES;
int g_frame = 0;

float f32_of(uint32_t bits) {
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}
uint32_t bits_of(float f) {
    uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

// jfg_hud_weapon_icon(const JfgWeaponIcon* icon): { int weapon; float x, y, scale, alpha; }, during
// jfg_on_weapon_hud.
void hud_weapon_icon(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    if (p == 0 || g_icons.size() >= 32) return;
    int weapon = (int)rd32(p);
    // Modded weapons: their big icon (sharp, the overlay), else slot 15's.
    if (weapon >= WEAPONS - 1 && weapons::draw_big_icon(weapon, f32_of(rd32(p + 4)), f32_of(rd32(p + 8)),
                                                         f32_of(rd32(p + 12)), f32_of(rd32(p + 16)))) return;
    if (weapon >= WEAPONS && weapons::known(weapon)) weapon = WEAPONS - 1;
    if (weapon < 0 || weapon >= WEAPONS) return;
    g_icons.push_back({ weapon, f32_of(rd32(p + 4)), f32_of(rd32(p + 8)), f32_of(rd32(p + 12)), f32_of(rd32(p + 16)) });
}

// jfg_hud_disc(const JfgHudDisc* disc): { float x, y, radius; unsigned int color; }, during jfg_on_weapon_hud.
void hud_disc(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t p = (uint32_t)ctx->r4;
    if (p == 0 || g_discs.size() >= (size_t)MAX_DISCS) return;
    const float r = f32_of(rd32(p + 8));
    if (r <= 0.0f) return;
    g_discs.push_back({ f32_of(rd32(p)), f32_of(rd32(p + 4)), r, rd32(p + 12) });
}

void emit(uint32_t w0, uint32_t w1) {
    const uint32_t gfx = rd32(ADDR_frontgfx);
    wr32(gfx, w0);
    wr32(gfx + 4, w1);
    wr32(ADDR_frontgfx, gfx + 8);
}

// The discs, as the game draws its own panels' dark backing (overlay 14's func_1AD8: no texture, its combiner and
// render mode, translucent), in the HUD's orthographic space.
// right_left: with the HUD in the corners, the 4:3 area's left when measured from the window's right edge (discs right
// of the middle are drawn that way, hud_align_right); < 0 otherwise.
void draw_discs(uint8_t* rdram_, recomp_context* ctx, float hud_left, float right_left, float middle) {
    if (g_buffers == 0) {
        g_buffers = jfg::api::gfx_block(FRAME_BYTES * 2 + CHUNK * 16);
        if (g_buffers == 0) return;
        const uint32_t tris = g_buffers + FRAME_BYTES * 2; // (centre, k, k + 1), both faces, no texture coordinates
        for (int k = 0; k < CHUNK; k++) wr32(tris + (uint32_t)k * 16, 0x40000000u | ((uint32_t)(k + 1) << 8) | (uint32_t)(k + 2));
    }
    g_frame ^= 1;
    const uint32_t base = g_buffers + (uint32_t)g_frame * FRAME_BYTES, mtxf = base, mtxl = base + 64;
    const uint32_t ortho = base + 128, verts = base + 192;
    const uint32_t tris = g_buffers + FRAME_BYTES * 2;
    call_game(camCopyOrthoMatrix, ctx, { mtxf });
    call_game(mathMtxF2L, ctx, { mtxf, ortho }); // to put back after: the icons and the rest of the HUD use it
    g_rdram = rdram_;
    for (uint32_t i = 0; i < 8; i++) wr32(mtxf + i * 4, bits_of(f32_of(rd32(mtxf + i * 4)) / FIX)); // rows x and y
    call_game(mathMtxF2L, ctx, { mtxf, mtxl });
    call_game(texDPTextureX, ctx, { ADDR_frontgfx, 0, 0, 0 });
    g_rdram = rdram_;
    emit(0xE7000000u, 0);                       // pipe sync
    emit(0xFC71FEE3u, 0xFFFEFB7Du);             // the panels' combiner
    emit(0xEF000C0Fu, 0x00504240u);             // ... and render mode
    emit(0x01000040u, mtxl);                    // the matrix, slot 0
    bool right = false;
    for (size_t d = 0; d < g_discs.size(); d++) {
        const Disc& disc = g_discs[d];
        const bool on_right = right_left >= 0.0f && disc.x > middle;
        if (on_right != right) {
            right = on_right;
            hud_align_right(right);
            g_rdram = rdram_;
        }
        emit(0xFA000000u, disc.rgba);           // the disc's colour (the combiner takes the primitive colour's)
        emit(0xFB000000u, disc.rgba);           // ... and opacity (the environment colour's alpha)
        const float gx = disc.x - (right ? right_left : hud_left) - 160.0f, gy = 120.0f - disc.y;
        for (int c = 0; c < CHUNKS; c++) {
            const uint32_t v = verts + ((uint32_t)d * CHUNKS + (uint32_t)c) * CHUNK_BYTES;
            for (uint32_t k = 0; k < CHUNK_VTX; k++) {
                float x = gx, y = gy;
                if (k > 0) {
                    const float a = (float)(c * CHUNK + (int)k - 1) * (6.2831853f / SLICES);
                    x += disc.r * std::cos(a);
                    y += disc.r * std::sin(a);
                }
                const uint32_t at = v + k * VTX_SIZE;
                wr16(at, (uint16_t)(int16_t)std::lround(x * FIX));
                wr16(at + 2, (uint16_t)(int16_t)std::lround(y * FIX));
                wr16(at + 4, 0);
                wr32(at + 6, 0xFFFFFFFFu);      // white: the combiner takes the environment colour
            }
            emit((0x04u << 24) | (((CHUNK_VTX << 3) | (v & 6)) << 16) | (CHUNK_VTX * VTX_SIZE + 8), v);
            emit((0x05u << 24) | ((((uint32_t)CHUNK - 1) << 4) << 16) | ((uint32_t)CHUNK * 16), tris);
        }
    }
    if (right) {
        hud_align_right(false);
        g_rdram = rdram_;
    }
    emit(0xE7000000u, 0);
    emit(0x01000040u, ortho);                   // the HUD's own matrix again
}

// jfg_game_text(int id, char* out, int size): front_text[id] (the game's texts, in the language shown) into out.
// Returns its length, 0 if there is none.
void game_text(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int id = (int)ctx->r4, size = (int)ctx->r6;
    const uint32_t out = (uint32_t)ctx->r5;
    ctx->r2 = 0;
    if (out == 0 || size <= 0) return;
    wr8(out, 0);
    const uint32_t table = rd32(ADDR_front_text);
    if (!valid_ptr(table) || id < 0 || id > 0x1000) return;
    const uint32_t text = rd32(table + (uint32_t)id * 4);
    if (!valid_ptr(text)) return;
    int n = 0;
    for (; n < size - 1; n++) {
        const uint8_t c = rd8(text + (uint32_t)n);
        if (c == 0) break;
        wr8(out + (uint32_t)n, c);
    }
    wr8(out + (uint32_t)n, 0);
    ctx->r2 = (gpr)n;
}

// jfg_tribal_counts(int* saved, int* killed, int* left): the area's Tribals (the counts the game's objective panel
// shows). Returns 0 outside a game.
void tribal_counts(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t game = rd32(ADDR_gameplay);
    const uint32_t out[3] = { (uint32_t)ctx->r4, (uint32_t)ctx->r5, (uint32_t)ctx->r6 };
    ctx->r2 = 0;
    if (!valid_ptr(game)) return;
    for (int i = 0; i < 3; i++) if (out[i] != 0) wr32(out[i], rd8(game + 4 + (uint32_t)i)); // tribalsSaved, Killed, Left
    ctx->r2 = 1;
}
} // namespace

void register_weapon_hud_api() {
    recomp::overlays::register_base_export("jfg_hud_weapon_icon", hud_weapon_icon);
    recomp::overlays::register_base_export("jfg_hud_disc", hud_disc);
    recomp::overlays::register_base_export("jfg_game_text", game_text);
    recomp::overlays::register_base_export("jfg_tribal_counts", tribal_counts);
}

} // namespace jfg

// The weapon panel's wrapper (jfg_hud_corners.cpp), before the game's code: jfg_on_weapon_hud. Returns false when a
// callback cancelled the game's panel. JfgWeaponHud: s32 weapon, ammo, max_ammo; f32 charge (0..1, -1 for a weapon
// without one); u32 owned (bit per weapon); s32 character, frames, cancel.
bool jfg_weapon_hud_event(uint8_t* rdram_, recomp_context* ctx) {
    using namespace jfg;
    g_icons.clear();
    g_discs.clear();
    if (!api::listened(api::Event::WeaponHud)) return true;
    g_rdram = rdram_;
    const int c = (int16_t)rd16(ADDR_currentCharacter);
    const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
    const uint32_t hero = player_object(ctx);
    g_rdram = rdram_;
    const uint32_t data = valid_ptr(hero) ? Obj::Data::get(hero) : 0;
    if (!valid_ptr(ch) || !valid_ptr(data)) return true;
    const gpr a0 = ctx->r4;
    const int weapon = Character::CurrentWeapon::get(ch) & 0xF;
    // The charge bar as the game fills it (0..252, 42 a segment): the pistol's heat, or the charge of the weapons that
    // charge (3, 4, 6), from full to empty as they charge up.
    float charge = -1.0f;
    if (weapon == 0) {
        charge = rd8(data + 0x542) / 252.0f;
    } else if (weapon == 3 || weapon == 4 || weapon == 6) {
        int max = (int32_t)call_game(controlGetMaxCharge, ctx, { ch, (uint32_t)weapon });
        g_rdram = rdram_;
        int value = (int16_t)rd16(data + 0x53E);
        if (weapon == 3) { max -= 32; value -= 32; }
        charge = max > 0 ? (252 - (value * 252) / max) / 252.0f : 0.0f;
    }
    if (charge > 1.0f) charge = 1.0f;
    if (charge < 0.0f && charge != -1.0f) charge = 0.0f;
    int32_t cancel;
    {
        api::Payload p(ctx, 0x20);
        wr32(p.addr + 0x00, (uint32_t)weapons::weapon_in_hand(ch, c)); // modded weapons: their ids (16 up)
        wr32(p.addr + 0x04, rd16(ch + 0x14 + (uint32_t)weapon * 2));
        wr32(p.addr + 0x08, rd16(ch + 0x34 + (uint32_t)weapon * 2));
        wr32(p.addr + 0x0C, bits_of(charge));
        wr32(p.addr + 0x10, rd16(ch + 0x0A));
        wr32(p.addr + 0x14, (uint32_t)c);
        wr32(p.addr + 0x18, (uint32_t)(int32_t)a0);
        wr32(p.addr + 0x1C, 0);
        api::fire(rdram_, ctx, api::Event::WeaponHud, (int32_t)p.addr);
        g_rdram = rdram_;
        cancel = (int32_t)rd32(p.addr + 0x1C);
    }
    ctx->r4 = a0;
    return cancel == 0;
}

// After the event (and the game's panel, if it drew): the discs the mod asked for, then the icons, drawn as the game
// draws its own (frontDrawObj through currentobjects[0], whose item, scale and place are put back after). Inside the
// weapon panel the HUD is aligned to its left side (HUD in Screen Corners) or in the middle 4:3 area, so places
// convert from overlay units from the HUD's left.
void jfg_weapon_hud_icons(uint8_t* rdram_, recomp_context* ctx) {
    using namespace jfg;
    if (g_icons.empty() && g_discs.empty()) return;
    g_rdram = rdram_;
    const float width = jfg::overlay::overlay_width();
    const float hud_left = hud_in_corners() ? 0.0f : (width - 320.0f) / 2.0f;
    const float right_left = hud_in_corners() ? width - 320.0f : -1.0f, middle = width / 2.0f;
    const gpr s0 = ctx->r4, s1 = ctx->r5;
    if (!g_discs.empty()) draw_discs(rdram_, ctx, hud_left, right_left, middle);
    g_discs.clear();
    ctx->r4 = s0;
    ctx->r5 = s1;
    g_rdram = rdram_;
    if (g_icons.empty()) return;
    const uint32_t obj = ADDR_currentobjects; // currentobjects[0]
    const uint16_t item0 = rd16(obj + FO_ITEM);
    const uint32_t scale0 = rd32(obj + FO_SCALE), x0 = rd32(obj + FO_X), y0 = rd32(obj + FO_Y);
    const uint32_t trans0 = rd32(ADDR_objtrans);
    const gpr a0 = ctx->r4, a1 = ctx->r5;
    call_game(sprSetTextureFilter_orig, ctx, { 0 });
    bool right = false;
    for (const Icon& icon : g_icons) {
        g_rdram = rdram_;
        const bool on_right = right_left >= 0.0f && icon.x > middle;
        if (on_right != right) {
            right = on_right;
            hud_align_right(right);
            g_rdram = rdram_;
        }
        const uint32_t item = (uint32_t)(ICON_ITEM + icon.weapon);
        if (rd32(ADDR_frontendptrs + item * 4) == 0) {
            call_game(loadFrontEndItem, ctx, { item });
            g_rdram = rdram_;
            if (rd32(ADDR_frontendptrs + item * 4) == 0) continue;
        }
        wr16(obj + FO_ITEM, (uint16_t)item);
        wr32(obj + FO_SCALE, bits_of(icon.scale));
        wr32(obj + FO_X, bits_of(icon.x - (right ? right_left : hud_left) - 160.0f));
        wr32(obj + FO_Y, bits_of(120.0f - icon.y));
        const float a = icon.alpha < 0.0f ? 0.0f : icon.alpha > 1.0f ? 1.0f : icon.alpha;
        wr32(ADDR_objtrans, (uint32_t)std::lround(a * 255.0f));
        call_game(frontDrawObj_orig, ctx, { 0 });
    }
    if (right) hud_align_right(false);
    g_rdram = rdram_;
    wr16(obj + FO_ITEM, item0);
    wr32(obj + FO_SCALE, scale0);
    wr32(obj + FO_X, x0);
    wr32(obj + FO_Y, y0);
    wr32(ADDR_objtrans, trans0);
    call_game(sprSetTextureFilter_orig, ctx, { 1 });
    g_icons.clear();
    ctx->r4 = a0;
    ctx->r5 = a1;
}
