# Testing

Testing is layered so a packaging success cannot be mistaken for gameplay correctness.

## Validation layers

### 1. Source and patch checks

```bash
git diff --check
git status --short
./scripts/apply-submodule-patches.sh
```

For each dependency patch, verify both forward application to a clean pinned submodule and reverse detection after it has been applied.

### 2. Clean platform builds

- Configure into a new build directory rather than reusing an old cache.
- Build macOS with `RelWithDebInfo` and verify the bundle using `codesign --verify --deep --strict`.
- Build Steam Deck through `scripts/build-steamdeck-appimage.sh` so the same Ubuntu 22.04 environment and packaging checks are used.
- Record compiler, dependency revisions, architecture, and package SHA-256.

### 3. Package inspection

Every deliverable must be checked for:

- correct architecture;
- complete launcher assets and controller database;
- executable AppImage mode inside the delivery tarball;
- absence of `.z64`, `.n64`, `.v64`, `baserom`, extracted asset, and private log payloads;
- preserved application signature on macOS;
- a fresh install path rather than only an in-place upgrade.

### 4. Automated Steam Deck smoke test

`scripts/smoke-test-steamdeck-appimage.sh` accepts an AppImage and a private test ROM. Under Xvfb it verifies:

1. the launcher becomes visible;
2. RT64 reports successful renderer setup;
3. the Start Game action begins shader preflight;
4. common shader pipelines finish warming in the expected order;
5. the process remains alive throughout the sequence.

Run the test inside a Linux x86-64 environment with Xvfb, xdotool, Vulkan/Mesa support, and a dummy SDL audio driver. The private ROM should be mounted read-only and must not be copied into the resulting artifact.

For renderer profiling from macOS, first build the staged AppDir and then run:

```bash
./scripts/build-steamdeck-appimage.sh
./scripts/profile-steamdeck-linux-vulkan.sh player-select /path/to/private-dkr-rev1.z64
./scripts/profile-steamdeck-linux-vulkan.sh overworld /path/to/private-dkr-rev1.z64
./scripts/profile-steamdeck-linux-vulkan.sh first-track /path/to/private-dkr-rev1.z64
```

This runs the x86-64 build with Xvfb and Mesa's Vulkan software renderer. It is useful for Linux/Vulkan startup, crash, automation, and relative draw-call checks. QEMU translation and software rendering make its absolute frame time unsuitable as a Steam Deck performance measurement; final performance still requires physical Deck hardware.

### 5. Gameplay regression

At minimum, exercise:

- first-run ROM selection and a subsequent stored-ROM launch;
- title screen, attract mode, and the water-heavy attract scenes;
- controller navigation, character selection, and held acceleration;
- Tracks mode with multiple characters and an eight-racer grid;
- close racer models during the countdown and first turns;
- a completed race through Race Times and Race Order;
- widescreen gameplay and automatic 4:3 post-race presentation;
- Adventure save creation, application restart, and save reload;
- clean exit from the launcher and from active gameplay.

Repeat renderer changes with at least Banjo and Krunch because their attached model groups exposed previous transform-identity failures clearly.

## Current tested baseline

As of the 0.1.17 development series:

| Check | macOS arm64 | Steam Deck x86-64 |
| --- | --- | --- |
| Clean compile | Pass | Pass |
| Launcher and ROM validation | Pass | Pass |
| Renderer initialization | Pass | Pass |
| Universal and 74-pipeline specialized shader preflight | Pass | Pass (headless) |
| Xbox-compatible controls | Pass | Pass |
| Normal race | Pass | Pass |
| Racer stretching regression | Pass | Pass |
| Player Select animation and display-list stress | Pass | Pass (512-frame lifetime soak plus 90-second frame-coherent music-clock soak under emulated Vulkan) |
| Race completion | Pass | Pass |
| Redundant geometry-state batching | Pass (about 3,900 to 482 calls/frame in Adventure overworld) | Pass (Linux/Vulkan smoke and physical Deck) |
| Water-heavy attract performance | Pass | Pass (physical Deck) |
| Latest post-race pillarbox visual pass | Pass | Pass (physical Deck) |
| Full Adventure / multiplayer / bosses | Incomplete | Incomplete |

This table is evidence for the named paths only; it is not a claim of complete game compatibility.

## Crash and performance reports

Capture:

- the last actions before failure;
- whether original refresh rate was selected;
- `runtime.log`, `runtime.previous.log`, `last-exit.log`, and `crash.log` when present;
- a macOS Diagnostic Report for native crashes;
- frame-rate behavior before and after the affected scene transition.

Review logs for local usernames, ROM paths, and other personal data before sharing them.

Useful diagnostic variables include:

| Variable | Purpose |
| --- | --- |
| `DKR_ANIM_COMPARE=1` | A/B compare native and recompiled `obj_animate`; retail output remains live |
| `DKR_ANIM_NATIVE=1` | use the diagnostic native animation implementation |
| `DKR_ANIM_INCREMENTAL=1` | use incremental rather than stateless native animation state |
| `DKR_ANIM_TRACE=1` | emit bounded animation diagnostics |
| `DKR_ANIM_PERF=1` | report cost and call count for the retail/native object animator |
| `DKR_MUSIC_ANIM_TRACE=1` | verify that Player Select dancers share one music-clock snapshot per game frame |
| `RT64_DKR_DL_TRACE=1` | trace selected DKR display-list activity |
| `RT64_DKR_FB_OVERLAP_LOG=1` | report framebuffer overlap and synchronization diagnostics |
| `DKR_GFX_PROFILE=1` | report display-list parse time and graphics-queue depth once per second |
| `RT64_DKR_PERF_LOG=1` | report GPU time, framebuffer pairs, and draw calls once per second |
| `RT64_DKR_SHADER_CAPTURE=/path/to/file` | capture unique renderer shader descriptions for maintaining the bounded preload table |

These switches are developer tools, not supported performance options. Reproduce a bug without them before reporting a release regression.

## Release gate

A public release candidate should not ship until:

- the source tree and documentation are publishable under GPLv3;
- a clean clone can apply dependency patches and build without machine-specific paths;
- macOS and Steam Deck artifacts pass package inspection;
- both platforms complete the gameplay checklist above;
- all supported deliverables are versioned consistently;
- release notes identify remaining limitations and include no copyrighted game data.
