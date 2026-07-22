#include "preload.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

namespace dino::runtime {

#ifdef _WIN32

bool preload_executable(PreloadContext& context) {
    wchar_t module_name[MAX_PATH];
    GetModuleFileNameW(NULL, module_name, MAX_PATH);

    context.handle = CreateFileW(module_name, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (context.handle == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "Failed to load executable into memory!");
        context = {};
        return false;
    }

    LARGE_INTEGER module_size;
    if (!GetFileSizeEx(context.handle, &module_size)) {
        fprintf(stderr, "Failed to get size of executable!");
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    context.size = module_size.QuadPart;

    context.mapping_handle = CreateFileMappingW(context.handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (context.mapping_handle == nullptr) {
        fprintf(stderr, "Failed to create file mapping of executable!");
        CloseHandle(context.handle);
        context = {};
        return EXIT_FAILURE;
    }

    context.view = MapViewOfFile(context.mapping_handle, FILE_MAP_READ, 0, 0, 0);
    if (context.view == nullptr) {
        fprintf(stderr, "Failed to map view of of executable!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    DWORD pid = GetCurrentProcessId();
    HANDLE process_handle = OpenProcess(PROCESS_SET_QUOTA | PROCESS_QUERY_INFORMATION, FALSE, pid);
    if (process_handle == nullptr) {
        fprintf(stderr, "Failed to open own process!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    SIZE_T minimum_set_size, maximum_set_size;
    if (!GetProcessWorkingSetSize(process_handle, &minimum_set_size, &maximum_set_size)) {
        fprintf(stderr, "Failed to get working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (!SetProcessWorkingSetSize(process_handle, minimum_set_size + context.size, maximum_set_size + context.size)) {
        fprintf(stderr, "Failed to set working set size!");
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    if (VirtualLock(context.view, context.size) == 0) {
        fprintf(stderr, "Failed to lock view of executable! (Error: %08lx)\n", GetLastError());
        CloseHandle(context.mapping_handle);
        CloseHandle(context.handle);
        context = {};
        return false;
    }

    return true;
}

void release_preload(PreloadContext& context) {
    VirtualUnlock(context.view, context.size);
    CloseHandle(context.mapping_handle);
    CloseHandle(context.handle);
    context = {};
}

#else

static std::filesystem::path get_executable_path() {
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buffer(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
        return {};
    }
    return std::filesystem::weakly_canonical(buffer.c_str());
#elif defined(__linux__)
    std::string buffer(4096, '\0');
    const ssize_t length = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
    if (length <= 0) {
        return {};
    }
    buffer.resize(static_cast<std::size_t>(length));
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

bool preload_executable(PreloadContext& context) {
    context = {};
    const std::filesystem::path executable_path = get_executable_path();
    if (executable_path.empty()) {
        fprintf(stderr, "Failed to locate executable for preloading.\n");
        return false;
    }

    const int fd = open(executable_path.c_str(), O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "Failed to open executable for preloading: %s\n", strerror(errno));
        return false;
    }

    struct stat file_stat {};
    if (fstat(fd, &file_stat) != 0 || file_stat.st_size <= 0) {
        fprintf(stderr, "Failed to determine executable size for preloading: %s\n", strerror(errno));
        close(fd);
        return false;
    }

    context.size = static_cast<std::size_t>(file_stat.st_size);
    context.view = mmap(nullptr, context.size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (context.view == MAP_FAILED) {
        fprintf(stderr, "Failed to map executable for preloading: %s\n", strerror(errno));
        context = {};
        return false;
    }

    // WILLNEED starts readahead, while touching one byte per page guarantees
    // the code is faulted into the page cache before the first race begins.
    (void)madvise(context.view, context.size, MADV_WILLNEED);
    constexpr std::size_t PageSize = 4096;
    const volatile unsigned char* bytes =
        static_cast<const volatile unsigned char*>(context.view);
    unsigned char checksum = 0;
    for (std::size_t offset = 0; offset < context.size; offset += PageSize) {
        checksum ^= bytes[offset];
    }
    checksum ^= bytes[context.size - 1];
    (void)checksum;
    return true;
}

void release_preload(PreloadContext& context) {
    if (context.view != nullptr) {
        munmap(context.view, context.size);
    }
    context = {};
}

#endif

}
