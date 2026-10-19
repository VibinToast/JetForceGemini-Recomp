// Jet Force Gemini port: every function the rest of the program calls in src/jfg, by caller.
//
// They have C linkage. The game hooks are called from the recompiled game code, from the [[patches.hook]] entries of
// jfg.us.toml (which declare them in their `text`); the others from the base project's code as changed by
// patches/jfg-base.patch (which declares them where they are used). Most are defined inside namespace jfg in the file
// named below; with extern "C" they keep their plain name.
#pragma once

#include <cstdint>

#include "recomp.h"

extern "C" {

// --- game hooks (jfg.us.toml) ---------------------------------------------------------------------------------------
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
int jfg_squad_frames(uint8_t* rdram, uint32_t member, int frames);     // SquaddieControl, SquadronControl, squadsModuleTick, FlowerBoltControl
int jfg_cadence_skip(void);                                            // frontPlayerTarget, the target lock countdown
void jfg_squad_draw_begin(uint8_t* rdram);                             // func_80045F00, right before trackDraw
void jfg_dangle(uint8_t* rdram, recomp_context* ctx);                  // replaces dAngle (angle easing)
void jfg_squad_draw_end(uint8_t* rdram);                               // func_80045F00, right after trackDraw
// jfg_controls.cpp
void jfg_move_scale(uint8_t* rdram, recomp_context* ctx);              // entry of objMoveXYZ
void jfg_boss_tick(void);                                              // entry of boyBossControl, dogBossControl, girlBossControl
void jfg_platform_move(int on);                                        // controlPlatform, around its objMoveXYZ call
void jfg_floyd_steer(uint8_t* rdram, recomp_context* ctx, int phase);  // sidekickControl, around Floyd's roll easing
// jfg_camera.cpp
void jfg_camera_override(uint8_t* rdram, recomp_context* ctx);         // func_80045F00, right before the scene is drawn
int jfg_freeze_world(void);                                            // func_80045F00, world tick or pause tick
// jfg_visuals.cpp
float jfg_fov(uint8_t* rdram, float fov, uint32_t zoomed);             // camSetFOV
void jfg_game_fog(uint8_t* rdram, uint32_t sp);                        // func_8001BF9C, right after trackGetFog
uint32_t jfg_hit_flash(uint32_t color);                                // entry of frontFlashScreen
// jfg_gameplay.cpp
int jfg_no_blood(void);                                                // entry of bloodSpurtAdd, bloodHitSpriteAdd
int jfg_refuel_amount(int amount);                                     // refuelControl
float jfg_sniper_zoom(float zoom);                                     // controlGetManualAim
int jfg_sniper_walk(int weapon);                                       // the characters' aim control (overlays 15-17)
int jfg_flag_override(int flag);                                       // entry of mainGetGameFlag
// jfg_area_progress.cpp
void jfg_region_reset_begin(uint8_t* rdram, recomp_context* ctx);      // entry of mainResetRegion
void jfg_region_reset_end(uint8_t* rdram, recomp_context* ctx);        // mainResetRegion, before it returns
// jfg_weapons.cpp
uint32_t jfg_anim_speed(uint8_t* rdram, uint32_t obj, uint32_t speed_bits); // entry of objAnimDframe
void jfg_mine_init(uint8_t* rdram, recomp_context* ctx);               // end of mineInit
void jfg_bolt_init(uint8_t* rdram, recomp_context* ctx);               // end of laserboltInit
// jfg_diagnostics.cpp
void jfg_mm_alloc_check(uint8_t* rdram, recomp_context* ctx);          // mmAlloc and mmAlloc2, after mempool_slot_find
void jfg_squad_spawn_before(uint8_t* rdram, recomp_context* ctx);      // overlay 3 +0x14C9C, the squad member loop
void jfg_squad_spawn_after(uint8_t* rdram, recomp_context* ctx);       // overlay 3 +0x14CA4, after its call
void jfg_objsetup_enter(uint8_t* rdram, recomp_context* ctx);          // entry of objSetupObject
void jfg_objsetup_step(uint8_t* rdram, recomp_context* ctx, int step); // objSetupObject, after each call that can fail
void jfg_objsetup_exit(uint8_t* rdram, recomp_context* ctx);           // objSetupObject, its exit (0x80006E04)
void jfg_squads_check_enemy(uint8_t* rdram, recomp_context* ctx);      // replaces squadsCheckEnemyPointers
void jfg_game_break(uint8_t* rdram, recomp_context* ctx);              // mathBreakPoint
void jfg_change_level_log(uint8_t* rdram, recomp_context* ctx);        // entry of mainChangeLevel
void jfg_sync_printf(uint8_t* rdram, recomp_context* ctx);             // replaces osSyncPrintf
// jfg_glue.cpp
void jfg_overlay_loaded(uint8_t* rdram, recomp_context* ctx);          // runlinkDownloadCode, runlinkResumeCode
void jfg_overlay_unloading(uint8_t* rdram, recomp_context* ctx);       // runlinkFreeCode, runlinkUnloadOverlay
uint32_t jfg_cart_read32(uint32_t address);                            // osCartTest3_6105, osCartTest4_6105

// --- base project (jfg-base.patch) ----------------------------------------------------------------------------------
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
int jfg_widescreen_active(void);                                       // jfg_visuals.cpp
int jfg_texture_three_point(void);                                     // jfg_visuals.cpp
int jfg_output_filtering(void);                                        // jfg_visuals.cpp
int jfg_skip_render_active(void);                                      // jfg_cutscene_skip.cpp
// src/main.cpp
void jfg_on_init(uint8_t* rdram, recomp_context* ctx);                 // jfg_glue.cpp

} // extern "C"
