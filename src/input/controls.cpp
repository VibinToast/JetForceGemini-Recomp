#include "controls.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>

#include "SDL.h"

// Dual-stick controls for Jet Force Gemini (src/jfg/jfg_controls.cpp).
extern "C" int jfg_dual_stick_active(void);
extern "C" int jfg_invert_aim_normal(uint16_t buttons);
extern "C" int jfg_dual_stick_layout(void);
extern "C" void jfg_dual_stick(uint16_t* buttons, float* x, float* y, float right_x, float right_y);
extern "C" int jfg_mouse_keyboard_active(void);
extern "C" int jfg_floyd_mouse_active(void);
extern "C" void jfg_dual_stick_release(void);
extern "C" void jfg_input_toggles(uint16_t* buttons); // JFG PORT: src/jfg/jfg_hero_api.cpp
extern "C" int jfg_sprint_enabled(void);
extern "C" int jfg_worldmap_switching(void);
extern "C" void jfg_sprint_input(int held);
extern "C" void jfg_pad_state(uint32_t buttons, float left_trigger, float right_trigger);

namespace dino::input {

// Arrays that hold the mappings for every input for keyboard and controller respectively.
using input_mapping = std::array<InputField, bindings_per_input>;
using input_mapping_array = std::array<input_mapping, static_cast<size_t>(GameInput::COUNT)>;
static input_mapping_array keyboard_input_mappings{};
static input_mapping_array controller_input_mappings{};

// Make the button value array, which maps a button index to its bit field.
#define DEFINE_INPUT(name, value, readable) uint16_t(value##u),
static const std::array n64_button_values = {
    DEFINE_N64_BUTTON_INPUTS()
};
#undef DEFINE_INPUT

// Make the input name array.
#define DEFINE_INPUT(name, value, readable) readable,
static const std::vector<std::string> input_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

// Make the input enum name array.
#define DEFINE_INPUT(name, value, readable) #name,
static const std::vector<std::string> input_enum_names = {
    DEFINE_ALL_INPUTS()
};
#undef DEFINE_INPUT

size_t get_num_inputs() {
    return (size_t)GameInput::COUNT;
}

const std::string& get_input_name(GameInput input) {
    return input_names.at(static_cast<size_t>(input));
}

const std::string& get_input_enum_name(GameInput input) {
    return input_enum_names.at(static_cast<size_t>(input));
}

GameInput get_input_from_enum_name(const std::string_view enum_name) {
    auto find_it = std::find(input_enum_names.begin(), input_enum_names.end(), enum_name);
    if (find_it == input_enum_names.end()) {
        return GameInput::COUNT;
    }

    return static_cast<GameInput>(find_it - input_enum_names.begin());
}

// Due to an RmlUi limitation this can't be const. Ideally it would return a const reference or even just a straight up copy.
InputField& get_input_binding(GameInput input, size_t binding_index, InputDevice device) {
    input_mapping_array& device_mappings = (device == InputDevice::Controller) ?  controller_input_mappings : keyboard_input_mappings;
    input_mapping& cur_input_mapping = device_mappings.at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        return cur_input_mapping[binding_index];
    }
    else {
        static InputField dummy_field = {};
        return dummy_field;
    }
}

void set_input_binding(GameInput input, size_t binding_index, InputDevice device, InputField value) {
    input_mapping_array& device_mappings = (device == InputDevice::Controller) ?  controller_input_mappings : keyboard_input_mappings;
    input_mapping& cur_input_mapping = device_mappings.at(static_cast<size_t>(input));

    if (binding_index < cur_input_mapping.size()) {
        cur_input_mapping[binding_index] = value;
    }
}

// JFG PORT: mouse buttons that are held when a menu closes count only once they have been let go. They are read from
// SDL's state, which can miss a release while a menu has the cursor (on the Steam Deck a tap on the screen or a
// trackpad click is a mouse click), and the character then kept firing by itself. The next click clears it.
static uint32_t mouse_wait_for_release = 0;

static uint32_t game_mouse_buttons() {
    const uint32_t buttons = SDL_GetMouseState(nullptr, nullptr);
    mouse_wait_for_release &= buttons; // a button seen up counts again
    return buttons & ~mouse_wait_for_release;
}

