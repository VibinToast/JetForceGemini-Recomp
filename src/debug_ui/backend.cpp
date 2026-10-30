#include "backend.hpp"

#include <atomic>

extern "C" void jfg_tools_key(int scancode, int down, int repeat); // JFG PORT: src/jfg/jfg_ui_tools.cpp

#ifdef _WIN32
#include <SDL_video.h>
#else
#include <SDL_video.h>
#endif
#include <SDL_events.h>

#include "plume_vulkan.h"
#include "common/rt64_user_configuration.h"

#include "concurrentqueue.h"

#include "imgui/imgui.h"
#include "imgui/imgui_internal.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_vulkan.h"

#if defined(_WIN32)
#   include "imgui/backends/imgui_impl_dx12.h"
#   include "imgui/backends/imgui_impl_win32.h"
#   include "utf8conv/utf8conv.h"
#endif

#if defined(_WIN32)
#include "plume_d3d12.h"
#endif

#include "renderer/hooks.hpp"
#include "config/config.hpp"
#include "runtime/gfx.hpp"

namespace jfg { bool cpu_layer_build(ImDrawList* list); void cpu_layer_trace_drawn(bool drawn); } // src/jfg/jfg_cpu_layer.cpp
namespace jfg::overlay { bool build(ImDrawList* list, ImVector<ImTextureData*>& textures); } // src/jfg/jfg_overlay.cpp

namespace dino::debug_ui::backend {

struct VulkanContext {
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;

    ~VulkanContext() {
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
        }

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
        }
    }
};

// We must use our own ImGui context to avoid conflicting with the RT64 inspector where possible
ImGuiContext *jfg_imgui_ctx;
bool b_is_open = false;
bool b_in_ui_frame = false;
// JFG PORT: lets the game-side code draw a passive overlay (the "press F9" hint) while the debug UI is closed.
static bool b_overlay = false;
// JFG PORT: F5 asks the game to go back to the title screen (taken by take_reset_request() on the game thread).
static std::atomic<bool> b_reset_requested{ false };
// JFG PORT: F6 switches to the next texture pack (taken by take_next_pack_request()).
static std::atomic<bool> b_next_pack_requested{ false };
// JFG PORT: F7 switches the fly camera on / off (taken by take_fly_request()).
static std::atomic<bool> b_fly_requested{ false };

static ImGuiContext *prev_ctx;
static std::unique_ptr<plume::RenderDescriptorSet> descriptor_set;
#if defined(_WIN32)
// JFG PORT: ImGui's D3D12 textures (the font, and the mod overlay's pictures: src/jfg/jfg_overlay.cpp) each get a
// descriptor set of their own; one shared slot made every new texture replace the font. (One set of many slots drew
// nothing past its first.)
static std::vector<std::unique_ptr<plume::RenderDescriptorSet>> d3d12_texture_sets;
static plume::D3D12Device* d3d12_imgui_device = nullptr;
#endif
static std::unique_ptr<plume::RenderCommandQueue> d3d12_upload_queue; // JFG PORT: ImGui's DX12 backend uploads its textures through a queue of its own
static std::mutex frame_mutex;
static moodycamel::ConcurrentQueue<SDL_Event> event_queue{};
static std::unique_ptr<VulkanContext> vulkanContext;
// JFG PORT: the API RT64 made its device with, set when RT64 starts. It can differ from the setting: Auto, a D3D12
// device that fails to start, or RT64's driver workarounds (old Intel 6th-gen, NVIDIA and AMD drivers on D3D12) all
// end up on Vulkan, and the hooks would treat the Vulkan device as a D3D12 one.
static RT64::UserConfiguration::GraphicsAPI active_graphics_api = RT64::UserConfiguration::GraphicsAPI::OptionCount;

static RT64::UserConfiguration::GraphicsAPI get_graphics_api() {
    if (active_graphics_api != RT64::UserConfiguration::GraphicsAPI::OptionCount) {
        return active_graphics_api;
    }

    const ultramodern::renderer::GraphicsConfig &config = ultramodern::renderer::get_graphics_config();

    switch (config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            return RT64::UserConfiguration::GraphicsAPI::D3D12;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            return RT64::UserConfiguration::GraphicsAPI::Vulkan;
        case ultramodern::renderer::GraphicsApi::Auto: {
#if defined(_WIN32)
            return RT64::UserConfiguration::GraphicsAPI::D3D12;
#elif defined(__gnu_linux__) || defined(__ANDROID__)
            return RT64::UserConfiguration::GraphicsAPI::Vulkan;
#elif defined(__APPLE__)
            // TODO: Add MoltenVK option for Mac?
            return RT64::UserConfiguration::GraphicsAPI::Vulkan;
#else
            static_assert(false && "Unimplemented")
#endif
        }
        default:
            assert(false && "Unknown Graphics API.");
            return RT64::UserConfiguration::GraphicsAPI::OptionCount;
    }
}

