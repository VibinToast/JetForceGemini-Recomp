// Jet Force Gemini port: START skips cutscenes, the logo screens and the story intro.
//
// START during a cutscene runs the game flat out until the cutscene has ended:
//   - jfg_after_joy (hook after joyRead in the main loop tick) sees the START press, swallows it and starts the skip;
//   - while skipping, jfg_vi_skip (hook at the entry of viFrameSync) does the buffer swap but none of the waiting for
//     video retraces, and jfg_frame_sync (hook after viFrameSync returns) makes every tick simulate 6 video frames,
//     the most the game allows. Nothing is bypassed in the game's logic, so whatever the cutscene sets up (flags,
//     items, the level it leads to) still happens; it just takes a fraction of a second instead of its real length.
// The skip ends when the level changes, when the cutscene is over, or after a time limit.
// A cutscene is recognised by the letterboxed VI mode the game switches to for most of them (sResolutionIndex bit 0)
// or by the level being one of the developers' sequence levels (name containing "Seq" or "Intro").
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

bool level_is_sequence(int level) {
    load_level_names();
    if (level < 0 || level >= (int)g_level_names.size()) return false;
    const std::string& n = g_level_names[level];
    return contains_ci(n, "seq") || contains_ci(n, "intro");
}

bool in_cutscene() {
    if (rd32(ADDR_mainGameMode) != 0) return false; // not in the game proper (front end, title)
    return (rd8(ADDR_sResolutionIndex) & 1) != 0 || level_is_sequence((int16_t)rd16(ADDR_currentLevel));
}

// Two stretches of the front end that START may also skip:
//  - the company and logo screens after power-on: level 0 until the title screen (level 349) has been reached once
//    (level 0 is also what is loaded behind the file-select menus later, which must not be skipped);
//  - the story intro after a new file has been created (levels 322-327, see jfg_after_joy): not fast-forwarded
//    (it is timed to its music, so it does not run faster) but left for character select directly.
constexpr int LEVEL_SPLASH = 0, LEVEL_TITLE = 349;
bool g_title_reached = false;
bool in_front_end_scene() {
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (rd32(ADDR_mainGameMode) == 0) return false;
    if (level == LEVEL_TITLE) g_title_reached = true;
    return level == LEVEL_SPLASH && !g_title_reached;
}
inline bool skippable_scene() { return in_cutscene() || in_front_end_scene(); }

std::chrono::steady_clock::time_point g_skip_started;
int g_skip_ticks = 0;
bool g_audio_catch_up = false; // a music player's event queue is nearly full: let the audio thread run
int g_skip_real_frames = 0;
std::thread::id g_main_thread; // the game's main thread (set in jfg_after_joy)
std::atomic<bool> jfg_skip_render{ false };
extern "C" int jfg_skip_render_active(void) { return jfg_skip_render.load(std::memory_order_relaxed) ? 1 : 0; } // read by the render thread

