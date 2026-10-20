#!/usr/bin/env bash
set -euo pipefail

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
builder_image="dkr-steamdeck-builder:latest"
build_dir="$project_dir/build-steamdeck"
dist_dir="$project_dir/dist-steamdeck"
app_dir="$dist_dir/AppDir"
appimage_name="DiddyKongRacingRecompiled-SteamDeck-x86_64.AppImage"
project_version="$(sed -nE 's/^project\(DKRRecomp VERSION ([0-9.]+).*/\1/p' "$project_dir/CMakeLists.txt")"
archive_name="DiddyKongRacingRecompiled-SteamDeck-${project_version}.tar.gz"
linuxdeploy="$build_dir/linuxdeploy-x86_64.AppImage"
linuxdeploy_dir="$build_dir/linuxdeploy-extracted"

if [[ -z "$project_version" ]]; then
    echo "Could not determine DKRRecomp version from CMakeLists.txt." >&2
    exit 1
fi

if [[ "${DKR_SKIP_COMPILE:-0}" != "1" ]]; then
    docker buildx build \
        --platform linux/amd64 \
        --load \
        --tag "$builder_image" \
        --file "$project_dir/scripts/steamdeck-builder.Dockerfile" \
        "$project_dir"

    docker run --rm --platform linux/amd64 \
        --volume "$project_dir:/src" \
        --workdir /src \
        "$builder_image" \
        cmake -S . -B build-steamdeck -G Ninja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_C_COMPILER=clang \
            -DCMAKE_CXX_COMPILER=clang++

    docker run --rm --platform linux/amd64 \
        --volume "$project_dir:/src" \
        --workdir /src \
        "$builder_image" \
        cmake --build build-steamdeck --parallel
elif [[ ! -x "$build_dir/DiddyKongRacingRecompiled" ]]; then
    echo "DKR_SKIP_COMPILE=1 requires an existing Linux build at $build_dir/DiddyKongRacingRecompiled." >&2
    exit 1
fi

mkdir -p "$build_dir" "$dist_dir"
if [[ ! -f "$linuxdeploy" ]]; then
    curl --fail --location \
        --output "$linuxdeploy" \
        https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage
fi

# The linuxdeploy AppImage bootstrap cannot run through every host's x86_64
# emulator, so extract its SquashFS payload and run the contained executable.
if [[ ! -x "$linuxdeploy_dir/AppRun" ]]; then
    squashfs_offset="$(LC_ALL=C grep -aob 'hsqs' "$linuxdeploy" | tail -n 1 | cut -d: -f1)"
    if [[ -z "$squashfs_offset" ]]; then
        echo "Could not locate linuxdeploy's SquashFS payload." >&2
        exit 1
    fi

    rm -rf "$linuxdeploy_dir"
    docker run --rm \
        --volume "$project_dir:/src" \
        --workdir /src \
        "$builder_image" \
        unsquashfs -o "$squashfs_offset" -d build-steamdeck/linuxdeploy-extracted \
            build-steamdeck/linuxdeploy-x86_64.AppImage
fi

rm -rf "$app_dir"
mkdir -p "$app_dir/usr/bin" "$app_dir/usr/share/metainfo"
cp "$build_dir/DiddyKongRacingRecompiled" "$app_dir/usr/bin/DiddyKongRacingRecompiled"
cp -R "$project_dir/assets" "$app_dir/usr/bin/assets"
cp "$project_dir/recompcontrollerdb.txt" "$app_dir/usr/bin/recompcontrollerdb.txt"
cp "$project_dir/deploy/io.github.cyscott.dkrrecomp.metainfo.xml" \
    "$app_dir/usr/share/metainfo/io.github.cyscott.dkrrecomp.metainfo.xml"

