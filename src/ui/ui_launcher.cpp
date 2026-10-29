#include "config/config.hpp"
#include "runtime/support.hpp"
#include "renderer/renderer.hpp"

#include "ui/recomp_ui.h"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#include "SDL.h"
#include <filesystem>
#include <chrono>
#include <fstream>
#include <iterator>
#include <map>

static std::string version_string;
#include "jfg/jfg_update_check.h"
#include "jfg/jfg_pack_install.h"
#include "jfg/jfg_mod_store.h" // JFG PORT: the Get Mods page
#include "jfg/jfg_locale.h" // JFG PORT: texts set from here in the chosen language (jfg::locale::ui)
#include "input/input.hpp" // JFG PORT: get_scanned_input_index (Esc on the bindings page)
// A newer release exists (src/jfg/jfg_update_check.cpp): a banner on the Play page and a warning triangle next to the
// version open the dialog with what is new and a confirmation, and the update is then downloaded and installed. The dialog
// does not open by itself. bin\jfg_launcher.txt can turn the check off (check_for_updates=0).
static bool update_available = false, update_dialog = false, update_can_install = false, update_restarting = false;
static std::string update_version, update_changelog, update_status, update_unavailable, changelog_text;
static bool changelog_dialog = false;
static int update_stage = 0, update_percent = 0;
// Dialogs take the focus when they open (controller players have no other way in) and give it back when they close.
static std::string pending_focus_id;  // focused a couple of UI frames later, once the data bindings have shown the element
static std::string return_focus_id;   // focused when the open dialog closes

Rml::DataModelHandle model_handle;
bool rom_valid = false;

// JFG PORT: VibinToast-style launcher (assets/launcher.rml). The sidebar picks a page; the settings pages are the
// Select / Esc menu's own templates and data models (src/ui/ui_config.cpp), so both always show the same settings.
static Rml::String launcher_page = "play";
static int toast_frame = 0;           // frame of the VibinToast icon on the About page (assets/jfg_launcher/vibintoast.rcss)
static bool logo_available = false;   // assets/jfg_launcher/logo.png exists: shown instead of the text wordmark
static constexpr int TOAST_FRAMES = 180, TOAST_FPS = 30;
void jfg_launcher_commit_settings();  // ui_config.cpp: applies pending graphics changes and saves the settings
void jfg_config_menu_refresh();       // ui_config.cpp: refreshes the port's tabs (profiles, texture packs)
void jfg_config_clear_option_focus(); // ui_config.cpp: no option's description shown
void jfg_controls_page_entered();     // ui_config.cpp: the Controls page's Players row and Mouse & Keyboard section
void jfg_open_url_prompt(const std::string& url, const std::string& return_id); // ui_config.cpp: asks, then opens it

struct LauncherPageInfo { const char* id; const char* title; const char* subtitle; const char* nav_id; };
static const LauncherPageInfo LAUNCHER_PAGES[] = {
    { "play", "Play", "Load your ROM, then jump in.", "nav_play" },
    { "profiles", "Profiles", "Whole sets of settings: the original game, Modern, Modern and Balanced, Extreme or your own.", "tab_jfg_profiles" },
    { "general", "General", "Volume, the settings menu, what is shown on screen and the fly camera.", "tab_jfg_general" },
    { "gameplay", "Gameplay", "Saving, gore, player tweaks and movement.", "tab_jfg_gameplay" },
    { "controls", "Controls", "Players, quick setup, camera and aiming, sticks and rumble, mouse and keyboard, and your buttons.", "tab_jfg_controls" },
    // Not in the sidebar: Customize Buttons on the Controls page opens it (jfg_launcher_show_bindings), and its sidebar
    // button is Controls'.
    { "bindings", "Customize Buttons", "Button bindings for the controller and the keyboard.", "tab_jfg_controls" },
    { "graphics", "Graphics", "Window, resolution, frame rate, widescreen and effects.", "tab_jfg_graphics" },
    { "textures", "Texture Packs", "Pick a texture pack from the texture_packs folder, and dump the game's textures to make your own.", "tab_jfg_textures" },
    { "mods", "Mods", "Turn mods on and off, set their order and options, and install new ones.", "tab_mods" },
    { "store", "Get Mods", "Mods from VibinToast's collections: install, update and remove them.", "tab_store" },
    { "about", "About", "Jet Force Gemini: Recompiled.", "nav_about" },
};
static const LauncherPageInfo& launcher_page_info() {
    for (const LauncherPageInfo& p : LAUNCHER_PAGES) if (launcher_page == p.id) return p;
    return LAUNCHER_PAGES[0];
}

// ---- dialogs and the focus (keyboard / controller) -----------------------------------------------------------------
extern recompui::ContextId launcher_context;
static int pending_focus_frames = 0;

static Rml::ElementDocument* launcher_document() {
    return launcher_context == recompui::ContextId::null() ? nullptr : launcher_context.get_document();
}

// Focuses `id` two UI frames from now: an element a data binding has just shown only exists for the focus after that.
static void focus_soon(const std::string& id) {
    pending_focus_id = id;
    pending_focus_frames = 2;
}

// Remembers what had the focus, to give it back when the dialog that is opening closes.
static void remember_focus(Rml::ElementDocument* doc) {
    return_focus_id.clear();
    if (doc != nullptr) {
        if (Rml::Element* focused = doc->GetFocusLeafNode(); focused != nullptr && focused != doc) return_focus_id = focused->GetId();
    }
    if (return_focus_id.empty()) return_focus_id = launcher_page_info().nav_id;
}

