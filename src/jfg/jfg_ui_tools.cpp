// Jet Force Gemini port: F10's tools bar and its windows, in the game's own RmlUi interface (src/ui). F9 keeps the
// original all-in-one Dear ImGui window (jfg_ui_window.cpp).
//
// The bar has drop-down menus (Cheats, Tools, Profile, Textures, Game) and the status of the session on the right;
// Tools opens the Level Warp, Items and Weapons windows. While the bar has the mouse and keyboard the game gets no
// input; Alt on its own, or a click outside the bar and its windows, hands them to the game (the bar stays up), Alt
// again takes them back. F10 shows and hides it.
//
// With a controller: the Select menu's tools button opens it on the Cheats menu. The D-pad / left stick go along the bar
// (each menu opens as its button gets the focus) and down through a menu, LB / RB go to the menu before / after, B
// closes the menu, then the window opened last, then the bar; Start hands the controller to the game (the bar stays up;
// the tools button takes it back).
//
// Threads: the interface is built and updated on the UI thread (jfg_tools_ui_frame, every UI frame, from
// src/ui/ui_state.cpp). Everything that reads or changes the game runs on the game thread: the bar queues it
// (run_on_game) and tools_game_tick carries it out, and also reads the values the bar shows (every 10 ticks while the
// bar is up), which the UI thread then puts on screen. The rows of the windows are ToolFields (jfg_port.h), shared with
// F9's tabs where they come from (item_fields, weapon_fields).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "ui/recomp_ui.h"
#include "ui/elements/ui_element.h"
#include "ui/elements/ui_button.h"
#include "ui/elements/ui_clickable.h"
#include "ui/elements/ui_label.h"
#include "ui/elements/ui_scroll_container.h"
#include "ui/elements/ui_text_input.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

std::atomic<float> g_toolbar_height{ 0.0f };

