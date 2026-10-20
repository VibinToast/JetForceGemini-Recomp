# Building Diddy Kong Racing: Recompiled

This guide describes the current development build. The process has two distinct stages:

1. N64Recomp and RSPRecomp translate instructions from a private, verified ROM into ignored local source files.
2. CMake builds those generated files together with the native runtime.

No ROM or extracted proprietary asset may be committed, copied into a tracked directory, or bundled with a package.

## Supported input

The only supported input is the unmodified **Diddy Kong Racing USA (En,Fr) Rev 1** ROM:

- decomp identifier: `us.v80`
- byte order expected by the build: big-endian `.z64`
- SHA-1: `6d96743d46f8c0cd0edb0ec5600b003c89b93755`

The launcher performs its own version check at runtime. A different region, revision, patched ROM, or bad dump will be rejected.

## Prerequisites

All builds require:

- Git with submodule support;
- CMake 3.20 or newer;
- Ninja;
- a recent Clang toolchain;
- Python 3;
- the supported ROM above.

For macOS, install Xcode Command Line Tools and, if needed:

```bash
brew install cmake ninja
```

For Ubuntu 22.04-compatible Linux builds, the current package set is recorded in [`scripts/steamdeck-builder.Dockerfile`](scripts/steamdeck-builder.Dockerfile). Steam Deck packaging additionally requires Docker with `buildx` support.

Windows support is inherited from the base runtime but has not yet been validated for this DKR conversion.

## 1. Clone and prepare submodules

```bash
git clone --recurse-submodules <repository-url> dkr-recomp
cd dkr-recomp
```

If the repository was cloned without submodules:

```bash
git submodule update --init --recursive
```

This prototype currently carries reviewed dependency changes as patch files rather than permanent submodule branches. Apply them once after initializing the submodules:

```bash
./scripts/apply-submodule-patches.sh
```

The script is idempotent: an already applied patch is detected and left alone.

## 2. Provide the private ROM

Place a private copy or symlink at the repository root:

```text
baserom.us.v80.z64
```

Prefer a symlink to a private ROM directory. Confirm its identity before generating code:

```bash
shasum -a 1 baserom.us.v80.z64
```

The expected output begins with the SHA-1 shown under [Supported input](#supported-input). The repository ignores `*.z64`, but ignore rules are not a substitute for checking what is staged before every commit.

## 3. Build the recompilation tools

Build the N64Recomp CLI and RSPRecomp from the patched submodule:

```bash
cmake -S lib/N64Recomp -B build-tools -G Ninja \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build-tools --parallel \
    --target N64RecompCLI RSPRecomp
```

The expected tool executables are:

```text
build-tools/N64Recomp
build-tools/RSPRecomp
```

## 4. Generate CPU and RSP sources

```bash
N64RECOMP="$PWD/build-tools/N64Recomp" \
RSPRECOMP="$PWD/build-tools/RSPRecomp" \
./scripts/generate-recomp.sh
```

This produces:

- CPU translations under `RecompiledFuncs/`;
- `rsp/aspMain.cpp` for audio;
- `rsp/f3ddkr_{xbus,fifo,dram}.cpp` for the custom graphics microcodes.

These outputs are generated from the user's ROM. They are local build inputs and are not the place to make durable source changes. The checked-in TOML files and native runtime code are the reproducible source of the conversion.

The checked-in `dkr.us.v80.syms.toml` is sufficient for normal builds. Maintainers regenerating that manifest from the matching decompilation ELF can use `scripts/prepare-input.sh`, but that is not part of the normal build.

## 5. Build macOS

```bash
cmake -S . -B build-macos -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build-macos --parallel
```

The result is:

```text
build-macos/Diddy Kong Racing Recompiled.app
```

Verify the ad-hoc development signature:

```bash
codesign --verify --deep --strict \
    "build-macos/Diddy Kong Racing Recompiled.app"
```

The current development bundle is Apple-silicon only and is not notarized.

## 6. Build Linux locally

Install the dependencies listed in the Steam Deck builder image, then run:

```bash
cmake -S . -B build-linux -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_C_COMPILER=clang \
    -DCMAKE_CXX_COMPILER=clang++
cmake --build build-linux --parallel
```

The raw executable is:

```text
build-linux/DiddyKongRacingRecompiled
```

## 7. Build the Steam Deck AppImage

From macOS or Linux with Docker running:

```bash
./scripts/build-steamdeck-appimage.sh
```

The script builds an Ubuntu 22.04 `linux/amd64` binary, stages a complete AppDir, rejects ROM-like payloads, repacks and reopens the final SquashFS image, and creates:

```text
dist-steamdeck/DiddyKongRacingRecompiled-SteamDeck-x86_64.AppImage
dist-steamdeck/DiddyKongRacingRecompiled-SteamDeck-<version>.tar.gz
```

Use the tarball for browser or cloud delivery because it preserves the AppImage executable bit. `DKR_SKIP_COMPILE=1` may be set only when a compatible binary already exists under `build-steamdeck/` and only packaging needs to be repeated.

## 8. Validate the output

At minimum:

```bash
git diff --check
find build-macos dist-steamdeck -type f \
    \( -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*baserom*' \)
```

The second command must print nothing. Follow the platform and gameplay checks in [docs/TESTING.md](docs/TESTING.md) before publishing a build.

## Troubleshooting

### The generator reports a missing ROM

Confirm that `baserom.us.v80.z64` exists at the repository root and matches the supported SHA-1. A launcher-selected ROM elsewhere on the machine is not automatically used for source generation.

### A submodule build lacks DKR support

Run `./scripts/apply-submodule-patches.sh` and inspect `git submodule status`. Do not reset a dirty submodule without first determining whether it contains the DKR patch.

### Steam Deck starts without a launcher or immediately exits

Use the permission-preserving tarball, extract it, and confirm the AppImage is executable. Inspect `~/.config/DiddyKongRacingRecompiled/runtime.log` and `last-exit.log`.

### The first scene stutters

Start the game through the launcher so common RT64 pipelines finish their preflight warmup. The `--skip-launcher` option is intended for debugging and bypasses that normal flow.