static void open_update_dialog(Rml::ElementDocument* doc) {
    if (update_dialog) return;
    jfg_update_reset();
    update_stage = 0;
    update_status.clear();
    remember_focus(doc);
    update_dialog = true;
    model_handle.DirtyVariable("update_dialog");
    model_handle.DirtyVariable("update_stage");
    model_handle.DirtyVariable("update_status");
    focus_soon("rl_update_go");
}

// "Later" (also Esc / the controller's Back button). Not while the update downloads or installs.
static void close_update_dialog() {
    if (!update_dialog || update_stage == 1 || update_stage == 3) return;
    update_dialog = false;
    jfg_update_reset();
    model_handle.DirtyVariable("update_dialog");
    focus_soon(return_focus_id);
}

static void open_changelog_dialog(Rml::ElementDocument* doc) {
    // The notes of the release in testing, or else the commits since the last release (from GitHub, when they have
    // arrived), above the changelog shipped with the game.
    changelog_text = jfg_changelog_rml(jfg_changelog_commits() + jfg_changelog_text(), version_string);
    model_handle.DirtyVariable("changelog_text");
    remember_focus(doc);
    changelog_dialog = true;
    model_handle.DirtyVariable("changelog_dialog");
    focus_soon("rl_changelog_close");
}

static void close_changelog_dialog() {
    if (!changelog_dialog) return;
    changelog_dialog = false;
    model_handle.DirtyVariable("changelog_dialog");
    focus_soon(return_focus_id);
}

static Rml::ElementDocument* event_document(Rml::Event& event) {
    Rml::Element* target = event.GetTargetElement();
    return target != nullptr ? target->GetOwnerDocument() : nullptr;
}

// Images the launcher shows, registered under the names its RML uses ("/jfg_vibintoast" -> "jfg_vibintoast").
static bool queue_launcher_image(const char* name, const char* file) {
    std::ifstream in(dino::runtime::get_asset_path(file), std::ios::binary);
    if (!in) return false;
    std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    recompui::queue_image_from_bytes_file(name, bytes);
    return true;
}

// ---- the language (the Play page's top right; src/jfg/jfg_locale.h) -----------------------------------------------------
// A drop-down of English and every locales/*.json. Picking one switches the launcher at once (its text nodes in
// ui_state.cpp, the texts set from here by dirtying the model) and the game starts in it.
struct LauncherLocale { Rml::String id, name; bool selected = false; };
static std::vector<LauncherLocale> launcher_locales;
static Rml::String launcher_locale_name;
static bool lang_open = false;
static std::string shown_locale = "\x01"; // the choice the launcher last showed (none yet)

static void refresh_locales() {
    const std::string chosen = jfg::locale::chosen();
    launcher_locales.clear();
    for (const auto& [id, name] : jfg::locale::list()) {
        launcher_locales.push_back({ id, name, id == chosen });
        if (id == chosen) launcher_locale_name = name;
    }
}

// ---- the official HD texture pack (Texture Packs page; src/jfg/jfg_pack_install.cpp) -------------------------------------
struct OfficialPackRow { Rml::String variant, size_text, action; bool installed = false; };
static std::vector<OfficialPackRow> official_rows;    // one button per variant in the latest release
static Rml::String official_status, official_text;    // "Latest version: 1.1.0"; the download's progress / result
static Rml::String official_result;                   // how the last download ended (shown while none is running)
static bool official_busy = false;
static bool official_shown = true;                    // the General page's "Official HD Texture Pack" (show_official_pack)
static int official_percent = 0, official_serial = -1;
static bool official_update = false;                  // an installed official pack is older: the Play page's bar
static Rml::String official_update_title, official_update_text, official_update_variant;

static Rml::String official_size_text(uint64_t bytes) {
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.1f GB", bytes / 1073741824.0);
    else std::snprintf(text, sizeof(text), "%.0f MB", bytes / 1048576.0);
    return text;
}

// Shows the state again. scan: also which packs are installed - when the page opens or a download or the check ends,
// not every time (it reads texture_packs).
static void refresh_official(bool scan) {
    if (!model_handle) return;
    const JfgOfficialState state = jfg_official_state(scan);
    if (state.busy != official_busy) {
        official_busy = state.busy;
        model_handle.DirtyVariable("official_busy");
        focus_soon(official_busy ? "rl_official_cancel" : "rl_official_0");
    }
    if (state.percent != official_percent) { official_percent = state.percent; model_handle.DirtyVariable("official_percent"); }
    if (state.text != official_text) { official_text = state.text; model_handle.DirtyVariable("official_text"); }
    const Rml::String result = state.busy ? Rml::String() : state.text;
    if (result != official_result) { official_result = result; model_handle.DirtyVariable("official_result"); }
    // The Play page's bar: the first installed official pack with a newer version out (looked up when scanning, its
    // text made on every refresh so it follows a change of language).
    static std::vector<JfgOfficialUpdate> updates;
    if (scan) updates = jfg_launcher_show_official_pack() ? jfg_official_updates() : std::vector<JfgOfficialUpdate>{};
    const bool update = !updates.empty();
    Rml::String title, text, variant;
    if (update) {
        title = jfg::locale::ui("HD texture pack update:") + " " + updates[0].latest;
        text = "The " + updates[0].variant + " pack you have is " + updates[0].installed + ". Open Texture Packs to update it.";
        variant = updates[0].variant;
    }
    if (update != official_update) { official_update = update; model_handle.DirtyVariable("official_update"); }
    if (title != official_update_title) { official_update_title = title; model_handle.DirtyVariable("official_update_title"); }
    if (text != official_update_text) { official_update_text = text; model_handle.DirtyVariable("official_update_text"); }
    official_update_variant = variant;
    if (!scan) return;
    if (jfg_launcher_show_official_pack() != official_shown) {
        official_shown = jfg_launcher_show_official_pack();
        model_handle.DirtyVariable("official_shown");
    }
    official_rows.clear();
    for (const JfgOfficialPack& pack : state.packs) {
        official_rows.push_back({ pack.variant, official_size_text(pack.size), jfg::locale::ui(pack.action), pack.action == "Installed" });
    }
    model_handle.DirtyVariable("official_rows");
    Rml::String status;
    if (!jfg_launcher_check_updates()) status = jfg::locale::ui("Turn on Check for Updates (General page) to download it here.");
    else if (state.stage <= 1) status = jfg::locale::ui("Looking for the latest version...");
    else if (state.stage == 3) status = jfg::locale::ui("Could not reach GitHub to look for it.");
    else if (state.packs.empty()) status = jfg::locale::ui("The latest release has nothing to download yet.");
    else status = jfg::locale::ui("Latest version:") + " " + state.latest;
    if (status != official_status) { official_status = status; model_handle.DirtyVariable("official_status"); }
}

