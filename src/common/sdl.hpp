#pragma once

#define SDL_MAIN_HANDLED
#ifdef _WIN32
#include "SDL.h" // IWYU pragma: export
#include "SDL_syswm.h" // IWYU pragma: export
#else
#include "SDL.h" // IWYU pragma: export
#include "SDL_syswm.h" // IWYU pragma: export
// Undefine x11 macros that get included by SDL_syswm.h.
#undef None
#undef Status
#undef LockMask
#undef ControlMask
#undef Success
#undef Always
#endif

#if defined(RT64_SDL_WINDOW_VULKAN)
#include "SDL_vulkan.h" // IWYU pragma: export
#endif

inline void recomp_get_window_size_in_pixels(SDL_Window* window, int* width, int* height) {
#if SDL_VERSION_ATLEAST(2, 26, 0)
    SDL_GetWindowSizeInPixels(window, width, height);
#elif defined(RT64_SDL_WINDOW_VULKAN)
    SDL_Vulkan_GetDrawableSize(window, width, height);
#else
    SDL_GetWindowSize(window, width, height);
#endif
}
