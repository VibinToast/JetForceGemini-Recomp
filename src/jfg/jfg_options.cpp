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
// apply_option: the frame_rate just read was the old "60 + Interpolated" (2), see below.
static bool g_old_interpolated = false;

AimView aim_view() {
    if (g.aim_shoulder) return AIM_SHOULDER_VIEW;
    return { g.aim_drag_margin, g.aim_drag_return, g.aim_offset_x, g.aim_offset_y, g.aim_offset_z };
}

bool aim_view_key(const char* key, int* preset_value) {
    const AimView& p = AIM_SHOULDER_VIEW;
    const struct { const char* key; int value; } keys[] = { { "aim_drag_margin", p.drag_margin }, { "aim_drag_return", p.drag_return },
        { "aim_offset_x", p.offset_x }, { "aim_offset_y", p.offset_y }, { "aim_offset_z", p.zoom } };
    for (const auto& k : keys) {
        if (std::strcmp(key, k.key) == 0) { if (preset_value != nullptr) *preset_value = k.value; return true; }
    }
    return false;
}

void apply_option(const char* key, int value) {
    if (std::strcmp(key, "no_blood") == 0) g.no_blood = value != 0;
    else if (std::strcmp(key, "gore_keep_blood") == 0) g.gore_keep_blood = value != 0;
    else if (std::strcmp(key, "gore_keep_limbs") == 0) g.gore_keep_limbs = value != 0;
    else if (std::strcmp(key, "blood_amount") == 0) g.blood_amount = std::clamp(value, 100, 800);
    else if (std::strcmp(key, "blood_splats") == 0) g.blood_splats = std::clamp(value, 64, 4096);
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
    else if (std::strcmp(key, "frame_rate") == 0) {
        // The old "60 + Interpolated" (2) is 60 with Smooth Motion: up to the monitor's rate, or the Interpolation Limit
        // that follows it in the file.
        g_old_interpolated = FRAME_RATE_OPTION && value == 2;
        g.frame_rate = !FRAME_RATE_OPTION ? 0 : std::clamp(value, 0, 1);
        if (g_old_interpolated && g.smooth_motion == 0) g.smooth_motion = 1;
    }
    else if (std::strcmp(key, "interp_limit") == 0) {
        g.interp_limit = value;
        if (g_old_interpolated && value > 1) apply_option("smooth_motion", value);
    }
    else if (std::strcmp(key, "smooth_motion") == 0) {
        // The choices of Smooth Motion (tools/gen_jfg_config_rml.py); anything else is off.
        static const int smooth_motion_values[] = { 0, 1, 60, 90, 120, 144, 165, 240 };
        g.smooth_motion = 0;
        for (int v : smooth_motion_values) if (v == value) g.smooth_motion = v;
    }
    else if (std::strcmp(key, "smooth_shadows") == 0) g.smooth_shadows = value != 0;
    else if (std::strcmp(key, "widescreen") == 0) g.widescreen = value != 0;
    else if (std::strcmp(key, "hud_corners") == 0) g.hud_corners = value != 0;
    else if (std::strcmp(key, "fov") == 0) g.fov = std::clamp(value, 75, 140);
    else if (std::strcmp(key, "hit_flash") == 0) g.hit_flash = std::clamp(value, 0, 100);
    else if (std::strcmp(key, "shadow_opacity") == 0) g.shadow_opacity = std::clamp(value, 0, 200);
    else if (std::strcmp(key, "hide_floyd") == 0) g.hide_floyd = value != 0;
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
    else if (std::strcmp(key, "sprint_aiming") == 0) g.sprint_aiming = value != 0;
    else if (std::strcmp(key, "sprint_swimming") == 0) g.sprint_swimming = value != 0;
    else if (std::strcmp(key, "sprint_swim_speed") == 0) g.sprint_swim_speed = std::clamp(value, 100, 300);
    else if (std::strcmp(key, "normalize_diagonal") == 0) g.normalize_diagonal = value != 0;
    else if (std::strcmp(key, "aim_even_speed") == 0) g.aim_even_speed = value != 0;
    else if (std::strcmp(key, "invert_aim_normal") == 0) g.invert_aim_normal = value != 0;
    else if (std::strcmp(key, "invert_camera_y") == 0) g.invert_camera_y = value != 0;
    else if (std::strcmp(key, "camera_speed_h") == 0) g.camera_speed_h = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "camera_speed_v") == 0) g.camera_speed_v = std::clamp(value, 10, 400);
    else if (std::strcmp(key, "camera_distance") == 0) g.camera_distance = std::clamp(value, 50, 250);
    else if (std::strcmp(key, "aim_drag_margin") == 0) g.aim_drag_margin = std::clamp(value, 0, 50);
    else if (std::strcmp(key, "aim_drag_return") == 0) g.aim_drag_return = std::clamp(value, 0, 400);
    else if (std::strcmp(key, "aim_offset_x") == 0) g.aim_offset_x = std::clamp(value, -50, 50);
    else if (std::strcmp(key, "aim_offset_y") == 0) g.aim_offset_y = std::clamp(value, -50, 50);
    else if (std::strcmp(key, "aim_offset_z") == 0) g.aim_offset_z = std::clamp(value, 0, 300);
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
    if (const char* env = std::getenv("JFG_FRAME_RATE")) apply_option("frame_rate", std::atoi(env)); // 2 = 60 + Smooth Motion
    if (const char* env = std::getenv("JFG_INTERP_LIMIT")) apply_option("interp_limit", std::atoi(env)); // with JFG_FRAME_RATE=2
    if (const char* env = std::getenv("JFG_SMOOTH_MOTION")) apply_option("smooth_motion", std::atoi(env)); // 0, 1 (monitor), 60..240
    push_render_options();
}

