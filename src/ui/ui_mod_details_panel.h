#ifndef RECOMPUI_ELEMENT_MOD_DETAILS_PANEL_H
#define RECOMPUI_ELEMENT_MOD_DETAILS_PANEL_H

#include "librecomp/mods.hpp"
#include "elements/ui_button.h"
#include "elements/ui_container.h"
#include "elements/ui_image.h"
#include "elements/ui_label.h"
#include "elements/ui_toggle.h"
#include "elements/ui_scroll_container.h"

namespace recompui {

// JFG PORT: the mod menu in the texture packs' look (ui_mod_menu.cpp, this panel): the launcher's navy, border blue,
// "Gemini" cyan and emblem gold, cards like the packs', and a tag on each mod saying whether it is on.
namespace modui {
    inline constexpr Color text{ 232, 243, 255, 255 };
    inline constexpr Color dim{ 129, 153, 191, 255 };
    inline constexpr Color cyan{ 60, 200, 255, 255 };
    inline constexpr Color gold{ 255, 178, 26, 255 };
    inline constexpr Color red{ 255, 74, 74, 255 };
    inline constexpr Color navy{ 5, 11, 28, 255 };
    inline constexpr Color panel{ 11, 23, 51, 255 };
    inline constexpr Color edge{ 31, 61, 115, 255 };
    inline constexpr Color hover{ 10, 38, 96, 255 };

    enum class Status { Off, On, Required, Missing, Outdated };

    // A mod's state now: on, off, or on because a mod that is on needs it.
    Status status_of(const std::string& mod_id);
    // A mod with an option "enabled" (Off / On, in that order) has a switch of its own: the On/Off toggle sets it, and
    // so works while the game runs (the mod stays loaded and does nothing while it is Off). The option is not listed
    // on the mod's settings page.
    bool has_switch(const std::string& mod_id);
    bool is_switch_option(const recomp::mods::ConfigOption& option);
    // Whether the toggle can be changed now: the game not started yet, a mod that turns on and off as it runs, or a
    // loaded mod with a switch.
    bool can_toggle(const recomp::mods::ModDetails& details);
    // Font size, line height and weight, times the menu's scale (1 in the game's menu, smaller in the launcher).
    void set_text(Element* e, float scale, float size, float line, uint32_t weight, Color color, float spacing = 0.0f);
    // The small tag beside a mod: ON, OFF, REQUIRED (on because a mod that is on needs it), MISSING or OLD VERSION.
    void set_tag(Label* tag, Status status, float scale);
    // A button of the mod menu (Install Mods, Configure, ...) at the menu's scale: the stock one is 28dp with 23dp padding.
    void set_button(Button* button, float scale, float radius);
}

// A mod in the details panel's Requires and Required by lists: its name, the version asked for, and its tag. A click
// selects that mod in the list.
class ModDependencyRow : public Element {
public:
    ModDependencyRow(Element *parent, float scale, float radius, const std::string &name, const std::string &detail, modui::Status status, std::function<void()> pressed);
protected:
    void process_event(const Event &e) override;
    std::string_view get_type_name() override { return "ModDependencyRow"; }
private:
    Style hover_style;
    Style focus_style;
    std::function<void()> pressed;
};

class ModDetailsPanel : public Element {
public:
    ModDetailsPanel(Element *parent);
    virtual ~ModDetailsPanel();
protected:
    void process_event(const Event &e) override;
public:
    void set_mod_details(const recomp::mods::ModDetails& details, const std::string &thumbnail, bool toggle_checked, bool toggle_enabled, bool toggle_label_visible, bool configure_enabled);
    void set_mod_toggled_callback(std::function<void(bool)> callback);
    void set_mod_configure_pressed_callback(std::function<void()> callback);
    // JFG PORT: every mod (for the Requires and Required by lists), and what a click on one of them does.
    void set_mod_list(const std::vector<recomp::mods::ModDetails> *all_mods) { this->all_mods = all_mods; }
    void set_mod_jump_callback(std::function<void(const std::string&)> callback) { mod_jump_callback = callback; }
    void set_scale(float scale, float radius); // JFG PORT
    void refresh_status(); // JFG PORT: the tags and the toggle's note again, after a mod was turned on or off
    void setup_mod_navigation(Element* nav_target);
    void clear_mod_navigation();
    Toggle* get_enable_toggle() { return enable_toggle; }
    Element* get_first_dependency_row() { return dependency_rows.empty() ? nullptr : dependency_rows.front(); } // JFG PORT
    Button* get_configure_button() { return configure_button; }
    void disable_toggle();
protected:
    std::string_view get_type_name() override { return "ModDetailsPanel"; }
private:
    recomp::mods::ModDetails cur_details;
    Container *thumbnail_container = nullptr;
    Image *thumbnail_image = nullptr;
    Container *header_container = nullptr;
    Container *header_details_container = nullptr;
    Label *title_label = nullptr;
    Label *version_label = nullptr;
    Label *status_tag = nullptr; // JFG PORT
    ScrollContainer *body_container = nullptr;
    Element *description_card = nullptr; // JFG PORT
    Label *description_label = nullptr;
    Label *warning_label = nullptr; // JFG PORT
    Element *dependencies_container = nullptr; // JFG PORT: Requires and Required by
    Container *buttons_container = nullptr;
    Container *enable_container = nullptr;
    Toggle *enable_toggle = nullptr;
    Element *enable_text = nullptr; // JFG PORT: the toggle's state and why it can't change
    Label *enable_state_label = nullptr;
    Label *enable_label = nullptr;
    Button *configure_button = nullptr;
    std::function<void(bool)> mod_toggled_callback = nullptr;
    std::function<void()> mod_configure_pressed_callback = nullptr;
    std::function<void(const std::string&)> mod_jump_callback = nullptr;
    const std::vector<recomp::mods::ModDetails> *all_mods = nullptr;
    std::vector<ModDependencyRow*> dependency_rows; // JFG PORT: reached with a controller: up from the toggle
    Element *list_nav_target = nullptr;             // the selected mod's card, left of the panel
    bool toggle_locked_by_game = false;
    float scale = 1.0f;
    float radius = 8.0f;

    void enable_toggle_checked(bool checked);
    void configure_button_pressed();
    bool dependencies_pending = false; // JFG PORT: build_dependencies at the next update (see request_dependencies)
    void request_dependencies();
    void build_dependencies();
    void apply_dependency_navigation();
    void add_section_title(const std::string &text);
};

} // namespace recompui
#endif
