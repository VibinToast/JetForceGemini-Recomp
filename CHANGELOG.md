# Changelog

Changes to Jet Force Gemini: Recompiled. Newest first. Entries under "Unreleased" are in the source but not yet
built and tested, unless marked otherwise.

## v1.1.4 (2026-10-16)

- Centre the launcher logo between the top of the panel and the Play button
- Show the Jet Force Gemini Recomp logo in the launcher
- Shorten the Modern and Balanced profile description
- Built-in profiles run at 60 fps, with shorter descriptions
- Update .gitignore
- Fix 60 fps running at 30-45 and looking like 30
- Add vignette, level fog distance and texture / picture filtering options
- Settings menu: keep the tabs clear of the quit and close buttons
- Add a Field of View slider for gameplay (Display / Visuals)
- Update .recomp.json
- Update README.md
- Updated readme, included banner.
- Create .recomp.json

## v1.1.3 (2026-10-15)

- Dual-stick camera: keep the camera out of walls with the game's own wall check

## v1.1.2 (2026-10-14)

- Keep the widescreen sides empty while paused instead of switching the aspect
- Keep widescreen on while paused again (testing the pause screen backdrop)
- Only restore the bloom snapshot at the first scene drawn into a colour buffer each frame
- Only black out the widescreen sides for scenes that fill the screen height
- No widescreen while paused; clear the widescreen sides at the start of each 3D scene
- Drop a colour buffer's bloom snapshot when something else draws into it
- Keep a bloom snapshot per colour buffer
- Fix bloom piling up on parts of the screen that are not redrawn

## v1.1.1 (2026-10-14)

- Updated release.yml
- Updated version number
- package_release asks for the version, writes version.txt and builds the game first
- Centre the version number with the GitHub link in a three-column row; say when there are no new commits
- Updated readme
- Show commits since the last release in Changelogs, add a GitHub link by the version
- Move the changelog button to the bottom of the Play page and rename it Changelogs
- Fix the Graphics page's info text floating over the options in the launcher
- Add a changelog viewer to the launcher's About page
- Release the sticks when a menu opens, update texture pack link, add Bluesky
- Update the game from the launcher
- Ignore the mouse in the game while the Select/Esc menu is open
- Show a warning triangle in the launcher when a newer release exists
- Aim the controller directly in boss fights, with the camera following
- Turn the camera with the aim in boss fights so bosses at the sides stay in view
- Let the mouse aim 120 degrees either side in boss fights
- Keep the player on the rail in boss fights with the mouse
- Keep the mouse turn in boss fights within a cone so the player only strafes
- Fix the shaking reticle with mouse and keyboard in boss fights
- Update .gitignore
- Fix JFG 4K texture pack link in README

## v1.1.0 (2026-10-14)

- Updated release.yml
- Centre the version number with the GitHub link in a three-column row; say when there are no new commits
- Updated readme
- Show commits since the last release in Changelogs, add a GitHub link by the version
- Move the changelog button to the bottom of the Play page and rename it Changelogs
- Fix the Graphics page's info text floating over the options in the launcher
- Add a changelog viewer to the launcher's About page
- Release the sticks when a menu opens, update texture pack link, add Bluesky
- Update the game from the launcher
- Ignore the mouse in the game while the Select/Esc menu is open
- Show a warning triangle in the launcher when a newer release exists
- Aim the controller directly in boss fights, with the camera following
- Turn the camera with the aim in boss fights so bosses at the sides stay in view
- Let the mouse aim 120 degrees either side in boss fights
- Keep the player on the rail in boss fights with the mouse
- Keep the mouse turn in boss fights within a cone so the player only strafes
- Fix the shaking reticle with mouse and keyboard in boss fights
- Update .gitignore
- Fix JFG 4K texture pack link in README

## v1.0.0 (2026-10-13)

- Choose the release version by hand and add the portable zip packager
- Stop reading saves and settings from %LOCALAPPDATA%
- Ignore every bin* folder
- Create bin/mods and bin/texture_packs after the build
- Make releases manual
- Add a guide to making texture packs
- Add drag-and-drop tools to convert a pack to DDS and to pack it as .rtz
- Load .rtz texture packs from texture_packs
- Show the Mods tab again
- Update JFG 4K texture pack link in README
- Document HD texture packs in the README
- Add a launcher in Jet Force Gemini colours
- Document the features and keys in the README
- Add automatic releases
- Add the Gameplay, Visuals, Profiles and Textures tabs to the settings menu
- Add the no-blood option, the Tribal check cheat and the fly camera freeze
- Add boss fight controls and the jetpack refuel option
- Add the weapons editor hooks
- Add the dual-stick camera and movement speed options
- Add the RT64 changes for texture dumping and packs, and the texture helper scripts
- Add the 60 fps option
- Let START skip cutscenes and the intro
- Add the native debug menu and settings
- Put the version number into the build
- Follow the game's overlay loader in the runtime
- Report genuine hardware to the anti-tamper checks
- Add the patch for the base project (config, input, renderer, runtime)
- Add the Linux build and run scripts
- Add the Windows build and run scripts
- Prepare the overlays for recompilation
- Add the merge step for the gen_anim_data entry points
- Add the N64Recomp config and the symbols exported from the decomp
- Add license, git settings and a first README

