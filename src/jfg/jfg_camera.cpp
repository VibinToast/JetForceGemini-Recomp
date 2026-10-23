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
bool menu_paused() {
    return g.pause_in_menu && g_settings_menu_open.load(std::memory_order_relaxed) && g_rdram != nullptr && !g.skipping &&
           rd32(ADDR_mainGameMode) == 0 && !in_cutscene();
}
extern "C" int jfg_freeze_world(void) {
    if (g_fly.active && g.fly_freeze) return 1;
    return menu_paused() ? 1 : 0;
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
    if ((!g_cam.active && !g_cam.shoulder) || g_cam.overridden) return;
    const uint32_t cam_now = ADDR_cameraActorArray + CAMERA_SIZE * (rd32(ADDR_cameraNo) & 3u);
    if (menu_paused() && g_cam.drawn_valid) {
        // "Pause in the Settings Menu": the view stays exactly as last drawn (jfg_camera_controls does not run meanwhile).
        for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam_now + i); std::memcpy(&g_cam.saved[i], &v, 4); }
        g_cam.camera_addr = cam_now;
        g_cam.overridden = true;
        wrf(cam_now + 0x0C, g_cam.drawn_x); wrf(cam_now + 0x10, g_cam.drawn_y); wrf(cam_now + 0x14, g_cam.drawn_z);
        wr16(cam_now + 0, g_cam.drawn_yaw);
        wr16(cam_now + 2, g_cam.drawn_pitch);
        return;
    }
    auto remember = [](uint32_t c) {
        g_cam.drawn_x = rdf(c + 0x0C); g_cam.drawn_y = rdf(c + 0x10); g_cam.drawn_z = rdf(c + 0x14);
        g_cam.drawn_yaw = rd16(c); g_cam.drawn_pitch = rd16(c + 2);
        g_cam.drawn_valid = true;
    };
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
        remember(cam);
        return;
    }
    // The camera orbits the middle of the character's body, ORBIT_CENTRE above its feet, at ORBIT_DISTANCE (times
    // "Camera Distance"), and always looks at it, while aiming too (Over-the-Shoulder off). Only the starting tilt comes
    // from the game's camera: its distance moves in and out with aiming, walls and the start of a level, and does not
    // always come back to the same value (161 before an aim, 144 after, in the first level), so taking it made the
    // view, and every aim's zoom, differ from one time to the next with the same settings.
    float game_elevation, boom;
    if (g_cam.free_aim) {
        // Dual-stick aiming: the game's aiming camera has moved in close; the tilt from before is kept.
        game_elevation = g_cam.orbit_elevation;
        boom = ORBIT_DISTANCE;
    }
    else {
        if (flat < 20.0f || flat > 2000.0f) { g_cam.view_saved = false; g_cam.orbit_valid = false; return; } // not an ordinary follow camera (first person, a fixed view, ...)
        game_elevation = angle_from_s16(rd16(cam + 2));
        // The game's camera looks at a point above the character's feet: its distance from that point.
        const float game_pivot = std::clamp((cy - py) - flat * std::tan(game_elevation), 0.0f, 200.0f);
        const float game_boom = std::clamp(std::sqrt(flat * flat + (cy - py - game_pivot) * (cy - py - game_pivot)), 40.0f, 1000.0f);
        boom = ORBIT_DISTANCE;
        if (g_cam.orbit_valid && g_cam.orbit_easing) {
            // Just after aiming the game's camera is still in close and swings back out: the kept tilt stays until the
            // game's is near it again (at most orbit_settle frames), then eases over to it, so the view does not jump
            // when the aim button is let go.
            constexpr float EASE = 0.15f, NEAR = 0.25f;
            const bool back = std::fabs(game_boom - boom) < NEAR * boom;
            const float ease = (back || g_cam.orbit_settle <= 0) ? EASE : 0.0f;
            game_elevation = g_cam.orbit_elevation + (game_elevation - g_cam.orbit_elevation) * ease;
            if (g_cam.orbit_settle > 0) g_cam.orbit_settle--;
            if (ease > 0.0f && std::fabs(game_elevation - angle_from_s16(rd16(cam + 2))) < 0.01f) g_cam.orbit_easing = false;
        }
        g_cam.orbit_valid = true;
        g_cam.orbit_elevation = game_elevation;
        g_cam.orbit_boom = boom;
    }
    float pivot;
    {
        // The height of the point orbited follows the character's: closely on the ground (slopes, steps), gently in the
        // air, so a jump or a hover lifts the view smoothly rather than jerking it, and Lupus does not seem to drop when
        // his jets come on (issue #16). Per video frame, so the same at Game 30 and 60. Somewhere new: at once.
        const uint32_t data = rd32(obj + 0x68);
        const bool air = valid_ptr(data) && (rd8(data + PD_STATE) & ~0x20u) == PD_STATE_JUMP;
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
        const float target = py + ORBIT_CENTRE;
        const bool fresh = g_cam.follow_tick == 0 || g_cam.follow_tick + 1 != g_tick_count || std::fabs(target - g_cam.follow_look) > 300.0f;
        g_cam.follow_look = fresh ? target : target + (g_cam.follow_look - target) * std::pow(air ? 0.92f : 0.6f, frames);
        g_cam.follow_tick = g_tick_count;
        pivot = g_cam.follow_look - py;
        g_cam.orbit_pivot = pivot;
        static const bool log = std::getenv("JFG_DEBUG_FOLLOW_CAM") != nullptr; // tests: the height orbited and the character's
        if (log) std::fprintf(stderr, "[cam] tick %u placed %.1f air %d player y %.1f\n", g_tick_count, py + pivot, (int)air, py);
    }
    // Where the point orbited is across the ground: on the character, except while aiming with Over-the-Shoulder off,
    // when it has drag margins (as a 2D camera's): the character steps sideways across the middle of the screen, and the
    // point only follows once it is "Aim Drag Margin" of the half screen out, holding it there; all the while the point
    // also comes back towards it, moving or not. Turning the view while aiming leads the point the same way, sideways,
    // and tilting it up or down leads it up or down. Forwards and back it follows at once. After aiming it eases back.
    float qx = px, qz = pz;
    const float distance = (float)g.camera_distance / 100.0f; // "Camera Distance": the game's distance, scaled
    // Stick up lowers the camera round the character until it looks up from below; the floor, when in the way, pulls
    // it in along the same line (camera_collide).
    const float pitch = std::clamp(game_elevation + g_cam.elevation, ORBIT_PITCH_MIN, ORBIT_PITCH_MAX);
    {
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
        const float rx = -std::cos(g_cam.heading), rz = std::sin(g_cam.heading); // screen-right
        // The point is kept as an offset from the character, always sideways (along screen-right).
        float side = g_cam.orbit_dx * rx + g_cam.orbit_dz * rz, up = g_cam.orbit_dy;
        const bool fresh = !g_cam.orbit_drag_valid || std::fabs(px - g_cam.orbit_last_x) + std::fabs(pz - g_cam.orbit_last_z) > 300.0f;
        if (fresh) side = up = 0.0f;
        if (g_cam.free_aim && g_cam.glide <= 0.0f && !g.aim_shoulder) {
            // tan of half the view's width and height (about 74 and 46 degrees across)
            constexpr float HALF_SCREEN = 0.75f, HALF_SCREEN_UP = 0.42f;
            const float margin = (float)g.aim_drag_margin / 100.0f * HALF_SCREEN * boom * distance; // "Aim Drag Margin"
            const float margin_up = (float)g.aim_drag_margin / 100.0f * HALF_SCREEN_UP * boom * distance;
            // The character's sideways step since the last frame leaves the point where it was, up to the margin.
            const float step = (px - g_cam.orbit_last_x) * rx + (pz - g_cam.orbit_last_z) * rz;
            side -= step;
            // Turning the view does the same: the point goes the way the view turns, by the arc the turn sweeps at the
            // camera's distance, so the crosshair leads away from the character (the heading grows turning left).
            if (!fresh) side -= wrap_angle(g_cam.heading - g_cam.orbit_last_heading) * boom * distance;
            // and tilting it up or down (the pitch grows looking down) moves it up or down
            if (!fresh) up -= (pitch - g_cam.orbit_last_pitch) * boom * distance;
            // and all the while it comes back over ("Aim Drag Return Speed", 100%: in about a third of a second)
            const float keep = std::pow(0.9f, frames * (float)g.aim_drag_return / 100.0f);
            side = std::clamp(side * keep, -margin, margin);
            up = std::clamp(up * keep, -margin_up, margin_up);
        }
        else {
            const float keep = std::pow(0.85f, frames);
            side *= keep;
            up *= keep;
            if (std::fabs(side) < 0.5f) side = 0.0f;
            if (std::fabs(up) < 0.5f) up = 0.0f;
        }
        if (!std::isfinite(side)) side = 0.0f;
        if (!std::isfinite(up)) up = 0.0f;
        g_cam.orbit_dx = side * rx;
        g_cam.orbit_dz = side * rz;
        g_cam.orbit_last_x = px;
        g_cam.orbit_last_z = pz;
        g_cam.orbit_dy = up;
        g_cam.orbit_last_heading = g_cam.heading;
        g_cam.orbit_last_pitch = pitch;
        g_cam.orbit_drag_valid = true;
        qx = px + g_cam.orbit_dx;
        qz = pz + g_cam.orbit_dz;
        pivot += up;
    }
    // "Aim Offset" (Over-the-Shoulder off): while aiming the point orbited moves right/up of the character and the
    // camera closer in ("Aim Zoom") by the options, gliding there and back in a tenth of a second, so the view can be set up like
    // an over-the-shoulder one. The crosshair stays in the middle of the screen (jfg_aim_along_view uses the view drawn).
    float reach = boom * distance;
    {
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
        const bool on = g_cam.free_aim && g_cam.glide <= 0.0f && !g.aim_shoulder;
        g_cam.aim_offset_blend = on ? std::min(1.0f, g_cam.aim_offset_blend + frames / 6.0f) : std::max(0.0f, g_cam.aim_offset_blend - frames / 6.0f);
        const float b = g_cam.aim_offset_blend;
        const float right = (float)g.aim_offset_x * b;
        g_cam.aim_offset_wx = -std::cos(g_cam.heading) * right; // screen-right
        g_cam.aim_offset_wz = std::sin(g_cam.heading) * right;
        qx += g_cam.aim_offset_wx;
        qz += g_cam.aim_offset_wz;
        pivot += (float)g.aim_offset_y * b;
        reach = std::max(20.0f, reach - (float)g.aim_offset_z * b); // "Aim Zoom": closer in
        g_cam.orbit_pivot = pivot;
    }
    const float elevation = pitch;

    for (uint32_t i = 0; i < CAMERA_SIZE; i += 4) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
    g_cam.camera_addr = cam;
    g_cam.overridden = true;
    float wx = qx - std::sin(g_cam.heading) * reach * std::cos(elevation);
    float wy = py + pivot + reach * std::sin(elevation);
    float wz = qz - std::cos(g_cam.heading) * reach * std::cos(elevation);
    camera_collide(ctx, qx, py + pivot, qz, wx, wy, wz);
    float look = g_cam.heading, look_pitch = pitch;
    {
        // Looking at the point orbited from where the camera ended up (the game's check may have lifted it off the floor).
        const float dx = qx - wx, dy = py + pivot - wy, dz = qz - wz, d = std::sqrt(dx * dx + dz * dz);
        if (d + std::fabs(dy) > 10.0f) { look = std::atan2(dx, dz); look_pitch = std::atan2(-dy, d); }
    }
    g_cam.view_saved = true;
    g_cam.view_x = wx; g_cam.view_y = wy; g_cam.view_z = wz;
    g_cam.view_yaw = angle_to_s16(-look);
    g_cam.view_pitch = angle_to_s16(look_pitch);
    if (g_cam.glide > 0.0f) {
        // Over-the-shoulder aiming with the dual stick: glide to a spot at head height, a little behind and to the
        // right, still looking at a point well ahead on the view's centre line (near the one the character aims at,
        // see aim_at_screen_centre), so what is aimed at stays put while the camera moves in.
        constexpr float SIDE = 18.0f, BACK = 40.0f, HEAD = 60.0f, CONVERGE = 1000.0f;
        const float fx = std::sin(g_cam.heading) * std::cos(pitch), fy = -std::sin(pitch), fz = std::cos(g_cam.heading) * std::cos(pitch);
        const float tx = px + fx * CONVERGE, ty = py + pivot + fy * CONVERGE, tz = pz + fz * CONVERGE;
        // screen-right is heading - 90 degrees: (-cos, sin)
        const float sx = px - fx * BACK - std::cos(g_cam.heading) * SIDE;
        const float sy = py + HEAD - fy * BACK;
        const float sz = pz - fz * BACK + std::sin(g_cam.heading) * SIDE;
        const float shoulder_look = std::atan2(tx - sx, tz - sz);
        const float shoulder_pitch = -std::atan2(ty - sy, std::sqrt((tx - sx) * (tx - sx) + (tz - sz) * (tz - sz)));
        const float b = g_cam.glide;
        wx += (sx - wx) * b;
        wy += (sy - wy) * b;
        wz += (sz - wz) * b;
        look += wrap_angle(shoulder_look - look) * b;
        look_pitch += (shoulder_pitch - look_pitch) * b;
    }
    wrf(cam + 0x0C, wx);
    wrf(cam + 0x10, wy);
    wrf(cam + 0x14, wz);
    wr16(cam + 0, angle_to_s16(-look));
    wr16(cam + 2, angle_to_s16(look_pitch));
    remember(cam);
    // Dual-stick aiming: the aim again, along the view that is drawn now. The game's tick has run since the aim was
    // set before it, and on the tick aiming starts it has just reset its aim angles; the crosshair, drawn after the
    // scene from them, is then in the middle of the screen from the first frame on.
    // A shot this tick has added its spread to the crosshair's angles (the game's shot code, func_800386B0: +0x1CA/+0x1CC are the aim
    // +0x1C6/+0x1C8 plus a random kick - the machine gun's shake); it stays on top of the aim.
    if (g_cam.free_aim) {
        const uint32_t data = rd32(obj + 0x68);
        const bool aim_data = valid_ptr(data);
        const uint16_t kick_yaw = aim_data ? (uint16_t)(rd16(data + 0x1CA) - rd16(data + 0x1C6)) : 0;
        const uint16_t kick_pitch = aim_data ? (uint16_t)(rd16(data + 0x1CC) - rd16(data + 0x1C8)) : 0;
        jfg_aim_along_view(ctx, obj, wx, wy, wz, std::sin(look) * std::cos(look_pitch), -std::sin(look_pitch), std::cos(look) * std::cos(look_pitch));
        if (aim_data) {
            wr16(data + 0x1CA, (uint16_t)(rd16(data + 0x1C6) + kick_yaw));
            wr16(data + 0x1CC, (uint16_t)(rd16(data + 0x1C8) + kick_pitch));
        }
    }
}

// The first tick of aiming after the free camera: the game's own camera, still where it was (behind the character's
// old facing), is put where the free camera was showing, so the aiming view starts there instead of swinging round
// from the old facing (issue #14). While our view is still in RDRAM (overridden) the saved game values are replaced.
void camera_hand_over_to_aim(uint32_t cam) {
    if (!g_cam.view_saved) return;
    wrf(cam + 0x0C, g_cam.view_x);
    wrf(cam + 0x10, g_cam.view_y);
    wrf(cam + 0x14, g_cam.view_z);
    wr16(cam + 0, g_cam.view_yaw);
    wr16(cam + 2, g_cam.view_pitch);
    if (g_cam.overridden && g_cam.camera_addr == cam) {
        for (uint32_t i : { 0x00u, 0x0Cu, 0x10u, 0x14u }) { const uint32_t v = rd32(cam + i); std::memcpy(&g_cam.saved[i], &v, 4); }
    }
    g_cam.view_saved = false;
}

} // namespace jfg
