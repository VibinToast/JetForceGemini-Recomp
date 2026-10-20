extern "C" int jfg_skip_render_active(void); // src/jfg/jfg_cutscene_skip.cpp
extern "C" int jfg_widescreen_active(void);  // src/jfg/jfg_visuals.cpp: the "Widescreen" option of the F9 menu
extern "C" int jfg_texture_dump_active(void);
extern "C" void jfg_texture_dump_scene(char* buf, int size); // src/jfg/jfg_texture_packs.cpp: the scene tag of draw_trace.txt
extern "C" void jfg_texture_dump_folder(char* buf, int size); // src/jfg/jfg_texture_packs.cpp: "One folder per level"  // src/jfg/jfg_texture_packs.cpp: "Dump textures" (F9 -> Textures)
extern "C" int jfg_texture_pack_current(char* buf, int size); // the chosen pack folder (relative, "" = none); returns a change counter
extern "C" int jfg_texture_three_point(void); // src/jfg/jfg_visuals.cpp: "Texture filtering" (1 = the N64's three-point filter)
extern "C" int jfg_output_filtering(void);    // src/jfg/jfg_visuals.cpp: "Picture scaling" (0 anti-aliased pixel scaling, 1 linear, 2 nearest)
extern "C" int jfg_refresh_override(void);   // src/jfg/jfg_frame_rate.cpp: "Frame rate" option; 0 = Original, 1 = Display (-1 would use rr_option)
#define HLSL_CPU
#include "hle/rt64_application.h"
#include "renderer.hpp"

#include <array>
#include <cctype>
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
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    // JFG PORT: the "Frame rate" option (Graphics tab, F9 menu) replaces the recomp's own Framerate setting.
    switch (jfg_refresh_override()) {
        case 0: application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Original; break;
        case 1: application->userConfig.refreshRate = RT64::UserConfiguration::RefreshRate::Display; break;
        default: break;
    }
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;
    // JFG PORT: the F9 / Visuals menu's filtering options (also switched while the game runs, see update_game_aspect_override).
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
    // the launcher preflight temporarily enables it for a bounded table and
    // all unlisted descriptions retain the universal-shader fallback.
    if (dino::config::running_on_steam_deck()) {
        app->rasterShaderCache->setCompilationEnabled(false);
        app->workloadQueue->ubershadersOnly.store(true, std::memory_order_relaxed);
        fprintf(stderr, "[JFG] Steam Deck runtime shader compilation disabled pending launcher preflight\n");
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
    constexpr uint32_t DkrRdramMask = 0x007FFFFFU;
    app->interpreter->loadUCodeGBI(task->t.ucode & DkrRdramMask, task->t.ucode_data & DkrRdramMask, true);
    app->processDisplayLists(app->core.RDRAM, task->t.data_ptr & DkrRdramMask, 0, true);
}

void RT64Context::preload_common_shaders() {
    if ((app != nullptr) && (app->rasterShaderCache != nullptr) &&
        (app->rasterShaderCache->shaderUber != nullptr)) {
        const auto start = std::chrono::steady_clock::now();
        fprintf(stderr, "[JFG] Finalizing RT64 common shader pipelines\n");
        app->rasterShaderCache->shaderUber->waitForPipelineCreation();

        if (dino::config::running_on_steam_deck()) {
            const uint32_t shader_count_before = app->rasterShaderCache->shaderCount();
            fprintf(stderr, "[JFG] Preloading %zu Steam Deck attract-scene shader pipelines\n",
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
                "[JFG] Steam Deck specialized shader preflight ready (%u new); universal fallback retained\n",
                shader_count_after - shader_count_before);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        fprintf(stderr, "[JFG] RT64 common shader pipelines ready in %lld ms\n",
            static_cast<long long>(elapsed.count()));
    }
}

void RT64Context::update_game_aspect_override() {
    // JFG PORT: everything below reads Diddy Kong Racing variables at fixed RDRAM addresses to switch the aspect
    // ratio for some DKR menus. Those addresses hold unrelated data in Jet Force Gemini. Instead, follow the
    // "Widescreen" option of the F9 menu (it can change while the game runs).
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
        // And the "Frame rate" option (Display = interpolate up to the monitor's refresh rate).
        static int applied_rate = -2;
        const int rate = jfg_refresh_override();
        if (rate != applied_rate) {
            applied_rate = rate;
            const auto &config = ultramodern::renderer::get_graphics_config();
            app->userConfig.refreshRate = (rate == 1) ? RT64::UserConfiguration::RefreshRate::Display
                : (rate == 0) ? RT64::UserConfiguration::RefreshRate::Original : to_rt64(config.rr_option);
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
    return;

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
    if (std::getenv("JFG_SCENE_TRACE") != nullptr) {
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
    fprintf(stderr, "[JFG] Authored 4:3 aspect override %s\n",
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
        const bool pack_is_rtz = !jfg_pack_folder.empty() && pack_dir.extension() == ".rtz" && std::filesystem::is_regular_file(pack_dir, pack_ec);
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
                        count = mz_zip_reader_get_num_files(&zip);
                        if (count > 0) count--; // rt64.json
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
