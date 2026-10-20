#!/usr/bin/env bash
# Runs the game built by build_linux.sh.   ./run_linux.sh [path/to/rom.z64] [extra arguments]
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROM="${1:-$HERE/baserom.us.z64}"; [ $# -gt 0 ] && shift
EXE="$HERE/bin/JetForceGeminiRecompiled"
[ -x "$EXE" ] || { echo "Executable not found: $EXE - run ./build_linux.sh first." >&2; exit 1; }
[ -f "$ROM" ] || { echo "ROM not found: $ROM" >&2; exit 1; }
ROM="$(realpath "$ROM")"
cd "$HERE/bin"
exec ./JetForceGeminiRecompiled --rom "$ROM" --skip-launcher "$@"
