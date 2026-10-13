# Jet Force Gemini: Recompiled

A native PC port of Jet Force Gemini (N64, US) for Windows and Linux, made by static recompilation. No emulator:
the game's code is translated from your ROM and compiled for your PC.

**No game code or assets are included.** You need your own ROM.

## ROM

Jet Force Gemini **US**, `.z64` format, SHA-1 `493ced9008dbe932d6e91179b68e8630cf23a023`. Save it as
`baserom.us.z64` in this folder.

## Features

**Launcher**
- Launcher in Jet Force Gemini colours with Play, Profiles, Gameplay, Visuals, Graphics, Sound, Controls,
  Stick & Rumble, Texture Packs and About pages. Works with a controller.

**Profiles**
- Built-in profiles: Default (the original game), Modern, Modern and Balanced (Modern with rebalanced weapons)
  and Extreme.
- Your own profiles: save, switch and delete them.

**Controls**
- Dual-stick controls with a camera-relative left stick, a free camera and a modern button layout.
- Mouse and keyboard controls.
- Over-the-shoulder aim with a centred reticle, and adjustable aim speed.
- Jump while aiming.
- Sniper rifle with a fixed zoom, so you can walk while scoped.
- Dual-stick and mouse-and-keyboard controls in boss fights.
- Adjustable movement speed and diagonal boost, plus camera, mouse and invert options.
- Rumble Pak support, and rebindable controls.
- Races and Floyd missions keep the original controls automatically.

**Gameplay**
- START skips cutscenes, the logo screens and the story intro.
- Adjustable damage taken, jetpack refuel speed and Vela's dive speed.
- A Weapons editor for each weapon: damage, fire rate, speed, range, spread, throw delay, bolt size, charge speed
  and mine limit. Faster throws also speed up the throw animation.
- No blood option.

**Visuals**
- Widescreen.
- Ambient occlusion (SSAO) and bloom, each with sliders.
- Disable LOD and draw distant objects.
- Fly camera for screenshots (F7), which can freeze the world and hide the HUD.
- On-screen FPS, level and texture pack.

**Texture packs**
- The JFG 4K texture pack is a separate download. See
  [vibintoast.com/content/textures/jfg-4k](https://vibintoast.com/content/textures/jfg-4k) for more info.
- Texture dumping, all in one folder or one folder per level.
- HD texture packs, picked from a list and switched in-game with F6.
- Helper scripts in `tools/textures` for collecting newly dumped textures.

**Tools (F10 toolbar)**
- Cheats: invulnerable, unlimited ammo, all weapons, all levels, skip the Tribal check.
- Level warp to any level, with a choice of character and entrance.
- Items editor: weapons, ammo, health, keys, objects, Tribals, ship and Floyd parts, progress flags and
  multiplayer unlocks.
- Quick profile and texture-pack switching.

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

## Keys

| Key | Action |
|---|---|
| Select / Esc | Settings menu |
| F10 | Tools toolbar (Alt switches between the toolbar and the game) |
| F9 | Debug window |
| F5 / F6 | Back to the title screen / next texture pack |
| F7 | Fly camera |
| F11 / Alt+Enter | Fullscreen |

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
