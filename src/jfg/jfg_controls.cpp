// Jet Force Gemini port: dual-stick, mouse and keyboard controls, the aiming options and the boss-fight controls.
//
// A mod, off by default. The game's own layout is from 1999: the stick runs forward/back and turns the character, the
// camera stays behind it, the C buttons sidestep (left/right), jump (up) and crouch (down); with R held the stick moves
// the reticle and all four C buttons move the character.
//
// With this option the two sticks work as in a modern third-person game:
//   walking: the right stick swings the camera around the character (and up/down); the left stick runs in the pushed
//            direction as seen from the camera;
//   aiming (R held): the character turns to face where the camera was looking (or, as an option, the view snaps to the
//            way the character faces); the right stick aims, the left stick moves through the four C buttons.
//
// How it is done, without changing the game's code:
//   - src/input/controls.cpp hands both sticks to jfg_dual_stick() when it builds the N64 controller state;
//   - jfg_camera_controls() runs on the game thread right after the game has read the controller (hook after joyRead).
//     The game's own movement is already camera-relative - relative to its own camera, which swings round behind the
//     character - so the left stick is rotated by the angle between the camera the player sees and the game's own and
//     written back as the game's stick value; the game's movement code then turns and runs the character as usual;
//   - jfg_camera_override() runs right before the scene is drawn (hook at the trackDraw call): it moves the active
//     camera, which the game keeps behind the character, to the angle the right stick has chosen, keeping the game's
//     own distance and looking at the same point. The game's own camera values are put back at the start of the next
//     tick (jfg_debug_frame), so the game's camera logic never sees the change.
// Angle conventions, measured in the game (heading = atan2(x, z)): the character runs along heading yaw + 180 degrees;
// the camera looks along heading -yaw; camera pitch is positive looking down; screen-right is heading - 90 degrees.
//
// The camera half (placing the camera, the fly camera) is in jfg_camera.cpp.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "debug_ui/debug_ui.hpp"
#include "input/input.hpp"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

// Sticks as last read by the input thread (jfg_dual_stick), up/right positive.
std::atomic<float> g_stick_lx{ 0 }, g_stick_ly{ 0 }, g_stick_rx{ 0 }, g_stick_ry{ 0 };
// "Jump while aiming" (aim_jump): the jump button as held in the dual-stick input, and the jump under way.
std::atomic<bool> g_jump_button{ false };     // dual-stick input: the jump button is held (input thread)
std::atomic<bool> g_aim_jump_active{ false }; // a jump from aiming is under way: R is held back (both threads)

CameraControl g_cam; // see jfg_port.h

// Boss fights: the player's boss control (boyBossControl / dogBossControl / girlBossControl) runs in place of the
// normal one; jfg_boss_tick (internal handler of jfg_on_boss_fight_control, jfg_api.cpp) notes the tick. See
// boss_controls().
uint32_t g_tick_count = 0;     // jfg_camera_controls calls
uint32_t g_boss_seen_tick = 0; // g_tick_count when the boss control last ran
bool boss_control_active() { return g_boss_seen_tick != 0 && g_tick_count - g_boss_seen_tick <= 2; }
// Boss fights with the mouse: how far the camera is turned in place (16-bit angle units: the aim's yaw offset beyond the box, same sign),
// set by boss_controls and applied by jfg_camera_override. 0 when not in use.
int g_boss_cam_turn = 0;

bool dual_stick_wanted() {
    static const bool env = std::getenv("JFG_DUAL_STICK") != nullptr; // for tests
    return g.dual_stick || g.mouse_keyboard || env;
}

// Mouse movement since the game thread last took it (input.cpp adds every SDL_MOUSEMOTION while the mouse is captured).
std::atomic<int> g_mouse_dx{ 0 }, g_mouse_dy{ 0 };
// Mouse per tick, in radians of camera turn / 16-bit angle units of aim, taken by jfg_camera_controls this tick.
float g_mouse_cam_x = 0, g_mouse_cam_y = 0;
int g_mouse_aim_x = 0, g_mouse_aim_y = 0;
constexpr float MOUSE_RADIANS_PER_COUNT = 0.0025f; // about 0.14 degrees per mouse count at 100%

// Handedness: on screen, "right" is the look heading minus 90 degrees, so turning right lowers the heading (measured:
// with these signs the character runs where the stick points and the view turns the way the stick is pushed).
// JFG_DUAL_STICK_SIGNS="move,camera" overrides them for tests.
float g_sign_move = -1.0f, g_sign_cam = -1.0f;

bool control_mods_blocked() { return in_cutscene() || rd8(ADDR_robotMission) != 0; } // see ADDR_robotMission

// Sprint (src/input/controls.cpp): the sprint button as last read by the input thread. With the option off the left
// stick click keeps its crouch in the dual-stick layout.
std::atomic<bool> g_sprint_held{ false };
extern "C" int jfg_sprint_enabled(void) { return g.sprint ? 1 : 0; }
extern "C" void jfg_sprint_input(int held) { g_sprint_held.store(held != 0, std::memory_order_relaxed); }

// Levels played with the game's own controls even with dual stick / mouse and keyboard on: the Mizar race (the ship
// steers with the stick, which the dual-stick camera and movement would take over) and the multiplayer races.
bool level_uses_original_controls(int level) {
    return level == 268 /* Mizar Racer */ || level == 258 /* Multi Race */ || level == 397 /* Multi Race II */;
}

extern "C" int jfg_dual_stick_active(void) {
    if (g_fly.active && g_rdram != nullptr) return 1; // the fly camera reads both sticks / WASD and the mouse
    if (!dual_stick_wanted() || g_rdram == nullptr) return 0;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || control_mods_blocked()) return 0;
    if (level_uses_original_controls((int16_t)rd16(ADDR_currentLevel))) return 0;
    return 1;
}

// 1 when dual-stick mode uses its own controller layout (src/input/controls.cpp) instead of the controller bindings.
extern "C" int jfg_dual_stick_layout(void) {
    return g.dual_layout ? 1 : 0;
}

// Floyd missions with mouse and keyboard on: the dual-stick controls stay off (they are built around the character, and
// a controller keeps the game's own Floyd controls), but the mouse and keyboard fly Floyd: their own keyboard layout
// (src/input/controls.cpp) and the mouse steering (jfg_floyd_steer).
extern "C" int jfg_floyd_mouse_active(void) {
    if (!g.mouse_keyboard || g_fly.active || g_rdram == nullptr) return 0;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || rd8(ADDR_robotMission) == 0 || in_cutscene()) return 0;
    return dino::debug_ui::game_has_input() ? 1 : 0;
}

