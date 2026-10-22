// Jet Force Gemini port: the camera - the dual-stick camera swung round the character, the over-the-shoulder aiming
// view, the boss-fight camera turn and the fly camera (F7). What moves it (sticks, mouse) is in jfg_controls.cpp.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

FlyCamera g_fly;
std::atomic<bool> g_fly_toggle_request{ false }; // F10 > Tools > Fly Camera (UI thread)

// --- fly camera ------------------------------------------------------------------------------------------------------
// Game thread, right after joyRead (from jfg_camera_controls). Returns true while the fly camera is on: it then owns
// this tick's input. Left stick / WASD: fly forward, back and sideways in the direction the camera looks; right stick
// / mouse: look; C-up (A, Space) up, C-down (B, Ctrl, C) down; holding fire (RT, left mouse button) flies 4x faster.
// The player character gets an empty controller meanwhile. jfg_camera_override puts the camera where it is.
bool fly_camera_tick(recomp_context* ctx) {
    // The HUD: the game's own "hide instruments" flag (frontSetInstrumentsHide, 0x800FF310), put back afterwards.
    constexpr uint32_t ADDR_instrumentsHide = 0x800FF310;
    static bool hud_hidden = false;
    static uint8_t hud_before = 0;
    auto show_hud = [&]() { if (hud_hidden) { wr8(ADDR_instrumentsHide, hud_before); hud_hidden = false; } };
    if (g_fly_toggle_request.exchange(false)) {
        g_fly.active = !g_fly.active;
        g_fly.started = false;
        std::fprintf(stderr, "[jfg] fly camera %s\n", g_fly.active ? "on" : "off");
    }
    if (!g_fly.active) { show_hud(); return false; }
    if (g_rdram == nullptr || rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || player_object(ctx) == 0) {
        // left the game proper (front end, pause menu, loading): the fly camera switches off
        g_fly.active = false;
        g_fly.started = false;
        show_hud();
        std::fprintf(stderr, "[jfg] fly camera off (not in gameplay)\n");
        return false;
    }
    if (g.fly_hide_hud) {
        if (!hud_hidden) { hud_before = rd8(ADDR_instrumentsHide); hud_hidden = true; }
        wr8(ADDR_instrumentsHide, 1);
    }
    else show_hud();
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    if (!g_fly.started) {
        g_fly.started = true;
        g_fly.x = rdf(cam + 0x0C); g_fly.y = rdf(cam + 0x10); g_fly.z = rdf(cam + 0x14);
        g_fly.heading = -angle_from_s16(rd16(cam));
        g_fly.pitch = angle_from_s16(rd16(cam + 2));
    }
    const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
    const uint32_t pad = ADDR_sControllerCurrData + 6u * i;
    const uint16_t buttons = rd16(pad);
    // The character stands still: nothing reaches the game this tick.
    wr16(pad, 0); wr8(pad + 2, 0); wr8(pad + 3, 0);
    wr16(ADDR_buttonsPressed + 2u * i, 0);

    const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    const float lx = g_stick_lx, ly = g_stick_ly, rx = g_stick_rx, ry = g_stick_ry;
    constexpr float YAW_RATE = 120.0f / 60.0f * (PI_F / 180.0f), PITCH_RATE = 90.0f / 60.0f * (PI_F / 180.0f);
    g_fly.heading = wrap_angle(g_fly.heading + g_sign_cam * (rx * YAW_RATE * frames * (float)g.camera_speed_h / 100.0f + g_mouse_cam_x));
    g_fly.pitch = std::clamp(g_fly.pitch + (g.invert_camera_y ? -ry : ry) * -PITCH_RATE * frames * (float)g.camera_speed_v / 100.0f - g_mouse_cam_y,
                             -1.45f, 1.45f);
    float speed = 5.0f * frames * (float)g.fly_speed / 100.0f;
    if (buttons & 0x2000) speed *= 4.0f; // fire held: fast
    const float ch = std::cos(g_fly.heading), sh = std::sin(g_fly.heading), cp = std::cos(g_fly.pitch), sp = std::sin(g_fly.pitch);
    const float up = ((buttons & 0x0008) ? 1.0f : 0.0f) - ((buttons & 0x0004) ? 1.0f : 0.0f);
    auto dz = [](float v) { return std::fabs(v) < 0.12f ? 0.0f : v; };
    const float f = dz(ly), r = dz(lx) * -g_sign_move; // stick right: towards screen right
    g_fly.x += (sh * cp * f - ch * r) * speed;
    g_fly.y += (-sp * f + up) * speed;
    g_fly.z += (ch * cp * f + sh * r) * speed;
    static const bool log = std::getenv("JFG_DEBUG_PLAYER") != nullptr;
    if (log && (g_tick_count % 10) == 0) std::fprintf(stderr, "[fly] pos %.0f %.0f %.0f heading %.0f pitch %.0f stick %.2f %.2f buttons %04X\n", g_fly.x, g_fly.y, g_fly.z, g_fly.heading * 180 / PI_F, g_fly.pitch * 180 / PI_F, lx, ly, buttons);
    return true;
}

