// Jet Force Gemini port: wrappers around the game's engine functions (scheduler, video, audio, the main loop, overlay
// loading) and around calls the port watches from inside one game function. The port's code is in the handlers they
// call (jfg_hooks.h); none of them is an API event. A wrapper that only acts for one caller's own call checks it with
// JfgCaller (jfg_hooks.h).
#include <cstdint>

#include "recomp.h"
#include "jfg_hooks.h"

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
