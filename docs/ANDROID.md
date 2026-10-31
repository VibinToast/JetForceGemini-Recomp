# Android (arm64) build

An experimental Android build for Snapdragon/Adreno handhelds (developed for the Retroid Pocket 6). It installs as a
normal APK, so Android launchers that install apps from GitHub releases (such as Quiver Launcher) can use it.

Status: **experimental**. Played on a Retroid Pocket 6 (Snapdragon 8 Gen 2, Adreno 740); other GPUs are untested.
There are no touch controls (use the handheld's buttons or a controller).

## Playing

1. Install the APK and open **Jet Force Gemini**.
2. Choose the ROM in the launcher. Android's file picker opens; pick your US `.z64` ROM. It is copied into the app's
   folder, so the original can stay where it is.
3. Saves, settings, mods and texture packs are kept in `Android/data/io.github.jfgrecomp/files` on the device's
   storage (visible from a PC over USB).

If the screen stays black or the game closes at start, the GPU driver may be the cause: on Adreno, a
[Turnip driver](https://github.com/MrPurple666/purple-turnip/releases) often helps.

## Building

Needs a Linux x86_64 PC with the [desktop build](../README.md) working (it generates the game code from your ROM),
plus the Android SDK and NDK. The Android changes to RT64, Plume and N64ModernRuntime are in their `jfg` branches,
so the pinned submodules build for Android as they are:

```sh
# once: Android packages (sdkmanager is in the SDK's cmdline-tools)
sdkmanager "platforms;android-35" "build-tools;35.0.0" "cmake;3.30.3" "ndk;26.0.10792818"

./build_linux.sh path/to/rom.z64             # generates RecompiledFuncs and rsp/aspMain.cpp
tools/android/build_android_deps.sh          # SDL2 and FreeType for Android arm64, into ~/Android/prefixes
c++ -std=c++17 -O2 lib/rt64/src/tools/file_to_c/file_to_c.cpp -o build-android-host/file_to_c

SDK=$HOME/Android/Sdk; P=$HOME/Android/prefixes
$SDK/cmake/3.30.3/bin/cmake -S . -B build-android -G Ninja -DCMAKE_MAKE_PROGRAM=$SDK/cmake/3.30.3/bin/ninja \
  -DCMAKE_TOOLCHAIN_FILE=$SDK/ndk/26.0.10792818/build/cmake/android.toolchain.cmake \
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DSDL2_DIR=$P/SDL2-2.32.10-android-arm64/lib/cmake/SDL2 \
  -DCMAKE_FIND_ROOT_PATH="$P/SDL2-2.32.10-android-arm64;$P/freetype-2.13.3-android-arm64" \
  -DFREETYPE_LIBRARY=$P/freetype-2.13.3-android-arm64/lib/libfreetype.a \
  -DFREETYPE_INCLUDE_DIRS=$P/freetype-2.13.3-android-arm64/include/freetype2 \
  -DRT64_HOST_FILE_TO_C=$PWD/build-android-host/file_to_c \
  -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_BUILD_DICTBUILDER=OFF
$SDK/cmake/3.30.3/bin/cmake --build build-android --parallel
tools/android/build_apk.sh                   # -> bin-android/JetForceGemini-android-arm64.apk
```

`build_apk.sh` packages with the SDK's own tools (aapt2, d8, zipalign, apksigner), so no Gradle is needed. It signs
with the local debug key unless `JFG_ANDROID_KEYSTORE`, `JFG_ANDROID_KEY_ALIAS` and `JFG_ANDROID_KEYSTORE_PASS` are
set; releases should use one fixed key so updates install over each other.

Like the desktop builds, the APK contains game code translated from your ROM. Keep it private.

## How it fits together

- `android/app/src/main`: the manifest, SDL 2.32.10's Java glue (`org/libsdl/app`, zlib licence) and
  `JFGActivity`, which unpacks the launcher files, sets `APP_PROGRAM_PATH` / `APP_FOLDER_PATH`, and provides the file
  picker the game's ROM and mod dialogs use (`src/runtime/support.cpp`).
- The game is built as `libmain.so`; SDL's activity calls its `SDL_main`.
- RT64 renders to the activity's `ANativeWindow` through Plume's Vulkan backend, in RGBA8.
- `patches/android/sdl2-vulkan-surface.patch` is applied to SDL 2.32.10 by `build_android_deps.sh`: SDL's
  `onNativeSurfaceChanged` would otherwise make an EGL surface on the window Vulkan presents to.

## Credits

The Android changes follow [Zelda64Recomp-Android](https://github.com/linkzenic/Zelda64Recomp-Android) by linkzenic
(RT64 window/descriptor changes, Vulkan queue and scissor fixes, the native mod loader, the file dialog bridge and the
nfd stub) and [DKR-R](https://github.com/ThatGuyMcd/DKR-R) by ThatGuyMcd (RGBA8 presentation).