// The Select / Esc settings menu is open (set by the UI thread every frame, src/ui/ui_state.cpp).
std::atomic<bool> g_settings_menu_open{ false };
extern "C" void jfg_settings_menu_open(int open) { g_settings_menu_open.store(open != 0, std::memory_order_relaxed); }

// Hook in func_80045F00 (0x80046058, where the game decides between running the world and its pause tick): 1 makes
// this tick a pause tick (objects drawn but not moved) without the pause menu. Used by the fly camera's "freeze" and
// by "Pause in the Settings Menu" (gameplay only: not in a cutscene, whose music would run on without it, or a skip).
extern "C" int jfg_freeze_world(void) {
    if (g_fly.active && g.fly_freeze) return 1;
    if (g.pause_in_menu && g_settings_menu_open.load(std::memory_order_relaxed) && g_rdram != nullptr && !g.skipping &&
        rd32(ADDR_mainGameMode) == 0 && !in_cutscene()) return 1;
    return 0;
}

// Start of the tick: give the game its own camera back.
void jfg_camera_restore() {
    if (!g_cam.overridden) return;
    g_cam.overridden = false;
    for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { uint32_t v; std::memcpy(&v, &g_cam.saved[i], 4); wr32(g_cam.camera_addr + i, v); }
}

// The game's follow camera keeps itself out of walls with func_8002CC70 (from the point it looks at, the wanted camera
// position is pulled in to the first wall in the way, and kept above the floor and below the ceiling). The dual-stick
// camera is placed by hand, so it asks the same function. (fx,fy,fz) is the point looked at, (tx,ty,tz) the wanted spot;
// the spot is moved in place. It snaps in when something is in the way and eases back out when the way is free.
void camera_collide(recomp_context* ctx, float fx, float fy, float fz, float& tx, float& ty, float& tz) {
    if (!g.camera_collision) { g_cam.collide_scale = 1.0f; return; }
    const uint32_t buf = ((uint32_t)ctx->r29 - 0x80u) & ~0x7u; // inside the gap call_game leaves below $sp
    wrf(buf + 0, fx); wrf(buf + 4, fy); wrf(buf + 8, fz);
    wrf(buf + 16, tx); wrf(buf + 20, ty); wrf(buf + 24, tz);
    call_game(func_8002CC70_2D870, ctx, { buf, buf + 16, 0u });
    const float rx = rdf(buf + 16), ry = rdf(buf + 20), rz = rdf(buf + 24);
    const float want = std::sqrt((tx - fx) * (tx - fx) + (ty - fy) * (ty - fy) + (tz - fz) * (tz - fz));
    const float got = std::sqrt((rx - fx) * (rx - fx) + (ry - fy) * (ry - fy) + (rz - fz) * (rz - fz));
    if (!(want > 1.0f) || !std::isfinite(got)) { g_cam.collide_scale = 1.0f; return; }
    // a little in front of the wall, so the near plane does not cut into it
    float free_scale = std::clamp((got - 8.0f) / want, 0.05f, 1.0f);
    if (got >= want - 0.5f) free_scale = 1.0f;
    g_cam.collide_scale = free_scale < g_cam.collide_scale ? free_scale : std::min(free_scale, g_cam.collide_scale + 0.05f);
    // the free part of the line to the spot; with the way clear, the height the game's check settled on (above the floor)
    const float sx = fx + (tx - fx) * g_cam.collide_scale, sy = fy + (ty - fy) * g_cam.collide_scale, sz = fz + (tz - fz) * g_cam.collide_scale;
    tx = sx; tz = sz;
    ty = (g_cam.collide_scale >= 1.0f) ? ry : sy;
}

