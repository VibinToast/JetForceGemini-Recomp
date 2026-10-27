#include "renderer/renderer.hpp"
#include "runtime/support.hpp"

#include "ui_mod_menu.h"
#include "ui_utils.h"
#include "ui/recomp_ui.h"

#include "librecomp/mods.hpp"

#include <string>

#ifdef WIN32
#include <shellapi.h>
#endif

// TODO:
// - Set up navigation.
// - Add hover and active state for mod entries.

namespace recompui {

static std::string generate_thumbnail_src_for_mod(const std::string &mod_id) {
    return "?/mods/" + mod_id + "/thumb";
}

// ModEntryView
#define COL_TEXT_DIM 169, 190, 221
constexpr float modEntryHeight = 76.0f; // JFG PORT: the thumbnail, as the texture packs' cards (was 120)

extern const std::string mod_tab_id;
const std::string mod_tab_id = "#tab_mods"; // JFG PORT: the launcher's Mods button has this id too

static void mark_other_mod_menus_dirty(ModMenu* changed);

// JFG PORT: a card like the texture packs': thumbnail, name, version and authors, and a tag saying whether it is on.
ModEntryView::ModEntryView(Element *parent) : Element(parent, Events(EventType::Update)) {
    ContextId context = get_current_context();

    set_display(Display::Flex);
    set_flex_direction(FlexDirection::Row);
    set_align_items(AlignItems::Center);
    set_width(100.0f, Unit::Percent);
    set_height_auto();
    set_border_width(2.0f);
    set_border_color(modui::edge);
    set_background_color(modui::navy);
    set_cursor(Cursor::Pointer);
    set_color(modui::text);

    checked_style.set_border_color(modui::gold);
    checked_style.set_background_color(modui::hover);
    hover_style.set_border_color(modui::cyan);
    hover_style.set_background_color(modui::hover);
    checked_hover_style.set_border_color(modui::gold);
    pulsing_style.set_border_color(modui::gold);

    {
        thumbnail_image = context.create_element<Image>(this, "");
        thumbnail_image->set_flex(0.0f, 0.0f);
        thumbnail_image->set_background_color(Color{ 3, 8, 22, 204 });

        body_container = context.create_element<Element>(this);
        body_container->set_width_auto();
        body_container->set_flex(1.0f, 1.0f);
        body_container->set_min_width(0.0f);

        {
            name_label = context.create_element<Label>(body_container, LabelStyle::Normal);
            meta_label = context.create_element<Label>(body_container, LabelStyle::Small);
        } // body_container

        status_tag = context.create_element<Label>(this, LabelStyle::Annotation);
    } // this

    add_style(&checked_style, checked_state);
    add_style(&hover_style, hover_state);
    add_style(&checked_hover_style, { checked_state, hover_state });
    add_style(&pulsing_style, { focus_state });
    set_scale(1.0f, 8.0f);
}

ModEntryView::~ModEntryView() {

}

void ModEntryView::set_scale(float scale, float radius) {
    this->scale = scale;
    set_padding(10.0f * scale);
    set_border_radius(radius);
    thumbnail_image->set_width(modEntryHeight * scale);
    thumbnail_image->set_height(modEntryHeight * scale);
    body_container->set_margin_left(18.0f * scale);
    modui::set_text(name_label, scale, 29.0f, 36.0f, 700, modui::text, 1.0f);
    modui::set_text(meta_label, scale, 23.0f, 29.0f, 400, modui::dim);
    meta_label->set_margin_top(6.0f * scale);
    status_tag->set_margin_left(12.0f * scale);
    set_mod_status(status);
}

void ModEntryView::set_mod_details(const recomp::mods::ModDetails &details) {
    name_label->set_text(details.display_name);
    std::string meta = details.version.to_string();
    bool first = true;
    for (const std::string& author : details.authors) {
        meta += (first ? "  " : ", ") + author;
        first = false;
    }
    meta_label->set_text(meta);
}

void ModEntryView::set_mod_thumbnail(const std::string &thumbnail) {
    thumbnail_image->set_src(thumbnail);
}

void ModEntryView::set_mod_status(modui::Status status) {
    this->status = status;
    modui::set_tag(status_tag, status, scale);
    // An off mod's picture and text are faded; its tag stays readable.
    float opacity = status == modui::Status::Off ? 0.55f : 1.0f;
    thumbnail_image->set_opacity(opacity);
    body_container->set_opacity(opacity);
}

void ModEntryView::set_selected(bool selected) {
    set_style_enabled(checked_state, selected);
}

void ModEntryView::set_focused(bool focused) {
    set_style_enabled(focus_state, focused);
}

void ModEntryView::process_event(const Event &e) {
    switch (e.type) {
    case EventType::Update:
        if (is_style_enabled(focus_state)) {
            pulsing_style.set_color(recompui::get_pulse_color(750));
            apply_styles();
            queue_update();
        }

        break;
    default:
        break;
    }
}

// ModEntryButton

ModEntryButton::ModEntryButton(Element *parent, uint32_t mod_index) : Element(parent, Events(EventType::Click, EventType::Hover, EventType::Focus, EventType::Drag)) {
    this->mod_index = mod_index;

    set_drag(Drag::Drag);
    enable_focus();

    ContextId context = get_current_context();
    view = context.create_element<ModEntryView>(this);
}

ModEntryButton::~ModEntryButton() {

}

void ModEntryButton::set_mod_selected_callback(std::function<void(uint32_t)> callback) {
    selected_callback = callback;
}

void ModEntryButton::set_mod_activated_callback(std::function<void(uint32_t)> callback) {
    activated_callback = callback;
}

void ModEntryButton::set_mod_drag_callback(std::function<void(uint32_t, EventDrag)> callback) {
    drag_callback = callback;
}

void ModEntryButton::set_mod_details(const recomp::mods::ModDetails &details) {
    view->set_mod_details(details);
}

void ModEntryButton::set_mod_thumbnail(const std::string &thumbnail) {
    view->set_mod_thumbnail(thumbnail);
}

void ModEntryButton::set_mod_status(modui::Status status) {
    view->set_mod_status(status);
}

void ModEntryButton::set_scale(float scale, float radius) {
    set_padding_bottom(10.0f * scale); // the gap between cards
    view->set_scale(scale, radius);
}

void ModEntryButton::set_selected(bool selected) {
    view->set_selected(selected);
}

void ModEntryButton::set_focused(bool focused) {
    view->set_focused(focused);
    view->queue_update();
}

void ModEntryButton::process_event(const Event& e) {
    switch (e.type) {
    case EventType::Focus:
        selected_callback(mod_index);
        set_focused(std::get<EventFocus>(e.variant).active);
        if (std::get<EventFocus>(e.variant).active) focused_at = std::chrono::steady_clock::now();
        break;
    case EventType::Click:
        // JFG PORT: confirm on the selected mod's card opens its settings, as its Configure button does; not the click
        // that focused it (a mouse click focuses and clicks at once: the first selects).
        if (activated_callback != nullptr && std::chrono::steady_clock::now() - focused_at > std::chrono::milliseconds(250))
            activated_callback(mod_index);
        break;
    case EventType::Hover:
        view->set_style_enabled(hover_state, std::get<EventHover>(e.variant).active);
        break;
    case EventType::Drag:
        drag_callback(mod_index, std::get<EventDrag>(e.variant));
        break;
    default:
        break;
    }
}

// ModEntrySpacer

void ModEntrySpacer::check_height_distance() {
    constexpr float tolerance = 0.01f;
    if (abs(target_height - height) < tolerance) {
        height = target_height;
        set_height(height, Unit::Px); // JFG PORT: pixels, the height of the dragged entry (entries fit their text)
    }
    else {
        queue_update();
    }
}

void ModEntrySpacer::process_event(const Event &e) {
    switch (e.type) {
    case EventType::Update: {
        std::chrono::high_resolution_clock::duration now = ultramodern::time_since_start();
        float delta_time = std::max(std::chrono::duration<float>(now - last_time).count(), 0.0f);
        constexpr float px_speed = 1000.0f;
        last_time = now;

        if (target_height < height) {
            height += std::max(-px_speed * delta_time, target_height - height);
        }
        else {
            height += std::min(px_speed * delta_time, target_height - height);
        }

        set_height(height, Unit::Px);
        check_height_distance();
        break;
    }
    default:
        break;
    }
}

ModEntrySpacer::ModEntrySpacer(Element *parent) : Element(parent) {
    // Do nothing.
}

void ModEntrySpacer::set_target_height(float target_height, bool animate_to_target) {
    this->target_height = target_height;

    if (animate_to_target) {
        last_time = ultramodern::time_since_start();
        check_height_distance();
    }
    else {
        height = target_height;
        set_height(target_height, Unit::Dp);
    }
}

// ModMenu

void ModMenu::refresh_mods(bool scan_mods) {
    for (const std::string &thumbnail : loaded_thumbnails) {
        recompui::release_image(thumbnail);
    }

    if (scan_mods) {
        recomp::mods::scan_mods();
    }
    mod_details = recomp::mods::get_all_mod_details(game_mod_id);
    if (active_mod_index >= mod_details.size()) {
        active_mod_index = -1;
    }
    create_mod_list();
}

void ModMenu::open_mods_folder() {
    std::filesystem::path mods_directory = recomp::mods::get_mods_directory();
#if defined(WIN32)
    std::wstring path_wstr = mods_directory.wstring();
    ShellExecuteW(NULL, L"open", path_wstr.c_str(), NULL, NULL, SW_SHOWDEFAULT);
#elif defined(__linux__)
    std::string command = "xdg-open \"" + mods_directory.string() + "\" &";
    std::system(command.c_str());
#elif defined(__APPLE__)
    std::string command = "open \"" + mods_directory.string() + "\"";
    std::system(command.c_str());
#else
    static_assert(false, "Not implemented for this platform.");
#endif
}

void ModMenu::open_install_dialog() {
    dino::runtime::open_file_dialog_multiple([](bool success, const std::list<std::filesystem::path>& paths) {
        if (success) {
            ContextId old_context = recompui::try_close_current_context();

            recompui::drop_files(paths);

            if (old_context != ContextId::null()) {
                old_context.open();
            }
        }
    });
}

void ModMenu::mod_toggled(bool enabled) {
    if (active_mod_index >= 0) {
        // JFG PORT: a mod with a switch (modui::has_switch) is turned on and off by it, and stays in the Mods list: so
        // it can change while the game runs. Turning it on before the game starts also puts it in the list.
        const std::string& id = mod_details[active_mod_index].mod_id;
        if (modui::has_switch(id)) {
            recomp::mods::set_mod_config_value(id, "enabled", (uint32_t)(enabled ? 1 : 0));
            if (enabled && !ultramodern::is_game_started()) recomp::mods::enable_mod(id, true);
        }
        else {
            recomp::mods::enable_mod(id, enabled);
        }
        
        // Refresh enabled status for all mods in case one of them got auto-enabled due to being a dependency.
        refresh_mod_status();
        mark_other_mod_menus_dirty(this); // JFG PORT
    }
}

// JFG PORT: every card's tag and the details panel's, after a mod was turned on or off.
void ModMenu::refresh_mod_status() {
    for (size_t i = 0; i < mod_entry_buttons.size(); i++) {
        mod_entry_buttons[i]->set_mod_status(modui::status_of(mod_details[i].mod_id));
    }
    mod_details_panel->refresh_status();
}

// JFG PORT: the launcher's text is smaller than the game menu's: the cards, details and buttons at 0.75, square.
void ModMenu::apply_scale() {
    const float s = scale;
    list_container->set_border_radius(radius);
    list_scroll_container->set_padding(12.0f * s);
    mod_details_panel->set_margin_left(18.0f * s);
    mod_details_panel->set_scale(s, radius);
    footer_container->set_padding(16.0f * s);
    footer_container->set_gap(16.0f * s);
    footer_container->set_margin_top(18.0f * s);
    modui::set_button(install_mods_button, s, radius);
    modui::set_button(refresh_button, s, radius);
    modui::set_button(mods_folder_button, s, radius);
    modui::set_text(ingame_note, s, 18.0f, 24.0f, 400, modui::dim);
    for (ModEntryButton* entry : mod_entry_buttons) {
        entry->set_scale(s, radius);
    }
    mod_entry_floating_view->set_scale(s, radius);
}

void ModMenu::mod_selected(uint32_t mod_index) {
    if (active_mod_index >= 0) {
        mod_entry_buttons[active_mod_index]->set_selected(false);
    }

    active_mod_index = mod_index;

    if (active_mod_index >= 0) {
        std::string thumbnail_src = generate_thumbnail_src_for_mod(mod_details[mod_index].mod_id);
        const recomp::mods::ConfigSchema &config_schema = recomp::mods::get_mod_config_schema(mod_details[active_mod_index].mod_id);
        bool toggle_checked = modui::status_of(mod_details[mod_index].mod_id) != modui::Status::Off;
        bool auto_enabled = recomp::mods::is_mod_auto_enabled(mod_details[mod_index].mod_id);
        bool toggle_enabled = !auto_enabled && modui::can_toggle(mod_details[mod_index]);
        // JFG PORT: a switch alone (modui::has_switch) gives the settings page nothing to show.
        bool configure_enabled = config_schema.options.size() > (modui::has_switch(mod_details[mod_index].mod_id) ? 1u : 0u);
        mod_details_panel->set_mod_details(mod_details[mod_index], thumbnail_src, toggle_checked, toggle_enabled, auto_enabled, configure_enabled);
        mod_entry_buttons[active_mod_index]->set_selected(true);

        mod_details_panel->setup_mod_navigation(mod_entry_buttons[mod_index]);

        // Navigation from the bottom bar.
        Button *configure_button = mod_details_panel->get_configure_button();
        Toggle *enable_toggle = mod_details_panel->get_enable_toggle();
        if (configure_enabled) {
            refresh_button->set_nav(NavDirection::Up, configure_button);
            mods_folder_button->set_nav(NavDirection::Up, configure_button);
        }
        else if (toggle_enabled) {
            refresh_button->set_nav(NavDirection::Up, enable_toggle);
            mods_folder_button->set_nav(NavDirection::Up, enable_toggle);
        }
        else {
            refresh_button->set_nav_manual(NavDirection::Up, mod_tab_id);
            mods_folder_button->set_nav_manual(NavDirection::Up, mod_tab_id);
        }

        // Navigation from the mod list.
        if (configure_enabled) { // JFG PORT: Configure comes first now
            mod_entry_buttons[active_mod_index]->set_nav(NavDirection::Right, configure_button);
        }
        else if (toggle_enabled) {
            mod_entry_buttons[active_mod_index]->set_nav(NavDirection::Right, enable_toggle);
        }
        else if (Element* row = mod_details_panel->get_first_dependency_row()) { // JFG PORT
            mod_entry_buttons[active_mod_index]->set_nav(NavDirection::Right, row);
        }
        else {
            mod_entry_buttons[active_mod_index]->set_nav_none(NavDirection::Right);
        }
    }
}

void ModMenu::mod_dragged(uint32_t mod_index, EventDrag drag) {
    // JFG PORT: the gap left while dragging is as tall as the dragged entry (set at the start; entries fit their text).
    float& spacer_height = mod_drag_spacer_height;

    switch (drag.phase) {
    case DragPhase::Start: {
        for (size_t i = 0; i < mod_entry_buttons.size(); i++) {
            mod_entry_middles[i] = mod_entry_buttons[i]->get_absolute_top() + mod_entry_buttons[i]->get_client_height() / 2.0f;
        }

        // When the drag phase starts, we make the floating mod details visible and store the relative coordinate of the
        // mouse cursor. Instantly hide the real element and use a spacer in its place that will stay on the same size as
        // long as the cursor is hovering over this slot.
        float width = mod_entry_buttons[mod_index]->get_client_width();
        float height = mod_entry_buttons[mod_index]->get_client_height();
        spacer_height = height;
        // JFG PORT: where the card is, relative to this menu (positioned, so the floating card's top and left are
        // measured from it): the card stays under the cursor where it was grabbed. It was the screen position less
        // half a card, which only lined up in one layout.
        float left = mod_entry_buttons[mod_index]->get_absolute_left() - get_absolute_left();
        float top = mod_entry_buttons[mod_index]->get_absolute_top() - get_absolute_top();
        mod_entry_buttons[mod_index]->set_display(Display::None);
        mod_entry_buttons[mod_index]->set_focused(false);
        mod_entry_floating_view->set_display(Display::Flex);
        mod_entry_floating_view->set_mod_details(mod_details[mod_index]);
        mod_entry_floating_view->set_mod_thumbnail(generate_thumbnail_src_for_mod(mod_details[mod_index].mod_id));
        mod_entry_floating_view->set_mod_status(modui::status_of(mod_details[mod_index].mod_id));
        mod_entry_floating_view->set_left(left, Unit::Px);
        mod_entry_floating_view->set_top(top, Unit::Px);
        mod_entry_floating_view->set_width(width, Unit::Px);
        mod_entry_floating_view->set_height(mod_entry_buttons[mod_index]->get_card_height(), Unit::Px); // JFG PORT: not the gap below
        mod_drag_start_coordinates[0] = drag.x;
        mod_drag_start_coordinates[1] = drag.y;
        mod_drag_view_coordinates[0] = left;
        mod_drag_view_coordinates[1] = top;
        
        mod_drag_target_index = mod_index;
        mod_entry_spacers[mod_drag_target_index]->set_target_height(spacer_height, false);
        break;
    }
    case DragPhase::Move: {
        // Binary search for the drag area.
        uint32_t low = 0;
        uint32_t high = mod_entry_buttons.size();
        while (low < high) {
            uint32_t mid = low + (high - low) / 2;
            if (drag.y < mod_entry_middles[mid]) {
                high = mid;
            }
            else {
                low = mid + 1;
            }
        }
        
        uint32_t new_index = low;
        float delta_x = drag.x - mod_drag_start_coordinates[0];
        float delta_y = drag.y - mod_drag_start_coordinates[1];
        mod_entry_floating_view->set_left(mod_drag_view_coordinates[0] + delta_x, Unit::Px);
        mod_entry_floating_view->set_top(mod_drag_view_coordinates[1] + delta_y, Unit::Px);
        if (mod_drag_target_index != new_index) {
            mod_entry_spacers[mod_drag_target_index]->set_target_height(0.0f, true);
            mod_entry_spacers[new_index]->set_target_height(spacer_height, true);
            mod_drag_target_index = new_index;
        }

        break;
    }
    case DragPhase::End: {
        // Dragging has ended, hide the floating view.
        mod_entry_buttons[mod_index]->set_display(Display::Block);
        mod_entry_buttons[mod_index]->set_selected(false);
        mod_entry_spacers[mod_drag_target_index]->set_target_height(0.0f, false);
        mod_entry_floating_view->set_display(Display::None);

        // Result needs a small substraction when dragging downwards.
        if (mod_drag_target_index > mod_index) {
            mod_drag_target_index--;
        }

        // Re-order the mods and update all the details on the menu.
        recomp::mods::set_mod_index(game_mod_id, mod_details[mod_index].mod_id, mod_drag_target_index);
        mod_details = recomp::mods::get_all_mod_details(game_mod_id);
        for (size_t i = 0; i < mod_entry_buttons.size(); i++) {
            mod_entry_buttons[i]->set_mod_details(mod_details[i]);
            mod_entry_buttons[i]->set_mod_thumbnail(generate_thumbnail_src_for_mod(mod_details[i].mod_id));
            mod_entry_buttons[i]->set_mod_status(modui::status_of(mod_details[i].mod_id));
        }

        mod_entry_buttons[mod_drag_target_index]->set_selected(true);
        active_mod_index = mod_drag_target_index;
        mark_other_mod_menus_dirty(this); // JFG PORT

        break;
    }
    default:
        break;
    }
}

// TODO remove this once this is migrated to the new system.
ContextId sub_menu_context;

ContextId get_config_sub_menu_context_id() {
    return sub_menu_context;
}

// JFG PORT: there is a mod menu in the config menu and one in the launcher. They share the sub menu (a mod's options);
// sub_menu_owner is the one that opened it, so Back returns there.
static std::vector<ModMenu*> mod_menus;
static ConfigSubMenu* shared_config_sub_menu = nullptr;
static ModMenu* sub_menu_owner = nullptr;

ContextId get_config_sub_menu_parent_context() {
    return sub_menu_owner != nullptr ? sub_menu_owner->get_owner_context() : get_config_context_id();
}

// After a change made in one mod menu (enabled, order, rescan), the others read the mod list again.
// It runs from the changed menu's callbacks, with that menu's context open: only one context may be open at a time, so
// that one is closed while the others are marked, and opened again after.
static void mark_other_mod_menus_dirty(ModMenu* changed) {
    struct Reopen {
        ContextId context = recompui::try_close_current_context();
        ~Reopen() {
            if (context != ContextId::null()) context.open();
        }
    } reopen;
    for (ModMenu* menu : mod_menus) {
        if (menu == changed) continue;
        ContextId menu_context = menu->get_owner_context();
        bool opened = menu_context.open_if_not_already();
        menu->set_mods_dirty(false);
        menu->queue_update();
        if (opened) {
            menu_context.close();
        }
    }
}

bool ModMenu::handle_special_config_options(const recomp::mods::ConfigOption& option, const recomp::mods::ConfigValueVariant& config_value) {
    if (dino::renderer::is_texture_pack_enable_config_option(option, true)) {
        const recomp::mods::ConfigOptionEnum &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);

        config_sub_menu->add_radio_option(option.id, option.name, option.description, std::get<uint32_t>(config_value), option_enum.options,
            [this](const std::string &id, uint32_t value) {
                mod_enum_option_changed(id, value);
                mod_hd_textures_enabled_changed(value);
            });

        return true;
    }

