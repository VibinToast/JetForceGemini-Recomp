#!/usr/bin/env python3
"""Generates the port's tabs of the "Select" config menu (assets/config_menu/jfg_*.rml) and the list of option keys
each tab binds (src/jfg/jfg_config_keys.inc). Run from the repository root after changing the lists below:
    python3 tools/gen_jfg_config_rml.py
The options themselves (ranges, defaults, saving) live in src/jfg/jfg_options.h and jfg_options.cpp (apply_option / options_text)."""
import html, os

# (kind, key, title, extra, help). kind: group | bool | range (extra = (min, max, suffix)) | choice (extra = [(value, label)])
# Keys are the port's options (jfg::settings) except the ones in UPSTREAM_KEYS (ui_config.cpp: volume, controller and
# window settings of the recomp itself), which the port's tabs show too so that each setting is in one place.
# General: volume, the menu, what is shown on screen and, last, Advanced Options. The Language list and a note about F9
# follow the options.
GENERAL = [
    ("group", None, "Sound", None, None),
    ("range", "main_volume", "Master Volume", (0, 100, "%"), "The volume of all of the game's sound."),
    ("group", None, "Menu & Cutscenes", None, None),
    ("bool", "start_skips_cutscenes", "START Skips Cutscenes", None,
     "START skips cutscenes, the logo screens at start-up and the story intro after creating a file. A skipped cutscene still runs to its end at full speed, so everything it does (flags, items, the next level) still happens."),
    ("bool", "fast_pause", "Faster Pause Menu", None,
     "The pause menu opens and closes three times as fast. Off is the original game's speed."),
    ("group", None, "On Screen", None, None),
    ("bool", "show_fps", "Show FPS", None, "In the top right corner: <b>Game</b>, how many times a second the game itself updates (its logic), and <b>Screen</b>, the frames a second shown (in-between frames included)."),
    ("bool", "show_level", "Show Level", None, "The level number and name in the top right corner."),
    ("bool", "show_pack", "Show Texture Pack", None, "The texture pack in use in the top right corner."),
    ("bool", "save_notification", "Show Save Notification", None, "\"Game saved\" shows briefly in the bottom right corner after an auto save."),
    ("group", None, "Fly Camera (F7)", None, None),
    ("range", "fly_speed", "Fly Speed", (10, 1000, "%"),
     "F7 (or F10 > Tools) flies a free camera for screenshots: WASD / left stick move where it looks, mouse / right stick look, Space / A up, Ctrl / B down, holding fire flies faster. F7 again ends it."),
    ("bool", "fly_freeze", "Freeze While Flying", None, "The game stands still while the fly camera is on (enemies, effects and the level stop as when paused)."),
    ("bool", "fly_hide_hud", "Hide HUD While Flying", None, "The health meter and the rest of the HUD are hidden while the fly camera is on."),
    ("group", None, "Launcher", None, None),
    ("bool", "skip_launcher", "Skip the Launcher", None,
     "<b>On</b>: the game starts straight away with your ROM, without the launcher. In the game, the launcher button next to Quit in this menu still takes you back to it. Saved in jfg_launcher.txt next to the game."),
    ("bool", "check_for_updates", "Check for Updates", None,
     "<b>On</b>: when the game starts it asks GitHub whether a newer release exists, and the launcher and a note in the game show one. <b>Off</b>: it never contacts GitHub. Turning it on checks straight away. Saved in jfg_launcher.txt next to the game."),
    ("bool", "show_official_pack", "Official HD Texture Pack", None,
     "<b>On</b>: the launcher's Texture Packs page shows the official HD texture pack, to download or update with one click (with Check for Updates on). <b>Off</b>: hidden, and GitHub is not asked about it. Saved in jfg_launcher.txt next to the game."),
    # Advanced Options: the switch that shows the settings for experts on their own tabs (ADVANCED_GROUPS, ADVANCED_KEYS).
    # It is not saved (UI_ONLY_KEYS): they are hidden again at each start.
    ("group", None, "Advanced Options", None, None),
    ("bool", "show_advanced", "Show Advanced Options", None,
     "Shows settings most players can leave as they are, each on its own tab: Game Frame Rate (Graphics, 60 is experimental), the fly camera (F7, above) and the aiming view's sliders (Controls, under Over-the-Shoulder Aiming)."),
]

GAMEPLAY = [
    ("group", None, "Saving", None, None),
    ("bool", "auto_save", "Auto Save", None,
     "Saves to the file you are playing a few seconds after arriving in a level, and every few minutes of play, the same save as Z on the pause screen. Not during cutscenes, races, Floyd missions or while paused."),
    ("range", "auto_save_minutes", "Auto Save Interval", (0, 30, " min"), "Minutes of play between auto saves. 0: only on arriving in a level."),
    ("bool", "keep_area_progress", "Keep Area Progress", None,
     "Saving in the middle of an area (auto save or Z on the pause screen) also keeps what was done there: Tribals rescued or lost, cleared rooms, continues, and the room you were in. Loading the file puts it all back. Off: the game's own behaviour, where an area starts over from its entrance after loading."),
    ("bool", "permanent_tribals", "Permanent Tribal Saves", None,
     "A Tribal you rescued stays rescued: going back into its area, a game over or loading your file no longer brings it back to be rescued again, so the Tribals of an area can be rescued over several visits. Rescues are kept each time the game saves (including auto saves)."),
    ("group", None, "Player", None, None),
    ("range", "damage_taken", "Damage Taken", (0, 400, "%"),
     "How much damage the character takes from enemies, falls and explosions. 0% takes none; the Invulnerable cheat overrides it."),
    ("bool", "unlimited_continues", "Unlimited Continues", None,
     "Dying never uses up your continues, so you always come back where you died instead of the game over that restarts the area from its entrance."),
    ("range", "refuel_speed", "Jetpack Refuel Speed", (25, 1000, "%"), "How fast a fuel pad fills the jetpack."),
    ("range", "floyd_fuel", "Floyd's Fuel", (100, 300, "%"),
     "How long Floyd's fuel lasts in his missions. 100% is the original game; 300% lasts three times as long."),
    # Vela's Dive Speed (dive_speed) is hidden for now: DIVE_SPEED_OPTION in jfg_options.h.
    ("group", None, "Movement (any control layout)", None, None),
    ("range", "move_speed", "Movement Speed", (25, 400, "%"), "How fast the character moves over the ground. The extra distance is collision-checked like normal movement."),
    ("bool", "sprint", "Sprint", None,
     "Hold the left stick click (controller) or Left Shift (mouse and keyboard) to run faster. In the built-in dual-stick layout the left stick click then sprints instead of crouching; B still crouches."),
    ("range", "sprint_speed", "Sprint Speed", (100, 300, "%"), "How fast the character runs while sprinting, on top of the Movement Speed."),
    ("bool", "sprint_aiming", "Sprint While Aiming", None, "Sprinting also works while aiming, so the character moves faster with the aim held."),
    ("bool", "sprint_swimming", "Sprint While Swimming", None, "Sprinting also works in the water: Vela swims and dives faster while the sprint button is held."),
    ("range", "sprint_swim_speed", "Swim Sprint Speed", (100, 300, "%"), "How fast Vela swims while sprinting, on top of Vela's Dive Speed."),
    ("bool", "normalize_diagonal", "Normalize Diagonal Speed", None,
     "In the game, running forward while sidestepping adds the two speeds together, so going diagonally is about 40% faster. With this on a diagonal is as fast as running straight."),
    ("bool", "aim_even_speed", "Same Aim-Walk Speed in All Directions", None,
     "While aiming the game walks forward fastest: backwards is about 25% slower and sideways about 40% slower. With this on the character walks as fast in every direction, diagonals included."),
]

