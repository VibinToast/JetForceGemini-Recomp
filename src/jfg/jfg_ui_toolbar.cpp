// Jet Force Gemini port: F10's toolbar.
//
// F10: toolbar. The settings moved to the Select / Esc menu (Gameplay, Visuals, Profiles tabs); the developer tools
// that do not fit there are here: cheats and the profile / texture pack quick switches in menus, and Level warp,
// Items and Weapons in their own windows. F9 still opens the original all-in-one window.
#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include "imgui/imgui.h"
#include "debug_ui/debug_ui.hpp"
#include "debug_ui/backend.hpp"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_options.h"
#include "jfg_port.h"

namespace jfg {

float g_toolbar_height = 0.0f;

namespace {
// Windows-style look (dark mode) for the bar and its drop-down menus: a dark bar with light text, a highlight on the
// menu under the mouse, drop-downs with a thin border, and thin vertical separators between the menus.
struct ToolbarTheme {
    int colors = 0, vars = 0;
    void color(ImGuiCol c, ImU32 v) { ImGui::PushStyleColor(c, v); colors++; }
    void var(ImGuiStyleVar s, float v) { ImGui::PushStyleVar(s, v); vars++; }
    void var(ImGuiStyleVar s, ImVec2 v) { ImGui::PushStyleVar(s, v); vars++; }
    ToolbarTheme() {
        color(ImGuiCol_MenuBarBg, IM_COL32(32, 32, 32, 255));
        color(ImGuiCol_PopupBg, IM_COL32(43, 43, 43, 255));
        color(ImGuiCol_Border, IM_COL32(75, 75, 75, 255));
        color(ImGuiCol_Text, IM_COL32(240, 240, 240, 255));
        color(ImGuiCol_TextDisabled, IM_COL32(150, 150, 150, 255));
        color(ImGuiCol_Header, IM_COL32(65, 65, 65, 255));
        color(ImGuiCol_HeaderHovered, IM_COL32(58, 58, 58, 255));
        color(ImGuiCol_HeaderActive, IM_COL32(70, 70, 70, 255));
        color(ImGuiCol_Separator, IM_COL32(70, 70, 70, 255));
        color(ImGuiCol_FrameBg, IM_COL32(30, 30, 30, 255));
        color(ImGuiCol_FrameBgHovered, IM_COL32(50, 50, 50, 255));
        color(ImGuiCol_FrameBgActive, IM_COL32(60, 60, 60, 255));
        color(ImGuiCol_CheckMark, IM_COL32(96, 205, 255, 255));
        color(ImGuiCol_SliderGrab, IM_COL32(96, 205, 255, 255));
        color(ImGuiCol_SliderGrabActive, IM_COL32(150, 220, 255, 255));
        color(ImGuiCol_Button, IM_COL32(55, 55, 55, 255));
        color(ImGuiCol_ButtonHovered, IM_COL32(68, 68, 68, 255));
        color(ImGuiCol_ButtonActive, IM_COL32(80, 80, 80, 255));
        color(ImGuiCol_ScrollbarBg, IM_COL32(43, 43, 43, 255));
        color(ImGuiCol_ScrollbarGrab, IM_COL32(100, 100, 100, 255));
        var(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
        var(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 6.0f));
        var(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
        var(ImGuiStyleVar_PopupRounding, 0.0f);
        var(ImGuiStyleVar_PopupBorderSize, 1.0f);
        var(ImGuiStyleVar_FrameBorderSize, 1.0f);
        var(ImGuiStyleVar_FrameRounding, 0.0f);
    }
    ~ToolbarTheme() { ImGui::PopStyleVar(vars); ImGui::PopStyleColor(colors); }
};

// Thin vertical line between two menus of the bar.
static void toolbar_separator() {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float x = p.x + 5.0f;
    ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, p.y + h * 0.8f), IM_COL32(85, 85, 85, 255), 1.0f);
    ImGui::Dummy(ImVec2(11.0f, h));
}

// A boxed "label  value" field on the right of the bar, so each value is clearly tied to its label.
static float toolbar_field_width(const char* label, const std::string& value) {
    return ImGui::CalcTextSize(label).x + ImGui::CalcTextSize(value.c_str()).x + 28.0f;
}
static void toolbar_field(const char* label, const std::string& value, ImU32 value_color = IM_COL32(240, 240, 240, 255)) {
    const float h = ImGui::GetFrameHeight();
    const float lw = ImGui::CalcTextSize(label).x, vw = ImGui::CalcTextSize(value.c_str()).x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = lw + vw + 28.0f, y0 = p.y + 3.0f, y1 = p.y + h - 3.0f, ty = p.y + (h - ImGui::GetTextLineHeight()) * 0.5f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, y0), ImVec2(p.x + lw + 14.0f, y1), IM_COL32(52, 52, 52, 255));
    dl->AddRectFilled(ImVec2(p.x + lw + 14.0f, y0), ImVec2(p.x + w, y1), IM_COL32(20, 20, 20, 255));
    dl->AddRect(ImVec2(p.x, y0), ImVec2(p.x + w, y1), IM_COL32(85, 85, 85, 255));
    dl->AddText(ImVec2(p.x + 7.0f, ty), IM_COL32(165, 165, 165, 255), label);
    dl->AddText(ImVec2(p.x + lw + 21.0f, ty), value_color, value.c_str());
    ImGui::Dummy(ImVec2(w, h));
}
} // namespace