    return false;
}

void ModMenu::mod_configure_requested() {
    if (active_mod_index >= 0) {
        // Record the context that was open when this function was called and close it.
        ContextId prev_context = recompui::get_current_context();
        prev_context.close();

        // Open the sub menu context and set up the element.
        sub_menu_context.open();
        config_sub_menu->clear_options();

        const recomp::mods::ConfigSchema &config_schema = recomp::mods::get_mod_config_schema(mod_details[active_mod_index].mod_id);
        for (const recomp::mods::ConfigOption &option : config_schema.options) {
            recomp::mods::ConfigValueVariant config_value = recomp::mods::get_mod_config_value(mod_details[active_mod_index].mod_id, option.id);
            if (std::holds_alternative<std::monostate>(config_value)) {
                continue;
            }

            if (handle_special_config_options(option, config_value)) {
                continue;
            }
            if (modui::is_switch_option(option)) continue; // JFG PORT: the On/Off toggle is this option

            switch (option.type) {
            case recomp::mods::ConfigOptionType::Enum: {
                const recomp::mods::ConfigOptionEnum &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
                config_sub_menu->add_radio_option(option.id, option.name, option.description, std::get<uint32_t>(config_value), option_enum.options,
                    [this](const std::string &id, uint32_t value){ mod_enum_option_changed(id, value); });
                break;
            }
            case recomp::mods::ConfigOptionType::Number: {
                const recomp::mods::ConfigOptionNumber &option_number = std::get<recomp::mods::ConfigOptionNumber>(option.variant);
                config_sub_menu->add_slider_option(option.id, option.name, option.description, std::get<double>(config_value), option_number.min, option_number.max, option_number.step, option_number.percent,
                    [this](const std::string &id, double value){ mod_number_option_changed(id, value); });
                break;
            }
            case recomp::mods::ConfigOptionType::String: {
                config_sub_menu->add_text_option(option.id, option.name, option.description, std::get<std::string>(config_value),
                    [this](const std::string &id, const std::string &value){ mod_string_option_changed(id, value); });
                break;
            }
            default:
                assert(false && "Unknown config option type.");
                break;
            }
        }

        config_sub_menu->enter(mod_details[active_mod_index].display_name);
        sub_menu_context.close();

        // Reopen the context that was open when this function was called.
        prev_context.open();
        
        // Hide the config menu (JFG PORT: or the launcher) and show the sub menu.
        sub_menu_owner = this;
        recompui::hide_context(owner_context);
        recompui::show_context(sub_menu_context, "");
    }
}