# The port's control options, after the Players row and Quick Setup (CONTROLS_TOP). The button bindings are their own
# page (controls.rml), opened with Customize Buttons. Layout holds the switches Quick Setup sets.
CONTROLS = [
    ("group", None, "Layout", None, None),
    ("bool", "dual_stick", "Dual-Stick Controls", None,
     "Right stick: moves the camera around the character. Left stick: runs in the pushed direction as seen from the camera. While the aim button is held the right stick aims and the left stick moves, as in a first-person game. Menus, cutscenes, Floyd missions and the races keep the normal layout."),
    ("bool", "dual_layout", "Built-in Button Layout", None,
     "LT aim, RT fire. A jump, B or left-stick click crouch. RB / Y next weapon, LB / X previous weapon. D-pad: weapon select; up/down zoom while aiming. View / right-stick click: N64 L. Off: your own controller bindings."),
    ("bool", "swap_triggers", "Swap Triggers", None, "RT aims and LT fires."),
    ("group", None, "Camera & Aim", None, None),
    ("bool", "invert_aim_y", "Invert Aim Up/Down", None, "Dual stick: right stick up aims down (the game's original flight-stick style)."),
    ("bool", "invert_camera_y", "Invert Camera Up/Down", None, "Dual stick: right stick up lowers the camera instead of raising the view."),
    ("bool", "aim_from_character", "Aim Where the Character Faces", None, "On aiming, the view turns to the character instead of the character to the view."),
    ("range", "camera_speed_h", "Camera Left/Right", (10, 400, "%"), "Dual stick: how fast the right stick turns the camera sideways."),
    ("range", "camera_speed_v", "Camera Up/Down", (10, 400, "%"), "Dual stick: how fast the right stick moves the camera up and down."),
    ("range", "camera_distance", "Camera Distance", (50, 250, "%"), "Dual stick: how far back the camera is from the character, as a share of the game's own distance. Lower is closer."),
    ("bool", "camera_collision", "Camera Collision", None, "Dual stick: the camera stays inside the level (the game's own wall check) instead of going through walls."),
    ("bool", "aim_shoulder", "Over-the-Shoulder Aiming", None, "While aiming, the view is from over the character's right shoulder. With the dual stick this sets the aiming view's sliders below to a fixed shoulder view (greyed out); off, they are yours to set."),
    ("range", "aim_drag_margin", "Aim Drag Margin", (0, 50, "%"), "Dual stick, aiming: how far the character may step aside from the middle of the screen before the camera follows, as a share of half the screen's width. 0 keeps the camera on the character. Fixed while Over-the-Shoulder Aiming is on."),
    ("range", "aim_drag_return", "Aim Drag Return Speed", (0, 400, "%"), "Dual stick, aiming: how fast the camera keeps coming back onto the character, moving or not. 100% takes about a third of a second; 0 leaves it where it is. Fixed while Over-the-Shoulder Aiming is on."),
    ("range", "aim_offset_x", "Aim Offset Left/Right", (-50, 50, ""), "Dual stick, aiming: the point the camera turns round moves this far to the right of the character (negative: left), so the view can look past a shoulder. Fixed while Over-the-Shoulder Aiming is on."),
    ("range", "aim_offset_y", "Aim Offset Up/Down", (-50, 50, ""), "Dual stick, aiming: the point the camera turns round moves this far up from the middle of the character (negative: down). Fixed while Over-the-Shoulder Aiming is on."),
    ("range", "aim_offset_z", "Aim Zoom", (0, 300, ""), "Dual stick, aiming: the camera moves this much closer in. Fixed while Over-the-Shoulder Aiming is on."),
    ("bool", "aim_centered", "Keep the Reticle Centered", None,
     "While aiming, the view turns as soon as the stick moves and the reticle stays in the middle (instead of the reticle first travelling to the edge of a box and drifting back)."),
    ("bool", "aim_jump", "Jump While Aiming", None,
     "The jump button jumps while aiming, also while walking or strafing: the character leaves the aim for the jump and aims again on landing if the aim button is still held. The sniper scope then zooms with the D-pad (dual stick) or 1 / 3 (mouse and keyboard)."),
    ("range", "aim_speed", "Aim Speed", (10, 300, "%"), "How fast the stick turns the view / moves the reticle while aiming."),
    ("bool", "sniper_fixed_zoom", "Sniper Fixed Zoom", None,
     "The Sniper Rifle's scope stays at the zoom set below; the zoom buttons do nothing."),
    ("range", "sniper_zoom", "Sniper Zoom", (100, 500, "%"), "The fixed zoom of the Sniper Rifle: 100% is no zoom, 500% the game's closest."),
    ("bool", "invert_aim_normal", "Invert Aim (Original Controls)", None, "Original controls only: flip the stick's up/down while aiming (R held)."),
    ("group", None, "Sticks & Rumble", None, None),
    ("range", "joystick_deadzone", "Joystick Deadzone", (0, 100, "%"),
     "Applies a deadzone to joystick inputs.<br/><br/><b>Note: the game may apply a small additional deadzone of its own.</b>"),
    ("range", "joystick_range", "Joystick Range", (0, 100, "%"),
     "Sets the input range the joystick is mapped to. A real N64 controller usually does not reach the full technical range.<br/><br/>Values above 62% increase joystick sensitivity. <i>Warning:</i> below 62% the stick can no longer reach its maximum values, which may hinder gameplay. If your joystick cannot reach 100% in the cardinal directions, set this above 62% to compensate."),
    ("range", "rumble_strength", "Rumble Strength", (0, 100, "%"),
     "The strength of rumble on a controller that supports it. <b>Zero turns rumble off.</b> Jet Force Gemini supports the Rumble Pak."),
    ("bool", "background_input", "Background Input", None,
     "The game reads the controller while its window is not in focus. <b>Keyboard input is not affected.</b>"),
    # Hidden while a controller plays and mouse and keyboard controls are off (GROUP_HIDDEN_BY, mk_hidden).
    ("group", None, "Mouse & Keyboard", None, None),
    ("bool", "mouse_keyboard", "Mouse and Keyboard Controls", None,
     "Plays like the dual-stick controls: WASD run, the mouse turns the camera, hold the right button to aim and click the left to fire, the wheel changes weapon. Space jumps, Left Ctrl or C crouches, Enter is START, Tab is N64 L, 1-4 are the D-pad. The cursor is captured during gameplay only."),
    ("range", "mouse_camera_speed", "Mouse Camera Speed", (10, 400, "%"), "How fast the mouse turns the camera."),
    ("range", "mouse_aim_speed", "Mouse Aim Speed", (10, 400, "%"), "How fast the mouse aims."),
    ("bool", "invert_mouse_y", "Invert Mouse Up/Down", None, "Mouse forward looks down."),
]

