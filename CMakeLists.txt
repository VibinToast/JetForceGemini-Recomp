cmake_minimum_required(VERSION 3.20)
# JFG PORT: the version comes from version.txt (the release workflow updates it); the launcher shows it.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" JFG_VERSION LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt")
project(JFGRecomp VERSION ${JFG_VERSION} LANGUAGES C CXX)
set(CMAKE_C_STANDARD 17)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang" AND CMAKE_CXX_SIMULATE_ID STREQUAL "MSVC")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-everything /W4")
endif()

# Opt out of constexpr mutex constructor on windows to prevent vcredist issues
if (WIN32)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D_DISABLE_CONSTEXPR_MUTEX_CONSTRUCTOR")
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    option(RECOMP_FLATPAK "Configure the build for Flatpak compatibility." OFF)
endif()

# Avoid warning about DOWNLOAD_EXTRACT_TIMESTAMP in CMake 3.24:
if (CMAKE_VERSION VERSION_GREATER_EQUAL "3.24.0")
    cmake_policy(SET CMP0135 NEW)
endif()

set (CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
set (CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

if (WIN32)
    set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} "${CMAKE_SOURCE_DIR}/lib/")
endif()

# Use xdg-desktop-portal (instead of GTK) for file dialogs on Linux
set(NFD_PORTAL TRUE CACHE BOOL "" FORCE)

set(RT64_STATIC TRUE)
set(RT64_SDL_WINDOW_VULKAN TRUE)
add_compile_definitions(HLSL_CPU)
if (RECOMP_FLATPAK)
    add_compile_definitions(RECOMP_FLATPAK)
endif()
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/rt64 ${CMAKE_BINARY_DIR}/rt64)

