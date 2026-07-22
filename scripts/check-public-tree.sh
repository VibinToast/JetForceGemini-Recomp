#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$project_dir"

git diff --check

tracked_files="$(git ls-files)"
forbidden_path='(^|/)(baserom[^/]*|[^/]+\.(z64|v64|n64|elf))$|^RecompiledFuncs/|^rsp/.*\.cpp$|(^|/)(DPLogo|krazoa|DinoFont)(\.|$)'
if grep -Eiq "$forbidden_path" <<<"$tracked_files"; then
    echo "Tracked private or generated input detected:" >&2
    grep -Ei "$forbidden_path" <<<"$tracked_files" >&2
    exit 1
fi

if git grep -nE '/Users/[^/]+/|/home/[^/]+/' -- \
    ':!docs/TESTING.md' ':!scripts/check-public-tree.sh'; then
    echo "Machine-specific absolute path detected in tracked source." >&2
    exit 1
fi

for required in \
    COPYING README.md BUILDING.md CONTRIBUTING.md CHANGELOG.md \
    dkr.us.v80.toml dkr.us.v80.syms.toml \
    scripts/generate-recomp.sh scripts/apply-submodule-patches.sh; do
    if [[ ! -s "$required" ]]; then
        echo "Required public-source file is missing or empty: $required" >&2
        exit 1
    fi
done

echo "Public source boundary checks passed."