# The top of the Controls tab: the Players row (P1 shows the device in use; P2-P4 are for later multiplayer: today every
# controller plays P1, src/input/input.cpp), Quick Setup (three ways to set the Layout switches at once) and Customize
# Buttons (the bindings page, controls.rml). Values come from jfg_controls_model (src/ui/ui_config.cpp). Words next to a
# {{value}} are in their own span, so a language can swap them.
QS_BUTTON = '''                            <button class="button" id="jfg_qs_{mode}" data-class-jfg-selected="qs_mode == '{mode}'" data-event-click="quick_setup('{mode}')" data-event-focus="set_cur_config_index({index})" data-event-blur="set_cur_config_index(-1)" data-event-mouseover="set_cur_config_index({index})" style="display: block; flex: 0 0 auto; white-space: nowrap; margin: 0dp 10dp 12dp 0dp; padding-left: 12dp; padding-right: 12dp; nav-up: #tab_jfg_controls; nav-down: #jfg_customize_buttons; nav-left: {left}; nav-right: {right};">{label}<span data-if="qs_recommend == '{mode}'"><span style="margin-left: 8dp; padding: 2dp 6dp; font-size: 13dp; line-height: 18dp; letter-spacing: 1dp; background-color: #ffb21a; color: #050b1c;">Recommended</span></span></button>'''
QS_MODES = (("modern", "Modern"), ("n64", "Original N64"), ("custom", "Custom"))
QS_INDEX = 1001 # cur_config_index of the first Quick Setup button's description (CONTROLS_DESC); Customize Buttons is 1004

def player_slot(n):
    return (f'                        <div style="display: block; flex: 1 1 0dp; margin: 0dp 12dp 0dp 0dp; padding: 12dp 14dp; border-width: 2dp; border-color: rgba(255,255,255,0.15); border-radius: 8dp; opacity: 0.45;">'
            f'<div style="display: block; font-size: 22dp; line-height: 28dp; font-weight: bold;">P{n}</div>'
            f'<div class="jfg-note" style="display: block; margin-top: 6dp; font-size: 16dp; line-height: 22dp;">Coming later</div></div>')

CONTROLS_TOP = "\n".join([
    '                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">Players</label></div>',
    '                    <div style="display: flex; flex-direction: row; margin: 8dp 12dp 0dp;">',
    '                        <div style="display: block; flex: 2 1 0dp; margin: 0dp 12dp 0dp 0dp; padding: 12dp 14dp; border-width: 2dp; border-color: #ffb21a; border-radius: 8dp; background-color: rgba(255,178,26,0.08);">'
    '<div style="display: block; font-size: 22dp; line-height: 28dp; font-weight: bold;">P1</div>'
    '<div style="display: block; margin-top: 6dp; font-size: 20dp; line-height: 26dp;">{{p1_device}}</div>'
    '<div data-if="p1_name != \'\'"><div class="jfg-note" style="display: block; margin-top: 4dp; font-size: 15dp; line-height: 20dp;">{{p1_name}}</div></div></div>',
    *(player_slot(n) for n in (2, 3, 4)),
    '                    </div>',
    '                    <div class="jfg-note" style="display: block; margin: 10dp 12dp 0dp; font-size: 18dp; line-height: 26dp;">'
    '<span data-if="controller_count &gt; 1">{{controller_count}} <span>controllers connected: all control P1.</span> </span>'
    '<span>Multiplayer: coming later.</span></div>',
    '                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">Quick Setup</label></div>',
    '                    <div style="display: flex; flex-direction: row; flex-wrap: wrap; margin: 10dp 12dp 0dp;">', # longer translations wrap
    *(QS_BUTTON.format(mode=m, label=label, index=QS_INDEX + i,
                       left="#tab_jfg_controls" if i == 0 else "#jfg_qs_" + QS_MODES[i - 1][0],
                       right="#jfg_qs_" + QS_MODES[i + 1][0] if i + 1 < len(QS_MODES) else "none")
      for i, (m, label) in enumerate(QS_MODES)),
    '                    </div>',
    '                    <div class="jfg-note" style="display: block; margin: 0dp 12dp 0dp; font-size: 18dp; line-height: 26dp;">Sets the Layout switches below in one go. Nothing changes until you pick one.</div>',
    '                    <div style="display: block; margin: 20dp 12dp 0dp;">',
    f'                        <button class="button" id="jfg_customize_buttons" data-event-click="open_bindings()" data-event-focus="set_cur_config_index({QS_INDEX + 3})" data-event-blur="set_cur_config_index(-1)" data-event-mouseover="set_cur_config_index({QS_INDEX + 3})" style="display: inline-block; margin: 0dp; nav-up: #jfg_qs_modern; nav-down: #jfg_dual_stick_on; nav-left: #tab_jfg_controls; nav-right: none;">Customize Buttons</button>',
    '                    </div>',
])