bool is_open() { return b_is_open; }
// JFG PORT: the lighting tool (F12) is an ImGui window of its own. While it is open the UI frame runs and the game keeps
// its controls; Alt pressed on its own hands the mouse and keyboard to the tool (to click and type in it) and back.
// A controller always plays the game.
static std::atomic<bool> b_lighting_open{ false };
static std::atomic<bool> b_lighting_playing{ true };
bool lighting_tool_open() { return b_lighting_open.load(); }
bool lighting_tool_playing() { return b_lighting_playing.load(); }
void set_lighting_tool_playing(bool playing) { b_lighting_playing = playing; }
void set_lighting_tool_open(bool open) { b_lighting_open = open; }
static bool ui_wanted() { return b_is_open || b_overlay || b_lighting_open.load(); }
bool in_ui_frame() { return b_in_ui_frame; }
void set_overlay(bool enabled) { b_overlay = enabled; }
bool take_reset_request() { return b_reset_requested.exchange(false); }
bool take_next_pack_request() { return b_next_pack_requested.exchange(false); }
bool take_fly_request() { return b_fly_requested.exchange(false); }

// JFG PORT: F9 (or `) opens / closes the debug window. F10's tools are RmlUi (src/jfg/jfg_ui_tools.cpp, jfg_tools_key).
static void handle_toggle_key(const SDL_Event& event) {
    if (event.type != SDL_KEYDOWN || event.key.repeat) return;
    const SDL_Scancode code = event.key.keysym.scancode;
    if (code == SDL_Scancode::SDL_SCANCODE_GRAVE || code == SDL_Scancode::SDL_SCANCODE_F9) b_is_open = !b_is_open;
    // F12: open (the game keeps the controls) or close the lighting tool.
    if (code == SDL_Scancode::SDL_SCANCODE_F12) {
        b_lighting_open = !b_lighting_open;
        b_lighting_playing = true;
    }
}

void begin() {
    if (!jfg_imgui_ctx->Initialized) return;

    if (!ui_wanted()) {
        // Still process the event queue but don't send any to ImGui
        SDL_Event event{};
        while (event_queue.try_dequeue(event)) {
            handle_toggle_key(event);
        }
    }

    if (!ui_wanted()) {
        // Still not open even after handling key events
        return;
    }

    assert(!b_in_ui_frame);
    b_in_ui_frame = true;

    frame_mutex.lock();

    prev_ctx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(jfg_imgui_ctx);

    SDL_Event event{};
    while (event_queue.try_dequeue(event)) {
        handle_toggle_key(event);
        ImGui_ImplSDL2_ProcessEvent(&event);
    }

    // JFG PORT: an overlay-only frame must not take over the mouse cursor.
    if (b_is_open || (b_lighting_open && !b_lighting_playing)) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    // ... nor clicks: while the game has the mouse, the lighting tool's window ignores it.
    if (!b_is_open && b_lighting_open && b_lighting_playing) ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NoMouse;
    else ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;

    if (!ui_wanted()) {
        // Closed after handling key events
        b_in_ui_frame = false;
        ImGui::SetCurrentContext(prev_ctx);
        frame_mutex.unlock();
        return;
    }

    ImGui_ImplSDL2_NewFrame();

    switch (get_graphics_api()) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: {
#ifdef _WIN32
            ImGui_ImplDX12_NewFrame();
#else
            assert(false && "Unsupported Graphics API.");
            return;
#endif
            break;
        }
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: {
            ImGui_ImplVulkan_NewFrame();
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            break;
    }

    ImGui::NewFrame();
}

void end() {
    if (b_in_ui_frame) {
        assert(jfg_imgui_ctx->Initialized);

        ImGui::Render();
        ImGui::SetCurrentContext(prev_ctx);

        frame_mutex.unlock();

        b_in_ui_frame = false;
    }
}

