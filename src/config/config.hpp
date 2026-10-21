#pragma once

#include <filesystem>
#include <string_view>

#include "json/json.hpp"

#include "input/input.hpp"

namespace dino::config {
    constexpr std::u8string_view program_id = u8"JetForceGeminiRecompiled";
    constexpr std::string_view program_name = "Jet Force Gemini: Recompiled";

    void load_config();
    void save_config();
    bool running_on_steam_deck();

    void reset_input_bindings();
    void reset_cont_input_bindings();
    void reset_kb_input_bindings();
    void reset_single_input_binding(dino::input::InputDevice device, dino::input::GameInput input);

    std::filesystem::path get_app_folder_path();
    std::filesystem::path get_executable_folder_path();

    void reset_sound_settings();
    void set_main_volume(int volume);
    int get_main_volume();

    void open_quit_game_prompt();
}
