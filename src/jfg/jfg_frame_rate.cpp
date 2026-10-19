// Jet Force Gemini port: frame pacing - 60 fps, the squad system at the game's own 30, getting back from 20 to 30 fps
// after a hitch, the cutscene skip's fast-forward and the FPS counter.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "librecomp/overlays.hpp"

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

// 60 fps: the next frame's graphics task waits for the buffer swap. The game double-buffers: after asking the VI for the
// frame it has just finished (jfg_vi_skip), it sends the next frame's task at once, and that task draws into the other
// buffer - the one still on screen until the VI applies the swap at the next retrace. The game's scheduler holds such
// a task back (__scTaskReady: "pending swap"), but with the runtime's VI that check still reports the swap as pending at
// the retrace that applies it, so it cost a retrace every frame and is bypassed at 60 fps. Without any hold, though, RT64
// rendered the next frame into the buffer on screen before the retrace, and at the retrace showed that new picture -
// without the things the game draws with the CPU after the graphics task (the reticles of most weapons, see
// enable_instant_present in renderer.cpp), which then never reached the screen. Whether it went wrong depended on where
// in the frame the tick ended, so it changed with frame-rate switches (measured with an event trace). So the task is
// held until the retrace that applies the swap has come (counted here: the hardware's rule, without the runtime's late
// flag) and RT64 has taken that retrace's screen update, which shows the buffer the task draws into one last time. The
// second part matters because the runtime's VI thread queues the screen update and the scheduler queues the task from
// different threads, and its queue does not keep their order: the trace showed the task drawn first. RT64 renders a
// frame in about 2 ms, so this keeps 60 fps; the picture is one frame later.
namespace {
std::atomic<uint32_t> g_retraces{ 0 };      // __scHandleRetrace calls (scheduler thread)
std::atomic<uint32_t> g_swap_retrace{ 0 };  // g_retraces when the last swap was asked for
std::atomic<uint32_t> g_swap_screens{ 0 };  // rt64_jfg_screen_updates then
std::atomic<uint32_t> g_swap_draw_fb{ 0 };  // the buffer the next frame draws into (physical address)
std::atomic<bool> g_swap_pending{ false };
}

// Entry of __scHandleRetrace: one more retrace, before the scheduler looks at its waiting tasks.
extern "C" void jfg_sched_retrace(void) {
    g_retraces.fetch_add(1, std::memory_order_acq_rel);
}

// Entry of __scTaskReady: 1 = the task may start, 0 = not yet (a swap is still pending), -1 = the game's own check.
extern "C" int jfg_sched_task_ready(void) {
    if (g.frame_rate < 1) { g_swap_pending.store(false, std::memory_order_release); return -1; }
    if (!g_swap_pending.load(std::memory_order_acquire)) return 1;
    if (g_retraces.load(std::memory_order_acquire) == g_swap_retrace.load(std::memory_order_acquire)) return 0;
    // The applying retrace has come. Its screen update was queued just before the scheduler was told; wait (briefly, it
    // takes RT64 well under a millisecond) until RT64 has taken it, so the task cannot be drawn before it.
    const uint32_t screens = g_swap_screens.load(std::memory_order_acquire), fb = g_swap_draw_fb.load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4);
    while (!(rt64_jfg_screen_updates.load(std::memory_order_acquire) != screens && rt64_jfg_screen_fb.load(std::memory_order_acquire) == fb)) {
        if (std::chrono::steady_clock::now() > deadline) break;
        std::this_thread::yield();
    }
    g_swap_pending.store(false, std::memory_order_release);
    return 1;
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
uint32_t g_tick_frames = 1;         // video frames this tick simulates
struct SquadMember { uint32_t obj, def; }; // a member SquaddieControl was called for this tick, and its definition then
std::vector<SquadMember> g_squad_members;
int g_squad_members_level = -1;     // the level they were noted in
void squad_draw_restore();
}

// Called with the frames of the tick about to run.
void squad_cadence_next_tick(uint32_t frames) {
    squad_draw_restore(); // normally done right after trackDraw already
    g_squad_members.clear();
    g_tick_frames = std::max<uint32_t>(frames, 1);
    if (g_squad_runs) g_squad_frames = 0;     // the previous tick handed them over
    g_squad_frames += g_tick_frames;
    // Only 60 fps ticks are paired up; at the original rate (2 or 3 frames a tick) and while a cutscene is skipped
    // (6 frames a tick) every tick runs as in the game.
    g_squad_runs = g.frame_rate < 1 || g.skipping || g_squad_frames >= 2;
}