void draw_toolbar(recomp_context* ctx) {
    static bool show_warp = false, show_items = false, show_weapons = false;
    const bool in_game = valid_ptr(rd32(ADDR_gameplay));
    ImGui::GetIO().FontGlobalScale = std::max(1.0f, ImGui::GetIO().DisplaySize.y / 720.0f);
    load_profile_choice();
    std::string current_pack;
    { std::lock_guard lock(g_pack_mutex); current_pack = g_pack_folder; }
    bool bar_open;
    float bar_height = 0.0f;
    {
        ToolbarTheme theme;
        bar_open = ImGui::BeginMainMenuBar();
        bar_height = ImGui::GetFrameHeight();
        g_toolbar_height = bar_height;
        if (bar_open) {
            if (ImGui::BeginMenu("Cheats")) {
                draw_cheats();
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Tools")) {
                ImGui::MenuItem("Level Warp...", nullptr, &show_warp);
                ImGui::MenuItem("Items...", nullptr, &show_items);
                ImGui::MenuItem("Weapons...", nullptr, &show_weapons);
                ImGui::Separator();
                if (ImGui::MenuItem("Fly Camera", "F7", g_fly.active)) g_fly_toggle_request = true;
                if (ImGui::MenuItem("Freeze While Flying", nullptr, &g.fly_freeze)) save_options();
                if (ImGui::MenuItem("Hide HUD While Flying", nullptr, &g.fly_hide_hud)) save_options();
                ImGui::Separator();
                ImGui::TextDisabled("The windows open on their own. Fly camera: WASD / left stick move,");
                ImGui::TextDisabled("mouse / right stick look, Space / A up, Ctrl / B down, fire = faster.");
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Profile")) {
                list_user_profiles();
                ImGui::TextDisabled("Built-in");
                for (const BuiltinProfile& p : BUILTIN_PROFILES) {
                    if (ImGui::MenuItem(p.name, nullptr, g_active_profile == p.name)) choose_profile(p.name);
                }
                if (!g_user_profiles.empty()) {
                    ImGui::Separator();
                    ImGui::TextDisabled("Your profiles");
                }
                for (const std::string& n : std::vector<std::string>(g_user_profiles)) {
                    if (ImGui::MenuItem(n.c_str(), nullptr, g_active_profile == n)) choose_profile(n);
                }
                ImGui::Separator();
                ImGui::TextDisabled("Save and edit profiles in the Select / Esc menu > Profiles.");
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Textures")) {
                ImGui::TextDisabled("Texture pack");
                if (ImGui::MenuItem(pack_display_name("").c_str(), nullptr, current_pack.empty())) set_texture_pack("");
                for (const std::string& p : list_texture_packs()) {
                    if (ImGui::MenuItem(pack_display_name(p).c_str(), nullptr, p == current_pack)) set_texture_pack(p);
                }
                if (ImGui::MenuItem("Reload Pack", "F6: next pack")) set_texture_pack(current_pack);
                ImGui::Separator();
                ImGui::TextDisabled("Texture dumping (bin\\texture_dump)");
                ImGui::MenuItem("Dump Textures (this session)", nullptr, &g.texture_dump);
                if (ImGui::MenuItem("One Folder per Level", nullptr, &g.dump_by_level)) save_options();
                ImGui::EndMenu();
            }
            toolbar_separator();
            if (ImGui::BeginMenu("Game")) {
                if (ImGui::MenuItem("Back to the Title Screen", "F5")) g_reset_pending = true;
                ImGui::Separator();
                ImGui::TextDisabled("Settings: Select (controller) or Esc.");
                ImGui::TextDisabled("F9: the original debug window. F10: this toolbar.");
                ImGui::TextDisabled("F7: fly camera. F5: title screen. F6: next texture pack.");
                ImGui::TextDisabled("Alt or a click outside the toolbar: play with the mouse and keyboard; Alt again: back to the toolbar.");
                ImGui::EndMenu();
            }
            // Status fields on the right, each boxed with its label.
            const int level = (int16_t)rd16(ADDR_currentLevel);
            const std::string level_text = std::to_string(level) + " " +
                ((level >= 0 && level < (int)g_level_names.size()) ? g_level_names[level] : std::string("?"));
            const std::string profile_text = g_active_profile.empty() ? std::string("(none)") : g_active_profile;
            const std::string pack_text = pack_display_name(current_pack);
            const bool game_input = dino::debug_ui::backend::toolbar_game_input();
            const std::string input_text = game_input ? "Game (Alt: toolbar)" : "Toolbar (Alt / click outside: game)";
            const float gap = 8.0f;
            const float total = toolbar_field_width("Mouse & keys", input_text) + toolbar_field_width("Profile", profile_text) +
                                toolbar_field_width("Pack", pack_text) + toolbar_field_width("Level", level_text) + gap * 4.0f;
            ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 16.0f, ImGui::GetWindowWidth() - total));
            toolbar_field("Mouse & keys", input_text, game_input ? IM_COL32(120, 220, 120, 255) : IM_COL32(96, 205, 255, 255));
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Profile", profile_text);
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Pack", pack_text);
            ImGui::SameLine(0.0f, gap);
            toolbar_field("Level", level_text);
            ImGui::EndMainMenuBar();
        }
    }
    // A thin shadow line under the bar, like a window's menu bar border.
    if (bar_open) {
        ImGui::GetForegroundDrawList()->AddLine(ImVec2(0.0f, bar_height), ImVec2(ImGui::GetIO().DisplaySize.x, bar_height), IM_COL32(70, 70, 70, 255));
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto window = [&](const char* title, bool* open, float x, auto&& body) {
        if (!*open) return;
        ImGui::SetNextWindowSize(ImVec2(std::min(760.0f, display.x * 0.45f), display.y * 0.75f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImVec2(x, ImGui::GetFrameHeight() * 2.0f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(title, open)) body();
        ImGui::End();
    };
    window("Level warp", &show_warp, 20.0f, [&]() { draw_warp(); });
    window("Items", &show_items, 60.0f, [&]() {
        if (in_game) draw_inventory(ctx); else ImGui::TextDisabled("No game loaded.");
    });
    window("Weapons", &show_weapons, display.x * 0.5f, [&]() { draw_weapons(); });
}

} // namespace jfg