// ---- the mod store (Get Mods page; src/jfg/jfg_mod_store.cpp) ---------------------------------------------------------
// The sources' catalogs merged into one list of cards, a filter (all / installed / updates), the sources' switches, and
// the chosen mod's details beside the list. Pictures are queued as "jfg_store_<id>" once each.
struct StoreRow {
    Rml::String id, name, short_text, version, source_name, tags, size_text, thumb, status, action;
    bool installed = false, update = false, managed = false, blocked = false, busy = false, selected = false;
};
struct StoreSourceRow { Rml::String id, name, status; bool enabled = true, official = false; };
static std::vector<StoreRow> store_rows;
static std::vector<StoreSourceRow> store_sources;
static Rml::String store_filter = "all", store_selected;
static Rml::String store_text, store_sel_name, store_sel_desc, store_sel_meta, store_sel_deps, store_sel_note, store_sel_thumb;
static Rml::String store_sel_action;
// For the controller: the selected mod's card ("#st_mod_<n>", or the first card when the filter hides it) and what is
// right of the cards (Install / Update, else Remove, else nothing).
static Rml::String store_sel_card = "#st_mod_0", store_right = "none";
static bool store_busy = false, store_preview = false, store_loaded = false, store_sel_can_act = false, store_sel_can_remove = false;
static bool store_sel_blocked = false;
static int store_percent = 0, store_serial = -1, store_count_all = 0, store_count_installed = 0, store_count_updates = 0;
static std::map<std::string, bool> store_images; // queued already

static void refresh_store_rows() {
    if (!model_handle) return;
    const JfgStoreState state = jfg_store_state();
    store_busy = state.busy;
    store_percent = state.percent;
    store_text = state.text;
    store_preview = state.preview;
    const std::vector<JfgStoreMod> mods = jfg_store_mods();
    store_rows.clear();
    store_sel_card = "#st_mod_0";
    store_count_all = (int)mods.size();
    store_count_installed = store_count_updates = 0;
    const JfgStoreMod* sel = nullptr;
    for (const JfgStoreMod& m : mods) {
        store_count_installed += m.installed ? 1 : 0;
        store_count_updates += m.update ? 1 : 0;
        if (store_selected.empty()) store_selected = m.id;
        if (m.id == store_selected) sel = &m;
        if (store_filter == "installed" && !m.installed) continue;
        if (store_filter == "updates" && !m.update) continue;
        StoreRow r;
        r.id = m.id; r.name = m.name; r.short_text = m.short_text; r.version = m.version;
        r.source_name = m.source_name; r.tags = m.tags; r.size_text = m.size_text;
        r.installed = m.installed; r.update = m.update; r.managed = m.managed; r.blocked = m.blocked;
        r.busy = state.busy && state.busy_id == m.id;
        r.selected = m.id == store_selected;
        if (!m.thumb.empty()) {
            const std::string image = "jfg_store_" + m.id;
            if (!store_images[image] && recompui::can_queue_images()) {
                std::ifstream in(std::filesystem::u8path(m.thumb), std::ios::binary);
                std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                if (!bytes.empty()) { recompui::queue_image_from_bytes_file(image, bytes); store_images[image] = true; }
            }
            if (store_images[image]) r.thumb = "/" + image;
        }
        if (m.blocked) r.status = m.blocked_reason;
        else if (m.update) r.status = "Update: " + m.installed_version + " > " + m.version;
        else if (m.installed) r.status = "Installed " + m.installed_version;
        else r.status = "Version " + m.version + "  -  " + m.size_text;
        r.action = m.blocked ? "Unavailable" : m.update ? "Update" : m.installed ? "Installed" : "Install";
        if (r.selected) store_sel_card = "#st_mod_" + std::to_string(store_rows.size());
        store_rows.push_back(r);
    }
    if (sel != nullptr) {
        store_sel_name = sel->name;
        store_sel_desc = sel->description.empty() ? sel->short_text : sel->description;
        store_sel_meta = "Version " + sel->version + "  -  " + sel->size_text + "  -  by " + (sel->authors.empty() ? std::string("?") : sel->authors) +
                         "  -  " + sel->source_name;
        std::string deps;
        for (const std::string& d : sel->dependencies) deps += (deps.empty() ? "" : ", ") + d;
        store_sel_deps = deps.empty() ? "" : "Needs: " + deps + " (installed with it)";
        std::string note;
        if (sel->installed && !sel->managed) note = "You added this mod yourself: the store won't remove it, but it can update it.";
        else if (sel->installed) note = "Installed " + sel->installed_version + " from " + sel->source_name + ".";
        if (!sel->shadowed.empty()) note += (note.empty() ? "" : " ") + std::string("Also in ") + sel->shadowed + ": this one is used.";
        store_sel_note = note;
        store_sel_thumb = store_images["jfg_store_" + sel->id] ? "/jfg_store_" + sel->id : "";
        store_sel_blocked = sel->blocked;
        store_sel_can_act = !sel->blocked && (!sel->installed || sel->update);
        store_sel_action = sel->blocked ? sel->blocked_reason : sel->update ? "Update to " + sel->version : sel->installed ? "Installed" : "Install";
        store_sel_can_remove = sel->installed && sel->managed;
    }
    store_right = store_busy ? "none" : store_sel_can_act ? "#st_act" : store_sel_can_remove ? "#st_remove" : "none";
    store_sources.clear();
    for (const JfgStoreSource& src : jfg_store_sources()) {
        StoreSourceRow r;
        r.id = src.id; r.name = src.name; r.enabled = src.enabled; r.official = src.official;
        r.status = !src.enabled ? "Off" : src.loaded ? std::to_string(src.mods) + " mods" : src.error;
        store_sources.push_back(r);
    }
    for (const char* v : { "store_rows", "store_sources", "store_busy", "store_percent", "store_text", "store_preview",
                           "store_filter", "store_sel_name", "store_sel_desc", "store_sel_meta", "store_sel_deps", "store_sel_note",
                           "store_sel_thumb", "store_sel_action", "store_sel_can_act", "store_sel_can_remove", "store_sel_blocked",
                           "store_count_all", "store_count_installed", "store_count_updates", "store_selected", "store_sel_card",
                           "store_right" })
        model_handle.DirtyVariable(v);
}

