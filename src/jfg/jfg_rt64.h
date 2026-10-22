// Jet Force Gemini port: values shared with RT64 (patches/rt64-jfg.patch), read and written by its threads.
#pragma once

#include <atomic>
#include <cstdint>

extern std::atomic<uint32_t> rt64_jfg_screen_fb;      // lib/rt64/src/hle/rt64_state.cpp (JFG patch): framebuffer of the latest screen update
extern std::atomic<uint32_t> rt64_jfg_screen_updates; // ... and how many screen updates RT64 has taken

extern std::atomic<uint32_t> rt64_jfg_present_count; // lib/rt64/src/hle/rt64_present_queue.cpp (JFG patch)
// lib/rt64/src/hle/rt64_present_queue.cpp: the frame being presented, set right before the render hook (same thread).
struct RT64JfgPresentInfo {
    uint32_t fbAddress;  // the framebuffer the VI shows
    float weight;        // (i + 1) / frames presented for that game frame, 1 without in-between frames
    float viewX, viewY, viewWidth, viewHeight; // where the framebuffer lands in the window, in pixels
    uint32_t fbWidth, fbHeight; // 0 when nothing is shown
};
extern RT64JfgPresentInfo rt64_jfg_present_info;
extern std::atomic<int> rt64_jfg_ssao;            // lib/rt64/src/render/rt64_framebuffer_renderer.cpp (JFG patch)
extern std::atomic<float> rt64_jfg_ssao_strength;
extern std::atomic<float> rt64_jfg_ssao_radius;
extern std::atomic<int> rt64_jfg_bloom;
extern std::atomic<int> rt64_jfg_paused;
extern std::atomic<float> rt64_jfg_bloom_intensity;
extern std::atomic<float> rt64_jfg_bloom_threshold;
extern std::atomic<float> rt64_jfg_bloom_radius;
extern std::atomic<float> rt64_jfg_fov_tan;
extern std::atomic<int> rt64_jfg_fog;
extern std::atomic<int> rt64_jfg_fog_blocked;
extern std::atomic<uint32_t> rt64_jfg_fog_drawn;
extern std::atomic<uint32_t> rt64_jfg_fog_game_scenes;
extern std::atomic<float> rt64_jfg_fog_start;
extern std::atomic<float> rt64_jfg_fog_end;
extern std::atomic<float> rt64_jfg_fog_density;
extern std::atomic<uint32_t> rt64_jfg_fog_color;
extern std::atomic<int> rt64_jfg_vignette;
extern std::atomic<float> rt64_jfg_vignette_strength;
extern std::atomic<float> rt64_jfg_vignette_size;
