// JFG PORT: the port's settings (the F9 Options / Profiles tabs) for the "Select" config menu (src/ui/ui_config.cpp).
// Implemented in jfg_settings_api.cpp. Called from the UI thread, like the F9 menu.
#pragma once

#include <string>
#include <vector>

namespace jfg::settings {
    // Options by their saved key (jfg_port_options.txt), plus "texture_dump" (this session only).
    int get(const std::string& key);
    void set(const std::string& key, int value);

    // Profiles (Default, Modern, Modern and Balanced, Extreme, then the player's own).
    std::vector<std::string> profiles();
    std::string active_profile();          // "" = none chosen
    std::string profile_description();     // of the active profile
    bool active_profile_is_builtin();
    bool profile_changed();                // settings differ from the built-in profile as chosen
    void choose_profile(const std::string& name);
    std::string profile_preview(const std::string& name); // what choosing it changes, as prompt text; "" = nothing
    bool keep_controls();                  // Keep My Controls: profiles leave the Controls tab as it is
    void set_keep_controls(bool keep);
    // The Bindings tab: during play the built-in dual-stick layout takes the place of the controller bindings /
    // mouse and keyboard controls have the keyboard's own layout in place of the keyboard bindings.
    bool controller_bindings_unused();
    bool keyboard_bindings_unused();
    bool save_profile(const std::string& name); // from the current settings; false if the name is not allowed
    void delete_active_profile();
    std::string profile_message();

    // Texture packs: "" = off. Folder ids as stored, with display names.
    std::vector<std::string> texture_packs();
    std::string texture_pack_name(const std::string& folder);
    std::string texture_pack();
    void set_texture_pack(const std::string& folder);
    int texture_pack_count();              // textures loaded from the current pack, -1 if none
    int dumped_texture_files();
    void open_texture_packs_folder();      // in the file manager
}