// Mouse and keyboard (src/input/controls.cpp, src/input/input.cpp): 1 while the mouse and keyboard play the game -
// the option is on, the dual-stick controls are in charge (gameplay, not paused, no cutscene) or a Floyd mission is
// under way, and the debug UI is closed (or F10's toolbar is open but handed the mouse and keyboard to the game with
// Alt / a click outside it). The cursor is captured (relative mouse mode) exactly then.
extern "C" int jfg_mouse_keyboard_active(void) {
    if (jfg_floyd_mouse_active()) return 1;
    return (g.mouse_keyboard || g_fly.active) && jfg_dual_stick_active() && dino::debug_ui::game_has_input() ? 1 : 0;
}

std::atomic<int> g_mouse_wheel{ 0 };
// Wheel notches (up positive) while mouse and keyboard play; jfg_camera_controls turns them into weapon changes.
extern "C" void jfg_mouse_wheel(int notches) {
    // Not while the Select / Esc menu (or any other menu that takes the input) is open: the game behind it must not react.
    if (jfg_mouse_keyboard_active() && !dino::input::game_input_disabled()) g_mouse_wheel.fetch_add(notches, std::memory_order_relaxed);
}

extern "C" void jfg_mouse_motion(int dx, int dy) {
    {
        // JFG_DEBUG_MOUSE=1: every mouse report, with the time since the one before (does the mouse arrive in bursts?).
        static const bool log_reports = std::getenv("JFG_DEBUG_MOUSE") != nullptr;
        if (log_reports) {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[report] %.2f ms dx %d dy %d\n", std::chrono::duration<double, std::milli>(now - last).count(), dx, dy);
            last = now;
        }
    }
    if (dino::input::game_input_disabled()) return; // a menu is open: the mouse belongs to it, the camera and aim stay still
    g_mouse_dx.fetch_add(dx, std::memory_order_relaxed);
    g_mouse_dy.fetch_add(dy, std::memory_order_relaxed);
}

// Normal controls: 1 when the stick's up/down has to be flipped because the player is aiming (R held) and asked for it.
extern "C" int jfg_invert_aim_normal(uint16_t buttons) {
    if (!g.invert_aim_normal || dual_stick_wanted() || g_rdram == nullptr || (buttons & 0x0010) == 0) return 0;
    if (rd32(ADDR_mainGameMode) != 0 || rd8(ADDR_pauseMode) != 0 || control_mods_blocked()) return 0;
    return 1;
}

