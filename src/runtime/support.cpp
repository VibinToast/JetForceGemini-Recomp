#include "support.hpp"
#include "config/config.hpp"
#include <SDL.h>
#include <cstdlib>
#include "nfd.h"
#include "RmlUi/Core.h"

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#endif

namespace dino::runtime {
#ifdef __APPLE__
    static std::filesystem::path executable_path() {
        uint32_t size = 0;
        _NSGetExecutablePath(nullptr, &size);
        std::string buffer(size, '\0');
        if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
            return std::filesystem::current_path();
        }
        return std::filesystem::weakly_canonical(buffer.c_str());
    }

    void dispatch_on_ui_thread(std::function<void()> func) {
        if (pthread_main_np() != 0) {
            func();
            return;
        }

        auto* pending = new std::function<void()>(std::move(func));
        dispatch_async_f(dispatch_get_main_queue(), pending, [](void* context) {
            std::unique_ptr<std::function<void()>> callback(
                static_cast<std::function<void()>*>(context));
            (*callback)();
        });
    }

    std::optional<std::filesystem::path> get_application_support_directory() {
        const char* home = std::getenv("HOME");
        if (home == nullptr) {
            return std::nullopt;
        }
        return std::filesystem::path(home) / "Library" / "Application Support";
    }

    std::filesystem::path get_bundle_resource_directory() {
        const auto executable_directory = executable_path().parent_path();
        if (executable_directory.filename() == "MacOS" &&
            executable_directory.parent_path().filename() == "Contents") {
            return executable_directory.parent_path() / "Resources";
        }

        // A development build is launched from the project root, where assets/
        // lives. Packaged builds use the app bundle Resources directory above.
        return std::filesystem::current_path();
    }

    std::filesystem::path get_bundle_directory() {
        const auto executable_directory = executable_path().parent_path();
        if (executable_directory.filename() == "MacOS" &&
            executable_directory.parent_path().filename() == "Contents") {
            return executable_directory.parent_path().parent_path();
        }
        return executable_directory;
    }
#endif

    // MARK: - Internal Helpers
    static void show_nfd_error() {
        const char *msg = NFD_GetError();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, dino::config::program_name.data(), msg, nullptr);
        fprintf(stderr, "[ERROR] %s\n", msg);
    }

    void perform_file_dialog_operation(const std::function<void(bool, const std::filesystem::path&)>& callback) {
        nfdnchar_t* native_path = nullptr;
        nfdresult_t result = NFD_OpenDialogN(&native_path, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::filesystem::path path;

        if (success) {
            path = std::filesystem::path{native_path};
            NFD_FreePathN(native_path);
        } else if (result == NFD_ERROR) {
            show_nfd_error();
        }

        callback(success, path);
    }

    void perform_file_dialog_operation_multiple(const std::function<void(bool, const std::list<std::filesystem::path>&)>& callback) {
        const nfdpathset_t* native_paths = nullptr;
        nfdresult_t result = NFD_OpenDialogMultipleN(&native_paths, nullptr, 0, nullptr);

        bool success = (result == NFD_OKAY);
        std::list<std::filesystem::path> paths;
        nfdpathsetsize_t count = 0;

        if (success) {
            NFD_PathSet_GetCount(native_paths, &count);
            for (nfdpathsetsize_t i = 0; i < count; i++) {
                nfdnchar_t* cur_path = nullptr;
                nfdresult_t cur_result = NFD_PathSet_GetPathN(native_paths, i, &cur_path);
                if (cur_result == NFD_OKAY) {
                    paths.emplace_back(std::filesystem::path{cur_path});
                }
            }
            NFD_PathSet_Free(native_paths);
        } else if (result == NFD_ERROR) {
            show_nfd_error();
        }

        callback(success, paths);
    }

    // MARK: - Public API

    std::filesystem::path get_program_path() {
#if defined(__APPLE__)
        return get_bundle_resource_directory();
#elif defined(__linux__) && defined(RECOMP_FLATPAK)
        return "/app/bin";
#else
        static std::optional<std::filesystem::path> program_path;
        if (!program_path.has_value()) {
            if (std::getenv("APPIMAGE") != nullptr) {
                // Inside of AppImage, program path is based on $APPDIR
                program_path = std::filesystem::path(std::getenv("APPDIR")) / "usr/bin";
            } else {
                program_path = "";
            }
        }

        return program_path.value();
#endif
    }

    std::filesystem::path get_asset_path(const char* asset) {
        return get_program_path() / "assets" / asset;
    }

    void open_file_dialog(std::function<void(bool success, const std::filesystem::path& path)> callback) {
#ifdef __APPLE__
        dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation(callback);
        });
#else
        perform_file_dialog_operation(callback);
#endif
    }

    void open_file_dialog_multiple(std::function<void(bool success, const std::list<std::filesystem::path>& paths)> callback) {
#ifdef __APPLE__
        dispatch_on_ui_thread([callback]() {
            perform_file_dialog_operation_multiple(callback);
        });
#else
        perform_file_dialog_operation_multiple(callback);
#endif
    }

    void show_error_message_box(const char *title, const char *message) {
#ifdef __APPLE__
    std::string title_copy(title);
    std::string message_copy(message);

    dispatch_on_ui_thread([title_copy, message_copy] {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title_copy.c_str(), message_copy.c_str(), nullptr);
    });
#else
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title, message, nullptr);
#endif
    }
}
