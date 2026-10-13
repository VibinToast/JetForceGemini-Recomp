# Changelog

Changes to Jet Force Gemini: Recompiled. Newest first. Entries under "Unreleased" are in the source but not yet
built and tested, unless marked otherwise.

## Unreleased

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