// Input thread. buttons, x, y: the N64 buttons and the (left) stick as bound, in and out. right_x/right_y: right stick,
// up positive. While walking the stick is passed on unchanged; jfg_camera_controls() replaces it on the game thread.
extern "C" void jfg_dual_stick(uint16_t* buttons, float* x, float* y, float right_x, float right_y) {
    constexpr uint16_t BTN_Z = 0x2000, BTN_R = 0x0010, C_UP = 0x0008, C_DOWN = 0x0004, C_LEFT = 0x0002, C_RIGHT = 0x0001;
    constexpr float DIGITAL = 0.5f; // stick deflection that counts as a C button press
    uint16_t b = *buttons;
    if (g.swap_triggers) {
        const bool z = (b & BTN_Z) != 0, r = (b & BTN_R) != 0;
        b = (uint16_t)((b & ~(BTN_Z | BTN_R)) | (z ? BTN_R : 0) | (r ? BTN_Z : 0));
    }
    // Tests without a controller: JFG_DUAL_STICK_TEST names a text file holding "lx,ly,rx,ry", read on every poll.
    static const char* const test_file = std::getenv("JFG_DUAL_STICK_TEST");
    if (test_file != nullptr) {
        if (FILE* f = std::fopen(test_file, "r")) {
            float v[4];
            if (std::fscanf(f, "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]) == 4) { *x = v[0]; *y = v[1]; right_x = v[2]; right_y = v[3]; }
            std::fclose(f);
        }
    }
    const float lx = *x, ly = *y;
    g_stick_lx = lx; g_stick_ly = ly; g_stick_rx = right_x; g_stick_ry = right_y;
    g_jump_button.store((b & C_UP) != 0, std::memory_order_relaxed); // the jump button itself, before the stick adds C-up
    if (g_aim_jump_active.load(std::memory_order_relaxed)) b &= (uint16_t)~BTN_R; // jumping from aiming (aim_jump)
    if ((b & BTN_R) != 0) {
        if (lx < -DIGITAL) b |= C_LEFT;
        if (lx > DIGITAL) b |= C_RIGHT;
        if (ly > DIGITAL) b |= C_UP;
        if (ly < -DIGITAL) b |= C_DOWN;
        // D-pad up/down while aiming: zoom in/out (C-up/C-down) instead of changing weapon.
        constexpr uint16_t D_UP = 0x0800, D_DOWN = 0x0400;
        if ((b & D_UP) != 0) b = (uint16_t)((b & ~D_UP) | C_UP);
        if ((b & D_DOWN) != 0) b = (uint16_t)((b & ~D_DOWN) | C_DOWN);
        *x = right_x;
        // The game itself aims like a flight stick (stick up = look down). Here up looks up unless "invert" is set.
        *y = g.invert_aim_y ? right_y : -right_y;
    }
    *buttons = b;
}

// A menu has taken the input (Select / Esc): the sticks and the jump button count as let go, as if their button-up had been
// received. Without this the values of the last poll stay in force and the character keeps walking.
extern "C" void jfg_dual_stick_release(void) {
    g_stick_lx = g_stick_ly = g_stick_rx = g_stick_ry = 0.0f;
    g_jump_button.store(false, std::memory_order_relaxed);
}

static bool aim_state(uint32_t obj) {
    const uint32_t data = Obj::Data::get(obj);
    return valid_ptr(data) && PlayerData::State::get(data) == PlayerData::StateAiming;
}

// "Jump while aiming" option. In the game, C-up while aiming (R held) zooms the sniper scope / steps forward, and
// the character can only jump from its normal state. With the option a press of the jump button while aiming is
// turned into the game's own jump: one tick with R let go (the aiming state ends the way it does when R is
// released), then one tick with C-up pressed (the normal state starts its jump, with its animation and sound). R stays
// let go until the jump is over, so the left stick steers the jump like a normal one (a running jump keeps its
// momentum) and the camera follows as when walking; with R still held the character aims again on landing.
// The jump button: in the dual-stick input C-up as bound / laid out (A, Space), not the C-up the left stick and the
// D-pad give while aiming (walking forward, zoom) - jfg_dual_stick() records it; otherwise the N64 C-up itself.
// Edits this tick's controller data and pressed-buttons word, after joyRead, like the mouse wheel does.

static void aim_jump(uint32_t obj, uint32_t pad, uint32_t pressed) {
    constexpr uint16_t BTN_R = 0x0010, C_UP = 0x0008;
    static int phase = 0, ticks = 0;
    static bool jump_was_held = false;
    const bool jump_held = g_jump_button.load(std::memory_order_relaxed);
    const bool jump_edge = jump_held && !jump_was_held;
    jump_was_held = jump_held;
    auto finish = [&]() { phase = 0; g_aim_jump_active.store(false, std::memory_order_relaxed); };
    if (!g.aim_jump) { finish(); return; }

    const uint16_t cur = rd16(pad), down = rd16(pressed);
    if (phase == 0) {
        const bool press = jfg_dual_stick_active() ? jump_edge : (down & C_UP) != 0;
        if (!aim_state(obj) || !press) return;
        phase = 1;
        g_aim_jump_active.store(true, std::memory_order_relaxed);
        wr16(pad, (uint16_t)(cur & ~(BTN_R | C_UP)));   // leave the aiming state
        wr16(pressed, (uint16_t)(down & ~(BTN_R | C_UP)));
        return;
    }

    if (phase == 1) {                                   // jump from the normal state
        wr16(pad, (uint16_t)((cur & ~BTN_R) | C_UP));
        wr16(pressed, (uint16_t)((down & ~BTN_R) | C_UP));
        phase = 2;
        ticks = 0;
        return;
    }

    // In the air: keep R let go until the jump state ends (or the jump never started).
    const uint32_t data = Obj::Data::get(obj);
    const int state = valid_ptr(data) ? PlayerData::State::get(data) : -1;
    ticks++;
    if ((state != PlayerData::StateJumping && ticks > 2) || ticks > 300) { finish(); return; }
    wr16(pad, (uint16_t)(cur & ~BTN_R));
    wr16(pressed, (uint16_t)(down & ~BTN_R));
}

// Applies the aim speed, and the "keep the reticle centered" option, to the stick the game is about to read.
//
// In the game's own aiming the stick first moves the reticle inside a box in the middle of the screen, the view only
// starts to turn once the reticle reaches the edge of the box, and the reticle drifts back to the middle when the
// stick is let go - taking the aim with it. Without the option only the stick is scaled, which slows both.
// With the option the game is given a centered stick, so its reticle offset stays at zero, and the stick turns the
// body direction and the view pitch directly, at a steady rate.
static void aim_controls(uint32_t obj, uint32_t pad, float frames) {
    const uint32_t data = Obj::Data::get(obj);
    const float speed = (float)g.aim_speed / 100.0f;
    const int raw_x = Pad::StickX::get(pad), raw_y = Pad::StickY::get(pad);
    if (!g.aim_centered && !jfg_mouse_keyboard_active()) { // the mouse always turns the view directly
        if (g.aim_speed != 100) {
            Pad::StickX::set(pad, (int8_t)std::clamp((int)std::lround(raw_x * speed), -80, 80));
            Pad::StickY::set(pad, (int8_t)std::clamp((int)std::lround(raw_y * speed), -80, 80));
        }
        return;
    }
    auto axis = [](int v) { // the game's own dead zone and range (joyClamp)
        const float a = (std::abs(v) - 6) / 64.0f;
        return a <= 0.0f ? 0.0f : std::min(a, 1.0f) * (v < 0 ? -1.0f : 1.0f);
    };
    // Full stick at 100%: 110 degrees a second sideways, 80 up/down. Stick right lowers the yaw; stick up lowers the
    // pitch value (which is the game's "push up to look down").
    constexpr float YAW_RATE = 110.0f / 60.0f * (32768.0f / 180.0f), PITCH_RATE = 80.0f / 60.0f * (32768.0f / 180.0f);
    constexpr int PITCH_LIMIT = 0x2AAA;
    // The mouse turns the view directly, by how far it was moved (already scaled, see jfg_camera_controls).
    const int yaw_step = (int)std::lround(axis(raw_x) * YAW_RATE * frames * speed) + g_mouse_aim_x;
    const int pitch_step = (int)std::lround(axis(raw_y) * PITCH_RATE * frames * speed) + g_mouse_aim_y;
    // Whatever offset the game still holds (from before the option was switched on) goes into the view first.
    const uint16_t body = (uint16_t)(PlayerData::BodyYaw::get(data) + PlayerData::ReticleYaw::get(data) - yaw_step);
    Obj::Yaw::set(obj, body);
    PlayerData::BodyYawCopy::set(data, body);
    PlayerData::BodyYaw::set(data, body);
    for (uint32_t k : { PlayerData::ReticleYaw::offset, PlayerData::ReticleYawCopy::offset, PlayerData::ReticleYawCopy2::offset }) wr16(data + k, 0);
    const int view = std::clamp((int)PlayerData::ViewPitch::get(data) + (int)PlayerData::ReticlePitch::get(data) - pitch_step, -PITCH_LIMIT, PITCH_LIMIT);
    PlayerData::ViewPitch::set(data, (int16_t)view);
    PlayerData::ReticlePitch::set(data, 0);
    Pad::StickX::set(pad, 0);
    Pad::StickY::set(pad, 0);
}

// --- boss fights -----------------------------------------------------------------------------------------------------
extern "C" void jfg_boss_tick(uint8_t*, recomp_context*) {
    g_boss_seen_tick = g_tick_count;
}

// --- moving platforms ------------------------------------------------------------------------------------------------
bool g_platform_move = false; // controlPlatform is carrying the player (its objMoveXYZ call), see jfg_move_scale
extern "C" void jfg_platform_move(int on) {
    g_platform_move = on != 0;
}

// Boss fights with the dual-stick / mouse and keyboard controls. In a boss fight the game runs a control of its own
// (boyBossControl / girlBossControl / dogBossControl, character state 32): the camera stays on the boss, the stick
// turns the character and aims (up/down: the aim's pitch), and C-left / C-right step round the arena. Measured in the
// Lurg fight (level 195): stick right turned the character and its reticle, C-left moved it 350 units sideways, the
// stick alone never moved it. The free camera and the camera-relative stick are off here; instead
//   left stick / A, D: step left and right (C-left / C-right), forward/back keep their buttons (jump, crouch);
//   right stick: the game's stick (turn and aim), up looks up unless "Invert Aim Up/Down" is set;
//   mouse: moves the aim directly, not through the stick; the body never turns (a rail).
void boss_controls(uint32_t obj, uint32_t pad, bool aiming) {
    (void)aiming;
    constexpr uint16_t C_LEFT = 0x0002, C_RIGHT = 0x0001;
    const float lx = g_stick_lx, rx = g_stick_rx, ry = g_stick_ry;
    uint16_t buttons = rd16(pad);
    buttons = (uint16_t)(buttons & ~(C_LEFT | C_RIGHT));
    if (lx < -0.4f) buttons |= C_LEFT;
    if (lx > 0.4f) buttons |= C_RIGHT;
    const uint32_t pressed = ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u);
    static uint16_t last = 0; // new C presses count as pressed this tick, as a real press would
    wr16(pressed, (uint16_t)((rd16(pressed) & ~(C_LEFT | C_RIGHT)) | (buttons & ~last & (C_LEFT | C_RIGHT))));
    last = buttons;
    wr16(pad, buttons);
    // Aim: the right stick and the mouse both move the aim directly (below); the game's own stick is left centred.
    // The mouse does not go through the stick: its movement comes in lumps (none on one tick, a lot on the next), and
    // as a stick deflection that made the reticle jump about and drift back towards the middle of the screen, so the
    // aim could not be held. It moves the aim directly, by exactly how far the mouse moved. The right stick is
    // treated the same way (at the rates of the "Keep the reticle centered" aiming, scaled by the aim speed), so the
    // controller works like the mouse here.
    //
    // In a boss fight the character is on a rail: it faces the boss, never turns, and only steps left and right
    // (C-left / C-right move it sideways relative to where it faces). So the mouse never turns the body - that is held
    // at the direction it had when the fight began (taken again whenever the boss control starts running anew). Left
    // and right move the reticle's offset from that direction instead, up and down tilt the view.
    constexpr int RETICLE_BOX = 0x1C71;   // 40 degrees: the aim may go this far to either side before the camera turns with it
    constexpr int RETICLE_LIMIT = 0x5555; // 120 degrees either side, as 16-bit angle units (some bosses move round the arena)
    constexpr int PITCH_LIMIT = 0x2AAA;
    static uint32_t last_obj = 0, last_tick = 0;
    static uint16_t centre = 0;
    static int reticle = 0;
    const uint32_t data = Obj::Data::get(obj);
    if (valid_ptr(data)) {
        if (obj != last_obj || g_tick_count - last_tick > 3) { last_obj = obj; centre = Obj::Yaw::get(obj); reticle = 0; }
        // Right stick as aim movement, with the game's own dead zone and range (joyClamp), as in aim_controls.
        auto axis = [](float v) {
            const float a = (std::abs(v) * 78.0f - 6.0f) / 64.0f;
            return a <= 0.0f ? 0.0f : std::min(a, 1.0f) * (v < 0 ? -1.0f : 1.0f);
        };
        constexpr float YAW_RATE = 110.0f / 60.0f * (32768.0f / 180.0f), PITCH_RATE = 80.0f / 60.0f * (32768.0f / 180.0f);
        const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6), speed = (float)g.aim_speed / 100.0f;
        const float stick_x = std::clamp(rx, -1.0f, 1.0f), stick_y = std::clamp(ry, -1.0f, 1.0f);
        const int yaw_step = (int)std::lround(axis(stick_x) * YAW_RATE * frames * speed) + g_mouse_aim_x;
        // stick up = look up unless "Invert Aim Up/Down" is set; the view pitch value goes down to look up
        const int pitch_step = (int)std::lround(axis(g.invert_aim_y ? stick_y : -stick_y) * PITCH_RATE * frames * speed) + g_mouse_aim_y;
        reticle = std::clamp(reticle - yaw_step, -RETICLE_LIMIT, RETICLE_LIMIT);
        Obj::Yaw::set(obj, centre);
        PlayerData::BodyYawCopy::set(data, centre);
        PlayerData::BodyYaw::set(data, centre);
        for (uint32_t k : { PlayerData::ReticleYaw::offset, PlayerData::ReticleYawCopy::offset, PlayerData::ReticleYawCopy2::offset }) {
            wr16(data + k, (uint16_t)(int16_t)reticle);
        }
        const int view = std::clamp((int)PlayerData::ViewPitch::get(data) + (int)PlayerData::ReticlePitch::get(data) - pitch_step, -PITCH_LIMIT, PITCH_LIMIT);
        PlayerData::ViewPitch::set(data, (int16_t)view);
        PlayerData::ReticlePitch::set(data, 0);
        Pad::StickX::set(pad, 0);
        Pad::StickY::set(pad, 0);
        // Past the box the camera turns on the spot, so the boss can be followed to the sides of the arena. It turns
        // about the player's position, never moving the player.
        // With "Keep the reticle centered" and a controller the camera follows the whole aim, so the reticle stays in
        // the middle of the screen.
        const int box = (!g.aim_centered || jfg_mouse_keyboard_active() != 0) ? RETICLE_BOX : 0;
        g_boss_cam_turn = reticle > box ? reticle - box : reticle < -box ? reticle + box : 0;
    }
    else {
        g_boss_cam_turn = 0;
    }
    last_tick = g_tick_count;
}