namespace {
namespace ui = recompui;

// --- look (a menu bar in Jet Force Gemini colours, as the launcher: space navy, "Gemini" cyan, emblem gold) ---------
constexpr ui::Color BAR_BG{ 5, 11, 28, 245 }, PANEL_BG{ 11, 23, 51, 250 }, BORDER{ 31, 61, 115, 255 };
constexpr ui::Color TEXT{ 232, 243, 255, 255 }, TEXT_DIM{ 129, 153, 191, 255 }, ACCENT{ 60, 200, 255, 255 };
constexpr ui::Color BUTTON_BG{ 14, 30, 66, 255 }, BUTTON_HOVER{ 26, 56, 118, 255 }, ON_BG{ 20, 104, 184, 255 }, ON_HOVER{ 30, 144, 232, 255 };
constexpr ui::Color DRAWER_BG{ 8, 17, 40, 255 }, LIST_BG{ 6, 13, 32, 255 }, NO_BORDER{ 0, 0, 0, 0 };
constexpr ui::Color GOLD{ 255, 178, 26, 255 };
constexpr float FONT = 15.0f, FONT_SMALL = 13.0f;

// --- game thread <-> UI thread ---------------------------------------------------------------------------------------
std::mutex g_cmd_mutex;
std::vector<std::function<void(recomp_context*)>> g_cmds;
void run_on_game(std::function<void(recomp_context*)> f) {
    std::lock_guard lock(g_cmd_mutex);
    g_cmds.push_back(std::move(f));
}

enum Group { GROUP_BAR = 0, GROUP_ITEMS = 1, GROUP_WEAPONS = 2 };

// A field on screen. The list is built once on the UI thread (before g_ready) and not changed after.
struct Bound {
    ToolField field;
    int group = GROUP_BAR;
    ui::Label* label = nullptr;   // the row's text (replaced by field.text when that is given)
    ui::Button* toggle = nullptr; // Toggle: ON / OFF
    ui::Label* value = nullptr;   // Number: the value
    int shown = INT_MIN;
    std::string shown_text;
};
std::vector<Bound> g_bound;
std::atomic<bool> g_ready{ false };
std::atomic<bool> g_shown{ false };
std::atomic<uint32_t> g_groups{ 1u << GROUP_BAR }; // groups the game thread reads (the bar, open windows)

struct Value { bool valid = false; int v = 0; std::string text; };
std::mutex g_values_mutex;
std::vector<Value> g_values;
bool g_values_new = false;

// Keys (from the SDL event filter, src/debug_ui/backend.cpp): F10 and Alt pressed on its own. The Select menu's tools
// button (src/ui/ui_config.cpp).
std::atomic<bool> g_toggle_request{ false }, g_alt_request{ false }, g_menu_open_request{ false };

// --- the interface (UI thread) ---------------------------------------------------------------------------------------
struct Window {
    ui::Element* frame = nullptr;
    ui::Element* body = nullptr;
    ui::Button* close = nullptr; // where the controller lands when the window opens
    int group = -1;
    bool open = false;
    float drag_x = 0, drag_y = 0, start_left = 0, start_top = 0;
};

// A menu's button on the bar. When it gets the focus (the controller along the bar, or the mouse over it while another
// menu is open) its menu opens, as in a menu bar.
class BarButton : public ui::Button {
public:
    BarButton(ui::Element* parent, const std::string& label) : ui::Button(parent, label, ui::ButtonStyle::Secondary) {}
    std::function<void()> focused;
protected:
    void process_event(const ui::Event& e) override {
        ui::Button::process_event(e);
        if (e.type == ui::EventType::Focus && std::get<ui::EventFocus>(e.variant).active && focused) focused();
    }
};

struct Menu {
    BarButton* button = nullptr;                 // on the bar
    ui::Element* menu = nullptr;                 // its drop-down
    std::vector<std::vector<ui::Button*>> rows;  // its buttons for the controller, top to bottom (side by side ones share a row)
};

struct Tools {
    ui::ContextId context = ui::ContextId::null();
    ui::Element* bar = nullptr;
    ui::Label* input_status = nullptr;
    std::vector<Menu> menus;
    ui::Element* open_menu = nullptr;
    bool opened_by_focus = false; // the open menu opened as its button got the focus: pressing the button goes into it
    bool quiet_focus = false;     // the focus is put back on a bar button: its menu stays closed
    float menu_left = -1.0f, menu_top = -1.0f; // where the open drop-down was last put (place_open_menu)
    std::vector<Window*> window_order; // open windows, the last opened last (B closes it first)
    ui::Element* profile_menu = nullptr;
    ui::Element* textures_menu = nullptr;
    Window warp, items, weapons;
    // Level warp
    ui::Element* level_list = nullptr;
    std::vector<std::pair<int, ui::Button*>> level_buttons; // level number, its row in the list
    ui::Label* warp_choice = nullptr;
    ui::Button* character_buttons[3] = {};
    std::string level_filter;
    int warp_level = 0, warp_character = 1, warp_entrance = 0;
    bool captures = true;
} t;
std::atomic<int> g_here_level{ -1 }, g_here_entrance{ 0 }; // the current level and entrance, as the game thread last read them

// While a drop-down is built its buttons are noted, a row each unless they sit side by side in a row of their own
// (wire_menu_nav joins them up for the controller).
std::vector<std::vector<ui::Button*>>* g_rows = nullptr;
ui::Element* g_rows_root = nullptr;
ui::Element* g_rows_parent = nullptr;

void note_button(ui::Element* parent, ui::Button* b) {
    if (g_rows == nullptr) return;
    if (g_rows->empty() || parent != g_rows_parent || parent == g_rows_root) g_rows->emplace_back();
    g_rows->back().push_back(b);
    g_rows_parent = parent;
}

void note_buttons_of(Menu* m) {
    g_rows = m != nullptr ? &m->rows : nullptr;
    g_rows_root = m != nullptr ? m->menu : nullptr;
    g_rows_parent = nullptr;
    if (m != nullptr) m->rows.clear();
}

void style_text(ui::Label* l, float size, ui::Color color) {
    l->set_font_size(size);
    l->set_line_height(size * 1.3f);
    l->set_letter_spacing(0.0f);
    l->set_font_weight(400);
    l->set_color(color);
}

ui::Label* text(ui::Element* parent, const std::string& s, float size = FONT, ui::Color color = TEXT) {
    ui::Label* l = t.context.create_element<ui::Label>(parent, s, ui::LabelStyle::Small);
    style_text(l, size, color);
    return l;
}

ui::Element* row(ui::Element* parent, float gap = 8.0f) {
    ui::Element* r = t.context.create_element<ui::Element>(parent);
    r->set_display(ui::Display::Flex);
    r->set_flex_direction(ui::FlexDirection::Row);
    r->set_align_items(ui::AlignItems::Center);
    r->set_gap(gap);
    return r;
}

ui::Element* column(ui::Element* parent, float gap = 4.0f) {
    ui::Element* c = t.context.create_element<ui::Element>(parent);
    c->set_display(ui::Display::Flex);
    c->set_flex_direction(ui::FlexDirection::Column);
    c->set_gap(gap);
    return c;
}

void style_button(ui::Button* b, ui::Color bg, ui::Color hover) {
    // Less below than above: the line box keeps room under the letters for descenders, so equal padding left a bigger
    // gap under the text than over it (measured on a capture: 3 / 2 gives the same space above and below in the bar).
    b->set_padding_top(3.0f); b->set_padding_bottom(2.0f); b->set_padding_left(10.0f); b->set_padding_right(10.0f);
    b->set_border_radius(3.0f);
    b->set_border_width(1.0f);
    b->set_border_color(BORDER);
    b->set_background_color(bg);
    b->set_font_size(FONT);
    b->set_line_height(FONT * 1.3f);
    b->set_letter_spacing(0.0f);
    b->set_font_weight(400);
    b->set_color(TEXT);
    b->get_hover_style()->set_background_color(hover);
    b->get_hover_style()->set_border_color(ACCENT);
    b->get_focus_style()->set_background_color(hover);
    b->get_focus_style()->set_border_color(ACCENT); // the controller's place, as the mouse's
}

ui::Button* button(ui::Element* parent, const std::string& label, std::function<void()> pressed) {
    ui::Button* b = t.context.create_element<ui::Button>(parent, label, ui::ButtonStyle::Secondary);
    style_button(b, BUTTON_BG, BUTTON_HOVER);
    b->add_pressed_callback(std::move(pressed));
    note_button(parent, b);
    return b;
}

void show_toggle_state(ui::Button* b, bool on) {
    b->set_text(on ? "ON" : "OFF");
    b->set_background_color(on ? ON_BG : BUTTON_BG);
    b->get_hover_style()->set_background_color(on ? ON_HOVER : BUTTON_HOVER);
}

// An ON / OFF button and its label.
ui::Button* toggle(ui::Element* parent, const std::string& label, bool on, std::function<void(bool)> changed) {
    ui::Element* r = row(parent);
    ui::Button* b = t.context.create_element<ui::Button>(r, "", ui::ButtonStyle::Secondary);
    style_button(b, BUTTON_BG, BUTTON_HOVER);
    b->set_min_width(50.0f);
    b->set_text_align(ui::TextAlign::Center);
    show_toggle_state(b, on);
    note_button(r, b);
    text(r, label);
    // The state lives in the button's text; flip it here so a quick second click sees the new state.
    auto state = std::make_shared<bool>(on);
    b->add_pressed_callback([b, state, changed]() { *state = !*state; show_toggle_state(b, *state); changed(*state); });
    return b;
}

// One ToolField as a row of a window (or a menu). Its value is filled in by refresh_values.
void add_field(ui::Element* parent, const ToolField& f, int group) {
    Bound bound{ f, group };
    const size_t index = g_bound.size();
    switch (f.kind) {
    case ToolField::Heading:
        bound.label = text(parent, f.label, FONT + 2.0f, ACCENT);
        bound.label->set_margin_top(14.0f);
        bound.label->set_font_weight(700);
        break;
    case ToolField::Note:
        bound.label = text(parent, f.label, FONT_SMALL, TEXT_DIM);
        break;
    case ToolField::Toggle: {
        ui::Element* r = row(parent);
        bound.toggle = t.context.create_element<ui::Button>(r, "", ui::ButtonStyle::Secondary);
        style_button(bound.toggle, BUTTON_BG, BUTTON_HOVER);
        bound.toggle->set_min_width(50.0f);
        bound.toggle->set_text_align(ui::TextAlign::Center);
        show_toggle_state(bound.toggle, false);
        note_button(r, bound.toggle);
        bound.label = text(r, f.label);
        bound.toggle->add_pressed_callback([index]() {
            Bound& b = g_bound[index];
            const int v = b.shown == 1 ? 0 : 1;
            b.shown = v;
            show_toggle_state(b.toggle, v != 0);
            auto set = b.field.set;
            if (set) run_on_game([set, v](recomp_context* c) { set(c, v); });
        });
        break;
    }
    case ToolField::Number: {
        ui::Element* r = row(parent, 4.0f);
        auto step = [index](int delta) {
            return [index, delta]() {
                Bound& b = g_bound[index];
                if (b.shown == INT_MIN) return;
                const int v = std::clamp(b.shown + delta, b.field.lo, b.field.hi);
                b.shown = v;
                b.value->set_text(std::to_string(v));
                auto set = b.field.set;
                if (set) run_on_game([set, v](recomp_context* c) { set(c, v); });
            };
        };
        if (f.big_step != f.step) button(r, "-" + std::to_string(f.big_step), step(-f.big_step));
        button(r, "-", step(-f.step));
        bound.value = text(r, "?");
        bound.value->set_min_width(44.0f);
        bound.value->set_text_align(ui::TextAlign::Center);
        button(r, "+", step(f.step));
        if (f.big_step != f.step) button(r, "+" + std::to_string(f.big_step), step(f.big_step));
        bound.label = text(r, f.label);
        bound.label->set_margin_left(8.0f);
        break;
    }
    case ToolField::Action: {
        auto set = f.set;
        ui::Element* r = row(parent);
        button(r, f.label, [set]() { if (set) run_on_game([set](recomp_context* c) { set(c, 1); }); });
        break;
    }
    }
    g_bound.push_back(std::move(bound));
}

// A drawer: a heading to click that shows or hides the rows under it, closed at first (like F9's collapsing headers).
// Returns the element its rows go into.
ui::Element* add_drawer(ui::Element* parent, const std::string& title) {
    ui::Button* header = t.context.create_element<ui::Button>(parent, "+   " + title, ui::ButtonStyle::Secondary);
    style_button(header, DRAWER_BG, BUTTON_HOVER);
    header->set_text_align(ui::TextAlign::Left);
    header->set_font_weight(700);
    header->set_color(ACCENT);
    ui::Element* content = column(parent, 5.0f);
    content->set_display(ui::Display::None);
    content->set_padding_left(10.0f);
    content->set_padding_top(2.0f);
    content->set_padding_bottom(6.0f);
    auto open = std::make_shared<bool>(false);
    header->add_pressed_callback([header, content, open, title]() {
        *open = !*open;
        content->set_display(*open ? ui::Display::Flex : ui::Display::None);
        header->set_text((*open ? "-   " : "+   ") + title);
    });
    return content;
}

// A window's ToolFields, each Heading starting a drawer that holds the rows after it (rows before the first heading
// stay above the drawers).
void add_fields(ui::Element* body, const std::vector<ToolField>& fields, int group) {
    ui::Element* into = body;
    for (const ToolField& f : fields) {
        if (f.kind == ToolField::Heading) into = add_drawer(body, f.label);
        else add_field(into, f, group);
    }
}

void close_menus() {
    for (Menu& m : t.menus) {
        m.menu->set_display(ui::Display::None);
        m.button->set_nav_none(ui::NavDirection::Down); // nothing to go down into
    }
    t.open_menu = nullptr;
    t.opened_by_focus = false;
}

int open_menu_index() {
    for (size_t i = 0; i < t.menus.size(); i++) if (t.menus[i].menu == t.open_menu) return (int)i;
    return -1;
}

// The focus back on a bar button without opening its menu.
void focus_bar_button(size_t i) {
    t.quiet_focus = true;
    t.menus[i].button->focus();
    t.quiet_focus = false;
}

// Closes the open menu after one of its buttons did its job; the controller goes back to the menu's button on the bar
// (the button it was on is hidden now).
void close_menus_to_bar() {
    const int i = open_menu_index();
    close_menus();
    if (i >= 0 && recompui::get_cont_active()) focus_bar_button(i);
}

ui::Button* first_item(const Menu& m) {
    return m.rows.empty() || m.rows[0].empty() ? nullptr : m.rows[0][0];
}

// The controller's way round a drop-down: up and down its rows (from the last round to the first, up from the first to
// the bar), along a row, and from a row's ends to the menus either side.
void wire_menu_nav(size_t i) {
    Menu& m = t.menus[i];
    const size_t n = t.menus.size();
    ui::Button* prev = t.menus[(i + n - 1) % n].button;
    ui::Button* next = t.menus[(i + 1) % n].button;
    for (size_t r = 0; r < m.rows.size(); r++) {
        std::vector<ui::Button*>& row = m.rows[r];
        for (size_t c = 0; c < row.size(); c++) {
            row[c]->set_nav(ui::NavDirection::Up, r == 0 ? m.button : m.rows[r - 1][0]);
            row[c]->set_nav(ui::NavDirection::Down, r + 1 == m.rows.size() ? m.rows[0][0] : m.rows[r + 1][0]);
            row[c]->set_nav(ui::NavDirection::Left, c == 0 ? prev : row[c - 1]);
            row[c]->set_nav(ui::NavDirection::Right, c + 1 == row.size() ? next : row[c + 1]);
        }
    }
}

void rebuild_profile_menu();
void rebuild_textures_menu();

// Opens a menu's drop-down under its button (closing any other).
// Puts the open drop-down under its button. Also every frame (jfg_tools_ui_frame): just after the bar is first shown its
// buttons are not laid out at their full height yet (their text comes later), and a menu placed only once then stayed
// over the bar, hiding the menu names (QA F-004, the Select menu's tools button with the controller).
void place_open_menu() {
    const int i = open_menu_index();
    if (i < 0) return;
    Menu& m = t.menus[i];
    const float left = m.button->get_absolute_left();
    const float top = m.button->get_absolute_top() + m.button->get_client_height() + 2.0f;
    if (left == t.menu_left && top == t.menu_top) return;
    t.menu_left = left;
    t.menu_top = top;
    m.menu->set_left(left, ui::Unit::Px);
    m.menu->set_top(top, ui::Unit::Px);
}

void open_menu(size_t i) {
    Menu& m = t.menus[i];
    close_menus();
    t.open_menu = m.menu;
    t.menu_left = t.menu_top = -1.0f;
    place_open_menu();
    m.menu->set_display(ui::Display::Flex);
    // Rebuilt each time: profiles and packs can be added while the game runs.
    if (m.menu == t.profile_menu || m.menu == t.textures_menu) {
        note_buttons_of(&m);
        if (m.menu == t.profile_menu) rebuild_profile_menu();
        else rebuild_textures_menu();
        note_buttons_of(nullptr);
    }
    wire_menu_nav(i);
    if (ui::Button* first = first_item(m)) m.button->set_nav(ui::NavDirection::Down, first);
}

// A menu button was pressed: its drop-down opens under it (or closes if it was open). With the controller the focus goes
// to its first row. A menu that opened as its button got the focus stays open: the press goes into it.
void toggle_menu(size_t i) {
    Menu& m = t.menus[i];
    if (t.open_menu == m.menu && !t.opened_by_focus) {
        close_menus();
        return;
    }
    if (t.open_menu != m.menu) open_menu(i);
    t.opened_by_focus = false;
    if (recompui::get_cont_active()) {
        if (ui::Button* first = first_item(m)) first->focus();
    }
}

// What the bar says about the input, on the right: who has it and how to hand it over.
void show_input_status() {
    if (t.input_status == nullptr) return;
    const bool pad = recompui::get_cont_active();
    t.input_status->set_text(t.captures ? (pad ? "LB / RB: menus   B: back   Start: play" : "Mouse & keys: tools  (Alt / click outside: game)")
                                        : (pad ? "Controller: game  (Select > Tools: back to the tools)" : "Mouse & keys: game  (Alt: tools)"));
    t.input_status->set_color(t.captures ? ACCENT : GOLD);
}

void set_captures(bool captures) {
    t.captures = captures;
    t.context.set_captures_input(captures);
    t.context.set_captures_mouse(captures);
    if (!captures) close_menus();
    show_input_status();
}

ui::Element* add_menu(const std::string& title) {
    ui::Element* menu = t.context.create_element<ui::Element>(t.context.get_root_element());
    menu->set_position(ui::Position::Absolute);
    menu->set_display(ui::Display::None);
    menu->set_flex_direction(ui::FlexDirection::Column);
    menu->set_gap(6.0f);
    menu->set_padding(10.0f);
    menu->set_background_color(PANEL_BG);
    menu->set_border_width(1.0f);
    menu->set_border_color(BORDER);
    menu->set_max_height(80.0f, ui::Unit::Percent);
    menu->set_overflow_y(ui::Overflow::Auto);
    BarButton* b = t.context.create_element<BarButton>(t.bar, title);
    style_button(b, BAR_BG, BUTTON_HOVER);
    b->set_border_color(BAR_BG);
    b->set_nav_none(ui::NavDirection::Up);
    b->set_nav_none(ui::NavDirection::Down);
    const size_t i = t.menus.size();
    b->add_pressed_callback([i]() { toggle_menu(i); });
    b->focused = [i]() {
        // Only once the bar is in use: a menu open, or the controller going along the bar.
        if (t.quiet_focus || t.open_menu == t.menus[i].menu) return;
        if (t.open_menu == nullptr && !recompui::get_cont_active()) return;
        open_menu(i);
        t.opened_by_focus = true;
    };
    t.menus.push_back(Menu{ b, menu, {} });
    note_buttons_of(&t.menus.back()); // the buttons added next are this menu's
    return menu;
}

// Left and right along the bar, round from the last menu to the first (once all the menus are there).
void wire_bar_nav() {
    const size_t n = t.menus.size();
    for (size_t i = 0; i < n; i++) {
        t.menus[i].button->set_nav(ui::NavDirection::Left, t.menus[(i + n - 1) % n].button);
        t.menus[i].button->set_nav(ui::NavDirection::Right, t.menus[(i + 1) % n].button);
    }
}

void open_window(Window& w, bool open) {
    w.open = open;
    w.frame->set_display(open ? ui::Display::Flex : ui::Display::None);
    t.window_order.erase(std::remove(t.window_order.begin(), t.window_order.end(), &w), t.window_order.end());
    if (open) t.window_order.push_back(&w);
    // The controller goes into a window that opens, and back to the Tools menu's button (where windows open from) when
    // one closes.
    if (recompui::get_cont_active()) {
        if (open) w.close->focus();
        else if (t.menus.size() > 1) focus_bar_button(1);
    }
    uint32_t groups = 1u << GROUP_BAR;
    if (t.items.open) groups |= 1u << GROUP_ITEMS;
    if (t.weapons.open) groups |= 1u << GROUP_WEAPONS;
    g_groups = groups;
}

// A floating window: a title bar to drag it by, a close button, a scrolling body (scrolls false: a body that fills the
// window and does not scroll, for a window that scrolls only a part of itself).
void make_window(Window& w, const std::string& title, float left, float top, float width, int group, bool scrolls = true) {
    w.group = group;
    w.frame = t.context.create_element<ui::Element>(t.context.get_root_element());
    w.frame->set_position(ui::Position::Absolute);
    w.frame->set_left(left, ui::Unit::Percent);
    w.frame->set_top(top, ui::Unit::Percent);
    w.frame->set_width(width, ui::Unit::Percent);
    w.frame->set_height(72.0f, ui::Unit::Percent);
    w.frame->set_display(ui::Display::None);
    w.frame->set_flex_direction(ui::FlexDirection::Column);
    w.frame->set_background_color(PANEL_BG);
    w.frame->set_border_width(1.0f);
    w.frame->set_border_color(BORDER);
    ui::Clickable* title_bar = t.context.create_element<ui::Clickable>(w.frame, true);
    title_bar->set_display(ui::Display::Flex);
    title_bar->set_flex_direction(ui::FlexDirection::Row);
    title_bar->set_align_items(ui::AlignItems::Center);
    title_bar->set_justify_content(ui::JustifyContent::SpaceBetween);
    title_bar->set_padding(5.0f);
    title_bar->set_padding_left(10.0f);
    title_bar->set_background_color(BAR_BG);
    text(title_bar, title, FONT, TEXT)->set_font_weight(700);
    Window* wp = &w;
    w.close = button(title_bar, "Close", [wp]() { open_window(*wp, false); });
    title_bar->add_dragged_callback([wp](float x, float y, ui::DragPhase phase) {
        if (phase == ui::DragPhase::Start) {
            wp->drag_x = x; wp->drag_y = y;
            wp->start_left = wp->frame->get_absolute_left(); wp->start_top = wp->frame->get_absolute_top();
        }
        else if (phase == ui::DragPhase::Move) {
            wp->frame->set_left(wp->start_left + (x - wp->drag_x), ui::Unit::Px);
            wp->frame->set_top(std::max(0.0f, wp->start_top + (y - wp->drag_y)), ui::Unit::Px);
        }
    });
    if (scrolls) {
        ui::ScrollContainer* scroll = t.context.create_element<ui::ScrollContainer>(w.frame, ui::ScrollDirection::Vertical);
        w.body = column(scroll, 5.0f);
    }
    else {
        w.body = column(w.frame, 5.0f);
        w.body->set_flex(1.0f, 1.0f, 0.0f);
        w.body->set_min_height(0.0f);
        w.body->set_overflow(ui::Overflow::Hidden);
    }
    w.body->set_padding(10.0f);
}

void rebuild_profile_menu() {
    ui::Element* m = t.profile_menu;
    m->clear_children();
    list_user_profiles();
    load_profile_choice();
    text(m, "Built-in", FONT_SMALL, TEXT_DIM);
    // Choosing a profile first shows what it changes (issue #7) in a panel under the list, applied with Apply. The panel
    // is updated in place: the menu is not rebuilt from inside one of its own buttons.
    // With the controller the focus goes to Apply when the panel shows, and back to the profile's button from it.
    struct Confirm {
        std::string pending;
        ui::Element* panel = nullptr;
        ui::Label* changes = nullptr;
        ui::Button* apply = nullptr;
        ui::Button* cancel = nullptr;
        ui::Button* from = nullptr; // the chosen profile's button
    };
    auto c = std::make_shared<Confirm>();
    auto show = [c](const std::string& name) {
        c->pending = name;
        const std::string preview = profile_preview_text(name);
        c->changes->set_text("Apply \"" + name + "\"?\n" + (preview.empty() ? std::string("It changes no settings.") : preview));
        c->panel->set_display(ui::Display::Flex);
    };
    auto add = [m, c, show](const std::string& name) {
        auto self = std::make_shared<ui::Button*>(nullptr);
        ui::Button* b = button(m, (g_active_profile == name ? "> " : "   ") + name, [name, c, show, self]() {
            c->from = *self;
            show(name);
            if (recompui::get_cont_active()) {
                c->apply->set_nav(ui::NavDirection::Up, c->from);
                c->cancel->set_nav(ui::NavDirection::Up, c->from);
                c->apply->focus();
            }
        });
        *self = b;
        if (g_active_profile == name) b->set_color(ACCENT);
    };
    for (const BuiltinProfile& p : BUILTIN_PROFILES) add(p.name);
    if (!g_user_profiles.empty()) text(m, "Your profiles", FONT_SMALL, TEXT_DIM);
    for (const std::string& n : std::vector<std::string>(g_user_profiles)) add(n);
    toggle(m, "Keep my controls (the Controls tab stays as it is)", g_profile_keep_controls, [c, show](bool on) {
        set_profile_keep_controls(on);
        if (!c->pending.empty()) show(c->pending);
    });
    // The panel's buttons are not among the menu's rows: hidden until a profile is chosen, the controller is put on them.
    auto* rows = g_rows;
    g_rows = nullptr;
    c->panel = column(m, 6.0f);
    c->panel->set_display(ui::Display::None);
    c->panel->set_max_width(520.0f);
    c->changes = text(c->panel, "", FONT_SMALL, TEXT);
    ui::Element* buttons = row(c->panel);
    c->apply = button(buttons, "Apply", [c]() {
        const std::string name = c->pending;
        if (!name.empty()) run_on_game([name](recomp_context*) { choose_profile(name); });
        close_menus_to_bar();
    });
    c->cancel = button(buttons, "Cancel", [c]() {
        c->pending.clear();
        c->panel->set_display(ui::Display::None);
        if (recompui::get_cont_active() && c->from != nullptr) c->from->focus();
    });
    c->apply->set_nav(ui::NavDirection::Right, c->cancel);
    c->apply->set_nav_none(ui::NavDirection::Left);
    c->apply->set_nav_none(ui::NavDirection::Down);
    c->cancel->set_nav(ui::NavDirection::Left, c->apply);
    c->cancel->set_nav_none(ui::NavDirection::Right);
    c->cancel->set_nav_none(ui::NavDirection::Down);
    g_rows = rows;
    text(m, "Save and edit profiles in the Select / Esc menu > Profiles.", FONT_SMALL, TEXT_DIM);
}

void rebuild_textures_menu() {
    ui::Element* m = t.textures_menu;
    m->clear_children();
    std::string current;
    { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
    text(m, "Texture pack", FONT_SMALL, TEXT_DIM);
    std::vector<std::string> packs{ "" };
    for (const std::string& p : list_texture_packs()) packs.push_back(p);
    for (const std::string& p : packs) {
        ui::Button* b = button(m, (p == current ? "> " : "   ") + pack_display_name(p), [p]() { set_texture_pack(p); close_menus_to_bar(); });
        if (p == current) b->set_color(ACCENT);
    }
    button(m, "Reload pack (F6: next pack)", [current]() { set_texture_pack(current); close_menus_to_bar(); });
    text(m, "Texture dumping (texture_dump folder)", FONT_SMALL, TEXT_DIM);
    toggle(m, "Dump textures (this session)", g.texture_dump, [](bool on) { run_on_game([on](recomp_context*) { g.texture_dump = on; }); });
    toggle(m, "One folder per level", g.dump_by_level, [](bool on) { run_on_game([on](recomp_context*) { g.dump_by_level = on; save_options(); }); });
}

// The chosen level's row has an outline; the others none (their border stays, see-through, so rows never shift).
void outline_chosen_level() {
    for (auto& [level, b] : t.level_buttons) {
        const bool chosen = level == t.warp_level;
        b->set_border_color(chosen ? ACCENT : NO_BORDER);
        b->get_hover_style()->set_border_color(chosen ? ACCENT : BORDER);
        b->set_color(chosen ? ACCENT : TEXT);
    }
}

void update_warp_choice();

void rebuild_level_list() {
    t.level_list->clear_children();
    t.level_buttons.clear();
    load_level_names();
    for (int i = 0; i < (int)g_level_names.size() && t.level_buttons.size() < 400; i++) {
        if (!contains_ci(g_level_names[i], t.level_filter.c_str())) continue;
        char label[96];
        std::snprintf(label, sizeof(label), "%3d   %s", i, g_level_names[i].c_str());
        ui::Button* b = t.context.create_element<ui::Button>(t.level_list, label, ui::ButtonStyle::Secondary);
        style_button(b, LIST_BG, BUTTON_HOVER);
        b->set_font_size(FONT_SMALL);
        b->set_line_height(FONT_SMALL * 1.25f);
        b->set_padding_top(2.0f); b->set_padding_bottom(2.0f); b->set_padding_left(6.0f); b->set_padding_right(6.0f);
        b->set_border_width(1.5f);
        b->set_border_radius(2.0f);
        b->set_text_align(ui::TextAlign::Left);
        b->add_pressed_callback([i]() { t.warp_level = i; update_warp_choice(); });
        t.level_buttons.emplace_back(i, b);
    }
    outline_chosen_level();
}

void update_warp_choice() {
    load_level_names();
    const int i = t.warp_level;
    t.warp_choice->set_text("Level " + std::to_string(i) + ": " + (i >= 0 && i < (int)g_level_names.size() ? g_level_names[i] : std::string("?")) +
                            "   entrance " + std::to_string(t.warp_entrance));
    outline_chosen_level();
    for (int c = 0; c < 3; c++) {
        t.character_buttons[c]->set_background_color(c == t.warp_character ? ON_BG : BUTTON_BG);
        t.character_buttons[c]->get_hover_style()->set_background_color(c == t.warp_character ? ON_HOVER : BUTTON_HOVER);
    }
}

void build_warp_window() {
    ui::Element* body = t.warp.body;
    ui::Element* filter_row = row(body);
    text(filter_row, "Filter");
    ui::TextInput* filter = t.context.create_element<ui::TextInput>(filter_row);
    filter->set_flex_grow(1.0f);
    filter->set_font_size(FONT);
    filter->set_color(TEXT);
    filter->add_text_changed_callback([](const std::string& s) { t.level_filter = s; rebuild_level_list(); });
    // Where the player is now (read on the game thread like the bar's status), and a button to pick it.
    ui::Element* here = row(body);
    add_field(here, ToolField{ ToolField::Note, "", 0, 1, 1, 1, nullptr, nullptr, [](recomp_context*) {
        const int level = (int16_t)rd16(ADDR_currentLevel), entrance = (int16_t)rd16(ADDR_currentEntrance);
        g_here_level = level; g_here_entrance = entrance;
        return "Now in: level " + std::to_string(level) + " " +
               ((level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level] : std::string("?")) +
               "   entrance " + std::to_string(entrance);
    } }, GROUP_BAR);
    g_bound.back().label->set_color(TEXT);
    button(here, "Pick", []() {
        const int level = g_here_level, entrance = g_here_entrance;
        if (level < 0) return;
        t.warp_level = level;
        t.warp_entrance = std::clamp(entrance, 0, 31);
        update_warp_choice();
    });
    t.warp_choice = text(body, "", FONT, ACCENT);
    ui::Element* chars = row(body);
    text(chars, "Character");
    for (int c = 0; c < 3; c++) {
        t.character_buttons[c] = button(chars, character_names[c], [c]() { t.warp_character = c; update_warp_choice(); });
    }
    ui::Element* entrance = row(body);
    text(entrance, "Entrance");
    button(entrance, "-", []() { t.warp_entrance = std::max(0, t.warp_entrance - 1); update_warp_choice(); });
    button(entrance, "+", []() { t.warp_entrance = std::min(31, t.warp_entrance + 1); update_warp_choice(); });
    ui::Element* go = row(body);
    button(go, "Go to level", []() {
        const int level = t.warp_level, character = t.warp_character, entrance = t.warp_entrance;
        run_on_game([level, character, entrance](recomp_context*) {
            g.warp_level = level; g.warp_character = character; g.warp_entrance = entrance; g.pending_warp = true;
        });
    });
    button(go, "Restart current level", []() {
        const int character = t.warp_character, entrance = t.warp_entrance;
        run_on_game([character, entrance](recomp_context*) {
            g.warp_level = (int16_t)rd16(ADDR_currentLevel); g.warp_character = character; g.warp_entrance = entrance; g.pending_warp = true;
        });
    });
    // The levels in a box of their own that scrolls, filling the rest of the window: the controls above stay in view.
    ui::Element* list_box = column(body, 0.0f);
    list_box->set_flex(1.0f, 1.0f, 0.0f);
    list_box->set_min_height(120.0f);
    list_box->set_margin_top(4.0f);
    list_box->set_background_color(LIST_BG);
    list_box->set_border_width(1.0f);
    list_box->set_border_color(BORDER);
    ui::ScrollContainer* scroll = t.context.create_element<ui::ScrollContainer>(list_box, ui::ScrollDirection::Vertical);
    t.level_list = column(scroll, 1.0f);
    t.level_list->set_padding(4.0f);
    text(body, "Names are the developers' internal ones (Forest = Goldwood, CargoShip = S.S. Anubis, AlphaSewer = Sekhmet, ...). "
               "Entries ending in Seq are cutscenes. Use from inside a game.", FONT_SMALL, TEXT_DIM);
    rebuild_level_list();
    update_warp_choice();
}

void build() {
    t.context = ui::create_context();
    t.context.open();
    ui::Element* root = t.context.get_root_element();

    // Behind everything: a click outside the bar and its windows hands the mouse and keyboard to the game.
    ui::Clickable* outside = t.context.create_element<ui::Clickable>(root, false);
    outside->set_position(ui::Position::Absolute);
    outside->set_top(0); outside->set_left(0); outside->set_right(0); outside->set_bottom(0);
    outside->set_background_color({ 0, 0, 0, 0 });
    outside->set_cursor(ui::Cursor::None);
    outside->add_pressed_callback([](float, float) { set_captures(false); });

    t.bar = t.context.create_element<ui::Element>(root);
    t.bar->set_position(ui::Position::Absolute);
    t.bar->set_top(0); t.bar->set_left(0); t.bar->set_right(0);
    t.bar->set_display(ui::Display::Flex);
    t.bar->set_flex_direction(ui::FlexDirection::Row);
    t.bar->set_align_items(ui::AlignItems::Center);
    t.bar->set_gap(2.0f);
    t.bar->set_padding(3.0f);
    t.bar->set_background_color(BAR_BG);
    t.bar->set_border_bottom_width(1.0f);
    t.bar->set_border_bottom_color(BORDER);
    t.menus.reserve(8); // each menu's rows are noted through a pointer into this while it is built

    // Cheats
    ui::Element* cheats = add_menu("Cheats");
    auto flag_field = [](const char* label, bool* option) {
        return ToolField{ ToolField::Toggle, label, 0, 1, 1, 1,
            [option](recomp_context*) { return *option ? 1 : 0; }, [option](recomp_context*, int v) { *option = v != 0; } };
    };
    add_field(cheats, flag_field("Invulnerable (the game's invulnerabilityCheat)", &g.invulnerable), GROUP_BAR);
    add_field(cheats, flag_field("Unlimited ammo (DebugAmmoFlag)", &g.infinite_ammo), GROUP_BAR);
    add_field(cheats, flag_field("All weapons (allweaponsCheat)", &g.all_weapons), GROUP_BAR);
    add_field(cheats, flag_field("All levels open (allLevelCheat)", &g.all_levels), GROUP_BAR);
    add_field(cheats, ToolField{ ToolField::Toggle, "The game's own debug display (debugMenuEnable)", 0, 1, 1, 1,
        [](recomp_context*) { return rd32(ADDR_debugMenuEnable) != 0 ? 1 : 0; },
        [](recomp_context*, int v) { wr32(ADDR_debugMenuEnable, v ? 1u : 0u); } }, GROUP_BAR);
    add_field(cheats, flag_field("Skip the Tribal check (all Tribals count as rescued)", &g.skip_tribal_check), GROUP_BAR);
    text(cheats, "The first four switch the game's own retail cheat flags. The Tribal check makes the game's\n"
                 "\"all Tribals rescued\" test pass (game flag 88) without changing your save.", FONT_SMALL, TEXT_DIM);

    // Tools
    ui::Element* tools = add_menu("Tools");
    button(tools, "Level Warp...", []() { close_menus(); open_window(t.warp, true); });
    button(tools, "Items...", []() { close_menus(); open_window(t.items, true); });
    button(tools, "Weapons...", []() { close_menus(); open_window(t.weapons, true); });
    add_field(tools, ToolField{ ToolField::Toggle, "Fly camera (F7)", 0, 1, 1, 1,
        [](recomp_context*) { return g_fly.active ? 1 : 0; }, [](recomp_context*, int) { g_fly_toggle_request = true; } }, GROUP_BAR);
    auto saved_flag = [](const char* label, bool* option) {
        return ToolField{ ToolField::Toggle, label, 0, 1, 1, 1,
            [option](recomp_context*) { return *option ? 1 : 0; }, [option](recomp_context*, int v) { *option = v != 0; save_options(); } };
    };
    add_field(tools, saved_flag("Freeze while flying", &g.fly_freeze), GROUP_BAR);
    add_field(tools, saved_flag("Hide HUD while flying", &g.fly_hide_hud), GROUP_BAR);
    text(tools, "Fly camera: WASD / left stick move, mouse / right stick look, Space / A up, Ctrl / B down, fire = faster.", FONT_SMALL, TEXT_DIM);

    t.profile_menu = add_menu("Profile");
    t.textures_menu = add_menu("Textures");

    // Game
    ui::Element* game = add_menu("Game");
    button(game, "Back to the title screen (F5)", []() { run_on_game([](recomp_context*) { g_reset_pending = true; }); close_menus_to_bar(); });
    text(game, "Settings: Select (controller) or Esc.   F9: the debug window.   F10: these tools.\n"
               "F7: fly camera.   F5: title screen.   F6: next texture pack.\n"
               "Alt or a click outside the bar and its windows: play with the mouse and keyboard; Alt again: back to the tools.",
         FONT_SMALL, TEXT_DIM);
    note_buttons_of(nullptr);
    wire_bar_nav();
    for (size_t i = 0; i < t.menus.size(); i++) wire_menu_nav(i);

    // Status on the right: input, profile, pack, level.
    ui::Element* spacer = t.context.create_element<ui::Element>(t.bar);
    spacer->set_flex_grow(1.0f);
    t.input_status = text(t.bar, "", FONT_SMALL, ACCENT);
    t.input_status->set_margin_right(12.0f);
    auto status = [](std::function<std::string(recomp_context*)> f) {
        add_field(t.bar, ToolField{ ToolField::Note, "", 0, 1, 1, 1, nullptr, nullptr, std::move(f) }, GROUP_BAR);
        g_bound.back().label->set_color(TEXT);
        g_bound.back().label->set_margin_right(14.0f);
    };
    status([](recomp_context*) { return "Profile: " + (g_active_profile.empty() ? std::string("(none)") : g_active_profile); });
    status([](recomp_context*) {
        std::string current;
        { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
        return "Pack: " + pack_display_name(current);
    });
    status([](recomp_context*) {
        const int level = (int16_t)rd16(ADDR_currentLevel);
        return "Level " + std::to_string(level) + " " + ((level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level] : std::string("?"));
    });

    // Windows
    make_window(t.warp, "Level Warp", 1.0f, 8.0f, 34.0f, -1, false);
    build_warp_window();
    make_window(t.items, "Items", 3.0f, 10.0f, 44.0f, GROUP_ITEMS);
    {
        ui::Element* chars = row(t.items.body);
        text(chars, "Character");
        for (int c = 0; c < 3; c++) button(chars, character_names[c], [c]() { run_on_game([c](recomp_context*) { g.edit_character = c; }); });
        add_fields(t.items.body, item_fields(), GROUP_ITEMS);
    }
    make_window(t.weapons, "Weapons", 52.0f, 20.0f, 44.0f, GROUP_WEAPONS); // below the corner notes (FPS, level)
    add_fields(t.weapons.body, weapon_fields(), GROUP_WEAPONS);

    set_captures(true);
    t.context.close();
    g_values.assign(g_bound.size(), Value{});
    g_ready = true;
}

// Puts the values the game thread read on screen (context open).
void refresh_values() {
    std::vector<Value> values;
    {
        std::lock_guard lock(g_values_mutex);
        if (!g_values_new) return;
        g_values_new = false;
        values = g_values;
    }
    for (size_t i = 0; i < g_bound.size() && i < values.size(); i++) {
        Bound& b = g_bound[i];
        const Value& v = values[i];
        if (!v.valid) continue;
        if (b.field.text && v.text != b.shown_text) { b.shown_text = v.text; b.label->set_text(v.text); }
        if (v.v != b.shown) {
            if (b.toggle != nullptr) { b.shown = v.v; show_toggle_state(b.toggle, v.v != 0); }
            else if (b.value != nullptr) { b.shown = v.v; b.value->set_text(std::to_string(v.v)); }
        }
    }
}
} // namespace

// Game thread, every tick (jfg_debug_frame).
void tools_game_tick(recomp_context* ctx) {
    std::vector<std::function<void(recomp_context*)>> cmds;
    {
        std::lock_guard lock(g_cmd_mutex);
        cmds.swap(g_cmds);
    }
    for (auto& c : cmds) c(ctx);
    static uint32_t tick = 0;
    if (!g_ready || !g_shown || (tick++ % 10 != 0 && cmds.empty())) return;
    load_level_names();
    const uint32_t groups = g_groups;
    std::vector<Value> values(g_bound.size());
    for (size_t i = 0; i < g_bound.size(); i++) {
        const Bound& b = g_bound[i];
        if (b.group < 0 || !(groups & (1u << b.group))) continue;
        values[i].valid = true;
        if (b.field.get) values[i].v = b.field.get(ctx);
        if (b.field.text) values[i].text = b.field.text(ctx);
    }
    std::lock_guard lock(g_values_mutex);
    g_values = std::move(values);
    g_values_new = true;
}

} // namespace jfg

// F10, Alt, and End and F8 for frame step (jfg_frame_rate.cpp), from the SDL event filter (src/debug_ui/backend.cpp; any thread). Alt counts when it is pressed and
// released with no other key in between, like a Windows menu bar.
extern "C" void jfg_tools_key(int scancode, int down, int repeat) {
    static bool alt_alone = false;
    const bool alt = scancode == SDL_SCANCODE_LALT || scancode == SDL_SCANCODE_RALT;
    if (down && !repeat) {
        alt_alone = alt;
        if (scancode == SDL_SCANCODE_F10) jfg::g_toggle_request = true;
        if (scancode == SDL_SCANCODE_END) jfg::frame_step_key(false);
    }
    if (down && scancode == SDL_SCANCODE_F8) jfg::frame_step_key(true); // repeats while held
    else if (!down && alt) {
        if (alt_alone) jfg::g_alt_request = true;
        alt_alone = false;
    }
}

// UI thread, every UI frame (src/ui/ui_state.cpp, draw_hook): builds the tools the first time F10 is pressed, shows and
// hides them, and puts the game thread's values on screen.
extern "C" void jfg_tools_ui_frame(void) {
    using namespace jfg;
    // Tests: JFG_DEBUG_TOOLS="secs[,warp|items|weapons|cheats|tools|game|menu]" opens the tools (and that window or menu)
    // once; menu opens them as the Select menu's tools button does.
    static const char* const debug = std::getenv("JFG_DEBUG_TOOLS");
    static const auto debug_start = std::chrono::steady_clock::now();
    static int debug_step = 0;
    if (debug != nullptr && debug_step < 2 && std::chrono::steady_clock::now() - debug_start > std::chrono::seconds(std::atoi(debug))) {
        const char* what = std::strchr(debug, ',');
        if (what != nullptr && std::strcmp(what, ",menu") == 0) { g_menu_open_request = true; debug_step = 2; }
        else if (debug_step++ == 0) g_toggle_request = true;
        else if (what != nullptr && g_ready) {
            const std::string w = what + 1;
            t.context.open();
            if (w == "warp") open_window(t.warp, true);
            else if (w == "items") open_window(t.items, true);
            else if (w == "weapons") open_window(t.weapons, true);
            else {
                static const char* const names[] = { "cheats", "tools", "profile", "textures", "game" };
                for (size_t i = 0; i < std::size(names) && i < t.menus.size(); i++)
                    if (w == names[i]) toggle_menu(i);
            }
            t.context.close();
        }
    }
    const bool toggle_request = g_toggle_request.exchange(false);
    const bool alt_request = g_alt_request.exchange(false);
    const bool menu_open_request = g_menu_open_request.exchange(false);
    if ((toggle_request || menu_open_request) && !g_ready) build();
    if (!g_ready) return;
    if (toggle_request && g_shown) { recompui::hide_context(t.context); g_shown = false; }
    else if ((toggle_request || menu_open_request) && !g_shown) {
        recompui::show_context(t.context, "");
        g_shown = true;
        t.context.open();
        set_captures(true);
        t.context.close();
    }
    if (menu_open_request) {
        // From the Select menu's tools button: the tools take the input back (after Start handed it to the game) and the
        // controller goes to the Cheats menu, open.
        t.context.open();
        set_captures(true);
        if (recompui::get_cont_active() && !t.menus.empty()) {
            open_menu(0);
            t.opened_by_focus = true;
            focus_bar_button(0);
        }
        t.context.close();
    }
    if (!g_shown) { g_toolbar_height = 0.0f; return; }
    t.context.open();
    if (alt_request) set_captures(!t.captures);
    static bool pad_shown = false; // the input status follows the controller / mouse and keys
    if (pad_shown != recompui::get_cont_active()) { pad_shown = !pad_shown; show_input_status(); }
    place_open_menu();
    refresh_values();
    g_toolbar_height = t.bar->get_client_height();
    t.context.close();
}

// The Select menu's tools button (src/ui/ui_config.cpp, UI thread): opens the tools next frame. With the mouse and keys
// it shows and hides them, as F10 does; with the controller it always opens them (it is how the controller gets them
// back after Start handed it to the game).
extern "C" void jfg_tools_open_from_menu(void) {
    if (jfg::g_shown && !recompui::get_cont_active()) jfg::g_toggle_request = true;
    else jfg::g_menu_open_request = true;
}

// The controller in the tools (src/ui/ui_state.cpp, UI thread, each button press and release while a menu takes the
// input; bound: the button is bound to a menu input, which keeps it). Returns 1 when the button was used here.
// LB / RB: the menu before / after, open, on its first row. B: closes the open menu, else the window opened last, else
// the tools (as F10). Start: hands the controller to the game, the bar stays up. B and Start act on release, so the game
// behind never sees them held.
extern "C" int jfg_tools_controller_button(int button, int down, int bound) {
    using namespace jfg;
    static bool b_held = false, start_held = false;
    const bool ours = g_ready && g_shown && t.captures && !recompui::is_prompt_open() &&
                      !recompui::is_context_shown(recompui::get_config_context_id()) &&
                      !recompui::is_context_shown(recompui::get_config_sub_menu_context_id()) &&
                      !recompui::is_context_shown(recompui::get_launcher_context_id());
    if (!ours) {
        b_held = start_held = false;
        return 0;
    }
    if (!down) {
        const bool was_held = button == SDL_CONTROLLER_BUTTON_B ? b_held : button == SDL_CONTROLLER_BUTTON_START ? start_held : false;
        if (button == SDL_CONTROLLER_BUTTON_B) b_held = false;
        if (button == SDL_CONTROLLER_BUTTON_START) start_held = false;
        if (!was_held) return 0;
        const bool opened = t.context.open_if_not_already();
        if (button == SDL_CONTROLLER_BUTTON_START) set_captures(false);
        else if (const int i = open_menu_index(); i >= 0) {
            close_menus();
            focus_bar_button(i);
        }
        else if (!t.window_order.empty()) open_window(*t.window_order.back(), false);
        else g_toggle_request = true;
        if (opened) t.context.close();
        return 1;
    }
    if (button == SDL_CONTROLLER_BUTTON_LEFTSHOULDER || button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) {
        const int n = (int)t.menus.size();
        if (n == 0) return 1;
        const int cur = open_menu_index();
        const int dir = button == SDL_CONTROLLER_BUTTON_RIGHTSHOULDER ? 1 : -1;
        const int next = cur < 0 ? (dir > 0 ? 0 : n - 1) : ((cur + dir) % n + n) % n;
        const bool opened = t.context.open_if_not_already();
        open_menu(next);
        if (ui::Button* first = first_item(t.menus[next])) first->focus();
        else focus_bar_button(next);
        if (opened) t.context.close();
        return 1;
    }
    if (bound) return 0;
    if (button == SDL_CONTROLLER_BUTTON_B) { b_held = true; return 1; }
    if (button == SDL_CONTROLLER_BUTTON_START) { start_held = true; return 1; }
    return 0;
}
