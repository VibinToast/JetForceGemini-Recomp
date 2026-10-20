# Architecture

## Overview

This project translates Diddy Kong Racing's MIPS CPU code into native code while preserving the original game state in an emulated N64 memory layout. Host implementations provide the hardware-facing services that cannot execute directly in a normal desktop process.

```text
User-supplied USA Rev 1 ROM
          |
          +-- N64Recomp --------------------> Recompiled CPU functions
          |
          +-- RSPRecomp --------------------> ABI3 audio microcode
          |                                  F3DDKR microcode references
          |
          +-- retail data loaded at runtime
                                             |
Recompiled CPU + native shims + runtime + RT64/Plume
                                             |
                                  macOS Metal / Linux Vulkan
```

The application is not a general N64 emulator. Its symbol manifest, entrypoint, patches, native overrides, save type, and microcode handlers are specific to one DKR revision.

## Source layers

### Recompilation inputs

`dkr.us.v80.toml` defines the CPU entrypoint, symbol manifest, ROM input, renamed functions, native overrides, and instruction-level compatibility patches. `scripts/generate-recomp.sh` invokes N64Recomp and RSPRecomp for all configured inputs.

`dkr.us.v80.syms.toml` was derived from the matching DKR decompilation. It is source metadata and contains no ROM payload. `RecompiledFuncs/` and the generated files under `rsp/` are produced locally from the private ROM and are ignored.

### Native application runtime

The inherited application shell provides:

- SDL window, audio, controller, and event handling;
- N64ModernRuntime scheduling and libultra services;
- ROM selection and version validation;
- EEPROM save data and configuration storage;
- RmlUi launcher and settings pages;
- crash registration and platform logs;
- RT64 renderer integration.

Some internal C++ namespaces still retain `dino` names from the application foundation. They are implementation details retained to avoid a high-risk, behavior-free rename while compatibility work is active; the build target and packages use DKR-specific names.

### Native DKR shims

`src/recomp_api/` contains narrowly scoped functions that replace behavior which depends on unavailable N64 hardware:

- `hardware.cpp` supplies successful results for retail boot and anti-tamper checks whose original memory locations do not exist in a native process;
- `pak.cpp` currently reports no Controller Pak while allowing normal EEPROM operation;
- `animation.cpp` contains a documented native implementation and an A/B harness for the hand-written `obj_animate` routine; the generated private reference is named `obj_animate_recomp` by `dkr.us.v80.toml` and retail recompiled behavior remains the default.

Instruction patches in `dkr.us.v80.toml` are preferred when a retail function only needs a small hardware-facing substitution. Every patch must record the original purpose, the native mismatch, and why the replacement preserves intended game behavior.

## RSP and graphics

### Audio

DKR's ABI3 `aspMain` program is translated with RSPRecomp. Its command handlers are selected through a halfword jump table, so `rsp-aspMain.toml` supplies indirect branch targets that static control-flow discovery cannot infer.

Empty startup audio tasks are completed without entering a stale command table. This behavior is represented in the N64ModernRuntime patch and guarded by the task type and signed data size.

### F3DDKR

DKR uses three related custom graphics microcodes:

- XBUS;
- FIFO;
- DRAM.

RT64 identifies their hashes and decodes F3DDKR-specific matrix, vertex, texture-offset, triangle, DMA display-list, move-word, and DMA-offset commands. Graphics tasks are handled by RT64's high-level path; their generated RSP files are retained as reproducible references rather than executed for rendering.

The renderer must finish reading a display list and its referenced memory before the submitting game thread can continue. DKR starts rebuilding its double-buffered graphics arena immediately after submission, before it necessarily consumes the later SP/DP messages. Graphics submission therefore waits for RT64's synchronous parse (not GPU completion), and only then releases the game thread and posts completion. Without that lifetime barrier, slower hosts can observe partially rebuilt commands, follow false display-list branches, and eventually overwrite game state through an invalid framebuffer extent.

F3DDKR's counted `G_DMADL` command is handled as a bounded stream of RDP words rather than a nested Fast3D control-flow list. The interpreter validates command addresses and budgets, and DKR's known `320 x 240` framebuffer receives a final height clamp before any native-to-RDRAM copy. Those checks are recovery boundaries; the submission lifetime barrier is the primary correctness fix.

## Renderer-specific compatibility

### Racer model transforms

DKR changes its two custom matrix slots frequently while rendering racers. An F3DDKR matrix selection must not manufacture a new view-projection identity for a pure M1/M2 switch. Doing so can associate otherwise valid racer vertices with the wrong transform and produce deterministic stretched heads, tails, backpacks, or vehicle parts.

The corresponding RT64 change lives in `submodule-patches/rt64-f3ddkr.patch` and should ultimately become a focused RT64 contribution.

### Framebuffers and viewports

Retail N64 code uses inclusive lower-right viewport bounds and relies on overscan. RT64 scissors are exclusive, so a nominal full-screen `319 x 239` bound can leave a one-pixel strip of stale framebuffer visible. The CPU patch expands only full-screen menu requests to the actual `320 x 240` boundary; partial viewports remain untouched.

Widescreen expansion is disabled for authored 4:3 pages that otherwise expose live scene data outside their frame. The runtime watches dedicated game state for track selection, the post-race flow, and standalone results, then restores the user's configured aspect ratio for normal racing.

### Steam Deck shader policy

Steam Deck disables background specialized-pipeline compilation during gameplay. Linux thread-priority behavior allowed those workers to compete with the game and graphics threads during scene changes, causing severe stutter and occasional driver failure.

The launcher preflight temporarily enables the compiler and waits for a bounded table of 74 shader descriptions captured from boot through the Ancient Lake water-heavy title attract scene using the Deck graphics preset. Gameplay then uses those specialized pipelines where they match and falls back to RT64's universal shader for every unlisted material; background compilation remains disabled. This preserves the stable fallback while reducing the steady universal-shader cost in the known-heavy scene.

Original refresh rate is also the safe default on Deck until generated-frame identity is reliable for dynamic racer display lists.

### Music animation clock

Player Select derives every dancer's pose from `music_animation_fraction`, while clouds and other background elements use the normal scene update. Retail mutates one shared music clock on every query and calls the helper separately for every dancer. Those queries are effectively simultaneous on N64, but native model work can spread them across a meaningful part of a Deck frame and assign visibly different poses to objects in the same update pass. The native wrapper advances the original clock on the first query of each Character Select game frame and returns that exact snapshot to every remaining dancer. Unsigned COUNT subtraction also handles equality and genuine 32-bit rollover without the retail equality edge case. Other callers continue to advance the clock normally.

## Dependency patches

| Patch | Purpose |
| --- | --- |
| `n64recomp-dkr.patch` | RSP branch wrapping, vector control-register instructions, and graphics DPC instruction support |
| `n64modernruntime-dkr.patch` | empty audio-task handling and synchronous display-list parse lifetime |
| `rt64-f3ddkr.patch` | F3DDKR decoding plus DKR renderer, framebuffer, transform, and shader fixes |
| `plume-sdl2-compat.patch` | Metal/Vulkan ownership and SDL compatibility fixes |

`scripts/apply-submodule-patches.sh` applies these idempotently. Patch files are a reproducibility mechanism, not a substitute for upstream review.

## Runtime data boundary

The source tree may contain symbols, offsets, hashes, and handwritten descriptions obtained through reverse engineering. It must not contain ROM bytes or extracted assets. At runtime, the user-selected ROM remains a local file and supplies the retail data required by the recompiled instructions.