void ModMenu::mod_enum_option_changed(const std::string &id, uint32_t value) {
    if (active_mod_index >= 0) {
        recomp::mods::set_mod_config_value(mod_details[active_mod_index].mod_id, id, value);
    }
}

void ModMenu::mod_string_option_changed(const std::string &id, const std::string &value) {
    if (active_mod_index >= 0) {
        recomp::mods::set_mod_config_value(mod_details[active_mod_index].mod_id, id, value);
    }
}

void ModMenu::mod_number_option_changed(const std::string &id, double value) {
    if (active_mod_index >= 0) {
        recomp::mods::set_mod_config_value(mod_details[active_mod_index].mod_id, id, value);
    }
}

void ModMenu::mod_hd_textures_enabled_changed(uint32_t value) {
    if (active_mod_index >= 0) {
        if (value) {
            dino::renderer::secondary_enable_texture_pack(mod_details[active_mod_index].mod_id);
        }
        else {
            dino::renderer::secondary_disable_texture_pack(mod_details[active_mod_index].mod_id);
        }
    }
}

void ModMenu::create_mod_list() {
    ContextId context = get_current_context();
    
    // Clear the contents of the list scroll.
    list_scroll_container->clear_children();
    mod_entry_buttons.clear();
    mod_entry_spacers.clear();

    mod_details_panel->set_mod_list(&mod_details); // JFG PORT: for its Requires and Required by lists

    // Create the child elements for the list scroll.
    for (size_t mod_index = 0; mod_index < mod_details.size(); mod_index++) {
        const std::vector<char> &thumbnail = recomp::mods::get_mod_thumbnail(mod_details[mod_index].mod_id);
        std::string thumbnail_name = generate_thumbnail_src_for_mod(mod_details[mod_index].mod_id);
        if (!thumbnail.empty()) {
            recompui::queue_image_from_bytes_file(thumbnail_name, thumbnail);
            loaded_thumbnails.emplace(thumbnail_name);
        }

        ModEntrySpacer *spacer = context.create_element<ModEntrySpacer>(list_scroll_container);
        mod_entry_spacers.emplace_back(spacer);

        ModEntryButton *mod_entry = context.create_element<ModEntryButton>(list_scroll_container, mod_index);
        mod_entry->set_mod_selected_callback([this](uint32_t mod_index){ mod_selected(mod_index); });
        mod_entry->set_mod_activated_callback([this](uint32_t mod_index) {
            // JFG PORT: only the mod already selected (a first click selects it), and only if it has settings.
            if ((int32_t)mod_index == active_mod_index && mod_details_panel->get_configure_button()->is_enabled()) mod_configure_requested();
        });
        mod_entry->set_mod_drag_callback([this](uint32_t mod_index, recompui::EventDrag drag){ mod_dragged(mod_index, drag); });
        mod_entry->set_scale(scale, radius);
        mod_entry->set_mod_details(mod_details[mod_index]);
        mod_entry->set_mod_thumbnail(thumbnail_name);
        mod_entry->set_mod_status(modui::status_of(mod_details[mod_index].mod_id));
        mod_entry_buttons.emplace_back(mod_entry);
    }

    if (!mod_entry_buttons.empty()) {
        mod_entry_buttons.front()->set_nav_manual(NavDirection::Up, mod_tab_id);
        mod_entry_buttons.back()->set_nav(NavDirection::Down, install_mods_button);
        install_mods_button->set_nav(NavDirection::Up, mod_entry_buttons.back());
    }
    else {
        install_mods_button->set_nav_manual(NavDirection::Up, mod_tab_id);
    }

    Rml::ElementTabSet* tabset = recompui::get_config_tabset();
    if (tabset && tabset->GetActiveTab() == recompui::config_tab_to_index(ConfigTab::Mods)) {
        recompui::set_config_tabset_mod_nav();
    }       

    // JFG PORT: in the launcher, right from the Mods button goes to the first mod (spatial nav found the toggle).
    if (owner_context == recompui::get_launcher_context_id()) {
        Rml::ElementDocument* doc = owner_context.get_document();
        Rml::Element* mods_button = doc != nullptr ? doc->GetElementById(mod_tab_id.substr(1)) : nullptr;
        if (mods_button != nullptr) {
            if (!mod_entry_buttons.empty()) {
                mods_button->SetProperty(Rml::PropertyId::NavRight, Rml::Property{ "#" + mod_entry_buttons.front()->get_id(), Rml::Unit::STRING });
            }
            else {
                mods_button->SetProperty(Rml::PropertyId::NavRight, Rml::Property{ "#" + install_mods_button->get_id(), Rml::Unit::STRING });
            }
        }
    }

    // Add one extra spacer at the bottom.
    ModEntrySpacer *spacer = context.create_element<ModEntrySpacer>(list_scroll_container);
    mod_entry_spacers.emplace_back(spacer);

    mod_entry_middles.resize(mod_entry_buttons.size());

    bool mods_available = !mod_details.empty();
    body_container->set_display(mods_available ? Display::Flex : Display::None);
    body_empty_container->set_display(mods_available ? Display::None : Display::Flex);
    if (mods_available) {
        mod_selected(0);
    }
}

