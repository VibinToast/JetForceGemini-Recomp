#define HLSL_CPU
#include "hle/rt64_application.h"
#include "renderer.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <variant>

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

// Shader descriptions observed from boot through the Ancient Lake title
// attract scene using the Steam Deck graphics preset (2x, no MSAA, LDR). This
// table contains renderer state only; it has no textures, display-list data,
// or other ROM-derived content. Entries that do not appear in a later scene
// simply remain cached while RT64 falls back to the universal shader for any
// description that is not listed here.
static constexpr std::array<RT64::ShaderDescription, 74> deck_attract_shader_preload = {{
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229010979u, 4281335807u}, {5259840u, 3087u}, {.value = 9043979u}},
    RT64::ShaderDescription{{4229010979u, 4281335807u}, {5259840u, 3087u}, {.value = 62538763u}},
    RT64::ShaderDescription{{4244635647u, 4294833915u}, {5260096u, 8399935u}, {.value = 134217739u}},
    RT64::ShaderDescription{{4244635647u, 4294833915u}, {252329984u, 8924223u}, {.value = 134217739u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4244609023u, 4281138815u}, {5262808u, 535567u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4244609023u, 4281138815u}, {5262808u, 535567u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229011116u, 4043308600u}, {3356569720u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229011116u, 4043308600u}, {3356509656u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229044396u, 4027579967u}, {1067480u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356511704u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4233500332u, 288161407u}, {5259840u, 3087u}, {.value = 80434187u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3405922304u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 196756494u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356511824u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4233526787u, 1326348287u}, {3356565624u, 1575951u}, {.value = 134217742u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356511704u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4244635647u, 4294867260u}, {5260096u, 527375u}, {.value = 134217738u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356511824u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4230112940u, 269521471u}, {1066456u, 1584143u}, {.value = 143523850u}},
    RT64::ShaderDescription{{4233500332u, 288161407u}, {5259840u, 3087u}, {.value = 26939403u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4233526787u, 1326348799u}, {1067480u, 1575951u}, {.value = 134217738u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356514392u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356565624u, 1584143u}, {.value = 196756490u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1067480u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356508736u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229044735u, 4279238207u}, {1067512u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356569720u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356569720u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356569720u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4229010979u, 4281335807u}, {5259840u, 11279u}, {.value = 143261707u}},
    RT64::ShaderDescription{{4229010979u, 4281335807u}, {5259840u, 11279u}, {.value = 196756491u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 214651918u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229044735u, 4279238207u}, {1067632u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 161157134u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4229070372u, 4294964217u}, {5261904u, 535567u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4233526956u, 1157527355u}, {5579896u, 527375u}, {.value = 134217738u}},
    RT64::ShaderDescription{{4229011116u, 4043308600u}, {3356509656u, 1584143u}, {.value = 143348750u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 161157130u}},
    RT64::ShaderDescription{{4229273252u, 821952056u}, {1122424u, 1584143u}, {.value = 196756494u}},
    RT64::ShaderDescription{{4233526788u, 536672248u}, {3356565624u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510680u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3405922304u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356569720u, 1584143u}, {.value = 143261710u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356566064u, 1584143u}, {.value = 196756494u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 196756494u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 143261706u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356510800u, 1584143u}, {.value = 196756490u}},
    RT64::ShaderDescription{{4229010979u, 4281335807u}, {5259720u, 535567u}, {.value = 214651914u}},
    RT64::ShaderDescription{{4229070508u, 4027576895u}, {3356511824u, 1584143u}, {.value = 196756494u}},
}};

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
            return RT64::UserConfiguration::AspectRatio::Expand;
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

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    application->userConfig.filtering = RT64::UserConfiguration::Filtering::AntiAliasedPixelScaling;
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
    fprintf(stderr, "[DKR] Initializing RT64 renderer\n");
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
    // Don't try to fix rectangle lower-right coords. This causes Dino Planet's font window scissors
    // to stretch glyph rects down by a pixel when they are drawn right on the edge of the scissor.
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
    fprintf(stderr, "[DKR] RT64 setup result: %d, graphics API: %d\n",
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
    // the launcher preflight temporarily enables it for a bounded table and
    // all unlisted descriptions retain the universal-shader fallback.
    if (dino::config::running_on_steam_deck()) {
        app->rasterShaderCache->setCompilationEnabled(false);
        app->workloadQueue->ubershadersOnly.store(true, std::memory_order_relaxed);
        fprintf(stderr, "[DKR] Steam Deck runtime shader compilation disabled pending launcher preflight\n");
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
    check_texture_pack_actions();
    app->state->rsp->reset();
    constexpr uint32_t DkrRdramMask = 0x007FFFFFU;
    app->interpreter->loadUCodeGBI(task->t.ucode & DkrRdramMask, task->t.ucode_data & DkrRdramMask, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & DkrRdramMask, 0, true);
}

void RT64Context::preload_common_shaders() {
    if ((app != nullptr) && (app->rasterShaderCache != nullptr) &&
        (app->rasterShaderCache->shaderUber != nullptr)) {
        const auto start = std::chrono::steady_clock::now();
        fprintf(stderr, "[DKR] Finalizing RT64 common shader pipelines\n");
        app->rasterShaderCache->shaderUber->waitForPipelineCreation();

        if (dino::config::running_on_steam_deck()) {
            const uint32_t shader_count_before = app->rasterShaderCache->shaderCount();
            fprintf(stderr, "[DKR] Preloading %zu Steam Deck attract-scene shader pipelines\n",
                deck_attract_shader_preload.size());

            app->rasterShaderCache->setCompilationEnabled(true);
            for (const RT64::ShaderDescription &shader : deck_attract_shader_preload) {
                app->rasterShaderCache->submit(shader);
            }
            app->rasterShaderCache->waitForAll();
            app->rasterShaderCache->setCompilationEnabled(false);

            const uint32_t shader_count_after = app->rasterShaderCache->shaderCount();
            app->workloadQueue->ubershadersOnly.store(false, std::memory_order_relaxed);
            fprintf(stderr,
                "[DKR] Steam Deck specialized shader preflight ready (%u new); universal fallback retained\n",
                shader_count_after - shader_count_before);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        fprintf(stderr, "[DKR] RT64 common shader pipelines ready in %lld ms\n",
            static_cast<long long>(elapsed.count()));
    }
}

void RT64Context::update_game_aspect_override() {
    // The track-select backdrop is an authored 320-pixel-wide scrolling page.
    // The post-race and results layouts likewise draw only a 4:3 page while the
    // race scene remains visible behind it. Expanding any of those projections
    // exposes content outside the page, so render them at the original aspect.
    // Normal race gameplay continues to use the user's configured aspect ratio.
    constexpr size_t DkrTrackMenuFlagOffset = 0x000E0EFC;
    constexpr size_t DkrCurrentMenuIdOffset = 0x000DF9F0;
    constexpr size_t DkrGameModeOffset = 0x00123A6C;
    constexpr size_t DkrPostRaceViewportOffset = 0x00123A96;
    constexpr int32_t DkrGameModeMenu = 1;
    constexpr int32_t DkrMenuResults = 17;

    const uint8_t *rdram = app->core.RDRAM;
    const bool track_menu_active =
        *reinterpret_cast<const int32_t *>(rdram + DkrTrackMenuFlagOffset) != 0;
    // N64Recomp keeps byte-addressed RDRAM word-swapped on little-endian hosts.
    const bool postrace_active = rdram[DkrPostRaceViewportOffset ^ 3U] != 0;
    const int32_t game_mode = *reinterpret_cast<const int32_t *>(rdram + DkrGameModeOffset);
    const int32_t current_menu_id = *reinterpret_cast<const int32_t *>(rdram + DkrCurrentMenuIdOffset);
    if (std::getenv("DKR_SCENE_TRACE") != nullptr) {
        static int32_t previous_game_mode = INT32_MIN;
        static int32_t previous_menu_id = INT32_MIN;
        if ((game_mode != previous_game_mode) || (current_menu_id != previous_menu_id)) {
            std::fprintf(stderr, "[DKR SCENE] gameMode=%d menuId=%d\n", game_mode, current_menu_id);
            previous_game_mode = game_mode;
            previous_menu_id = current_menu_id;
        }
    }
    const bool results_menu_active =
        (game_mode == DkrGameModeMenu) && (current_menu_id == DkrMenuResults);
    const bool original_aspect_active =
        track_menu_active || postrace_active || results_menu_active;

    if (original_aspect_active == game_aspect_override) {
        return;
    }

    game_aspect_override = original_aspect_active;
    if (game_aspect_override) {
        app->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
    }
    else {
        app->userConfig.aspectRatio = to_rt64(ultramodern::renderer::get_graphics_config().ar_option);
    }

    app->updateUserConfig(false);
    fprintf(stderr, "[DKR] Authored 4:3 aspect override %s\n",
        game_aspect_override ? "enabled" : "disabled");
}

void RT64Context::update_screen() {
    update_game_aspect_override();
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

    if (game_aspect_override) {
        app->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
    }

    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void RT64Context::enable_instant_present() {
    // Enable the present early presentation mode for minimal latency.
    app->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;

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

void RT64Context::check_texture_pack_actions() {
    bool packs_changed = false;
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
        replacement_directories.reserve(enabled_texture_packs.size());
        for (const std::string &mod_id : sorted_texture_packs) {
            replacement_directories.emplace_back(RT64::ReplacementDirectory(recomp::mods::get_mod_filename(mod_id)));
        }

        if (!replacement_directories.empty()) {
            app->textureCache->loadReplacementDirectories(replacement_directories);
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