static void set_launcher_page(const std::string& page) {
    if (launcher_page == page) return;
    jfg_launcher_commit_settings(); // leaving a settings page keeps its changes
    launcher_page = page;
    jfg_config_clear_option_focus();
    jfg_config_menu_refresh();
    if (launcher_page == "textures") refresh_official(true);
    if (launcher_page == "store" && !store_loaded) { store_loaded = true; jfg_store_refresh(); refresh_store_rows(); }
    if (launcher_page == "controls") jfg_controls_page_entered();
    model_handle.DirtyVariable("page");
    model_handle.DirtyVariable("page_title");
    model_handle.DirtyVariable("page_subtitle");
    model_handle.DirtyVariable("skip_launcher"); // the General page has the same switch
}

// ---- controller (ui_state.cpp, while the launcher is shown; D-pad / left stick / A / Select already work as keys) ------
// LB / RB: the page before / after in the sidebar (focusing its button opens the page), wrapping round.
void jfg_launcher_step_page(int dir) {
    if (update_dialog || changelog_dialog) return;
    Rml::ElementDocument* doc = launcher_document();
    if (doc == nullptr) return;
    constexpr int n = (int)(sizeof(LAUNCHER_PAGES) / sizeof(LAUNCHER_PAGES[0]));
    int cur = 0;
    for (int i = 0; i < n; i++) if (launcher_page == LAUNCHER_PAGES[i].id) cur = i;
    int next = ((cur + dir) % n + n) % n;
    if (std::string(LAUNCHER_PAGES[next].id) == "bindings") next = ((next + dir) % n + n) % n; // not in the sidebar
    if (Rml::Element* button = doc->GetElementById(LAUNCHER_PAGES[next].nav_id)) button->Focus(true);
}

// Customize Buttons (Controls page) opens the bindings page; Back to Controls or Esc / B goes back onto that button.
void jfg_launcher_show_bindings(bool open) {
    set_launcher_page(open ? "bindings" : "controls");
    focus_soon(open ? "input_row_button_0_0" : "jfg_customize_buttons");
}

// Start: the Play page, on its Start button (or Browse while no ROM is loaded).
void jfg_launcher_start_pressed() {
    if (update_dialog || changelog_dialog) return;
    Rml::ElementDocument* doc = launcher_document();
    if (doc == nullptr) return;
    if (Rml::Element* button = doc->GetElementById("nav_play")) button->Focus(true);
    focus_soon(rom_valid ? "rl_start" : "rl_browse");
}

// The element of `root` (itself or the first one inside it) that can scroll vertically.
static Rml::Element* find_scroller(Rml::Element* root) {
    if (root == nullptr) return nullptr;
    if (root->GetScrollHeight() > root->GetClientHeight() + 1.0f) return root;
    for (int i = 0; i < root->GetNumChildren(); i++) {
        if (Rml::Element* found = find_scroller(root->GetChild(i))) return found;
    }
    return nullptr;
}

// Right stick: scrolls by `pixels` (positive = down) the open dialog's notes, else what holds the focus on the page
// (the nearest scrolling box round it), else the page's first scrolling box.
void jfg_launcher_scroll(float pixels) {
    Rml::ElementDocument* doc = launcher_document();
    if (doc == nullptr) return;
    Rml::Element* scroller = nullptr;
    if (changelog_dialog || update_dialog) {
        scroller = doc->GetElementById(changelog_dialog ? "rl_changelog_notes" : "rl_update_notes");
    }
    else {
        Rml::Element* page = doc->GetElementById("rl_page_" + launcher_page);
        if (page == nullptr) return;
        // Get Mods: the D-pad moves through the cards (they scroll to it), so the stick reads the chosen mod's details.
        if (Rml::Element* detail = launcher_page == "store" ? doc->GetElementById("st_detail") : nullptr;
            detail != nullptr && detail->GetScrollHeight() > detail->GetClientHeight() + 1.0f) {
            detail->SetScrollTop(detail->GetScrollTop() + pixels);
            return;
        }
        // Only when the focus is inside the page (not in the sidebar): the nearest scrolling box between it and the page.
        bool in_page = false;
        for (Rml::Element* e = doc->GetFocusLeafNode(); e != nullptr && !in_page; e = e->GetParentNode()) in_page = e == page;
        if (in_page) {
            for (Rml::Element* e = doc->GetFocusLeafNode(); e != nullptr && e != page && scroller == nullptr; e = e->GetParentNode()) {
                if (e->GetScrollHeight() > e->GetClientHeight() + 1.0f) scroller = e;
            }
        }
        if (scroller == nullptr) scroller = find_scroller(page);
    }
    if (scroller != nullptr) scroller->SetScrollTop(scroller->GetScrollTop() + pixels);
}

