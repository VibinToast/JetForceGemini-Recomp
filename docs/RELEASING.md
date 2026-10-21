# Releasing

Releases start by themselves when something is merged into `main`. GitHub works out the version, writes the changelog
and makes a **draft** release. The builds are made on your own PC, because they need your ROM and the ROM never goes to
GitHub. Players don't see a draft, and the launcher's update check skips it, until every file is on it.

## Which number goes up

| Merged into `main` from | Version | Example |
|---|---|---|
| `release/<x.y>` | exactly x.y.0 | `release/1.2` → 1.2.0 |
| `dev` (only if merged straight into `main`) | minor | 1.1.8 → 1.2.0 |
| `hotfix/<name>` | patch | 1.1.8 → 1.1.9 |
| `rt64-update`, `milestone/<name>` | major | 1.1.8 → 2.0.0 |

To make an exception, put a label on the pull request: `release:major`, `release:minor`, `release:patch`, or
`release:none` (no release, for example a docs-only merge). Create these four labels once under Issues → Labels.

If you push to `main` without a pull request, there is only a release when the commit message contains `[major]`,
`[minor]` or `[patch]`, or when it's a local `Merge branch 'release/1.2'` (or `dev`, `hotfix/...`) commit. `[no release]` in a message skips the release.

You can also start a release by hand: Actions → Release → Run workflow on `main`, then choose a bump or type an exact
version.

## Branches

- `feature/<name>`: one piece of work, branched from `dev`, merged into `dev` when it's done and tested.
- `dev`: what the next release will be.
- `release/<x.y>`: cut from `dev` when a release goes into testing. Only bug fixes go on it, and each fix is merged back
  into `dev` right away (`git switch dev && git merge release/<x.y>`). `dev` stays free for new work meanwhile.
- `main`: only finished releases and hotfixes.
- `hotfix/<x.y.z>`: an urgent fix for what players have, branched from `main`. It reaches `dev` and any open
  `release/<x.y>` through the workflow's merge back (below), so the release in testing gets it too.

## Day to day

- **Normal release:** run `git switch -c release/<x.y> dev` when the release goes into testing. Fix bugs there, and
  write the release notes there too (see [Release notes](#release-notes)). When it passes, open a pull request from
  `release/<x.y>` into `main` and merge it, then delete the release branch.
- **Hotfix:** run `git switch -c hotfix/<x.y.z> main`, fix the problem, push, then open a pull request into `main` and
  merge it. Only the fix ships. Unfinished work on `dev` stays out.
- **RT64:** open a pull request from `rt64-update` into `main` and merge it. That makes 2.0.0.

After a release, the workflow merges `main` back into `dev`, `rt64-update` and every `release/*` branch. That brings over the release commit
(`CHANGELOG.md`, `version.txt`) and any hotfix, so the next merge has no conflicts. If that merge itself conflicts, the
workflow run shows a warning and you merge by hand (`git switch dev && git merge origin/main`). The list of branches is
`BACK_MERGE` in `.github/workflows/release.yml`.

Only one release can wait at a time. While a draft is waiting for its builds, a new merge into `main` gets no release.
Build and publish the draft first, then start the workflow by hand.

## Release notes

The notes are written by hand, in an `## Unreleased` section at the top of `CHANGELOG.md` (above the newest version).
Write them on the release branch while it's in testing, so they describe exactly what ships. When the release is made,
the workflow renames the section to `## vX.Y.Z (date)` and uses the same text for the GitHub release, with a "Full
changelog" link added at the end. The game and the launcher show `CHANGELOG.md` under Changelogs, and test builds of
the release branch already show the Unreleased section. Once the release branch with its notes is pushed, the launcher
of older versions shows them too, at the top of Changelogs as "Coming in x.y" (instead of the commits since the last
release).

```markdown
## Unreleased

### New options

- Auto Save: the game saves itself every few minutes, so a crash no longer loses your progress.

### Fixes

- Fixed a crash in the Lurg fight.
```

- Write for players: what changed in the game and what it means for them. No code, file or function names.
- Group the notes by area under `### ` headings (New options, Frame rate, Launcher, Fixes and so on).
- One `- ` line per change. Keep the list flat: the launcher shows nested bullets as ordinary ones, without
  indenting them. Bold, code marks and links don't show in the launcher either.
- Describe where things ended up, not each step on the way: if a setting was changed three times during testing,
  only its final form goes in. Say plainly when something players had is gone.
- Leave out changes players can't see (build, tools, refactoring, test switches).
- A `<!-- ... -->` comment is left out of the release, so it's safe for notes to yourself. Keep each one on one line:
  the in-game changelog of a test build shows it as a line of text.

**Fallback:** if there's no Unreleased section, or it has only the heading (and comments), the notes are the commit
messages since the last release instead: the first line of each, newest first. That's fine for a quick hotfix, so
keep commit messages readable for players anyway. This list leaves out "WIP" commits, commits listed in
`.github/changelog-skip.txt`, and commits that only change files players don't see (README, docs, `.github`, release
tools).

## Building the draft

When the workflow finishes, its summary says which draft is waiting. Then:

- **Windows:** double-click `tools\package_release.bat`.
- **Linux:** run `tools/package_release.sh`, in WSL or on a Linux PC.

Each one finds the draft by itself. It builds that release's exact commit in `dist/src-vX.Y.Z` (your own checkout,
uncommitted changes and `bin` folder are left alone), then uploads its files. The second one to finish **publishes**
the release. To publish with only one platform's files, use `package_release.bat -Publish` or
`tools/package_release.sh --publish`.

