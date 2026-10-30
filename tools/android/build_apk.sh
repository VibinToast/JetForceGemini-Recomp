#!/usr/bin/env bash
# Packages the Android arm64 build into an installable APK with the Android SDK's own tools (no Gradle, no downloads).
#
#   tools/android/build_apk.sh [build-dir] [output.apk]
#
# build-dir is the NDK CMake build that produced libmain.so (default: build-android, see docs/ANDROID.md).
# The APK is signed with the local debug key (~/.android/debug.keystore, created if missing), which is fine for
# sideloading. Releases should sign with their own key: set JFG_ANDROID_KEYSTORE, JFG_ANDROID_KEY_ALIAS and
# JFG_ANDROID_KEYSTORE_PASS.
# Diagnostics: JFG_ANDROID_VULKAN_LAYER=/path/to/libVkLayer_khronos_validation.so builds a debuggable APK with that
# layer inside; enable it with `adb shell setprop debug.vulkan.layers VK_LAYER_KHRONOS_validation`.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$(realpath "${1:-$HERE/build-android}")"
OUT="$(realpath -m "${2:-$HERE/bin-android/JetForceGemini-android-arm64.apk}")"

: "${ANDROID_HOME:=$HOME/Android/Sdk}"
: "${ANDROID_NDK_HOME:=$ANDROID_HOME/ndk/26.0.10792818}"
BUILD_TOOLS="$ANDROID_HOME/build-tools/35.0.0"
ANDROID_JAR="$ANDROID_HOME/platforms/android-35/android.jar"
SDL2_PREFIX="${JFG_ANDROID_SDL2_PREFIX:-$HOME/Android/prefixes/SDL2-2.32.10-android-arm64}"
STRIP="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip"
MIN_SDK=28
TARGET_SDK=35

VERSION="$(head -n1 "$HERE/version.txt")"
# versionCode: 1.2.4 -> 10204, so every version updates over the previous install.
IFS=. read -r V_MAJOR V_MINOR V_PATCH <<< "${VERSION%%-*}"
VERSION_CODE=$(( V_MAJOR * 10000 + V_MINOR * 100 + ${V_PATCH:-0} ))

say() { echo "[jfg-apk] $*"; }
die() { echo "[jfg-apk] ERROR: $*" >&2; exit 1; }
[ -f "$BUILD/libmain.so" ] || die "no libmain.so in $BUILD (build the Android target first)"
[ -f "$SDL2_PREFIX/lib/libSDL2.so" ] || die "no libSDL2.so in $SDL2_PREFIX (run tools/android/build_android_deps.sh)"
for t in aapt2 d8 zipalign apksigner; do [ -x "$BUILD_TOOLS/$t" ] || die "$BUILD_TOOLS/$t missing"; done
[ -f "$ANDROID_JAR" ] || die "$ANDROID_JAR missing (sdkmanager \"platforms;android-35\")"

WORK="$BUILD/apk"
rm -rf "$WORK" && mkdir -p "$WORK"/{res,gen,classes,dex,stage/lib/arm64-v8a,stage/assets/program}
APP="$HERE/android/app/src/main"

say "resources and manifest"
"$BUILD_TOOLS/aapt2" compile --dir "$APP/res" -o "$WORK/res/res.zip"
"$BUILD_TOOLS/aapt2" link -I "$ANDROID_JAR" --manifest "$APP/AndroidManifest.xml" \
  --min-sdk-version $MIN_SDK --target-sdk-version $TARGET_SDK \
  --version-code $VERSION_CODE --version-name "$VERSION" ${JFG_ANDROID_VULKAN_LAYER:+--debug-mode} \
  --java "$WORK/gen" -o "$WORK/base.apk" "$WORK/res/res.zip"

say "java"
find "$APP/java" "$WORK/gen" -name '*.java' > "$WORK/sources.txt"
javac -nowarn -Xlint:-options -source 11 -target 11 -encoding UTF-8 -classpath "$ANDROID_JAR" \
  -d "$WORK/classes" @"$WORK/sources.txt"
"$BUILD_TOOLS/d8" --release --min-api $MIN_SDK --lib "$ANDROID_JAR" --output "$WORK/dex" \
  $(find "$WORK/classes" -name '*.class')

say "native libraries and program files"
"$STRIP" --strip-unneeded -o "$WORK/stage/lib/arm64-v8a/libmain.so" "$BUILD/libmain.so"
cp "$SDL2_PREFIX/lib/libSDL2.so" "$WORK/stage/lib/arm64-v8a/"
if [ -n "${JFG_ANDROID_VULKAN_LAYER:-}" ]; then
  say "debuggable build with Vulkan layer $(basename "$JFG_ANDROID_VULKAN_LAYER")"
  cp "$JFG_ANDROID_VULKAN_LAYER" "$WORK/stage/lib/arm64-v8a/"
fi
cp -r "$HERE/assets" "$WORK/stage/assets/program/assets"
cp "$HERE/recompcontrollerdb.txt" "$WORK/stage/assets/program/"
cp "$WORK/dex/classes.dex" "$WORK/stage/"

cp "$WORK/base.apk" "$WORK/unsigned.apk"
(cd "$WORK/stage" && zip -qr -X "$WORK/unsigned.apk" classes.dex lib assets)

say "align and sign"
"$BUILD_TOOLS/zipalign" -f -p 4 "$WORK/unsigned.apk" "$WORK/aligned.apk"
KEYSTORE="${JFG_ANDROID_KEYSTORE:-$HOME/.android/debug.keystore}"
ALIAS="${JFG_ANDROID_KEY_ALIAS:-androiddebugkey}"
PASS="${JFG_ANDROID_KEYSTORE_PASS:-android}"
if [ ! -f "$KEYSTORE" ] && [ -z "${JFG_ANDROID_KEYSTORE:-}" ]; then
  mkdir -p "$(dirname "$KEYSTORE")"
  keytool -genkeypair -keystore "$KEYSTORE" -storepass android -keypass android -alias androiddebugkey \
    -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Android Debug,O=Android,C=US" >/dev/null
fi
mkdir -p "$(dirname "$OUT")"
"$BUILD_TOOLS/apksigner" sign --ks "$KEYSTORE" --ks-key-alias "$ALIAS" --ks-pass "pass:$PASS" \
  --out "$OUT" "$WORK/aligned.apk"
"$BUILD_TOOLS/apksigner" verify "$OUT"
say "APK: $OUT ($(du -h "$OUT" | cut -f1), version $VERSION / $VERSION_CODE)"
