#pragma once

#include "imgui.h"

namespace dino::debug_ui::backend {

extern ImGuiContext *dino_imgui_ctx;
bool is_open();
bool in_ui_frame();
void set_overlay(bool enabled);
bool take_reset_request(); // JFG PORT: F5 was pressed since the last call
bool take_next_pack_request(); // JFG PORT: F6 was pressed since the last call
bool take_fly_request(); // JFG PORT: F7 was pressed since the last call
bool lighting_tool_open(); // JFG PORT: F12, the lighting tool (src/jfg/jfg_lighting_tool.cpp)
bool lighting_tool_playing(); // ... the game has the mouse and keyboard (else the tool has them; Alt switches)
void set_lighting_tool_playing(bool playing);
void set_lighting_tool_open(bool open);

void begin();
void end();
void set_render_hooks();

}