// Called every UI frame (ui_state.cpp): steps the VibinToast icon while the About page is shown.
void jfg_launcher_ui_tick() {
    jfg::locale::debug_pick();
    // A language picked here or anywhere else: the list's choice and every text this file sets.
    if (model_handle) {
        const std::string chosen = jfg::locale::chosen();
        if (chosen != shown_locale) {
            shown_locale = chosen;
            refresh_locales();
            refresh_official(true);
            model_handle.DirtyAllVariables();
        }
    }
    if (!pending_focus_id.empty() && --pending_focus_frames <= 0) {
        if (recompui::is_context_shown(launcher_context)) {
            if (Rml::ElementDocument* doc = launcher_document()) {
                if (Rml::Element* element = doc->GetElementById(pending_focus_id)) element->Focus(true);
            }
        }
        pending_focus_id.clear();
    }
    // "Check for Updates" turned off (General page): the banner and the triangle go; turned on, they come once found.
    if (model_handle && update_available && !jfg_launcher_check_updates() && !update_dialog) {
        update_available = false;
        model_handle.DirtyVariable("update_available");
    }
    if (model_handle && !update_available && jfg_launcher_check_updates()) {
        JfgUpdateInfo info;
        if (jfg_update_available(info)) {
            update_available = true;
            update_version = info.version;
            update_changelog = jfg_changelog_rml(info.changelog, version_string);
            update_can_install = info.can_install;
            update_unavailable = info.unavailable;
            model_handle.DirtyVariable("update_available");
            model_handle.DirtyVariable("update_version");
            model_handle.DirtyVariable("update_changelog");
            model_handle.DirtyVariable("update_can_install");
            model_handle.DirtyVariable("update_unavailable");
        }
    }
    if (model_handle && update_dialog) {
        const JfgUpdateProgress progress = jfg_update_progress();
        if (progress.stage != update_stage) {
            update_stage = progress.stage;
            model_handle.DirtyVariable("update_stage");
            if (update_stage == 2) focus_soon("rl_update_later"); // the install failed: "Update now" is gone, "Close" stays
        }
        if (progress.percent != update_percent) { update_percent = progress.percent; model_handle.DirtyVariable("update_percent"); }
        if (progress.text != update_status) { update_status = progress.text; model_handle.DirtyVariable("update_status"); }
        if (progress.stage == 3 && !update_restarting) {
            // Installed: start the new build and leave. (Settings are saved first, like leaving a settings page.)
            update_restarting = true;
            jfg_launcher_commit_settings();
            jfg_update_restart();
            ultramodern::quit();
        }
    }
    // The official pack: its download progress (Texture Packs page), the list again once a download or the check ends,
    // and the Play page's bar.
    if (model_handle) {
        static std::chrono::steady_clock::time_point next_look{};
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_look) {
            next_look = now + std::chrono::milliseconds(250);
            jfg_official_check_start(); // nothing if it has been asked already, or Check for Updates is off
            const int serial = jfg_official_state(false).serial;
            const bool changed = serial != official_serial;
            if (changed) {
                official_serial = serial;
                jfg_config_menu_refresh(); // a new pack to list
            }
            refresh_official(changed);
        }
    }
    // The mod store: read the first time its page is shown (also when the launcher opens on it), then a background
    // install's progress, and the list again once it ends.
    if (model_handle && launcher_page == "store" && !store_loaded) { store_loaded = true; jfg_store_refresh(); refresh_store_rows(); }
    if (model_handle && store_loaded) {
        jfg_store_tick();
        const JfgStoreState state = jfg_store_state();
        if (state.serial != store_serial) { store_serial = state.serial; refresh_store_rows(); }
    }
    if (!model_handle || launcher_page != "about") return;
    using namespace std::chrono;
    const long long ms = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
    const int frame = (int)((ms * TOAST_FPS / 1000) % TOAST_FRAMES);
    if (frame != toast_frame) {
        toast_frame = frame;
        model_handle.DirtyVariable("toast_frame");
    }
}

extern std::vector<recomp::GameEntry> supported_games;

void open_rom_select_dialog() {
    dino::runtime::open_file_dialog([](bool success, const std::filesystem::path& path) {
        if (success) {
            recomp::RomValidationError rom_error = recomp::select_rom(path, supported_games[0].game_id);
            switch (rom_error) {
                case recomp::RomValidationError::Good:
                    rom_valid = true;
                    model_handle.DirtyVariable("rom_valid");
                    model_handle.DirtyVariable("browse_label");
                    break;
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    break;
                case recomp::RomValidationError::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    break;
                case recomp::RomValidationError::IncorrectRom:
                    recompui::message_box("This ROM is not the correct game.");
                    break;
                case recomp::RomValidationError::NotYet:
                    recompui::message_box("This game isn't supported yet.");
                    break;
                case recomp::RomValidationError::IncorrectVersion:
                    recompui::message_box(
                            "This ROM is Jet Force Gemini, but it is the wrong version.\nThis build requires the unmodified US ROM.");
                    break;
                case recomp::RomValidationError::OtherError:
                    recompui::message_box("An unknown error has occurred.");
                    break;
            }
        }
    });
}

void select_rom() {
    recompui::open_info_prompt("Recomp Setup",
        "Welcome to Jet Force Gemini: Recompiled!\n\n"
            "Choose your legally obtained, unmodified Jet Force Gemini US ROM. "
            "The ROM stays on your computer and is never bundled into the application.",
        "Select ROM",
        open_rom_select_dialog,
        recompui::ButtonVariant::Primary);
}

