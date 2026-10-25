// Jet Force Gemini port: the per-tick entry point.
//
// jfg_debug_frame() is called once per game tick on the game's main thread, from a hook at the entry of the main
// loop tick func_80044FAC_45BAC (see [[patches.hook]] in jfg.us.toml). It keeps the options and cheats applied, runs
// the scripted test switches (JFG_DEBUG_*), draws the on-screen notes and the F9 debug window, runs what F10's tools
// asked for, and
// carries out what was asked for: F5 (back to the title screen), level warps, the story intro skip.
#include <algorithm>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "debug_ui/debug_ui.hpp"
#include "debug_ui/backend.hpp"
#include "jfg_api.h"
#include "jfg_fields.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"
#include "jfg_update_check.h"

namespace jfg {

bool g_story_skip_pending = false;
bool g_reset_pending = false, g_reset_press_start = false; // F5, see jfg_debug_frame

namespace {
int g_warp_mode = -1; // JFG_DEBUG_WARP's front-end mode for the next warp, -1: the game's

// A cheat checkbox drives one of the game's own u8 flags: forced to 1 every tick while ticked, cleared once on untick.
void apply_flag(bool want, bool& applied, uint32_t addr) {
    if (want) { wr8(addr, 1); applied = true; }
    else if (applied) { wr8(addr, 0); applied = false; }
}

static float toolbar_offset() {
    return g_toolbar_height.load(std::memory_order_relaxed);
}
} // namespace

// Called once per game tick on the game's main thread (hook at the entry of func_80044FAC_45BAC).
extern "C" void jfg_debug_frame(uint8_t* rdram, recomp_context* ctx) {
    g_rdram = rdram;
    jfg::api::fire(rdram, ctx, jfg::api::Event::GameTick, (int32_t)rd32(ADDR_delayDat));
    jfg_camera_restore();
    load_options();
    update_dump_folder();
    apply_no_lod(ctx);
    apply_weapons();
    debug_profile_tick();
    auto_save_tick(ctx);
    gameplay_tick();
    tribal_saves_tick();
    tools_game_tick(ctx); // F10's requests and the values it shows

    // Cheats keep applying while the menu is closed.
    apply_flag(g.invulnerable, g.applied_invulnerable, ADDR_invulnerabilityCheat);
    apply_flag(g.infinite_ammo, g.applied_ammo, ADDR_DebugAmmoFlag);
    apply_flag(g.all_weapons, g.applied_weapons, ADDR_allweaponsCheat);
    apply_flag(g.all_levels, g.applied_levels, ADDR_allLevelCheat);

    // Scripted testing without the UI: JFG_DEBUG_CHEATS=1 turns the four cheats on;
    // JFG_DEBUG_WARP="seconds,level,character,entrance[,reset seconds[,front mode]]" warps once that many seconds after
    // start. With the fifth number, F5 (back to the title screen) is done first at that time: warping from the start-up
    // screens does not stay in the level (the splash sequence takes over again). The sixth replaces the front-end mode
    // passed on (from the title it is 3, the title backdrop, with no player). JFG_DEBUG_WARP2="seconds,level,character,
    // entrance" warps a second time, the way a level exit does (an arrival through a door); JFG_DEBUG_WARP3, same
    // fields, a third time after that.
    {
        static uint32_t tick = 0;
        static const auto start = std::chrono::steady_clock::now();
        static bool warped = false, warped2 = false, warped3 = false, reset_done = false;
        static const char* warp = std::getenv("JFG_DEBUG_WARP");
        static const char* warp2 = std::getenv("JFG_DEBUG_WARP2");
        static const char* warp3 = std::getenv("JFG_DEBUG_WARP3");
        static const bool cheats = std::getenv("JFG_DEBUG_CHEATS") != nullptr;
        static int w[6] = { -1, 0, 1, 0, -1, -1 }, w2[4] = { -1, 0, 1, 0 }, w3[4] = { -1, 0, 1, 0 };
        if (tick == 0) {
            if (cheats) g.invulnerable = g.infinite_ammo = g.all_weapons = g.all_levels = true;
            if (warp != nullptr) std::sscanf(warp, "%d,%d,%d,%d,%d,%d", &w[0], &w[1], &w[2], &w[3], &w[4], &w[5]);
            if (warp2 != nullptr) std::sscanf(warp2, "%d,%d,%d,%d", &w2[0], &w2[1], &w2[2], &w2[3]);
            if (warp3 != nullptr) std::sscanf(warp3, "%d,%d,%d,%d", &w3[0], &w3[1], &w3[2], &w3[3]);
        }
        tick++;
        const auto now = std::chrono::steady_clock::now();
        if (!reset_done && w[4] >= 0 && now - start >= std::chrono::seconds(w[4])) {
            reset_done = true;
            g_reset_pending = true;
        }
        if (!warped && w[0] >= 0 && now - start >= std::chrono::seconds(w[0])) {
            warped = true;
            g.warp_level = w[1]; g.warp_character = w[2]; g.warp_entrance = w[3]; g.pending_warp = true;
            g_warp_mode = w[5];
        }
        else if (warped && !warped2 && w2[0] >= 0 && now - start >= std::chrono::seconds(w2[0])) {
            warped2 = true;
            g.warp_level = w2[1]; g.warp_character = w2[2]; g.warp_entrance = w2[3]; g.pending_warp = true;
        }
        else if (warped2 && !warped3 && w3[0] >= 0 && now - start >= std::chrono::seconds(w3[0])) {
            warped3 = true;
            g.warp_level = w3[1]; g.warp_character = w3[2]; g.warp_entrance = w3[3]; g.pending_warp = true;
        }
    }

    // Scripted testing: JFG_DEBUG_ITEMS=1 gives the current character every key and object;
    // JFG_DEBUG_WEAPONS=<mask> sets the current character's owned-weapon mask the way the Items tab does.
    {
        static const bool items = std::getenv("JFG_DEBUG_ITEMS") != nullptr;
        static const char* const weapons = std::getenv("JFG_DEBUG_WEAPONS");
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
        if (ch != 0 && rd32(ADDR_mainGameMode) == 0) {
            if (items) for (uint32_t b = 0; b < 4; b++) Character::Collected::set(ch, b, 0xFF);
            if (weapons != nullptr) set_owned_weapons(ch, (uint16_t)std::strtoul(weapons, nullptr, 0));
        }
    }

    // Scripted testing: JFG_DEBUG_HOLD_WEAPON=<slot> keeps that weapon in the current character's hand.
    {
        static const char* hold = std::getenv("JFG_DEBUG_HOLD_WEAPON");
        const int c = (int16_t)rd16(ADDR_currentCharacter);
        const uint32_t ch = (c >= 0 && c < 3) ? character_ptr(c) : 0;
        if (hold != nullptr && ch != 0 && rd32(ADDR_mainGameMode) == 0) {
            const int slot = std::atoi(hold);
            if (Character::CurrentWeapon::get(ch) != slot && select_weapon(ctx, slot)) {
                const uint32_t obj = player_object(ctx);
                if (obj != 0 && valid_ptr(Obj::Data::get(obj))) PlayerData::LastWeapon::set(Obj::Data::get(obj), (uint8_t)slot);
            }
        }
    }

    // Scripted testing: JFG_DEBUG_WEAPON_CYCLE="start,period" puts weapon slot 1, 2, 3... in the current
    // character's hand, one every `period` seconds from `start` seconds after launch.
    {
        static const char* cyc = std::getenv("JFG_DEBUG_WEAPON_CYCLE");
        static int c[2] = { -1, 5 };
        static bool parsed = false;
        static const auto start = std::chrono::steady_clock::now();
        static int last = 0;
        if (cyc != nullptr) {
            if (!parsed) { std::sscanf(cyc, "%d,%d", &c[0], &c[1]); parsed = true; }
            const int secs = (int)std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count();
            const int slot = (c[0] >= 0 && secs >= c[0] && c[1] > 0) ? 1 + (secs - c[0]) / c[1] : 0;
            if (slot != last && slot >= 1 && slot <= 15) {
                last = slot;
                const bool ok = select_weapon(ctx, slot);
                std::fprintf(stderr, "[jfg] debug: weapon slot %d %s\n", slot, ok ? "selected" : "refused");
            }
        }
    }

    // "Press F9" hint: shown for a few seconds after the game starts, then faded out. It is drawn through the same
    // ImGui context as the menu; the backend renders it even though the menu is closed (set_overlay).
    static const auto hint_start = std::chrono::steady_clock::now();
    const float hint_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - hint_start).count();
    constexpr float HINT_DELAY = 2.0f, HINT_HOLD = 7.0f, HINT_FADE = 2.0f;
    static bool hint_done = false;
    if (dino::debug_ui::is_open() || hint_t > HINT_DELAY + HINT_HOLD + HINT_FADE) hint_done = true;
    const bool hint_active = !hint_done && hint_t >= HINT_DELAY;
    const float pack_toast_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_pack_toast).count();
    const bool pack_toast = g_pack_toast.time_since_epoch().count() != 0 && pack_toast_t < 3.0f;
    const float save_toast_t = std::chrono::duration<float>(std::chrono::steady_clock::now() - g_save_toast).count();
    const bool save_toast = g.save_notification && g_save_toast.time_since_epoch().count() != 0 && save_toast_t < 2.5f;
    // A newer release (src/jfg/jfg_update_check.cpp): shown once per run in the game too, for players who skip the
    // launcher, where it is otherwise only shown. Polled once a second; shown from 3 s after start, for UPDATE_SHOW s.
    constexpr float UPDATE_SHOW = 5.0f; // the last second fades out
    static std::chrono::steady_clock::time_point update_seen{}, update_poll{};
    static std::string update_version;
    if (update_version.empty() && std::chrono::steady_clock::now() >= update_poll) {
        update_poll = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        JfgUpdateInfo info;
        if (jfg_update_available(info)) { update_version = info.version; update_seen = std::chrono::steady_clock::now(); }
    }
    const float update_t = update_version.empty() ? -1.0f
        : std::chrono::duration<float>(std::chrono::steady_clock::now() - std::max(update_seen, hint_start + std::chrono::seconds(3))).count();
    const bool update_toast = update_t >= 0.0f && update_t < UPDATE_SHOW && jfg_launcher_check_updates(); // not once turned off
    dino::debug_ui::backend::set_overlay(hint_active || g.show_fps || g.show_level || g.show_pack || pack_toast || save_toast || update_toast);

    dino::debug_ui::ui_frame_begin();
    if (hint_active && dino::debug_ui::backend::in_ui_frame()) {
        const float t = hint_t - HINT_DELAY;
        float alpha = 1.0f;
        if (t < 0.5f) alpha = t / 0.5f;
        else if (t > HINT_HOLD) alpha = std::max(0.0f, 1.0f - (t - HINT_HOLD) / HINT_FADE);
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        const char* text = "Settings: Select / Esc  -  Tools: F10"; // F9 (debug window) is named in the settings menu instead
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(18.0f, size.y / 32.0f);
        const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
        const ImVec2 pos(size.x * 0.5f - extent.x * 0.5f, size.y * 0.90f);
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const float pad = font_size * 0.4f;
        dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f * alpha)), pad * 0.5f);
        dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), text);
    }
    if (pack_toast && dino::debug_ui::backend::in_ui_frame()) {
        // F6: which pack is now in use, for a few seconds at the top of the screen.
        std::string current;
        { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
        std::string text = "Texture pack: " + pack_display_name(current);
        const int n = g_texture_pack_count.load();
        if (!current.empty() && n >= 0) text += "  (" + std::to_string(n) + " textures)";
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(18.0f, size.y / 32.0f);
        const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 pos(size.x * 0.5f - extent.x * 0.5f, size.y * 0.06f + toolbar_offset());
        const float alpha = pack_toast_t > 2.5f ? (3.0f - pack_toast_t) / 0.5f : 1.0f;
        const float pad = font_size * 0.4f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.6f * alpha)), pad * 0.5f);
        dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), text.c_str());
    }
    if (update_toast && dino::debug_ui::backend::in_ui_frame()) {
        // Top middle, out of the way of the start-up key hint at the bottom: two lines in the launcher's gold.
        const std::string line1 = "Update v" + update_version + " is available";
        const char* line2 = "Select / Esc, then Return to Launcher on the Quit tab, to install it";
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(18.0f, size.y / 34.0f), small = font_size * 0.75f;
        const ImVec2 e1 = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line1.c_str());
        const ImVec2 e2 = font->CalcTextSizeA(small, FLT_MAX, 0.0f, line2);
        const float pad = font_size * 0.5f, width = std::max(e1.x, e2.x), height = e1.y + pad * 0.3f + e2.y;
        const ImVec2 box0(size.x * 0.5f - width * 0.5f - pad, size.y * 0.05f), box1(size.x * 0.5f + width * 0.5f + pad, size.y * 0.05f + height + pad * 2.0f);
        float alpha = update_t > UPDATE_SHOW - 1.0f ? UPDATE_SHOW - update_t : 1.0f;
        if (update_t < 0.3f) alpha = update_t / 0.3f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(box0, box1, ImGui::GetColorU32(ImVec4(5 / 255.0f, 11 / 255.0f, 28 / 255.0f, 0.85f * alpha)), pad * 0.4f);
        dl->AddRect(box0, box1, ImGui::GetColorU32(ImVec4(1.0f, 178 / 255.0f, 26 / 255.0f, alpha)), pad * 0.4f, 0, 2.0f);
        dl->AddText(font, font_size, ImVec2(size.x * 0.5f - e1.x * 0.5f, box0.y + pad),
            ImGui::GetColorU32(ImVec4(1.0f, 178 / 255.0f, 26 / 255.0f, alpha)), line1.c_str());
        dl->AddText(font, small, ImVec2(size.x * 0.5f - e2.x * 0.5f, box0.y + pad + e1.y + pad * 0.3f),
            ImGui::GetColorU32(ImVec4(232 / 255.0f, 243 / 255.0f, 1.0f, alpha)), line2);
    }
    if (save_toast && dino::debug_ui::backend::in_ui_frame()) {
        // Auto save: a small note in the bottom right corner, out of the way of the HUD's middle.
        const char* text = "Game saved";
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(16.0f, size.y / 40.0f);
        const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, text);
        const float pad = font_size * 0.4f;
        const ImVec2 pos(size.x - extent.x - pad * 3.0f, size.y - extent.y - pad * 3.0f);
        float alpha = save_toast_t > 2.0f ? (2.5f - save_toast_t) / 0.5f : 1.0f;
        if (save_toast_t < 0.2f) alpha = save_toast_t / 0.2f;
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
            ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f * alpha)), pad * 0.5f);
        dl->AddText(font, font_size, pos, ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, alpha)), text);
    }
    if ((g.show_fps || g.show_level || g.show_pack) && dino::debug_ui::backend::in_ui_frame()) {
        // Top right corner, one line each: frame rate (main loop ticks per second, measured in jfg_frame_sync), the
        // level, the texture pack.
        struct Line { std::string text; ImVec4 color; };
        std::vector<Line> lines;
        if (g.show_fps) {
            char text[64];
            // Game: the game's own updates a second (its logic). Screen: the frames shown a second (the renderer's
            // presents, in-between frames included).
            std::snprintf(text, sizeof(text), "Game %.0f FPS  Screen %.0f FPS", g_fps.load(std::memory_order_relaxed),
                          g_screen_fps.load(std::memory_order_relaxed));
            lines.push_back({ text, ImVec4(1.0f, 1.0f, 0.4f, 1.0f) });
        }
        if (g.show_level && g_rdram != nullptr) {
            load_level_names();
            const int level = (int16_t)rd16(ADDR_currentLevel);
            std::string text = "Level " + std::to_string(level);
            if (level >= 0 && level < (int)g_level_names.size()) text += ": " + g_level_names[level];
            lines.push_back({ text, ImVec4(0.75f, 0.95f, 1.0f, 1.0f) });
        }
        if (g.show_pack) {
            std::string current;
            { std::lock_guard lock(g_pack_mutex); current = g_pack_folder; }
            lines.push_back({ "Texture pack: " + pack_display_name(current), ImVec4(0.8f, 1.0f, 0.8f, 1.0f) });
        }
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        ImFont* font = ImGui::GetFont();
        const float font_size = std::max(16.0f, size.y / 40.0f);
        const float pad = font_size * 0.35f;
        float y = pad * 1.5f + toolbar_offset(); // below F10's toolbar while it is open
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        for (const Line& line : lines) {
            const ImVec2 extent = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, line.text.c_str());
            const ImVec2 pos(size.x - extent.x - pad * 2.5f, y);
            dl->AddRectFilled(ImVec2(pos.x - pad, pos.y - pad * 0.5f), ImVec2(pos.x + extent.x + pad, pos.y + extent.y + pad * 0.5f),
                ImGui::GetColorU32(ImVec4(0.0f, 0.0f, 0.0f, 0.55f)), pad * 0.5f);
            dl->AddText(font, font_size, pos, ImGui::GetColorU32(line.color), line.text.c_str());
            y += extent.y + pad * 1.4f;
        }
    }
    if (dino::debug_ui::is_open() && dino::debug_ui::backend::in_ui_frame()) {
        load_level_names();
        draw_debug_window(ctx);
    }
    dino::debug_ui::ui_frame_end();

    // 60 fps: viFrameSync paces the game by gVideoDeltaTime (2 video frames, 3 when it falls behind); the game's own
    // runInOneFrame flag makes it 1. viFrameRateReset() clears the flag again (level changes), so set it every tick.
    if (game_frame_rate() >= 1) wr32(ADDR_runInOneFrame, 1);

    // F5: back to the title screen (F1-F4 belong to RT64's developer tools), the way the game itself gets there (mainTitlePageInit: game mode 1, then
    // mainChangeLevel(title level, 0, 0, 3, 1, 0)). Not while a skip runs. From the pause screen the title level
    // does not load (trackInit hits a break), so the pause screen is closed first with a START press.
    {
        static int wait_ticks = 0;
        bool& pending = g_reset_pending;
        if (dino::debug_ui::backend::take_reset_request()) { pending = true; wait_ticks = 0; }
        load_texture_pack_choice();
        if (dino::debug_ui::backend::take_next_pack_request()) next_texture_pack();
        if (dino::debug_ui::backend::take_fly_request()) g_fly_toggle_request = true; // F7: fly camera
        const int mode = (int)rd32(ADDR_mainGameMode);
        if (pending && !g.skipping && (mode == 0 || mode == 1)) {
            if (rd8(ADDR_pauseMode) != 0) {
                // jfg_after_joy presses START (after joyRead, which would overwrite it here) every 15th tick.
                g_reset_press_start = (wait_ticks++ % 15) == 0;
                if (wait_ticks > 300) { pending = false; g_reset_press_start = false; } // the pause screen did not close
            }
            else {
                pending = false;
                g_reset_press_start = false;
                std::fprintf(stderr, "[jfg] F5: back to the title screen (level %d, game mode %d)\n", (int)(int16_t)rd16(ADDR_currentLevel), mode);
                wr8(ADDR_waitMidiSync, 0);
                wr32(ADDR_midiSyncReached, 1);
                call_game(mainTitlePageInit, ctx, {});
            }
        }
    }
    {
        // Tests: JFG_DEBUG_FORCE_BREAK=<secs> calls the game's mathBreakPoint once.
        static const char* const brk = std::getenv("JFG_DEBUG_FORCE_BREAK");
        static const auto start = std::chrono::steady_clock::now();
        if (brk != nullptr && std::chrono::steady_clock::now() - start > std::chrono::seconds(std::atoi(brk))) call_game(mathBreakPoint, ctx, {});
    }
    if (g_story_skip_pending) {
        g_story_skip_pending = false;
        const int level = (int16_t)rd16(ADDR_currentLevel);
        if (level >= LEVEL_STORY_FIRST && level <= LEVEL_STORY_LAST) {
            // The intro's level changes wait for a marker in its music (func_800468EC calls amWaitForMidiSync when
            // u8 D_800A326C is set; amWaitForMidiSync returns at once when D_800E9774 is set). Neither may hold up
            // this change, or the main loop sits there with a frozen picture while the music plays on.
            wr8(ADDR_waitMidiSync, 0);
            wr32(ADDR_midiSyncReached, 1);
            call_game(mainChangeLevel, ctx, { (uint32_t)LEVEL_CHARACTER_SELECT, 0u, 0u, 5u, 1u, 0u });
        }
    }
    if (g.pending_warp) {
        g.pending_warp = false;
        // Same argument pattern as the game's own level exits (controlPlayer): the front-end mode is passed
        // through unchanged - passing 0 there starts the level without its HUD and weapon selection.
        const uint32_t mode = g_warp_mode >= 0 ? (uint32_t)g_warp_mode : call_game(frontGetMode, ctx, {}) & 0xFFu;
        if (rd32(ADDR_mainGameMode) != 0) auto_save_forget_file(); // not from inside a game: no file to auto save to
        // From the title with a front-end mode other than the title's own (3): in game, as starting a game does. 16 is the
        // in-game HUD (frontInstruments), as in a game loaded from a file; 0 has no HUD.
        if (g_warp_mode >= 0 && g_warp_mode != 3) wr32(ADDR_mainGameMode, 0);
        g_warp_mode = -1;
        std::fprintf(stderr, "[jfg] debug menu: mainChangeLevel(%d, %d, %d, %u, 1, 0)\n", g.warp_level, g.warp_character, g.warp_entrance, mode);
        call_game(mainChangeLevel, ctx, { (uint32_t)g.warp_level, (uint32_t)g.warp_character, (uint32_t)g.warp_entrance, mode, 1u, 0u });
    }
}

} // namespace jfg
