#include "ui_mod_details_panel.h"

#include "librecomp/mods.hpp"

#include <ultramodern/ultramodern.hpp>

extern "C" const char* jfg_mod_warning(const char* mod_id); // JFG PORT: src/jfg/jfg_mods.cpp

namespace recompui {

extern const std::string mod_tab_id;

// JFG PORT: the mod menu's look, shared with ui_mod_menu.cpp.
namespace modui {
    bool is_switch_option(const recomp::mods::ConfigOption& option) {
        if (option.id != "enabled" || option.type != recomp::mods::ConfigOptionType::Enum) return false;
        const auto& e = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
        return e.options.size() == 2;
    }

    bool has_switch(const std::string& mod_id) {
        const recomp::mods::ConfigSchema& schema = recomp::mods::get_mod_config_schema(mod_id);
        const auto it = schema.options_by_id.find("enabled");
        return it != schema.options_by_id.end() && is_switch_option(schema.options[it->second]);
    }

    static bool switched_off(const std::string& mod_id) {
        if (!has_switch(mod_id)) return false;
        const recomp::mods::ConfigValueVariant v = recomp::mods::get_mod_config_value(mod_id, "enabled");
        return std::holds_alternative<uint32_t>(v) && std::get<uint32_t>(v) == 0;
    }

    bool can_toggle(const recomp::mods::ModDetails& details) {
        if (details.runtime_toggleable || !ultramodern::is_game_started()) return true;
        return has_switch(details.mod_id) && recomp::mods::is_mod_enabled(details.mod_id); // loaded, with a switch
    }

    Status status_of(const std::string& mod_id) {
        if (recomp::mods::is_mod_enabled(mod_id)) return switched_off(mod_id) ? Status::Off : Status::On;
        if (recomp::mods::is_mod_auto_enabled(mod_id)) return Status::Required;
        return Status::Off;
    }

    void set_text(Element* e, float scale, float size, float line, uint32_t weight, Color color, float spacing) {
        e->set_font_size(size * scale);
        e->set_line_height(line * scale);
        e->set_font_weight(weight);
        e->set_letter_spacing(spacing * scale);
        e->set_color(color);
    }

    void set_tag(Label* tag, Status status, float scale) {
        static constexpr Color clear{ 0, 0, 0, 0 };
        const char* text = "OFF";
        Color fill = clear, edge_colour = dim, colour = dim;
        switch (status) {
        case Status::On: text = "ON"; fill = cyan; edge_colour = cyan; colour = navy; break;
        case Status::Required: text = "REQUIRED"; fill = gold; edge_colour = gold; colour = navy; break;
        case Status::Missing: text = "MISSING"; edge_colour = red; colour = red; break;
        case Status::Outdated: text = "OLD VERSION"; edge_colour = red; colour = red; break;
        case Status::Off: break;
        }
        tag->set_text(text);
        set_text(tag, scale, 18.0f, 20.0f, 700, colour, 1.5f);
        tag->set_flex(0.0f, 0.0f);
        tag->set_padding_top(3.0f * scale);
        tag->set_padding_bottom(3.0f * scale);
        tag->set_padding_left(10.0f * scale);
        tag->set_padding_right(10.0f * scale);
        tag->set_border_width(2.0f);
        tag->set_border_color(edge_colour);
        tag->set_background_color(fill);
    }

