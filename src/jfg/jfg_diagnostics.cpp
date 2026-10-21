// Jet Force Gemini port: logs and error reports - out of memory, the game's own error breaks, level changes and the
// game's osSyncPrintf messages.
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <unordered_map>

#include "ultramodern/error_handling.hpp"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

// --- out of memory and game assertions --------------------------------------------------------------------------
constexpr uint32_t ADDR_gFreeQueueCount = 0x800FDD88; // s32: frees waiting out mmSetDelay() ticks (gFreeQueue)
constexpr uint32_t ADDR_FreeRAM = 0x800FDD90;         // s32: free bytes in the main heap

void jfg_log_line(const char* file, const char* text) {
    std::fprintf(stderr, "[jfg] %s\n", text);
    if (std::FILE* f = std::fopen(file, "a")) {
        const std::time_t now = std::time(nullptr);
        char stamp[32];
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", std::localtime(&now));
        std::fprintf(f, "%s  %s\n", stamp, text);
        std::fclose(f);
    }
}

// mmAlloc and mmAlloc2 (the same function twice), after mempool_slot_find ($v0 = block or 0; size and colour are at
// 0x30/0x34($sp)). A level load asks for
// big blocks (the track buffer is 0x9F000 bytes, see func_overlay_24_000001F8) while earlier frees may still wait in
// the delayed free queue. When the heap has no room, release that queue and try once more; log either way to
// bin/jfg_memory.log.
extern "C" void jfg_mm_alloc_check(uint8_t* rdram_, recomp_context* ctx, uint32_t size, uint32_t colour) {
    {
        // Tests: JFG_DEBUG_FAIL_ALLOC=<size hex> makes the first allocation of that size look failed (it leaks).
        static const char* const fail = std::getenv("JFG_DEBUG_FAIL_ALLOC");
        static bool failed_once = false;
        if (fail != nullptr && !failed_once && (uint32_t)ctx->r2 != 0) {
            g_rdram = rdram_;
            if (size == (uint32_t)std::strtoul(fail, nullptr, 16)) { failed_once = true; ctx->r2 = 0; }
        }
    }
    if ((uint32_t)ctx->r2 != 0) return;
    g_rdram = rdram_;
    static thread_local bool retrying = false;
    if (retrying) return;
    const int pending = (int)rd32(ADDR_gFreeQueueCount);
    uint32_t block = 0;
    if (pending > 0) {
        retrying = true;
        call_game(mmFlushFreeStack, ctx, {});
        block = call_game(mmAlloc, ctx, { size, colour });
        retrying = false;
    }
    char line[256];
    std::snprintf(line, sizeof(line), "mmAlloc(0x%X) found no room (level %d, next %d, %d delayed frees); %s", size, (int)(int16_t)rd16(ADDR_currentLevel),
        (int)(int16_t)rd16(ADDR_nextLevel), pending, block != 0 ? "retry after releasing them worked" : "still no room");
    jfg_log_line("jfg_memory.log", line);
    if (block != 0) ctx->r2 = (gpr)(int32_t)block;
}

// The squad system's loop that brings a squadron's members into play (overlay 3, the loop at +0x14C9C in
// func_overlay_3_000148C0): while squadron->created (+0x63) < squadron->wanted (+0x01), func_overlay_3_00014694 reuses a
// deactivated member or creates a new one. When creating one fails (objSetupObject returns NULL: seen with 15 KB of
// heap left, and with 200 KB) nothing is counted, so the loop tried again forever: the game froze.
// The hooks around that call count a pass that added nobody, as the game itself counts a member it does not bring back;
// the squadron then has one member fewer. $s0 is the squadron.
static uint8_t g_squad_created = 0;
static std::string g_last_obj_failure = "none seen"; // the latest failed objSetupObject (jfg_objsetup_exit), for the line below
extern "C" void jfg_squad_spawn_before(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    g_squad_created = rd8((uint32_t)ctx->r16 + 0x63);
}
extern "C" void jfg_squad_spawn_after(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t squadron = (uint32_t)ctx->r16;
    if (rd8(squadron + 0x63) != g_squad_created) return;
    wr8(squadron + 0x63, (uint8_t)(g_squad_created + 1));
    static int logged = 0;
    if (logged++ >= 20) return;
    char line[600];
    std::snprintf(line, sizeof(line), "squad member could not be created (level %d, %d bytes of heap free, %d delayed frees); left out. Last failed objSetupObject: %s",
        (int)(int16_t)rd16(ADDR_currentLevel), (int)rd32(ADDR_FreeRAM), (int)rd32(ADDR_gFreeQueueCount), g_last_obj_failure.c_str());
    jfg_log_line("jfg_memory.log", line);
}

