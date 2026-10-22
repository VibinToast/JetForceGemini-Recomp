#pragma once

#include <cstdint>

// ABI shared by the native UI queue and recompiled callbacks. Keep the values
// synchronized with recompui::EventType and recompui::DragPhase.
enum RecompuiEventType : std::int32_t {
    UI_EVENT_NONE,
    UI_EVENT_CLICK,
    UI_EVENT_FOCUS,
    UI_EVENT_HOVER,
    UI_EVENT_ENABLE,
    UI_EVENT_DRAG,
    UI_EVENT_RESERVED1,
    UI_EVENT_UPDATE,
    UI_EVENT_COUNT
};

enum RecompuiDragPhase : std::int32_t {
    UI_DRAG_NONE,
    UI_DRAG_START,
    UI_DRAG_MOVE,
    UI_DRAG_END
};

struct RecompuiEventData {
    RecompuiEventType type;
    union {
        struct {
            float x;
            float y;
        } click;
        struct {
            bool active;
        } focus;
        struct {
            bool active;
        } hover;
        struct {
            bool active;
        } enable;
        struct {
            float x;
            float y;
            RecompuiDragPhase phase;
        } drag;
    } data;
};