// Hooks at the entry of SquaddieControl(obj, frames), SquadronControl(obj, frames) and squadsModuleTick(frames): the frame
// count to run with this tick, or 0 to leave the call out. `member` is the squad member (SquaddieControl), else 0.
extern "C" int jfg_squad_frames(uint8_t* rdram, uint32_t member, int frames) {
    g_rdram = rdram;
    if (member != 0) {
        g_squad_members.push_back({ member, rd32(member + 0x40) });
        g_squad_members_level = (int16_t)rd16(ADDR_currentLevel);
    }
    if (g.frame_rate < 1 || g.skipping) return frames;
    return g_squad_runs ? (int)g_squad_frames : 0;
}

// The squad cadence without hooks inside the three functions, so mods can hook them (a mod hook regenerates a function
// without this port's [[patches.hook]] text, and enemies ran twice as fast at 60 fps again). All their callers are in
// the main segment and reach them through the runtime's function table (main -> overlay calls are JFG_DYN_CALL
// lookups), so when overlay 3 loads, these wrappers take their place in that table: the same decision the entry hooks
// made, then a call to the real function (the original, or a mod's regenerated one: mods patch it in place).
namespace {
constexpr uint32_t OVERLAY_3_ROM = 0x01ED54E0;                 // jfg.us.syms.toml, .overlay_3
constexpr uint32_t OFS_SquaddieControl = 0x10600, OFS_SquadronControl = 0x14D4C, OFS_squadsModuleTick = 0x16990;
recomp_func_t* g_real_SquaddieControl = nullptr;
recomp_func_t* g_real_SquadronControl = nullptr;
recomp_func_t* g_real_squadsModuleTick = nullptr;

extern "C" void jfg_wrap_SquaddieControl(uint8_t* rdram, recomp_context* ctx) {
    const int r = jfg_squad_frames(rdram, (uint32_t)ctx->r4, (int32_t)ctx->r5);
    if (r == 0) return;
    ctx->r5 = (gpr)r;
    g_real_SquaddieControl(rdram, ctx);
}

extern "C" void jfg_wrap_SquadronControl(uint8_t* rdram, recomp_context* ctx) {
    const int r = jfg_squad_frames(rdram, 0, (int32_t)ctx->r5);
    if (r == 0) return;
    ctx->r5 = (gpr)r;
    g_real_SquadronControl(rdram, ctx);
}

extern "C" void jfg_wrap_squadsModuleTick(uint8_t* rdram, recomp_context* ctx) {
    const int r = jfg_squad_frames(rdram, 0, (int32_t)ctx->r4);
    if (r == 0) { ctx->r2 = 0; return; }
    ctx->r4 = (gpr)r;
    g_real_squadsModuleTick(rdram, ctx);
}
} // namespace

// Called from jfg_overlay_loaded after the runtime has entered the overlay's functions at its load address.
extern "C" void jfg_squad_wrappers_install(uint32_t rom, uint32_t ram) {
    if (rom != OVERLAY_3_ROM) return;
    const auto& by_rom = recomp::overlays::get_vrom_to_section_map();
    auto it = by_rom.find(rom);
    if (it == by_rom.end()) return;
    g_real_SquaddieControl = recomp::overlays::get_func_by_section_index_function_offset(it->second, OFS_SquaddieControl);
    g_real_SquadronControl = recomp::overlays::get_func_by_section_index_function_offset(it->second, OFS_SquadronControl);
    g_real_squadsModuleTick = recomp::overlays::get_func_by_section_index_function_offset(it->second, OFS_squadsModuleTick);
    if (!g_real_SquaddieControl || !g_real_SquadronControl || !g_real_squadsModuleTick) {
        std::fprintf(stderr, "[jfg] squad cadence: overlay 3's functions not found; enemies run at the tick rate\n");
        return;
    }
    recomp::overlays::add_loaded_function((int32_t)(ram + OFS_SquaddieControl), jfg_wrap_SquaddieControl);
    recomp::overlays::add_loaded_function((int32_t)(ram + OFS_SquadronControl), jfg_wrap_SquadronControl);
    recomp::overlays::add_loaded_function((int32_t)(ram + OFS_squadsModuleTick), jfg_wrap_squadsModuleTick);
}