void ModMenu::process_event(const Event &e) {
    if (e.type == EventType::Update) {
        // JFG PORT: the launcher's mod menu is smaller (see apply_scale). Known once either menu's document exists: the
        // first update comes from update_mod_list, after one of them was made.
        ContextId launcher_context = recompui::get_launcher_context_id();
        ContextId config_context = recompui::get_config_context_id();
        if (!scale_known && (launcher_context != ContextId::null() || config_context != ContextId::null())) {
            scale_known = true;
            bool launcher = launcher_context != ContextId::null() ? owner_context == launcher_context : owner_context != config_context;
            scale = launcher ? 0.75f : 1.0f;
            radius = launcher ? 0.0f : 8.0f;
            apply_scale();
        }
        if (mods_dirty) {
            refresh_mods(mod_scan_queued);
            mods_dirty = false;
            mod_scan_queued = false;
        }
        if (ultramodern::is_game_started()) {
            // JFG PORT: mods load when the game starts, so installing and rescanning only work from the launcher. Hidden
            // in game instead of greyed out, with a note saying where to do it.
            install_mods_button->set_display(Display::None);
            refresh_button->set_display(Display::None);
            ingame_note->set_display(Display::Block);
        }
        // JFG PORT: a mod picked in the details' Requires / Required by lists. Selected here, not in the click: selecting
        // rebuilds those lists, and the clicked row with them.
        if (!pending_jump.empty()) {
            for (size_t i = 0; i < mod_details.size(); i++) {
                if (mod_details[i].mod_id == pending_jump) {
                    mod_entry_buttons[i]->focus();
                    mod_entry_buttons[i]->scroll_into_view(); // the list shows the mod picked
                    mod_selected(i);
                    break;
                }
            }
            pending_jump.clear();
        }
        if (active_mod_index != -1) {        
            bool auto_enabled = recomp::mods::is_mod_auto_enabled(mod_details[active_mod_index].mod_id);
            bool toggle_enabled = !auto_enabled && modui::can_toggle(mod_details[active_mod_index]);
            if (!toggle_enabled) {
                mod_details_panel->disable_toggle();
            }
        }
    }
}