void start_game() {
    jfg_launcher_commit_settings(); // JFG PORT: settings changed on the launcher's pages
    recomp::start_game(supported_games[0].game_id);
    recompui::hide_all_contexts();
}

void start_game_preflight() {
    dino::renderer::preload_common_shaders();
    start_game();
}

recompui::ContextId launcher_context;

recompui::ContextId recompui::get_launcher_context_id() {
	return launcher_context;
}

class LauncherMenu : public recompui::MenuController {
public:
    LauncherMenu() {
        rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
    }
    void load_document() override {
        // JFG PORT: the launcher's images (see assets/launcher.rml).
        queue_launcher_image("jfg_vibintoast", "jfg_launcher/vibintoast.png");
        queue_launcher_image("jfg_scanlines", "jfg_launcher/scanlines.png");
        logo_available = queue_launcher_image("jfg_logo", "jfg_launcher/logo.png");
		launcher_context = recompui::create_context(dino::runtime::get_asset_path("launcher.rml"));
        // JFG_DEBUG_LAUNCHER_PAGE=<page> (Recomp-Docs/REFERENCE.md): opens on that page (play, profiles, ..., mods,
        // about), for window captures without key presses.
        if (const char* page = std::getenv("JFG_DEBUG_LAUNCHER_PAGE")) launcher_page = page;
        // JFG PORT: with a ROM loaded, the launcher opens on its Start button (autofocus in launcher.rml follows rom_valid).
        if (rom_valid) focus_soon("rl_start");
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "select_rom",
            [](const std::string& param, Rml::Event& event) {
                select_rom();
            }
        );
        recompui::register_event(listener, "rom_selected",
            [](const std::string& param, Rml::Event& event) {
                rom_valid = true;
                model_handle.DirtyVariable("rom_valid");
            }
        );
        recompui::register_event(listener, "start_game",
            [](const std::string& param, Rml::Event& event) {
                start_game_preflight();
            }
        );
        recompui::register_event(listener, "open_controls",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::Controls);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_settings",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::General);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_mods",
            [](const std::string &param, Rml::Event &event) {
                recompui::set_config_tab(recompui::ConfigTab::Mods);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "exit_game",
            [](const std::string& param, Rml::Event& event) {
                jfg_launcher_commit_settings(); // JFG PORT
                ultramodern::quit();
            }
        );
        // JFG PORT: Esc / B on a page goes back to its button in the sidebar.
        recompui::register_event(listener, "launcher_keydown",
            [](const std::string& param, Rml::Event& event) {
                if (event.GetId() != Rml::EventId::Keydown || recompui::is_prompt_open()) return;
                const auto key = event.GetParameter<Rml::Input::KeyIdentifier>("key_identifier", Rml::Input::KeyIdentifier::KI_UNKNOWN);
                // Up / Down (D-pad, stick, arrows) and Page Up / Page Down scroll the notes of an open dialog: its buttons
                // don't navigate up or down, so a controller has no other way to read a long changelog.
                if (changelog_dialog || update_dialog) {
                    const bool line_up = key == Rml::Input::KeyIdentifier::KI_UP, line_down = key == Rml::Input::KeyIdentifier::KI_DOWN;
                    const bool page_up = key == Rml::Input::KeyIdentifier::KI_PRIOR, page_down = key == Rml::Input::KeyIdentifier::KI_NEXT;
                    if (line_up || line_down || page_up || page_down) {
                        Rml::Element* target = event.GetTargetElement();
                        Rml::ElementDocument* doc = target ? target->GetOwnerDocument() : nullptr;
                        Rml::Element* notes = doc ? doc->GetElementById(changelog_dialog ? "rl_changelog_notes" : "rl_update_notes") : nullptr;
                        if (notes != nullptr) {
                            const float step = notes->GetClientHeight() * ((page_up || page_down) ? 0.9f : 0.12f);
                            notes->SetScrollTop(notes->GetScrollTop() + ((line_up || page_up) ? -step : step));
                        }
                        return;
                    }
                }
                if (key != Rml::Input::KeyIdentifier::KI_ESCAPE) return;
                if (changelog_dialog) {
                    close_changelog_dialog();
                    return;
                }
                if (update_dialog) { // Esc / Back is "Later" (not while it is installing)
                    close_update_dialog();
                    return;
                }
                if (launcher_page == "bindings" && dino::input::get_scanned_input_index() < 0) {
                    jfg_launcher_show_bindings(false);
                    return;
                }
                Rml::Element* target = event.GetTargetElement();
                Rml::ElementDocument* doc = target ? target->GetOwnerDocument() : nullptr;
                if (doc == nullptr) return;
                // Get Mods: from Install / Remove back to the mod's card first.
                if (launcher_page == "store" && (target->GetId() == "st_act" || target->GetId() == "st_remove")) {
                    if (Rml::Element* card = doc->GetElementById(store_sel_card.substr(1))) { card->Focus(true); return; }
                }
                if (Rml::Element* button = doc->GetElementById(launcher_page_info().nav_id)) button->Focus(true);
            }
        );
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");