# A partial UI asset copy crashes older builds while RT64 is starting. Verify
# both the complete copy and the files required to construct the launcher.
source_asset_count="$(find "$project_dir/assets" -type f | wc -l | tr -d ' ')"
packaged_asset_count="$(find "$app_dir/usr/bin/assets" -type f | wc -l | tr -d ' ')"
if [[ "$source_asset_count" != "$packaged_asset_count" ]]; then
    echo "Refusing to package an incomplete UI asset tree ($packaged_asset_count of $source_asset_count files)." >&2
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
    if [[ ! -s "$app_dir/usr/bin/assets/$required_asset" ]]; then
        echo "Refusing to package without required UI asset: $required_asset" >&2
        exit 1
    fi
done

# The application must never carry a ROM or an accidentally copied baserom.
rom_payload="$(find "$app_dir" -type f \( \
    -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*baserom*' \
    \) -print -quit)"
if [[ -n "$rom_payload" ]]; then
    echo "Refusing to package private ROM data: $rom_payload" >&2
    exit 1
fi

rm -f "$dist_dir/$appimage_name"

docker run --rm --platform linux/amd64 \
    --volume "$project_dir:/src" \
    --workdir /src/dist-steamdeck \
    --env LDAI_OUTPUT="$appimage_name" \
    "$builder_image" \
    bash -lc 'PATH="/src/build-steamdeck/linuxdeploy-extracted/usr/bin:$PATH" \
        /src/build-steamdeck/linuxdeploy-extracted/AppRun \
        --appdir AppDir \
        --executable AppDir/usr/bin/DiddyKongRacingRecompiled \
        --exclude-library 'libSDL2-2.0.so.0' \
        --desktop-file ../deploy/io.github.cyscott.dkrrecomp.desktop \
        --icon-file ../icons/512.png \
        --icon-filename io.github.cyscott.dkrrecomp \
        --custom-apprun ../deploy/appimage/AppRun \
        --output appimage'

# linuxdeploy creates AppRun as a symlink directly to the executable. DKR's
# launcher loads its UI and controller database relative to the working
# directory, while Steam starts an AppImage from an arbitrary directory.
# Replace the symlink with a wrapper that enters usr/bin, then rebuild the
# AppImage payload around linuxdeploy's runtime.
rm -f "$app_dir/AppRun"
install -m 0755 "$project_dir/deploy/appimage/AppRun" "$app_dir/AppRun"

runtime_offset="$(LC_ALL=C grep -aob 'hsqs' "$dist_dir/$appimage_name" | tail -n 1 | cut -d: -f1)"
if [[ -z "$runtime_offset" ]]; then
    echo "Could not locate the generated AppImage's SquashFS payload." >&2
    exit 1
fi

docker run --rm --platform linux/amd64 \
    --volume "$project_dir:/src" \
    --workdir /src \
    --env APPIMAGE_RUNTIME_OFFSET="$runtime_offset" \
    --env APPIMAGE_NAME="$appimage_name" \
    "$builder_image" \
    bash -lc '
        set -euo pipefail
        squashfs="build-steamdeck/dkr-appdir.squashfs"
        repacked="build-steamdeck/${APPIMAGE_NAME}.repacked"
        rm -f "$squashfs" "$repacked"
        mksquashfs dist-steamdeck/AppDir "$squashfs" -root-owned -noappend -comp zstd >/tmp/dkr-mksquashfs.log
        cp "dist-steamdeck/$APPIMAGE_NAME" "$repacked"
        truncate -s "$APPIMAGE_RUNTIME_OFFSET" "$repacked"
        dd if="$squashfs" of="$repacked" bs=4M oflag=seek_bytes \
            seek="$APPIMAGE_RUNTIME_OFFSET" conv=notrunc status=none
        chmod 0755 "$repacked"
        mv "$repacked" "dist-steamdeck/$APPIMAGE_NAME"

        rm -rf build-steamdeck/appimage-verify
        unsquashfs -o "$APPIMAGE_RUNTIME_OFFSET" \
            -d build-steamdeck/appimage-verify \
            "dist-steamdeck/$APPIMAGE_NAME" >/tmp/dkr-unsquashfs.log
        test -x build-steamdeck/appimage-verify/AppRun
        test "$(head -c 9 build-steamdeck/appimage-verify/AppRun)" = "#!/bin/sh"
        test -x build-steamdeck/appimage-verify/usr/bin/DiddyKongRacingRecompiled
        test -s build-steamdeck/appimage-verify/usr/bin/assets/launcher.rml
        test -s build-steamdeck/appimage-verify/usr/bin/recompcontrollerdb.txt
    '