static int sdl_event_filter(void *userdata, SDL_Event *event) {
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_Scancode::SDL_SCANCODE_F5 && !event->key.repeat) {
        b_reset_requested = true;
    }
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_Scancode::SDL_SCANCODE_F6 && !event->key.repeat) {
        b_next_pack_requested = true;
    }
    if (event->type == SDL_KEYDOWN && event->key.keysym.scancode == SDL_Scancode::SDL_SCANCODE_F7 && !event->key.repeat) {
        b_fly_requested = true;
    }
    // JFG PORT: Alt pressed and released with no other key between, while the lighting tool is open: the mouse and
    // keyboard to the tool and back.
    if ((event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) && !event->key.repeat) {
        static bool alt_alone = false;
        const SDL_Scancode code = event->key.keysym.scancode;
        const bool alt = (code == SDL_SCANCODE_LALT) || (code == SDL_SCANCODE_RALT);
        if (event->type == SDL_KEYDOWN) alt_alone = alt;
        else if (alt && alt_alone) {
            alt_alone = false;
            if (b_lighting_open) b_lighting_playing = !b_lighting_playing;
        }
    }
    // JFG PORT: F10 (show / hide) and Alt (mouse and keyboard to the game and back) for F10's tools.
    if (event->type == SDL_KEYDOWN || event->type == SDL_KEYUP) {
        jfg_tools_key((int)event->key.keysym.scancode, event->type == SDL_KEYDOWN ? 1 : 0, event->key.repeat ? 1 : 0);
    }
    event_queue.enqueue(*event);

    return 1;
}

static void checkVulkanResult(VkResult res) {
    if (res == 0) {
        return;
    }

    fprintf(stderr, "Debug UI ImGui Vulkan Backend failed with error code 0x%X.\n", res);
}

static void rt64_init_hook(plume::RenderInterface* _interface, plume::RenderDevice* device) {
    IMGUI_CHECKVERSION();

    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();

    jfg_imgui_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(jfg_imgui_ctx);

    static std::string imgui_ini_path = (dino::config::get_app_folder_path() / "imgui.ini").string();
    jfg_imgui_ctx->IO.IniFilename = imgui_ini_path.c_str();

    ImGui_ImplSDL2_InitForOther(dino::runtime::get_window());

    // JFG PORT: ask the render interface which API it is (see active_graphics_api).
    switch (_interface->getCapabilities().shaderFormat) {
        case plume::RenderShaderFormat::DXIL:
            active_graphics_api = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case plume::RenderShaderFormat::SPIRV:
            active_graphics_api = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        default:
            active_graphics_api = RT64::UserConfiguration::GraphicsAPI::OptionCount;
            break;
    }

    switch (get_graphics_api()) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: {
#ifdef _WIN32
            plume::D3D12Device *interface_device = static_cast<plume::D3D12Device *>(device);
            plume::RenderDescriptorRange descriptor_range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
            descriptor_set = interface_device->createDescriptorSet(plume::RenderDescriptorSetDesc(&descriptor_range, 1));

            plume::D3D12DescriptorSet *interface_descriptor_set = static_cast<plume::D3D12DescriptorSet *>(descriptor_set.get());
            d3d12_imgui_device = interface_device;
            d3d12_texture_sets.clear();
            // JFG PORT: ImGui 1.92+ (upstream RT64's) takes an init struct and needs a command queue for texture uploads.
            d3d12_upload_queue = interface_device->createCommandQueue(plume::RenderCommandListType::DIRECT);
            ImGui_ImplDX12_InitInfo init_info;
            init_info.Device = interface_device->d3d;
            init_info.CommandQueue = static_cast<plume::D3D12CommandQueue *>(d3d12_upload_queue.get())->d3d;
            init_info.NumFramesInFlight = 2;
            init_info.RTVFormat = DXGI_FORMAT::DXGI_FORMAT_B8G8R8A8_UNORM;
            init_info.SrvDescriptorHeap = interface_device->viewHeapAllocator->heap;
            init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu) {
                plume::RenderDescriptorRange range(plume::RenderDescriptorRangeType::TEXTURE, 0, 1);
                auto set = d3d12_imgui_device->createDescriptorSet(plume::RenderDescriptorSetDesc(&range, 1));
                const uint32_t at = static_cast<plume::D3D12DescriptorSet *>(set.get())->viewAllocation.offset;
                *out_cpu = d3d12_imgui_device->viewHeapAllocator->getCPUHandleAt(at);
                *out_gpu = d3d12_imgui_device->viewHeapAllocator->getGPUHandleAt(at);
                d3d12_texture_sets.push_back(std::move(set));
            };
            init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu, D3D12_GPU_DESCRIPTOR_HANDLE) {
                for (auto it = d3d12_texture_sets.begin(); it != d3d12_texture_sets.end(); ++it) {
                    const uint32_t at = static_cast<plume::D3D12DescriptorSet *>(it->get())->viewAllocation.offset;
                    if (d3d12_imgui_device->viewHeapAllocator->getCPUHandleAt(at).ptr == cpu.ptr) { d3d12_texture_sets.erase(it); return; }
                }
            };
            ImGui_ImplDX12_Init(&init_info);
