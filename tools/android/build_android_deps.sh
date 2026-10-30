#!/usr/bin/env bash
# Builds SDL2 and FreeType for Android arm64 with the NDK, into ~/Android/prefixes.
# Based on Zelda64Recomp-Android's tools/ci/setup_android_deps.sh (linkzenic), with pinned tarball hashes.
#
#   tools/android/build_android_deps.sh [work-dir]
set -euo pipefail

SDL2_VERSION=2.32.10
SDL2_SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
FREETYPE_VERSION=2.13.3
FREETYPE_SHA256=0550350666d427c74daeb85d5ac7bb353acba5f76956395995311a9c6f063289

: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
PREFIX_ROOT="${JFG_ANDROID_PREFIX_ROOT:-$HOME/Android/prefixes}"
WORK="${1:-${TMPDIR:-/tmp}/jfg-android-deps}"
CMAKE="$ANDROID_HOME/cmake/3.30.3/bin/cmake"
[ -x "$CMAKE" ] || CMAKE="$(command -v cmake)"
JOBS="${JOBS:-$(nproc)}"

mkdir -p "$WORK" "$PREFIX_ROOT"
HERE_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$WORK"

fetch() {  # url file sha256
  [ -f "$2" ] || curl -fsSL --retry 3 -o "$2" "$1"
  echo "$3  $2" | sha256sum -c -
}
fetch "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.tar.gz" \
  "SDL2-$SDL2_VERSION.tar.gz" "$SDL2_SHA256"
fetch "https://download.savannah.gnu.org/releases/freetype/freetype-$FREETYPE_VERSION.tar.xz" \
  "freetype-$FREETYPE_VERSION.tar.xz" "$FREETYPE_SHA256"

COMMON=(-G Ninja -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK_HOME/build/cmake/android.toolchain.cmake"
  -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release)

rm -rf "SDL2-$SDL2_VERSION" sdl-build && tar -xzf "SDL2-$SDL2_VERSION.tar.gz"
# Vulkan windows must not get EGL surfaces when Android recreates the surface (see the patch).
patch -d "SDL2-$SDL2_VERSION" -p1 < "$HERE_DIR/../../patches/android/sdl2-vulkan-surface.patch"
"$CMAKE" -S "SDL2-$SDL2_VERSION" -B sdl-build "${COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX_ROOT/SDL2-$SDL2_VERSION-android-arm64" -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TEST=OFF
"$CMAKE" --build sdl-build --parallel "$JOBS"
"$CMAKE" --install sdl-build

rm -rf "freetype-$FREETYPE_VERSION" ft-build && tar -xJf "freetype-$FREETYPE_VERSION.tar.xz"
"$CMAKE" -S "freetype-$FREETYPE_VERSION" -B ft-build "${COMMON[@]}" \
  -DCMAKE_INSTALL_PREFIX="$PREFIX_ROOT/freetype-$FREETYPE_VERSION-android-arm64" -DBUILD_SHARED_LIBS=OFF \
  -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE
"$CMAKE" --build ft-build --parallel "$JOBS"
"$CMAKE" --install ft-build

echo "SDL2:     $PREFIX_ROOT/SDL2-$SDL2_VERSION-android-arm64"
echo "FreeType: $PREFIX_ROOT/freetype-$FREETYPE_VERSION-android-arm64"