    void set_button(Button* button, float scale, float radius) {
        button->set_padding_top(12.0f * scale);
        button->set_padding_bottom(12.0f * scale);
        button->set_padding_left(22.0f * scale);
        button->set_padding_right(22.0f * scale);
        button->set_font_size(22.0f * scale);
        button->set_line_height(24.0f * scale);
        button->set_letter_spacing(1.5f * scale);
        button->set_border_width(2.0f);
        button->set_border_radius(radius);
        button->set_flex(0.0f, 0.0f);
    }
}

// ModDependencyRow

ModDependencyRow::ModDependencyRow(Element *parent, float scale, float radius, const std::string &name, const std::string &detail, modui::Status status, std::function<void()> pressed)
    : Element(parent, Events(EventType::Click, EventType::Hover, EventType::Focus)), pressed(pressed) {
    ContextId context = get_current_context();

    set_display(Display::Flex);
    set_flex_direction(FlexDirection::Row);
    set_align_items(AlignItems::Center);
    set_margin_bottom(8.0f * scale);
    set_padding_top(8.0f * scale);
    set_padding_bottom(8.0f * scale);
    set_padding_left(14.0f * scale);
    set_padding_right(12.0f * scale);
    set_border_width(2.0f);
    set_border_radius(radius);
    set_border_color(modui::edge);
    set_background_color(modui::navy);
    set_cursor(Cursor::Pointer);
    hover_style.set_border_color(modui::cyan);
    hover_style.set_background_color(modui::hover);
    focus_style.set_border_color(modui::gold);
    focus_style.set_background_color(modui::hover);
    add_style(&hover_style, hover_state);
    add_style(&focus_style, focus_state);
    enable_focus();

    Element* text = context.create_element<Element>(this);
    text->set_flex(1.0f, 1.0f);
    {
        Label* name_label = context.create_element<Label>(text, name, LabelStyle::Normal);
        modui::set_text(name_label, scale, 25.0f, 31.0f, 700, modui::text);
        if (!detail.empty()) {
            Label* detail_label = context.create_element<Label>(text, detail, LabelStyle::Small);
            modui::set_text(detail_label, scale, 20.0f, 26.0f, 400, modui::dim);
        }
    }
    Label* tag = context.create_element<Label>(this, LabelStyle::Annotation);
    tag->set_margin_left(12.0f * scale);
    modui::set_tag(tag, status, scale);
}

void ModDependencyRow::process_event(const Event &e) {
    switch (e.type) {
    case EventType::Click:
        if (pressed) pressed();
        break;
    case EventType::Hover:
        set_style_enabled(hover_state, std::get<EventHover>(e.variant).active);
        break;
    case EventType::Focus:
        set_style_enabled(focus_state, std::get<EventFocus>(e.variant).active);
        break;
    default:
        break;
    }
}

// ModDetailsPanel

ModDetailsPanel::ModDetailsPanel(Element *parent) : Element(parent) {
    set_flex(1.0f, 1.0f, 200.0f);
    set_height(100.0f, Unit::Percent);
    set_display(Display::Flex);
    set_flex_direction(FlexDirection::Column);
    set_background_color(Color{ 3, 8, 22, 200 });
    set_border_width(2.0f);
    set_border_color(modui::edge);

    ContextId context = get_current_context();

    header_container = context.create_element<Container>(this, FlexDirection::Row, JustifyContent::FlexStart);
    header_container->set_flex(0.0f, 0.0f);
    header_container->set_align_items(AlignItems::Center);
    header_container->set_background_color(modui::panel);
    header_container->set_border_bottom_width(2.0f);
    header_container->set_border_bottom_color(modui::edge);
    {
        thumbnail_container = context.create_element<Container>(header_container, FlexDirection::Column, JustifyContent::SpaceEvenly);
        thumbnail_container->set_flex(0.0f, 0.0f);
        {
            thumbnail_image = context.create_element<Image>(thumbnail_container, "");
            thumbnail_image->set_background_color(Color{ 3, 8, 22, 204 });
        }

        header_details_container = context.create_element<Container>(header_container, FlexDirection::Column, JustifyContent::Center);
        header_details_container->set_flex(1.0f, 1.0f);
        header_details_container->set_flex_basis(100.0f, Unit::Percent);
        header_details_container->set_align_items(AlignItems::FlexStart);
        header_details_container->set_text_align(TextAlign::Left);
        {
            title_label = context.create_element<Label>(header_details_container, LabelStyle::Large);
            version_label = context.create_element<Label>(header_details_container, LabelStyle::Normal);
            status_tag = context.create_element<Label>(header_details_container, LabelStyle::Annotation);
        }
    }

    body_container = context.create_element<ScrollContainer>(this, ScrollDirection::Vertical);
    body_container->set_text_align(TextAlign::Left);
    {
        description_card = context.create_element<Element>(body_container);
        description_card->set_border_left_color(modui::cyan);
        {
            description_label = context.create_element<Label>(description_card, LabelStyle::Small);
        }
        // JFG PORT: port features this mod turns off (jfg_mod_warning, src/jfg/jfg_mods.cpp).
        warning_label = context.create_element<Label>(body_container, LabelStyle::Small);
        warning_label->set_display(Display::None);
    }

    // JFG PORT: Requires and Required by, under the description and outside its scroll, so they are always in the same
    // place whatever the description's length. They scroll on their own when long.
    dependencies_container = context.create_element<Element>(this);
    dependencies_container->set_flex(0.0f, 0.0f);
    dependencies_container->set_max_height(45.0f, Unit::Percent);
    dependencies_container->set_overflow_y(Overflow::Auto);
    dependencies_container->set_text_align(TextAlign::Left);
    dependencies_container->set_border_top_width(2.0f);
    dependencies_container->set_border_top_color(modui::edge);
    dependencies_container->set_display(Display::None);

    buttons_container = context.create_element<Container>(this, FlexDirection::Row, JustifyContent::SpaceAround);
    buttons_container->set_flex(0.0f, 0.0f);
    buttons_container->set_justify_content(JustifyContent::SpaceBetween);
    buttons_container->set_align_items(AlignItems::Center);
    buttons_container->set_border_top_width(2.0f);
    buttons_container->set_border_top_color(modui::edge);
    buttons_container->set_background_color(modui::panel);
    {
        enable_container = context.create_element<Container>(buttons_container, FlexDirection::Row, JustifyContent::FlexStart);
        enable_container->set_align_items(AlignItems::Center);
        enable_container->set_flex(1.0f, 1.0f);
        enable_container->set_min_width(0.0f);
        {
            enable_toggle = context.create_element<Toggle>(enable_container);
            enable_toggle->set_flex(0.0f, 0.0f);
            enable_toggle->add_checked_callback([this](bool checked){ enable_toggle_checked(checked); });
            enable_toggle->set_nav_manual(NavDirection::Up, mod_tab_id);

            enable_text = context.create_element<Element>(enable_container);
            enable_text->set_flex(1.0f, 1.0f);
            enable_text->set_text_align(TextAlign::Left);
            {
                enable_state_label = context.create_element<Label>(enable_text, LabelStyle::Normal);
                enable_label = context.create_element<Label>(enable_text, LabelStyle::Small);
            }
        }

        configure_button = context.create_element<Button>(buttons_container, "Configure", recompui::ButtonStyle::Secondary);
        configure_button->add_pressed_callback([this](){ configure_button_pressed(); });
        configure_button->set_nav_manual(NavDirection::Up, mod_tab_id);
    }
    set_scale(1.0f, 8.0f);
    clear_mod_navigation();
}

ModDetailsPanel::~ModDetailsPanel() {
}

void ModDetailsPanel::set_scale(float scale, float radius) {
    this->scale = scale;
    this->radius = radius;
    const float s = scale;

    header_container->set_padding(20.0f * s);
    header_container->set_gap(22.0f * s);
    thumbnail_image->set_width(132.0f * s);
    thumbnail_image->set_height(132.0f * s);
    modui::set_text(title_label, s, 36.0f, 44.0f, 700, modui::gold, 1.0f);
    modui::set_text(version_label, s, 24.0f, 32.0f, 400, modui::cyan);
    version_label->set_margin_top(4.0f * s);
    status_tag->set_margin_top(10.0f * s);

    body_container->set_padding(24.0f * s);
    description_card->set_border_left_width(4.0f * s);
    description_card->set_padding_left(22.0f * s);
    description_card->set_padding_top(4.0f * s);
    description_card->set_padding_bottom(4.0f * s);
    modui::set_text(description_label, s, 27.0f, 41.0f, 400, modui::text);
    modui::set_text(warning_label, s, 24.0f, 36.0f, 400, modui::gold);
    warning_label->set_margin_top(18.0f * s);
    dependencies_container->set_padding_left(24.0f * s);
    dependencies_container->set_padding_right(24.0f * s);
    dependencies_container->set_padding_bottom(12.0f * s);

    buttons_container->set_padding(16.0f * s);
    buttons_container->set_gap(16.0f * s);
    enable_container->set_gap(16.0f * s);
    enable_toggle->set_size(108.0f * s, 48.0f * s);
    modui::set_text(enable_state_label, s, 24.0f, 30.0f, 700, modui::text, 1.0f);
    modui::set_text(enable_label, s, 20.0f, 26.0f, 400, modui::dim);
    modui::set_button(configure_button, s, radius);

    refresh_status();
}

void ModDetailsPanel::disable_toggle() {
    enable_toggle->set_enabled(false);
}

void ModDetailsPanel::set_mod_details(const recomp::mods::ModDetails& details, const std::string &thumbnail, bool toggle_checked, bool toggle_enabled, bool toggle_label_visible, bool configure_enabled) {
    cur_details = details;

    thumbnail_image->set_src(thumbnail);

    title_label->set_text(cur_details.display_name);

    // JFG PORT: the version and authors on one line under the name, as the texture packs show theirs.
    std::string meta = cur_details.version.to_string();
    bool first = true;
    for (const std::string& author : details.authors) {
        meta += (first ? "  by " : ", ") + author;
        first = false;
    }
    version_label->set_text(meta);

    const std::string &description = !cur_details.description.empty() ? cur_details.description : cur_details.short_description;
    description_label->set_text(description.empty() ? "No description." : description);
    const std::string warning = jfg_mod_warning(cur_details.mod_id.c_str()); // JFG PORT
    warning_label->set_text(warning);
    warning_label->set_display(warning.empty() ? Display::None : Display::Block);
    enable_toggle->set_checked(toggle_checked);
    enable_toggle->set_enabled(toggle_enabled);
    configure_button->set_enabled(configure_enabled);
    (void)toggle_label_visible; // the note under On/Off says why, see refresh_status
    toggle_locked_by_game = !modui::can_toggle(cur_details);

    if (configure_enabled) {
        enable_toggle->set_nav(NavDirection::Right, configure_button);
    }
    else {
        enable_toggle->set_nav_none(NavDirection::Right);
    }

    refresh_status();
}

void ModDetailsPanel::refresh_status() {
    if (cur_details.mod_id.empty()) {
        return;
    }
    modui::Status status = modui::status_of(cur_details.mod_id);
    modui::set_tag(status_tag, status, scale);
    enable_state_label->set_text(status == modui::Status::Off ? "Off" : "On");

    // Why the toggle can't change: a mod that is on needs this one, or the game is running.
    std::string note;
    if (status == modui::Status::Required) {
        std::string names;
        if (all_mods != nullptr) {
            for (const recomp::mods::ModDetails& other : *all_mods) {
                if (modui::status_of(other.mod_id) == modui::Status::Off) continue;
                for (const recomp::mods::Dependency& dep : other.dependencies) {
                    if (dep.mod_id == cur_details.mod_id) {
                        names += (names.empty() ? "" : ", ") + other.display_name;
                        break;
                    }
                }
            }
        }
        note = names.empty() ? "On because a mod that is on needs it" : "On because " + names + " needs it";
    }
    else if (toggle_locked_by_game) {
        note = "Turned on or off from the launcher";
    }
    enable_label->set_text(note);
    enable_label->set_display(note.empty() ? Display::None : Display::Block);

    request_dependencies();
}

// JFG PORT: the dependency rows are made again at the next UI update, not now: selecting a mod often comes from an event
// of one of those rows (a click on it, or the focus leaving it for the mod's card), and deleting the row while the event
// is still being handled crashed the menu.
void ModDetailsPanel::request_dependencies() {
    if (get_current_context() == ContextId::null()) { build_dependencies(); return; } // no update to wait for
    dependencies_pending = true;
    queue_update();
}

void ModDetailsPanel::process_event(const Event &e) {
    if (e.type == EventType::Update && dependencies_pending) {
        dependencies_pending = false;
        build_dependencies();
    }
}

void ModDetailsPanel::add_section_title(const std::string &text) {
    dependencies_container->set_display(Display::Block);
    Label* title = get_current_context().create_element<Label>(dependencies_container, text, LabelStyle::Normal);
    modui::set_text(title, scale, 20.0f, 26.0f, 700, modui::gold, 2.0f);
    title->set_margin_top(18.0f * scale);
    title->set_margin_bottom(12.0f * scale);
}

// JFG PORT: what this mod needs, and the mods that need it, each with its tag; a click selects that mod.
void ModDetailsPanel::build_dependencies() {
    dependencies_container->clear_children();
    dependencies_container->set_display(Display::None);
    dependency_rows.clear();
    if (all_mods == nullptr) {
        apply_dependency_navigation();
        return;
    }
    ContextId context = get_current_context();
    auto find = [this](const std::string& id) -> const recomp::mods::ModDetails* {
        for (const recomp::mods::ModDetails& m : *all_mods) {
            if (m.mod_id == id) return &m;
        }
        return nullptr;
    };
    auto jump = [this](const std::string& id) {
        return [this, id]() { if (mod_jump_callback) mod_jump_callback(id); };
    };

    if (!cur_details.dependencies.empty()) {
        add_section_title("REQUIRES");
        for (const recomp::mods::Dependency& dep : cur_details.dependencies) {
            const recomp::mods::ModDetails* found = find(dep.mod_id);
            modui::Status status = found == nullptr ? modui::Status::Missing
                : found->version < dep.version ? modui::Status::Outdated
                : modui::status_of(dep.mod_id);
            std::string detail = dep.version.to_string() + " or newer";
            if (found != nullptr && status == modui::Status::Outdated) detail += " (" + found->version.to_string() + " is installed)";
            if (dep.optional) detail += ", optional";
            if (found == nullptr) detail += ": not in the mods folder";
            dependency_rows.push_back(context.create_element<ModDependencyRow>(dependencies_container, scale, radius,
                found != nullptr ? found->display_name : dep.mod_id, detail, status, found != nullptr ? jump(dep.mod_id) : std::function<void()>{}));
        }
    }

    bool any = false;
    for (const recomp::mods::ModDetails& other : *all_mods) {
        for (const recomp::mods::Dependency& dep : other.dependencies) {
            if (dep.mod_id != cur_details.mod_id) continue;
            if (!any) add_section_title("REQUIRED BY");
            any = true;
            dependency_rows.push_back(context.create_element<ModDependencyRow>(dependencies_container, scale, radius, other.display_name,
                other.version.to_string() + (dep.optional ? ", uses it if it is on" : ""), modui::status_of(other.mod_id), jump(other.mod_id)));
            break;
        }
    }
    apply_dependency_navigation();
}

// JFG PORT: the dependency rows sit between the description and the toggle: up from the toggle (or Configure) reaches
// the last, down from the last returns, and left goes back to the selected mod's card.
void ModDetailsPanel::apply_dependency_navigation() {
    Element* below = enable_toggle->is_enabled() ? static_cast<Element*>(enable_toggle) : static_cast<Element*>(configure_button);
    for (size_t i = 0; i < dependency_rows.size(); i++) {
        ModDependencyRow* row = dependency_rows[i];
        if (i > 0) row->set_nav(NavDirection::Up, dependency_rows[i - 1]);
        else row->set_nav_manual(NavDirection::Up, mod_tab_id);
        row->set_nav(NavDirection::Down, i + 1 < dependency_rows.size() ? static_cast<Element*>(dependency_rows[i + 1]) : below);
        if (list_nav_target != nullptr) row->set_nav(NavDirection::Left, list_nav_target);
        else row->set_nav_none(NavDirection::Left);
        row->set_nav_none(NavDirection::Right);
    }
    if (!dependency_rows.empty()) {
        enable_toggle->set_nav(NavDirection::Up, dependency_rows.back());
        configure_button->set_nav(NavDirection::Up, dependency_rows.back());
    }
    else {
        enable_toggle->set_nav_manual(NavDirection::Up, mod_tab_id);
        configure_button->set_nav_manual(NavDirection::Up, mod_tab_id);
    }
}

void ModDetailsPanel::set_mod_toggled_callback(std::function<void(bool)> callback) {
    mod_toggled_callback = callback;
}

void ModDetailsPanel::set_mod_configure_pressed_callback(std::function<void()> callback) {
    mod_configure_pressed_callback = callback;
}

void ModDetailsPanel::setup_mod_navigation(Element* nav_target) {
    list_nav_target = nav_target;
    apply_dependency_navigation();
    enable_toggle->set_nav(NavDirection::Left, nav_target);

    if (enable_toggle->is_enabled()) {
        configure_button->set_nav(NavDirection::Left, enable_toggle);
    }
    else {
        configure_button->set_nav(NavDirection::Left, nav_target);
    }
}

void ModDetailsPanel::clear_mod_navigation() {
    list_nav_target = nullptr;
    enable_toggle->set_nav_none(NavDirection::Left);
    configure_button->set_nav_none(NavDirection::Left);
}

void ModDetailsPanel::enable_toggle_checked(bool checked) {
    if (mod_toggled_callback != nullptr) {
        mod_toggled_callback(checked);
    }
}

void ModDetailsPanel::configure_button_pressed() {
    if (mod_configure_pressed_callback != nullptr) {
        mod_configure_pressed_callback();
    }
}

} // namespace recompui
