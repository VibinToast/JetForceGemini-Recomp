#include <cstdio>
#include <cassert>
#include <cstdlib>
#include <thread>
#include <vector>
#include <filesystem>
#include <string>
#include <system_error>

#include "nfd.h"
#include "ultramodern/ultramodern.hpp"
#include "librecomp/game.hpp"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <timeapi.h>
#endif

#include "input/input.hpp"
#include "input/controls.hpp"
#include "renderer/renderer.hpp"
#include "config/config.hpp"
#include "recomp_api/debug_ui_api.hpp"
#include "recomp_api/general_api.hpp"
#include "recomp_api/recomp_data_api.hpp"
#include "common/sdl.hpp"
#include "ui/recomp_ui.h"
#include "jfg/jfg_update_check.h"

#include "runtime/audio.hpp"
#include "runtime/crash.hpp"
#include "runtime/gfx.hpp"
#include "runtime/mods.hpp"
#include "runtime/overlays.hpp"
#include "runtime/support.hpp"
#include "runtime/preload.hpp"
#include "runtime/rsp.hpp"
#include "runtime/threads.hpp"

const std::string version_string = JFG_VERSION_STRING; // JFG PORT: from version.txt (CMakeLists.txt)

extern "C" void recomp_entrypoint(uint8_t *rdram, recomp_context *ctx);
gpr get_entrypoint_address();
extern "C" void jfg_apply_asset_mods(void); // JFG PORT: src/jfg/jfg_assets.cpp

static void custom_recomp_entrypoint(uint8_t *rdram, recomp_context *ctx) {
    printf("Initialized RDRAM at %p\n", rdram);

    dino::runtime::crash_register_rdram(rdram);
    dino::runtime::crash_register_thread_context(ctx);

    jfg_apply_asset_mods(); // JFG PORT: data replaced by mods (src/jfg/jfg_assets.cpp), once mods are loaded

    recomp_entrypoint(rdram, ctx);
}

// JFG port: boot-time globals the runtime's IPL3 emulation leaves unset (src/jfg/jfg_glue.cpp).
extern "C" void jfg_on_init(uint8_t* rdram, recomp_context* ctx);
// JFG port: mod support (src/jfg/jfg_mods.cpp): functions mods import, and the pre-linked ROM hooks regenerate from.
extern "C" void jfg_register_mod_exports(void);
std::vector<uint8_t> jfg_prelink_rom(std::span<const uint8_t> rom);

// JFG PORT: Jet Force Gemini's modified osCreateThread (libultra/src/os/createthread.c, JFGDIFFS) starts every
// thread with SR_FR set, i.e. 64-bit FPU registers where odd-numbered registers are independent, and clears it
// only for threads created with a negative id (the audio thread, whose libultra code needs FR = 0).
// The runtime replaces osCreateThread, so reproduce that rule here; game code that uses odd FPU registers
// (camera, collision and matrix maths) computes garbage otherwise.
static void custom_thread_create_callback(uint8_t* rdram, recomp_context* ctx) {
    dino::runtime::crash_register_thread_context(ctx);

    PTR(OSThread) self = ultramodern::this_thread();
    const bool fr = (self == NULLPTR) || (TO_PTR(OSThread, self)->id >= 0);
    if (fr) {
        ctx->f_odd = &ctx->f1.u32l;
        ctx->mips3_float_mode = true;
        ctx->status_reg |= 0x04000000u; // SR_FR
    }
}

// array of supported GameEntry objects
std::vector<recomp::GameEntry> supported_games = {
    {
        // XXH3-64 of the US ROM (sha1 493ced9008dbe932d6e91179b68e8630cf23a023)
        .rom_hash = 0x44830C263C2DE108,
        .internal_name = "JET FORCE GEMINI    ",
        .game_id = u8"jfg-us",
        .mod_game_id = "jet-force-gemini",
        .save_type = recomp::SaveType::Flashram,
        .is_enabled = true,
        // JFG PORT: not compressed, but mods' hooks regenerate functions from this ROM, and the port recompiles
        // overlays pre-linked at private addresses (tools/prepare_overlays.py): jfg_prelink_rom gives the same copy.
        .decompression_routine = jfg_prelink_rom,
        .has_compressed_code = true,
        .entrypoint_address = get_entrypoint_address(),
        .entrypoint = custom_recomp_entrypoint,
        .thread_create_callback = custom_thread_create_callback,
        .on_init_callback = jfg_on_init,
    },
};

struct CliArgs {
    bool skip_launcher = false;
    bool force_launcher = false;
    bool show_console = false;
    const char* rom_path = nullptr; // JFG port: --rom <path> selects/validates the ROM without the launcher UI
    int window_width = 1600;
    int window_height = 960;
};
static CliArgs cli_args;

static ultramodern::renderer::WindowHandle create_window(ultramodern::gfx_callbacks_t::gfx_data_t data) {
    return dino::runtime::create_window(data, cli_args.window_width, cli_args.window_height);
}