// objSetupObject(entry, flags) returning NULL: which of its steps failed, logged to jfg_memory.log. A squadron member and
// a gun turret's shot could not be created in level 248 with 200 KB of heap free (the shot's NULL then crashed
// squadsFireWeapon), so the heap is not the only thing that runs out. Hooks: the entry, the return of each call that can
// fail (step numbers below), and the shared exit at 0x80006E04 ($v0 = the object, the type at 0x7A($sp)). Calls nest
// (an object's children), so the record is kept per depth.
namespace {
constexpr uint32_t ADDR_ObjListCount = 0x800F2CA8, ADDR_NoAddObjListCount = 0x800F2CB8;
constexpr int OBJ_STEPS = 8, OBJ_DEPTH = 8;
const char* const OBJ_STEP_NAMES[OBJ_STEPS] = { "objdef", "8000F978", "texture", "model", "model2", "sprite", "allocR", "alloc" };
struct ObjSetupRecord { bool ran[OBJ_STEPS]; uint32_t result[OBJ_STEPS]; };
ObjSetupRecord g_obj_setup[OBJ_DEPTH];
int g_obj_depth = 0;
}
extern "C" void jfg_objsetup_enter(uint8_t* rdram_, recomp_context*) {
    g_rdram = rdram_;
    if (g_obj_depth < OBJ_DEPTH) g_obj_setup[g_obj_depth] = ObjSetupRecord{};
    g_obj_depth++;
}
extern "C" void jfg_objsetup_step(uint8_t* rdram_, recomp_context* ctx, int step) {
    g_rdram = rdram_;
    const int d = g_obj_depth - 1;
    if (d < 0 || d >= OBJ_DEPTH || step < 0 || step >= OBJ_STEPS) return;
    g_obj_setup[d].ran[step] = true;
    g_obj_setup[d].result[step] = (uint32_t)ctx->r2;
}
extern "C" void jfg_objsetup_exit(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const int d = --g_obj_depth;
    if (g_obj_depth < 0) g_obj_depth = 0;
    if ((uint32_t)ctx->r2 != 0 || d < 0 || d >= OBJ_DEPTH) return;
    const int type = (int16_t)rd16((uint32_t)ctx->r29 + 0x7A);
    std::string steps;
    uint32_t signature = (uint32_t)type << 16; // the type, which steps ran, which of them returned 0
    for (int i = 0; i < OBJ_STEPS; i++) {
        if (!g_obj_setup[d].ran[i]) continue;
        signature ^= (1u << i) | ((g_obj_setup[d].result[i] == 0 ? 1u : 0u) << (i + 8));
        char s[32];
        std::snprintf(s, sizeof(s), " %s=%08X", OBJ_STEP_NAMES[i], g_obj_setup[d].result[i]);
        steps += s;
    }
    char line[400];
    std::snprintf(line, sizeof(line), "type %d, level %d, %d objects (+%d not listed), %d bytes of heap free, %d delayed frees; steps:%s",
        type, (int)(int16_t)rd16(ADDR_currentLevel), (int)rd32(ADDR_ObjListCount), (int)rd32(ADDR_NoAddObjListCount),
        (int)rd32(ADDR_FreeRAM), (int)rd32(ADDR_gFreeQueueCount), steps.empty() ? " none" : steps.c_str());
    g_last_obj_failure = line;
    // The first 3 of each kind of failure (one that repeats every frame must not use up the log), 300 lines at most.
    static std::unordered_map<uint32_t, int> seen;
    static int logged = 0;
    if (++seen[signature] > 3 || logged++ >= 300) return;
    jfg_log_line("jfg_memory.log", (std::string("objSetupObject failed: ") + line).c_str());
}

// Replaces squadsCheckEnemyPointers(obj), run whenever an object is freed: it clears every squad member's enemy pointer
// to that object (squadrons in D_800A38E4[count D_800FE9F0]; a squadron's data at obj+0x68, its first member at
// data+0x58; a member's data at obj+0x68, its enemy at data+0x64, the next member at data+0x28). In a boss fight a
// member reached through those links had data 0x005E0000, a member object freed while still linked (its memory reused),
// and reading it crashed the game. Same work, but each pointer is checked before it is followed, a chain is followed at
// most 256 links, and a broken one is left at that point and logged (jfg_memory.log). Nothing is changed in the links.
static bool rdram_ptr(uint32_t p) { return p >= 0x80000000u && p < 0x80800000u && (p & 3) == 0; }
extern "C" void jfg_squads_check_enemy(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    constexpr uint32_t ADDR_squadronList = 0x800A38E4, ADDR_squadronCount = 0x800FE9F0;
    const uint32_t freed = (uint32_t)ctx->r4;
    const int count = (int)rd32(ADDR_squadronCount);
    const uint32_t list = rd32(ADDR_squadronList);
    if (count <= 0 || !rdram_ptr(list)) return;
    static int logged = 0;
    auto broken = [&](int squadron, int link, const char* what, uint32_t value) {
        if (logged++ >= 20) return;
        char line[256];
        std::snprintf(line, sizeof(line), "squad list: %s 0x%08X (squadron %d of %d, link %d, level %d); that chain skipped",
            what, value, squadron, count, link, (int)(int16_t)rd16(ADDR_currentLevel));
        jfg_log_line("jfg_memory.log", line);
    };
    for (int i = count - 1; i >= 0; i--) {
        const uint32_t node = rd32(list + 4u * (uint32_t)i);
        if (!rdram_ptr(node)) { broken(i, -1, "bad squadron object", node); continue; }
        const uint32_t data = rd32(node + 0x68);
        if (!rdram_ptr(data)) { broken(i, -1, "bad squadron data", data); continue; }
        uint32_t member = rd32(data + 0x58);
        for (int link = 0; member != 0; link++) {
            if (link >= 256) { broken(i, link, "chain too long at member", member); break; }
            if (!rdram_ptr(member)) { broken(i, link, "bad member object", member); break; }
            const uint32_t m = rd32(member + 0x68);
            if (!rdram_ptr(m)) { broken(i, link, "bad member data", m); break; }
            if (rd32(m + 0x64) == freed) wr32(m + 0x64, 0);
            member = rd32(m + 0x28);
        }
    }
}