// Steps outside the squad system that the game takes once per call whatever the frame count (found by the decomp,
// docs/PORT_FINDINGS.md there): 1 = leave this tick's step out. They are taken on the ticks the squad system runs, so
// they keep the cadence they have at 30 fps.
extern "C" int jfg_cadence_skip(void) {
    return (g.frame_rate >= 1 && !g.skipping && !g_squad_runs) ? 1 : 0;
}

// --- 60 fps: angles that ease towards a target ---------------------------------------------------------------------
// Replaces dAngle(angle, target, t) (charControl.c): angle + (shortest signed difference * t), the product truncated.
// Callers ease an angle with t = 1 - damping^frames (Powerf), so with one frame per tick t is about half what it is at
// 30 fps, and the step truncates to 0 while the angle is still a little short of the target: it never arrives. Mizar's
// first phase waits for his rotation to reach 0x8000 exactly (mizarControl, the three dAngle calls at +0x301C..): at
// 60 fps it stayed at 0x8020 and the fight did not go on until the frame rate was switched back (2026-10-18). Same
// arithmetic as the game; in 60 fps mode only, a step that truncates to 0 with a difference left and t > 0 is 1 unit.
extern "C" void jfg_dangle(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    const int32_t angle = (int16_t)(uint32_t)ctx->r4;
    const int32_t target = (int32_t)ctx->r5;
    const uint32_t t_bits = (uint32_t)ctx->r6;
    float t;
    std::memcpy(&t, &t_bits, sizeof(t));
    int32_t diff = (target - angle) & 0xFFFF;
    const int32_t back = (angle - target) & 0xFFFF;
    if (back < diff) diff = -back;
    const float product = (float)diff * t;
    int32_t step = (product > -2147483648.0f && product < 2147483648.0f) ? (int32_t)product : 0x7FFFFFFF; // cvt.w.s, toward 0
    if (step == 0 && diff != 0 && t > 0.0f && g.frame_rate >= 1 && !g.skipping) step = diff > 0 ? 1 : -1;
    ctx->r2 = (gpr)(int32_t)(int16_t)(angle + step);
}