int main(int argc, char** argv) {
    // Setup crash handler
    dino::runtime::crash_setup_handler();

    // Parse CLI args
    for (int i = 1; i < argc; i++) {
        char *arg = argv[i];
        if (strcmp(arg, "--skip-launcher") == 0) {
            cli_args.skip_launcher = true;
        } else if (strcmp(arg, "--rom") == 0) {
            i++;
            if (i < argc) {
                // Made absolute here: the working directory changes below.
                static std::string rom_path_absolute;
                std::error_code ec;
                const std::filesystem::path absolute = std::filesystem::absolute(argv[i], ec);
                rom_path_absolute = ec ? std::string{argv[i]} : absolute.string();
                cli_args.rom_path = rom_path_absolute.c_str();
            }
        } else if (strcmp(arg, "--launcher") == 0) {
            cli_args.force_launcher = true;
        } else if (strcmp(argv[i], "--show-console") == 0) {
            cli_args.show_console = true;
        } else if (strcmp(argv[i], "--4x3") == 0) {
            cli_args.window_width = 320 * 4;
            cli_args.window_height = 240 * 4;
        } else if (strcmp(argv[i], "--window-width") == 0) {
            i++;
            if (i < argc) {
                int width = std::atoi(argv[i]);
                if (width > 0) {
                    cli_args.window_width = width;
                }
            }
        } else if (strcmp(argv[i], "--window-height") == 0) {
            i++;
            if (i < argc) {
                int height = std::atoi(argv[i]);
                if (height > 0) {
                    cli_args.window_height = height;
                }
            }
        }
    }

#if !defined(__APPLE__)
    // JFG port: run from the folder the executable is in, whatever folder it was started from. Assets, the controller
    // database, saves, settings and the options file are all found relative to it.
    {
        std::error_code ec;
        std::filesystem::current_path(dino::config::get_executable_folder_path(), ec);
    }
#endif

    // Load project version
    recomp::Version project_version{};
    if (!recomp::Version::from_string(version_string, project_version)) {
        ultramodern::error_handling::message_box(("Invalid version string: " + version_string).c_str());
        return EXIT_FAILURE;
    }

    // Map this executable into memory and lock it, which should keep it in physical memory. This ensures
    // that there are no stutters from the OS having to load new pages of the executable whenever a new code page is run.
    dino::runtime::PreloadContext preload_context;
    bool preloaded = preload_executable(preload_context);

    if (!preloaded) {
        fprintf(stderr, "Failed to preload executable!\n");
    }

#ifdef _WIN32
    // Set up high resolution timing period.
    timeBeginPeriod(1);

    // Allocate console on Windows if requested
    if (cli_args.show_console) {
        if (GetConsoleWindow() == nullptr) {
            AllocConsole();
            freopen("CONIN$", "r", stdin);
            freopen("CONOUT$", "w", stderr);
            freopen("CONOUT$", "w", stdout);
        }
    }

    // Set up console output to accept UTF-8 on windows
    SetConsoleOutputCP(CP_UTF8);

    // Change to a font that supports Japanese characters
    CONSOLE_FONT_INFOEX cfi;
    cfi.cbSize = sizeof cfi;
    cfi.nFont = 0;
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 16;
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, L"NSimSun");
    SetCurrentConsoleFontEx(GetStdHandle(STD_OUTPUT_HANDLE), FALSE, &cfi);

    // Force wasapi on Windows, as there seems to be some issue with sample queueing with directsound currently.
    SDL_setenv("SDL_AUDIODRIVER", "wasapi", true);
#endif

#if defined(__linux__) && defined(RECOMP_FLATPAK)
    // When using Flatpak, applications tend to launch from the home directory by default.
    // Mods might use the current working directory to store the data, so we switch it to a directory
    // with persistent data storage and write permissions under Flatpak to ensure it works.
    std::error_code ec;
    std::filesystem::current_path("/var/data", ec);