// mathBreakPoint: the game stopped itself on a failed check (on the N64 this is a debugger break). Leave a note and a
// message instead of letting the runtime's exit() run its shutdown (which has crashed and left an empty crash.log).
extern "C" void jfg_game_break(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    char line[320];
    std::snprintf(line, sizeof(line), "game error (break) called from 0x%08X; level %d, next level %d, game mode %d, last mmAlloc result 0x%08X",
        (uint32_t)ctx->r31, (int)(int16_t)rd16(ADDR_currentLevel), (int)(int16_t)rd16(ADDR_nextLevel), (int)rd32(ADDR_mainGameMode), (uint32_t)ctx->r2);
    jfg_log_line("jfg_error.log", line);
    std::string msg = std::string("Jet Force Gemini stopped with an internal game error.\n\n") + line +
        "\n\nThe details were saved to jfg_error.log (and jfg_memory.log, if it exists) in the game folder.";
    ultramodern::error_handling::message_box(msg.c_str());
    std::fflush(nullptr);
    std::_Exit(1);
}

// An internal handler of jfg_on_level_change (mainChangeLevel's wrapper, jfg_api.cpp): logs the arguments with
// JFG_DEBUG_STATS.
extern "C" void jfg_change_level_log(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    static const bool log = std::getenv("JFG_DEBUG_STATS") != nullptr;
    if (!log) return;
    const uint32_t sp = (uint32_t)ctx->r29;
    std::fprintf(stderr, "[jfg] mainChangeLevel(%d, %d, %d, %d, %d, %d) from level %d, game mode %d, ra %08X\n", (int)ctx->r4, (int)ctx->r5, (int)ctx->r6, (int)ctx->r7,
        (int)rd32(sp + 0x10), (int)rd32(sp + 0x14), (int)(int16_t)rd16(ADDR_currentLevel), (int)rd32(ADDR_mainGameMode), (uint32_t)ctx->r31);
}

// Replaces osSyncPrintf(fmt, ...): the format string goes to stderr, unformatted.
extern "C" void jfg_sync_printf(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t fmt = (uint32_t)ctx->r4;
    static int budget = 200; // a warning repeated every frame must not flood the log
    if (budget <= 0 || fmt < 0x80000000u || fmt >= 0x80800000u) return;
    budget--;
    char buf[160]; size_t n = 0;
    for (; n < sizeof(buf) - 1; n++) { const char c = (char)rd8(fmt + (uint32_t)n); if (c == 0) break; buf[n] = c; }
    while (n > 0 && buf[n - 1] == '\n') n--;
    buf[n] = 0;
    std::fprintf(stderr, "[game] %s  (args %08X %08X %08X)\n", buf, (uint32_t)ctx->r5, (uint32_t)ctx->r6, (uint32_t)ctx->r7);
}

} // namespace jfg

// mmAlloc(size, colour) and mmAlloc2 (the same function twice): jfg_mm_alloc_check on the result.
JFG_WRAP(mmAlloc) {
    const uint32_t size = (uint32_t)ctx->r4, colour = (uint32_t)ctx->r5;
    mmAlloc_orig(rdram, ctx);
    jfg_mm_alloc_check(rdram, ctx, size, colour);
}

JFG_WRAP(mmAlloc2) {
    const uint32_t size = (uint32_t)ctx->r4, colour = (uint32_t)ctx->r5;
    mmAlloc2_orig(rdram, ctx);
    jfg_mm_alloc_check(rdram, ctx, size, colour);
}

// The game's assertion ("break"): logged and reported before the process ends. Without this the runtime's exit()
// could crash in its shutdown and leave nothing behind.
JFG_WRAP(mathBreakPoint) {
    jfg_game_break(rdram, ctx);
    mathBreakPoint_orig(rdram, ctx);
}

// Replaced by the port's own code (they don't call the game's). squadsCheckEnemyPointers runs whenever an object is
// freed; the port's copy checks each squad link before following it (in a boss fight the game's reached a squad
// member freed while still linked and crashed).
JFG_WRAP(squadsCheckEnemyPointers) {
    jfg_squads_check_enemy(rdram, ctx);
}

// osSyncPrintf's output routine (0x8009A710) is reached through a function pointer the recompiler has no entry for,
// which stopped the program the first time the game printed a warning ("out of sound states").
JFG_WRAP(osSyncPrintf) {
    jfg_sync_printf(rdram, ctx);
}