        constructor.Bind("rom_valid", &rom_valid);

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);
        constructor.Bind("update_available", &update_available);
        constructor.Bind("update_version", &update_version);
        constructor.Bind("update_dialog", &update_dialog);
        constructor.Bind("update_changelog", &update_changelog);
        constructor.Bind("update_can_install", &update_can_install);
        constructor.Bind("update_unavailable", &update_unavailable);
        constructor.Bind("update_stage", &update_stage);
        constructor.Bind("update_percent", &update_percent);
        constructor.Bind("update_status", &update_status);
        constructor.Bind("changelog_dialog", &changelog_dialog);
        constructor.Bind("changelog_text", &changelog_text);
        constructor.BindEventCallback("show_changelog", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
            open_changelog_dialog(event_document(event));
        });
        constructor.BindEventCallback("changelog_close", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            close_changelog_dialog();
        });
        constructor.BindEventCallback("show_update", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList&) {
            open_update_dialog(event_document(event));
        });
        constructor.BindEventCallback("update_close", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            close_update_dialog();
        });
        constructor.BindEventCallback("update_install", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (update_stage != 0) return;
            if (!update_can_install) { jfg_update_fail(update_unavailable); return; } // the poll in jfg_launcher_ui_tick shows the message
            jfg_update_install();
            update_stage = 1;
            update_percent = 0;
            update_status = jfg::locale::ui("Starting...");
            model_handle.DirtyVariable("update_stage");
            model_handle.DirtyVariable("update_percent");
            model_handle.DirtyVariable("update_status");
        });
        jfg_update_check_start(version_string);
        official_shown = jfg_launcher_show_official_pack();
        jfg_official_check_start(); // the official HD texture pack's latest version (Texture Packs page)

        // JFG PORT: the language drop-down (Play page).
        if (auto locale = constructor.RegisterStruct<LauncherLocale>()) {
            locale.RegisterMember("id", &LauncherLocale::id);
            locale.RegisterMember("name", &LauncherLocale::name);
            locale.RegisterMember("selected", &LauncherLocale::selected);
        }
        constructor.RegisterArray<std::vector<LauncherLocale>>();
        refresh_locales();
        constructor.Bind("rl_locales", &launcher_locales);
        constructor.Bind("rl_locale_name", &launcher_locale_name);
        constructor.Bind("rl_lang_open", &lang_open);
        constructor.BindEventCallback("toggle_lang", [](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList&) {
            lang_open = !lang_open;
            model.DirtyVariable("rl_lang_open");
            if (lang_open) {
                for (size_t i = 0; i < launcher_locales.size(); i++) if (launcher_locales[i].selected) focus_soon("rl_lang_" + std::to_string(i));
            }
        });
        constructor.BindEventCallback("pick_locale", [](Rml::DataModelHandle model, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) jfg::locale::choose(args[0].Get<Rml::String>());
            lang_open = false;
            model.DirtyVariable("rl_lang_open");
            focus_soon("rl_lang");
        });

        // JFG PORT: the official HD texture pack (Texture Packs page).
        if (auto row = constructor.RegisterStruct<OfficialPackRow>()) {
            row.RegisterMember("variant", &OfficialPackRow::variant);
            row.RegisterMember("size_text", &OfficialPackRow::size_text);
            row.RegisterMember("action", &OfficialPackRow::action);
            row.RegisterMember("installed", &OfficialPackRow::installed);
        }
        constructor.RegisterArray<std::vector<OfficialPackRow>>();
        constructor.Bind("official_rows", &official_rows);
        constructor.Bind("official_status", &official_status);
        constructor.Bind("official_text", &official_text);
        constructor.Bind("official_result", &official_result);
        constructor.Bind("official_busy", &official_busy);
        constructor.Bind("official_shown", &official_shown);
        constructor.Bind("official_update", &official_update);
        constructor.Bind("official_update_title", &official_update_title);
        constructor.Bind("official_update_text", &official_update_text);
        // The Play page's bar: to the Texture Packs page, on that pack's Update button.
        constructor.BindEventCallback("official_show_update", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            set_launcher_page("textures");
            for (size_t i = 0; i < official_rows.size(); i++) {
                if (official_rows[i].variant == official_update_variant) focus_soon("rl_official_" + std::to_string(i));
            }
        });
        constructor.Bind("official_percent", &official_percent);
        constructor.BindEventCallback("official_install", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty() || official_busy) return;
            const Rml::String variant = args[0].Get<Rml::String>();
            for (const OfficialPackRow& row : official_rows) {
                if (row.variant == variant && !row.installed) { jfg_official_install(variant); refresh_official(false); }
            }
        });
        constructor.BindEventCallback("official_cancel", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            jfg_official_cancel();
        });

        // JFG PORT: the mod store (Get Mods page).
        if (auto row = constructor.RegisterStruct<StoreRow>()) {
            row.RegisterMember("id", &StoreRow::id);
            row.RegisterMember("name", &StoreRow::name);
            row.RegisterMember("short_text", &StoreRow::short_text);
            row.RegisterMember("version", &StoreRow::version);
            row.RegisterMember("source_name", &StoreRow::source_name);
            row.RegisterMember("tags", &StoreRow::tags);
            row.RegisterMember("size_text", &StoreRow::size_text);
            row.RegisterMember("thumb", &StoreRow::thumb);
            row.RegisterMember("status", &StoreRow::status);
            row.RegisterMember("action", &StoreRow::action);
            row.RegisterMember("installed", &StoreRow::installed);
            row.RegisterMember("update", &StoreRow::update);
            row.RegisterMember("managed", &StoreRow::managed);
            row.RegisterMember("blocked", &StoreRow::blocked);
            row.RegisterMember("busy", &StoreRow::busy);
            row.RegisterMember("selected", &StoreRow::selected);
        }
        constructor.RegisterArray<std::vector<StoreRow>>();
        if (auto src = constructor.RegisterStruct<StoreSourceRow>()) {
            src.RegisterMember("id", &StoreSourceRow::id);
            src.RegisterMember("name", &StoreSourceRow::name);
            src.RegisterMember("status", &StoreSourceRow::status);
            src.RegisterMember("enabled", &StoreSourceRow::enabled);
            src.RegisterMember("official", &StoreSourceRow::official);
        }
        constructor.RegisterArray<std::vector<StoreSourceRow>>();
        constructor.Bind("store_rows", &store_rows);
        constructor.Bind("store_sources", &store_sources);
        constructor.Bind("store_filter", &store_filter);
        constructor.Bind("store_selected", &store_selected);
        constructor.Bind("store_busy", &store_busy);
        constructor.Bind("store_percent", &store_percent);
        constructor.Bind("store_text", &store_text);
        constructor.Bind("store_preview", &store_preview);
        constructor.Bind("store_sel_name", &store_sel_name);
        constructor.Bind("store_sel_desc", &store_sel_desc);
        constructor.Bind("store_sel_meta", &store_sel_meta);
        constructor.Bind("store_sel_deps", &store_sel_deps);
        constructor.Bind("store_sel_note", &store_sel_note);
        constructor.Bind("store_sel_thumb", &store_sel_thumb);
        constructor.Bind("store_sel_action", &store_sel_action);
        constructor.Bind("store_sel_can_act", &store_sel_can_act);
        constructor.Bind("store_sel_can_remove", &store_sel_can_remove);
        constructor.Bind("store_sel_blocked", &store_sel_blocked);
        constructor.Bind("store_count_all", &store_count_all);
        constructor.Bind("store_count_installed", &store_count_installed);
        constructor.Bind("store_count_updates", &store_count_updates);
        constructor.Bind("store_sel_card", &store_sel_card);
        constructor.Bind("store_right", &store_right);
        constructor.BindEventCallback("store_set_filter", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) { store_filter = args[0].Get<Rml::String>(); refresh_store_rows(); }
        });
        // A card that gets the focus (D-pad, stick) is the chosen mod, and scrolls into view.
        constructor.BindEventCallback("store_focus", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
            if (args.empty()) return;
            if (Rml::Element* card = event.GetTargetElement())
                card->ScrollIntoView(Rml::ScrollIntoViewOptions(Rml::ScrollAlignment::Nearest, Rml::ScrollAlignment::Nearest));
            if (store_selected == args[0].Get<Rml::String>()) return;
            store_selected = args[0].Get<Rml::String>();
            refresh_store_rows();
            if (Rml::Element* detail = event.GetTargetElement()->GetOwnerDocument()->GetElementById("st_detail")) detail->SetScrollTop(0.0f);
        });
        // A card clicked: chosen; pressed with A (the controller in use): on to its Install / Update / Remove.
        constructor.BindEventCallback("store_select", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) return;
            store_selected = args[0].Get<Rml::String>();
            refresh_store_rows();
            if (recompui::get_cont_active() && store_right != "none") focus_soon(std::string(store_right.substr(1)));
        });
        constructor.BindEventCallback("store_install", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            const std::string id = args.empty() ? std::string(store_selected) : std::string(args[0].Get<Rml::String>());
            if (!id.empty()) { store_selected = id; jfg_store_install(id); refresh_store_rows(); focus_soon(std::string(store_sel_card.substr(1))); }
        });
        constructor.BindEventCallback("store_remove", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (!store_selected.empty()) {
                jfg_store_remove(store_selected);
                refresh_store_rows();
                focus_soon(std::string((store_right != "none" ? store_right : store_sel_card).substr(1)));
            }
        });
        constructor.BindEventCallback("store_toggle_source", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (args.empty()) return;
            const std::string id = args[0].Get<Rml::String>();
            for (size_t i = 0; i < store_sources.size(); i++) {
                if (store_sources[i].id != id) continue;
                jfg_store_set_source_enabled(id, !store_sources[i].enabled);
                focus_soon("st_source_" + std::to_string(i)); // stays on the switch while the list is made again
            }
            refresh_store_rows();
        });
        constructor.BindEventCallback("store_reload", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            jfg_store_refresh();
            refresh_store_rows();
        });

        // JFG PORT: the launcher's pages (see the top of this file).
        constructor.Bind("page", &launcher_page);
        constructor.Bind("toast_frame", &toast_frame);
        constructor.Bind("logo_available", &logo_available);
        constructor.BindFunc("page_title", [](Rml::Variant& out) { out = jfg::locale::ui(launcher_page_info().title); });
        constructor.BindFunc("page_subtitle", [](Rml::Variant& out) { out = jfg::locale::ui(launcher_page_info().subtitle); });
        constructor.BindFunc("browse_label", [](Rml::Variant& out) { out = jfg::locale::ui(rom_valid ? "Change ROM..." : "Browse for ROM..."); });
        constructor.BindEventCallback("set_page", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) set_launcher_page(args[0].Get<Rml::String>());
        });
        constructor.BindEventCallback("browse_rom", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            open_rom_select_dialog();
        });
        // "Skip the launcher" (Play page, once a ROM is loaded): saved to bin\jfg_launcher.txt as skip_launcher.
        constructor.BindFunc("skip_launcher", [](Rml::Variant& out) { out = jfg_launcher_skip(); });
        constructor.BindEventCallback("toggle_skip_launcher", [](Rml::DataModelHandle handle, Rml::Event&, const Rml::VariantList&) {
            jfg_launcher_set_skip(!jfg_launcher_skip());
            handle.DirtyVariable("skip_launcher");
            jfg_config_menu_refresh(); // the General page's copy of the switch
        });
        constructor.BindEventCallback("start_game", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (rom_valid) start_game_preflight();
        });
        // Links (About page, Texture Packs note): opened in the default browser.
        constructor.BindEventCallback("open_url", [](Rml::DataModelHandle, Rml::Event& event, const Rml::VariantList& args) {
            Rml::Element* button = event.GetCurrentElement();
            if (!args.empty()) jfg_open_url_prompt(args[0].Get<Rml::String>(), button != nullptr ? button->GetId() : std::string());
        });

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
