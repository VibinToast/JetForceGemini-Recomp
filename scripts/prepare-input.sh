#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "$0")/.." && pwd)"
workspace_dir="$(cd "$project_dir/.." && pwd)"
source_elf="${DKR_ELF:-$workspace_dir/diddy-kong-racing/build/dkr.us.v80.elf}"
output_elf="$project_dir/dkr.us.v80.recomp.elf"
objcopy="${MIPS_OBJCOPY:-}"

if [[ ! -f "$source_elf" ]]; then
    echo "Missing matching DKR ELF: $source_elf" >&2
    echo "Run the workspace ROM setup/build before preparing recomp input." >&2
    exit 1
fi

if [[ -z "$objcopy" ]]; then
    objcopy="$(command -v mips64-elf-objcopy || true)"
fi

if [[ -z "$objcopy" || ! -x "$objcopy" ]]; then
    echo "Missing MIPS objcopy. Set MIPS_OBJCOPY to a compatible executable." >&2
    exit 1
fi

# DKR's linker exposes the boot entrypoint as an absolute symbol even though
# its instructions live at the start of .main. N64Recomp requires functions to
# be associated with their executable section, so fix only that symbol in a
# private generated ELF. The matching decomp ELF remains untouched.
"$objcopy" \
    --strip-symbol=entrypoint \
    --add-symbol=entrypoint=.main:0,global,function \
    "$source_elf" "$output_elf"

echo "Prepared $output_elf"