// --- Floyd missions --------------------------------------------------------------------------------------------------
// Floyd flies like a plane (sidekickControl, data = Floyd's object + 0x68): the stick sets a turn rate (+0x66) and a
// pitch rate (+0x68) that ease in and out and are added to the heading (+0x60, stick right lowers it) and the pitch
// (+0x62, clamped to 90 degrees either way); the roll (+0x64) eases towards the turn rate (-20 x rate). N64 A is thrust,
// B brakes, Z fires at the crosshair, which springs to where the stick points. In one mode (player data + 0x10D = 3) the
// pitch drifts back to level every tick. Through the stick the mouse would fight all that easing (its movement comes
// in lumps, as in the boss fights), so it turns and pitches Floyd directly: by exactly how far it moved, with the
// game's stick left as it is (the A / D keys and a controller still turn Floyd the game's way). The levelling drift is
// left out while the mouse flies, and the roll gets a bank of its own from the mouse's turn rate.
//
// sidekickControl's wrapper notes Floyd's data and frames; dAngle's wrapper does the steering around the roll easing.
namespace {
uint32_t g_sidekick_data = 0;   // Floyd's data (object + 0x68) while sidekickControl runs
int g_sidekick_frames = 0;      // its updateRate
int g_sidekick_dangles = -1;    // dAngle calls it has made; -1 outside it
uint32_t g_bank_data = 0, g_pitch_data = 0, g_pitch_tick = 0;
int g_bank_added = 0, g_pitch = 0;
float g_bank = 0.0f;
} // namespace

