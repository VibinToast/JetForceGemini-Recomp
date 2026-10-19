// Jet Force Gemini port: the options (jfg_options.h), saved in jfg_port_options.txt and handed to RT64.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_rt64.h"

namespace jfg {

State g;

// Options persist in a small text file next to the working directory the game is started from.
const char* const OPTIONS_FILE = "jfg_port_options.txt";
bool g_options_loaded = false;

// Depth fog colours (0xRRGGBB): Haze, Mist, Dusk, Night. Names in the F9 menu and tools/gen_jfg_config_rml.py.
constexpr uint32_t FOG_COLORS[] = { 0xA0B4C8u, 0xD8DCE0u, 0xC89A78u, 0x2A3448u };
constexpr int FOG_COLOR_COUNT = (int)(sizeof(FOG_COLORS) / sizeof(FOG_COLORS[0]));

// Hands the renderer options to RT64 (read by its render thread every frame).
void push_render_options() {
    rt64_jfg_ssao_strength.store(g.ssao_strength / 100.0f, std::memory_order_relaxed);
    rt64_jfg_ssao_radius.store(float(g.ssao_radius), std::memory_order_relaxed);
    rt64_jfg_ssao.store(g.ssao ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_bloom_intensity.store(g.bloom_intensity / 100.0f, std::memory_order_relaxed);
    rt64_jfg_bloom_threshold.store(g.bloom_threshold / 100.0f, std::memory_order_relaxed);
    rt64_jfg_bloom_radius.store(float(g.bloom_radius), std::memory_order_relaxed);
    rt64_jfg_bloom.store(g.bloom ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_vignette_strength.store(g.vignette_strength / 100.0f, std::memory_order_relaxed);
    rt64_jfg_vignette_size.store(g.vignette_size / 100.0f, std::memory_order_relaxed);
    rt64_jfg_vignette.store(g.vignette ? 1 : 0, std::memory_order_relaxed);
    rt64_jfg_fog_start.store(float(g.fog_start), std::memory_order_relaxed);
    rt64_jfg_fog_end.store(float(std::max(g.fog_end, g.fog_start + 100)), std::memory_order_relaxed);
    rt64_jfg_fog_density.store(g.fog_density / 100.0f, std::memory_order_relaxed);
    rt64_jfg_fog_color.store(FOG_COLORS[std::clamp(g.fog_color, 0, FOG_COLOR_COUNT - 1)], std::memory_order_relaxed);
    rt64_jfg_fog.store(g.depth_fog ? 1 : 0, std::memory_order_relaxed);
}

// Sets one option from its saved form (the options file, profiles).
void apply_option(const char* key, int value) {
    if (std::strcmp(key, "no_blood") == 0) g.no_blood = value != 0;
    else if (std::strcmp(key, "show_fps") == 0) g.show_fps = value != 0;
    else if (std::strcmp(key, "show_level") == 0) g.show_level = value != 0;
    else if (std::strcmp(key, "show_pack") == 0) g.show_pack = value != 0;
    else if (std::strcmp(key, "dump_by_level") == 0) g.dump_by_level = value != 0;
    else if (std::strcmp(key, "no_lod") == 0) g.no_lod = value != 0;
    else if (std::strcmp(key, "far_objects") == 0) g.far_objects = value != 0;
    else if (std::strcmp(key, "ssao") == 0) g.ssao = value != 0;
    else if (std::strcmp(key, "ssao_strength") == 0) g.ssao_strength = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "ssao_radius") == 0) g.ssao_radius = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "bloom") == 0) g.bloom = value != 0;
    else if (std::strcmp(key, "bloom_intensity") == 0) g.bloom_intensity = std::clamp(value, 0, 300);
    else if (std::strcmp(key, "bloom_threshold") == 0) g.bloom_threshold = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "bloom_radius") == 0) g.bloom_radius = std::clamp(value, 1, 20);
    else if (std::strcmp(key, "frame_rate") == 0) g.frame_rate = FRAME_RATE_OPTION ? std::clamp(value, 0, 2) : 0;
    else if (std::strcmp(key, "widescreen") == 0) g.widescreen = value != 0;
    else if (std::strcmp(key, "fov") == 0) g.fov = std::clamp(value, 75, 140);
    else if (std::strcmp(key, "hit_flash") == 0) g.hit_flash = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "vignette") == 0) g.vignette = value != 0;
    else if (std::strcmp(key, "vignette_strength") == 0) g.vignette_strength = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "vignette_size") == 0) g.vignette_size = std::clamp(value, 0, 90);
    else if (std::strcmp(key, "level_fog_distance") == 0) g.level_fog_distance = std::clamp(value, 50, 400);
    else if (std::strcmp(key, "no_level_fog") == 0) g.no_level_fog = value != 0;
    else if (std::strcmp(key, "depth_fog") == 0) g.depth_fog = DEPTH_FOG_OPTION && value != 0;
    else if (std::strcmp(key, "fog_start") == 0) g.fog_start = std::clamp(value, 0, 10000);
    else if (std::strcmp(key, "fog_end") == 0) g.fog_end = std::clamp(value, 500, 15000);
    else if (std::strcmp(key, "fog_density") == 0) g.fog_density = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "fog_color") == 0) g.fog_color = std::clamp(value, 0, 3);
    else if (std::strcmp(key, "texture_filter") == 0) g.texture_filter = std::clamp(value, 0, 1);
    else if (std::strcmp(key, "output_filter") == 0) g.output_filter = std::clamp(value, 0, 2);
    else if (std::strcmp(key, "start_skips_cutscenes") == 0) g.start_skips_cutscenes = value != 0;
    else if (std::strcmp(key, "pause_in_menu") == 0) g.pause_in_menu = value != 0;
    else if (std::strcmp(key, "auto_save") == 0) g.auto_save = value != 0;
    else if (std::strcmp(key, "auto_save_minutes") == 0) g.auto_save_minutes = std::clamp(value, 0, 30);
    else if (std::strcmp(key, "save_notification") == 0) g.save_notification = value != 0;
    else if (std::strcmp(key, "keep_area_progress") == 0) g.keep_area_progress = value != 0;
    else if (std::strcmp(key, "permanent_tribals") == 0) g.permanent_tribals = value != 0;
    else if (std::strcmp(key, "dual_stick") == 0) g.dual_stick = value != 0;
    else if (std::strcmp(key, "invert_aim_y") == 0) g.invert_aim_y = value != 0;
    else if (std::strcmp(key, "swap_triggers") == 0) g.swap_triggers = value != 0;
    else if (std::strcmp(key, "dual_layout") == 0) g.dual_layout = value != 0;
    else if (std::strcmp(key, "aim_from_character") == 0) g.aim_from_character = value != 0;
    else if (std::strcmp(key, "camera_collision") == 0) g.camera_collision = value != 0;
    else if (std::strcmp(key, "move_speed") == 0) g.move_speed = std::clamp(value, 25, 400);
    else if (std::strcmp(key, "sprint") == 0) g.sprint = value != 0;
    else if (std::strcmp(key, "sprint_speed") == 0) g.sprint_speed = std::clamp(value, 100, 300);
    else if (std::strcmp(key, "diagonal_boost") == 0) g.diagonal_boost = value != 0;
    else if (std::strcmp(key, "strafe_speed") == 0) g.strafe_speed = std::clamp(value, 0, 300);
    else if (std::strcmp(key, "invert_aim_normal") == 0) g.invert_aim_normal = value != 0;
    else if (std::strcmp(key, "invert_camera_y") == 0) g.invert_camera_y = value != 0;
    else if (std::strcmp(key, "camera_speed_h") == 0) g.camera_speed_h = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "camera_speed_v") == 0) g.camera_speed_v = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "aim_shoulder") == 0) g.aim_shoulder = value != 0;
    else if (std::strcmp(key, "aim_centered") == 0) g.aim_centered = value != 0;
    else if (std::strcmp(key, "aim_jump") == 0) g.aim_jump = value != 0;
    else if (std::strcmp(key, "aim_speed") == 0) g.aim_speed = std::clamp(value, 10, 300);
    else if (std::strcmp(key, "mouse_keyboard") == 0) g.mouse_keyboard = value != 0;
    else if (std::strcmp(key, "mouse_camera_speed") == 0) g.mouse_camera_speed = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "mouse_aim_speed") == 0) g.mouse_aim_speed = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "invert_mouse_y") == 0) g.invert_mouse_y = value != 0;
    else if (std::strcmp(key, "refuel_speed") == 0) g.refuel_speed = std::clamp(value, 25, 1000);
    else if (std::strcmp(key, "sniper_fixed_zoom") == 0) g.sniper_fixed_zoom = value != 0;
    else if (std::strcmp(key, "sniper_zoom") == 0) g.sniper_zoom = std::clamp(value, 100, 500);
    else if (std::strcmp(key, "damage_taken") == 0) g.damage_taken = std::clamp(value, 0, 400);
    else if (std::strcmp(key, "unlimited_continues") == 0) g.unlimited_continues = value != 0;
    else if (std::strcmp(key, "dive_speed") == 0) g.dive_speed = std::clamp(value, 50, 400);
    else if (std::strcmp(key, "fly_speed") == 0) g.fly_speed = std::clamp(value, 10, 1000);
    else if (std::strcmp(key, "fly_freeze") == 0) g.fly_freeze = value != 0;
    else if (std::strcmp(key, "fly_hide_hud") == 0) g.fly_hide_hud = value != 0;
}

