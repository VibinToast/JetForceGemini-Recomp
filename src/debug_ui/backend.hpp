#pragma once

#include "imgui.h"

namespace dino::debug_ui::backend {

extern ImGuiContext *jfg_imgui_ctx;
bool is_open();
bool in_ui_frame();
void set_overlay(bool enabled);
bool take_reset_request(); // JFG PORT: F5 was pressed since the last call
bool take_next_pack_request(); // JFG PORT: F6 was pressed since the last call
bool take_fly_request(); // JFG PORT: F7 was pressed since the last call

void begin();
void end();
void set_render_hooks();

}
