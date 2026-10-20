#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 2 || $# -gt 3 ]]; then
    echo "Usage: $0 {player-select|overworld|first-track} /path/to/private-dkr-rev1.z64 [output-directory]" >&2
    exit 1
fi

scene="$1"
case "$scene" in
    player-select|overworld|first-track) ;;
    *)
        echo "Unknown scene '$scene' (expected player-select, overworld, or first-track)." >&2
        exit 1
        ;;
esac

if [[ ! -f "$2" ]]; then
    echo "The private test ROM is missing." >&2
    exit 1
fi

project_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
rom="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
app_dir="$project_dir/dist-steamdeck/AppDir"
builder_image="dkr-steamdeck-builder:latest"
output_dir="${3:-$(mktemp -d "${TMPDIR:-/tmp}/dkr-linux-vulkan-${scene}.XXXXXX")}"

if [[ ! -x "$app_dir/usr/bin/DiddyKongRacingRecompiled" ]]; then
    echo "No staged Linux build was found. Run scripts/build-steamdeck-appimage.sh first." >&2
    exit 1
fi

mkdir -p "$output_dir/config" "$output_dir/home"
ln -sfn /test/private-dkr-rev1.z64 "$output_dir/config/dkr-us-v80.z64"

docker buildx build \
    --platform linux/amd64 \
    --load \
    --tag "$builder_image" \
    --file "$project_dir/scripts/steamdeck-builder.Dockerfile" \
    "$project_dir" >/dev/null

docker run --rm --platform linux/amd64 \
    --volume "$project_dir:/src:ro" \
    --volume "$rom:/test/private-dkr-rev1.z64:ro" \
    --volume "$output_dir:/test/output" \
    --env TEST_SCENE="$scene" \
    --env RT64_DKR_FB_OVERLAP_LOG="${RT64_DKR_FB_OVERLAP_LOG:-}" \
    --env RT64_DKR_DL_TRACE="${RT64_DKR_DL_TRACE:-}" \
    --env DKR_SCENE_TRACE="${DKR_SCENE_TRACE:-}" \
    --env DKR_PLAYER_SELECT_SOAK_SECONDS="${DKR_PLAYER_SELECT_SOAK_SECONDS:-120}" \
    --env DKR_ANIM_COMPARE="${DKR_ANIM_COMPARE:-}" \
    --env DKR_ANIM_NATIVE="${DKR_ANIM_NATIVE:-}" \
    --env DKR_ANIM_INCREMENTAL="${DKR_ANIM_INCREMENTAL:-}" \
    --env DKR_ANIM_PERF="${DKR_ANIM_PERF:-}" \
    --env DKR_MUSIC_ANIM_TRACE="${DKR_MUSIC_ANIM_TRACE:-}" \
    --workdir /src/dist-steamdeck/AppDir/usr/bin \
    "$builder_image" \
    bash -lc '
        set -euo pipefail
        export DISPLAY=:99
        export HOME=/test/output/home
        export APP_FOLDER_PATH=/test/output/config
        export SteamDeck=1
        export SDL_AUDIODRIVER=dummy
        export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json
        export DKR_GFX_PROFILE=1
        export RT64_DKR_PERF_LOG=1

        Xvfb "$DISPLAY" -screen 0 1280x800x24 -nolisten tcp >/test/output/xvfb.log 2>&1 &
        xvfb_pid=$!
        game_pid=""
        cleanup() {
            if [[ -n "$game_pid" ]]; then
                kill -TERM "$game_pid" 2>/dev/null || true
                for attempt in $(seq 1 50); do
                    if ! kill -0 "$game_pid" 2>/dev/null; then break; fi
                    sleep 0.1
                done
                kill -KILL "$game_pid" 2>/dev/null || true
                wait "$game_pid" 2>/dev/null || true
            fi
            kill "$xvfb_pid" 2>/dev/null || true
            wait "$xvfb_pid" 2>/dev/null || true
        }
        trap cleanup EXIT

        for attempt in $(seq 1 100); do
            if xdotool getmouselocation >/dev/null 2>&1; then break; fi
            sleep 0.1
        done

        ./DiddyKongRacingRecompiled --skip-launcher --window-width 1280 --window-height 800 \
            >/test/output/runtime.log 2>&1 &
        game_pid=$!

        window_id=""
        for attempt in $(seq 1 900); do
            window_id="$(xdotool search --onlyvisible --name "Diddy Kong Racing" 2>/dev/null | head -n 1 || true)"
            if [[ -n "$window_id" ]] && grep -q "RT64 setup result: 0" /test/output/runtime.log; then break; fi
            if ! kill -0 "$game_pid" 2>/dev/null; then
                echo "Linux/Vulkan build exited during startup." >&2
                tail -n 100 /test/output/runtime.log >&2
                exit 1
            fi
            sleep 0.1
        done

        if [[ -z "$window_id" ]]; then
            echo "Linux/Vulkan window did not appear." >&2
            exit 1
        fi

        press_return() {
            # QEMU plus software Vulkan can run below five frames per second.
            # Hold the key long enough that SDL observes it even at that rate.
            xdotool keydown --window "$window_id" Return
            sleep 0.5
            xdotool keyup --window "$window_id" Return
            sleep "${1:-2}"
        }

        # The first recompiled display lists trigger a one-time translation
        # burst under x86-64 emulation. Do not send menu input while that work
        # has the game thread paused or the key presses will be consumed early.
        sleep 45
        press_return 2
        press_return 2
        press_return 2
        press_return 2

        # Both paths are now at the controller-pak caution screen.
        press_return 3
        if [[ "$TEST_SCENE" == "player-select" ]]; then
            # Under QEMU/software Vulkan the logo and title timers advance far
            # more slowly than wall time. Drive the menus until the runtime
            # scene trace reports MENU_CHARACTER_SELECT (3), then stop input
            # immediately so this test cannot accidentally skip the target.
            for attempt in $(seq 1 40); do
                if grep -q "\[DKR SCENE\] gameMode=1 menuId=3" /test/output/runtime.log; then
                    break
                fi
                press_return 3
            done
            if ! grep -q "\[DKR SCENE\] gameMode=1 menuId=3" /test/output/runtime.log; then
                echo "Linux/Vulkan run did not reach Player Select." >&2
                tail -n 100 /test/output/runtime.log >&2
                exit 1
            fi
            # Leave the animated character roster active long enough to expose
            # lifetime and framebuffer corruption that only appears after the
            # menu has cycled through several poses.
            sleep "$DKR_PLAYER_SELECT_SOAK_SECONDS"
        elif [[ "$TEST_SCENE" == "overworld" ]]; then
            press_return 3
            press_return 10
        else
            xdotool keydown --window "$window_id" s
            sleep 0.5
            xdotool keyup --window "$window_id" s
            sleep 1
            press_return 4
            press_return 4
            press_return 4
            press_return 2
            press_return 12
        fi

        if [[ "$TEST_SCENE" != "player-select" ]]; then
            sleep 12
        fi
        if ! kill -0 "$game_pid" 2>/dev/null; then
            echo "Linux/Vulkan build exited before the scene profile completed." >&2
            tail -n 100 /test/output/runtime.log >&2
            exit 1
        fi
    '

if ! grep -q '\[RT64 PERF\]' "$output_dir/runtime.log"; then
    echo "The Linux/Vulkan run completed without renderer timing output." >&2
    exit 1
fi

echo "Linux/Vulkan $scene profile completed: $output_dir/runtime.log"
tail -n 20 "$output_dir/runtime.log"