extern "C" void jfg_sidekick_begin(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    g_sidekick_data = valid_ptr(obj) ? Obj::Data::get(obj) : 0;
    g_sidekick_frames = std::clamp((int)ctx->r5, 1, 6);
    g_sidekick_dangles = 0;
}

extern "C" void jfg_sidekick_end(void) { g_sidekick_dangles = -1; }

// dAngle's wrapper, before the game's code. In a Floyd mission the first dAngle sidekickControl makes eases the roll
// towards the turn rate; then the bank added last tick is taken off (the roll argument and Floyd's data), so the game
// eases its own roll. Returns 1 for that call.
extern "C" int jfg_floyd_roll_before(uint8_t* rdram_, recomp_context* ctx) {
    if (g_sidekick_dangles < 0 || g_sidekick_dangles++ != 0) return 0;
    g_rdram = rdram_;
    const uint32_t data = g_sidekick_data;
    if (rd8(ADDR_robotMission) == 0 || !valid_ptr(data)) return 0;
    if (g_bank_added != 0 && data == g_bank_data) {
        SidekickData::Roll::set(data, (uint16_t)(SidekickData::Roll::get(data) - g_bank_added));
        ctx->r4 = (gpr)(int32_t)(int16_t)((int32_t)ctx->r4 - g_bank_added);
    }
    g_bank_added = 0;
    return 1;
}

// ... and after it, before sidekickControl copies heading, pitch and roll to Floyd's object: the mouse's turn and
// pitch go into his data, and its bank onto the eased roll (dAngle's result, which the game stores).
extern "C" void jfg_floyd_roll_after(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    constexpr int PITCH_LIMIT = 0x4000, ROLL_LIMIT = 520 * 20; // the game's own limits (full stick: rate 520)
    const uint32_t data = g_sidekick_data;
    if (!jfg_floyd_mouse_active()) { g_bank = 0.0f; g_pitch_data = 0; return; }
    const int frames = g_sidekick_frames;
    // Heading: right lowers it, as with the stick.
    SidekickData::Yaw::set(data, (uint16_t)(SidekickData::Yaw::get(data) - g_mouse_aim_x));
    // Pitch: carried on from what was written last tick plus what the game's own pitch rate added (a controller's stick),
    // so the levelling drift does not pull it back. Taken from the game afresh when Floyd or the mission is new.
    if (data != g_pitch_data || g_tick_count - g_pitch_tick > 1) g_pitch = SidekickData::Pitch::get(data);
    else g_pitch += SidekickData::PitchRate::get(data) * frames;
    static const int pitch_sign = std::getenv("JFG_FLOYD_PITCH_SIGN") ? std::atoi(std::getenv("JFG_FLOYD_PITCH_SIGN")) : 1; // tests
    g_pitch = std::clamp(g_pitch + pitch_sign * g_mouse_aim_y, -PITCH_LIMIT, PITCH_LIMIT);
    SidekickData::Pitch::set(data, (int16_t)g_pitch);
    g_pitch_data = data;
    g_pitch_tick = g_tick_count;
    // Bank: as the game banks for a turn rate (roll = -20 x rate per frame; the mouse's rate is minus its step per frame),
    // smoothed, since the mouse's steps come in lumps.
    const float target = std::clamp(20.0f * (float)g_mouse_aim_x / (float)frames, (float)-ROLL_LIMIT, (float)ROLL_LIMIT);
    g_bank += (target - g_bank) * std::min(1.0f, 0.2f * (float)frames);
    g_bank_added = (int)std::lround(g_bank);
    g_bank_data = data;
    ctx->r2 = (gpr)(int32_t)(int16_t)((int32_t)ctx->r2 + g_bank_added);
}

// Tests: JFG_DEBUG_PLAYER=<ticks> logs the player's state every that many ticks.
void debug_player_log(uint32_t obj) {
    {
        // Tests: JFG_DEBUG_HURT="secs,amount" takes that much health once (scaled by "Damage taken" the next tick).
        static const char* const hurt = std::getenv("JFG_DEBUG_HURT");
        static const auto start = std::chrono::steady_clock::now();
        static bool done = false;
        float t = 0; int amount = 0;
        if (hurt != nullptr && !done && obj != 0 && std::sscanf(hurt, "%f,%d", &t, &amount) == 2 &&
            std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count() >= t) {
            done = true;
            const uint32_t hit = Obj::Damage::get(obj);
            if (valid_ptr(hit)) {
                std::fprintf(stderr, "[player] hurt: health %d - %d\n", (int)Damage::Health::get(hit), amount);
                Damage::Health::set(hit, (int16_t)(Damage::Health::get(hit) - amount));
            }
        }
    }
    static const int every = std::getenv("JFG_DEBUG_PLAYER") ? std::max(1, std::atoi(std::getenv("JFG_DEBUG_PLAYER"))) : 0;
    if (every == 0 || obj == 0 || (g_tick_count % (uint32_t)every) != 0) return;
    const uint32_t data = Obj::Data::get(obj), hit = Obj::Damage::get(obj);
    const uint32_t pad = ADDR_sControllerCurrData + Pad::Size * (rd8(ADDR_sPlayerID) & 3u);
    std::fprintf(stderr, "[player] tick %u level %d boss %d state %d anim %d pos %.1f %.1f %.1f yaw %d pad %04X %d %d hp %d dyaw %d rpitch %d vpitch %d 577 %d zoom %.2f fuel %d\n",
        g_tick_count, (int)(int16_t)rd16(ADDR_currentLevel), int(boss_control_active()), valid_ptr(data) ? PlayerData::State::get(data) : -1,
        (int)Obj::Anim::get(obj), Obj::PosX::get(obj), Obj::PosY::get(obj), Obj::PosZ::get(obj), (int)(int16_t)Obj::Yaw::get(obj),
        Pad::Buttons::get(pad), (int)Pad::StickX::get(pad), (int)Pad::StickY::get(pad), valid_ptr(hit) ? (int)Damage::Health::get(hit) : -1,
        valid_ptr(data) ? (int)PlayerData::ReticleYaw::get(data) : 0, valid_ptr(data) ? (int)PlayerData::ReticlePitch::get(data) : 0,
        valid_ptr(data) ? (int)PlayerData::ViewPitch::get(data) : 0, valid_ptr(data) ? (int)PlayerData::ThrowLock::get(data) : 0,
        valid_ptr(data) ? PlayerData::AimZoom::get(data) : 0.0f, valid_ptr(data) ? (int)PlayerData::Fuel::get(data) : 0);
}

