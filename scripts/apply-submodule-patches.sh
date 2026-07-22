#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"

apply_once() {
    local repo="$1"
    local patch="$2"

    if git -C "$repo" apply --recount --reverse --check "$patch" >/dev/null 2>&1; then
        echo "Already applied: $(basename "$patch")"
    else
        git -C "$repo" apply --recount --check "$patch"
        git -C "$repo" apply --recount "$patch"
        echo "Applied: $(basename "$patch")"
    fi
}

apply_once "$project_dir/lib/N64Recomp" "$project_dir/submodule-patches/n64recomp-dkr.patch"
apply_once "$project_dir/lib/N64ModernRuntime" "$project_dir/submodule-patches/n64modernruntime-dkr.patch"
apply_once "$project_dir/lib/rt64" "$project_dir/submodule-patches/rt64-f3ddkr.patch"
apply_once "$project_dir/lib/rt64/src/contrib/plume" "$project_dir/submodule-patches/plume-sdl2-compat.patch"
