// Jet Force Gemini port: frame pacing - 60 fps, the squad system at the game's own 30, getting back from 20 to 30 fps
// after a hitch, the cutscene skip's fast-forward and the FPS counter.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_rt64.h"

namespace jfg {

std::atomic<float> g_fps{ 0.0f }; // game frames per second, for the "Show FPS" option
std::atomic<float> g_screen_fps{ 0.0f };

// Scheduler thread, every video retrace (hook in __scHandleRetrace, see jfg.us.toml): 1 = count the retrace twice, so
// the scheduler hands each finished frame back after one retrace instead of two (60 fps instead of 30).
extern "C" int jfg_sched_extra_frame(void) {
    return g.frame_rate >= 1 ? 1 : 0;
}

// Read by the renderer thread (src/renderer/renderer.cpp) for the "Frame rate" option: -1 = keep the Graphics menu's
// Framerate setting, 0 = Original (the frames the game draws), 1 = Display (RT64 adds in-between frames up to the
// monitor's refresh rate).
extern "C" int jfg_refresh_override(void) {
    switch (g.frame_rate) {
        case 1: return 0;
        case 2: return 1;
        default: return -1;
    }
}

// --- 60 fps: the squad system at the game's own 30 fps cadence --------------------------------------------------------
// Enemies, tribals and the other squad members (overlay 3: SquaddieControl per member, SquadronControl per group,
// squadsModuleTick once a tick) are passed the video frames a tick covers, but much of what they do is still per tick:
// a member moves by its velocity once per SquaddieControl call, behaviours roll their chances and step their counters
// once per call. Measured in Forest Clear (level 54) with the frame rate at 60: ants, tribals and drones moved about twice
// as fast in real time as at the original rate (median 298 against 144 units a second, game time 60 frames a second in
// both), animated faster and attacked more often. With this, a squad member covers the same distance per update as at
// the original rate (8.13 against 8.19 units, measured) at 30 updates a second.
// In 60 fps mode the whole squad system therefore runs on every second tick only: all three functions are left out on
// the other tick, and the frame count they get covers both ticks (2, or more after a slow tick). That is exactly the
// cadence the game has at 30 fps, so squad members behave as they do there; everything else stays at 60. Squad members
// are then also drawn moving 30 times a second.
// The decision is made once per tick in jfg_frame_sync (squad_cadence_next_tick), so a squadron and its members always
// run on the same tick.
namespace {
uint32_t g_game_ticks = 0;          // main loop ticks (jfg_frame_sync)
uint32_t g_squad_frames = 0;        // video frames the squad system has not been given yet, including this tick's
bool g_squad_runs = true;           // the squad system runs this tick
}

// Called with the frames of the tick about to run.
void squad_cadence_next_tick(uint32_t frames) {
    if (g_squad_runs) g_squad_frames = 0;     // the previous tick handed them over
    g_squad_frames += std::max<uint32_t>(frames, 1);
    // Only 60 fps ticks are paired up; at the original rate (2 or 3 frames a tick) and while a cutscene is skipped
    // (6 frames a tick) every tick runs as in the game.
    g_squad_runs = g.frame_rate < 1 || g.skipping || g_squad_frames >= 2;
}

// Hooks at the entry of SquaddieControl(obj, frames), SquadronControl(obj, frames) and squadsModuleTick(frames): the frame
// count to run with this tick, or 0 to leave the call out. `member` is the squad member (SquaddieControl), else 0.
extern "C" int jfg_squad_frames(uint8_t* rdram, uint32_t member, int frames) {
    g_rdram = rdram;
    (void)member;
    if (g.frame_rate < 1 || g.skipping) return frames;
    return g_squad_runs ? (int)g_squad_frames : 0;
}

// Entry of viFrameSync(mesg). Returns 1 when the call has been handled here (skip in progress).
// Frame pacing log (JFG_DEBUG_PACING=1): per tick, how long the tick took up to viFrameSync and in viFrameSync.
std::chrono::steady_clock::time_point g_pace_tick_start, g_pace_sync_start;
extern "C" int jfg_vi_skip(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    g_pace_sync_start = std::chrono::steady_clock::now(); { static const bool pl = std::getenv("JFG_DEBUG_PACING") != nullptr; if (pl) std::fprintf(stderr, "[pace] vi queue at entry: %d\n", (int)rd32(0x800FEB80 + 8)); }
    if (g.frame_rate >= 1 && !g.skipping) {
        // 60 fps. viFrameSync waits until gVideoDeltaTime retraces have been counted (runInOneFrame makes that 1) and
        // then always waits for one more retrace after the buffer swap. The scheduler only starts a graphics task at a
        // retrace, so a tick has always seen one retrace by the time it gets here; that extra wait made every tick two
        // retraces long. Same steps as viFrameSync (src/gameVi.c) without it: wait until at least one retrace has
        // passed since the last tick, swap, and report how many passed (the time the next tick simulates).
        constexpr uint32_t ADDR_sBlackScreenTimer = 0x800FECA9, ADDR_gSkipVideoDeltaAdjust = 0x800FECAD, ADDR_gVideoMesgQueue = 0x800FEB80;
        const uint8_t black = rd8(ADDR_sBlackScreenTimer);
        if (black != 0) {
            wr8(ADDR_sBlackScreenTimer, (uint8_t)(black - 1));
            if (black == 1) call_game(osViBlack_recomp, ctx, { 0 });
        }
        if ((int32_t)ctx->r4 != 8) call_game(fb_swap, ctx, {}); // MESG_SKIP_BUFFER_SWAP
        uint32_t frames = 0;
        while ((int32_t)call_game(osRecvMesg_recomp, ctx, { ADDR_gVideoMesgQueue, 0, 0 /* NOBLOCK */ }) != -1) frames++;
        if (frames == 0) { call_game(osRecvMesg_recomp, ctx, { ADDR_gVideoMesgQueue, 0, 1 /* BLOCK */ }); frames = 1; }
        call_game(osViSwapBuffer_recomp, ctx, { rd32(ADDR_otherScreen) });
        wr8(ADDR_gSkipVideoDeltaAdjust, 0);
        ctx->r2 = frames;
        return 1;
    }
    if (!g.skipping) return 0;
    if (g_audio_catch_up) { // run the real viFrameSync once: it blocks until a video frame has passed
        g_audio_catch_up = false;
        g_skip_real_frames++;
        return 0;
    }
    if ((uint32_t)ctx->r4 != 0xFFFFFFFFu) call_game(fb_swap, ctx, {}); // as viFrameSync: unless MESG_SKIP_BUFFER_SWAP
    call_game(osViSwapBuffer_recomp, ctx, { rd32(ADDR_otherScreen) });
    ctx->r2 = 6;
    return 1;
}

// Right after viFrameSync() returned in the main loop tick: $v0 = video frames the next tick simulates (clamped to 6).
extern "C" uint32_t jfg_frame_sync(uint8_t* rdram_, uint32_t frames) {
    g_rdram = rdram_;
    g_game_ticks++;
    {
        static const bool pacing = std::getenv("JFG_DEBUG_PACING") != nullptr;
        const auto now = std::chrono::steady_clock::now();
        if (pacing && g_pace_tick_start != decltype(g_pace_tick_start){}) {
            std::fprintf(stderr, "[pace] work %.1f ms sync %.1f ms frames %u delta %u counter %u\n",
                std::chrono::duration<double, std::milli>(g_pace_sync_start - g_pace_tick_start).count(),
                std::chrono::duration<double, std::milli>(now - g_pace_sync_start).count(), frames,
                rd8(0x800FECAC), rd8(0x800FECAB));
        }
        g_pace_tick_start = now;
    }
    {
        // Getting back to 30 fps after a hitch. viFrameSync (src/gameVi.c) drops the game to 20 fps (3 video frames a
        // tick, gVideoDeltaTime) as soon as one tick takes 3 retraces, and only returns to 30 after 30 ticks in a row
        // that took fewer. Under the recompilation's timing the 20 fps state locks itself in: the ticks alternate
        // between 2 and 4 retraces (the scheduler holds back each frame's "done" until 2 retraces have passed), so
        // every second tick counts as slow and the run of 30 never comes - one long frame (a texture pack being
        // loaded, the window losing focus) left the game at 20 fps for good. Measured with JFG_DEBUG_PACING and a slowed
        // VI clock: at 20 fps the ticks went 70 / 230 ms of work forever; set back to 30 fps, the same scene held it.
        // So after a while at 20 fps the game is put back to 30 fps; if a tick is slow again it drops straight back by
        // itself (one slightly long frame), and the next try waits twice as long (up to about half a minute).
        constexpr uint32_t ADDR_gVideoDeltaTime = 0x800FECAC, ADDR_gVideoDeltaCounter = 0x800FECAB;
        static int ticks_at_20 = 0, wait_ticks = 45, held_ticks = 0;
        static bool trying = false;
        static const bool log = std::getenv("JFG_DEBUG_PACING") != nullptr;
        const uint8_t delta = rd8(ADDR_gVideoDeltaTime);
        static int level_seen = -1;
        const int level_now = (int16_t)rd16(ADDR_currentLevel);
        if (level_now != level_seen) { level_seen = level_now; wait_ticks = 45; } // a new area: start trying early again
        if (g.skipping || rd32(ADDR_runInOneFrame) != 0 || (delta != 2 && delta != 3)) {
            ticks_at_20 = 0; trying = false;
        }
        else if (delta == 3) {
            if (trying) { // dropped again while trying: the game really needs 20 fps here for now
                trying = false;
                wait_ticks = std::min(wait_ticks * 2, 600);
                if (log) std::fprintf(stderr, "[pace] back to 20 fps after %d ticks; next try in %d ticks\n", held_ticks, wait_ticks);
            }
            // Switched right after one of the long ticks (counter 0), the next tick is a short one and 30 fps starts
            // cleanly; switched before a long one, it drops back at once.
            if (++ticks_at_20 >= wait_ticks && rd8(ADDR_gVideoDeltaCounter) == 0) {
                ticks_at_20 = 0; trying = true; held_ticks = 0;
                wr8(ADDR_gVideoDeltaTime, 2);
                wr8(ADDR_gVideoDeltaCounter, 0);
                if (log) std::fprintf(stderr, "[pace] trying 30 fps again\n");
            }
        }
        else { // 30 fps
            ticks_at_20 = 0;
            if (trying && ++held_ticks >= 30) { trying = false; wait_ticks = 45; if (log) std::fprintf(stderr, "[pace] 30 fps holds\n"); }
        }
    }
    {
        static auto fps_t0 = std::chrono::steady_clock::now();
        static int fps_n = 0;
        fps_n++;
        const double dt = std::chrono::duration<double>(std::chrono::steady_clock::now() - fps_t0).count();
        if (dt >= 0.5) {
            static uint32_t last_presents = 0;
            const uint32_t presents = rt64_jfg_present_count.load(std::memory_order_relaxed);
            g_fps.store((float)(fps_n / dt), std::memory_order_relaxed);
            g_screen_fps.store((float)((presents - last_presents) / dt), std::memory_order_relaxed);
            last_presents = presents;
            fps_n = 0; fps_t0 = std::chrono::steady_clock::now();
        }
    }
    {
        static const bool stats = std::getenv("JFG_DEBUG_STATS") != nullptr;
        static const int stats_every = stats ? std::atoi(std::getenv("JFG_DEBUG_STATS")) : 0; // JFG_DEBUG_STATS=<ticks between lines>, default 200
        static uint32_t n = 0, sum = 0;
        static auto t0 = std::chrono::steady_clock::now();
        if (stats) {
            sum += frames;
            if (++n == (stats_every > 0 ? stats_every : 200)) {
                const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::fprintf(stderr, "[jfg] ticks/s %.1f, mean frames per tick %.2f, level %d, skipping %d, tune %d state %d\n", n / secs, sum / double(n), (int)(int16_t)rd16(ADDR_currentLevel), int(g.skipping),
                    (int)rd8(ADDR_tuneSeqNo), valid_ptr(rd32(ADDR_tuneSeqPlayer)) ? (int)rd32(rd32(ADDR_tuneSeqPlayer) + 0x2C) : -1);
                if (g_cam.player != 0) std::fprintf(stderr, "[jfg] player at %.0f %.0f %.0f, speed factor %.2f\n", rdf(g_cam.player + 0x0C), rdf(g_cam.player + 0x10), rdf(g_cam.player + 0x14), g_cam.move_factor);
                std::fprintf(stderr, "[jfg] robotMission %d, dual stick active %d, game mode %d, vi mode %d, next level %d\n", (int)rd8(ADDR_robotMission), jfg_dual_stick_active(), (int)rd32(ADDR_mainGameMode), (int)rd8(ADDR_sResolutionIndex), (int)(int16_t)rd16(ADDR_nextLevel));
                { load_level_names(); const int lv = (int16_t)rd16(ADDR_currentLevel); if (lv >= 0 && lv < (int)g_level_names.size()) std::fprintf(stderr, "[jfg] level name '%s'\n", g_level_names[lv].c_str()); }
                n = 0; sum = 0; t0 = std::chrono::steady_clock::now();
            }
        }
    }
    {
        // Scripted testing: JFG_DEBUG_REFLECT=<secs> lists the level's reflective floor batches once;
        // JFG_DEBUG_TELEPORT="secs,x,y,z" moves the player there once.
        static const char* const refl = std::getenv("JFG_DEBUG_REFLECT");
        static const char* const tele = std::getenv("JFG_DEBUG_TELEPORT");
        static const auto start = std::chrono::steady_clock::now();
        static bool done_r = false, done_t = false;
        const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (refl != nullptr && !done_r && secs >= std::atof(refl)) {
            done_r = true;
            const uint32_t track = rd32(0x800A0D60);
            if (valid_ptr(track)) {
                const int nseg = (int16_t)rd16(track + 0x1A);
                const uint32_t segs = rd32(track + 4);
                for (int i = 0; i < nseg; i++) {
                    const uint32_t seg = segs + 0x48 * i;
                    const uint32_t verts = rd32(seg), tris = rd32(seg + 4), batches = rd32(seg + 0xC);
                    const int nb = (int16_t)rd16(seg + 0x28);
                    for (int b = 0; b < nb; b++) {
                        const uint32_t bt = batches + 0x10 * b;
                        const uint32_t flags = rd32(bt + 0xC);
                        if (!(flags & 0x20000000)) continue;
                        const int vo = (int16_t)rd16(bt + 6), fo = (int16_t)rd16(bt + 8), vn = (int16_t)rd16(bt + 0x16), fn = (int16_t)rd16(bt + 0x18);
                        int x0 = 99999, x1 = -99999, z0 = 99999, z1 = -99999, y = 0;
                        for (int f = fo; f < fn; f++) for (int k = 1; k <= 3; k++) {
                            const uint32_t v = verts + 10 * (vo + rd8(tris + 16 * f + k));
                            const int x = (int16_t)rd16(v), z = (int16_t)rd16(v + 4); y = (int16_t)rd16(v + 2);
                            x0 = std::min(x0, x); x1 = std::max(x1, x); z0 = std::min(z0, z); z1 = std::max(z1, z);
                        }
                        std::fprintf(stderr, "[jfg] reflect seg %d batch %d flags %08X tex %d y %d x[%d,%d] z[%d,%d] faces %d..%d (next vo %d)\n", i, b, flags, (int)rd8(bt), y, x0, x1, z0, z1, fo, fn, vn);
                    }
                }
            }
        }
        if (tele != nullptr && !done_t && g_cam.player != 0) {
            float t = 0, x = 0, y = 0, z = 0;
            std::sscanf(tele, "%f,%f,%f,%f", &t, &x, &y, &z);
            if (secs >= t) {
                done_t = true;
                const uint32_t obj = g_cam.player, data = rd32(obj + 0x68);
                wrf(obj + 0x0C, x); wrf(obj + 0x10, y); wrf(obj + 0x14, z);
                if (valid_ptr(data)) { wrf(data + 0x3C, x); wrf(data + 0x40, y); wrf(data + 0x44, z); }
            }
        }
    }
    static const int force_frames = std::getenv("JFG_DEBUG_FORCE_FRAMES") ? std::atoi(std::getenv("JFG_DEBUG_FORCE_FRAMES")) : 0; // tests: frames per tick
    const uint32_t result = (force_frames > 0 && !g.skipping) ? (uint32_t)force_frames : (g.skipping ? 6 : frames);
    squad_cadence_next_tick(result);
    return result;
}

} // namespace jfg