// Game thread, right after joyRead. Decides whether the right stick owns the camera this tick, advances the camera
// angles, turns the character and rewrites the game's stick value.
void jfg_camera_controls(recomp_context* ctx) {
    static bool signs_read = false;
    if (!signs_read) {
        signs_read = true;
        if (const char* e = std::getenv("JFG_DUAL_STICK_SIGNS")) std::sscanf(e, "%f,%f", &g_sign_move, &g_sign_cam);
    }
    const bool was_active = g_cam.active;
    g_cam.active = false;
    g_cam.shoulder = false;
    {
        // Mouse movement for this tick (always taken, so nothing piles up in menus).
        const int mx = g_mouse_dx.exchange(0, std::memory_order_relaxed), my = g_mouse_dy.exchange(0, std::memory_order_relaxed);
        // JFG_DEBUG_MOUSE=1 logs each tick's mouse movement and the time since the tick before (smoothness tests).
        static const bool log_mouse = std::getenv("JFG_DEBUG_MOUSE") != nullptr;
        if (log_mouse) {
            static auto last = std::chrono::steady_clock::now();
            const auto now = std::chrono::steady_clock::now();
            std::fprintf(stderr, "[mouse] %.2f ms dx %d dy %d\n", std::chrono::duration<double, std::milli>(now - last).count(), mx, my);
            last = now;
        }
        const bool mouse = jfg_mouse_keyboard_active() != 0 && !dino::input::game_input_disabled();
        const float cam = MOUSE_RADIANS_PER_COUNT * (float)g.mouse_camera_speed / 100.0f;
        const float aim = MOUSE_RADIANS_PER_COUNT * (32768.0f / PI_F) * (float)g.mouse_aim_speed / 100.0f;
        const float up = g.invert_mouse_y ? -1.0f : 1.0f; // mouse forward (SDL: negative y) looks up
        g_mouse_cam_x = mouse ? (float)mx * cam : 0.0f;
        g_mouse_cam_y = mouse ? -(float)my * cam * up : 0.0f;
        // aim: right lowers the yaw like the stick; forward = the stick pushed up, which (not inverted) lowers the step
        g_mouse_aim_x = mouse ? (int)std::lround((float)mx * aim) : 0;
        g_mouse_aim_y = mouse ? (int)std::lround((float)my * aim * up) : 0;
    }
    g_tick_count++;
    if (fly_camera_tick(ctx)) { g_cam.player = 0; g_cam.was_aiming = false; return; }
    const bool gameplay = g_rdram != nullptr && rd32(ADDR_mainGameMode) == 0 && rd8(ADDR_pauseMode) == 0 && !control_mods_blocked() &&
        !level_uses_original_controls((int16_t)rd16(ADDR_currentLevel)); // races: no control changes at all
    const uint32_t obj = gameplay ? player_object(ctx) : 0;
    g_cam.player = obj;
    apply_damage_taken(obj);
    debug_player_log(obj);
    g_cam.move_factor = (float)g.move_speed / 100.0f;
    g_cam.sprint_factor = 1.0f;
    static const bool sprint_test = std::getenv("JFG_DEBUG_SPRINT") != nullptr; // tests: sprint on and held
    if (((g.sprint && g_sprint_held.load(std::memory_order_relaxed)) || sprint_test) && obj != 0 && !dino::input::game_input_disabled()) {
        // On foot, and while aiming (the aim walk) only with "Sprint while aiming". Swimming (Vela, state 4: in the
        // water she is always in it, at the surface and below) has its own option and speed; jfg_move_scale applies
        // it to the whole 3D step, as it does the Dive speed.
        const uint32_t data = Obj::Data::get(obj);
        const int state = valid_ptr(data) ? PlayerData::State::get(data) : -1;
        if (state == PlayerData::StateSwimming) {
            if (g.sprint_swimming) g_cam.sprint_factor = (float)g.sprint_swim_speed / 100.0f;
        }
        else if (state >= 0 && (state != PlayerData::StateAiming || g.sprint_aiming)) {
            g_cam.sprint_factor = (float)g.sprint_speed / 100.0f;
            g_cam.move_factor *= g_cam.sprint_factor;
        }
    }
    {
        // Mouse wheel: each notch is one press of the game's own weapon buttons (N64 A: next, B: previous - what RB /
        // LB give in the dual-stick layout), so the switch has its sound and the weapon list scrolls on screen. The
        // press is put straight into this tick's controller data (after joyRead, as a real press would arrive), one
        // per tick: a flick of several notches steps through several weapons in as many ticks. Nothing is kept
        // waiting - at most 2 further notches are remembered, and turning the wheel the other way drops them.
        static int wheel_pending = 0;
        const int notches = g_mouse_wheel.exchange(0, std::memory_order_relaxed);
        if (notches != 0) {
            if ((notches > 0) != (wheel_pending > 0)) wheel_pending = 0;
            wheel_pending = std::clamp(wheel_pending + notches, -3, 3);
        }
        if (wheel_pending != 0 && (obj == 0 || !jfg_mouse_keyboard_active())) wheel_pending = 0;
        if (wheel_pending != 0) {
            const uint16_t button = wheel_pending < 0 ? 0x8000 : 0x4000; // wheel down: A (next), up: B (previous)
            wheel_pending += wheel_pending < 0 ? 1 : -1;
            const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
            const uint32_t pad = ADDR_sControllerCurrData + Pad::Size * i;
            wr16(pad, (uint16_t)(rd16(pad) | button));
            wr16(ADDR_buttonsPressed + 2u * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2u * i) | button));
            static const bool log = std::getenv("JFG_DEBUG_WHEEL") != nullptr;
            const int c = (int16_t)rd16(ADDR_currentCharacter);
            const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
            if (log) std::fprintf(stderr, "[jfg] wheel: press %s (weapon %d, %d more)\n", button == 0x8000 ? "A" : "B", ch ? Character::CurrentWeapon::get(ch) : -1, wheel_pending);
        }
    }
    if (obj == 0) { g_cam.was_aiming = false; return; }
    const uint32_t pad = ADDR_sControllerCurrData + Pad::Size * (rd8(ADDR_sPlayerID) & 3u);
    aim_jump(obj, pad, ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u));
    const bool aiming = (rd16(pad) & 0x0010) != 0;

    // The aiming options work with any control layout.
    const float tick_frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    if (aim_state(obj)) {
        g_cam.shoulder = g.aim_shoulder;
        g_cam.shoulder_blend = g_cam.shoulder ? std::min(1.0f, g_cam.shoulder_blend + tick_frames / 6.0f) : 0.0f;
        aim_controls(obj, pad, tick_frames);
    }
    else {
        g_cam.shoulder_blend = 0.0f;
    }
    {
        const int level = (int16_t)rd16(ADDR_currentLevel);
        const float x = Obj::PosX::get(obj), z = Obj::PosZ::get(obj);
        const float moved = std::sqrt((x - g_cam.last_x) * (x - g_cam.last_x) + (z - g_cam.last_z) * (z - g_cam.last_z));
        if (level != g_cam.level || obj != g_cam.last_player || moved > 400.0f) g_cam.face_reset = true;
        g_cam.level = level; g_cam.last_player = obj; g_cam.last_x = x; g_cam.last_z = z;
        // JFG_DEBUG_ARRIVE=1 logs the first 120 ticks in each new place: the character, the game's camera and ours.
        static const bool log_arrive = std::getenv("JFG_DEBUG_ARRIVE") != nullptr;
        static int arrive_ticks = 1000;
        if (g_cam.face_reset) arrive_ticks = 0;
        if (log_arrive && arrive_ticks < 120) {
            const uint32_t c = ADDR_cameraActorArray + Camera::Size * (rd32(ADDR_cameraNo) & 3u);
            const uint32_t data = Obj::Data::get(obj);
            std::fprintf(stderr, "[arrive] %d lvl %d obj %08X pos %.0f %.0f facing %.1f game %.1f cam %.0f %.0f free %.1f act %d show %d reset %d st %d stick %d %d L %.2f %.2f\n",
                arrive_ticks, level, obj, x, z, wrap_angle(angle_from_s16(Obj::Yaw::get(obj)) + PI_F) * 180.0f / PI_F,
                -angle_from_s16(Camera::Yaw::get(c)) * 180.0f / PI_F, Camera::PosX::get(c), Camera::PosZ::get(c), g_cam.heading * 180.0f / PI_F, (int)was_active,
                g_cam.showing, (int)g_cam.face_reset, valid_ptr(data) ? PlayerData::State::get(data) : -1, (int)Pad::StickX::get(pad), (int)Pad::StickY::get(pad),
                (float)g_stick_lx, (float)g_stick_ly);
        }
        arrive_ticks++;
    }
    if (!jfg_dual_stick_active()) { g_cam.was_aiming = false; return; }
    if (boss_control_active()) { boss_controls(obj, pad, aiming); g_cam.was_aiming = false; return; }
    const uint32_t cam = ADDR_cameraActorArray + Camera::Size * (rd32(ADDR_cameraNo) & 3u);
    const float game_heading = -angle_from_s16(Camera::Yaw::get(cam)); // where the game's own camera looks

    // The game sometimes takes the camera to show something - the door that opens once a room has been cleared, for
    // instance. Its follow camera always looks towards the character; a camera that is pointed well away from it (or
    // has been moved far off) is such a shot, and the right stick leaves the camera to the game until it is back.
    // The left stick goes to the game unchanged meanwhile, and the free camera restarts from the game's afterwards.
    {
        const float px = Obj::PosX::get(obj), pz = Obj::PosZ::get(obj), cx = Camera::PosX::get(cam), cz = Camera::PosZ::get(cam);
        const float flat = std::sqrt((px - cx) * (px - cx) + (pz - cz) * (pz - cz));
        const float off = flat < 30.0f ? 0.0f : std::fabs(wrap_angle(std::atan2(px - cx, pz - cz) - game_heading));
        const bool away = (flat > 2000.0f || off > 50.0f * PI_F / 180.0f) && g_cam.arrive_hold <= 0 && !g_cam.face_reset;
        // a couple of ticks of grace, so a single odd frame (a wall pushing the camera in) does not let go
        g_cam.showing = away ? g_cam.showing + 1 : 0;
        static const bool log_cam = std::getenv("JFG_DEBUG_CAM") != nullptr;
        if (log_cam) {
            std::fprintf(stderr, "[cam] level %d flat %.0f off %.1f showing %d game %.1f facing %.1f free %.1f\n", g_cam.level, flat,
                         off * 180.0f / PI_F, g_cam.showing, game_heading * 180.0f / PI_F,
                         wrap_angle(angle_from_s16(Obj::Yaw::get(obj)) + PI_F) * 180.0f / PI_F, g_cam.heading * 180.0f / PI_F);
        }
        if (g_cam.showing >= 2) { g_cam.was_aiming = false; return; }
    }

    if (aiming) {
        // The game's aiming view takes over; it looks the way the character is facing. By default the character is
        // first turned to face where the free camera was looking, so the aim starts on what the player was looking
        // at; with the option, the character is left alone and the view jumps round to its facing instead.
        // While aiming, follow the game's camera so the free camera continues from it afterwards.
        if (!g_cam.was_aiming && was_active && !g.aim_from_character) {
            const uint16_t yaw = angle_to_s16(g_cam.heading + PI_F);
            Obj::Yaw::set(obj, yaw);
            const uint32_t data = Obj::Data::get(obj);
            // the object's yaw and the copies the control code keeps and writes back every tick
            if (valid_ptr(data)) { for (uint32_t k : { PlayerData::BodyYawCopy::offset, PlayerData::BodyYaw::offset, PlayerData::BodyYawCopy2::offset, PlayerData::BodyYawCopy3::offset, PlayerData::BodyYawCopy4::offset }) {
                    wr16(data + k, yaw);
                } }
        }
        g_cam.was_aiming = true;
        g_cam.heading = game_heading;
        g_cam.elevation = 0;
        return;
    }
    const float frames = (float)std::clamp((int)rd32(ADDR_delayDat), 1, 6);
    const float lx = g_stick_lx, ly = g_stick_ly, rx = g_stick_rx, ry = g_stick_ry;
    if (g_cam.face_reset) { // somewhere new: start behind the character, looking the way it faces
        g_cam.heading = wrap_angle(angle_from_s16(Obj::Yaw::get(obj)) + PI_F);
        g_cam.elevation = 0;
        g_cam.face_reset = false;
        g_cam.arrive_hold = 60;
    }
    else if (g_cam.arrive_hold > 0) {
        // Until the player turns the camera, it keeps to the character's facing - only while the left stick is not
        // pushed sideways (the character turns to the stick, and a camera following it would turn it round in circles).
        if (std::fabs(rx) > 0.2f || std::fabs(ry) > 0.2f || g_mouse_cam_x != 0.0f || g_mouse_cam_y != 0.0f) g_cam.arrive_hold = 0;
        else if (std::fabs(lx) < 0.3f) g_cam.heading = wrap_angle(angle_from_s16(Obj::Yaw::get(obj)) + PI_F);
        g_cam.arrive_hold -= (int)frames;
    }
    else if (!was_active || g_cam.was_aiming) { // (re)start from the game's camera
        g_cam.heading = game_heading;
        g_cam.elevation = 0;
    }
    g_cam.was_aiming = false;
    g_cam.active = true;

    // Right stick: 150 degrees a second sideways, 75 up/down, at full deflection.
    constexpr float YAW_RATE = 150.0f / 60.0f * (PI_F / 180.0f), PITCH_RATE = 75.0f / 60.0f * (PI_F / 180.0f);
    g_cam.heading = wrap_angle(g_cam.heading + g_sign_cam * (rx * YAW_RATE * frames * (float)g.camera_speed_h / 100.0f + g_mouse_cam_x));
    g_cam.elevation = std::clamp(g_cam.elevation + (g.invert_camera_y ? -ry : ry) * -PITCH_RATE * frames * (float)g.camera_speed_v / 100.0f - g_mouse_cam_y, -1.1f, 0.75f);

    // Left stick. The game's own controls are already relative to a camera - its own, the one that swings round behind
    // the character: the stick's direction, seen from that camera, is the way the character turns to and runs. So the
    // stick only has to be rotated by the angle between the camera the player sees and the game's own.
    const float deflection = std::min(1.0f, std::sqrt(lx * lx + ly * ly));
    int stick_x = 0, stick_y = 0;
    if (deflection > 0.12f) {
        const float wanted = g_cam.heading + g_sign_move * std::atan2(lx, ly); // world heading to run along
        const float relative = wrap_angle(wanted - game_heading);
        stick_x = (int)std::lround(std::sin(relative) * deflection * 78.0f * g_sign_move);
        stick_y = (int)std::lround(std::cos(relative) * deflection * 78.0f);
    }
    Pad::StickX::set(pad, (int8_t)stick_x);
    Pad::StickY::set(pad, (int8_t)stick_y);
}