ModMenu::ModMenu(Element *parent) : Element(parent) {
    game_mod_id = "jet-force-gemini"; // JFG PORT: the mod_game_id in src/main.cpp

    ContextId context = get_current_context();
    owner_context = context;

    set_display(Display::Flex);
    set_position(Position::Relative); // JFG PORT: the dragged card (absolute) is placed from this menu
    set_flex(1.0f, 1.0f, 100.0f);
    set_flex_direction(FlexDirection::Column);
    set_align_items(AlignItems::Center);
    set_justify_content(JustifyContent::FlexStart);
    set_width(100.0f, Unit::Percent);
    set_height(100.0f, Unit::Percent);
    
    {
        body_container = context.create_element<Container>(this, FlexDirection::Row, JustifyContent::FlexStart);
        body_container->set_flex(1.0f, 1.0f, 100.0f);
        body_container->set_width(100.0f, Unit::Percent);
        body_container->set_height(100.0f, Unit::Percent);
        {
            list_container = context.create_element<Container>(body_container, FlexDirection::Column, JustifyContent::Center);
            list_container->set_display(Display::Block);
            list_container->set_flex_basis(100.0f);
            list_container->set_align_items(AlignItems::Center);
            list_container->set_height(100.0f, Unit::Percent);
            list_container->set_background_color(Color{ 3, 8, 22, 200 }); // JFG PORT: the texture packs' panels
            list_container->set_border_width(2.0f);
            list_container->set_border_color(modui::edge);
            {
                list_scroll_container = context.create_element<ScrollContainer>(list_container, ScrollDirection::Vertical);
            } // list_container

            mod_details_panel = context.create_element<ModDetailsPanel>(body_container);
            mod_details_panel->set_mod_toggled_callback([this](bool enabled){ mod_toggled(enabled); });
            mod_details_panel->set_mod_configure_pressed_callback([this](){ mod_configure_requested(); });
            mod_details_panel->set_mod_jump_callback([this](const std::string& mod_id){ pending_jump = mod_id; queue_update(); });
        } // body_container
        
        body_empty_container = context.create_element<Container>(this, FlexDirection::Column, JustifyContent::SpaceBetween);
        body_empty_container->set_flex(1.0f, 1.0f, 100.0f);
        body_empty_container->set_display(Display::None);
        {
            context.create_element<Element>(body_empty_container);
            context.create_element<Label>(body_empty_container, "You have no mods.", LabelStyle::Large);
            context.create_element<Element>(body_empty_container);
        } // body_empty_container

        footer_container = context.create_element<Container>(this, FlexDirection::Row, JustifyContent::FlexStart);
        footer_container->set_width(100.0f, recompui::Unit::Percent);
        footer_container->set_align_items(recompui::AlignItems::Center);
        footer_container->set_background_color(modui::panel); // JFG PORT
        footer_container->set_border_width(2.0f);
        footer_container->set_border_color(modui::edge);
        {
            Button* configure_button = mod_details_panel->get_configure_button();
            install_mods_button = context.create_element<Button>(footer_container, "Install Mods", recompui::ButtonStyle::Primary);
            install_mods_button->add_pressed_callback([this](){ open_install_dialog(); });

            ingame_note = context.create_element<Label>(footer_container, "Mods can be installed and changed from the launcher.", LabelStyle::Small);
            ingame_note->set_color(Color{ COL_TEXT_DIM, 255 });
            ingame_note->set_display(Display::None);

            Element* footer_spacer = context.create_element<Element>(footer_container);
            footer_spacer->set_flex(1.0f, 0.0f);

            refresh_button = context.create_element<Button>(footer_container, "Refresh", recompui::ButtonStyle::Primary);
            refresh_button->add_pressed_callback([this](){ refresh_mods(true); mark_other_mod_menus_dirty(this); });
            refresh_button->set_nav_manual(NavDirection::Up, mod_tab_id);

            mods_folder_button = context.create_element<Button>(footer_container, "Open Mods Folder", recompui::ButtonStyle::Primary);
            mods_folder_button->add_pressed_callback([this](){ open_mods_folder(); });
            mods_folder_button->set_nav(NavDirection::Up, configure_button);
            mods_folder_button->set_nav_manual(NavDirection::Up, mod_tab_id);
        } // footer_container
    } // this
    
    mod_entry_floating_view = context.create_element<ModEntryView>(this);
    mod_entry_floating_view->set_display(Display::None);
    mod_entry_floating_view->set_position(Position::Absolute);
    mod_entry_floating_view->set_selected(true);

    context.close();

    // JFG PORT: made once, for every mod menu.
    if (shared_config_sub_menu == nullptr) {
        sub_menu_context = recompui::create_context(dino::runtime::get_asset_path("config_sub_menu.rml"));
        sub_menu_context.open();
        Rml::ElementDocument* sub_menu_doc = sub_menu_context.get_document();
        Rml::Element* config_sub_menu_generic = sub_menu_doc->GetElementById("config_sub_menu");
        ElementConfigSubMenu* config_sub_menu_element = rmlui_dynamic_cast<ElementConfigSubMenu*>(config_sub_menu_generic);
        shared_config_sub_menu = config_sub_menu_element->get_config_sub_menu_element();
        sub_menu_context.close();
    }
    config_sub_menu = shared_config_sub_menu;

    context.open();
}

