#pragma once

#include <atomic>
#include <unordered_set>
#include <filesystem>

#include "common/rt64_user_configuration.h"
#include "ultramodern/renderer_context.hpp"
#include "librecomp/mods.hpp"

namespace RT64 {
    struct Application;
}

struct SDL_Window;

namespace dino::renderer {
    inline const std::string special_option_texture_pack_enabled = "_recomp_texture_pack_enabled";

    class RT64Context final : public ultramodern::renderer::RendererContext {
    public:
        ~RT64Context() override;
        RT64Context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

        bool valid() override { return static_cast<bool>(app); }

        bool update_config(const ultramodern::renderer::GraphicsConfig &old_config, const ultramodern::renderer::GraphicsConfig &new_config) override;

        void enable_instant_present() override;
        void send_dl(const OSTask *task) override;
        void update_screen() override;
        void shutdown() override;
        uint32_t get_display_framerate() const override;
        float get_resolution_scale() const override;
        void preload_common_shaders();
#ifdef __ANDROID__
        void android_detach_window();
#endif

    private:
        std::unique_ptr<RT64::Application> app;
#ifdef __ANDROID__
        SDL_Window* sdl_window = nullptr;
        std::atomic<bool> window_detached{ false };
        void android_reattach_window();
#endif
        std::unordered_set<std::string> enabled_texture_packs;
        std::unordered_set<std::string> secondary_disabled_texture_packs;

        void check_texture_pack_actions();
        void update_render_options();
    };

    std::unique_ptr<ultramodern::renderer::RendererContext> create_render_context(uint8_t *rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode);

    RT64::UserConfiguration::Antialiasing RT64MaxMSAA();
    bool RT64SamplePositionsSupported();
    bool RT64HighPrecisionFBEnabled();
    void preload_common_shaders();

    void trigger_texture_pack_update();
    void enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod);
    void disable_texture_pack(const recomp::mods::ModHandle& mod);
    void secondary_enable_texture_pack(const std::string& mod_id);
    void secondary_disable_texture_pack(const std::string& mod_id);

    // Texture pack enable option. Must be an enum with two options.
    // The first option is treated as disabled and the second option is treated as enabled.
    bool is_texture_pack_enable_config_option(const recomp::mods::ConfigOption& option, bool show_errors);
}