#else
            assert(false && "Unsupported Graphics API.");
            return;
#endif
            break;
        }
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: {
            plume::VulkanDevice *interfaceDevice = static_cast<plume::VulkanDevice *>(device);
            ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_2, [](const char *functionName, void *vulkanInstance) {
                return vkGetInstanceProcAddr(*(reinterpret_cast<VkInstance *>(vulkanInstance)), functionName);
            }, &interfaceDevice->renderInterface->instance);

            std::unordered_map<VkDescriptorType, uint32_t> typeCounts;
            typeCounts[VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER] = 1;

            const VkFormat requestSurfaceImageFormat[] = { VK_FORMAT_B8G8R8A8_UNORM/*, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8_UNORM, VK_FORMAT_R8G8B8_UNORM*/ };

            vulkanContext = std::make_unique<VulkanContext>();
            vulkanContext->device = interfaceDevice->vk;
            vulkanContext->renderPass = plume::VulkanGraphicsPipeline::createRenderPass(interfaceDevice, requestSurfaceImageFormat,
                (size_t)IM_ARRAYSIZE(requestSurfaceImageFormat), VK_FORMAT_UNDEFINED, VK_SAMPLE_COUNT_1_BIT);
            vulkanContext->descriptorPool = plume::VulkanDescriptorSet::createDescriptorPool(interfaceDevice, typeCounts, false);

            uint32_t queueFamily = ImGui_ImplVulkanH_SelectQueueFamilyIndex(interfaceDevice->physicalDevice);
            IM_ASSERT(queueFamily != (uint32_t)-1);

            VkQueue queue;
            vkGetDeviceQueue(interfaceDevice->vk, queueFamily, 0, &queue);

            ImGui_ImplVulkan_InitInfo initInfo = {};
            initInfo.Instance = interfaceDevice->renderInterface->instance;
            initInfo.PhysicalDevice = interfaceDevice->physicalDevice;
            initInfo.Device = vulkanContext->device;
            initInfo.QueueFamily = queueFamily;
            initInfo.Queue = queue;
            initInfo.DescriptorPool = vulkanContext->descriptorPool;
            initInfo.DescriptorPoolSize = IMGUI_IMPL_VULKAN_MINIMUM_SAMPLED_IMAGE_POOL_SIZE; // JFG PORT: ImGui 1.92+ makes its own pool (more than one texture)
            initInfo.PipelineInfoMain.RenderPass = vulkanContext->renderPass;
            initInfo.PipelineInfoMain.Subpass = 0;
            initInfo.MinImageCount = 2;
            initInfo.ImageCount = 2;
            initInfo.CheckVkResultFn = &checkVulkanResult;

            ImGui_ImplVulkan_Init(&initInfo);
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            break;
    }

    SDL_AddEventWatch(sdl_event_filter, nullptr);

    ImGui::SetCurrentContext(prev_ctx);
}

static void render_draw_data(plume::RenderCommandList* command_list, ImDrawData* draw_data) {
    switch (get_graphics_api()) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12: {
#ifdef _WIN32
            plume::D3D12CommandList *interface_command_list = static_cast<plume::D3D12CommandList *>(command_list);
            interface_command_list->checkDescriptorHeaps();
            ImGui_ImplDX12_RenderDrawData(draw_data, interface_command_list->d3d);
#else
            assert(false && "Unsupported Graphics API.");
#endif
            break;
        }
        case RT64::UserConfiguration::GraphicsAPI::Vulkan: {
            plume::VulkanCommandList *interface_command_list = static_cast<plume::VulkanCommandList *>(command_list);
            ImGui_ImplVulkan_RenderDrawData(draw_data, interface_command_list->vk);
            break;
        }
        default:
            assert(false && "Unknown Graphics API.");
            break;
    }
}