void load_options() {
    if (g_options_loaded) return;
    g_options_loaded = true;
    if (std::FILE* f = std::fopen(OPTIONS_FILE, "r")) {
        char key[64]; int value;
        while (std::fscanf(f, "%63[^=]=%d\n", key, &value) == 2) apply_option(key, value);
        std::fclose(f);
    }
    // JFG_FRAME_RATE=0/1/2 (tests): the "Frame rate" option for this run (saved only if an option is changed in the run).
    if (const char* env = std::getenv("JFG_FRAME_RATE")) g.frame_rate = std::clamp(std::atoi(env), 0, 2);
    push_render_options();
}

std::string options_text(const State& s) {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "no_blood=%d\nstart_skips_cutscenes=%d\n", s.no_blood ? 1 : 0, s.start_skips_cutscenes ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "auto_save=%d\nauto_save_minutes=%d\nsave_notification=%d\nkeep_area_progress=%d\n", s.auto_save ? 1 : 0, s.auto_save_minutes, s.save_notification ? 1 : 0, s.keep_area_progress ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "show_fps=%d\nwidescreen=%d\nframe_rate=%d\n", s.show_fps ? 1 : 0, s.widescreen ? 1 : 0, s.frame_rate); out += buf;
    std::snprintf(buf, sizeof(buf), "fov=%d\nhit_flash=%d\n", s.fov, s.hit_flash); out += buf;
    std::snprintf(buf, sizeof(buf), "vignette=%d\nvignette_strength=%d\nvignette_size=%d\n", s.vignette ? 1 : 0, s.vignette_strength, s.vignette_size); out += buf;
    std::snprintf(buf, sizeof(buf), "level_fog_distance=%d\nno_level_fog=%d\n", s.level_fog_distance, s.no_level_fog ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "depth_fog=%d\nfog_start=%d\nfog_end=%d\nfog_density=%d\nfog_color=%d\n", s.depth_fog ? 1 : 0, s.fog_start, s.fog_end, s.fog_density, s.fog_color); out += buf;
    std::snprintf(buf, sizeof(buf), "texture_filter=%d\noutput_filter=%d\n", s.texture_filter, s.output_filter); out += buf;
    std::snprintf(buf, sizeof(buf), "show_level=%d\nshow_pack=%d\ndump_by_level=%d\n", s.show_level ? 1 : 0, s.show_pack ? 1 : 0, s.dump_by_level ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "no_lod=%d\nfar_objects=%d\n", s.no_lod ? 1 : 0, s.far_objects ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "ssao=%d\nssao_strength=%d\nssao_radius=%d\n", s.ssao ? 1 : 0, s.ssao_strength, s.ssao_radius); out += buf;
    std::snprintf(buf, sizeof(buf), "bloom=%d\nbloom_intensity=%d\nbloom_threshold=%d\nbloom_radius=%d\n", s.bloom ? 1 : 0, s.bloom_intensity, s.bloom_threshold, s.bloom_radius); out += buf;
    std::snprintf(buf, sizeof(buf), "dual_stick=%d\ninvert_aim_y=%d\nswap_triggers=%d\ndual_layout=%d\n", s.dual_stick ? 1 : 0, s.invert_aim_y ? 1 : 0, s.swap_triggers ? 1 : 0, s.dual_layout ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "invert_aim_normal=%d\ninvert_camera_y=%d\naim_from_character=%d\n", s.invert_aim_normal ? 1 : 0, s.invert_camera_y ? 1 : 0, s.aim_from_character ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "camera_collision=%d\n", s.camera_collision ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "move_speed=%d\ndiagonal_boost=%d\nstrafe_speed=%d\n", s.move_speed, s.diagonal_boost ? 1 : 0, s.strafe_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "camera_speed_h=%d\ncamera_speed_v=%d\naim_shoulder=%d\naim_centered=%d\naim_speed=%d\n", s.camera_speed_h, s.camera_speed_v, s.aim_shoulder ? 1 : 0, s.aim_centered ? 1 : 0, s.aim_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "aim_jump=%d\n", s.aim_jump ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "mouse_keyboard=%d\nmouse_camera_speed=%d\nmouse_aim_speed=%d\ninvert_mouse_y=%d\n", s.mouse_keyboard ? 1 : 0, s.mouse_camera_speed, s.mouse_aim_speed, s.invert_mouse_y ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "refuel_speed=%d\nsniper_fixed_zoom=%d\nsniper_zoom=%d\ndamage_taken=%d\ndive_speed=%d\n", s.refuel_speed, s.sniper_fixed_zoom ? 1 : 0, s.sniper_zoom, s.damage_taken, s.dive_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "fly_speed=%d\nfly_freeze=%d\nfly_hide_hud=%d\n", s.fly_speed, s.fly_freeze ? 1 : 0, s.fly_hide_hud ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "unlimited_continues=%d\npermanent_tribals=%d\n", s.unlimited_continues ? 1 : 0, s.permanent_tribals ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "sprint=%d\nsprint_speed=%d\n", s.sprint ? 1 : 0, s.sprint_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "pause_in_menu=%d\n", s.pause_in_menu ? 1 : 0); out += buf;
    return out;
}

void save_options() {
    if (std::FILE* f = std::fopen(OPTIONS_FILE, "w")) {
        std::fputs(options_text(g).c_str(), f);
        std::fclose(f);
    }
    profile_autosave();
}

} // namespace jfg
