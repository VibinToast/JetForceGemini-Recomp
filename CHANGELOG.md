# Changelog

Changes to Jet Force Gemini: Recompiled. Newest first. "Unreleased" is the next release, still in testing: its notes
are written here by hand and become that version's notes when it is released.

## Unreleased

### Changes

- Simpler settings: Frame Interpolation is now called Display Frame Rate (your setting is kept). Game Frame Rate, the fly camera's settings (F7) and the over-the-shoulder aiming sliders are hidden until you turn on Show Advanced Options (General tab, above Language); they then show in their usual places.
- New players start on the Modern profile. If you already have settings, nothing changes.
- The Default profile is now called Original.

### Fixes

- Custom levels: the hero no longer falls through floors or stops at invisible walls in big levels (higher collision limits, long thin triangles handled, floors joined where the level's pieces meet), and rooms remember up to 255 killed enemies and broken crates instead of 128.
- Customize Buttons: the note about the Built-in Button Layout no longer covers the first binding row, Use My Bindings stays inside the panel, and a controller can reach it.

## v1.2.4 (2026-10-25)

### Fixes

- Fixed the game showing a black screen and closing at start-up on some PCs, where the renderer has to use Vulkan instead of Direct3D 12 (for example older Intel HD Graphics drivers) (#22).

## v1.2.3 (2026-10-24)

### New options

- HUD in Screen Corners (Graphics): in widescreen the HUD moves out to the corners of the window. It needs Widescreen on.
- Shadow Opacity (Graphics): how dark the shadows under characters are drawn (50% in every profile).
- Blood Splat Limit (Gameplay): with Blood Stays on, how many blood splats stay, from 64 (the original game) up to 4096. Blood Amount now goes up to 800%.
- Camera Distance, Aim Offset and Aim Zoom (Controls): where the dual-stick camera sits and how the aiming view looks. Over-the-Shoulder Aiming is now a preset of these sliders.
- Same Aim-Walk Speed in All Directions (Controls): while aiming, walking backwards and sideways is as fast as walking forwards.
- A Quit tab in the settings menu (Return to Launcher, Quit Game) replaces the two small icon buttons.

### Changes

- Dual stick: the camera pivots on the middle of the character, while aiming too. The crosshair is in the middle of the screen from the first frame you aim, with a small drag margin when you strafe. In boss fights the game's own camera stays in charge and the view turns with the aim. Lupus's hover no longer looks like a drop (#14, #16).
- Settings menu: the game behind it is darkened, it reopens on the last tab and remembers your place in each tab, and the Profiles tab's description is a card like the other tabs'.
- The selected profile and texture pack have a gold outline, in the settings menu and the launcher.
- With a controller: with a slider selected, LB / RB move it by 10 (hold to keep moving), and holding a direction speeds it up. The last option scrolls the list to the bottom, and locked options are skipped.
- Touch in the menus on the Steam Deck's screen: tap to select, drag to scroll, and drag a slider.
- Profiles: Blood Splat Limit 64 in Default, 128 in Modern and Modern and Balanced, and 4096 in Extreme (with Blood Stays and Blood Amount 800%). Normalize Diagonal Speed and Same Aim-Walk Speed in All Directions are on in all but Default. Extreme sprints, also while aiming and swimming.
- The game always pauses while the settings menu is open (the Pause in Settings Menu option is gone).
- The in-game "Update available" notice shows for 5 seconds.

### Fixes

- Lupus's jetpack flames no longer flicker or lag behind while hovering at frame rates above 30.
- The machine gun's crosshair shakes again while firing.
- The reticle no longer stays on screen through a loading zone or warp with Frame Interpolation on.
- Widescreen: the planet map shown after leaving a planet keeps its planets inside the picture.
- Blood Stays keeps every splat up to the Blood Splat Limit, instead of old ones disappearing to make room.
- Fixed a rare crash in busy fights at Game Frame Rate 60.

## v1.2.2 (2026-10-22)

### New options

- Smooth Shadows (Graphics, on by default): with Frame Interpolation, character shadows move smoothly with the character instead of trailing behind it.

### Fixes

- Blood Stays: new blood always appears; the oldest stains make room once the screen is full.
- Game Frame Rate 60 is now marked experimental in the settings menu.
- The Modern, Modern and Balanced and Extreme profiles now keep the game at its original 30 and smooth it to 60 fps with Frame Interpolation (choose the profile again to get the new settings).
- The settings menu scrolls all the way to the top with a controller or the keyboard, so the text and titles above the first options show again.
- Fixed a crash in the Lurg boss fight (Tawfret) at 60 fps.
- The aiming reticle of the Tri-Rocket, Homing Missiles, mines, grenades and the other weapons whose reticle the game draws itself no longer shakes while you aim and turn with Frame Interpolation above the game's frame rate.
- With Frame Interpolation, those reticles use the exact in-between point each frame was drawn at, so they stay in step when an in-between frame is dropped (issue #8).
- At Game Frame Rate 30 with Frame Interpolation on, those reticles no longer flicker between two positions when you swing the aim quickly.
- The aiming reticle no longer shakes up and down while you aim and turn with Frame Interpolation on (most noticeable with the mouse).
- The star in the middle of the aiming reticle (e.g. the blaster's) no longer flickers between two places while you aim and turn with Frame Interpolation on.
- With the game's own Widescreen screen option on, mouse and keyboard controls work again and START opens the pause menu instead of fast-forwarding (#10).
- In the Bindings tab, Ctrl, Alt and the Windows key show their key symbols instead of a number (such as 224 for Left Ctrl), and keys shown by name (such as Keypad 1) fit their box (#10).
- START now skips the scene in level 398, the last one that couldn't be skipped.
- Dual stick: pressing aim after turning the camera now starts the aim where you were looking, up and down included, instead of snapping back to the way the character faces (#14).

## v1.2.1 (2026-10-22)

### Fixes

- In-app updates now install completely, even when the game can't swap its own exe straight away. Before, the menus and launcher could stay on the old version.
- If you updated to 1.2.0 inside the game and your launcher or menus look old, update to 1.2.1, or download it from the release page.

## v1.2.0 (2026-10-22)

### New options

- Auto Save: the game saves itself a few seconds after you arrive in a level and every few minutes, so a crash no longer loses your progress. It is on in every profile except Default (the original game).
- Keep Area Progress: saving in the middle of an area keeps the rescued Tribals, cleared rooms, continues and the room you were in.
- Permanent Tribal Saves: a rescued Tribal stays rescued after you leave its area, get a game over or load your file.
- Unlimited Continues: dying never uses up your continues, so you are never sent back to the start of an area.
- Sprint: hold the left stick click or Left Shift to run faster. Sprint While Aiming lets you keep sprinting while you aim, and Normalize Diagonal Speed replaces Diagonal Speed Boost and Strafe Speed.
- Sprint While Swimming: hold the sprint button to make Vela swim and dive faster, with its own Swim Sprint Speed.
- Gore: Blood Stays, Body Parts Stay and Blood Amount.
- Hide Floyd: Floyd is not drawn while he follows you, and his shadow and sounds are gone too. He still flies along and shoots.
- Hit Flash Opacity: how strongly the screen turns red when you are hit. 0% turns the red flash off.
- Pause in Settings Menu: the game pauses while the settings menu (Select / Esc) is open.
- Show Save Notification: the "Game saved" message can be turned off.
- Floyd missions can be flown with mouse and keyboard.

### Frame rate

- Two new options in the Graphics tab replace the old Frame Rate setting:
- Game Frame Rate: 30 (like the N64) or 60. Game speed stays the same either way.
- Frame Interpolation: adds in-between frames so movement looks smoother, with either Game Frame Rate. Choose Off, 60, 90, 120, 144, 240 or Monitor (your monitor's refresh rate).
- Changes to either option apply at once, with no restart.
- The old "60 + Interpolated" choice is gone: it is now Game Frame Rate 60 with Frame Interpolation on Monitor. If you had it chosen, the update switches you to that by itself.
- Show FPS shows both the game's frame rate and the frame rate on screen.
- At 60 fps, ants, Tribals and other enemies move on every frame instead of looking like 30 fps.
- At 60 fps, the slug boss's bolts and the target lock keep their original timing.

### Settings menu

- Everything is in one menu with 8 tabs: Profiles, General, Gameplay, Controls, Bindings, Graphics, Textures and Mods.
- New Jet Force Gemini colors, bigger option descriptions with the option's name as a heading, and options grouped under headings.
- The bumpers switch tabs instead of the triggers, and a controller reaches every option.
- Choosing a profile shows what it will change before it is applied, and warns you if it would replace your controller bindings. New Keep My Controls option.
- The Bindings tab tells you when its bindings are not the ones in use, and its note no longer covers the list. The Profiles tab scrolls instead of running off the bottom of the menu.

### Launcher

- Titles and buttons use the Jet Force Gemini font, and the selected button has a gold outline like the in-game menu.
- New options: Skip the Launcher, Return to Launcher and Check for Updates. The launcher opens on Start Jet Force Gemini.
- Full controller support: B goes back, LB / RB switch pages, Start jumps to the Start button and the right stick scrolls.
- New Mods page, and an Open Texture Packs Folder button.
- The changelogs are easier to read.

### Texture packs

- The launcher can install and update the official HD texture pack.
- Texture packs show their credits: name, version, author, description, website and picture.
- Texture packs that come split into several files are joined by the game.
- Texture packs keep their own settings (such as the texture shift) instead of having them reset when the pack loads.
- Pack buttons are labelled Folder or .rtz, so two copies of the same pack can be told apart.
- HUD numbers and text in HD texture packs sit where the original ones do.
- Texture packs whose file ends in .RTZ (or any other mix of capitals) now load. Before, they were listed but the game ignored them.

### Graphics

- The renderer is updated to the latest RT64: more accurate textures and clipping, and a fix for AMD RDNA4 graphics cards on Vulkan.
- Ambient occlusion no longer draws a crosshatch pattern on faces, works in the title screen and cutscenes, and no longer darkens flat or very close surfaces.
- Widescreen: the Tribal counter's heads no longer squeeze together and cover their numbers.
- Widescreen: reticles for the Tri-Rocket, Homing Missiles, mines, grenades and other weapons are no longer stretched.

### Linux

- Linux releases come as one AppImage that runs on SteamOS (Steam Deck), Fedora and Ubuntu, plus a portable folder.
- On Linux, options that need a restart now start the game again instead of only closing it.

### Tools (F10)

- The F10 tools have the game's own menu style, and work with a controller (a Tools button in the Select menu).
- Level Warp shows the level and entrance you are in, with a Pick button to warp there, and keeps its controls in view above the level list.
- The Items and Weapons sections open as drawers.

### Fixes

- Fixed a crash in the Lurg fight.
- Fixed the game freezing when the menu was closed twice in a row.
- Fixed a freeze at start-up, and when going fullscreen or changing the frame rate during play, with in-between frames on (#3).
- Fixed a crash in cutscenes with anti-aliasing and in-between frames on.
- Fixed a rare crash in the alien boss fight where the boss throws a volley of rocks.
- After closing a menu, the mouse buttons fire and aim again, and holding fire (or a Steam Deck screen tap) while closing it no longer keeps you firing.
- Weapon reticles drawn by the game (Tri-Rocket, Homing Missiles, mines, grenades and others) show again with Frame Interpolation on.
- The start-up key hint no longer mentions F9 (the debug window).

## v1.1.8 (2026-10-18)

- Old shadow pictures no longer show up as black boxes in place of textures
- Texture packs (.rtz) bigger than 2 GB made with Python load again

## v1.1.7 (2026-10-18)

- ToDo's and known issues
- Reduce recorded junk thrown into trace_log.
- Reflective floors no longer show a 4:3 box in widescreen
- Going through a door with dual-stick or mouse and keyboard no longer turns the camera towards the exit
- Character shadows have a soft edge again
- Character shadows are now drawn at full resolution instead of blocky
- Smooth character shadows instead of blocky ones
- The update window no longer pops up by itself, and a new bin\jfg_launcher.txt can turn off update checks or start the game directly

## v1.1.6 (2026-10-18)

- Fix Mizar's first phase never ending at 60 fps
- Fix a crash going through a loading zone at 60 fps
- Fix crashes and freezes in busy fights at 60 fps: smoothed enemies no longer touch enemies that were just removed
- Fix a crash in boss fights when an enemy that had already been removed was still listed in its squad
- The re-upscale texture script now copies textures instead of moving them, so the dump stays complete
- Fix a freeze and a crash when the game could not create an enemy or a turret shot
- Fix a rare freeze in play where the picture stopped and the game hung
- Texture dumps now record how each texture is drawn, for better HD texture packs
- Smoother enemies at 60 fps, and weapon reticles no longer vanish at 60 fps
- Reorganise the port's code into smaller files (no change in the game)

## v1.1.5 (2026-10-17)

- Modern profiles: aim speed 100% and movement speed 110%
- Run enemies and tribals at their original speed at 60 fps
- Fix drones flying in and attacking too fast at 60 fps
- Make new updates hard to miss in the launcher
- Leave README and other repository-only changes out of the changelogs

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
- Add a launcher in Jet Force Gemini colors
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