# hlslpp's scalar fallback uses labs() without including <cstdlib>. Newer
# AppleClang versions no longer make that declaration available transitively.
if (APPLE)
    target_compile_options(rt64 PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include cstdlib>")
endif()

# set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
set(BUILD_SHARED_LIBS OFF)
SET(LUNASVG_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/lunasvg)
# set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
SET(RMLUI_SVG_PLUGIN ON CACHE BOOL "" FORCE)
SET(RMLUI_TESTS_ENABLED OFF CACHE BOOL "" FORCE)
add_subdirectory(${CMAKE_SOURCE_DIR}/lib/RmlUi)
target_compile_definitions(rmlui_core PRIVATE LUNASVG_BUILD_STATIC)

add_subdirectory(${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime)

target_include_directories(rt64 PRIVATE ${CMAKE_BINARY_DIR}/rt64/src)

# RecompiledFuncs - Library containing the primary recompiler output
add_library(RecompiledFuncs STATIC)

target_compile_options(RecompiledFuncs PRIVATE
    # -Wno-unused-but-set-variable
    -fno-strict-aliasing
    -Wno-unused-variable
    -Wno-implicit-function-declaration
)

target_include_directories(RecompiledFuncs PRIVATE
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/ultramodern/include
    ${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/librecomp/include
    #${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64Recomp/include
)

file(GLOB FUNC_C_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.c)
file(GLOB FUNC_CXX_SOURCES ${CMAKE_SOURCE_DIR}/RecompiledFuncs/*.cpp)

target_sources(RecompiledFuncs PRIVATE ${FUNC_C_SOURCES} ${FUNC_CXX_SOURCES})

# Create RecompControllerDB
# Combine upstream SDL_GameControllerDB with extra database
set(RECOMP_CONTROLLER_DB_TXT ${CMAKE_SOURCE_DIR}/recompcontrollerdb.txt)

function(make_recomp_controller_db)
    file(READ ${CMAKE_SOURCE_DIR}/lib/SDL_GameControllerDB/gamecontrollerdb.txt SDL_DB_CONTENTS)
    file(READ ${CMAKE_SOURCE_DIR}/gamecontrollerdb-extra.txt CUSTOM_DB_CONTENTS)

    file(WRITE ${RECOMP_CONTROLLER_DB_TXT} ${SDL_DB_CONTENTS} "\n" ${CUSTOM_DB_CONTENTS})
endfunction()

make_recomp_controller_db()
add_custom_target(MakeRecompControllerDB
    DEPENDS ${RECOMP_CONTROLLER_DB_TXT})

# Main executable
add_executable(JetForceGeminiRecompiled)
add_dependencies(JetForceGeminiRecompiled MakeRecompControllerDB)
target_compile_definitions(JetForceGeminiRecompiled PRIVATE JFG_VERSION_STRING="${JFG_VERSION}")

if (APPLE)
    set_target_properties(JetForceGeminiRecompiled PROPERTIES
        MACOSX_BUNDLE TRUE
        OUTPUT_NAME "Jet Force Gemini Recompiled"
        MACOSX_BUNDLE_BUNDLE_NAME "Jet Force Gemini Recompiled"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.jfgrecomp"
        MACOSX_BUNDLE_BUNDLE_VERSION "${PROJECT_VERSION}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${PROJECT_VERSION}"
        MACOSX_BUNDLE_COPYRIGHT "Jet Force Gemini game data is supplied by the user."
    )

    add_custom_command(TARGET JetForceGeminiRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RECOMP_CONTROLLER_DB_TXT}"
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources/recompcontrollerdb.txt"
        COMMAND "${CMAKE_SOURCE_DIR}/scripts/package-macos-dependencies.sh"
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>"
        COMMENT "Packaging the macOS app resources"
    )

    # Keep edited launcher/settings assets in sync even when no C++ file needs
    # relinking. POST_BUILD alone only runs after the executable is rebuilt.
    file(GLOB_RECURSE JFG_MACOS_ASSETS CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/*")
    add_custom_target(JFGMacResources ALL
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/assets"
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources/assets"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RECOMP_CONTROLLER_DB_TXT}"
            "$<TARGET_BUNDLE_DIR:JetForceGeminiRecompiled>/Contents/Resources/recompcontrollerdb.txt"
        DEPENDS ${JFG_MACOS_ASSETS} ${RECOMP_CONTROLLER_DB_TXT}
        COMMENT "Synchronizing the macOS app resources"
    )
    add_dependencies(JetForceGeminiRecompiled JFGMacResources)
endif()

# JFG PORT: Lua 5.4 for mods' scripts (lib/lua, vendored; src/jfg/jfg_lua.cpp)
file(GLOB JFG_LUA_SOURCES ${CMAKE_SOURCE_DIR}/lib/lua/src/*.c)
add_library(jfg_lua STATIC ${JFG_LUA_SOURCES})
target_include_directories(jfg_lua PUBLIC ${CMAKE_SOURCE_DIR}/lib/lua/src)
if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    target_compile_definitions(jfg_lua PRIVATE LUA_USE_LINUX)
endif()
target_link_libraries(JetForceGeminiRecompiled PRIVATE jfg_lua)

file(GLOB SOURCES ${CMAKE_SOURCE_DIR}/src/*.cpp)
file(GLOB SOURCES_RECURSIVE
    ${CMAKE_SOURCE_DIR}/src/**/*.cpp
    ${CMAKE_SOURCE_DIR}/src/**/**/*.cpp
)
set (SOURCES_EXTRA
    ${CMAKE_SOURCE_DIR}/rsp/aspMain.cpp
    ${CMAKE_SOURCE_DIR}/lib/RmlUi/Backends/RmlUi_Platform_SDL.cpp
)

target_include_directories(JetForceGeminiRecompiled PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    #${CMAKE_SOURCE_DIR}/lib/N64ModernRuntime/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/N64Recomp/include
    ${CMAKE_SOURCE_DIR}/lib/concurrentqueue
    ${CMAKE_SOURCE_DIR}/lib/GamepadMotionHelpers
    ${CMAKE_SOURCE_DIR}/lib/RmlUi/Include
    ${CMAKE_SOURCE_DIR}/lib/RmlUi/Backends
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/hlslpp/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/inc
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/imgui
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/Vulkan-Headers/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/D3D12MemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/VulkanMemoryAllocator/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/rhi
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/render
    ${CMAKE_SOURCE_DIR}/lib/freetype-windows-binaries/include
    ${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/nativefiledialog-extended/src/include
    ${CMAKE_SOURCE_DIR}/lib/promptfont
    ${CMAKE_SOURCE_DIR}/lib/SlotMap/slot_map
    ${CMAKE_BINARY_DIR}/shaders
    ${CMAKE_CURRENT_BINARY_DIR}
)

if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    target_compile_options(JetForceGeminiRecompiled PRIVATE
        -march=nehalem
        -fno-strict-aliasing
        -fms-extensions
    )
else()
    target_compile_options(JetForceGeminiRecompiled PRIVATE
        -fno-strict-aliasing
        -fms-extensions
    )
endif()

if (CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    # imgui currently triggers some compiler warnings on clang 19 for function casts which leads to a failed build, so disable that warning on clang 19+
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 19.0)
        add_compile_options(-Wno-cast-function-type-mismatch)
    endif()
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 20.0)
        add_compile_options(-Wno-deprecated-literal-operator -Wno-nontrivial-memcall)
    endif()
    if (CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 21.0)
        add_compile_options(-Wno-uninitialized-const-pointer)
    endif()
endif()

if (MSVC)
    # Disable identical code folding, since this breaks mod function patching as multiple functions can get merged into one.
    target_link_options(JetForceGeminiRecompiled PRIVATE /OPT:NOICF)
endif()

if (WIN32)
    include(FetchContent)

    if (DEFINED ENV{SDL2_VERSION})
        set(SDL2_VERSION $ENV{SDL2_VERSION})
    else()
        set(SDL2_VERSION "2.30.3")
    endif()

    # Fetch SDL2 on windows
    FetchContent_Declare(
        sdl2
        URL https://github.com/libsdl-org/SDL/releases/download/release-${SDL2_VERSION}/SDL2-devel-${SDL2_VERSION}-VC.zip
    )
    FetchContent_MakeAvailable(sdl2)
    target_include_directories(JetForceGeminiRecompiled PRIVATE
        ${sdl2_SOURCE_DIR}/include
    )
    target_link_directories(JetForceGeminiRecompiled PRIVATE
        ${sdl2_SOURCE_DIR}/lib/x64
    )

    # Copy SDL2 and dxc DLLs to output folder as post build step
    add_custom_command(TARGET JetForceGeminiRecompiled POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${sdl2_SOURCE_DIR}/lib/x64/SDL2.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxil.dll"
            "${CMAKE_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxcompiler.dll"
            $<TARGET_FILE_DIR:JetForceGeminiRecompiled>)

    set_target_properties(
        JetForceGeminiRecompiled
        PROPERTIES
            LINK_FLAGS_DEBUG "/SUBSYSTEM:CONSOLE"
            LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
            LINK_FLAGS_RELWITHDEBINFO "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
            LINK_FLAGS_MINSIZEREL "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup"
    )

    target_sources(JetForceGeminiRecompiled PRIVATE ${CMAKE_SOURCE_DIR}/icons/app.rc)
    target_link_libraries(JetForceGeminiRecompiled PRIVATE SDL2 Winmm.lib)
endif()

if (CMAKE_SYSTEM_NAME MATCHES "Linux")
    find_package(SDL2 REQUIRED)

    add_compile_definitions("RT64_SDL_WINDOW_VULKAN")

    # Generate icon_bytes.c from the app icon PNG.
    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h
       COMMAND file_to_c ${CMAKE_SOURCE_DIR}/icons/512.png icon_bytes ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.h
       DEPENDS ${CMAKE_SOURCE_DIR}/icons/512.png
    )
    target_sources(JetForceGeminiRecompiled PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/icon_bytes.c)

    message(STATUS "SDL2_FOUND = ${SDL2_FOUND}")
    message(STATUS "SDL2_INCLUDE_DIRS = ${SDL2_INCLUDE_DIRS}")

    target_include_directories(JetForceGeminiRecompiled PRIVATE ${SDL2_INCLUDE_DIRS})

    find_package(Freetype REQUIRED)

    message(STATUS "FREETYPE_FOUND = ${FREETYPE_FOUND}")
    message(STATUS "FREETYPE_INCLUDE_DIRS = ${FREETYPE_INCLUDE_DIRS}")
    message(STATUS "FREETYPE_LIBRARIES = ${FREETYPE_LIBRARIES}")

    include_directories(${FREETYPE_LIBRARIES})
    target_link_libraries(JetForceGeminiRecompiled PRIVATE ${FREETYPE_LIBRARIES} SDL2::SDL2)

    set(CMAKE_THREAD_PREFER_PTHREAD TRUE)
    set(THREADS_PREFER_PTHREAD_FLAG TRUE)
    find_package(Threads REQUIRED)

    # JFG PORT: libatomic is linked in statically when the compiler has libatomic.a (the release container does):
    # Fedora does not install libatomic.so.1 by default, and a game that needs it does not start there. It goes at the
    # very end of the link line, after the static libraries (rt64, librecomp...) that may use it.
    execute_process(COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libatomic.a
        OUTPUT_VARIABLE JFG_LIBATOMIC_A OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if (IS_ABSOLUTE "${JFG_LIBATOMIC_A}" AND EXISTS "${JFG_LIBATOMIC_A}")
        message(STATUS "libatomic: static (${JFG_LIBATOMIC_A})")
        set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${JFG_LIBATOMIC_A}")
    else()
        message(STATUS "libatomic: shared (no libatomic.a found; the game needs libatomic.so.1 on the system)")
        set(CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} -latomic")
    endif()
    target_link_libraries(JetForceGeminiRecompiled PRIVATE "-static-libstdc++" ${CMAKE_DL_LIBS} Threads::Threads)

    # Export symbols to the dynamic symbol table so backtraces work (when symbols aren't stripped)
    target_link_options(JetForceGeminiRecompiled PRIVATE -Wl,-export-dynamic)
endif()

target_link_libraries(JetForceGeminiRecompiled PRIVATE
    RecompiledFuncs
    librecomp
    ultramodern
    rt64
    RmlUi::Core
    RmlUi::Debugger
    nfd
    lunasvg
)

if (${WIN32})
    add_compile_definitions(NOMINMAX)
endif()

# TODO fix the rt64 CMake script so that this doesn't need to be duplicated here
# For DXC
set (DXC_COMMON_OPTS "-I${PROJECT_SOURCE_DIR}/src")
set (DXC_DXIL_OPTS "-Wno-ignored-attributes")
set (DXC_SPV_OPTS "-spirv" "-fspv-target-env=vulkan1.0" "-fvk-use-dx-layout")
set (DXC_PS_OPTS "${DXC_COMMON_OPTS}" "-E" "PSMain" "-T ps_6_0" "-D DYNAMIC_RENDER_PARAMS")
set (DXC_VS_OPTS "${DXC_COMMON_OPTS}" "-E" "VSMain" "-T vs_6_0" "-D DYNAMIC_RENDER_PARAMS" "-fvk-invert-y")
set (DXC_CS_OPTS "${DXC_COMMON_OPTS}" "-E" "CSMain" "-T cs_6_0")
set (DXC_GS_OPTS "${DXC_COMMON_OPTS}" "-E" "GSMain" "-T gs_6_0")
set (DXC_RT_OPTS "${DXC_COMMON_OPTS}" "-D" "RT_SHADER" "-T" "lib_6_3" "-fspv-target-env=vulkan1.1spirv1.4" "-fspv-extension=SPV_KHR_ray_tracing" "-fspv-extension=SPV_EXT_descriptor_indexing")

if (${WIN32})
    set (DXC "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc.exe")
else()
    if (APPLE)
        # Apple's binary is universal, so it'll work on both x86_64 and arm64
        set (DXC "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-macos")
        if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set(SPIRVCROSS "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64//src/contrib/spirv-cross/bin/x64/spirv-cross")
        else()
            set(SPIRVCROSS "DYLD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/spirv-cross/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64//src/contrib/spirv-cross/bin/x64/spirv-cross")
        endif()
    else()
        if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
            set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/x64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/x64/dxc-linux")
        else()
            set (DXC "LD_LIBRARY_PATH=${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/lib/arm64" "${PROJECT_SOURCE_DIR}/lib/rt64/src/contrib/dxc/bin/arm64/dxc-linux")
        endif()
    endif()
endif()

build_vertex_shader(JetForceGeminiRecompiled "shaders/InterfaceVS.hlsl" "shaders/InterfaceVS.hlsl")
build_pixel_shader(JetForceGeminiRecompiled "shaders/InterfacePS.hlsl" "shaders/InterfacePS.hlsl")

target_sources(JetForceGeminiRecompiled PRIVATE ${SOURCES} ${SOURCES_RECURSIVE} ${SOURCES_EXTRA})

set_property(TARGET JetForceGeminiRecompiled PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")

# JFG PORT: a build check that the game's overlay pre-linker (src/jfg/jfg_prelink.cpp, for mods' hooks) gives the
# bytes tools/prepare_overlays.py gave the recompiler; run by build_windows.bat and build_linux.sh.
add_executable(jfg_prelink_check ${CMAKE_SOURCE_DIR}/tools/prelink_check.cpp ${CMAKE_SOURCE_DIR}/src/jfg/jfg_prelink.cpp)
target_include_directories(jfg_prelink_check PRIVATE ${CMAKE_SOURCE_DIR}/src)