// Entry of objMoveXYZ(object, dx, dy, dz), the game's "move this object by this much, with collision". The character
// control code calls it once a tick for the player with velocity * frames, and afterwards works out the velocity again
// from how far the character actually got, measured from the position it saved in its data (object + 0x68: x at +0x3C,
// z at +0x44) - so simply enlarging the step would come back as a higher velocity and be enlarged again every tick.
// Instead the extra distance is covered by a second, separate objMoveXYZ call made from here (so it is collision
// checked too), and the saved position is moved along by what that call achieved. The game then makes its own step
// and sees exactly its own speed.
extern "C" void jfg_move_scale(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    static bool busy = false;
    if (busy || g_platform_move || g_cam.player == 0 || (uint32_t)ctx->r4 != g_cam.player) return;
    const uint32_t obj = g_cam.player, data = Obj::Data::get(obj);
    if (!valid_ptr(data)) return;
    // Underwater (character state 4, Vela's swimming) the "Dive speed" option, and swim sprinting, scale the whole 3D
    // step, up and down too. (On foot sprint_factor is already part of move_factor.)
    const bool swim = PlayerData::State::get(data) == PlayerData::StateSwimming;
    const float dive = swim ? (float)g.dive_speed / 100.0f * g_cam.sprint_factor : 1.0f;
    auto as_float = [](gpr reg) { const uint32_t bits = (uint32_t)reg; float f; std::memcpy(&f, &bits, 4); return f; };
    // "Normalize diagonal speed": running while sidestepping (C-left / C-right, also what the left stick gives while
    // aiming) adds the two speeds together. The step is split into its forward and sideways parts (relative to the way
    // the character faces) and shortened to the larger part alone, so a diagonal is as fast as either direction.
    float normalize = 1.0f;
    if (g.normalize_diagonal && !swim) {
        constexpr uint16_t C_LEFT = 0x0002, C_RIGHT = 0x0001;
        const uint32_t pad = ADDR_sControllerCurrData + Pad::Size * (rd8(ADDR_sPlayerID) & 3u);
        if ((rd16(pad) & (C_LEFT | C_RIGHT)) != 0) {
            const float heading = angle_from_s16(Obj::Yaw::get(obj)) + PI_F; // the way the character runs
            const float sx = as_float(ctx->r5), sz = as_float(ctx->r7);
            const float forward = sx * std::sin(heading) + sz * std::cos(heading), sideways = sx * std::cos(heading) - sz * std::sin(heading);
            const float length = std::sqrt(forward * forward + sideways * sideways);
            if (length > 0.0f) normalize = std::max(std::fabs(forward), std::fabs(sideways)) / length;
        }
    }
    const float factor_xz = g_cam.move_factor * dive * normalize, factor_y = dive;
    if (factor_xz == 1.0f && factor_y == 1.0f) return;
    auto as_bits = [](float f) { uint32_t bits; std::memcpy(&bits, &f, 4); return bits; };
    const float dx = as_float(ctx->r5) * (factor_xz - 1.0f), dz = as_float(ctx->r7) * (factor_xz - 1.0f);
    const float dy = swim ? as_float(ctx->r6) * (factor_y - 1.0f) : 0.0f;
    if (dx == 0.0f && dy == 0.0f && dz == 0.0f) return;
    const float x0 = Obj::PosX::get(obj), y0 = Obj::PosY::get(obj), z0 = Obj::PosZ::get(obj);
    busy = true;
    call_game(objMoveXYZ, ctx, { obj, as_bits(dx), as_bits(dy), as_bits(dz) });
    busy = false;
    PlayerData::SavedX::set(data, PlayerData::SavedX::get(data) + (Obj::PosX::get(obj) - x0));
    if (swim) PlayerData::SavedY::set(data, PlayerData::SavedY::get(data) + (Obj::PosY::get(obj) - y0));
    PlayerData::SavedZ::set(data, PlayerData::SavedZ::get(data) + (Obj::PosZ::get(obj) - z0));
}

} // namespace jfg

// controlPlatform moves what stands on a platform with one objMoveXYZ call; jfg_platform_move marks it.
static JfgCaller g_platform;

JFG_WRAP(controlPlatform) {
    JfgCaller::Scope scope(g_platform, ctx, 0x58);
    controlPlatform_orig(rdram, ctx);
}

JFG_WRAP(objMoveXYZ) {
    const bool platform = g_platform.direct(ctx);
    if (platform) jfg_platform_move(1);
    jfg_move_scale(rdram, ctx);
    objMoveXYZ_orig(rdram, ctx);
    if (platform) jfg_platform_move(0);
}