# The descriptions of the Quick Setup buttons and Customize Buttons (on the right, as the options' are).
QS_HELP = (
    ("Modern", "Dual-stick controls with the built-in button layout, and mouse and keyboard controls: the right stick or the mouse turns the camera, LT or the right mouse button aims, RT or the left button fires.<br/><br/>Sets <b>Dual-Stick Controls</b>, <b>Built-in Button Layout</b> and <b>Mouse and Keyboard Controls</b> on."),
    ("Original N64", "The original game's controls: the stick runs and turns the character as on the N64, with your button bindings.<br/><br/>Sets <b>Dual-Stick Controls</b> and <b>Mouse and Keyboard Controls</b> off."),
    ("Custom", "Dual-stick controls with your own button bindings (Customize Buttons).<br/><br/>Sets <b>Dual-Stick Controls</b> on and <b>Built-in Button Layout</b> off."),
    ("Customize Buttons", "Choose which button or key does what, for the controller and for the keyboard. While the Built-in Button Layout or mouse and keyboard controls are on, they use their own layout during play."),
)
CONTROLS_DESC = "\n".join(
    f'                    <div data-if="cur_config_index == {QS_INDEX + i}"><div class="jfg-desc__card"><div class="jfg-desc__title">{t}</div><p>{h}</p></div></div>'
    for i, (t, h) in enumerate(QS_HELP))

# Groups shown only while a data expression is false, with their options (a wrapper with data-if; Up/Down go past them
# while hidden, as past locked rows).
# The groups and single options for experts, hidden until Show Advanced Options (General) is on.
ADVANCED_GROUPS = ("Fly Camera (F7)",)
ADVANCED_KEYS = ("frame_rate", "aim_drag_margin", "aim_drag_return", "aim_offset_x", "aim_offset_y", "aim_offset_z")
GROUP_HIDDEN_BY = {"Mouse & Keyboard": "mk_hidden", **{g: "advanced_hidden" for g in ADVANCED_GROUPS}}
# Rows bound to the tab's own data model (ui_config.cpp) instead of a saved option: left out of the option key lists.
UI_ONLY_KEYS = ("show_advanced",)

GRAPHICS = [
    ("group", None, "Window", None, None),
    ("choice", "wm_option", "Window Mode", [(0, "Windowed"), (1, "Fullscreen")],
     "<b>Windowed</b> or <b>Fullscreen</b>. <b>F11</b> or <b>Alt + Enter</b> also switch it."),
    ("choice", "res_option", "Resolution", [(0, "Original"), (1, "Original 2x"), (2, "Auto")],
     "The resolution the game is drawn at. <b>Original</b>: the game's own 240p. <b>Original 2x</b>: 480p. <b>Auto</b>: follows the size of the window."),
    ("choice", "ds_option", "Downsampling", [(1, "Off"), (2, "2x"), (4, "4x")],
     "Draws at a higher resolution and scales it down for a cleaner picture. Only with the <b>Original</b> and <b>Original 2x</b> resolutions. <b>4x</b> at <b>Original 2x</b> draws at almost 4K and may be slow on low-end graphics cards."),
    ("choice", "msaa_option", "Anti-Aliasing (MSAA)", [(0, "None"), (1, "2x"), (2, "4x")],
     "Multisample anti-aliasing: smoother edges at the cost of rendering performance. Not available if your graphics card does not support programmable sample positions (needed to avoid rendering glitches)."),
    ("group", None, "Frame Rate", None, None),
    # Saved as smooth_motion (its name before 1.3, "Frame Interpolation"), so saved settings and profiles keep it.
    ("choice", "smooth_motion", "Display Frame Rate", [(0, "Off"), (60, "60"), (90, "90"), (120, "120"), (144, "144"), (240, "240"), (1, "Monitor")],
     "How many frames a second you see. Above the game's own, the renderer adds in-between frames: movement looks smoother, with a little more delay, but the game itself runs no faster. <b>Monitor</b>: up to your monitor's refresh rate; never more than it. Rain and a few effects still move at the game's rate. A lower setting takes less from the graphics card."),
    ("choice", "frame_rate", "Game Frame Rate", [(0, "30 (Original)"), (1, "60 (Experimental)")],
     "How many times a second the game itself updates: movement, aiming, enemies. <b>30 (Original)</b>: like the N64 (it drops to 20 when busy). <b>60</b>: the quickest response; game speed stays the same. 60 is experimental: some enemies and bosses can behave differently than at 30. Choose <b>30</b> for the original game."),
    ("bool", "smooth_shadows", "Smooth Shadows", None,
     "With <b>Display Frame Rate</b>, character shadows move with the in-between frames instead of at the game's rate, so they no longer trail behind the character."),
    ("group", None, "Picture", None, None),
    ("bool", "widescreen", "Widescreen", None, "During gameplay the 3D view fills the window; the HUD stays in the middle. Things at the far left and right may appear late."),
    ("bool", "hud_corners", "HUD in Screen Corners", None, "With <b>Widescreen</b>, the HUD moves from the middle out to the left and right edges of the window."),
    ("range", "fov", "Field of View", (75, 140, "%"),
     "Widens or narrows the view during gameplay, relative to each level's own field of view. Cutscenes, menus, the pause screen and zoomed scopes keep the game's own."),
    ("choice", "texture_filter", "Texture Filtering", [(0, "N64 Three-Point"), (1, "Bilinear")],
     "<b>N64 Three-Point</b>: how the N64 smooths textures. <b>Bilinear</b>: smoother, as on PC."),
    ("choice", "output_filter", "Picture Scaling", [(0, "Anti-Aliased"), (1, "Linear"), (2, "Nearest")],
     "How the picture is scaled to the window. <b>Anti-Aliased</b>: sharp pixels with smooth edges. <b>Linear</b>: softer. <b>Nearest</b>: sharp pixels."),
    ("range", "hit_flash", "Hit Flash Opacity", (0, 100, "%"),
     "How strongly the screen turns red when your character is hit. 100% is the original game; 0% turns the red flash off."),
    ("range", "shadow_opacity", "Shadow Opacity", (0, 200, "%"),
     "How dark the shadows under characters and objects are drawn. 100% is the original game; 0% turns them off."),
    ("group", None, "Objects", None, None),
    ("bool", "no_lod", "Disable LOD", None, "Characters and objects keep their full-detail model at any distance."),
    ("bool", "far_objects", "Draw Distant Objects", None, "Objects no longer fade out and disappear beyond their draw distance. Level scenery itself is not affected."),
    ("group", None, "Ambient Occlusion", None, None),
    ("bool", "ssao", "Ambient Occlusion (SSAO)", None, "Darkens creases, corners and the ground around objects, worked out from the depth of the 3D view. The HUD is not affected."),
    ("range", "ssao_strength", "SSAO Strength", (10, 300, "%"), "How dark the ambient occlusion gets."),
    ("range", "ssao_radius", "SSAO Radius", (10, 300, ""), "How far around a surface ambient occlusion looks, in game units."),
    ("group", None, "Bloom", None, None),
    ("bool", "bloom", "Bloom", None, "Bright parts of the 3D view (lights, explosions, sky, shots) glow softly into their surroundings. The HUD does not glow."),
    ("range", "bloom_intensity", "Bloom Intensity", (0, 300, "%"), "How strong the glow is."),
    ("range", "bloom_threshold", "Bloom Threshold", (0, 100, "%"), "How bright a colour must be to glow. Lower: more of the picture glows."),
    ("range", "bloom_radius", "Bloom Radius", (1, 20, ""), "How far the glow spreads."),
    ("group", None, "Vignette", None, None),
    ("bool", "vignette", "Vignette", None, "Darkens the edges and corners of the 3D view. The HUD is not darkened."),
    ("range", "vignette_strength", "Vignette Strength", (0, 100, "%"), "How dark the corners get."),
    ("range", "vignette_size", "Vignette Size", (0, 90, "%"), "How far from the middle the darkening starts: higher keeps more of the picture clear."),
    ("group", None, "Fog", None, None),
    ("range", "level_fog_distance", "Level Fog Distance", (50, 400, "%"),
     "How far away the levels' own fog is. Some levels use it to hide where the scenery stops being drawn, so moving it back can show things appearing in the distance."),
    ("bool", "no_level_fog", "No Level Fog", None, "Turns the levels' own fog off. Things in the distance may be seen appearing."),
    # Depth fog (depth_fog, fog_start, fog_end, fog_density, fog_color) is hidden for now: DEPTH_FOG_OPTION in jfg_options.h.
]

