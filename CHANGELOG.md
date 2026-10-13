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

### Notes
- Changes were made in three places that must stay in step: `patches/jfg-base.patch` (what the build applies),
  `build/src/...` (the current build tree) and `src/jfg/` (the port's own code).
