#!/usr/bin/env bash
# Builds the Linux release and uploads it to its draft on GitHub. It never publishes: only the owner does, on GitHub.
# The Linux side of tools\package_release.bat. Needs the GitHub CLI
# (gh), signed in (gh auth login), for anything that talks to GitHub, and Docker or Podman for the build (see below).
# See RELEASING.md in the owner's Recomp-Docs notes.
#
#   tools/package_release.sh                   finds the draft release waiting for a Linux build (made by the Release
#                                              workflow when something is merged into main), builds that release's exact
#                                              commit in dist/src-vX.Y.Z (your own checkout and bin folder are not
#                                              touched), uploads the AppImage, the portable .tar.gz and their .sha256
#                                              files to the draft
#   tools/package_release.sh 1.0.1             builds this checkout as 1.0.1 (writes version.txt) and uploads to v1.0.1
#                                              if that release is a draft (a published release is never changed)
#   tools/package_release.sh --skip-build 1.0.1   packs what is already in bin (no new build)
#   --native                                   builds on this system instead of in the container
#
# The build normally runs in the tools/linux-build container (Ubuntu 22.04), so the game runs on SteamOS, Fedora and
# Ubuntu 22.04 or newer whatever this system is. A --native build only runs on systems as new as this one.
# The ROM is baserom.us.z64 next to build_linux.sh, or JFG_ROM. It is only read; it goes into no file that is uploaded.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ROM="$(realpath "${JFG_ROM:-$ROOT/baserom.us.z64}")"
OUT="$ROOT/dist"
NATIVE=0 SKIP_BUILD=0 VERSION=""
for a in "$@"; do
  case "$a" in
    --native) NATIVE=1 ;;
    --skip-build) SKIP_BUILD=1 ;;
    -h|--help) sed -n '2,19p' "$0"; exit 0 ;;
    -*) echo "unknown option $a (see --help)" >&2; exit 1 ;;
    *) VERSION="${a#v}" ;;
  esac
done