# The Textures tab: the texture pack list (TEXTURE_PACKS) comes first, then these.
TEXTURES = [
    ("group", None, "Texture Dumping", None, None),
    ("bool", "texture_dump", "Dump Textures", None,
     "Saves every texture the game draws, the first time it appears, to the texture_dump folder next to the game, as &lt;hash&gt;.png. Off again at the next start. Upscale the files without renaming them and put them in a pack folder (texture_packs\\&lt;name&gt;)."),
    ("bool", "dump_by_level", "One Folder per Level", None, "Dumps into texture_dump\\&lt;level number&gt; &lt;level name&gt; instead."),
]

# Options shown greyed out and locked while a condition on another option holds (a data expression): the aiming view's sliders follow Over-the-Shoulder
# Aiming's fixed view then (jfg_options.h, aim_view; jfg_settings_api.cpp shows its values and ignores changes).
LOCKED_BY = {k: "aim_shoulder == 1" for k in ("aim_drag_margin", "aim_drag_return", "aim_offset_x", "aim_offset_y", "aim_offset_z")}
LOCKED_BY["hud_corners"] = "widescreen == 0" # it moves the widescreen HUD; without Widescreen there is nothing to move

def row_ids(kind, key, extra):
    """The focusable ids of one option row, left to right."""
    if kind == "bool": return [f"jfg_{key}_on", f"jfg_{key}_off"]
    if kind == "choice": return [f"jfg_{key}_{v}" for v, _ in extra]
    return [f"jfg_{key}_input"]