// --- 60 fps: squad members drawn in between their updates ---------------------------------------------------------
// With the squad system on every second tick, squad members move and animate 30 times a second while everything else
// does 60. So on a tick where the squad system ran, each member is drawn halfway between where it was after its previous
// update and where it is now, and on the tick in between where it is: the drawn positions are evenly spaced, 60 a second,
// one video frame behind the game's own state. Only the drawing sees this: the values are written into the object right
// before trackDraw (jfg_squad_draw_begin) and put back right after it (jfg_squad_draw_end), the way the game itself
// shifts the player for drawing (objDoPlayerTumble / objUndoPlayerTumble). The game's logic, collision and speeds are
// untouched. With "60 + interpolated", RT64's in-between frames are then built from evenly spaced frames as well.
// Smoothed: the position (object + 0x0C, three floats), the rotation (+0x00, three s16 angles) and the animation phase
// (+0x28, 0..1, objAnimDframe) while the animation (+0x3B) stays the same; the model instance is asked to pose its
// skeleton again from them (modGenAnimMatrices, below). Left as they are: a member that has moved
// further than a jump (a respawn, a teleport), whose object now holds something else (+0x40, the object definition),
// and every tick that is not the usual 60 fps pattern (slow ticks, the original rate and a cutscene skip, where the
// squad system runs every tick anyway). JFG_NO_SQUAD_SMOOTH=1 turns it off, for comparing.
namespace {
struct SquadPose {
    float x = 0, y = 0, z = 0, phase = 0;
    uint16_t rot[3] = {};
    int8_t anim = 0;
    uint32_t def = 0;
};
struct SquadTrack {
    SquadPose last;             // as after the member's last update
    uint32_t updated_tick = 0;  // g_game_ticks at that update, 0 = none
    uint32_t posed_tick = 0;    // g_game_ticks of the last in-between tick on which the game itself re-posed the model
};
std::unordered_map<uint32_t, SquadTrack> g_squad_tracks; // by object
// A pose is not read-only: through SquaddieGetTiltList (called by modGenAnimMatrices) it steps the member's head and
// torso aim angles (control data +0x8C..+0x9A, angle and speed pairs, by iDeltaTime, the frames of the last squad
// update), counts poses in +0x06, sets flags in +0x07 and clears +0x10 on animation 0x3F (decomp, docs/PORT_FINDINGS.md;
// a dragonfly's wing tilt steps +0xA8/+0xAA the same way). The pose asked for below comes on top of the game's own one
// on the tick in between, so those stepped twice as often. They are kept from before that pose and put back after the
// draw. What the pose computes from the drawn skeleton (head and joint positions +0x68..+0x8B, facing +0x9C) is left as
// drawn: it is recomputed by the next pose anyway, before the next squad update reads it.
struct SquadDataField { uint32_t offset, size; };
constexpr SquadDataField SQUAD_STEPPED[] = { { 0x06, 2 }, { 0x10, 2 }, { 0x8C, 0x10 }, { 0xA8, 4 } };
constexpr uint32_t SQUAD_STEPPED_BYTES = 2 + 2 + 0x10 + 4;
struct SquadDrawn {
    uint32_t obj;
    SquadPose real, drawn;
    uint32_t data = 0;                      // control data (object + 0x68) saved before an extra pose, 0 = none
    uint8_t saved[SQUAD_STEPPED_BYTES] = {};
};
std::vector<SquadDrawn> g_squad_drawn;                   // objects that hold drawn values until jfg_squad_draw_end
uint32_t g_squad_smoothed = 0;                           // members drawn in between, for JFG_DEBUG_STATS
uint32_t g_squad_data_kept = 0;                          // extra poses that stepped the control data (put back), for JFG_DEBUG_STATS

SquadPose read_pose(uint32_t obj) {
    SquadPose p;
    p.x = rdf(obj + 0x0C); p.y = rdf(obj + 0x10); p.z = rdf(obj + 0x14);
    p.phase = rdf(obj + 0x28);
    for (uint32_t i = 0; i < 3; i++) p.rot[i] = rd16(obj + 2 * i);
    p.anim = (int8_t)rd8(obj + 0x3B);
    p.def = rd32(obj + 0x40);
    return p;
}

// Puts back what the game had, field by field, unless the game itself changed the field while drawing.
void squad_draw_restore() {
    for (const SquadDrawn& d : g_squad_drawn) {
        const uint32_t o = d.obj;
        if (rdf(o + 0x0C) == d.drawn.x) wrf(o + 0x0C, d.real.x);
        if (rdf(o + 0x10) == d.drawn.y) wrf(o + 0x10, d.real.y);
        if (rdf(o + 0x14) == d.drawn.z) wrf(o + 0x14, d.real.z);
        if (rdf(o + 0x28) == d.drawn.phase) wrf(o + 0x28, d.real.phase);
        for (uint32_t i = 0; i < 3; i++) if (rd16(o + 2 * i) == d.drawn.rot[i]) wr16(o + 2 * i, d.real.rot[i]);
        if (d.data != 0) {
            bool changed = false;
            uint32_t n = 0;
            for (const SquadDataField& f : SQUAD_STEPPED) {
                for (uint32_t i = 0; i < f.size; i++, n++) {
                    if (rd8(d.data + f.offset + i) != d.saved[n]) { wr8(d.data + f.offset + i, d.saved[n]); changed = true; }
                }
            }
            if (changed) g_squad_data_kept++;
        }
    }
    g_squad_drawn.clear();
}

uint16_t half_angle(uint16_t from, uint16_t to) {
    return (uint16_t)(from + (int16_t)(uint16_t)(to - from) / 2);
}

// The member's model instance (obj->unk6C[obj->unk3A]), the one objPrintModelObject poses, or 0. Its +0x0A is the
// number of ticks to animate since the last pose: the draw poses the model when it is non-zero and clears it; +0x0B is
// which of its double-buffered joint matrices (+0x10) and vertices (+0x18, with the vertex colours) is current (0 or 1).
// Only for animated 3D models: the draw picks the object's kind from its definition (+0x40, then +0x1E: 0 model,
// 1 sprite, 4 custom; func_8000DDAC), and only for a model is +0x6C a list of model instances. For a sprite it is
// sprite data, and writing +0x0A there corrupted it (the level flickered and the game froze).
uint32_t model_instance(uint32_t obj) {
    const uint32_t def = rd32(obj + 0x40);
    if (!valid_ptr(def) || rd8(def + 0x1E) != 0) return 0;
    const uint32_t list = rd32(obj + 0x6C);
    if (!valid_ptr(list)) return 0;
    const int index = (int8_t)rd8(obj + 0x3A);
    if (index < 0 || index > 7) return 0;
    const uint32_t inst = rd32(list + 4u * (uint32_t)index);
    if (!valid_ptr(inst) || rd8(inst + 0x0B) > 1) return 0;
    const uint32_t model = rd32(inst); // the model: +0x4E non-zero = animated (objPrintModelObject's test)
    if (!valid_ptr(model) || rd8(model + 0x4E) == 0) return 0;
    return inst;
}

// The objects still in play: the game's two object lists (ObjList, NoAddObjList; objSetupObject puts each object in one).
// Members are noted when SquaddieControl runs (objObjectsTick), but objDoFrees runs after that and before trackDraw, so a
// member that died this tick may be freed by the time it is drawn. Writing a drawn pose into it then wrote into freed
// memory, which by then could belong to another object: squad links pointing nowhere, crashes and freezes in fights
// (2026-10-17). Only members found here are touched.
std::unordered_set<uint32_t> g_live_objects;
void read_live_objects() {
    constexpr uint32_t ADDR_ObjList = 0x800F2CA4, ADDR_ObjListCount = 0x800F2CA8, ADDR_NoAddObjList = 0x800F2CB4, ADDR_NoAddObjListCount = 0x800F2CB8;
    g_live_objects.clear();
    const uint32_t lists[2][2] = { { ADDR_ObjList, ADDR_ObjListCount }, { ADDR_NoAddObjList, ADDR_NoAddObjListCount } };
    for (const auto& l : lists) {
        const uint32_t list = rd32(l[0]);
        const int count = (int)rd32(l[1]);
        if (!valid_ptr(list) || count <= 0 || count > 4096) continue;
        for (int i = 0; i < count; i++) g_live_objects.insert(rd32(list + 4u * (uint32_t)i));
    }
}
bool live_object(uint32_t obj) { return valid_ptr(obj) && g_live_objects.count(obj) != 0; }
} // namespace

