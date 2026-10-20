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

static std::string version_string;
#include "jfg/jfg_update_check.h"
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

struct LauncherPageInfo { const char* id; const char* title; const char* subtitle; const char* nav_id; };
static const LauncherPageInfo LAUNCHER_PAGES[] = {
    { "play", "Play", "Load your ROM, then jump in.", "nav_play" },
    { "profiles", "Profiles", "Whole sets of settings: the original game, Modern, Modern and Balanced, Extreme or your own.", "tab_jfg_profiles" },
    { "general", "General", "Volume, the settings menu, what is shown on screen and the fly camera.", "tab_jfg_general" },
    { "gameplay", "Gameplay", "Saving, gore, player tweaks and movement.", "tab_jfg_gameplay" },
    { "controls", "Controls", "Dual-stick, mouse and keyboard, aiming, and the controller's stick and rumble.", "tab_jfg_controls" },
    { "bindings", "Bindings", "Button bindings for the controller and the keyboard.", "tab_controls" },
    { "graphics", "Graphics", "Window, resolution, frame rate, widescreen and effects.", "tab_jfg_graphics" },
    { "textures", "Texture Packs", "Pick a texture pack from the texture_packs folder, and dump the game's textures to make your own.", "tab_jfg_textures" },
    { "mods", "Mods", "Turn mods on and off, set their order and options, and install new ones.", "tab_mods" },
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
    // The commits since the last release (from GitHub, when they have arrived) above the changelog shipped with the game.
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

static void set_launcher_page(const std::string& page) {
    if (launcher_page == page) return;
    jfg_launcher_commit_settings(); // leaving a settings page keeps its changes
    launcher_page = page;
    jfg_config_menu_refresh();
    model_handle.DirtyVariable("page");
    model_handle.DirtyVariable("page_title");
    model_handle.DirtyVariable("page_subtitle");
}

// Called every UI frame (ui_state.cpp): steps the VibinToast icon while the About page is shown.
void jfg_launcher_ui_tick() {
    if (!pending_focus_id.empty() && --pending_focus_frames <= 0) {
        if (recompui::is_context_shown(launcher_context)) {
            if (Rml::ElementDocument* doc = launcher_document()) {
                if (Rml::Element* element = doc->GetElementById(pending_focus_id)) element->Focus(true);
            }
        }
        pending_focus_id.clear();
    }
    if (model_handle && !update_available) {
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
                Rml::Element* target = event.GetTargetElement();
                Rml::ElementDocument* doc = target ? target->GetOwnerDocument() : nullptr;
                if (doc == nullptr) return;
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
            update_status = "Starting...";
            model_handle.DirtyVariable("update_stage");
            model_handle.DirtyVariable("update_percent");
            model_handle.DirtyVariable("update_status");
        });
        jfg_update_check_start(version_string);

        // JFG PORT: the launcher's pages (see the top of this file).
        constructor.Bind("page", &launcher_page);
        constructor.Bind("toast_frame", &toast_frame);
        constructor.Bind("logo_available", &logo_available);
        constructor.BindFunc("page_title", [](Rml::Variant& out) { out = Rml::String(launcher_page_info().title); });
        constructor.BindFunc("page_subtitle", [](Rml::Variant& out) { out = Rml::String(launcher_page_info().subtitle); });
        constructor.BindFunc("browse_label", [](Rml::Variant& out) { out = Rml::String(rom_valid ? "Change ROM..." : "Browse for ROM..."); });
        constructor.BindEventCallback("set_page", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) set_launcher_page(args[0].Get<Rml::String>());
        });
        constructor.BindEventCallback("browse_rom", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            open_rom_select_dialog();
        });
        constructor.BindEventCallback("start_game", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) {
            if (rom_valid) start_game_preflight();
        });
        // Links (About page, Texture Packs note): opened in the default browser.
        constructor.BindEventCallback("open_url", [](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) {
            if (!args.empty()) SDL_OpenURL(args[0].Get<Rml::String>().c_str());
        });

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
