#!/usr/bin/env bash
# Applies the Android changes in patches/android to the pinned library checkouts (lib/rt64, its Plume, and
# lib/N64ModernRuntime). Safe to run again: a patch that is already applied is skipped. The changes are Android-only
# or harmless on desktop, so the desktop builds keep working on patched libraries.
#
#   tools/android/apply_patches.sh
set -euo pipefail
HERE="$(cd "$(dirname "$0")/../.." && pwd)"

apply() {  # library-dir patch
  local dir="$HERE/$1" patch="$HERE/patches/android/$2"
  if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
    echo "[jfg-android] $2: already applied"
  else
    git -C "$dir" apply --whitespace=nowarn "$patch"
    echo "[jfg-android] $2: applied to $1"
  fi
}

apply lib/rt64/src/contrib/plume plume-android.patch
apply lib/rt64 rt64-android.patch
apply lib/N64ModernRuntime n64modernruntime-android.patch
