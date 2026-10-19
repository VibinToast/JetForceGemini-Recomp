#!/usr/bin/env bash
# Jet Force Gemini: Recompiled - Linux build.
#
#   ./build_linux.sh [path/to/rom.z64]
#
# The ROM defaults to baserom.us.z64 next to this script. The build folder defaults to ./build
# (override with JFG_PORT_DIR=/some/path); the finished game goes to ./bin, where its saves and settings
# are kept too. See README.md for the packages this needs.
#
# What it does:
#   1. downloads the base project (cyscott/dkr-recomp, GPLv3, pinned commit) and its libraries
#   2. applies the base's dependency patches and the JFG patches
#   3. builds the recompiler tools and translates the game code from YOUR ROM to C
#   4. builds the executable and puts the finished game in ./bin
# Git is used for downloads only; nothing is uploaded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROM="${1:-$HERE/baserom.us.z64}"
WORK="${JFG_PORT_DIR:-$HERE/build}"
BASE_URL=https://github.com/cyscott/dkr-recomp.git
BASE_COMMIT=eb8151c05837359c9eca0ad006077a14f5bf72b8
ROM_SHA1=493ced9008dbe932d6e91179b68e8630cf23a023

say() { echo "[jfg] $*"; }
die() { echo "[jfg] ERROR: $*" >&2; exit 1; }

[ -f "$ROM" ] || die "ROM not found: $ROM (put your Jet Force Gemini US ROM there, or pass its path)"
ROM="$(realpath "$ROM")"
have="$(sha1sum "$ROM" | cut -d' ' -f1)"
[ "$have" = "$ROM_SHA1" ] || die "wrong ROM: SHA-1 is $have, expected $ROM_SHA1 (US version, .z64 / big-endian format)"
case "$WORK" in *" "*) die "the build folder path contains a space ($WORK); the libraries' build scripts cannot handle that. Move this folder to a path without spaces, or set JFG_PORT_DIR to one.";; esac
for t in git cmake ninja clang clang++ python3; do command -v "$t" >/dev/null || die "$t not found - see README.md for the package list"; done

# ---- 1. base project -------------------------------------------------------
if [ ! -d "$WORK/.git" ]; then
  say "downloading the base project into $WORK"
  git clone "$BASE_URL" "$WORK"
  git -C "$WORK" checkout --detach "$BASE_COMMIT"
fi
cd "$WORK"
say "fetching libraries (slow the first time)"
git submodule update --init --recursive --depth 1

# ---- 2. patches ------------------------------------------------------------
# Put the files the JFG patches touch back to the downloaded state, so a re-run always applies cleanly.
git checkout -- CMakeLists.txt src assets icons
git -C lib/rt64 checkout -- .
git -C lib/rt64 clean -fdq -- src/gbi src/hle src/shaders src/shared
apply_once() { # <repository> <patch>
  if git -C "$1" apply --recount --reverse --check "$2" >/dev/null 2>&1; then say "already applied: $(basename "$2")"
  else git -C "$1" apply --recount "$2"; say "applied: $(basename "$2")"; fi
}
apply_once lib/N64Recomp               "$WORK/submodule-patches/n64recomp-dkr.patch"
apply_once lib/N64ModernRuntime        "$WORK/submodule-patches/n64modernruntime-dkr.patch"
apply_once lib/rt64                    "$WORK/submodule-patches/rt64-f3ddkr.patch"
apply_once lib/rt64/src/contrib/plume  "$WORK/submodule-patches/plume-sdl2-compat.patch"
apply_once .                           "$HERE/patches/jfg-base.patch"
apply_once lib/rt64                    "$HERE/patches/rt64-jfg.patch"
python3 "$HERE/tools/set_version.py" "$HERE/version.txt" .   # version.txt (updated by the release workflow) -> the launcher

rm -rf src/jfg   # only the port's files: emptied so a file removed or renamed here does not stay in the build
mkdir -p src/jfg include tools
cp "$HERE"/src/jfg/*.cpp src/jfg/
cp "$HERE"/src/jfg/*.h "$HERE"/src/jfg/*.inc src/jfg/
cp "$HERE"/assets/config_menu/jfg_*.rml assets/config_menu/   # the port's Select-menu tabs (tools/gen_jfg_config_rml.py)
mkdir -p assets/jfg_launcher && cp "$HERE"/assets/jfg_launcher/* assets/jfg_launcher/   # the launcher's fonts and images
cp "$HERE"/include/*.h include/
cp "$HERE"/icons/* icons/
cp "$HERE"/jfg.us.toml "$HERE"/jfg.us.syms.toml "$HERE"/rsp-aspMain.jfg.toml .
cp "$HERE"/tools/*.py "$HERE"/tools/jfg.us.dynamic_relocs.json tools/

# ---- 3. game code from the ROM ---------------------------------------------
say "building the recompiler tools"
cmake -S lib/N64Recomp -B build-tools -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-tools --parallel --target N64RecompCLI RSPRecomp
say "translating the game code (your ROM -> C)"
cp -f "$ROM" baserom.us.z64
# Recompiler input only: a copy of the ROM with the overlays pre-linked at private addresses. Never used at run time.
python3 tools/prepare_overlays.py baserom.us.z64 - baserom.us.recomp.z64 - tools/jfg.us.dynamic_relocs.json
rm -rf RecompiledFuncs && mkdir RecompiledFuncs
./build-tools/N64Recomp jfg.us.toml > recomp.log 2>&1 || { tail -20 recomp.log; die "N64Recomp failed (full output: $WORK/recomp.log)"; }
./build-tools/RSPRecomp rsp-aspMain.jfg.toml       # audio microcode -> rsp/aspMain.cpp
python3 tools/merge_gen_anim_data.py RecompiledFuncs
python3 tools/fix_dynamic_relocs.py RecompiledFuncs tools/jfg.us.dynamic_relocs.json baserom.us.recomp.z64 jfg.us.syms.toml
python3 tools/refuse_mod_hooks.py RecompiledFuncs tools/jfg.us.dynamic_relocs.json jfg.us.syms.toml jfg.us.toml

# ---- 4. build --------------------------------------------------------------
say "building (10-40 minutes the first time)"
cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build-linux --parallel
[ -x build-linux/JetForceGeminiRecompiled ] || die "build finished but the executable is missing"

# ---- 5. game folder --------------------------------------------------------
# Everything the game needs to run, in one place. Saves and settings are written here as well.
BIN="$HERE/bin"
mkdir -p "$BIN/icons"
cp -f build-linux/JetForceGeminiRecompiled "$BIN/"
cp -f recompcontrollerdb.txt "$BIN/"
cp -f icons/512.png "$BIN/icons/"
rm -rf "$BIN/assets" && cp -r assets "$BIN/"
# Empty folders for mods and texture packs (.rtz files or folders); existing ones, and what is in them, are left alone.
mkdir -p "$BIN/mods" "$BIN/texture_packs"
say "BUILD OK: $BIN/JetForceGeminiRecompiled"
say "next: ./run_linux.sh"