// JFG PORT: what the render hook draws over each presented frame, in one ImGui render call (the backends cycle their
// vertex buffers once per call and assume one call a frame: a second call's drawing was lost):
// - the game's CPU-drawn reticles while RT64 adds in-between frames (src/jfg/jfg_cpu_layer.cpp), built here for every
//   presented frame; it needs the font atlas for its white pixel, so nothing is drawn before the first overlay or debug
//   window frame created it;
// - the mod overlay (src/jfg/jfg_overlay.cpp): mods' pictures and shapes at the window's resolution, every presented
//   frame, with its own textures (a white one for the shapes, the mods' pictures), which the backend creates and updates;
// - the debug window and the port's overlay (FPS, toasts), whose draw data is made once per game tick.
static void rt64_draw_hook(plume::RenderCommandList* command_list, plume::RenderFramebuffer* swap_chain_framebuffer) {
    const std::lock_guard<std::mutex> frame_lock(frame_mutex);
    if (jfg_imgui_ctx == nullptr || !jfg_imgui_ctx->Initialized) return;
    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(jfg_imgui_ctx);
    ImGuiIO &io = ImGui::GetIO();
    const ImVec2 size((float)swap_chain_framebuffer->getWidth(), (float)swap_chain_framebuffer->getHeight());

    static ImDrawList *cpu_list = nullptr, *mod_list = nullptr;
    if (cpu_list == nullptr) cpu_list = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    if (mod_list == nullptr) mod_list = IM_NEW(ImDrawList)(ImGui::GetDrawListSharedData());
    static ImVector<ImTextureData*> textures;
    textures.resize(0);

    ImDrawData combined;
    combined.Valid = true;
    combined.DisplayPos = ImVec2(0.0f, 0.0f);
    combined.DisplaySize = size;
    combined.FramebufferScale = ImVec2(1.0f, 1.0f);

    bool cpu_layer = false;
    if (io.Fonts != nullptr && io.Fonts->TexRef.GetTexID() != ImTextureID_Invalid) {
        cpu_list->_ResetForNewFrame();
        cpu_list->PushClipRect(ImVec2(0.0f, 0.0f), size);
        cpu_list->PushTexture(io.Fonts->TexRef);
        cpu_layer = jfg::cpu_layer_build(cpu_list);
        if (cpu_layer) combined.AddDrawList(cpu_list);
    }
    mod_list->_ResetForNewFrame();
    mod_list->PushClipRect(ImVec2(0.0f, 0.0f), size);
    if (jfg::overlay::build(mod_list, textures) && mod_list->VtxBuffer.Size > 0) combined.AddDrawList(mod_list);

    ImDrawData *ui = ui_wanted() ? ImGui::GetDrawData() : nullptr;
    if (ui != nullptr && ui->Valid) {
        if (ui->Textures != nullptr) for (ImTextureData *t : *ui->Textures) textures.push_back(t);
        for (ImDrawList *l : ui->CmdLists) combined.AddDrawList(l);
    }
    combined.Textures = &textures;
    // CmdLists.Size: AddDrawList does not keep ImGui's obsolete CmdListsCount, which stays 0 here. Testing that left the
    // reticles undrawn on every frame without a texture to update (no mod overlay, FPS counter or toast) (JFG-175).
    const bool draw = combined.CmdLists.Size > 0 || !textures.empty();
    if (draw) render_draw_data(command_list, &combined);
    if (cpu_layer) jfg::cpu_layer_trace_drawn(draw);
    ImGui::SetCurrentContext(prev_ctx);
}

static void rt64_deinit_hook() {
    const std::lock_guard<std::mutex> frame_lock(frame_mutex);
    assert(!b_in_ui_frame);
    b_is_open = false;

    ImGuiContext *prev_ctx = ImGui::GetCurrentContext();
    ImGui::SetCurrentContext(jfg_imgui_ctx);

    switch (get_graphics_api()) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
#ifdef _WIN32
            ImGui_ImplDX12_Shutdown();
            descriptor_set.release();
            d3d12_upload_queue.reset();
#else
            assert(false && "Unsupported Graphics API.");
            return;
#endif
            break;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            ImGui_ImplVulkan_Shutdown();
            vulkanContext.reset(nullptr);
            break;
        default:
            assert(false && "Unknown Graphics API.");
            break;
    }

    ImGui_ImplSDL2_Shutdown();

    ImGui::DestroyContext(jfg_imgui_ctx);
    ImGui::SetCurrentContext(prev_ctx);
    active_graphics_api = RT64::UserConfiguration::GraphicsAPI::OptionCount;
}

void set_render_hooks() {
    dino::renderer::add_hook(rt64_init_hook, rt64_draw_hook, rt64_deinit_hook);
}

}