bool get_n64_input(int controller_num, uint16_t* buttons_out, float* x_out, float* y_out) {
    uint16_t cur_buttons = 0;
    float cur_x = 0.0f;
    float cur_y = 0.0f;

    if (controller_num != 0) {
        return false;
    }

    if (!game_input_disabled()) {
        const bool dual_stick = jfg_dual_stick_active() != 0;
        const bool dual_layout = dual_stick && jfg_dual_stick_layout() != 0;
        // Mouse and keyboard (JFG option): during gameplay the keyboard has its own layout below instead of the
        // keyboard bindings, the mouse buttons aim and fire, and src/jfg turns mouse movement into camera and aim and
        // the wheel into weapon changes. In a Floyd mission (dual stick off) the keyboard has a Floyd layout and the
        // mouse steers Floyd (jfg_floyd_steer).
        const bool floyd = !dual_stick && jfg_floyd_mouse_active() != 0;
        const bool mouse_keyboard = (dual_stick || floyd) && jfg_mouse_keyboard_active() != 0;
        for (size_t i = 0; i < n64_button_values.size(); i++) {
            size_t input_index = (size_t)GameInput::N64_BUTTON_START + i;
            if (!mouse_keyboard) {
                cur_buttons |= get_input_digital(keyboard_input_mappings[input_index]) ? n64_button_values[i] : 0;
            }
            if (!dual_stick) {
                cur_buttons |= get_input_digital(controller_input_mappings[input_index]) ? n64_button_values[i] : 0;
            }
            else if (dual_layout) {
                // The built-in layout below replaces the controller bindings.
            }
            else {
                // The right stick is an analog stick in this mode: bindings of N64 buttons to it (by default the
                // C buttons) are ignored.
                for (const InputField& field : controller_input_mappings[input_index]) {
                    if (field.input_type == 4) { // InputType::ControllerAnalog (input.cpp)
                        const int axis = abs(field.input_id) - 1;
                        if (axis == SDL_CONTROLLER_AXIS_RIGHTX || axis == SDL_CONTROLLER_AXIS_RIGHTY) continue;
                    }
                    cur_buttons |= get_input_digital(field) ? n64_button_values[i] : 0;
                }
            }
        }

        if (dual_layout) {
            // Dual-stick mode's own layout for a modern (Xbox-style) controller, used during gameplay only. The game's
            // actions sit on N64 buttons: C-up jumps (and zooms in / walks forward while aiming), C-down crouches (zooms
            // out / walks back), A and B change weapon, R aims, Z fires. jfg_dual_stick() finishes the job (D-pad up/down
            // as zoom while aiming, the optional trigger swap).
            constexpr uint32_t digital = 3, analog = 4; // InputType::ControllerDigital / ControllerAnalog (input.cpp)
            struct LayoutEntry { uint32_t type; int32_t id; uint16_t n64; };
            static const LayoutEntry layout[] = {
                { analog,  SDL_CONTROLLER_AXIS_TRIGGERLEFT + 1,  0x0010 }, // LT: aim (R)
                { analog,  SDL_CONTROLLER_AXIS_TRIGGERRIGHT + 1, 0x2000 }, // RT: fire (Z)
                { digital, SDL_CONTROLLER_BUTTON_A,              0x0008 }, // A: jump (C-up)
                { digital, SDL_CONTROLLER_BUTTON_B,              0x0004 }, // B: crouch (C-down)
                { digital, SDL_CONTROLLER_BUTTON_LEFTSTICK,      0x0004 }, // left stick click: crouch (C-down)
                { digital, SDL_CONTROLLER_BUTTON_Y,              0x8000 }, // Y: N64 A (next weapon)
                { digital, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,  0x8000 }, // RB: N64 A (next weapon)
                { digital, SDL_CONTROLLER_BUTTON_X,              0x4000 }, // X: N64 B (previous weapon)
                { digital, SDL_CONTROLLER_BUTTON_LEFTSHOULDER,   0x4000 }, // LB: N64 B (previous weapon)
                { digital, SDL_CONTROLLER_BUTTON_RIGHTSTICK,     0x0020 }, // right stick click: N64 L
                { digital, SDL_CONTROLLER_BUTTON_BACK,           0x0020 }, // View/Back: N64 L
                { digital, SDL_CONTROLLER_BUTTON_START,          0x1000 }, // Menu/Start: START
                { digital, SDL_CONTROLLER_BUTTON_DPAD_UP,        0x0800 }, // D-pad: the N64 D-pad (weapon select);
                { digital, SDL_CONTROLLER_BUTTON_DPAD_DOWN,      0x0400 }, //   up/down zoom while aiming
                { digital, SDL_CONTROLLER_BUTTON_DPAD_LEFT,      0x0200 },
                { digital, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,     0x0100 },
            };
            const bool sprint = jfg_sprint_enabled() != 0;
            for (const LayoutEntry& entry : layout) {
                if (sprint && entry.type == digital && entry.id == SDL_CONTROLLER_BUTTON_LEFTSTICK) continue; // sprints instead (B still crouches)
                if (get_input_digital(InputField{ entry.type, entry.id })) {
                    cur_buttons |= entry.n64;
                }
            }
        }

        float joystick_x = get_input_analog(controller_input_mappings[(size_t)GameInput::X_AXIS_POS])
                        - get_input_analog(controller_input_mappings[(size_t)GameInput::X_AXIS_NEG]);

        float joystick_y = get_input_analog(controller_input_mappings[(size_t)GameInput::Y_AXIS_POS])
                        - get_input_analog(controller_input_mappings[(size_t)GameInput::Y_AXIS_NEG]);

        apply_joystick_deadzone(joystick_x, joystick_y, &joystick_x, &joystick_y);

        if (mouse_keyboard && floyd) {
            // Floyd: W thrust (N64 A), S brake (N64 B), A / D turn (the stick), left mouse button fires (Z); the mouse
            // turns and pitches Floyd itself (src/jfg).
            constexpr uint32_t keyboard = 1; // InputType::Keyboard (input.cpp)
            auto key = [](SDL_Scancode code) { return get_input_digital(InputField{ keyboard, (int32_t)code }); };
            struct KeyEntry { SDL_Scancode key; uint16_t n64; };
            static const KeyEntry keys[] = {
                { SDL_SCANCODE_W,      0x8000 }, // thrust (N64 A)
                { SDL_SCANCODE_SPACE,  0x8000 },
                { SDL_SCANCODE_S,      0x4000 }, // brake (N64 B)
                { SDL_SCANCODE_LSHIFT, 0x4000 },
                { SDL_SCANCODE_RETURN, 0x1000 }, // START
                { SDL_SCANCODE_TAB,    0x0020 }, // N64 L
            };
            for (const KeyEntry& entry : keys) {
                if (key(entry.key)) cur_buttons |= entry.n64;
            }
            const uint32_t mouse_buttons = game_mouse_buttons();
            if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) cur_buttons |= 0x2000;  // fire (Z)
            if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) cur_buttons |= 0x0010; // N64 R
            const float kx = (key(SDL_SCANCODE_D) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_A) ? 1.0f : 0.0f);
            joystick_x = std::clamp(joystick_x + kx, -1.0f, 1.0f);
        }
        else if (mouse_keyboard) {
            constexpr uint32_t keyboard = 1; // InputType::Keyboard (input.cpp)
            auto key = [](SDL_Scancode code) { return get_input_digital(InputField{ keyboard, (int32_t)code }); };
            struct KeyEntry { SDL_Scancode key; uint16_t n64; };
            static const KeyEntry keys[] = {
                { SDL_SCANCODE_SPACE,  0x0008 }, // jump (C-up)
                { SDL_SCANCODE_LCTRL,  0x0004 }, // crouch (C-down)
                { SDL_SCANCODE_C,      0x0004 },
                { SDL_SCANCODE_E,      0x8000 }, // next weapon (N64 A)
                { SDL_SCANCODE_Q,      0x4000 }, // previous weapon (N64 B)
                { SDL_SCANCODE_RETURN, 0x1000 }, // START
                { SDL_SCANCODE_TAB,    0x0020 }, // N64 L
                { SDL_SCANCODE_1,      0x0800 }, // D-pad up (zoom in while aiming)
                { SDL_SCANCODE_2,      0x0100 }, // D-pad right
                { SDL_SCANCODE_3,      0x0400 }, // D-pad down (zoom out while aiming)
                { SDL_SCANCODE_4,      0x0200 }, // D-pad left
            };
            for (const KeyEntry& entry : keys) {
                if (key(entry.key)) cur_buttons |= entry.n64;
            }
            const uint32_t mouse_buttons = game_mouse_buttons();
            if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_RIGHT)) cur_buttons |= 0x0010; // aim (R)
            if (mouse_buttons & SDL_BUTTON(SDL_BUTTON_LEFT)) cur_buttons |= 0x2000;  // fire (Z)

            // WASD as the left stick.
            float kx = (key(SDL_SCANCODE_D) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_A) ? 1.0f : 0.0f);
            float ky = (key(SDL_SCANCODE_W) ? 1.0f : 0.0f) - (key(SDL_SCANCODE_S) ? 1.0f : 0.0f);
            if (kx != 0.0f && ky != 0.0f) { kx *= 0.7071f; ky *= 0.7071f; }
            joystick_x = std::clamp(joystick_x + kx, -1.0f, 1.0f);
            joystick_y = std::clamp(joystick_y + ky, -1.0f, 1.0f);
        }

        // The physical controls as a modern pad, for mods (jfg_controls_get): the controller's buttons and triggers,
        // and while mouse and keyboard play the keys and mouse buttons that stand for them (Space A, Left Ctrl / C B,
        // F X, R Y, Q LB, E RB, right mouse button LT, left RT, Left Shift L3, Tab Back, Enter Start, 1-4 the D-pad as
        // in the layout above).
        {
            constexpr uint32_t keyboard = 1, controller_digital = 3, controller_analog = 4; // InputType (input.cpp)
            const bool keys = mouse_keyboard && !floyd;
            auto key = [&](SDL_Scancode code) { return keys && get_input_digital(InputField{ keyboard, (int32_t)code }); };
            auto pad = [](int32_t button) { return get_input_digital(InputField{ controller_digital, button }); };
            const uint32_t mouse = keys ? game_mouse_buttons() : 0;
            const float lt = std::max(get_input_analog(InputField{ controller_analog, SDL_CONTROLLER_AXIS_TRIGGERLEFT + 1 }),
                                      (mouse & SDL_BUTTON(SDL_BUTTON_RIGHT)) ? 1.0f : 0.0f);
            const float rt = std::max(get_input_analog(InputField{ controller_analog, SDL_CONTROLLER_AXIS_TRIGGERRIGHT + 1 }),
                                      (mouse & SDL_BUTTON(SDL_BUTTON_LEFT)) ? 1.0f : 0.0f);
            struct PadEntry { int32_t button; SDL_Scancode key, key2; uint32_t bit; };
            static const PadEntry entries[] = {
                { SDL_CONTROLLER_BUTTON_A,             SDL_SCANCODE_SPACE,  SDL_SCANCODE_UNKNOWN, 0x0001 },
                { SDL_CONTROLLER_BUTTON_B,             SDL_SCANCODE_LCTRL,  SDL_SCANCODE_C,       0x0002 },
                { SDL_CONTROLLER_BUTTON_X,             SDL_SCANCODE_F,      SDL_SCANCODE_UNKNOWN, 0x0004 },
                { SDL_CONTROLLER_BUTTON_Y,             SDL_SCANCODE_R,      SDL_SCANCODE_UNKNOWN, 0x0008 },
                { SDL_CONTROLLER_BUTTON_LEFTSHOULDER,  SDL_SCANCODE_Q,      SDL_SCANCODE_UNKNOWN, 0x0010 },
                { SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_SCANCODE_E,      SDL_SCANCODE_UNKNOWN, 0x0020 },
                { SDL_CONTROLLER_BUTTON_LEFTSTICK,     SDL_SCANCODE_LSHIFT, SDL_SCANCODE_UNKNOWN, 0x0100 },
                { SDL_CONTROLLER_BUTTON_RIGHTSTICK,    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_UNKNOWN, 0x0200 },
                { SDL_CONTROLLER_BUTTON_BACK,          SDL_SCANCODE_TAB,    SDL_SCANCODE_UNKNOWN, 0x0400 },
                { SDL_CONTROLLER_BUTTON_START,         SDL_SCANCODE_RETURN, SDL_SCANCODE_UNKNOWN, 0x0800 },
                { SDL_CONTROLLER_BUTTON_DPAD_UP,       SDL_SCANCODE_1,      SDL_SCANCODE_UNKNOWN, 0x1000 },
                { SDL_CONTROLLER_BUTTON_DPAD_DOWN,     SDL_SCANCODE_3,      SDL_SCANCODE_UNKNOWN, 0x2000 },
                { SDL_CONTROLLER_BUTTON_DPAD_LEFT,     SDL_SCANCODE_4,      SDL_SCANCODE_UNKNOWN, 0x4000 },
                { SDL_CONTROLLER_BUTTON_DPAD_RIGHT,    SDL_SCANCODE_2,      SDL_SCANCODE_UNKNOWN, 0x8000 },
            };
            uint32_t bits = (lt > 0.5f ? 0x0040u : 0u) | (rt > 0.5f ? 0x0080u : 0u);
            for (const PadEntry& e : entries) {
                if (pad(e.button) || (e.key != SDL_SCANCODE_UNKNOWN && key(e.key)) || (e.key2 != SDL_SCANCODE_UNKNOWN && key(e.key2))) {
                    bits |= e.bit;
                }
            }
            jfg_pad_state(bits, lt, rt);
        }

        // Sprint (JFG option): left-stick click on a controller, Left Shift with mouse and keyboard (not in a Floyd
        // mission, where it brakes).
        {
            constexpr uint32_t keyboard = 1, controller_digital = 3; // InputType::Keyboard / ControllerDigital (input.cpp)
            bool held = get_input_digital(InputField{ controller_digital, SDL_CONTROLLER_BUTTON_LEFTSTICK });
            if (mouse_keyboard && !floyd) held = held || get_input_digital(InputField{ keyboard, SDL_SCANCODE_LSHIFT });
            jfg_sprint_input(held ? 1 : 0);
        }

        // The planet map with more than one galaxy (jfg_worldmap.cpp): the shoulder buttons switch galaxy. LB is N64 L
        // in the default bindings; RB is bound to nothing there, so it gives N64 R while the map is up.
        if (jfg_worldmap_switching() != 0) {
            constexpr uint32_t controller_digital = 3; // InputType::ControllerDigital (input.cpp)
            if (get_input_digital(InputField{ controller_digital, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER })) cur_buttons |= 0x0010;
            if (get_input_digital(InputField{ controller_digital, SDL_CONTROLLER_BUTTON_LEFTSHOULDER })) cur_buttons |= 0x0020;
        }

        if (!dual_stick) jfg_input_toggles(&cur_buttons); // JFG PORT: a mod's toggled buttons (jfg_dual_stick does it there)
        if (dual_stick) {
            float right_x = 0.0f, right_y = 0.0f;
            get_right_analog(&right_x, &right_y);
            jfg_dual_stick(&cur_buttons, &joystick_x, &joystick_y, right_x, -right_y); // SDL: down is positive
        }

        cur_x = joystick_x;
        cur_y = joystick_y;
        if (!mouse_keyboard) {
            cur_x += get_input_analog(keyboard_input_mappings[(size_t)GameInput::X_AXIS_POS])
                   - get_input_analog(keyboard_input_mappings[(size_t)GameInput::X_AXIS_NEG]);
            cur_y += get_input_analog(keyboard_input_mappings[(size_t)GameInput::Y_AXIS_POS])
                   - get_input_analog(keyboard_input_mappings[(size_t)GameInput::Y_AXIS_NEG]);
        }

        if (!dual_stick && jfg_invert_aim_normal(cur_buttons) != 0) {
            cur_y = -cur_y; // option: flip up/down while aiming with the normal controls
        }

        float joystick_range = get_joystick_range() / 100.0f;
        
        cur_x *= joystick_range;
        cur_y *= joystick_range;
    }

    else {
        jfg_dual_stick_release(); // a menu has the input: let go of the sticks the game logic remembers
        jfg_pad_state(0, 0.0f, 0.0f);
        mouse_wait_for_release = SDL_BUTTON(SDL_BUTTON_LEFT) | SDL_BUTTON(SDL_BUTTON_RIGHT); // fire, aim
        jfg_sprint_input(0);
    }

    *buttons_out = cur_buttons;
    *x_out = std::clamp(cur_x, -1.0f, 1.0f);
    *y_out = std::clamp(cur_y, -1.0f, 1.0f);

    return true;
}

}
