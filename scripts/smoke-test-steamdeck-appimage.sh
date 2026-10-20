#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "Usage: $0 /path/to/DKR.AppImage /path/to/private-dkr-rev1.z64" >&2
    exit 1
fi

appimage="$(cd "$(dirname "$1")" && pwd)/$(basename "$1")"

if [[ ! -x "$appimage" ]]; then
    echo "AppImage is missing or is not executable." >&2
    exit 1
fi

if [[ ! -f "$2" ]]; then
    echo "The private test ROM is missing." >&2
    exit 1
fi

rom="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"

for command in Xvfb xdotool; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Missing smoke-test dependency: $command" >&2
        exit 1
    fi
done

test_root="$(mktemp -d "${TMPDIR:-/tmp}/dkr-appimage-smoke.XXXXXX")"
test_home="$test_root/home"
config_dir="$test_root/config"
log_file="$config_dir/runtime.log"
wrapper_output_file="$test_root/apprun.log"
mkdir -p "$test_home" "$config_dir"
ln -s "$rom" "$config_dir/dkr-us-v80.z64"

display_number=""
for candidate in $(seq 90 120); do
    if [[ ! -e "/tmp/.X${candidate}-lock" ]]; then
        display_number="$candidate"
        break
    fi
done

if [[ -z "$display_number" ]]; then
    echo "Could not find a free virtual X display." >&2
    exit 1
fi

xvfb_pid=""
game_pid=""
cleanup() {
    if [[ -n "$game_pid" ]]; then
        kill "$game_pid" 2>/dev/null || true
        wait "$game_pid" 2>/dev/null || true
    fi
    if [[ -n "$xvfb_pid" ]]; then
        kill "$xvfb_pid" 2>/dev/null || true
        wait "$xvfb_pid" 2>/dev/null || true
    fi
    rm -rf "$test_root"
}
trap cleanup EXIT

display=":$display_number"
Xvfb "$display" -screen 0 1600x960x24 -nolisten tcp >"$test_root/xvfb.log" 2>&1 &
xvfb_pid="$!"

for attempt in $(seq 1 50); do
    if DISPLAY="$display" xdotool getmouselocation >/dev/null 2>&1; then
        break
    fi
    if [[ "$attempt" -eq 50 ]]; then
        echo "The virtual display did not become ready." >&2
        exit 1
    fi
    sleep 0.1
done

DISPLAY="$display" \
HOME="$test_home" \
APP_FOLDER_PATH="$config_dir" \
SteamDeck=1 \
APPIMAGE_EXTRACT_AND_RUN=1 \
SDL_AUDIODRIVER=dummy \
LIBGL_ALWAYS_SOFTWARE=true \
GALLIUM_DRIVER=llvmpipe \
"$appimage" >"$wrapper_output_file" 2>&1 &
game_pid="$!"

window_id=""
for attempt in $(seq 1 300); do
    window_id="$(DISPLAY="$display" xdotool search --onlyvisible --name \
        "Diddy Kong Racing" 2>/dev/null | head -n 1 || true)"
    if [[ -n "$window_id" ]]; then
        break
    fi
    if ! kill -0 "$game_pid" 2>/dev/null; then
        echo "The AppImage exited before its launcher appeared." >&2
        tail -n 80 "$log_file" >&2
        exit 1
    fi
    sleep 0.1
done

if [[ -z "$window_id" ]]; then
    echo "The AppImage launcher did not appear within 30 seconds." >&2
    tail -n 80 "$log_file" >&2
    exit 1
fi

for attempt in $(seq 1 300); do
    if grep -q "RT64 setup result: 0" "$log_file"; then
        break
    fi
    if ! kill -0 "$game_pid" 2>/dev/null; then
        echo "The AppImage exited while initializing its renderer." >&2
        tail -n 100 "$log_file" >&2
        exit 1
    fi
    if [[ "$attempt" -eq 300 ]]; then
        echo "The AppImage renderer did not initialize within 30 seconds." >&2
        tail -n 100 "$log_file" >&2
        exit 1
    fi
    sleep 0.1
done

# RmlUi can map the SDL window a few frames before the launcher document has
# completed its first layout and paint. Avoid clicking that transient frame.
sleep 1

# The test display is fixed at 1600x960. Start game is the launcher menu's
# first entry near the lower-right corner; pointer input avoids assumptions
# about keyboard focus or controller enumeration in a headless environment.
DISPLAY="$display" xdotool mousemove --window "$window_id" 1415 660 click 1

for attempt in $(seq 1 450); do
    if grep -q "RT64 common shader pipelines ready" "$log_file"; then
        break
    fi
    if ! kill -0 "$game_pid" 2>/dev/null; then
        echo "The AppImage exited before shader warmup completed." >&2
        tail -n 100 "$log_file" >&2
        exit 1
    fi
    sleep 0.1
done

setup_line="$(grep -n -m1 "RT64 setup result: 0" "$log_file" | cut -d: -f1 || true)"
start_line="$(grep -n -m1 "Finalizing RT64 common shader pipelines" "$log_file" | cut -d: -f1 || true)"
ready_line="$(grep -n -m1 "RT64 common shader pipelines ready" "$log_file" | cut -d: -f1 || true)"

if [[ -z "$setup_line" || -z "$start_line" || -z "$ready_line" ]]; then
    echo "The AppImage did not emit the complete shader warmup sequence." >&2
    tail -n 100 "$log_file" >&2
    exit 1
fi

if (( setup_line >= start_line || start_line >= ready_line )); then
    echo "The AppImage emitted shader warmup events out of order." >&2
    tail -n 100 "$log_file" >&2
    exit 1
fi

warmup_result="$(grep -m1 "RT64 common shader pipelines ready" "$log_file")"
echo "Steam Deck AppImage launcher and shader warmup smoke test passed."
echo "$warmup_result"
