# Changelog

Changes to Jet Force Gemini: Recompiled. Newest first. Entries under "Unreleased" are in the source but not yet
built and tested, unless marked otherwise.

## Unreleased

### Launcher
- About > Changelog shows the changes of this version and every earlier one, from the CHANGELOG.md that now ships in
  `assets/jfg_launcher` (so it also updates with the game and works offline). (`build_windows.bat`, `jfg_update_check.cpp`,
  launcher via `patches/jfg-base.patch`.)
- The texture pack link points to vibintoast.com/projects/jfg-4k, and the About tab links to Bluesky.
- Update from the launcher: a warning triangle next to the version number appears when GitHub has a newer release.
  Clicking it shows what changed since this build and asks to update (or to open the release page). The zip is
  downloaded from the release, checked against the SHA-256 GitHub lists for it, unpacked, swapped in (the running exe
  and DLLs are renamed to `.old`, with a rollback if anything fails) and the game restarts. Saves, settings, texture
  packs and mods are not touched. Releases need the portable zip attached (`tools/package_release.bat`).
  (`src/jfg/jfg_update_check.cpp`, `.h`, launcher via `patches/jfg-base.patch`.)
- Launcher buttons no longer flicker when the cursor is on their edge.

### Fixes
- Launcher, Graphics page: the info text is a box beside the options again instead of floating over them at the top right.
- Opening the Select / Esc menu while moving no longer leaves the character walking: the sticks and the jump button are
  released as if the buttons had come up. (`src/jfg/jfg_debug_menu.cpp`, `controls.cpp` via `patches/jfg-base.patch`.)
- Mouse and keyboard: while the Select / Esc menu is open the mouse no longer turns the camera or aim, or changes weapon, in the
  game behind it. (`src/jfg/jfg_debug_menu.cpp`.)
- Boss fights with the dual-stick and mouse and keyboard controls: the aim moves directly (not through the game's stick, which
  made the mouse reticle shake and drift), and the character never turns: it stays on the rail, facing the boss, and
  only steps left and right. Once the aim is more than 40 degrees to a side the camera turns on the spot with it, so a
  boss at the sides of the arena can be followed. With a controller and "Keep the reticle centered" the camera follows
  the whole aim. (`src/jfg/jfg_debug_menu.cpp`, `boss_controls`, `jfg_camera_override`.)

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

