// Jet Force Gemini port: every function the rest of the program calls in src/jfg, by caller.
//
// They have C linkage. The game handlers are called from the wrappers around game functions (JFG_WRAP below) and from
// the mod API's internal handlers (jfg_api.cpp); the others from the base project's code. Most are defined inside namespace jfg in the file named below; with extern "C" they
// keep their plain name.
#pragma once

#include <cstdint>

#include "recomp.h"

extern "C" {

// --- game handlers (called around game functions) ------------------------------------------------------------------
// jfg_tick.cpp
void jfg_debug_frame(uint8_t* rdram, recomp_context* ctx);             // entry of the main loop tick func_80044FAC
// jfg_cutscene_skip.cpp
void jfg_after_joy(uint8_t* rdram, recomp_context* ctx);               // main loop tick, right after joyRead
int jfg_rcp_skip(uint8_t* rdram, uint32_t type);                       // entry of rcpFast3d
int jfg_snd_skip(uint8_t* rdram, uint32_t sp);                         // entry of ad_sndp_play
int jfg_evt_skip(uint8_t* rdram, uint32_t evtq, uint32_t evt);         // entry of n_alEvtqPostEvent
void jfg_evt_next(uint8_t* rdram, recomp_context* ctx);                 // entry of n_alEvtqNextEvent
// jfg_frame_rate.cpp
int jfg_vi_skip(uint8_t* rdram, recomp_context* ctx);                  // entry of viFrameSync
uint32_t jfg_frame_sync(uint8_t* rdram, uint32_t frames);              // main loop tick, right after viFrameSync
int jfg_sched_extra_frame(void);                                       // __scHandleRetrace
void jfg_sched_retrace(void);                                          // entry of __scHandleRetrace
int jfg_sched_task_ready(void);                                        // entry of __scTaskReady
int jfg_squad_frames(uint8_t* rdram, uint32_t member, int frames);     // FlowerBoltControl; the squad wrappers below
int jfg_cadence_skip(void);                                            // frontPlayerTarget, the target lock countdown
void jfg_squad_draw_begin(uint8_t* rdram);                             // func_80045F00, right before trackDraw
void jfg_squad_draw_end(uint8_t* rdram);                               // func_80045F00, right after trackDraw
// jfg_controls.cpp
void jfg_move_scale(uint8_t* rdram, recomp_context* ctx);              // objMoveXYZ's wrapper
void jfg_boss_tick(uint8_t* rdram, recomp_context* ctx);               // jfg_on_boss_fight_control handler (jfg_api.cpp)
void jfg_platform_move(int on);                                        // controlPlatform, around its objMoveXYZ call
void jfg_sidekick_begin(uint8_t* rdram, recomp_context* ctx);          // sidekickControl's wrapper (jfg_controls.cpp)
void jfg_sidekick_end(void);
int jfg_floyd_roll_before(uint8_t* rdram, recomp_context* ctx);       // dAngle's wrapper: Floyd missions' mouse steering
void jfg_floyd_roll_after(uint8_t* rdram, recomp_context* ctx);
// jfg_camera.cpp
void jfg_camera_override(uint8_t* rdram, recomp_context* ctx);         // func_80045F00, right before the scene is drawn
int jfg_freeze_world(void);                                            // func_80045F00, world tick or pause tick
// jfg_visuals.cpp
float jfg_fov(uint8_t* rdram, float fov, uint32_t zoomed);             // camSetFOV
void jfg_game_fog(uint8_t* rdram, uint32_t sp);                        // func_8001BF9C, right after trackGetFog
uint32_t jfg_hit_flash(uint32_t color);                                // the hit flash's colour (jfg_visuals.cpp)
void jfg_hit_flash_event(uint8_t* rdram, recomp_context* ctx);        // jfg_on_screen_flash handler
void jfg_floyd_seen(uint8_t* rdram, recomp_context* ctx);              // jfg_on_sidekick_tick handler
void jfg_hide_object(uint8_t* rdram, recomp_context* ctx);             // jfg_on_object_draw handler
void jfg_floyd_done(void);                                             // after sidekickControl
void jfg_floyd_mute(uint8_t* rdram, recomp_context* ctx);             // jfg_on_sound_play handler
// jfg_interpolation.cpp
void jfg_interp_object(uint8_t* rdram, recomp_context* ctx);            // objPrintObject's wrapper, before
void jfg_interp_object_done(uint8_t* rdram, recomp_context* ctx);       // ... and after
void jfg_interp_push(uint8_t* rdram, recomp_context* ctx);              // the four matrix push functions' wrappers, before
void jfg_interp_pushed(uint8_t* rdram, recomp_context* ctx);            // ... and after
// jfg_gameplay.cpp
int jfg_no_blood(void);                                                // the blood event handlers (jfg_gore.cpp)
int jfg_refuel_amount(int amount);                                     // refuelControl
float jfg_sniper_zoom(float zoom);                                     // controlGetManualAim
int jfg_sniper_walk(int weapon);                                       // the characters' aim control (overlays 15-17)
int jfg_flag_override(int flag);                                       // mainGetGameFlag's wrapper
// jfg_gore.cpp
void jfg_gore_on_blood_spurt(uint8_t* rdram, recomp_context* ctx);     // jfg_on_blood_spurt handler (jfg_api.cpp)
void jfg_gore_on_blood_hit(uint8_t* rdram, recomp_context* ctx);       // jfg_on_blood_hit handler
void jfg_gore_limb(uint8_t* rdram, recomp_context* ctx);               // jfg_on_severed_limb_tick handler
void jfg_blood_extra_drops(uint8_t* rdram, recomp_context* ctx, int amount); // bloodSpurtAdd's wrapper
// jfg_area_progress.cpp
void jfg_region_reset_begin(uint8_t* rdram, recomp_context* ctx);      // mainResetRegion's wrapper, before
void jfg_region_reset_end(uint8_t* rdram, recomp_context* ctx);        // ... and after
// jfg_weapons.cpp
uint32_t jfg_anim_speed(uint8_t* rdram, uint32_t obj, uint32_t speed_bits); // objAnimDframe's wrapper
void jfg_mine_init(uint8_t* rdram, uint32_t mine);                     // after mineInit (its wrapper)
void jfg_bolt_init(uint8_t* rdram, recomp_context* ctx);               // after laserboltInit, a0 = the bolt
// jfg_diagnostics.cpp
void jfg_mm_alloc_check(uint8_t* rdram, recomp_context* ctx, uint32_t size, uint32_t colour); // after mmAlloc/mmAlloc2
void jfg_squad_spawn_before(uint8_t* rdram, recomp_context* ctx);      // overlay 3 +0x14C9C, the squad member loop
void jfg_squad_spawn_after(uint8_t* rdram, recomp_context* ctx);       // overlay 3 +0x14CA4, after its call
void jfg_objsetup_enter(uint8_t* rdram, recomp_context* ctx);          // entry of objSetupObject
void jfg_objsetup_step(uint8_t* rdram, recomp_context* ctx, int step); // objSetupObject, after each call that can fail
void jfg_objsetup_exit(uint8_t* rdram, recomp_context* ctx, int type); // objSetupObject's wrapper, after
void jfg_squads_check_enemy(uint8_t* rdram, recomp_context* ctx);      // squadsCheckEnemyPointers' wrapper (replaces it)
void jfg_tidy_linked_list_after(uint8_t* rdram, uint32_t obj);         // TidyLinkedList's wrapper: a member it missed
int jfg_bats_chain_ok(uint8_t* rdram, uint32_t data);                  // GalaxianBatsSoundsHandler's wrapper
void jfg_game_break(uint8_t* rdram, recomp_context* ctx);              // mathBreakPoint's wrapper
void jfg_change_level_log(uint8_t* rdram, recomp_context* ctx);        // jfg_on_level_change handler (jfg_api.cpp)
void jfg_sync_printf(uint8_t* rdram, recomp_context* ctx);             // osSyncPrintf's wrapper (replaces it)
// jfg_glue.cpp
void jfg_overlay_loaded(uint8_t* rdram, recomp_context* ctx);          // runlinkDownloadCode, runlinkResumeCode (wrappers)
void jfg_overlay_unloading(uint8_t* rdram, recomp_context* ctx);       // runlinkFreeCode, runlinkUnloadOverlay (wrappers)

// --- base project --------------------------------------------------------------------------------------------------
// jfg_controls.cpp: src/input/controls.cpp, src/input/input.cpp
int jfg_dual_stick_active(void);
int jfg_dual_stick_layout(void);
int jfg_mouse_keyboard_active(void);
int jfg_floyd_mouse_active(void);
void jfg_mouse_wheel(int notches);
void jfg_mouse_motion(int dx, int dy);
int jfg_invert_aim_normal(uint16_t buttons);
void jfg_dual_stick(uint16_t* buttons, float* x, float* y, float right_x, float right_y);
void jfg_dual_stick_release(void);
int jfg_sprint_enabled(void);
void jfg_sprint_input(int held);
// src/renderer/renderer.cpp
int jfg_texture_dump_active(void);                                     // jfg_texture_packs.cpp
void jfg_texture_pack_set_count(int count);                            // jfg_texture_packs.cpp
void jfg_texture_dump_folder(char* buf, int size);                     // jfg_texture_packs.cpp
void jfg_texture_dump_scene(char* buf, int size);                      // jfg_texture_packs.cpp
int jfg_texture_pack_current(char* buf, int size);                     // jfg_texture_packs.cpp
int jfg_refresh_override(void);                                        // jfg_frame_rate.cpp
int jfg_interp_limit(void);                                            // jfg_frame_rate.cpp
int jfg_widescreen_active(void);                                       // jfg_visuals.cpp
int jfg_texture_three_point(void);                                     // jfg_visuals.cpp
int jfg_output_filtering(void);                                        // jfg_visuals.cpp
int jfg_skip_render_active(void);                                      // jfg_cutscene_skip.cpp
// src/main.cpp
void jfg_on_init(uint8_t* rdram, recomp_context* ctx);                 // jfg_glue.cpp
void jfg_register_mod_exports(void);                                   // jfg_mods.cpp: functions mods import ("*")
void jfg_apply_asset_mods(void);                                       // jfg_assets.cpp: data replaced by mods
// src/ui/ui_state.cpp
void jfg_settings_menu_open(int open);                                 // jfg_camera.cpp, every UI frame
void jfg_tools_ui_frame(void);                                         // jfg_ui_tools.cpp, every UI frame (F10's tools)
// src/debug_ui/backend.cpp
void jfg_tools_key(int scancode, int down, int repeat);                // jfg_ui_tools.cpp, F10, Alt, End, F8 (SDL event filter)
// jfg_mods.cpp (jfg_register_mod_exports)
void jfg_register_asset_mods(void);                                    // jfg_assets.cpp: the jfg_assets.json content type
// jfg_glue.cpp (jfg_on_init)
void jfg_mods_alias_overlays(void);                                    // jfg_mods.cpp: overlay funcs at private addresses

// --- functions mods import ("*"; registered by jfg_register_mod_exports) -------------------------------------------
void jfg_recomp_printf(uint8_t* rdram, recomp_context* ctx);           // jfg_mods.cpp: recomp_printf

} // extern "C"

