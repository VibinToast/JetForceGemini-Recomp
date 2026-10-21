// Jet Force Gemini port: wrappers around the game's engine functions (scheduler, video, audio, the main loop, overlay
// loading) and around calls the port watches from inside one game function. The port's code is in the handlers they
// call (jfg_hooks.h); none of them is an API event. A wrapper that only acts for one caller's own call checks it with
// JfgCaller (jfg_hooks.h).
#include <cstdint>

#include "recomp.h"
#include "jfg_hooks.h"

extern "C" int jfg_overlay_is_loaded(uint32_t ram); // jfg_glue.cpp

// --- RSP tasks, audio, scheduler: the port can leave a call out --------------------------------------------------------
// Cutscene skip (jfg_cutscene_skip.cpp, jfg_frame_rate.cpp): while a cutscene is skipped no graphics task is sent
// (rcpFast3d returns at once), sounds it starts are dropped (ad_sndp_play's handle, its 7th argument on the stack, is
// set to NULL), and the sound player's update events are dropped except "stop": a skip runs hundreds of ticks before
// the audio thread gets a turn, and a full event queue made the player lose its periodic event for good. If that
// happened anyway, jfg_evt_next puts the event back before the queue is read (otherwise the audio thread spun forever
// on "nonsense" events and the game froze).
JFG_WRAP(rcpFast3d) {
    if (jfg_rcp_skip(rdram, (uint32_t)ctx->r6)) { ctx->r2 = 0; return; }
    rcpFast3d_orig(rdram, ctx);
}

JFG_WRAP(ad_sndp_play) {
    if (jfg_snd_skip(rdram, (uint32_t)ctx->r29)) { ctx->r2 = 0; return; }
    ad_sndp_play_orig(rdram, ctx);
}

JFG_WRAP(n_alEvtqPostEvent) {
    if (jfg_evt_skip(rdram, (uint32_t)ctx->r4, (uint32_t)ctx->r5)) return;
    n_alEvtqPostEvent_orig(rdram, ctx);
}

JFG_WRAP(n_alEvtqNextEvent) {
    jfg_evt_next(rdram, ctx);
    n_alEvtqNextEvent_orig(rdram, ctx);
}

JFG_WRAP(__scTaskReady) {
    const int r = jfg_sched_task_ready();
    if (r == 1) { ctx->r2 = ctx->r4; return; }
    if (r == 0) { ctx->r2 = 0; return; }
    __scTaskReady_orig(rdram, ctx);
}

// --- the main loop tick (func_80044FAC) ----------------------------------------------------------------------------
// Each tick: jfg_debug_frame before it; jfg_after_joy right after its joyRead (before it stores the result); and
// jfg_frame_sync on what its viFrameSync returns (the frames the next tick simulates). joyRead is called from inside
// the tick by other code too.
static JfgCaller g_tick;

JFG_WRAP(func_80044FAC_45BAC) {
    jfg_debug_frame(rdram, ctx);
    JfgCaller::Scope scope(g_tick, ctx, 0x50);
    func_80044FAC_45BAC_orig(rdram, ctx);
}

JFG_WRAP(joyRead) {
    const bool tick = g_tick.direct(ctx);
    joyRead_orig(rdram, ctx);
    if (tick) jfg_after_joy(rdram, ctx);
}

JFG_WRAP(viFrameSync) {
    const bool tick = g_tick.direct(ctx);
    if (!jfg_vi_skip(rdram, ctx)) viFrameSync_orig(rdram, ctx);
    if (tick) ctx->r2 = (gpr)(int32_t)jfg_frame_sync(rdram, (uint32_t)ctx->r2);
}

// --- fog: func_8001BF9C reads the level's fog with trackGetFog into its stack ($sp + 0x38..) ------------------------
// Fog options (jfg_visuals.cpp): near (s16 at $sp + 0x3E) and far (+0x3C) may be rewritten before they go in the
// display list.
static JfgCaller g_fog;

JFG_WRAP(func_8001BF9C_1CB9C) {
    JfgCaller::Scope scope(g_fog, ctx, 0x40);
    func_8001BF9C_1CB9C_orig(rdram, ctx);
}

JFG_WRAP(trackGetFog) {
    const bool fog = g_fog.direct(ctx);
    trackGetFog_orig(rdram, ctx);
    if (fog) jfg_game_fog(rdram, (uint32_t)ctx->r29); // the caller's $sp, as trackGetFog returned
}

// --- overlays: the runtime learns where the game's own linker put an overlay, and when it frees one -------------------
// runlinkDownloadCode and runlinkResumeCode: before the romCopy of the overlay's code (from the header's ROM address
// to its load address; $fp = the OverlayHeader). Nothing in the overlay runs before its copies are done.
// runlinkFreeCode / runlinkUnloadOverlay: before the mmFree of the overlay's own block, which comes after the overlay's
// exit function has run (it still needs the mapping). runlinkSuspendCode frees through runlinkFreeCode.
static int g_runlink = 0; // 1 = in runlinkDownloadCode, 2 = in runlinkResumeCode, 3 = freeing
static bool g_runlink_loaded = false;

JFG_WRAP(runlinkDownloadCode) {
    const int prev = g_runlink; const bool prev_loaded = g_runlink_loaded;
    g_runlink = 1; g_runlink_loaded = false;
    runlinkDownloadCode_orig(rdram, ctx);
    g_runlink = prev; g_runlink_loaded = prev_loaded;
}

JFG_WRAP(runlinkResumeCode) {
    const int prev = g_runlink; const bool prev_loaded = g_runlink_loaded;
    g_runlink = 2; g_runlink_loaded = false;
    runlinkResumeCode_orig(rdram, ctx);
    g_runlink = prev; g_runlink_loaded = prev_loaded;
}

JFG_WRAP(romCopy) {
    const gpr fp = ctx->r30;
    if ((g_runlink == 1 || g_runlink == 2) && !g_runlink_loaded && (uint32_t)ctx->r4 == (uint32_t)MEM_W(4, fp) &&
        (uint32_t)ctx->r5 == (uint32_t)MEM_W(0, fp)) {
        g_runlink_loaded = true;
        jfg_overlay_loaded(rdram, ctx);
    }
    romCopy_orig(rdram, ctx);
}

JFG_WRAP(runlinkFreeCode) {
    const int prev = g_runlink;
    g_runlink = 3;
    runlinkFreeCode_orig(rdram, ctx);
    g_runlink = prev;
}

JFG_WRAP(runlinkUnloadOverlay) {
    const int prev = g_runlink;
    g_runlink = 3;
    runlinkUnloadOverlay_orig(rdram, ctx);
    g_runlink = prev;
}

JFG_WRAP(mmFree) {
    if (g_runlink == 3 && jfg_overlay_is_loaded((uint32_t)ctx->r4)) jfg_overlay_unloading(rdram, ctx);
    mmFree_orig(rdram, ctx);
}
