# Making a texture pack

A texture pack replaces the game's textures with your own images, for example upscaled or repainted ones. This page
covers the whole path: dumping the game's textures, replacing them, testing, and packing the result into one file to
share.

You need the game built and running (see the main [README](../README.md)). All paths below are inside the `bin` folder
next to the game.

## How it works

The game identifies each texture by a 16-character hash, such as `0bdd2938fb585294`. A replacement is any image whose
file name starts with that hash. Nothing else needs to be set up: the hash is the link between the image and the
texture it replaces.

- A replacement can be any size. The game scales it to fit.
- A suffix after the hash is fine, so `0bdd2938fb585294_upscayl_4x_model.png` still works (this is what Upscayl adds).
- Files can be PNG or DDS, and can sit in sub-folders.
- Textures with no replacement are drawn as normal, so a pack can be as small or as large as you like.

## 1. Dump the game's textures

1. Start the game and open the settings menu (Select / Esc), then the **Textures** tab.
2. Turn on **Dump textures**. Turn on **One folder per level** if you want the dump sorted by level.
3. Play. Every texture the game draws is saved the first time it appears, as `<hash>.png`, in `bin/texture_dump`
   (or in a sub-folder named after the level).
4. Turn dumping off when you are done. The Textures tab shows how many files have been dumped.

Walk through every level you want covered. A texture only appears in the dump once the game has drawn it.

### Leave out the shadow textures

The game draws each character's shadow by rendering a 64x64 black silhouette every frame, so the dump fills up with
thousands of shadow images that are never used again. Upscaling them does nothing. Run
`tools/textures/move shadow textures.ps1` to move them into `bin/texture_shadows` (nothing is deleted; use `-ListOnly`
to only count them).

## 2. Make the replacements

Do this part with the tools you like. The only rules are that the file name keeps its 16-character hash at the start
and that the image keeps its proportions.

A common route is to run the dumped PNGs through an AI upscaler (Upscayl, for example) and save the results into a
pack folder. Some notes:

- Make a folder `bin/texture_packs/<your pack name>` and put the finished images in it.
- To work through the dump in batches, `tools/textures/move new textures.ps1` copies every dumped texture that does
  not have a replacement in your pack yet into `bin/texture_new`, ready to upscale. It leaves files already there
  alone, so run it again after every play session. Pass `-Pack <name>` to use a pack other than the default.
- Some textures are stored upside down or mirrored because of how the game maps them. Keep them as they are.
- Transparent textures (fences, foliage, HUD pieces) are where upscalers go wrong most often. Check them in game.

## 3. Test it in the game

1. Open the **Textures** tab. Your pack folder is listed under **Texture pack**. Select it.
2. The tab shows how many replacement textures were loaded. If it says 0, the file names do not start with a hash.
3. **F6** switches to the next pack, then to Off, so you can compare with the original. **Reload** picks up files you
   added or changed.

The game writes a file called `rt64.json` into the pack folder. It lists every image it found, and you need it for
the packing steps below, so select the pack in the game at least once.

## 4. Convert to DDS (optional)

PNG packs work fine. DDS is the format graphics cards read directly, so a DDS pack needs less video memory and
should load with less stutter, at the cost of a small loss in quality. It is worth trying for a large pack
(thousands of textures).

1. Download `texconv.exe` from the Microsoft DirectXTex releases (https://github.com/microsoft/DirectXTex/releases)
   and put it in `tools/textures`, or anywhere on your PATH.
2. Drag your pack folder onto `tools/textures/png_to_dds.bat`.
3. It makes a new folder next to yours called `<pack>_dds`, holding BC7 DDS versions with mipmaps and an `rt64.json`
   that points at them. Your original folder is not changed.

Settings (format, color handling) are at the top of `png_to_dds.ps1`.

Try a few textures first. If the DDS ones look darker or lighter than the PNG ones in game, open `png_to_dds.ps1` and
set `$KeepRawValues` to `$false`, then convert again. Check textures with transparency too, since compression can
soften hard edges.

## 5. Pack it into a single file

A pack with thousands of loose files is awkward to share. A `.rtz` file is one archive of the whole pack.

1. Drag the pack folder (the PNG one, or the `_dds` one) onto `tools/textures/make_rtz.bat`.
2. It writes `<pack>.rtz` next to the folder. The folder must contain `rt64.json` (see step 3).

Then put the `.rtz` in one of two places:

| Location | What happens |
|---|---|
| `bin/texture_packs/` | Listed in the **Textures** tab with the folder packs. Choose it there, or cycle with F6. |
| `bin/mods/` | Loaded as a mod and turned on by default. Switch it on and off, or change its order, in the **Mods** tab. |

Put a pack in one place, not both. To change a pack later, edit the folder and run `make_rtz.bat` again.

By default `make_rtz.bat` stores files without compression, which loads fastest. For a DDS pack, set `$Compression`
to `Optimal` at the top of `make_rtz.ps1` if you want a smaller file.

## Sharing a pack

The dumped textures are the game's own images, and an upscale of them is still derived from the game. Do not share
the dump, and do not share the ROM or anything generated from it. A pack made of your own work is for you to share
under whatever terms you choose; check what the tool or model you used allows.

## Summary

| Step | Tool |
|---|---|
| Dump textures | Textures tab, **Dump textures** |
| Remove shadow textures | `tools/textures/move shadow textures.ps1` |
| Collect what still needs upscaling | `tools/textures/move new textures.ps1` |
| Test | Textures tab, **F6**, **Reload** |
| Convert to DDS (optional) | `tools/textures/png_to_dds.bat` (needs `texconv.exe`) |
| Pack into one file | `tools/textures/make_rtz.bat` |