// func_80045F00, right before trackDraw (with jfg_camera_override).
extern "C" void jfg_squad_draw_begin(uint8_t* rdram) {
    g_rdram = rdram;
    squad_draw_restore();
    static const bool off = std::getenv("JFG_NO_SQUAD_SMOOTH") != nullptr;
    if (off || g.frame_rate < 1 || g.skipping) { g_squad_tracks.clear(); return; }
    // Around a level change nothing is smoothed and everything is forgotten: the next level's objects are set up in the
    // memory the last level's used, so a member noted this tick may be gone and its address hold a new object (a
    // crash going through a loading zone, 2026-10-18).
    const int level = (int16_t)rd16(ADDR_currentLevel);
    if (level != (int16_t)rd16(ADDR_nextLevel) || (!g_squad_members.empty() && level != g_squad_members_level)) {
        g_squad_tracks.clear();
        return;
    }
    read_live_objects();
    for (auto it = g_squad_tracks.begin(); it != g_squad_tracks.end();) { // freed: its memory may hold a new object later
        if (!live_object(it->first)) it = g_squad_tracks.erase(it); else ++it;
    }
    // Still in play and still the same object (the definition it had when it was noted).
    auto same = [](const SquadMember& m) { return live_object(m.obj) && rd32(m.obj + 0x40) == m.def; };
    if (!g_squad_runs) {
        // The tick in between: drawn where they are. Note which models the game poses itself this tick: only those
        // are animating, and only those are posed again on the next update tick (see below).
        for (const SquadMember& m : g_squad_members) {
            if (!same(m)) continue;
            const uint32_t obj = m.obj;
            const uint32_t inst = model_instance(obj);
            if (inst != 0 && rd8(inst + 0x0A) != 0) g_squad_tracks[obj].posed_tick = g_game_ticks;
        }
        return;
    }
    // The usual 60 fps pattern: this update covers this tick and the one before it, which had none.
    const bool even = g_squad_frames == 2 && g_tick_frames == 1;
    constexpr float JUMP = 300.0f; // squad members cover about 8 units per update
    for (const SquadMember& m : g_squad_members) {
        if (!same(m)) continue;
        const uint32_t obj = m.obj;
        SquadTrack& t = g_squad_tracks[obj];
        if (t.updated_tick == g_game_ticks) continue; // already handled this tick
        const SquadPose now = read_pose(obj);
        const SquadPose before = t.last;
        const bool had_update = t.updated_tick != 0 && g_game_ticks - t.updated_tick == 2;
        t.last = now;
        t.updated_tick = g_game_ticks;
        if (!even || !had_update || before.def != now.def) continue;
        const float dx = now.x - before.x, dy = now.y - before.y, dz = now.z - before.z;
        if (!(dx * dx + dy * dy + dz * dz < JUMP * JUMP)) continue;
        SquadPose mid = now;
        mid.x = before.x + dx * 0.5f; mid.y = before.y + dy * 0.5f; mid.z = before.z + dz * 0.5f;
        for (int i = 0; i < 3; i++) mid.rot[i] = half_angle(before.rot[i], now.rot[i]);
        if (now.anim == before.anim) {
            // The phase wraps from 1 back to 0 in looping animations: take the short way round.
            float d = now.phase - before.phase;
            if (d < -0.5f) d += 1.0f;
            else if (d > 0.5f) d -= 1.0f;
            float p = before.phase + d * 0.5f;
            if (p >= 1.0f) p -= 1.0f;
            else if (p < 0.0f) p += 1.0f;
            mid.phase = p;
        }
        wrf(obj + 0x0C, mid.x); wrf(obj + 0x10, mid.y); wrf(obj + 0x14, mid.z);
        wrf(obj + 0x28, mid.phase);
        for (uint32_t i = 0; i < 3; i++) wr16(obj + 2 * i, mid.rot[i]);
        // Animated models are posed by the draw only when their model instance asks for it (+0x0A), and squad
        // members' instances ask on the tick after their update, not on the update tick itself. Without this the
        // skeleton (and with it the whole model) would keep the pose from the tick before and move 30 times a second.
        // Each pose also flips the model's double buffers, so only models the game itself posed on the tick before
        // (animating, set up) and that are fully opaque (+0x39 = 0xFF; a dying member fading out is not) are asked:
        // posing the others made fading bodies blink and could pose a model that is not set up yet.
        // The value written is also the number of ticks objAnimTextures advances the model's texture animations by
        // (objResetAnimModels writes the tick's frames there for every model); 1 is this tick's.
        const uint32_t inst = model_instance(obj);
        SquadDrawn d{ obj, now, read_pose(obj) };
        if (inst != 0 && t.posed_tick == g_game_ticks - 1 && rd8(obj + 0x39) == 0xFF) {
            wr8(inst + 0x0A, 1);
            const uint32_t data = rd32(obj + 0x68);
            if (valid_ptr(data) && valid_ptr(data + 0xAC)) {
                d.data = data;
                uint32_t n = 0;
                for (const SquadDataField& f : SQUAD_STEPPED) {
                    for (uint32_t i = 0; i < f.size; i++) d.saved[n++] = rd8(data + f.offset + i);
                }
            }
        }
        g_squad_drawn.push_back(d);
        g_squad_smoothed++;
    }
    // Members not seen for a while (freed, out of play) are forgotten.
    if ((g_game_ticks & 63) == 0) {
        for (auto it = g_squad_tracks.begin(); it != g_squad_tracks.end();) {
            if (g_game_ticks - it->second.updated_tick > 8) it = g_squad_tracks.erase(it); else ++it;
        }
    }
}

// func_80045F00, right after trackDraw: the game gets its own values back.
extern "C" void jfg_squad_draw_end(uint8_t* rdram) {
    g_rdram = rdram;
    squad_draw_restore();
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
        // The next frame's task waits for the retrace that applies this swap (jfg_sched_task_ready).
        g_swap_retrace.store(g_retraces.load(std::memory_order_acquire), std::memory_order_release);
        g_swap_screens.store(rt64_jfg_screen_updates.load(std::memory_order_acquire), std::memory_order_release);
        g_swap_draw_fb.store(rd32(ADDR_currentScreen) & 0x1FFFFFFFu, std::memory_order_release);
        g_swap_pending.store(true, std::memory_order_release);
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
                std::fprintf(stderr, "[jfg] squad: %d members this tick, %u drawn in between since the last line, %u extra poses put back\n", (int)g_squad_members.size(), g_squad_smoothed, g_squad_data_kept);
                g_squad_smoothed = 0;
                g_squad_data_kept = 0;
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
