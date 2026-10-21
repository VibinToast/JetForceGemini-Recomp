extern "C" int jfg_skip_render_active(void); // src/jfg/jfg_cutscene_skip.cpp
extern "C" int jfg_widescreen_active(void);  // src/jfg/jfg_visuals.cpp: the "Widescreen" option of the F9 menu
extern "C" int jfg_texture_dump_active(void);
extern "C" void jfg_texture_dump_scene(char* buf, int size); // src/jfg/jfg_texture_packs.cpp: the scene tag of draw_trace.txt
extern "C" void jfg_texture_dump_folder(char* buf, int size); // src/jfg/jfg_texture_packs.cpp: "One folder per level"  // src/jfg/jfg_texture_packs.cpp: "Dump textures" (F9 -> Textures)
extern "C" int jfg_texture_pack_current(char* buf, int size); // the chosen pack folder (relative, "" = none); returns a change counter
extern "C" int jfg_texture_three_point(void); // src/jfg/jfg_visuals.cpp: "Texture filtering" (1 = the N64's three-point filter)
extern "C" int jfg_output_filtering(void);    // src/jfg/jfg_visuals.cpp: "Picture scaling" (0 anti-aliased pixel scaling, 1 linear, 2 nearest)
extern "C" int jfg_interp_limit(void);       // src/jfg/jfg_frame_rate.cpp: "Interpolation Limit" option; 0 = the monitor's rate
extern "C" int jfg_renderer_rate_needs_restart(void); // defined below, read by src/jfg/jfg_frame_rate.cpp
extern "C" int jfg_refresh_override(void);   // src/jfg/jfg_frame_rate.cpp: "Frame rate" option; 0 = Original, 1 = Display (-1 would use rr_option)
#define HLSL_CPU
#include "hle/rt64_application.h"
#include "renderer.hpp"
#include "ultramodern/ultramodern.hpp"

#include <array>
#include <atomic>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#include <json/json.hpp>

#include "ultramodern/config.hpp"

#include "common/overloaded.h"
#include "config/config.hpp"
#include "debug_ui/debug_ui.hpp"
#include "debug_ui/backend.hpp"
#include "ui/recomp_ui.h"
#include "concurrentqueue.h"

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

struct TexturePackEnableAction {
    std::string mod_id;
};

struct TexturePackDisableAction {
    std::string mod_id;
};

struct TexturePackSecondaryEnableAction {
    std::string mod_id;
};

struct TexturePackSecondaryDisableAction {
    std::string mod_id;
};

struct TexturePackUpdateAction {
};

using TexturePackAction = std::variant<TexturePackEnableAction, TexturePackDisableAction, TexturePackSecondaryEnableAction, TexturePackSecondaryDisableAction, TexturePackUpdateAction>;

static moodycamel::ConcurrentQueue<TexturePackAction> texture_pack_action_queue;

// Note: These must be outside of a namespace, N64ModernRuntime references them as global externs
unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

