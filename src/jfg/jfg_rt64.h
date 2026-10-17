// Jet Force Gemini port: values shared with RT64 (patches/rt64-jfg.patch), read and written by its threads.
#pragma once

#include <atomic>
#include <cstdint>

extern std::atomic<uint32_t> rt64_jfg_present_count; // lib/rt64/src/hle/rt64_present_queue.cpp (JFG patch)
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
