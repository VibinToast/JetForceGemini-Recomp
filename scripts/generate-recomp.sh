#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"

n64recomp="${N64RECOMP:-$project_dir/../n64recomp/build/N64Recomp}"
rsprecomp="${RSPRECOMP:-$project_dir/lib/N64Recomp/build/RSPRecomp}"

if [[ ! -x "$n64recomp" ]]; then
    echo "Missing N64Recomp. Set N64RECOMP to a built N64Recomp executable." >&2
    exit 1
fi

if [[ ! -x "$rsprecomp" ]]; then
    echo "Missing RSPRecomp. Set RSPRECOMP to a built RSPRecomp executable." >&2
    exit 1
fi

if [[ ! -f "$project_dir/baserom.us.v80.z64" ]]; then
    echo "Missing private input ROM: $project_dir/baserom.us.v80.z64" >&2
    echo "Use a symlink or private local copy; never commit the ROM." >&2
    exit 1
fi

cd "$project_dir"
"$n64recomp" dkr.us.v80.toml
"$rsprecomp" rsp-aspMain.toml
"$rsprecomp" rsp-f3ddkr-dram.toml
"$rsprecomp" rsp-f3ddkr-fifo.toml
"$rsprecomp" rsp-f3ddkr-xbus.toml

echo "Generated DKR CPU and RSP sources."