std::string options_text(const State& s) {
    std::string out;
    char buf[512];
    std::snprintf(buf, sizeof(buf), "no_blood=%d\nstart_skips_cutscenes=%d\n", s.no_blood ? 1 : 0, s.start_skips_cutscenes ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "auto_save=%d\nauto_save_minutes=%d\nsave_notification=%d\nkeep_area_progress=%d\n", s.auto_save ? 1 : 0, s.auto_save_minutes, s.save_notification ? 1 : 0, s.keep_area_progress ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "show_fps=%d\nwidescreen=%d\nframe_rate=%d\nsmooth_motion=%d\n", s.show_fps ? 1 : 0, s.widescreen ? 1 : 0, s.frame_rate, s.smooth_motion); out += buf;
    std::snprintf(buf, sizeof(buf), "smooth_shadows=%d\nshadow_opacity=%d\nhud_corners=%d\n", s.smooth_shadows ? 1 : 0, s.shadow_opacity, s.hud_corners ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "fov=%d\nhit_flash=%d\nhide_floyd=%d\n", s.fov, s.hit_flash, s.hide_floyd ? 1 : 0); out += buf;
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
    std::snprintf(buf, sizeof(buf), "camera_collision=%d\ncamera_distance=%d\n", s.camera_collision ? 1 : 0, s.camera_distance); out += buf;
    std::snprintf(buf, sizeof(buf), "aim_drag_margin=%d\naim_drag_return=%d\n", s.aim_drag_margin, s.aim_drag_return); out += buf;
    std::snprintf(buf, sizeof(buf), "aim_offset_x=%d\naim_offset_y=%d\naim_offset_z=%d\n", s.aim_offset_x, s.aim_offset_y, s.aim_offset_z); out += buf;
    std::snprintf(buf, sizeof(buf), "move_speed=%d\nnormalize_diagonal=%d\naim_even_speed=%d\n", s.move_speed, s.normalize_diagonal ? 1 : 0, s.aim_even_speed ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "camera_speed_h=%d\ncamera_speed_v=%d\naim_shoulder=%d\naim_centered=%d\naim_speed=%d\n", s.camera_speed_h, s.camera_speed_v, s.aim_shoulder ? 1 : 0, s.aim_centered ? 1 : 0, s.aim_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "aim_jump=%d\n", s.aim_jump ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "mouse_keyboard=%d\nmouse_camera_speed=%d\nmouse_aim_speed=%d\ninvert_mouse_y=%d\n", s.mouse_keyboard ? 1 : 0, s.mouse_camera_speed, s.mouse_aim_speed, s.invert_mouse_y ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "refuel_speed=%d\nsniper_fixed_zoom=%d\nsniper_zoom=%d\ndamage_taken=%d\ndive_speed=%d\n", s.refuel_speed, s.sniper_fixed_zoom ? 1 : 0, s.sniper_zoom, s.damage_taken, s.dive_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "fly_speed=%d\nfly_freeze=%d\nfly_hide_hud=%d\n", s.fly_speed, s.fly_freeze ? 1 : 0, s.fly_hide_hud ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "unlimited_continues=%d\npermanent_tribals=%d\n", s.unlimited_continues ? 1 : 0, s.permanent_tribals ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "sprint=%d\nsprint_speed=%d\nsprint_aiming=%d\n", s.sprint ? 1 : 0, s.sprint_speed, s.sprint_aiming ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "sprint_swimming=%d\nsprint_swim_speed=%d\n", s.sprint_swimming ? 1 : 0, s.sprint_swim_speed); out += buf;
    std::snprintf(buf, sizeof(buf), "pause_in_menu=%d\n", s.pause_in_menu ? 1 : 0); out += buf;
    std::snprintf(buf, sizeof(buf), "gore_keep_blood=%d\ngore_keep_limbs=%d\nblood_amount=%d\nblood_splats=%d\n", s.gore_keep_blood ? 1 : 0, s.gore_keep_limbs ? 1 : 0, s.blood_amount, s.blood_splats); out += buf;
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