def option_markup(items, first_nav_up, last_nav_down="none"):
    # Up/Down are given explicitly for every control: automatic navigation skipped the sliders below a row of radio
    # buttons (and some rows above), and at the last row Down went sideways between its radios. Between two rows of
    # radios the column is kept (On to On); a slider row has one control. The first row goes up to first_nav_up (the
    # tab; None leaves it automatic), the last row's Down to last_nav_down (nowhere by default).
    # A locked row (LOCKED_BY) can't take the focus, so while its lock is on, Up/Down jump over the locked rows next to it
    # (a data-style-nav-* attribute, see nav_locked, in place of the fixed property).
    # Rows of a hidden group (GROUP_HIDDEN_BY) and hidden rows (ADVANCED_KEYS) are skipped like locked ones while hidden;
    # a row both hidden and locked is skipped while either holds.
    hidden_by, group_hide = {}, None
    for kind, key, title, _, _ in items:
        if kind == "group": group_hide = GROUP_HIDDEN_BY.get(title)
        elif group_hide: hidden_by[key] = group_hide
        elif key in ADVANCED_KEYS: hidden_by[key] = "advanced_hidden"
    rows = [row_ids(kind, key, extra) for kind, key, _, extra, _ in items if kind != "group"]
    locks = [" || ".join(c for c in (hidden_by.get(key), LOCKED_BY.get(key)) if c) or None
             for kind, key, _, _, _ in items if kind != "group"]
    def target(row, column):
        return "#" + row[min(column, len(row) - 1)]
    def neighbour(i, step, column):
        j = i + step
        if j < 0: return first_nav_up
        return target(rows[j], column) if j < len(rows) else last_nav_down
    def past_lock(i, step, column):
        """(lock, where to go while it is on) when the next row that way is locked; None otherwise."""
        j = i + step
        if not (0 <= j < len(rows)) or locks[j] is None or locks[j] == locks[i]: return None
        lock = locks[j]
        while 0 <= j < len(rows) and locks[j] == lock: j += step
        return lock, neighbour(j - step, step, column) or "auto"
    def nav_style(i, column):
        up = None if past_lock(i, -1, column) else neighbour(i, -1, column)
        down = None if past_lock(i, 1, column) else neighbour(i, 1, column)
        return (f"nav-up: {up}; " if up else "") + (f"nav-down: {down};" if down else "")
    def nav_locked(i, column):
        attrs = ""
        for step, prop in ((-1, "nav-up"), (1, "nav-down")):
            skip = past_lock(i, step, column)
            if skip:
                attrs += f' data-style-{prop}="{skip[0]} ? \'{skip[1]}\' : \'{neighbour(i, step, column) or "auto"}\'"'
        return attrs
    out, helps, idx = [], [], 0
    for kind, key, title, extra, help_text in items:
        t = html.escape(title)
        if kind == "group":
            hide = GROUP_HIDDEN_BY.get(title)
            title_div = f'<div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">{t}</label></div>'
            out.append('                    ' + (f'<div data-if="!{hide}">{title_div}</div>' if hide else title_div))
            continue
        hide = hidden_by.get(key)
        if hide: out.append(f'                    <div data-if="!{hide}">')
        ev = f'data-event-blur="set_cur_config_index(-1)" data-event-focus="set_cur_config_index({idx})"'
        ids = rows[idx]
        lock = LOCKED_BY.get(key)
        locked_style = (' data-style-opacity="' + lock + " ? '0.4' : '1'" + '"') if lock else ''
        locked_attr = f' data-attrif-disabled="{lock}"' if lock else ''
        out.append(f'                    <div class="config-option jfg-option" data-event-mouseover="set_cur_config_index({idx})"{locked_style}>')
        out.append(f'                        <label class="config-option__title">{t}</label>')
        if kind == "bool":
            out.append('                        <div class="config-option__list">')
            for column, (v, label) in enumerate(((1, "On"), (0, "Off"))):
                rid = ids[column]
                out.append(f'                            <input type="radio" {ev} name="jfg_{key}" data-checked="{key}" value="{v}" id="{rid}" style="{nav_style(idx, column)}"{nav_locked(idx, column)}{locked_attr}/>')
                out.append(f'                            <label class="config-option__tab-label" for="{rid}">{label}</label>')
            out.append('                        </div>')
        elif kind == "choice":
            out.append('                        <div class="config-option__list">')
            for column, (v, label) in enumerate(extra):
                rid = ids[column]
                out.append(f'                            <input type="radio" {ev} name="jfg_{key}" data-checked="{key}" value="{v}" id="{rid}" style="{nav_style(idx, column)}"{nav_locked(idx, column)}{locked_attr}/>')
                out.append(f'                            <label class="config-option__tab-label" for="{rid}">{html.escape(label)}</label>')
            out.append('                        </div>')
        elif kind == "range":
            lo, hi, suffix = extra
            out.append('                        <div class="config-option__range-wrapper config-option__list">')
            # A suffix with words gets its own text node, apart from the value, so a language switch can change it
            # (jfg_locale.h "ui": the menus' text nodes are swapped in place).
            if any(c.isalpha() for c in suffix):
                suffix = f'<span>{suffix}</span>'
            out.append(f'                            <label class="config-option__range-label">{{{{{key}}}}}{suffix}</label>')
            out.append(f'                            <input class="nav-vert" {ev} id="{ids[0]}" type="range" min="{lo}" max="{hi}" style="flex: 1; margin: 0dp; {nav_style(idx, 0)}"{nav_locked(idx, 0)} data-value="{key}"{locked_attr}/>')
            out.append('                        </div>')
        out.append('                    </div>')
        if hide: out.append('                    </div>')
        # The focused option's description: its name as a heading, then the text (styled by .jfg-desc in config_menu.rml
        # and launcher.rml). data-if goes on a wrapper without its own display style (it hides with display: none).
        helps.append(f'                    <div data-if="cur_config_index == {idx}"><div class="jfg-desc__card">'
                     f'<div class="jfg-desc__title">{t}</div><p>{help_text}</p></div></div>')
        idx += 1
    return out, helps, idx

def first_id(items):
    kind, key, _, extra, _ = next(i for i in items if i[0] != "group")
    return row_ids(kind, key, extra)[0]

def options_tab(name, items, tab_id, extra_markup="", before_markup="", desc_markup="", first_nav_up=None, last_nav_down="none"):
    # With markup before the options, that markup's first element goes up to the tab instead (or the first option goes
    # up to first_nav_up).
    body, helps, _ = option_markup(items, first_nav_up or (None if before_markup else "#" + tab_id), last_nav_down)
    return "\n".join([
        f'<template name="config-menu__{name}">',
        '    <head>',
        '    </head>',
        '    <body>',
        '        <form class="config__form">',
        '            <div class="config__hz-wrapper">',
        '                <!-- Options (generated by tools/gen_jfg_config_rml.py) -->',
        '                <div class="config__wrapper" data-event-mouseout="set_cur_config_index(-1)" style="overflow-y: auto; height: 100%;">',
        before_markup,
        *body,
        extra_markup,
        '                </div>',
        '                <!-- Descriptions -->',
        '                <div class="config__wrapper jfg-desc">',
        *helps,
        *([desc_markup] if desc_markup else []),
        '                </div>',
        '            </div>',
        '        </form>',
        '    </body>',
        '</template>',
        ''])

# Pack buttons get ids (jfg_pack_0, ...) so the tabs / the launcher's sidebar can go down/right to the first one. Each
# is a card: the pack's image (pack_image.png), its name, version and author (credits.json), and whether it is a folder
# or an .rtz file. Focusing or pointing at one
# shows its details on the right (PACK_DETAILS).
TEXTURE_PACKS = '''                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">Texture Pack</label></div>
                    <div class="config-option jfg-option" style="flex: 0 0 auto;">
                        <label class="config-option__title jfg-pack-status">{{pack_status}}<span class="jfg-pack__variant" data-if="pack_status_variant != ''">{{pack_status_variant}}</span>{{pack_status_count}}</label>
                        <div class="jfg-note jfg-pack-join" data-if="pack_join_text != ''">{{pack_join_text}}</div>
                        <div class="jfg-pack-list">
                            <button class="button jfg-pack" data-for="p : packs" data-attr-id="'jfg_pack_' + it_index" data-style-nav-left="it_index == 0 ? '#tab_jfg_textures' : 'auto'" data-style-nav-up="it_index == 0 ? '#tab_jfg_textures' : 'auto'" data-event-click="set_pack(p.folder)" data-event-focus="view_pack(it_index)" data-event-mouseover="view_pack(it_index)" data-class-jfg-selected="p.selected">
                                <div class="jfg-pack__thumb" data-style-decorator="p.image != '' ? 'image(' + p.image + ' contain)' : 'none'"></div>
                                <div class="jfg-pack__text">
                                    <div class="jfg-pack__name">{{p.label}}<span class="jfg-pack__variant" data-if="p.variant != ''">{{p.variant}}</span></div>
                                    <div class="jfg-pack__meta">{{p.version}} {{p.author}}<span class="jfg-pack__size" data-if="p.size != ''">{{p.size}}</span></div>
                                </div>
                                <div class="jfg-pack__tags">
                                    <div class="jfg-pack__update" data-if="p.update">Update available</div>
                                    <div class="jfg-pack__kind" data-if="p.kind != ''">{{p.kind}}</div>
                                </div>
                            </button>
                        </div>
                        <div style="display: block; width: 100%;">
                            <button class="button" style="display: inline-block; margin: 8dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_textures;" data-event-click="reload_pack()">Reload Pack</button>
                            <button class="button" style="display: inline-block; margin: 8dp; nav-up: auto; nav-down: auto; nav-left: auto;" data-event-click="open_pack_folder()">Open Texture Packs Folder</button>
                        </div>
                        <div class="jfg-note" style="display: block; margin: 8dp 12dp 0dp; font-size: 18dp; line-height: 26dp;">Packs are the folders (or .rtz files) in the texture_packs folder next to the game. F6 switches packs while playing. Reload after adding or changing files. <span>{{dump_status}}</span></div>
                    </div>'''