## v1.0 (2026-10-13)

### Mods
- The Mods tab in the settings menu (Select / Esc) is shown again. It was hidden by the port; only the Debug tab
  stays hidden. (`ui_config.cpp`, via `patches/jfg-base.patch`)
- A `.rtz` texture pack placed in `bin/mods/` is loaded as a mod and turned on by default (this comes from the base
  project's mod support: `src/runtime/mods.cpp` registers `.rtz` with `rt64.json` as its content file).
  Confirmed working by hand-testing with `4x_LSDIRDAT.rtz`.
- Not restored: the "Mods" button on the launcher screen (removed by the patch).

### Texture packs
- Packs can now be packed `.rtz` files in `bin/texture_packs/` as well as folders. They are listed in the Textures
  tab, the F10 menu and F6, shown without the `.rtz`. (`src/jfg/jfg_debug_menu.cpp`: `list_texture_packs`,
  `pack_display_name`.)
- The renderer loads an `.rtz` from `texture_packs/` straight from the file and counts its textures with miniz;
  `rt64.json` is not rewritten for it. Folder packs work as before. (`jfg-base.patch`, renderer `check_texture_pack_actions`.)
- Not compiled or tested yet. A pack should be in `texture_packs/` or in `mods/`, not both.
- How to make a pack: zip the contents of the pack folder (`rt64.json` at the archive root, textures beside it, stored or
  deflate) and name the file `<name>.rtz`.

### Tools
- `tools/textures/png_to_dds.bat`: drag a texture pack folder onto it to get a `<folder>_dds` folder with every PNG
  converted to BC7 DDS (full mipmaps) by `texconv.exe`, and an `rt64.json` pointing at the `.dds` files. Needs
  `texconv.exe` (Microsoft DirectXTex) next to the script or on PATH. Settings (format, raw-value handling) are at the top
  of `png_to_dds.ps1`.
- `tools/textures/make_rtz.bat`: drag a pack folder onto it to get `<folder>.rtz` next to it (a zip with `rt64.json` at
  its root, stored by default). Put the result in `bin/texture_packs/` or `bin/mods/`.
- `docs/TEXTURE_PACKS.md`: a guide to making a texture pack (dump, replace, test, convert to DDS, pack as `.rtz`),
  linked from the README.
- Both are .bat wrappers around a .ps1 of the same name. Not run yet on Windows.

### Saves and settings
- The game no longer looks in `%LOCALAPPDATA%` (or `~/.config` on Linux) for saves and settings. Saves and settings are
  only read from, and written to, the `bin` folder next to the executable. The one-time copy from an earlier build's
  per-user folder is removed. (`src/config/config.cpp`, via `patches/jfg-base.patch`.) Anyone who still has a save in
  `%LOCALAPPDATA%\JetForceGeminiRecompiled` must copy it into `bin\saves` by hand.

### Build
- `build_windows.bat` and `build_linux.sh` now create empty `bin/mods` and `bin/texture_packs` folders after the
  build. Existing folders and their contents are left alone.

### Releases
- `.github/workflows/release.yml`: releases are made by hand. In GitHub, Actions > Release > Run workflow, then type the
  version (three numbers, for example `1.0.0`). The workflow turns this "Unreleased" section into the release notes,
  renames it to the new version, writes `version.txt`, tags `vX.Y.Z` and publishes the release.
- `tools/package_release.bat`: makes `dist/JFG-X.Y.Z-windows-x64-portable.zip` from `bin` (the exe, DLLs, assets and
  empty `mods` and `texture_packs` folders; no saves, settings or ROM) and uploads it to the release with `gh` if it is
  installed. The game cannot be built on GitHub because it needs your ROM, so this step runs on your PC after the
  workflow.

### Notes
- Changes were made in three places that must stay in step: `patches/jfg-base.patch` (what the build applies),
  `build/src/...` (the current build tree) and `src/jfg/` (the port's own code).