extern "C" void jfg_after_joy(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    g_main_thread = std::this_thread::get_id();
    jfg_camera_controls(ctx);
    {
        // Scripted testing: JFG_DEBUG_INPUT="secs,dur,buttons,x,y;..." holds N64 buttons (hex mask) and the stick on
        // controller 1 for dur seconds from secs seconds after launch (the buttons count as pressed on the first tick).
        static const char* const inp = std::getenv("JFG_DEBUG_INPUT");
        struct Step { float t, dur; unsigned buttons; int x, y; bool started; };
        static std::vector<Step> steps;
        static bool parsed = false;
        static const auto start = std::chrono::steady_clock::now();
        if (inp != nullptr && !parsed) {
            parsed = true;
            for (const char* p = inp; *p; ) {
                Step st{}; int n = 0;
                if (std::sscanf(p, "%f,%f,%x,%d,%d%n", &st.t, &st.dur, &st.buttons, &st.x, &st.y, &n) >= 5) steps.push_back(st);
                const char* q = std::strchr(p, ';'); if (q == nullptr) break; p = q + 1;
            }
        }
        const float now = std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
        const uint32_t pad = ADDR_sControllerCurrData + 6u * (rd8(ADDR_sPlayerID) & 3u);
        for (Step& st : steps) {
            if (now < st.t || now >= st.t + st.dur) continue;
            wr16(pad, (uint16_t)(rd16(pad) | st.buttons));
            if (st.x != 0 || st.y != 0) { wr8(pad + 2, (uint8_t)(int8_t)st.x); wr8(pad + 3, (uint8_t)(int8_t)st.y); }
            if (!st.started) { st.started = true; wr16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u), (uint16_t)(rd16(ADDR_buttonsPressed + 2u * (rd8(ADDR_sPlayerID) & 3u)) | st.buttons)); }
        }
    }
    if (g_reset_press_start) { // F5 while paused: close the pause screen first (see jfg_debug_frame)
        g_reset_press_start = false;
        const uint32_t i = rd8(ADDR_sPlayerID) & 3u;
        wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) | START_BUTTON));
        return;
    }
    static const bool env_skip = std::getenv("JFG_SKIP_CUTSCENES") != nullptr; // scripted tests: skip every cutscene
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (!g.skipping) {
        bool start = false;
        for (uint32_t i = 0; i < 4; i++) start |= (rd16(ADDR_buttonsPressed + 2 * i) & START_BUTTON) != 0;
        { static const bool log = std::getenv("JFG_DEBUG_STATS") != nullptr; if (log && start) std::fprintf(stderr, "[jfg] START pressed: level %d, game mode %d, skippable %d, option %d\n", level, (int)rd32(ADDR_mainGameMode), int(skippable_scene()), int(g.start_skips_cutscenes)); }
        if (g.start_skips_cutscenes && start && level >= LEVEL_STORY_FIRST && level <= LEVEL_STORY_LAST) {
            // Story intro after creating a file: 322 JFG ship, 324 ship look-in, 323 control room, 325 ant attack,
            // 326 spawn attack, 327 character select sequence. These scenes wait for their music, so fast-forwarding
            // them only blacks the screen out. Go to character select the way the file-select menu does instead
            // (logged: mainChangeLevel(127, 0, 0, 5, 1, 0)); jfg_debug_frame makes the call at the next tick.
            if (!g_story_skip_pending) std::fprintf(stderr, "[jfg] story intro skip (level %d)\n", level);
            g_story_skip_pending = true;
            for (uint32_t i = 0; i < 4; i++) wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) & ~START_BUTTON));
        }
        else if (((g.start_skips_cutscenes && start) || env_skip) && skippable_scene()) {
            g.skipping = true;
            g.skip_level = level;
            g_skip_started = std::chrono::steady_clock::now();
            g_skip_ticks = 0;
            g_skip_real_frames = 0;
            g_audio_catch_up = false;
            jfg_skip_render = true;
            std::fprintf(stderr, "[jfg] cutscene skip: start (level %d)\n", level);
            // The game must not also see this START press (it could open the pause menu and stall the cutscene).
            for (uint32_t i = 0; i < 4; i++) wr16(ADDR_buttonsPressed + 2 * i, (uint16_t)(rd16(ADDR_buttonsPressed + 2 * i) & ~START_BUTTON));
        }
    }
    else {
        // Safety limits: 10 minutes of cutscene (6 frames per tick) or one minute of real time.
        const bool timeout = ++g_skip_ticks > 6000 || std::chrono::steady_clock::now() - g_skip_started > std::chrono::seconds(60);
        if (level != g.skip_level && skippable_scene() && !timeout) {
            // One scene led straight into another (new game: intro -> arrival cutscene): keep going.
            std::fprintf(stderr, "[jfg] cutscene skip: continues (level %d -> %d)\n", g.skip_level, level);
            g.skip_level = level;
        }
        else if (level != g.skip_level || !skippable_scene() || timeout) {
            std::fprintf(stderr, "[jfg] cutscene skip: end (level %d -> %d%s)\n", g.skip_level, level, timeout ? ", time limit" : "");
            std::fprintf(stderr, "[jfg] cutscene skip: %d ticks (%d with a real frame for the audio thread) in %.2f s; tune %d state %d\n", g_skip_ticks, g_skip_real_frames, std::chrono::duration<double>(std::chrono::steady_clock::now() - g_skip_started).count(),
                (int)rd8(ADDR_tuneSeqNo), valid_ptr(rd32(ADDR_tuneSeqPlayer)) ? (int)rd32(rd32(ADDR_tuneSeqPlayer) + 0x2C) : -1);
            g.skipping = false;
            jfg_skip_render = false;
        }
    }
}

// Entry of rcpFast3d(start, end, type, flags), which sends a graphics task to the scheduler. Returns 1 to drop the task.
// Type 0 is the frame's main task (sent at the top of the main loop tick); 3, 4 and 5 are the clone, refraction and
// blur side tasks of the same frame. The decision is taken at the main task and kept until the next one, so a frame's
// tasks are either all sent or all dropped (rcpWaitDP waits for the main task whenever any of them was sent).
extern "C" int jfg_rcp_skip(uint8_t* rdram_, uint32_t type) {
    g_rdram = rdram_;
    static bool dropping = false;
    if (type == 0) {
        if (dropping) { // side tasks dropped last frame left their "active" flags set; rcpWaitDP would wait for them
            wr32(ADDR_cloneTaskActive, 0);
            wr32(ADDR_refractTaskActive, 0);
            wr32(ADDR_blurTaskActive, 0);
        }
        dropping = g.skipping;
    }
    return dropping ? 1 : 0;
}