#endif

    // Initialize SDL audio and set the output frequency.
    SDL_InitSubSystem(SDL_INIT_AUDIO);
    if (!dino::runtime::reset_audio(48000)) {
        // It is not possible to initialize without an audio device.
        return EXIT_FAILURE;
    }

    // Source controller mappings file
    std::string controller_mappings_path = (dino::runtime::get_program_path() / "recompcontrollerdb.txt").string();
    if (SDL_GameControllerAddMappingsFromFile(controller_mappings_path.c_str()) < 0) {
        fprintf(stderr, "Failed to load controller mappings: %s\n", SDL_GetError());
    }

    recomp::register_config_path(dino::config::get_app_folder_path());

    // Register supported games
    for (const auto& game : supported_games) {
        recomp::register_game(game);
    }
    // Populate ROM validity before deciding whether the launcher is needed.
    // The runtime performs this check again during startup; doing it here lets
    // an already configured Mac app launch straight into DKR.
    recomp::check_all_stored_roms();
    // JFG PORT: bin\jfg_launcher.txt (src/jfg/jfg_update_check.cpp). A ROM path there starts the game straight away, as
    // --rom <path> --skip-launcher would, unless --rom or --launcher was given. It also holds check_for_updates.
    jfg_launcher_config_load(dino::config::get_app_folder_path());
    static std::string config_rom;
    if (cli_args.rom_path == nullptr && !cli_args.force_launcher && !jfg_launcher_config_rom().empty()) {
        config_rom = jfg_launcher_config_rom();
        cli_args.rom_path = config_rom.c_str();
        cli_args.skip_launcher = true;
    }
    if (cli_args.rom_path != nullptr) {
        recomp::RomValidationError rom_error = recomp::select_rom(cli_args.rom_path, supported_games[0].game_id);
        fprintf(stderr, "[jfg] --rom %s: %s\n", cli_args.rom_path,
            rom_error == recomp::RomValidationError::Good ? "accepted" : "REJECTED (wrong or modified ROM)");
    }

    recompui::register_ui_exports();
    jfg_register_mod_exports(); // JFG PORT: recomp_printf and other functions mods import (src/jfg/jfg_mods.cpp)
    dino::runtime::register_overlays();

    dino::config::load_config();

    recomp::rsp::callbacks_t rsp_callbacks{
        .get_rsp_microcode = dino::runtime::get_rsp_microcode,
    };

    ultramodern::renderer::callbacks_t renderer_callbacks{
        .create_render_context = dino::renderer::create_render_context,
    };

    ultramodern::gfx_callbacks_t gfx_callbacks{
        .create_gfx = dino::runtime::create_gfx,
        .create_window = create_window,
        .update_gfx = dino::runtime::update_gfx,
    };

    ultramodern::audio_callbacks_t audio_callbacks{
        .queue_samples = dino::runtime::queue_samples,
        .get_frames_remaining = dino::runtime::get_frames_remaining,
        .set_frequency = dino::runtime::set_frequency,
    };

    ultramodern::input::callbacks_t input_callbacks{
        .poll_input = dino::input::poll_inputs,
        .get_input = dino::input::get_n64_input,
        .set_rumble = dino::input::set_rumble,
        .get_connected_device_info = dino::input::get_connected_device_info,
    };

    ultramodern::events::callbacks_t events_callbacks{
        .vi_callback = dino::input::update_rumble,
        .gfx_init_callback = recompui::update_supported_options,
    };

    ultramodern::error_handling::callbacks_t error_handling_callbacks{
        .message_box = recompui::message_box,
    };

    ultramodern::threads::callbacks_t threads_callbacks{
        .get_game_thread_name = dino::runtime::get_game_thread_name,
    };

    dino::runtime::register_mods();

    // Keep the launcher visible during normal starts so RT64 can finish its
    // common pipeline warm-up before gameplay. Developers can still request
    // the old direct-start behavior explicitly with --skip-launcher.
    if (!cli_args.force_launcher && cli_args.skip_launcher &&
        recomp::is_rom_valid(supported_games[0].game_id)) {
        // HACK: This relies on unsupported behavior in the runtime and requires using a fork
        //       of the runtime with a hack to avoid an uninitialized VI origin when doing this
        recomp::start_game(supported_games[0].game_id);
        recompui::hide_all_contexts();
    }

    recomp::Configuration config = {
        .project_version = project_version,
        .rsp_callbacks = rsp_callbacks,
        .renderer_callbacks = renderer_callbacks,
        .audio_callbacks = audio_callbacks,
        .input_callbacks = input_callbacks,
        .gfx_callbacks = gfx_callbacks,
        .events_callbacks = events_callbacks,
        .error_handling_callbacks = error_handling_callbacks,
        .threads_callbacks = threads_callbacks,
        // JFG PORT: a controller (SI) message for a full queue is dropped, as on the N64 (events are sent with
        // OS_MESG_NOBLOCK), instead of being put back in the runtime's external queue. The game's controller queue holds
        // one message and every one is the same; with re-queuing, one surplus message (after a level load, say) kept
        // the idle thread taking it out and putting it back for the whole of every frame, and the video retrace
        // messages waiting behind it reached the game a frame late: two at a time, 30 times a second, so 60 fps
        // ran at 30-45 and looked like 30.
        .message_queue_control = { .requeue_si = false }
    };

    recomp::start(config);

    // Closing the launcher can return before RT64 invokes its render deinit
    // hook. Ensure RmlUi is torn down while its registries and interfaces are
    // still alive; the shutdown path is idempotent if the hook already ran.
    recompui::shutdown();

#ifdef _WIN32
    // End high resolution timing period.
    timeEndPeriod(1);
#endif

    // Note: NFD_Init is done in create_gfx, not main
    NFD_Quit();

    if (preloaded) {
        release_preload(preload_context);
    }

    return EXIT_SUCCESS;
}