ModMenu::~ModMenu() {
}

// Placeholder class until the rest of the UI refactor is finished.

// JFG PORT: the config menu's mod menu (the launcher has another, see mod_menus).
static recompui::ModMenu* config_mod_menu() {
    for (ModMenu* menu : mod_menus) {
        if (menu->get_owner_context() == recompui::get_config_context_id()) return menu;
    }
    return nullptr;
}

void update_mod_list(bool scan_mods) {
    // JFG PORT: every mod menu (config menu and launcher). Each scans the mods folder on its own update, so neither
    // can read the list before the scan.
    for (ModMenu* menu : mod_menus) {
        recompui::ContextId ui_context = menu->get_owner_context();
        bool opened = ui_context.open_if_not_already();

        menu->set_mods_dirty(scan_mods);
        menu->queue_update();

        if (opened) {
            ui_context.close();
        }
    }
}

void process_game_started() {
    for (ModMenu* menu : mod_menus) {
        recompui::ContextId ui_context = menu->get_owner_context();
        bool opened = ui_context.open_if_not_already();

        menu->queue_update();

        if (opened) {
            ui_context.close();
        }
    }
}

void set_config_tabset_mod_nav() {
    if (ModMenu* mod_menu = config_mod_menu()) {
        Rml::ElementTabSet* tabset = recompui::get_config_tabset();
        Rml::Element* tabs = recompui::get_child_by_tag(tabset, "tabs");
        if (tabs != nullptr) {
            size_t num_children = tabs->GetNumChildren();
            Element* first_mod_entry = mod_menu->get_first_mod_entry();
            if (first_mod_entry != nullptr) {
                std::string id = "#" + first_mod_entry->get_id();
                for (size_t i = 0; i < num_children; i++) {
                    tabs->GetChild(i)->SetProperty(Rml::PropertyId::NavDown, Rml::Property{ id, Rml::Unit::STRING });
                }
            }
            else {
                for (size_t i = 0; i < num_children; i++) {
                    tabs->GetChild(i)->SetProperty(Rml::PropertyId::NavDown, Rml::Style::Nav::Auto);
                }
            }
        }
    }
}

void focus_mod_configure_button() {
    ModMenu* mod_menu = sub_menu_owner != nullptr ? sub_menu_owner : config_mod_menu();
    if (mod_menu == nullptr) return;
    Element* configure_button = mod_menu->get_mod_configure_button();
    if (configure_button) {
        configure_button->focus();
    }
}

ElementModMenu::ElementModMenu(const Rml::String &tag) : Rml::Element(tag) {
    SetProperty("width", "100%");
    SetProperty("height", "100%");

    recompui::Element this_compat(this);
    recompui::ContextId context = get_current_context();
    mod_menus.push_back(context.create_element<ModMenu>(&this_compat));
}

ElementModMenu::~ElementModMenu() {

}

} // namespace recompui