// Entry of ad_sndp_play(bank, sound, volume, pan, pitch, ?, handlePtr). Returns 1 to drop the sound (cutscene skip in
// progress); the caller's sound handle, the 7th argument at 0x18($sp), is then set to NULL.
extern "C" int jfg_snd_skip(uint8_t* rdram_, uint32_t sp) {
    g_rdram = rdram_;
    if (!g.skipping) return 0;
    const uint32_t handle_ptr = rd32(sp + 0x18);
    if (handle_ptr >= 0x80000000u && handle_ptr < 0x80800000u) wr32(handle_ptr, 0);
    return 1;
}

// Sound-effect player events (gsSnd.c) and the fields of its event queue (US version: count of queued events at +0x10,
// of events dropped because the queue was full at +0x12) and of the player (frameTime at +0x4C).
constexpr uint16_t AL_SNDP_PAN_EVT = 1 << 2, AL_SNDP_VOL_EVT = 1 << 3, AL_SNDP_PITCH_EVT = 1 << 4, AL_SNDP_API_EVT = 1 << 5,
                   AL_SNDP_FX_EVT = 1 << 8, AL_SNDP_STOP_EVT = 1 << 10, AL_SNDP_GROUP_VOL_EVT = 1 << 11;
constexpr uint16_t SNDP_UPDATE_EVTS = AL_SNDP_PAN_EVT | AL_SNDP_VOL_EVT | AL_SNDP_PITCH_EVT | AL_SNDP_FX_EVT | AL_SNDP_GROUP_VOL_EVT;
constexpr int SNDP_RESERVE = 16; // free queue items kept for everything but the game thread's updates

int free_events(uint32_t evtq, int limit) { // evtq->freeList.next chain, counted up to limit
    int n = 0;
    for (uint32_t p = rd32(evtq); p >= 0x80000000u && p < 0x80800000u && n < limit; p = rd32(p)) n++;
    return n;
}

// Entry of n_alEvtqPostEvent(evtq, evt, delta). Returns 1 to drop the event: during a skip, everything the game's main
// thread posts except "stop" events, and those too if the queue is nearly full (see jfg.us.toml). Outside a skip, the
// main thread's volume, pan, pitch and effect updates of sounds already playing when the sound-effect queue (200
// items) is nearly full: once it is full the game drops whatever comes next, the player's own periodic event too,
// which froze the game (see jfg_evt_next). Starting and stopping sounds is never dropped there.
extern "C" int jfg_evt_skip(uint8_t* rdram_, uint32_t evtq, uint32_t evt) {
    if (std::this_thread::get_id() != g_main_thread) return 0;
    g_rdram = rdram_;
    if (!g.skipping) {
        if (evtq != rd32(ADDR_gSoundPlayerPtr) + 0x14 || (rd16(evt) & SNDP_UPDATE_EVTS) == 0) return 0;
        return free_events(evtq, SNDP_RESERVE) < SNDP_RESERVE ? 1 : 0;
    }
    const int free_items = free_events(evtq, 12);
    if (evtq != rd32(ADDR_gSoundPlayerPtr) + 0x14) {
        // A sequence player (music, ambience): everything goes through, or the music of the skipped cutscene would
        // never be stopped or replaced. If its queue is filling up, the next tick waits for a real video frame so the
        // audio thread can work the queue off (jfg_vi_skip).
        if (free_items < 12) g_audio_catch_up = true;
        return free_items < 2 ? 1 : 0;
    }
    if (rd16(evt) != AL_SNDP_STOP_EVT) return 1;
    return free_items < 8 ? 1 : 0;
}

// Entry of n_alEvtqNextEvent(evtq, evt). The sound-effect player keeps one periodic event (AL_SNDP_API_EVT) in its queue
// at all times and posts it again each time it comes up, so the queue is never empty here. If it is, that event was
// lost (the queue was full when it was posted again), and the player's handler (func_80084468) would freeze the game:
// an empty queue returns a "nonsense" event due right now, and the handler asks for the next one until one is not due
// right now, forever (audio thread at 100%, picture stuck). The event is put back first, as the player itself posts it.
extern "C" void jfg_evt_next(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t evtq = (uint32_t)ctx->r4;
    const uint32_t sndp = rd32(ADDR_gSoundPlayerPtr);
    if (sndp == 0 || evtq != sndp + 0x14 || rd32(evtq + 8) != 0 || rd32(evtq) == 0) return; // other queue, events queued, or no free item
    const uint32_t evt = ((uint32_t)ctx->r29 - 0x40u) & ~0x7u; // inside the gap call_game leaves below $sp
    for (uint32_t i = 0; i < 0x10; i += 4) wr32(evt + i, 0);
    wr16(evt, AL_SNDP_API_EVT);
    call_game(n_alEvtqPostEvent, ctx, { evtq, evt, rd32(sndp + 0x4C) });
    static int logged = 0;
    if (logged++ < 3) std::fprintf(stderr, "[jfg] sound player lost its periodic event (%u events dropped so far); put back\n", (unsigned)rd16(evtq + 0x12));
}

} // namespace jfg
