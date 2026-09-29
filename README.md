# Jet Force Gemini: Recompiled

A native PC port of Jet Force Gemini (N64, US) for Windows and Linux, made by static recompilation. No emulator:
the game's code is translated from your ROM and compiled for your PC.

**No game code or assets are included.** You need your own ROM.

## ROM

Jet Force Gemini **US**, `.z64` format, SHA-1 `493ced9008dbe932d6e91179b68e8630cf23a023`. Save it as
`baserom.us.z64` in this folder.

## Build and run

**Windows** (10/11, Direct3D 12 GPU): install Visual Studio 2022 (Desktop development with C++, plus C++ Clang
tools), Git and Python 3. Run `build_windows.bat` (the first build takes 10-40 minutes, about 15 GB), then
`run_windows.bat`.

**Linux** (Vulkan GPU):

```
sudo apt install git cmake ninja-build clang lld python3 libsdl2-dev libvulkan-dev libgtk-3-dev libdbus-1-dev libfreetype6-dev
./build_linux.sh && ./run_linux.sh
```

The game ends up in `bin`, together with its saves and settings. The `build` folder is only needed for building.
Keep `bin` private, because it holds a copy of the ROM.

## How it works

The port uses [N64Recomp](https://github.com/N64Recomp/N64Recomp) to translate the game's code (configured by
`jfg.us.toml` and `jfg.us.syms.toml`), [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) in place
of the N64 operating system, and [RT64](https://github.com/rt64/rt64) for rendering. It is built on
[cyscott/dkr-recomp](https://github.com/cyscott/dkr-recomp), which the build scripts download and patch with
`patches/`. The port's own code is in `src/jfg/`.

## AI disclosure

This project was built with heavy help from AI (Claude), under the direction of the project owner. It has not had
an independent code review.

## Legal

Jet Force Gemini and its logos belong to their rights holders. This project is not affiliated with or endorsed by
them. Do not share the ROM or anything generated from it. The code is licensed under GPL v3 (see `LICENSE`).

## Credits

- The Jet Force Gemini decompilation project
- cyscott/dkr-recomp and the Diddy Kong Racing decompilation
- N64Recomp, N64ModernRuntime, RT64 and their contributors
