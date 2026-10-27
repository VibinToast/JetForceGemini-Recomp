// JFG PORT: the first-run language question. Until a language is saved (jfg::locale::choice_saved: a first start, or
// an update from a version without the question) the launcher, or the game when the launcher is skipped, opens on a
// list of the languages there are: English and every locales/<id>.json, each tagged official or unofficial
// (jfg::locale::official). Picking one saves it as the Language list does; it can be changed later on the launcher's
// Play page or in the settings. Its own texts are in the language the player points at (focus or mouse), from that
// locale's "ui" (jfg::locale::ui_in), not the one saved, so a player who reads only that language understands it. With
// English alone there is nothing to ask, so nothing is saved and the question waits
// for a locale file. JFG_DEBUG_LANGUAGE_PROMPT=1 asks even when a language is saved (window captures).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "ui/recomp_ui.h"
#include "ui_mod_details_panel.h"
#include "elements/ui_button.h"
#include "elements/ui_element.h"
#include "elements/ui_label.h"
#include "../jfg/jfg_locale.h"

namespace {

namespace ui = recompui;
namespace modui = recompui::modui;

constexpr char TITLE[] = "Choose Your Language";
constexpr char NOTE[] = "You can change it later on the launcher's Play page or in the settings.";
constexpr char LEGEND[] = "Unofficial translations come from players or are machine-assisted, and may have mistakes.";

struct {
    bool decided = false; // asked this run, or nothing to ask
    ui::ContextId context = ui::ContextId::null();
    ui::Button* first = nullptr;
    ui::Label* title = nullptr;
    ui::Label* note = nullptr;
    ui::Label* legend = nullptr;
    std::vector<std::pair<ui::Label*, bool>> tags; // each row's tag, official or not
    std::string shown;                             // the language the texts are in
} g_prompt;

// The question's texts in locale `id`, kept from the menus' own translation.
void show_in(const std::string& id) {
    if (id == g_prompt.shown || g_prompt.title == nullptr) return;
    g_prompt.shown = id;
    jfg::locale::hold_translation(true);
    g_prompt.title->set_text(jfg::locale::ui_in(id, TITLE));
    g_prompt.note->set_text(jfg::locale::ui_in(id, NOTE));
    if (g_prompt.legend != nullptr) g_prompt.legend->set_text(jfg::locale::ui_in(id, LEGEND));
    const std::string official = jfg::locale::ui_in(id, "OFFICIAL"), unofficial = jfg::locale::ui_in(id, "UNOFFICIAL");
    for (auto& [tag, is_official] : g_prompt.tags) tag->set_text(is_official ? official : unofficial);
    jfg::locale::hold_translation(false);
}

// A language's row: focusing or pointing at it shows the question in that language.
class LanguageButton : public ui::Button {
public:
    LanguageButton(ui::Element* parent, const std::string& label, std::string id)
        : ui::Button(parent, label, ui::ButtonStyle::Secondary), id(std::move(id)) {}
protected:
    void process_event(const ui::Event& e) override {
        ui::Button::process_event(e);
        if ((e.type == ui::EventType::Focus && std::get<ui::EventFocus>(e.variant).active) ||
            (e.type == ui::EventType::Hover && std::get<ui::EventHover>(e.variant).active)) show_in(id);
    }
private:
    std::string id;
};

void style_row_button(ui::Button* b) {
    b->set_flex_grow(1.0f);
    b->set_padding_top(10.0f); b->set_padding_bottom(8.0f); b->set_padding_left(18.0f); b->set_padding_right(18.0f);
    b->set_border_radius(0.0f);
    b->set_border_width(1.0f);
    b->set_border_color(modui::edge);
    b->set_background_color(modui::panel);
    b->set_font_family("JetBrains Mono");
    b->set_font_size(30.0f);
    b->set_line_height(38.0f);
    b->set_letter_spacing(0.0f);
    b->set_font_weight(700);
    b->set_color(modui::text);
    b->set_text_align(ui::TextAlign::Left);
    b->get_hover_style()->set_background_color(modui::hover);
    b->get_hover_style()->set_border_color(modui::cyan);
    b->get_focus_style()->set_background_color(modui::hover);
    b->get_focus_style()->set_border_color(modui::cyan);
}

void pick(const std::string& id) {
    jfg::locale::choose(id);
    ui::hide_context(g_prompt.context);
}

void build(const std::vector<std::pair<std::string, std::string>>& languages) {
    g_prompt.context = ui::create_context();
    ui::ContextId& context = g_prompt.context;
    context.open();
    jfg::locale::hold_translation(true); // English to start with (the first row), whatever is saved
    ui::Element* root = context.get_root_element();

    ui::Element* shade = context.create_element<ui::Element>(root);
    shade->set_position(ui::Position::Absolute);
    shade->set_top(0); shade->set_left(0); shade->set_right(0); shade->set_bottom(0);
    shade->set_background_color({ 2, 6, 16, 235 });
    shade->set_display(ui::Display::Flex);
    shade->set_justify_content(ui::JustifyContent::Center);
    shade->set_align_items(ui::AlignItems::Center);

    ui::Element* panel = context.create_element<ui::Element>(shade);
    panel->set_display(ui::Display::Flex);
    panel->set_flex_direction(ui::FlexDirection::Column);
    panel->set_width(760.0f);
    panel->set_max_height(80.0f, ui::Unit::Percent); // clear of the game's "Settings: Select / Esc" line at the bottom
    panel->set_padding(32.0f);
    panel->set_gap(14.0f);
    panel->set_background_color(modui::navy);
    panel->set_border_width(2.0f);
    panel->set_border_color(modui::edge);

    ui::Label* title = context.create_element<ui::Label>(panel, TITLE, ui::LabelStyle::Large);
    g_prompt.title = title;
    modui::set_text(title, 1.0f, 44.0f, 52.0f, 700, modui::cyan, 1.0f);
    title->set_font_family("JFG Black");
    ui::Label* note = context.create_element<ui::Label>(panel, NOTE, ui::LabelStyle::Small);
    g_prompt.note = note;
    modui::set_text(note, 1.0f, 24.0f, 32.0f, 400, modui::dim);
    note->set_font_family("JetBrains Mono");

    ui::Element* list = context.create_element<ui::Element>(panel);
    list->set_display(ui::Display::Flex);
    list->set_flex_direction(ui::FlexDirection::Column);
    list->set_gap(8.0f);
    list->set_overflow_y(ui::Overflow::Auto);
    list->set_flex_shrink(1.0f);
    list->set_padding_right(6.0f);

    bool any_unofficial = false;
    ui::Button* prev = nullptr;
    for (const auto& [id, label] : languages) {
        ui::Element* row = context.create_element<ui::Element>(list);
        row->set_display(ui::Display::Flex);
        row->set_flex_direction(ui::FlexDirection::Row);
        row->set_align_items(ui::AlignItems::Center);
        row->set_gap(16.0f);
        ui::Button* b = context.create_element<LanguageButton>(row, label, id);
        style_row_button(b);
        const std::string pick_id = id;
        b->add_pressed_callback([pick_id]() { pick(pick_id); });
        const bool official = jfg::locale::official(id);
        any_unofficial |= !official;
        ui::Label* tag = context.create_element<ui::Label>(row, official ? "OFFICIAL" : "UNOFFICIAL", ui::LabelStyle::Small);
        g_prompt.tags.emplace_back(tag, official);
        modui::set_text(tag, 1.0f, 20.0f, 26.0f, 700, official ? modui::cyan : modui::gold, 1.5f);
        tag->set_font_family("JetBrains Mono");
        tag->set_width(170.0f);
        tag->set_text_align(ui::TextAlign::Right);
        if (prev != nullptr) {
            prev->set_nav(ui::NavDirection::Down, b);
            b->set_nav(ui::NavDirection::Up, prev);
        } else {
            g_prompt.first = b;
            b->set_nav_none(ui::NavDirection::Up);
        }
        b->set_nav_none(ui::NavDirection::Left);
        b->set_nav_none(ui::NavDirection::Right);
        prev = b;
    }
    if (prev != nullptr) prev->set_nav_none(ui::NavDirection::Down);
    if (any_unofficial) {
        ui::Label* legend = context.create_element<ui::Label>(panel, LEGEND, ui::LabelStyle::Small);
        g_prompt.legend = legend;
        modui::set_text(legend, 1.0f, 22.0f, 30.0f, 400, modui::dim);
        legend->set_font_family("JetBrains Mono");
    }

    jfg::locale::hold_translation(false);
    g_prompt.shown = "";
    context.set_captures_input(true);
    context.set_captures_mouse(true);
    context.close();
}

} // namespace

// Every UI frame (src/ui/ui_state.cpp): opens the question once, at the first frame it is needed.
void jfg_language_prompt_frame() {
    if (g_prompt.decided) {
        // The launcher is shown after the first frames; keep the question over it while it is open.
        if (g_prompt.context != ui::ContextId::null() && ui::is_context_shown(g_prompt.context)) g_prompt.context.get_document()->PullToFront();
        return;
    }
    g_prompt.decided = true;
    static const bool forced = std::getenv("JFG_DEBUG_LANGUAGE_PROMPT") != nullptr;
    if (!forced && jfg::locale::choice_saved()) return;
    std::vector<std::pair<std::string, std::string>> languages = jfg::locale::list();
    // The official ones first (English leads), then the unofficial, each in the Language list's order.
    std::stable_partition(languages.begin(), languages.end(), [](const auto& l) { return jfg::locale::official(l.first); });
    std::fprintf(stderr, "[jfg] language question: %zu languages%s\n", languages.size(), languages.size() < 2 ? ", nothing to ask" : "");
    if (languages.size() < 2) return;
    build(languages);
    ui::show_context(g_prompt.context, "");
    g_prompt.context.open();
    if (g_prompt.first != nullptr) g_prompt.first->focus();
    g_prompt.context.close();
}