namespace dino::renderer {

static RT64Context* active_rt64_context = nullptr;
static std::mutex active_rt64_context_mutex;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(plume::RenderSampleCounts bits) {
    if (bits & plume::RenderSampleCount::Bits::COUNT_2) {
        if (bits & plume::RenderSampleCount::Bits::COUNT_4) {
            if (bits & plume::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            // JFG PORT: "Expand" widens the render target and relies on widescreen patches in the game (the base
            // project has them for Diddy Kong Racing; Jet Force Gemini has none). Without them only full-width
            // rectangles (screen clears, fades, flashes) spill into the side borders, which shows up as the
            // borders flickering. Treat it as Original until JFG gets real widescreen support.
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

static RT64::UserConfiguration::Filtering jfg_rt64_filtering(int option) {
    switch (option) {
        case 1: return RT64::UserConfiguration::Filtering::Linear;
        case 2: return RT64::UserConfiguration::Filtering::Nearest;
        default: return RT64::UserConfiguration::Filtering::AntiAliasedPixelScaling;
    }
}

// JFG PORT: the Renderer Framerate (rr_option, rr_manual_value) applies from the next start, like the port's Frame Rate:
// changing it while the game ran left the in-between frames uneven until a restart. Taken when the game starts
// (until then, in the launcher, it follows the setting); a later change is saved and the menus offer a restart.
static std::atomic<int> jfg_rr_option_used{ -1 }, jfg_rr_value_used{ 0 };

static ultramodern::renderer::RefreshRate jfg_rr_option(const ultramodern::renderer::GraphicsConfig& config) {
    const int used = jfg_rr_option_used.load();
    return used >= 0 ? (ultramodern::renderer::RefreshRate)used : config.rr_option;
}

static int jfg_rr_value(const ultramodern::renderer::GraphicsConfig& config) {
    return jfg_rr_option_used.load() >= 0 ? jfg_rr_value_used.load() : config.rr_manual_value;
}

// Read by src/jfg/jfg_cpu_layer.cpp (game and present threads): whether RT64 adds in-between frames to the game's own,
// so the CPU-drawn reticles are drawn by the render hook instead.
extern "C" int jfg_renderer_adds_frames(void) {
    const int r = jfg_refresh_override();
    if (r >= 0) return r;
    return jfg_rr_option(ultramodern::renderer::get_graphics_config()) != ultramodern::renderer::RefreshRate::Original;
}

extern "C" int jfg_renderer_rate_needs_restart(void) {
    const int used = jfg_rr_option_used.load();
    if (used < 0) return 0;
    const auto &config = ultramodern::renderer::get_graphics_config();
    if ((int)config.rr_option != used) return 1;
    return (config.rr_option == ultramodern::renderer::RefreshRate::Manual) && (config.rr_manual_value != jfg_rr_value_used.load());
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = jfg_widescreen_active() ? RT64::UserConfiguration::AspectRatio::Expand : to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(jfg_rr_option(config));
    application->userConfig.refreshRateTarget = jfg_rr_value(config);
    // JFG PORT: the "Frame rate" option (Graphics tab, F9 menu) replaces the recomp's own Framerate setting.
    switch (jfg_refresh_override()) {
        case 0: application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original; break;
        case 1:
            // With an Interpolation Limit, RT64's Manual rate (capped at the monitor's rate by RT64 itself).
            if (jfg_interp_limit() > 0) {
                application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Manual;
                application->userConfig.refreshRateTarget = jfg_interp_limit();
            }
            else {
                application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Display;
            }
            break;
        default: break;
    }
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    // JFG PORT: the F9 / Visuals menu's filtering options (also switched while the game runs, see update_render_options).
    application->userConfig.filtering = jfg_rt64_filtering(jfg_output_filtering());
    application->userConfig.threePointFiltering = jfg_texture_three_point() != 0;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
    fprintf(stderr, "[JFG] Initializing RT64 renderer\n");
    static unsigned char dummy_rom_header[0x40];
    recompui::set_render_hooks();
#if !defined(__APPLE__)
    // The inherited developer-only ImGui overlay has D3D12 and Vulkan
    // backends, but no Metal backend. The regular RmlUi launcher and menus
    // have their own renderer and remain enabled on macOS.
    debug_ui::backend::set_render_hooks();
#endif

    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
    appCore.window = window_handle;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    // Force gbi depth branches to prevent LODs from kicking in.
    app->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    app->enhancementConfig.textureLOD.scale = true;
    // Don't try to fix rectangle lower-right coords (kept from the base project, where the fix stretched glyph
    // rects drawn on the edge of a scissor down by a pixel; not checked for Jet Force Gemini).
    app->enhancementConfig.rect.fixRectLR = false;
    // Pick an API if the user has set an override.
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    const auto rt64_setup_result = app->setup(thread_id);
    fprintf(stderr, "[JFG] RT64 setup result: %d, graphics API: %d\n",
        static_cast<int>(rt64_setup_result), static_cast<int>(app->chosenGraphicsAPI));
    setup_result = map_setup_result(rt64_setup_result);
    // Get the API that RT64 chose.
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        app = nullptr;
        return;
    }

    // RT64's scene-specific shader compiler asks for idle-priority workers,
    // but thread priorities are a no-op on Linux. On Steam Deck those workers
    // consequently compete with the game and graphics threads whenever a new
    // scene introduces material combinations, causing severe frame drops and
    // occasional driver termination. Keep compilation disabled during play;
    // everything is drawn with the universal shader (no Jet Force Gemini
    // preload table yet, see preload_common_shaders).
    if (dino::config::running_on_steam_deck()) {
        app->rasterShaderCache->setCompilationEnabled(false);
        app->workloadQueue->ubershadersOnly.store(true, std::memory_order_relaxed);
        fprintf(stderr, "[JFG] Steam Deck runtime shader compilation disabled (universal shader)\n");
    }

    // Set the application's fullscreen state.
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (app->device->getCapabilities().sampleLocations) {
        plume::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::R8G8B8A8_UNORM);
        plume::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(plume::RenderFormat::D32_FLOAT);
        plume::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
    {
        std::lock_guard lock{ active_rt64_context_mutex };
        active_rt64_context = this;
    }
}

RT64Context::~RT64Context() {
    std::lock_guard lock{ active_rt64_context_mutex };
    if (active_rt64_context == this) {
        active_rt64_context = nullptr;
    }
}

void RT64Context::send_dl(const OSTask* task) {
    // While a cutscene is being skipped the display lists of the skipped frames are dropped, so the skip runs at the
    // speed of the game logic alone; the last picture stays up meanwhile.
    if (jfg_skip_render_active()) return;
    check_texture_pack_actions();
    app->state->rsp->reset();
    // Physical addresses in the console's 8 MB of RDRAM (the task's pointers are KSEG0 addresses).
    constexpr uint32_t RdramPhysicalMask = 0x007FFFFFU;
    app->interpreter->loadUCodeGBI(task->t.ucode & RdramPhysicalMask, task->t.ucode_data & RdramPhysicalMask, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & RdramPhysicalMask, 0, true);
}

void RT64Context::preload_common_shaders() {
    if ((app != nullptr) && (app->rasterShaderCache != nullptr) &&
        (app->rasterShaderCache->shaderUber != nullptr)) {
        const auto start = std::chrono::steady_clock::now();
        fprintf(stderr, "[JFG] Finalizing RT64 common shader pipelines\n");
        app->rasterShaderCache->shaderUber->waitForPipelineCreation();

        // Steam Deck: no specialized shaders are compiled ahead of time yet. The base project's table was
        // recorded from Diddy Kong Racing; one recorded from Jet Force Gemini can go here. Until then the Deck
        // draws with the universal shader (see the RT64Context constructor).

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        fprintf(stderr, "[JFG] RT64 common shader pipelines ready in %lld ms\n",
            static_cast<long long>(elapsed.count()));
    }
}

// JFG PORT: the port's render options that can change while the game runs, applied once per screen update: the
// "Widescreen" option of the F9 menu, the presentation mode, the "Frame rate" option and the filtering options.
void RT64Context::update_render_options() {
    {
        static int applied = -1;
        const int want = jfg_widescreen_active();
        if (want != applied) {
            applied = want;
            app->userConfig.aspectRatio = want ? RT64::UserConfiguration::AspectRatio::Expand : RT64::UserConfiguration::AspectRatio::Original;
            app->updateUserConfig(false);
        }
        // RT64 only interpolates when the frame on screen is the one the last workload drew. In Console mode it
        // shows what the VI points at, which with this game's buffering is the frame before, so interpolation
        // stays off. Skip Buffering looks the drawn frame up instead. Console stays the default because it is the
        // mode that was tested with the CPU-drawn reticles (they also show in Skip Buffering). Checked every
        // update because enable_instant_present() sets the mode once at start-up.
        if (std::getenv("JFG_PRESENT_MODE") == nullptr) {
            const int r = jfg_refresh_override();
            const bool interpolate = (r == 1) || ((r == -1) && (ultramodern::renderer::get_graphics_config().rr_option != ultramodern::renderer::RefreshRate::Original));
            const auto wanted = interpolate ? RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering
                                            : RT64::EnhancementConfiguration::Presentation::Mode::Console;
            if (app->enhancementConfig.presentation.mode != wanted) {
                app->enhancementConfig.presentation.mode = wanted;
                app->updateEnhancementConfig();
            }
        }
        // And the "Frame rate" option (Display = interpolate up to the monitor's refresh rate; with an Interpolation
        // Limit, up to that).
        static int applied_rate = -2, applied_limit = -1;
        // Off: Smooth Motion (src/jfg/jfg_frame_rate.cpp) replaced the Renderer Framerate, and a change applies at once.
        constexpr bool JFG_RR_NEEDS_RESTART = false;
        if (JFG_RR_NEEDS_RESTART && (jfg_rr_option_used.load() < 0) && ultramodern::is_game_started()) {
            const auto &started = ultramodern::renderer::get_graphics_config();
            jfg_rr_value_used.store(started.rr_manual_value);
            jfg_rr_option_used.store((int)started.rr_option);
            applied_rate = -2; // apply it below
        }
        const int rate = jfg_refresh_override();
        const int limit = jfg_interp_limit();
        if ((rate != applied_rate) || (limit != applied_limit)) {
            applied_rate = rate;
            applied_limit = limit;
            const auto &config = ultramodern::renderer::get_graphics_config();
            app->userConfig.refreshRate = (rate == 1) ? ((limit > 0) ? RT64::UserConfiguration::RefreshRate::Manual : RT64::UserConfiguration::RefreshRate::Display)
                : (rate == 0) ? RT64::UserConfiguration::RefreshRate::Original : to_rt64(jfg_rr_option(config));
            app->userConfig.refreshRateTarget = ((rate == 1) && (limit > 0)) ? limit : jfg_rr_value(config);
            app->updateUserConfig(false);
        }
        // And the filtering options. Both are read by RT64 every frame, so no framebuffers need to be discarded.
        const RT64::UserConfiguration::Filtering filtering = jfg_rt64_filtering(jfg_output_filtering());
        const bool three_point = jfg_texture_three_point() != 0;
        if ((app->userConfig.filtering != filtering) || (app->userConfig.threePointFiltering != three_point)) {
            app->userConfig.filtering = filtering;
            app->userConfig.threePointFiltering = three_point;
            app->updateUserConfig(false);
        }
    }
}

void RT64Context::update_screen() {
    update_render_options();
    app->updateScreen();
}

void RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    // JFG PORT: a change of the Framerate options alone (Renderer Framerate, Manual Framerate: the Graphics tab applies
    // each step of the slider at once) keeps the framebuffers. Discarding them while the present thread still shows
    // in-between frames made from them crashed in the graphics driver (RT64 Present thread).
    ultramodern::renderer::GraphicsConfig rate_only = old_config;
    rate_only.rr_option = new_config.rr_option;
    rate_only.rr_manual_value = new_config.rr_manual_value;
    app->updateUserConfig(!(rate_only == new_config));

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void RT64Context::enable_instant_present() {
    // Enable the present early presentation mode for minimal latency.
    // JFG PORT: Jet Force Gemini draws some things with the CPU straight into the framebuffer after the RDP is
    // done (mainCPUeffects -> fxOutputLines: the reticles of most weapons, digital read-outs, lines). RT64 only
    // picks such changes up when it processes the VI update, which the PresentEarly mode skips entirely.
    const char *mode_env = std::getenv("JFG_PRESENT_MODE"); // debug: early, skip, console
    auto mode = RT64::EnhancementConfiguration::Presentation::Mode::Console;
    if (mode_env != nullptr && mode_env[0] == 'e') mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
    if (mode_env != nullptr && mode_env[0] == 's') mode = RT64::EnhancementConfiguration::Presentation::Mode::SkipBuffering;
    app->enhancementConfig.presentation.mode = mode;

    app->updateEnhancementConfig();
}

uint32_t RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            else {
                return 1.0f;
            }
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

extern "C" void jfg_texture_pack_set_count(int count); // src/jfg/jfg_texture_packs.cpp: shown in Options -> Textures
// JFG PORT: writes texture_pack/rt64.json listing every image whose name starts with a 16-digit RT64 texture hash
// (as written by "Dump textures"), so a folder of upscaled dumps is a texture pack without any other tool.
// An rt64.json already there is updated, not replaced: its "configuration" (a tool's "defaultShift": Toasty writes
// "none", and without it RT64 samples every texture half a texel off) and each entry's own fields ("shift") stay.
// Entries of textures added to the folder are added, entries whose file is gone are dropped; the file is only written
// when that changes something. Returns the number of textures listed.
static std::string jfg_pack_folder; // the folder pack in use, relative to the app folder ("" = none)
static size_t jfg_write_pack_database(const std::filesystem::path& dir) {
    std::vector<std::pair<std::string, std::string>> files; // (hash, path relative to dir), in folder order
    std::unordered_map<std::string, std::string> file_of;   // hash -> the first file found for it
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file()) continue;
        std::string ext = it->path().extension().string();
        for (char& c : ext) c = (char)std::tolower((unsigned char)c);
        if (ext != ".png" && ext != ".dds") continue;
        const std::string name = it->path().filename().string();
        if (name.size() < 17) continue;
        bool hex = true;
        for (int i = 0; i < 16; i++) hex = hex && std::isxdigit((unsigned char)name[i]);
        // <hash>.png, or with a suffix added by an upscaler (Upscayl writes <name>_upscayl_4x_<model>.png).
        if (!hex || (name[16] != '.' && name[16] != '_' && name[16] != '-' && name[16] != ' ')) continue;
        const auto rel_u8 = std::filesystem::relative(it->path(), dir, ec).generic_u8string();
        const std::string rel(rel_u8.begin(), rel_u8.end());
        std::string hash = name.substr(0, 16);
        for (char& c : hash) c = (char)std::tolower((unsigned char)c);
        if (file_of.emplace(hash, rel).second) files.emplace_back(hash, rel);
    }

    const std::filesystem::path db_path = dir / "rt64.json";
    std::string old_text;
    {
        std::ifstream in(db_path, std::ios::binary);
        if (in) old_text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    nlohmann::json config = { { "autoPath", "rt64" }, { "configurationVersion", 3 }, { "hashVersion", 5 } };
    std::vector<nlohmann::json> entries;
    std::unordered_set<std::string> listed; // hashes with an entry
    if (!old_text.empty()) {
        const nlohmann::json old = nlohmann::json::parse(old_text, nullptr, false);
        if (old.is_discarded() || !old.is_object()) {
            fprintf(stderr, "[JFG] %s could not be read, so a new one is written\n", db_path.string().c_str());
        }
        else {
            const auto old_config = old.find("configuration");
            if (old_config != old.end() && old_config->is_object()) config = *old_config;
            const auto old_textures = old.find("textures");
            if (old_textures != old.end() && old_textures->is_array()) {
                for (const nlohmann::json& entry : *old_textures) {
                    if (!entry.is_object()) continue;
                    std::string hash;
                    const auto hashes = entry.find("hashes");
                    if (hashes != entry.end() && hashes->is_object()) {
                        const auto rt64 = hashes->find("rt64");
                        if (rt64 != hashes->end() && rt64->is_string()) hash = rt64->get<std::string>();
                    }
                    for (char& c : hash) c = (char)std::tolower((unsigned char)c);
                    const auto path_it = entry.find("path");
                    const std::string path = path_it != entry.end() && path_it->is_string() ? path_it->get<std::string>() : "";
                    const auto file = file_of.find(hash);
                    if (path.empty() || std::filesystem::is_regular_file(dir / std::filesystem::u8path(path), ec)) {
                        entries.push_back(entry); // its file is there (or RT64 finds it by its hash): kept as it is
                    }
                    else if (file != file_of.end()) {
                        nlohmann::json moved = entry; // the file has another name now (.png -> .dds)
                        moved["path"] = file->second;
                        entries.push_back(std::move(moved));
                    }
                    else {
                        continue; // its file is gone
                    }
                    if (!hash.empty()) listed.insert(hash);
                }
            }
        }
    }
    for (const auto& [hash, rel] : files) {
        if (listed.insert(hash).second) {
            entries.push_back({ { "path", rel }, { "hashes", { { "rt64", hash } } } });
        }
    }

    std::string text = "{\n  \"configuration\": " + config.dump() + ",\n  \"textures\": [\n";
    for (size_t i = 0; i < entries.size(); i++) {
        text += "    " + entries[i].dump() + (i + 1 < entries.size() ? ",\n" : "\n");
    }
    text += "  ]\n}\n";
    if (text != old_text) {
        std::ofstream out(db_path, std::ios::binary);
        out << text;
    }
    return files.size();
}

void RT64Context::check_texture_pack_actions() {
    bool packs_changed = false;

    // JFG PORT: texture dumping and the folder pack (F9 -> Textures). Done here, on the thread that draws, because
    // RT64 dumps while it processes display lists.
    {
        const bool dump = jfg_texture_dump_active() != 0;
        // With "One folder per level" the folder follows the level: a sub-folder named after it, with its own record of
        // what has been dumped (so a texture is saved again in every level that uses it).
        char sub[256];
        jfg_texture_dump_folder(sub, sizeof(sub));
        static std::string dump_sub;
        if (dump != app->state->dumpingTexturesPng || (dump && dump_sub != sub)) {
            if (dump) {
                dump_sub = sub;
                std::filesystem::path dir = dino::config::get_app_folder_path() / "texture_dump";
                if (!dump_sub.empty()) dir /= std::filesystem::u8path(dump_sub);
                std::error_code ec;
                std::filesystem::create_directories(dir, ec);
                app->state->dumpingTexturesDirectory = dir;
                // Textures already in the folder (an earlier session) are not written again; JFG_DEBUG_DUMP_ALL=1 keeps
                // dumping textures the game redraws on most frames (dynamic_sources.txt) instead of moving them to _dynamic.
                app->state->textureManager.resetDump(dir);
                app->state->dumpingTexturesSkipDynamic = std::getenv("JFG_DEBUG_DUMP_ALL") == nullptr;
                fprintf(stderr, "[JFG] dumping textures to %s\n", dir.string().c_str());
            }
            else {
                app->state->dumpingTexturesDirectory.clear();
            }
            app->state->dumpingTexturesPng = dump;
        }
        if (dump) {
            // The level or screen this display list draws, stamped on each line of draw_trace.txt.
            char scene[64];
            jfg_texture_dump_scene(scene, sizeof(scene));
            app->state->traceScene = scene;
        }
        static int pack_generation = -1;
        char rel[512];
        const int generation = jfg_texture_pack_current(rel, sizeof(rel));
        if (generation != pack_generation) {
            pack_generation = generation;
            jfg_pack_folder = rel;
            packs_changed = true;
        }
    }
    TexturePackAction cur_action;
    while (texture_pack_action_queue.try_dequeue(cur_action)) {
        std::visit(overloaded{
            [&](TexturePackDisableAction &to_disable) {
                enabled_texture_packs.erase(to_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackEnableAction &to_enable) {
                enabled_texture_packs.insert(to_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryDisableAction &to_override_disable) {
                secondary_disabled_texture_packs.insert(to_override_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryEnableAction &to_override_enable) {
                secondary_disabled_texture_packs.erase(to_override_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackUpdateAction &) {
                packs_changed = true;
            }
        }, cur_action);
    }

    // If any packs were disabled, unload all packs and load all the active ones.
    if (packs_changed) {
        // Sort the enabled texture packs in reverse order so that earlier ones override later ones.
        std::vector<std::string> sorted_texture_packs{};
        sorted_texture_packs.reserve(enabled_texture_packs.size());
        for (const std::string& mod : enabled_texture_packs) {
            if (!secondary_disabled_texture_packs.contains(mod)) {
                sorted_texture_packs.emplace_back(mod);
            }
        }

        std::sort(sorted_texture_packs.begin(), sorted_texture_packs.end(),
            [](const std::string& lhs, const std::string& rhs) {
                return recomp::mods::get_mod_order_index(lhs) > recomp::mods::get_mod_order_index(rhs);
            }
        );

        // Build the path list from the sorted mod list.
        std::vector<RT64::ReplacementDirectory> replacement_directories;
        replacement_directories.reserve(enabled_texture_packs.size() + 1);
        for (const std::string &mod_id : sorted_texture_packs) {
            replacement_directories.emplace_back(RT64::ReplacementDirectory(recomp::mods::get_mod_filename(mod_id)));
        }

        // JFG PORT: the folder pack chosen in Options -> Textures (or with F6), after the installed packs (earlier
        // entries win).
        const std::filesystem::path pack_dir = dino::config::get_app_folder_path() / std::filesystem::u8path(jfg_pack_folder);
        std::error_code pack_ec;
        jfg_texture_pack_set_count(-1);
        // The extension in any case (Pack.RTZ too), as the pack list takes it (jfg_texture_packs.cpp).
        const auto pack_ext_u8 = pack_dir.extension().u8string();
        std::string pack_ext(pack_ext_u8.begin(), pack_ext_u8.end());
        for (char& c : pack_ext) c = (char)std::tolower((unsigned char)c);
        const bool pack_is_rtz = !jfg_pack_folder.empty() && pack_ext == ".rtz" && std::filesystem::is_regular_file(pack_dir, pack_ec);
        if (!jfg_pack_folder.empty() && (pack_is_rtz || std::filesystem::is_directory(pack_dir, pack_ec))) {
            size_t count = 0;
            if (pack_is_rtz) {
                // A packed pack (.rtz: a zip with rt64.json at its root) is used as it is; only its textures are counted.
#ifdef _WIN32
                std::FILE* zip_file = _wfopen(pack_dir.c_str(), L"rb");
#else
                std::FILE* zip_file = std::fopen(pack_dir.c_str(), "rb");
#endif
                if (zip_file != nullptr) {
                    mz_zip_archive zip = {};
                    if (mz_zip_reader_init_cfile(&zip, zip_file, 0, 0)) {
                        // The images named by a texture hash (not rt64.json, pack.json or the pack's image).
                        const mz_uint files = mz_zip_reader_get_num_files(&zip);
                        for (mz_uint i = 0; i < files; i++) {
                            char name[512] = {};
                            mz_zip_reader_get_filename(&zip, i, name, sizeof(name));
                            const char* base = std::strrchr(name, '/');
                            base = base != nullptr ? base + 1 : name;
                            size_t hex = 0;
                            while (hex < 16 && std::isxdigit((unsigned char)base[hex])) hex++;
                            if (hex == 16) count++;
                        }
                        mz_zip_reader_end(&zip);
                    }
                    std::fclose(zip_file);
                }
            }
            else {
                count = jfg_write_pack_database(pack_dir);
            }
            fprintf(stderr, "[JFG] texture pack %s: %zu textures\n", jfg_pack_folder.c_str(), count);
            jfg_texture_pack_set_count((int)count);
            if (count > 0) {
                replacement_directories.emplace_back(RT64::ReplacementDirectory(pack_dir));
            }
        }

        if (!replacement_directories.empty()) {
            app->textureCache->loadReplacementDirectories(replacement_directories);
            // JFG PORT: how many textures RT64 found in each pack (by hash, in its rt64.json), so a pack whose paths
            // don't match its files shows 0. Packs inside mods (.nrm) and .rtz packs alike.
            std::unique_lock lock(app->textureCache->textureMapMutex);
            const auto &resolved = app->textureCache->textureMap.replacementMap.fileSystemResolvedPaths;
            for (size_t i = 0; (i < replacement_directories.size()) && (i < resolved.size()); i++) {
                fprintf(stderr, "[JFG] texture pack loaded: %s, %zu textures\n",
                    replacement_directories[i].dirOrZipPath.filename().string().c_str(), resolved[i].size());
            }
        }
        else {
            app->textureCache->clearReplacementDirectories();
        }
    }
}

RT64::UserConfiguration::Antialiasing RT64MaxMSAA() {
    return device_max_msaa;
}

std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<RT64Context>(rdram, window_handle, developer_mode);
}

bool RT64SamplePositionsSupported() {
    return sample_positions_supported;
}

bool RT64HighPrecisionFBEnabled() {
    return high_precision_fb_enabled;
}

void preload_common_shaders() {
    std::lock_guard lock{ active_rt64_context_mutex };
    if (active_rt64_context != nullptr) {
        active_rt64_context->preload_common_shaders();
    }
}

void trigger_texture_pack_update() {
    texture_pack_action_queue.enqueue(TexturePackUpdateAction{});
}

void enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackEnableAction{mod.manifest.mod_id});

    // Check for the texture pack enabled config option.
    const recomp::mods::ConfigSchema& config_schema = context.get_mod_config_schema(mod.manifest.mod_id);
    auto find_it = config_schema.options_by_id.find(special_option_texture_pack_enabled);
    if (find_it != config_schema.options_by_id.end()) {
        const recomp::mods::ConfigOption& config_option = config_schema.options[find_it->second];

        if (is_texture_pack_enable_config_option(config_option, false)) {
            recomp::mods::ConfigValueVariant value_variant = context.get_mod_config_value(mod.manifest.mod_id, config_option.id);
            uint32_t value;
            if (uint32_t* value_ptr = std::get_if<uint32_t>(&value_variant)) {
                value = *value_ptr;
            }
            else {
                value = 0;
            }

            if (value) {
                secondary_enable_texture_pack(mod.manifest.mod_id);
            }
            else {
                secondary_disable_texture_pack(mod.manifest.mod_id);
            }
        }
    }
}

void disable_texture_pack(const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackDisableAction{mod.manifest.mod_id});
}

void secondary_enable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryEnableAction{mod_id});
}

void secondary_disable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryDisableAction{mod_id});
}


// HD texture enable option. Must be an enum with two options.
// The first option is treated as disabled and the second option is treated as enabled.
bool is_texture_pack_enable_config_option(const recomp::mods::ConfigOption& option, bool show_errors) {
    if (option.id == special_option_texture_pack_enabled) {
        if (option.type != recomp::mods::ConfigOptionType::Enum) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + special_option_texture_pack_enabled + "\"), but the config option is not an enum.").c_str());
            }
            return false;
        }

        const recomp::mods::ConfigOptionEnum &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
        if (option_enum.options.size() != 2) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + special_option_texture_pack_enabled + "\"), but the config option doesn't have exactly 2 values.").c_str());
            }
            return false;
        }

        return true;
    }
    return false;
}

}