say() { echo "[jfg] $*"; }
die() { echo "[jfg] ERROR: $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null; }

# Every file a release needs before the owner publishes it. Keep in step with tools/package_release.ps1.
release_files() {
  local v="$1"
  printf '%s\n' "JFG-$v-windows-x64-portable.zip" "JFG-$v-windows-x64-portable.zip.sha256" \
    "JFG-$v-linux-x86_64.AppImage" "JFG-$v-linux-x86_64.AppImage.sha256" \
    "JFG-$v-linux-x86_64-portable.tar.gz" "JFG-$v-linux-x86_64-portable.tar.gz.sha256"
}
assets_of() { gh release view "$1" --json assets --jq '.assets[].name'; }

cd "$ROOT"
SRC="$ROOT" TAG="" WORKTREE=""
if [ -z "$VERSION" ]; then
  # ---- find the draft --------------------------------------------------------------------------------------------
  have gh || die "gh (GitHub CLI) is not installed: https://cli.github.com, then run gh auth login."
  gh auth status >/dev/null 2>&1 || die "gh is not signed in. Run gh auth login first."
  mapfile -t drafts < <(gh release list --limit 50 --json tagName,isDraft --jq '.[] | select(.isDraft) | .tagName')
  for t in "${drafts[@]}"; do
    if ! assets_of "$t" | grep -qxF "JFG-${t#v}-linux-x86_64.AppImage"; then TAG="$t"; break; fi
  done
  if [ -z "$TAG" ]; then
    if [ "${#drafts[@]}" -gt 0 ]; then say "the draft release ${drafts[0]} already has its Linux build"
    else say "no draft release is waiting for a build. Merge into main (or run the Release workflow) to make one."; fi
    exit 0
  fi
  [ "$SKIP_BUILD" = 0 ] || die "--skip-build only works with a version (tools/package_release.sh --skip-build 1.0.1)"
  VERSION="${TAG#v}"
  COMMIT="$(gh release view "$TAG" --json targetCommitish --jq .targetCommitish)"
  say "draft release $TAG is waiting for a Linux build (commit $COMMIT)"

  # ---- that release's exact commit, in its own folder ------------------------------------------------------------
  git fetch -q origin
  git cat-file -e "$COMMIT^{commit}" 2>/dev/null || die "commit $COMMIT of $TAG is not on GitHub's main branch. Was it pushed?"
  WORKTREE="$OUT/src-$TAG"
  [ ! -e "$WORKTREE" ] || git worktree remove --force "$WORKTREE" 2>/dev/null || rm -rf "$WORKTREE"
  git worktree prune
  mkdir -p "$OUT"
  git worktree add --detach "$WORKTREE" "$COMMIT"
  SRC="$WORKTREE"
  in_tree="$(tr -d '[:space:]' < "$SRC/version.txt")"
  [ "$in_tree" = "$VERSION" ] || die "commit $COMMIT has version.txt $in_tree, not $VERSION"
else
  # ---- this checkout, as the version given -----------------------------------------------------------------------
  [[ "$VERSION" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]] || die "the version must be three numbers like 1.0.0, not '$VERSION'"
  TAG="v$VERSION"
  current="$(tr -d '[:space:]' < "$ROOT/version.txt" 2>/dev/null || true)"
  if [ "$current" != "$VERSION" ]; then echo "$VERSION" > "$ROOT/version.txt"; say "version.txt: $current -> $VERSION"; fi
fi
mkdir -p "$OUT"

# ---- build and pack ----------------------------------------------------------------------------------------------
if [ "$NATIVE" = 1 ]; then
  if [ "$SKIP_BUILD" = 0 ]; then
    [ -f "$ROM" ] || die "ROM not found: $ROM (or set JFG_ROM)"
    bash "$SRC/build_linux.sh" "$ROM"
  fi
  bash "$SRC/tools/linux-build/package.sh" "$SRC" "$VERSION" "$OUT"
else
  if [ -n "${JFG_CONTAINER:-}" ]; then ENGINE="$JFG_CONTAINER"
  elif have podman; then ENGINE=podman
  elif have docker; then ENGINE=docker
  else die "neither Docker nor Podman is installed. Install one, or build with --native (only runs on systems as new as this one)."; fi
  DOCKERFILE="$SRC/tools/linux-build/Dockerfile"
  IMAGE="jfg-linux-build:$(sha256sum "$DOCKERFILE" | cut -c1-12)"
  if ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    say "making the build container $IMAGE (once; a few minutes)"
    "$ENGINE" build -t "$IMAGE" -f "$DOCKERFILE" "$SRC/tools/linux-build"
  fi
  # Its own CMake folders: their files hold the container's paths, so they cannot be shared with a native build.
  BUILD="${JFG_CONTAINER_BUILD_DIR:-$ROOT/build-container}"
  mkdir -p "$BUILD"
  user=(--user "$(id -u):$(id -g)")
  [ "$ENGINE" = podman ] && user+=(--userns=keep-id)
  steps="bash /src/tools/linux-build/package.sh /src $VERSION /out"
  if [ "$SKIP_BUILD" = 0 ]; then
    [ -f "$ROM" ] || die "ROM not found: $ROM (or set JFG_ROM)"
    # The source folder is owned by your user, not the container's: let git use it (and its submodules).
    steps="git config --global --add safe.directory '*' && bash /src/build_linux.sh /rom/baserom.us.z64 && $steps"
  fi
  rom_mount=()
  [ -f "$ROM" ] && rom_mount=(-v "$ROM:/rom/baserom.us.z64:ro,z")
  "$ENGINE" run --rm "${user[@]}" -e HOME=/tmp -e JFG_BUILD_DIR=/work \
    -v "$SRC:/src:z" -v "$BUILD:/work:z" -v "$OUT:/out:z" "${rom_mount[@]}" \
    -w /src "$IMAGE" bash -c "$steps"
fi

APP="JFG-$VERSION-linux-x86_64.AppImage"
TGZ="JFG-$VERSION-linux-x86_64-portable.tar.gz"
UPLOAD=("$OUT/$APP" "$OUT/$APP.sha256" "$OUT/$TGZ" "$OUT/$TGZ.sha256")
for f in "${UPLOAD[@]}"; do [ -f "$f" ] || die "missing $f"; done

# ---- upload to the draft; its draft state is never changed here --------------------------------------------------
if ! have gh; then say "gh (GitHub CLI) is not installed. Add $APP, $TGZ and their .sha256 files to $TAG on GitHub by hand."; exit 0; fi
if ! gh release view "$TAG" >/dev/null 2>&1; then
  say "no GitHub release $TAG. Merge into main (or run the Release workflow) to make one, then run this again."
  exit 0
fi
# Only a draft gets files: a published release is never changed, so a test build cannot replace what players download.
if [ "$(gh release view "$TAG" --json isDraft --jq .isDraft)" != true ]; then
  say "$TAG is already published, so nothing is uploaded. The files are in $OUT."
  exit 0
fi
gh release upload "$TAG" "${UPLOAD[@]}" --clobber
say "uploaded to the GitHub release $TAG"
missing="$(release_files "$VERSION" | grep -vxF -f <(assets_of "$TAG") || true)"
if [ -n "$missing" ]; then
  say "$TAG is a draft and still needs these (tools\package_release.bat on Windows):"
  sed 's/^/  /' <<< "$missing"
else
  say "$TAG has every file and stays a draft: only the owner publishes it."
fi
[ -z "$WORKTREE" ] || git worktree remove --force "$WORKTREE" 2>/dev/null || true
