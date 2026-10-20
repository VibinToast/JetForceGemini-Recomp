#!/usr/bin/env bash
# Jet Force Gemini: Recompiled - Linux build.
#
#   ./build_linux.sh [path/to/rom.z64]
#
# The ROM defaults to baserom.us.z64 next to this script. The game is built in this folder (build-tools,
# build-linux, RecompiledFuncs; none of it is committed); the finished game goes to ./bin, where its saves and
# settings are kept too. See README.md for the packages this needs.
#
# What it does:
#   1. downloads the libraries (git submodules: the port's forks of N64Recomp, N64ModernRuntime and RT64,
#      each pinned to a commit on its jfg branch, and the base project's other libraries)
#   2. builds the recompiler tools and translates the game code from YOUR ROM to C (skipped when nothing it is
#      made from changed; JFG_REGENERATE=1 does it anyway)
#   3. builds the executable and puts the finished game in ./bin
# Set JFG_KEEP_LIBS=1 to leave lib/ as it is (for work inside a library: no checkout to the pinned commits).
# Set JFG_BUILD_DIR to put the CMake folders (build-tools, build-linux) somewhere else (the release container does).
# Git is used for downloads only; nothing is uploaded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROM="${1:-$HERE/baserom.us.z64}"
ROM_SHA1=493ced9008dbe932d6e91179b68e8630cf23a023
OUT="${JFG_BUILD_DIR:-$HERE}"

say() { echo "[jfg] $*"; }
die() { echo "[jfg] ERROR: $*" >&2; exit 1; }

[ -f "$ROM" ] || die "ROM not found: $ROM (put your Jet Force Gemini US ROM there, or pass its path)"
ROM="$(realpath "$ROM")"
have="$(sha1sum "$ROM" | cut -d' ' -f1)"
[ "$have" = "$ROM_SHA1" ] || die "wrong ROM: SHA-1 is $have, expected $ROM_SHA1 (US version, .z64 / big-endian format)"
case "$HERE" in *" "*) die "this folder's path contains a space ($HERE); the libraries' build scripts cannot handle that. Move it to a path without spaces.";; esac
for t in git cmake ninja clang clang++ python3; do command -v "$t" >/dev/null || die "$t not found - see README.md for the package list"; done
cd "$HERE"

# ---- 1. libraries ----------------------------------------------------------
if [ -n "${JFG_KEEP_LIBS:-}" ]; then
  say "JFG_KEEP_LIBS is set: lib/ is left as it is"
else
  say "fetching libraries (slow the first time)"
  git submodule update --init --recursive
fi

# ---- 2. game code from the ROM ---------------------------------------------
say "building the recompiler tools"
cmake -S lib/N64Recomp -B "$OUT/build-tools" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$OUT/build-tools" --parallel --target N64RecompCLI RSPRecomp
# The game code is generated again only when something it is made from changed (the hash below, saved with it), so
# a rebuild after a change in src/ or lib/ compiles only that. JFG_REGENERATE=1 generates it again anyway.
STAMP=RecompiledFuncs/.inputs_hash
inputs_hash() { python3 tools/recomp_inputs_hash.py "$ROM" jfg.us.toml JFGRecompSyms/jfg.us.syms.toml rsp-aspMain.jfg.toml tools/*.py \
  tools/jfg.us.dynamic_relocs.json tools/wrapped_funcs.txt "$OUT/build-tools/N64Recomp" "$OUT/build-tools/RSPRecomp"; }
if [ -z "${JFG_REGENERATE:-}" ] && [ -f "$STAMP" ] && [ -f rsp/aspMain.cpp ] && [ "$(cat "$STAMP")" = "$(inputs_hash)" ]; then
  say "game code is up to date (ROM, recompiler configs and tools unchanged): not generated again"
else
  say "translating the game code (your ROM -> C)"
  [ "$ROM" = "$HERE/baserom.us.z64" ] || cp -f "$ROM" baserom.us.z64
  # Recompiler input only: a copy of the ROM with the overlays pre-linked at private addresses. Never used at run time.
  python3 tools/prepare_overlays.py baserom.us.z64 - baserom.us.recomp.z64 - tools/jfg.us.dynamic_relocs.json
  rm -rf RecompiledFuncs && mkdir RecompiledFuncs
  "$OUT/build-tools/N64Recomp" jfg.us.toml > recomp.log 2>&1 || { tail -20 recomp.log; die "N64Recomp failed (full output: $HERE/recomp.log)"; }
  "$OUT/build-tools/RSPRecomp" rsp-aspMain.jfg.toml       # audio microcode -> rsp/aspMain.cpp
  python3 tools/merge_gen_anim_data.py RecompiledFuncs
  python3 tools/fix_dynamic_relocs.py RecompiledFuncs tools/jfg.us.dynamic_relocs.json baserom.us.recomp.z64 JFGRecompSyms/jfg.us.syms.toml
  python3 tools/refuse_mod_hooks.py RecompiledFuncs tools/jfg.us.dynamic_relocs.json JFGRecompSyms/jfg.us.syms.toml jfg.us.toml
  python3 tools/wrap_functions.py RecompiledFuncs tools/wrapped_funcs.txt
  inputs_hash > "$STAMP"   # last, so a run that stopped half-way generates again next time
fi

# ---- 3. build --------------------------------------------------------------
say "building (10-40 minutes the first time)"
cmake -S . -B "$OUT/build-linux" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build "$OUT/build-linux" --parallel
[ -x "$OUT/build-linux/JetForceGeminiRecompiled" ] || die "build finished but the executable is missing"

# ---- 4. game folder --------------------------------------------------------
# Everything the game needs to run, in one place. Saves and settings are written here as well.
BIN="$HERE/bin"
mkdir -p "$BIN/icons"
cp -f "$OUT/build-linux/JetForceGeminiRecompiled" "$BIN/"
cp -f recompcontrollerdb.txt "$BIN/"
cp -f icons/512.png "$BIN/icons/"
rm -rf "$BIN/assets" && cp -r assets "$BIN/"
# The changelog the launcher shows (About > Changelog) ships with the assets, so it also updates with the game.
cp -f CHANGELOG.md "$BIN/assets/jfg_launcher/CHANGELOG.md"
# Empty folders for mods and texture packs (.rtz files or folders); existing ones, and what is in them, are left alone.
mkdir -p "$BIN/mods" "$BIN/texture_packs"
say "BUILD OK: $BIN/JetForceGeminiRecompiled"
say "next: ./run_linux.sh"
