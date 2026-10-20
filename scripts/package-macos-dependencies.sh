#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 1 ]]; then
    echo "Usage: $0 /path/to/Application.app" >&2
    exit 1
fi

bundle="$1"
project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
binary="$bundle/Contents/MacOS/Diddy Kong Racing Recompiled"
frameworks="$bundle/Contents/Frameworks"
source_assets="$project_dir/assets"
packaged_assets="$bundle/Contents/Resources/assets"

if [[ ! -x "$binary" ]]; then
    echo "Missing app executable: $binary" >&2
    exit 1
fi

# Refuse to sign a partial UI bundle. A missing recomp.rcss caused the Linux
# 0.1.4 build to abort before its launcher appeared; the macOS artifact should
# enforce the same complete-tree invariant as the AppImage packager.
source_asset_count="$(find "$source_assets" -type f | wc -l | tr -d ' ')"
packaged_asset_count="$(find "$packaged_assets" -type f | wc -l | tr -d ' ')"
if [[ "$source_asset_count" != "$packaged_asset_count" ]]; then
    echo "Refusing to sign an incomplete UI asset tree ($packaged_asset_count of $source_asset_count files)." >&2
    exit 1
fi

required_assets=(
    recomp.rcss
    rml.rcss
    launcher.rml
    config_menu.rml
    config_sub_menu.rml
    LatoLatin-Regular.ttf
    LatoLatin-Italic.ttf
    LatoLatin-Bold.ttf
    LatoLatin-BoldItalic.ttf
    NotoEmoji-Regular.ttf
    promptfont/promptfont.ttf
)
for required_asset in "${required_assets[@]}"; do
    if [[ ! -s "$packaged_assets/$required_asset" ]]; then
        echo "Refusing to sign without required UI asset: $required_asset" >&2
        exit 1
    fi
done

mkdir -p "$frameworks"
seen_paths="|"

copy_dependency() {
    local dependency="$1"
    local destination="$frameworks/$(basename "$dependency")"
    local child

    case "$dependency" in
        /opt/homebrew/*|/usr/local/*) ;;
        *) return ;;
    esac

    case "$seen_paths" in
        *"|$dependency|"*) return ;;
    esac
    seen_paths="${seen_paths}${dependency}|"

    cp -f "$dependency" "$destination"
    chmod u+w "$destination"
    install_name_tool -id "@rpath/$(basename "$dependency")" "$destination"

    while IFS= read -r child; do
        case "$child" in
            /opt/homebrew/*|/usr/local/*)
                copy_dependency "$child"
                install_name_tool -change "$child" "@loader_path/$(basename "$child")" "$destination"
                ;;
        esac
    done < <(otool -L "$destination" | awk 'NR > 1 { print $1 }')
}

while IFS= read -r dependency; do
    case "$dependency" in
        /opt/homebrew/*|/usr/local/*)
            copy_dependency "$dependency"
            install_name_tool -change "$dependency" \
                "@executable_path/../Frameworks/$(basename "$dependency")" "$binary"
            ;;
    esac
done < <(otool -L "$binary" | awk 'NR > 1 { print $1 }')

# A distributable app may contain fonts, UI assets, and libraries, but never a
# ROM or an accidentally copied baserom from local development.
rom_payload="$(find "$bundle" -type f \( \
    -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*baserom*' \
    \) -print -quit)"
if [[ -n "$rom_payload" ]]; then
    echo "Refusing to sign a bundle containing private ROM data: $rom_payload" >&2
    exit 1
fi

# Sign from a temporary local directory. File Provider can immediately restore
# FinderInfo on an .app inside Documents after xattr removes it, which makes
# codesign reject an otherwise valid bundle. Signing an attribute-free staging
# copy and copying its signed bytes back avoids that race while leaving the
# build output at the path CMake expects.
signing_dir="$(mktemp -d "${TMPDIR:-/tmp}/dkr-macos-sign.XXXXXX")"
trap 'rm -rf "$signing_dir"' EXIT
signing_bundle="$signing_dir/$(basename "$bundle")"
ditto --norsrc --noextattr --noacl "$bundle" "$signing_bundle"
xattr -cr "$signing_bundle"
codesign --force --deep --sign - "$signing_bundle"
codesign --verify --deep --strict "$signing_bundle"
ditto --norsrc --noextattr --noacl "$signing_bundle" "$bundle"

echo "Bundled non-system macOS dependencies and applied an ad-hoc signature in $frameworks"