# The Textures tab's right side while no option is focused: the pack last focused (at first the one in use), with its
# image, name, version, author, description and website (credits.json).
PACK_DETAILS = '''                    <div data-if="cur_config_index == -1"><div data-for="p : packs"><div data-if="it_index == pack_view">
                        <div class="jfg-pack-detail__image" data-if="p.image != ''" data-style-decorator="p.image != '' ? 'image(' + p.image + ' contain)' : 'none'"></div>
                        <div class="jfg-desc__card">
                            <div class="jfg-desc__title">{{p.label}}<span class="jfg-pack__variant" data-if="p.variant != ''">{{p.variant}}</span></div>
                            <div class="jfg-pack-detail__meta" data-if="p.version != '' || p.author != '' || p.size != ''">{{p.version}} {{p.author}}<span class="jfg-pack__size" data-if="p.size != ''">{{p.size}}</span></div>
                            <p data-if="p.description != ''">{{p.description}}</p>
                            <p data-if="p.no_details">This pack has no credits.json with its details.</p>
                            <button class="button jfg-pack-detail__site" data-if="p.website != ''" data-event-click="open_pack_website()">{{p.website}}</button>
                        </div>
                    </div></div></div>'''

PROFILES = '''<template name="config-menu__jfg_profiles">
    <head>
    </head>
    <body>
        <form class="config__form">
            <div class="config__hz-wrapper">
                <!-- Generated by tools/gen_jfg_config_rml.py. A scrolling box, as on the other tabs: Down on the tab goes
                     to the first profile button (jfg_profile_0, jfg_tab_first_ids), as automatic navigation does not
                     look into one from outside. -->
                <div class="config__wrapper" style="overflow-y: auto; height: 100%;">
                    <div style="display: block; margin: 16dp 12dp 0dp;">
                        <label class="config-option__title config-group__title" style="display: block; padding: 0dp;">Profile: <span>{{active_profile}}</span></label>
                        <div class="jfg-note" style="display: block; margin-top: 14dp; font-size: 20dp; line-height: 28dp;">{{profile_description}}</div>
                    </div>
                    <div style="display: block; margin: 32dp 12dp 0dp;">
                        <label class="config-option__title" style="display: block; padding: 0dp;">Choose a Profile</label>
                        <div style="display: block; width: 100%; margin-top: 18dp;">
                            <button class="button" style="display: inline-block; margin: 0dp 16dp 16dp 0dp; nav-up: auto; nav-down: auto; nav-left: auto; nav-right: auto;" data-for="p : profiles" data-attr-id="'jfg_profile_' + it_index" data-style-nav-left="it_index == 0 ? '#tab_jfg_profiles' : 'auto'" data-event-click="choose_profile(p.name)" data-class-jfg-selected="p.selected">{{p.name}}</button>
                        </div>
                    </div>
                    <div style="display: block; margin: 24dp 12dp 0dp;">
                        <label class="config-option__title" style="display: block; padding: 0dp;">Keep My Controls</label>
                        <div class="config-option__list" style="margin-top: 8dp;">
                                <input type="radio" name="jfg_keep_controls" data-checked="keep_controls" value="1" id="jfg_keep_controls_on" style="nav-up: auto; nav-down: auto; nav-left: #tab_jfg_profiles;"/>
                                <label class="config-option__tab-label" for="jfg_keep_controls_on">On</label>
                                <input type="radio" name="jfg_keep_controls" data-checked="keep_controls" value="0" id="jfg_keep_controls_off" style="nav-up: auto; nav-down: auto;"/>
                                <label class="config-option__tab-label" for="jfg_keep_controls_off">Off</label>
                        </div>
                        <div class="jfg-note" style="display: block; margin-top: 8dp; font-size: 18dp; line-height: 26dp;">On: choosing a profile leaves the Controls tab as it is (dual stick, button layout, mouse and keyboard, aiming).</div>
                    </div>
                    <!-- data-if hides with an inline display: none, so it is on an element without its own display style -->
                    <div data-if="profile_changed">
                        <div style="display: block; margin: 24dp 12dp 0dp;">
                            <div class="jfg-note" style="display: block; font-size: 20dp; line-height: 28dp;">Settings changed since choosing it. Built-in profiles are not changed; save your settings as your own profile to keep them.</div>
                            <button class="button" style="display: inline-block; margin: 16dp 0dp 0dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_profiles; nav-right: auto;" data-event-click="choose_profile(active_profile)">Go Back to This Profile</button>
                        </div>
                    </div>
                    <div style="display: block; margin: 32dp 12dp 24dp;">
                        <label class="config-option__title" style="display: block; padding: 0dp;">New Profile from the Current Settings</label>
                        <input type="text" class="nav-vert" style="display: block; width: 90%; max-width: 600dp; height: 52dp; margin: 18dp 0dp 0dp; padding: 10dp 14dp; font-size: 26dp; color: #f2f2f2; background-color: rgba(255,255,255,0.08); border-width: 1.1dp; border-color: rgba(233,205,53,0.8); border-radius: 12dp;" data-value="new_profile_name" maxlength="40"/>
                        <div style="display: block; width: 100%; margin-top: 16dp;">
                            <button class="button" style="display: inline-block; margin: 0dp 16dp 0dp 0dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_profiles; nav-right: auto;" data-event-click="save_profile()">Save as New Profile</button>
                            <span data-if="profile_deletable"><button class="button" style="display: inline-block; margin: 0dp; nav-up: auto; nav-down: auto; nav-left: auto; nav-right: auto;" data-event-click="delete_profile()">Delete This Profile</button></span>
                        </div>
                        <div class="jfg-note" style="display: block; margin-top: 14dp; font-size: 20dp; line-height: 28dp;">{{profile_message}}</div>
                    </div>
                </div>
                <!-- Description: a card like the other tabs' option descriptions (.jfg-desc in config_menu.rml), kept short
                     enough to fit without scrolling (nothing on this side takes the controller's focus). -->
                <div class="config__wrapper jfg-desc">
                    <div class="jfg-desc__card"><div class="jfg-desc__title">Profiles</div>
                        <p>Choosing one resets every setting to the original game, then applies the profile; you see what changes first.</p>
                        <p><b>Default</b>: the original game.<br/><b>Modern</b>: modern controls and visuals, 60 fps.<br/><b>Modern and Balanced</b>: plus rebalanced weapons.<br/><b>Extreme</b>: everything turned up.</p>
                        <p>The Modern ones use their own button layout.<br/>Turn on <b>Keep My Controls</b> to keep yours.</p>
                        <p>Your own are saved in the profiles folder. Not included: cheats, textures, volume, controller, display, bindings.</p></div>
                </div>
            </div>
        </form>
    </body>
</template>
'''