chmod +x "$dist_dir/$appimage_name"

# Validate the exact SquashFS payload that will be delivered, rather than
# assuming linuxdeploy copied every staged file into the AppImage. This is the
# failure mode that made 0.1.4 abort before its launcher appeared.
appimage_verify_root="$(mktemp -d "$dist_dir/AppImage-verify.XXXXXX")"
appimage_verify_dir="$appimage_verify_root/payload"
delivery_dir="$(mktemp -d "${TMPDIR:-/tmp}/dkr-steamdeck-delivery.XXXXXX")"
cleanup() {
    rm -rf "$appimage_verify_root" "$delivery_dir"
}
trap cleanup EXIT

appimage_squashfs_offset="$(LC_ALL=C grep -aob 'hsqs' "$dist_dir/$appimage_name" | tail -n 1 | cut -d: -f1)"
if [[ -z "$appimage_squashfs_offset" ]]; then
    echo "Could not locate the finished AppImage's SquashFS payload." >&2
    exit 1
fi

appimage_verify_rel="${appimage_verify_dir#"$project_dir"/}"
docker run --rm --platform linux/amd64 \
    --volume "$project_dir:/src" \
    --workdir /src \
    "$builder_image" \
    unsquashfs -o "$appimage_squashfs_offset" -d "$appimage_verify_rel" \
        "dist-steamdeck/$appimage_name" >/dev/null

finished_assets="$appimage_verify_dir/usr/bin/assets"
finished_asset_count="$(find "$finished_assets" -type f | wc -l | tr -d ' ')"
if [[ "$source_asset_count" != "$finished_asset_count" ]]; then
    echo "Finished AppImage has an incomplete UI asset tree ($finished_asset_count of $source_asset_count files)." >&2
    exit 1
fi

if [[ -L "$appimage_verify_dir/AppRun" || ! -x "$appimage_verify_dir/AppRun" ]] ||
   ! grep -q 'runtime.log' "$appimage_verify_dir/AppRun"; then
    echo "Finished AppImage is missing the persistent diagnostic AppRun wrapper." >&2
    exit 1
fi

for required_asset in "${required_assets[@]}"; do
    if [[ ! -s "$finished_assets/$required_asset" ]]; then
        echo "Finished AppImage is missing required UI asset: $required_asset" >&2
        exit 1
    fi
done

finished_rom_payload="$(find "$appimage_verify_dir" -type f \( \
    -iname '*.z64' -o -iname '*.n64' -o -iname '*.v64' -o -iname '*baserom*' \
    \) -print -quit)"
if [[ -n "$finished_rom_payload" ]]; then
    echo "Finished AppImage contains private ROM data: $finished_rom_payload" >&2
    exit 1
fi

# Browser and Drive downloads commonly strip the executable bit from a raw
# AppImage. Ship a tarball whose internal filename stays stable so an existing
# Steam shortcut keeps working after Extract Here / Replace.
install -m 0755 "$dist_dir/$appimage_name" "$delivery_dir/$appimage_name"
rm -f "$dist_dir/$archive_name"
tar -C "$delivery_dir" -czf "$dist_dir/$archive_name" "$appimage_name"

if ! tar -tvf "$dist_dir/$archive_name" | grep -q -- '^-rwxr-xr-x'; then
    echo "The delivery archive did not preserve the AppImage executable bit." >&2
    exit 1
fi

echo "Created $dist_dir/$appimage_name"
echo "Created permission-preserving delivery archive $dist_dir/$archive_name"
if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$dist_dir/$archive_name"
else
    shasum -a 256 "$dist_dir/$archive_name"
fi
