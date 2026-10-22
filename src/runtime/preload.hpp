#pragma once

#include <cstddef>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace dino::runtime {

#ifdef _WIN32

struct PreloadContext {
    HANDLE handle;
    HANDLE mapping_handle;
    SIZE_T size;
    PVOID view;
};

#else

struct PreloadContext {
    std::size_t size = 0;
    void* view = nullptr;
};

#endif

bool preload_executable(PreloadContext& context);
void release_preload(PreloadContext& context);

}