// --- game functions the port wraps (tools/wrapped_funcs.txt) -------------------------------------------------------
// The port never changes a game function itself: tools/wrap_functions.py renames the recompiled <name> to <name>_orig
// and makes every call to <name> run jfg_wrap_<name>, defined with this macro. The wrapper runs the port's code and
// calls <name>_orig; mods' patches and hooks apply to <name>_orig, so the wrapper keeps running around them.
#define JFG_WRAP(name)                                                     \
    extern "C" void name##_orig(uint8_t* rdram, recomp_context* ctx);     \
    extern "C" void jfg_wrap_##name(uint8_t* rdram, recomp_context* ctx)

// A wrapper that acts on one caller's own call of another function. The caller's wrapper opens a JfgCaller::Scope
// around its _orig with the caller's frame size (its prologue's addiu $sp); the callee's wrapper checks direct(ctx): a
// call made from deeper down (by a function the caller called) is entered with a lower $sp.
struct JfgCaller {
    struct Scope {
        Scope(JfgCaller& c, recomp_context* ctx, uint32_t frame) : c(c), prev(c.sp) { c.sp = ctx->r29 - frame; }
        ~Scope() { c.sp = prev; }
        JfgCaller& c;
        gpr prev;
    };
    bool direct(const recomp_context* ctx) const { return sp != 0 && ctx->r29 == sp; }
    gpr sp = 0;
};

#include <span>
#include <vector>
// src/main.cpp, GameEntry::decompression_routine: the ROM the runtime regenerates hooked functions from, with every
// overlay pre-linked as tools/prepare_overlays.py does for the build (jfg_mods.cpp).
std::vector<uint8_t> jfg_prelink_rom(std::span<const uint8_t> rom);