| File | For |
|---|---|
| `JFG-X.Y.Z-windows-x64-portable.zip` | Windows. The launcher's updater downloads this one. |
| `JFG-X.Y.Z-linux-x86_64.AppImage` | Linux, and the main file for SteamOS/Steam Deck: make it executable, then "Add a Non-Steam Game". Settings and saves go to `~/.local/share/JetForceGeminiRecompiled` (set `JFG_GAME_DIR` to change that). |
| `JFG-X.Y.Z-linux-x86_64-portable.tar.gz` | Linux, as a folder, like the Windows zip. Settings and saves stay in the folder. |

Each file has a `.sha256` next to it. The release isn't published until all six files are there (or you force it).

### One-time setup

- **Both:** install the [GitHub CLI](https://cli.github.com) and sign in with `gh auth login`. Keep `baserom.us.z64` next
  to the build scripts, or set `JFG_ROM` to wherever it is.
- **Windows:** the same tools as a normal `build_windows.bat` build. The release build reuses your build folders, so it
  only rebuilds what changed.
- **Linux:** Docker or Podman. The build runs in a container made from `tools/linux-build/Dockerfile` (Ubuntu 22.04
  with clang 18), which is set up automatically the first time. On Windows, use WSL2 with Docker Desktop's WSL
  integration, and clone the repository inside WSL (for example `~/JetForceGemini-Recomp`). Building from `/mnt/r/...`
  works but is very slow. The container keeps its own build folder, `build-container` (about 15 GB).

### Why Ubuntu 22.04

A Linux program built on a system runs on that system and on newer ones, but not on older ones. That's the glibc
version. Building on Ubuntu 22.04 (glibc 2.35) gives one file that runs on:

- **SteamOS 3** (Steam Deck): its system files are read-only and players can't install packages, so the AppImage is
  the one to use there.
- **Fedora** (current releases).
- **Ubuntu** 22.04 and later, and distros based on it (Mint, Pop!_OS).

SDL2 is packed in with the game. Vulkan, the GPU drivers, GTK and the C/C++ runtime always come from the player's
system. `tools/package_release.sh --native` builds without the container, but the result only runs on systems as new
as the one that built it.

The quickest check before announcing a release: start the AppImage once on the Steam Deck (Desktop Mode and Game Mode)
and once in a Fedora live session or VM.
