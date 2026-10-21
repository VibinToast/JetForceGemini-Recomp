#!/usr/bin/env bash
# Packs the Linux game that build_linux.sh put in <source>/bin into the two Linux release files:
#
#   JFG-<version>-linux-x86_64-portable.tar.gz   a folder like the Windows zip; saves and settings stay in it
#   JFG-<version>-linux-x86_64.AppImage          one file to run (SteamOS: "Add a Non-Steam Game"); saves and settings go
#                                                to ~/.local/share/JetForceGeminiRecompiled (JFG_GAME_DIR to change it)
#
# plus a .sha256 for each. Run by tools/package_release.sh, normally inside the tools/linux-build container:
#   tools/linux-build/package.sh <source folder> <version> <output folder>
# Needs patchelf, appimagetool, binutils. SDL2 goes in with the game (so controller support does not depend on the
# system); Vulkan, the GPU drivers, GTK and the C/C++ runtime come from the system, as they must.
set -euo pipefail
SRC="$(realpath "$1")"; VERSION="$2"; OUT="$(realpath "$3")"
BIN="$SRC/bin"
EXE="$BIN/JetForceGeminiRecompiled"
BUNDLE="libSDL2-2.0.so.0"

say() { echo "[jfg] $*"; }
die() { echo "[jfg] ERROR: $*" >&2; exit 1; }

[ -x "$EXE" ] || die "$EXE not found - build first"
for t in patchelf appimagetool strip objdump ldd sha256sum tar; do command -v "$t" >/dev/null || die "$t not found"; done
missing=$(ldd "$EXE" | awk '/not found/ {print $1}')
[ -z "$missing" ] || die "libraries not found for the game: $missing"
say "needs glibc $(objdump -T "$EXE" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -n 1 | cut -d_ -f2) or newer"

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUT"

# ---- the game folder ---------------------------------------------------------------------------------------------
# Only what a new player needs. Saves, settings, dumps and ROM copies in bin are yours and stay out.
NAME="JFG-$VERSION-linux-x86_64-portable"
GAME="$STAGE/$NAME"
mkdir -p "$GAME/lib" "$GAME/icons" "$GAME/docs" "$GAME/mods" "$GAME/texture_packs"
cp "$EXE" "$GAME/"
strip --strip-debug "$GAME/JetForceGeminiRecompiled"
cp "$BIN/recompcontrollerdb.txt" "$GAME/"
cp -r "$BIN/assets" "$GAME/"
cp "$BIN/icons/512.png" "$GAME/icons/"
cp "$SRC/LICENSE" "$SRC/README.md" "$SRC/CHANGELOG.md" "$GAME/"
cp "$SRC/docs/TEXTURE_PACKS.md" "$GAME/docs/"
for lib in $BUNDLE; do
  path=$(ldd "$EXE" | awk -v l="$lib" '$1 == l {print $3}')
  if [ -n "$path" ]; then cp -L "$path" "$GAME/lib/"; say "bundled $lib"; fi
done
patchelf --set-rpath '$ORIGIN/lib' "$GAME/JetForceGeminiRecompiled"

tar -C "$STAGE" --owner=0 --group=0 -czf "$OUT/$NAME.tar.gz" "$NAME"
(cd "$OUT" && sha256sum "$NAME.tar.gz" > "$NAME.tar.gz.sha256")
say "made $OUT/$NAME.tar.gz"

# ---- the AppImage ------------------------------------------------------------------------------------------------
# An AppImage is read-only, but the game keeps its settings and saves next to itself. So AppRun copies the game into a
# folder you own (again whenever the build changes) and starts it from there.
APP="JFG-$VERSION-linux-x86_64.AppImage"
APPDIR="$STAGE/AppDir"
mkdir -p "$APPDIR/game"
cp -a "$GAME/." "$APPDIR/game/"
rm -rf "$APPDIR/game/mods" "$APPDIR/game/texture_packs"
# The version plus a fingerprint of the game, so a test build with the same version number is copied over too.
echo "$VERSION $(sha256sum "$GAME/JetForceGeminiRecompiled" | cut -c1-16)" > "$APPDIR/game/.appimage_version"
cp "$GAME/icons/512.png" "$APPDIR/jetforcegeminirecompiled.png"
cat > "$APPDIR/jetforcegeminirecompiled.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Jet Force Gemini: Recompiled
Exec=JetForceGeminiRecompiled
Icon=jetforcegeminirecompiled
Categories=Game;
Terminal=false
EOF
cat > "$APPDIR/AppRun" <<'EOF'
#!/bin/sh
# Copies the game into a folder you own when the build changed, then starts it from there.
HERE="$(dirname "$(readlink -f "$0")")"
DIR="${JFG_GAME_DIR:-${XDG_DATA_HOME:-$HOME/.local/share}/JetForceGeminiRecompiled}"
mkdir -p "$DIR/mods" "$DIR/texture_packs" || { echo "Cannot create $DIR" >&2; exit 1; }
if [ "$(cat "$HERE/game/.appimage_version")" != "$(cat "$DIR/.appimage_version" 2>/dev/null)" ]; then
  rm -rf "$DIR/assets" "$DIR/lib"
  cp -rf "$HERE/game/." "$DIR/" || { echo "Cannot copy the game into $DIR" >&2; exit 1; }
fi
# The game runs from the copy, not from this mount: with APPIMAGE set it would look for its assets in $APPDIR/usr/bin
# (get_program_path in src/runtime/support.cpp), which this AppImage does not have, and a restarted game would point at
# a mount that is already gone. Unset, it uses the folder it runs in.
unset APPIMAGE APPDIR
cd "$DIR" && exec ./JetForceGeminiRecompiled "$@"
EOF
chmod +x "$APPDIR/AppRun"
ARCH=x86_64 appimagetool --no-appstream "$APPDIR" "$OUT/$APP"
(cd "$OUT" && sha256sum "$APP" > "$APP.sha256")
say "made $OUT/$APP"