// Right before the scene is drawn: move the camera round the character to the chosen angles.
extern "C" void jfg_camera_override(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    fov_refresh();
    if (g_fly.active && g_fly.started && !g_cam.overridden) {
        const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        wrf(cam + 0x0C, g_fly.x); wrf(cam + 0x10, g_fly.y); wrf(cam + 0x14, g_fly.z);
        wr16(cam + 0, angle_to_s16(-g_fly.heading));
        wr16(cam + 2, angle_to_s16(g_fly.pitch));
        wr16(cam + 4, 0);
        return;
    }
    if (g_boss_cam_turn != 0 && boss_control_active() && !g_cam.overridden) {
        // Boss fight, mouse and keyboard: the game's camera, turned on the spot by how far the aim has gone to the side.
        const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        wr16(cam + 0, (uint16_t)(rd16(cam + 0) - g_boss_cam_turn)); // camera angle = minus the heading it looks along
        return;
    }
    if ((!g_cam.active && !g_cam.shoulder) || g_cam.overridden) return;
    const uint32_t obj = player_object(ctx);
    if (obj == 0) return;
    const uint32_t cam = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    const float px = rdf(obj + 0x0C), py = rdf(obj + 0x10), pz = rdf(obj + 0x14);
    const float cx = rdf(cam + 0x0C), cy = rdf(cam + 0x10), cz = rdf(cam + 0x14);
    const float flat = std::sqrt((cx - px) * (cx - px) + (cz - pz) * (cz - pz));

    if (g_cam.shoulder) {
        // The game's own aiming camera glides, over about a second, to a point 50 units straight behind the character's
        // head and then trails the character's turns. Here the view is built directly from the character instead: at
        // head height, a little behind and to the right, looking exactly where the character aims, turned slightly
        // inwards so the middle of the screen meets the line of fire some way ahead. It takes a tenth of a second to
        // get there from wherever the game's camera is.
        const uint32_t data = rd32(obj + 0x68);
        if (!valid_ptr(data)) return;
        constexpr float SIDE = 18.0f, BACK = 40.0f, HEAD = 60.0f, CONVERGE = 700.0f;
        const float heading = angle_from_s16(rd16(data + PD_BODY_YAW)) + PI_F;
        // the game's own aiming camera pitch: view pitch plus 3/8 of the reticle's offset, positive looking down
        const float pitch = -(angle_from_s16(rd16(data + PD_VIEW_PITCH)) + 0.375f * angle_from_s16(rd16(data + PD_RETICLE_PITCH)));
        const float fx = std::sin(heading) * std::cos(pitch), fy = -std::sin(pitch), fz = std::cos(heading) * std::cos(pitch);
        // screen-right is heading - 90 degrees: (-cos, sin)
        const float tx = px - fx * BACK - std::cos(heading) * SIDE;
        const float ty = py + HEAD - fy * BACK;
        const float tz = pz - fz * BACK + std::sin(heading) * SIDE;
        const float look = heading + std::atan2(SIDE, CONVERGE);
        const float b = g_cam.shoulder_blend;
        float ctx_ = tx, cty_ = ty, ctz_ = tz;
        camera_collide(ctx, px, py + HEAD, pz, ctx_, cty_, ctz_);
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam;
        g_cam.overridden = true;
        const float game_look = -angle_from_s16(rd16(cam)), game_pitch = angle_from_s16(rd16(cam + 2));
        wrf(cam + 0x0C, cx + (ctx_ - cx) * b);
        wrf(cam + 0x10, cy + (cty_ - cy) * b);
        wrf(cam + 0x14, cz + (ctz_ - cz) * b);
        wr16(cam + 0, angle_to_s16(-(game_look + wrap_angle(look - game_look) * b)));
        wr16(cam + 2, angle_to_s16(game_pitch + (pitch - game_pitch) * b));
        return;
    }
    if (flat < 20.0f || flat > 2000.0f) return; // not an ordinary follow camera (first person, a fixed view, ...)
    const float game_elevation = angle_from_s16(rd16(cam + 2));
    // The game's camera looks at a point above the character's feet; keep looking at that point from the same distance.
    const float pivot = std::clamp((cy - py) - flat * std::tan(game_elevation), 0.0f, 200.0f);
    const float boom = std::sqrt(flat * flat + (cy - py - pivot) * (cy - py - pivot));
    // Stick up lowers the camera towards the ground behind the character; once it is nearly level it stays there
    // (so it cannot sink into the floor) and tilts upwards on the spot instead.
    const float pitch = std::clamp(game_elevation + g_cam.elevation, -0.45f, 1.25f);
    const float elevation = std::max(pitch, 0.06f);

    for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
    g_cam.camera_addr = cam;
    g_cam.overridden = true;
    float wx = px - std::sin(g_cam.heading) * boom * std::cos(elevation);
    float wy = py + pivot + boom * std::sin(elevation);
    float wz = pz - std::cos(g_cam.heading) * boom * std::cos(elevation);
    camera_collide(ctx, px, py + pivot, pz, wx, wy, wz);
    // The height eases towards the wanted one instead of jumping to it. The wanted height follows the character and
    // the game's camera, which step over uneven ground: in Mizar's Palace entrance (level 155) it jumped 5-22 units
    // in single ticks while walking, so the picture bobbed (a shake with Frame Interpolation). Rising is quick, so the
    // camera does not sink into the ground; sinking is slower. Per video frame, so the same at Game 30 and 60.
    {
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
        const bool fresh = g_cam.height_reset || g_cam.height_tick + 1 != g_tick_count || std::fabs(wy - g_cam.height) > 300.0f;
        if (fresh) {
            g_cam.height = wy;
        }
        else {
            const float keep = (wy > g_cam.height) ? 0.5f : 0.75f; // of the gap left after each video frame
            g_cam.height = wy + (g_cam.height - wy) * std::pow(keep, frames);
        }
        g_cam.height_reset = false;
        g_cam.height_tick = g_tick_count;
        wy = g_cam.height;
    }
    wrf(cam + 0x0C, wx);
    wrf(cam + 0x10, wy);
    wrf(cam + 0x14, wz);
    wr16(cam + 0, angle_to_s16(-g_cam.heading));
    wr16(cam + 2, angle_to_s16(pitch));
}

} // namespace jfg
