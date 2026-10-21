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
// squadsFireWeapon), so the heap is not the only thing that runs out. Its wrapper keeps a record per depth (calls nest:
// an object's children); the wrappers of the calls that can fail (step numbers below) note their result when
// objSetupObject made the call itself, not one of them. The type is objGetObjdef's argument.
namespace {
constexpr uint32_t ADDR_ObjListCount = 0x800F2CA8, ADDR_NoAddObjListCount = 0x800F2CB8;
constexpr int OBJ_STEPS = 8, OBJ_DEPTH = 8;
const char* const OBJ_STEP_NAMES[OBJ_STEPS] = { "objdef", "8000F978", "texture", "model", "model2", "sprite", "allocR", "alloc" };
struct ObjSetupRecord { bool ran[OBJ_STEPS]; uint32_t result[OBJ_STEPS]; int type; bool typed; };
ObjSetupRecord g_obj_setup[OBJ_DEPTH];
int g_obj_depth = 0;
int g_obj_step_nest = 0; // step calls under way: only the outermost (made by objSetupObject) is noted
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
    if (step == 4 && !g_obj_setup[d].ran[3]) step = 3; // modLoadModel: the first call is step 3, the second step 4
    g_obj_setup[d].ran[step] = true;
    g_obj_setup[d].result[step] = (uint32_t)ctx->r2;
}
extern "C" void jfg_objsetup_exit(uint8_t* rdram_, recomp_context* ctx, int type) {
    g_rdram = rdram_;
    const int d = --g_obj_depth;
    if (g_obj_depth < 0) g_obj_depth = 0;
    if ((uint32_t)ctx->r2 != 0 || d < 0 || d >= OBJ_DEPTH) return;
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
// most 256 links, and a broken one is left at that point and logged (jfg_memory.log). Nothing is changed in the links:
// this runs before TidyLinkedList, while a member being freed is still linked as it should be (see below).
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

// End of TidyLinkedList(obj) (overlay 3; objDoFrees calls it for a squad member being freed, after
// squadsCheckEnemyPointers): it takes obj out of its squadron's member chain, but looks only as far as the squadron's
// member count (+0x00, which it has just lowered). In the Lurg fight (level 195) a squadron's chain held one member more
// than its count (count 2, chain 3), so a member at the end was never found, stayed linked after being freed, and the
// next walk of the chain crashed in the reused memory (GalaxianBatsSoundsHandler, squadsCheckEnemyPointers; the level
// 138 boss fight too). After it (its wrapper) the whole chain is walked; if obj is still in it, it is taken out (the count
// is left as the game set it) and logged. Its member data: +0x24 the squadron.
extern "C" void jfg_tidy_linked_list_after(uint8_t* rdram_, uint32_t obj) {
    g_rdram = rdram_;
    if (!rdram_ptr(obj)) return;
    const uint32_t sq = rd32(obj + 0x68);
    if (!rdram_ptr(sq)) return;
    const uint32_t squadron = rd32(sq + 0x24);
    if (!rdram_ptr(squadron)) return;
    const uint32_t data = rd32(squadron + 0x68);
    if (!rdram_ptr(data)) return;
    int length = 0;
    for (uint32_t c = rd32(data + 0x58); c != 0 && length < 256 && rdram_ptr(c) && rdram_ptr(rd32(c + 0x68)); length++) c = rd32(rd32(c + 0x68) + 0x28);
    uint32_t prev = 0, member = rd32(data + 0x58);
    for (int link = 0; member != 0 && link < 256; link++) {
        if (!rdram_ptr(member)) return;
        const uint32_t m = rd32(member + 0x68);
        if (!rdram_ptr(m)) return;
        const uint32_t next = rd32(m + 0x28);
        if (member == obj) {
            if (prev == 0) wr32(data + 0x58, next);
            else wr32(prev + 0x28, next);
            static int logged = 0;
            if (logged++ < 20) {
                char line[320];
                std::snprintf(line, sizeof(line),
                    "squad list: freed member 0x%08X was left in its squadron's chain (link %d of %d, squadron count now %d, level %d); taken out",
                    obj, link, length, (int)rd8(data + 0x00), (int)(int16_t)rd16(ADDR_currentLevel));
                jfg_log_line("jfg_memory.log", line);
            }
            return;
        }
        prev = m;
        member = next;
    }
}

// GalaxianBatsSoundsHandler (overlay 143, the bats' squadron in the Lurg fight, level 195) walks its squadron's member
// list to pick a bat to squeak - the same links as above, without checks. A member freed while still linked crashed it
// there. Its wrapper asks this first, with the squadron data (its a0; first member +0x58): every link the walk would
// follow is checked, and on a bad one the function is left out (no squeak this time; the game's own walk would have
// ended with no bat picked, after two mathRnd calls). At most 20 lines are logged a run.
static int bats_link_ok(uint32_t ptr, int which, int link) {
    if (rdram_ptr(ptr) && link < 256) return 1;
    static int logged = 0;
    if (logged++ < 20) {
        char line[256];
        std::snprintf(line, sizeof(line), "bats' squeak: %s 0x%08X at link %d (level %d); no bat picked",
            link >= 256 ? "chain too long at" : (which == 0 ? "bad member object" : "bad member data"), ptr, link,
            (int)(int16_t)rd16(ADDR_currentLevel));
        jfg_log_line("jfg_memory.log", line);
    }
    return 0;
}

extern "C" int jfg_bats_chain_ok(uint8_t* rdram_, uint32_t data) {
    g_rdram = rdram_;
    if (!rdram_ptr(data)) return 1; // not read by the walk unless the function gets that far; left to the game
    uint32_t member = rd32(data + 0x58);
    for (int link = 0; member != 0; link++) {
        if (!bats_link_ok(member, 0, link)) return 0;
        const uint32_t m = rd32(member + 0x68);
        if (!bats_link_ok(m, 1, link)) return 0;
        member = rd32(m + 0x28);
    }
    return 1;
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
extern "C" void mmAlloc_orig(uint8_t* rdram, recomp_context* ctx);

static void mm_alloc_checked(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t size = (uint32_t)ctx->r4, colour = (uint32_t)ctx->r5;
    mmAlloc_orig(rdram, ctx);
    jfg_mm_alloc_check(rdram, ctx, size, colour);
}

static void objsetup_step_call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* orig, int step);

JFG_WRAP(mmAlloc) {
    objsetup_step_call(rdram, ctx, mm_alloc_checked, 7);
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

// A squad member TidyLinkedList(obj) left in its squadron's chain is taken out (jfg_tidy_linked_list_after).
JFG_WRAP(TidyLinkedList) {
    const uint32_t obj = (uint32_t)ctx->r4;
    TidyLinkedList_orig(rdram, ctx);
    jfg_tidy_linked_list_after(rdram, obj);
}

// The bats' squeak (overlay 143) runs only when its squadron's chain is sound (jfg_bats_chain_ok).
JFG_WRAP(GalaxianBatsSoundsHandler) {
    if (!jfg_bats_chain_ok(rdram, (uint32_t)ctx->r4)) return;
    GalaxianBatsSoundsHandler_orig(rdram, ctx);
}

// osSyncPrintf's output routine (0x8009A710) is reached through a function pointer the recompiler has no entry for,
// which stopped the program the first time the game printed a warning ("out of sound states").
JFG_WRAP(osSyncPrintf) {
    jfg_sync_printf(rdram, ctx);
}

// squadsFireWeapon (overlay 3) makes a squad member's weapon object (member data +0xB4, while it has none) with one
// objSetupObject call whose result it never checks (+0x8640, setup data at $sp + 0x174): on NULL the stores right after
// it crashed the game (a gun turret in level 248, with 200 KB of heap free). Its other objSetupObject calls leave the
// function on NULL, and this one now does too, as far as the game can tell: the NULL becomes a scratch block that takes
// those stores, the function's next objSetupObject (the shot) gets NULL without anything being made, which takes the
// function's own NULL exit, and afterwards the member's weapon slot is cleared so its next shot tries again. The member
// data is still in $t4 at the call (from the +0xB4 check).
namespace recomp { void* alloc(uint8_t* rdram, size_t size); }
static JfgCaller g_fire;
static uint32_t g_fire_scratch = 0; // the scratch block, in the runtime's heap (outside the game's memory)
static uint32_t g_fire_member = 0;  // member data whose weapon object failed in this squadsFireWeapon; 0 = none
constexpr uint32_t FIRE_SCRATCH_SIZE = 0x100;

static bool fire_weapon_failed(uint8_t* rdram, recomp_context* ctx, uint32_t member) {
    if (member < 0x80000000u || member >= 0x80800000u || MEM_W(0xB4, (gpr)(int32_t)member) != 0) return false;
    if (g_fire_scratch == 0) {
        void* block = recomp::alloc(rdram, FIRE_SCRATCH_SIZE);
        if (block == nullptr) return false;
        g_fire_scratch = 0x80000000u + (uint32_t)((uint8_t*)block - rdram);
    }
    for (uint32_t i = 0; i < FIRE_SCRATCH_SIZE; i += 4) MEM_W(i, (gpr)(int32_t)g_fire_scratch) = 0;
    ctx->r2 = (gpr)(int32_t)g_fire_scratch;
    g_fire_member = member;
    static int logged = 0;
    if (logged++ < 20) {
        char line[200];
        std::snprintf(line, sizeof(line), "squad member weapon object could not be created (member data 0x%08X, level %d); shot left out",
            member, (int)(int16_t)MEM_H(0, (gpr)(int32_t)jfg::ADDR_currentLevel));
        jfg::jfg_log_line("jfg_memory.log", line);
    }
    return true;
}

JFG_WRAP(squadsFireWeapon) {
    const uint32_t prev = g_fire_member;
    g_fire_member = 0;
    {
        JfgCaller::Scope scope(g_fire, ctx, 0x1F0);
        squadsFireWeapon_orig(rdram, ctx);
    }
    if (g_fire_member != 0 && (uint32_t)MEM_W(0xB4, (gpr)(int32_t)g_fire_member) == g_fire_scratch) {
        MEM_W(0xB4, (gpr)(int32_t)g_fire_member) = 0;
    }
    g_fire_member = prev;
}

// objSetupObject and its steps (see jfg_objsetup_enter).
JFG_WRAP(objSetupObject) {
    const bool fire = g_fire.direct(ctx);
    if (fire && g_fire_member != 0) { ctx->r2 = 0; return; } // squadsFireWeapon's shot after a failed weapon object
    const bool weapon = fire && (uint32_t)ctx->r4 == (uint32_t)(g_fire.sp + 0x174);
    const uint32_t member = (uint32_t)ctx->r12;
    const int nest = jfg::g_obj_step_nest;
    jfg::g_obj_step_nest = 0;
    jfg_objsetup_enter(rdram, ctx);
    const int d = jfg::g_obj_depth - 1;
    objSetupObject_orig(rdram, ctx);
    const int type = (d >= 0 && d < jfg::OBJ_DEPTH && jfg::g_obj_setup[d].typed) ? jfg::g_obj_setup[d].type : -1;
    jfg_objsetup_exit(rdram, ctx, type);
    jfg::g_obj_step_nest = nest;
    if (weapon && (uint32_t)ctx->r2 == 0) fire_weapon_failed(rdram, ctx, member);
}

// A step call: noted when objSetupObject is running and made it itself.
static void objsetup_step_call(uint8_t* rdram, recomp_context* ctx, recomp_func_t* orig, int step) {
    const bool outer = jfg::g_obj_depth > 0 && jfg::g_obj_step_nest == 0;
    if (outer && step == 0) {
        const int d = jfg::g_obj_depth - 1;
        if (d < jfg::OBJ_DEPTH && !jfg::g_obj_setup[d].typed) {
            jfg::g_obj_setup[d].type = (int16_t)(int32_t)ctx->r4;
            jfg::g_obj_setup[d].typed = true;
        }
    }
    jfg::g_obj_step_nest++;
    orig(rdram, ctx);
    jfg::g_obj_step_nest--;
    if (outer) jfg_objsetup_step(rdram, ctx, step);
}

JFG_WRAP(objGetObjdef) { objsetup_step_call(rdram, ctx, objGetObjdef_orig, 0); }
JFG_WRAP(func_8000F978_10578) { objsetup_step_call(rdram, ctx, func_8000F978_10578_orig, 1); }
JFG_WRAP(texLoadTexture) { objsetup_step_call(rdram, ctx, texLoadTexture_orig, 2); }
JFG_WRAP(modLoadModel) { objsetup_step_call(rdram, ctx, modLoadModel_orig, 4); }
JFG_WRAP(texLoadSprite) { objsetup_step_call(rdram, ctx, texLoadSprite_orig, 5); }
JFG_WRAP(mmAllocR) { objsetup_step_call(rdram, ctx, mmAllocR_orig, 6); }