# The Language list (General tab): English and every locale file (locales/*.json), from the jfg_general_model's
# "locales" (src/ui/ui_config.cpp). Locale files are read as the game starts, so only the game's menu shows it (the
# launcher hides .jfg-ingame-only).
LANGUAGE_LIST = '''                    <div class="jfg-ingame-only">
                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">Language</label></div>
                    <div class="config-option jfg-option">
                        <div class="config-option__list" style="flex-wrap: wrap;">
                            <button class="button" data-for="l : locales" data-attr-id="'jfg_locale_' + it_index" data-event-click="set_locale(l.id)" data-class-jfg-selected="l.selected" style="display: inline-block; margin: 6dp; nav-up: auto; nav-down: auto;">{{l.name}}</button>
                        </div>
                    </div>
                    <div class="jfg-note" style="display: block; margin: 4dp 12dp 0dp; font-size: 18dp; line-height: 26dp;">Menus, subtitles and hints. More languages are files in the game's locales folder; text a language lacks is shown in English.</div>
                    </div>'''

# In a wrapper the launcher hides (it has no F10 or F9: it is the game's menu that has them).
GENERAL_NOTE = '''                    <div class="jfg-keys-note"><div class="jfg-note" style="display: block; margin: 32dp 12dp 16dp; font-size: 18dp; line-height: 26dp;">F10: the tools bar (cheats, level warp, items, weapons, texture packs). F9 or ` (backquote): the all-in-one debug window.</div></div>'''

# (name, items, extra markup after the options, markup before them). Each is assets/config_menu/<name>.rml, bound to the
# data model <name>_model, its tab is tab_<name>.
OPTION_TABS = (
    ("jfg_general", GENERAL, LANGUAGE_LIST + "\n" + GENERAL_NOTE, ""),
    ("jfg_gameplay", GAMEPLAY, "", ""),
    ("jfg_controls", CONTROLS, "", CONTROLS_TOP),
    ("jfg_graphics", GRAPHICS, "", ""),
    ("jfg_textures", TEXTURES, "", TEXTURE_PACKS),
)
# For a tab with markup before its options: the element Down on the tab goes to, the first option's Up, and its extra
# descriptions.
TAB_FIRST = {"jfg_textures": "jfg_pack_0", "jfg_controls": "jfg_qs_modern"}
TAB_FIRST_NAV_UP = {"jfg_controls": "#jfg_customize_buttons"}
TAB_DESC = {"jfg_textures": PACK_DETAILS, "jfg_controls": CONTROLS_DESC}
# Where Down on a tab's last option goes: on General, the Language list after the options (in the game's menu only; the
# launcher hides it, and there Down stays put).
TAB_LAST_NAV_DOWN = {"jfg_general": "#jfg_locale_0"}

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cm = os.path.join(root, "assets", "config_menu")
    for name, items, extra, before in OPTION_TABS:
        with open(os.path.join(cm, name + ".rml"), "w", newline="\n") as f:
            f.write(options_tab(name, items, "tab_" + name, extra_markup=extra, before_markup=before,
                                desc_markup=TAB_DESC.get(name, ""), first_nav_up=TAB_FIRST_NAV_UP.get(name),
                                last_nav_down=TAB_LAST_NAV_DOWN.get(name, "none")))
    with open(os.path.join(cm, "jfg_profiles.rml"), "w", newline="\n") as f:
        f.write(PROFILES)
    with open(os.path.join(root, "src", "jfg", "jfg_config_keys.inc"), "w", newline="\n") as f:
        f.write("// Generated by tools/gen_jfg_config_rml.py: the option keys each port tab of the config menu binds.\n")
        for name, items, _, _ in OPTION_TABS:
            keys = [k for kind, k, *_ in items if kind != "group" and k not in UI_ONLY_KEYS]
            f.write(f"static const char* const {name}_keys[] = {{ " + ", ".join(f'"{k}"' for k in keys) + " };\n")
        # The element Down on a tab goes to (the options are in a scrolling box; see ConfigTabsetListener): the first
        # option, or on the Textures tab the first texture pack button.
        f.write("struct JfgTabFirst { const char* tab; const char* first; };\n")
        f.write("static const JfgTabFirst jfg_tab_first_ids[] = {\n")
        f.write('    { "tab_jfg_profiles", "jfg_profile_0" },\n')
        for name, items, _, before in OPTION_TABS:
            f.write(f'    {{ "tab_{name}", "{TAB_FIRST.get(name) or first_id(items)}" }},\n')
        f.write("};\n")
        # Each option's name and how its value reads, for the list of what choosing a profile changes
        # (jfg_profiles.cpp). kind: 'b' On/Off, 'r' a number with suffix, 'c' one of choices ("value:label|...").
        f.write("struct JfgOptionTitle { const char* key; const char* title; char kind; const char* suffix; const char* choices; };\n")
        f.write("static const JfgOptionTitle jfg_option_titles[] = {\n")
        for _, items, _, _ in OPTION_TABS:
            for kind, key, title, extra, _ in items:
                if kind == "group" or key in UI_ONLY_KEYS: continue
                suffix = extra[2] if kind == "range" else ""
                choices = "|".join(f"{v}:{label}" for v, label in extra) if kind == "choice" else ""
                f.write(f'    {{ "{key}", "{title}", \'{kind[0]}\', "{suffix}", "{choices}" }},\n')
        f.write("};\n")

if __name__ == "__main__":
    main()
