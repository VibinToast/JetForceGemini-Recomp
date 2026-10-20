#!/usr/bin/env python3
"""Generates the port's tabs of the "Select" config menu (assets/config_menu/jfg_*.rml) and the list of option keys
each tab binds (src/jfg/jfg_config_keys.inc). Run from the repository root after changing the lists below:
    python3 tools/gen_jfg_config_rml.py
The options themselves (ranges, defaults, saving) live in src/jfg/jfg_options.h and jfg_options.cpp (apply_option / options_text)."""
import html, os

# (kind, key, title, extra, help). kind: group | bool | range (extra = (min, max, suffix)) | choice (extra = [(value, label)])
# Keys are the port's options (jfg::settings) except the ones in UPSTREAM_KEYS (ui_config.cpp: volume, controller and
# window settings of the recomp itself), which the port's tabs show too so that each setting is in one place.
# General: volume, the menu, what is shown on screen and the fly camera. A note about F9 follows the options.
GENERAL = [
    ("group", None, "Sound", None, None),
    ("range", "main_volume", "Master Volume", (0, 100, "%"), "The volume of all of the game's sound."),
    ("group", None, "Menu & Cutscenes", None, None),
    ("bool", "pause_in_menu", "Pause in Settings Menu", None,
     "The game stands still while the settings menu (Select / Esc) is open during play, as on the pause screen. Cutscenes keep playing."),
    ("bool", "start_skips_cutscenes", "START Skips Cutscenes", None,
     "START skips cutscenes, the logo screens at start-up and the story intro after creating a file. A skipped cutscene still runs to its end at full speed, so everything it does (flags, items, the next level) still happens."),
    ("group", None, "On Screen", None, None),
    ("bool", "show_fps", "Show FPS", None, "The game's frame rate in the top right corner."),
    ("bool", "show_level", "Show Level", None, "The level number and name in the top right corner."),
    ("bool", "show_pack", "Show Texture Pack", None, "The texture pack in use in the top right corner."),
    ("bool", "save_notification", "Show Save Notification", None, "\"Game saved\" shows briefly in the bottom right corner after an auto save."),
    ("group", None, "Fly Camera (F7)", None, None),
    ("range", "fly_speed", "Fly Speed", (10, 1000, "%"),
     "F7 (or F10 > Tools) flies a free camera for screenshots: WASD / left stick move where it looks, mouse / right stick look, Space / A up, Ctrl / B down, holding fire flies faster. F7 again ends it."),
    ("bool", "fly_freeze", "Freeze While Flying", None, "The game stands still while the fly camera is on (enemies, effects and the level stop as when paused)."),
    ("bool", "fly_hide_hud", "Hide HUD While Flying", None, "The health meter and the rest of the HUD are hidden while the fly camera is on."),
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
    ("group", None, "Gore", None, None),
    ("bool", "no_blood", "No Blood", None, "No blood, as in the Japanese version."),
    ("bool", "gore_keep_blood", "Blood Stays", None,
     "Blood splats stay on the ground and walls instead of fading away. The newest 40 are kept; older ones still fade so new blood always has room."),
    ("bool", "gore_keep_limbs", "Body Parts Stay", None,
     "Severed limbs and heads stay where they fell instead of fading away. The newest 24 are kept, so the game does not run out of memory in big fights."),
    ("range", "blood_amount", "Blood Amount", (100, 400, "%"), "How much blood a hit sprays. 100% is the original game."),
    ("group", None, "Player", None, None),
    ("range", "damage_taken", "Damage Taken", (0, 400, "%"),
     "How much damage the character takes from enemies, falls and explosions. 0% takes none; the Invulnerable cheat overrides it."),
    ("bool", "unlimited_continues", "Unlimited Continues", None,
     "Dying never uses up your continues, so you always come back where you died instead of the game over that restarts the area from its entrance."),
    ("range", "refuel_speed", "Jetpack Refuel Speed", (25, 1000, "%"), "How fast a fuel pad fills the jetpack."),
    ("range", "dive_speed", "Vela's Dive Speed", (50, 400, "%"), "How fast Vela swims underwater, in every direction."),
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
]

# The port's control options; the button bindings are the Bindings tab (controls.rml).
CONTROLS = [
    ("group", None, "Dual Stick", None, None),
    ("bool", "dual_stick", "Dual-Stick Controls", None,
     "Right stick: moves the camera around the character. Left stick: runs in the pushed direction as seen from the camera. While the aim button is held the right stick aims and the left stick moves, as in a first-person game. Menus, cutscenes, Floyd missions and the races keep the normal layout."),
    ("bool", "dual_layout", "Built-in Button Layout", None,
     "LT aim, RT fire. A jump, B or left-stick click crouch. RB / Y next weapon, LB / X previous weapon. D-pad: weapon select; up/down zoom while aiming. View / right-stick click: N64 L. Off: your own controller bindings."),
    ("bool", "swap_triggers", "Swap Triggers", None, "RT aims and LT fires."),
    ("bool", "invert_aim_y", "Invert Aim Up/Down", None, "Dual stick: right stick up aims down (the game's original flight-stick style)."),
    ("bool", "invert_camera_y", "Invert Camera Up/Down", None, "Dual stick: right stick up lowers the camera instead of raising the view."),
    ("bool", "aim_from_character", "Aim Where the Character Faces", None, "On aiming, the view turns to the character instead of the character to the view."),
    ("range", "camera_speed_h", "Camera Left/Right", (10, 400, "%"), "Dual stick: how fast the right stick turns the camera sideways."),
    ("range", "camera_speed_v", "Camera Up/Down", (10, 400, "%"), "Dual stick: how fast the right stick moves the camera up and down."),
    ("bool", "camera_collision", "Camera Collision", None, "Dual stick: the camera stays inside the level (the game's own wall check) instead of going through walls."),
    ("group", None, "Mouse & Keyboard", None, None),
    ("bool", "mouse_keyboard", "Mouse and Keyboard Controls", None,
     "Plays like the dual-stick controls: WASD run, the mouse turns the camera, hold the right button to aim and click the left to fire, the wheel changes weapon. Space jumps, Left Ctrl or C crouches, Enter is START, Tab is N64 L, 1-4 are the D-pad. The cursor is captured during gameplay only."),
    ("range", "mouse_camera_speed", "Mouse Camera Speed", (10, 400, "%"), "How fast the mouse turns the camera."),
    ("range", "mouse_aim_speed", "Mouse Aim Speed", (10, 400, "%"), "How fast the mouse aims."),
    ("bool", "invert_mouse_y", "Invert Mouse Up/Down", None, "Mouse forward looks down."),
    ("group", None, "Aiming (any control layout)", None, None),
    ("bool", "aim_shoulder", "Over-the-Shoulder Aiming", None, "While aiming, the view is from over the character's right shoulder."),
    ("bool", "aim_centered", "Keep the Reticle Centered", None,
     "While aiming, the view turns as soon as the stick moves and the reticle stays in the middle (instead of the reticle first travelling to the edge of a box and drifting back)."),
    ("bool", "aim_jump", "Jump While Aiming", None,
     "The jump button jumps while aiming, also while walking or strafing: the character leaves the aim for the jump and aims again on landing if the aim button is still held. The sniper scope then zooms with the D-pad (dual stick) or 1 / 3 (mouse and keyboard)."),
    ("range", "aim_speed", "Aim Speed", (10, 300, "%"), "How fast the stick turns the view / moves the reticle while aiming."),
    ("bool", "sniper_fixed_zoom", "Sniper Fixed Zoom", None,
     "The Sniper Rifle's scope stays at the zoom set below; the zoom buttons do nothing."),
    ("range", "sniper_zoom", "Sniper Zoom", (100, 500, "%"), "The fixed zoom of the Sniper Rifle: 100% is no zoom, 500% the game's closest."),
    ("bool", "invert_aim_normal", "Invert Aim (Original Controls)", None, "Original controls only: flip the stick's up/down while aiming (R held)."),
    ("group", None, "Controller", None, None),
    ("range", "joystick_deadzone", "Joystick Deadzone", (0, 100, "%"),
     "Applies a deadzone to joystick inputs.<br/><br/><b>Note: the game may apply a small additional deadzone of its own.</b>"),
    ("range", "joystick_range", "Joystick Range", (0, 100, "%"),
     "Sets the input range the joystick is mapped to. A real N64 controller usually does not reach the full technical range.<br/><br/>Values above 62% increase joystick sensitivity. <i>Warning:</i> below 62% the stick can no longer reach its maximum values, which may hinder gameplay. If your joystick cannot reach 100% in the cardinal directions, set this above 62% to compensate."),
    ("range", "rumble_strength", "Rumble Strength", (0, 100, "%"),
     "The strength of rumble on a controller that supports it. <b>Zero turns rumble off.</b> Jet Force Gemini supports the Rumble Pak."),
    ("bool", "background_input", "Background Input", None,
     "The game reads the controller while its window is not in focus. <b>Keyboard input is not affected.</b>"),
]

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
    ("choice", "frame_rate", "Frame Rate", [(0, "Original"), (1, "60"), (2, "60 + Interpolated")],
     "<b>Original</b>: 30, dropping to 20 like the N64. <b>60</b>: the game updates every video frame; game speed stays the same. <b>60 + Interpolated</b>: the renderer also adds in-between frames up to your monitor's refresh rate; some moving objects may look off."),
    ("group", None, "Picture", None, None),
    ("bool", "widescreen", "Widescreen", None, "During gameplay the 3D view fills the window; the HUD stays in the middle. Things at the far left and right may appear late."),
    ("range", "fov", "Field of View", (75, 140, "%"),
     "Widens or narrows the view during gameplay, relative to each level's own field of view. Cutscenes, menus, the pause screen and zoomed scopes keep the game's own."),
    ("choice", "texture_filter", "Texture Filtering", [(0, "N64 Three-Point"), (1, "Bilinear")],
     "<b>N64 Three-Point</b>: how the N64 smooths textures. <b>Bilinear</b>: smoother, as on PC."),
    ("choice", "output_filter", "Picture Scaling", [(0, "Anti-Aliased"), (1, "Linear"), (2, "Nearest")],
     "How the picture is scaled to the window. <b>Anti-Aliased</b>: sharp pixels with smooth edges. <b>Linear</b>: softer. <b>Nearest</b>: sharp pixels."),
    ("range", "hit_flash", "Hit Flash Opacity", (0, 100, "%"),
     "How strongly the screen turns red when your character is hit. 100% is the original game; 0% turns the red flash off."),
    ("group", None, "Objects", None, None),
    ("bool", "no_lod", "Disable LOD", None, "Characters and objects keep their full-detail model at any distance."),
    ("bool", "far_objects", "Draw Distant Objects", None, "Objects no longer fade out and disappear beyond their draw distance. Level scenery itself is not affected."),
    ("bool", "hide_floyd", "Hide Floyd", None,
     "Floyd, his shadow and his sounds are gone while he follows your character; he still flies along and shoots. Floyd missions and a second player flying him show him as usual."),
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
     "Saves every texture the game draws, the first time it appears, to bin\\texture_dump as &lt;hash&gt;.png. Off again at the next start. Upscale the files without renaming them and put them in a pack folder (bin\\texture_packs\\&lt;name&gt;)."),
    ("bool", "dump_by_level", "One Folder per Level", None, "Dumps into bin\\texture_dump\\&lt;level number&gt; &lt;level name&gt; instead."),
]

def row_ids(kind, key, extra):
    """The focusable ids of one option row, left to right."""
    if kind == "bool": return [f"jfg_{key}_on", f"jfg_{key}_off"]
    if kind == "choice": return [f"jfg_{key}_{v}" for v, _ in extra]
    return [f"jfg_{key}_input"]

def option_markup(items, first_nav_up):
    # Up/Down are given explicitly for every control: automatic navigation skipped the sliders below a row of radio
    # buttons (and some rows above), and at the last row Down went sideways between its radios. Between two rows of
    # radios the column is kept (On to On); a slider row has one control. The first row goes up to first_nav_up (the
    # tab; None leaves it automatic), the last row's Down goes nowhere.
    rows = [row_ids(kind, key, extra) for kind, key, _, extra, _ in items if kind != "group"]
    def target(row, column):
        return "#" + row[min(column, len(row) - 1)]
    def nav_style(i, column):
        up = target(rows[i - 1], column) if i > 0 else first_nav_up
        down = target(rows[i + 1], column) if i + 1 < len(rows) else "none"
        return (f"nav-up: {up}; " if up else "") + f"nav-down: {down};"
    out, helps, idx = [], [], 0
    for kind, key, title, extra, help_text in items:
        t = html.escape(title)
        if kind == "group":
            out.append(f'                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">{t}</label></div>')
            continue
        ev = f'data-event-blur="set_cur_config_index(-1)" data-event-focus="set_cur_config_index({idx})"'
        ids = rows[idx]
        out.append(f'                    <div class="config-option" data-event-mouseover="set_cur_config_index({idx})">')
        out.append(f'                        <label class="config-option__title">{t}</label>')
        if kind == "bool":
            out.append('                        <div class="config-option__list">')
            for column, (v, label) in enumerate(((1, "On"), (0, "Off"))):
                rid = ids[column]
                out.append(f'                            <input type="radio" {ev} name="jfg_{key}" data-checked="{key}" value="{v}" id="{rid}" style="{nav_style(idx, column)}"/>')
                out.append(f'                            <label class="config-option__tab-label" for="{rid}">{label}</label>')
            out.append('                        </div>')
        elif kind == "choice":
            out.append('                        <div class="config-option__list">')
            for column, (v, label) in enumerate(extra):
                rid = ids[column]
                out.append(f'                            <input type="radio" {ev} name="jfg_{key}" data-checked="{key}" value="{v}" id="{rid}" style="{nav_style(idx, column)}"/>')
                out.append(f'                            <label class="config-option__tab-label" for="{rid}">{html.escape(label)}</label>')
            out.append('                        </div>')
        elif kind == "range":
            lo, hi, suffix = extra
            out.append('                        <div class="config-option__range-wrapper config-option__list">')
            out.append(f'                            <label class="config-option__range-label">{{{{{key}}}}}{suffix}</label>')
            out.append(f'                            <input class="nav-vert" {ev} id="{ids[0]}" type="range" min="{lo}" max="{hi}" style="flex: 1; margin: 0dp; {nav_style(idx, 0)}" data-value="{key}"/>')
            out.append('                        </div>')
        out.append('                    </div>')
        helps.append(f'                    <p data-if="cur_config_index == {idx}">{help_text}</p>')
        idx += 1
    return out, helps, idx

def first_id(items):
    kind, key, _, extra, _ = next(i for i in items if i[0] != "group")
    return row_ids(kind, key, extra)[0]

def options_tab(name, items, tab_id, extra_markup="", before_markup=""):
    # With markup before the options, that markup's first element goes up to the tab instead.
    body, helps, _ = option_markup(items, None if before_markup else "#" + tab_id)
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
        '                <div class="config__wrapper">',
        *helps,
        '                </div>',
        '            </div>',
        '        </form>',
        '    </body>',
        '</template>',
        ''])

# Pack buttons get ids (jfg_pack_0, ...) so the tabs / the launcher's sidebar can go down/right to the first one.
TEXTURE_PACKS = '''                    <div class="config-option" style="margin: 24dp 0dp 0dp;"><label class="config-option__title config-group__title">Texture Pack</label></div>
                    <div class="config-option" style="flex: 0 0 auto;">
                        <label class="config-option__title">{{pack_status}}</label>
                        <div style="display: block; width: 100%;">
                            <button class="button" style="display: inline-block; margin: 8dp; nav-up: auto; nav-down: auto; nav-left: auto; nav-right: auto;" data-for="p : packs" data-attr-id="'jfg_pack_' + it_index" data-style-nav-left="it_index == 0 ? '#tab_jfg_textures' : 'auto'" data-style-nav-up="it_index == 0 ? '#tab_jfg_textures' : 'auto'" data-event-click="set_pack(p.folder)" data-class-jfg-selected="p.selected">{{p.label}}</button>
                        </div>
                        <div style="display: block; width: 100%;">
                            <button class="button" style="display: inline-block; margin: 8dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_textures;" data-event-click="reload_pack()">Reload Pack</button>
                        </div>
                        <div class="jfg-note" style="display: block; margin: 8dp 12dp 0dp; font-size: 18dp; line-height: 26dp;">Packs are the folders in bin\\texture_packs. F6 switches packs while playing. Reload after adding or changing files. {{dump_status}}</div>
                    </div>'''

PROFILES = '''<template name="config-menu__jfg_profiles">
    <head>
    </head>
    <body>
        <form class="config__form">
            <div class="config__hz-wrapper">
                <!-- Generated by tools/gen_jfg_config_rml.py. No scrolling box here: automatic controller navigation
                     does not look into one from the tabs, and this page is short. -->
                <div class="config__wrapper">
                    <div style="display: block; margin: 16dp 12dp 0dp;">
                        <label class="config-option__title config-group__title" style="display: block; padding: 0dp;">Profile: {{active_profile}}</label>
                        <div class="jfg-note" style="display: block; margin-top: 14dp; font-size: 20dp; line-height: 28dp;">{{profile_description}}</div>
                    </div>
                    <div style="display: block; margin: 40dp 12dp 0dp;">
                        <label class="config-option__title" style="display: block; padding: 0dp;">Choose a Profile</label>
                        <div style="display: block; width: 100%; margin-top: 18dp;">
                            <button class="button" style="display: inline-block; margin: 0dp 16dp 16dp 0dp; nav-up: auto; nav-down: auto; nav-left: auto; nav-right: auto;" data-for="p : profiles" data-style-nav-left="it_index == 0 ? '#tab_jfg_profiles' : 'auto'" data-event-click="choose_profile(p.name)" data-class-jfg-selected="p.selected">{{p.name}}</button>
                        </div>
                    </div>
                    <!-- data-if hides with an inline display: none, so it is on an element without its own display style -->
                    <div data-if="profile_changed">
                        <div style="display: block; margin: 24dp 12dp 0dp;">
                            <div class="jfg-note" style="display: block; font-size: 20dp; line-height: 28dp;">Settings changed since choosing it. Built-in profiles are not changed; save your settings as your own profile to keep them.</div>
                            <button class="button" style="display: inline-block; margin: 16dp 0dp 0dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_profiles; nav-right: auto;" data-event-click="choose_profile(active_profile)">Go Back to This Profile</button>
                        </div>
                    </div>
                    <div style="display: block; margin: 40dp 12dp 0dp;">
                        <label class="config-option__title" style="display: block; padding: 0dp;">New Profile from the Current Settings</label>
                        <input type="text" class="nav-vert" style="display: block; width: 90%; max-width: 600dp; height: 52dp; margin: 18dp 0dp 0dp; padding: 10dp 14dp; font-size: 26dp; color: #f2f2f2; background-color: rgba(255,255,255,0.08); border-width: 1.1dp; border-color: rgba(233,205,53,0.8); border-radius: 12dp;" data-value="new_profile_name" maxlength="40"/>
                        <div style="display: block; width: 100%; margin-top: 16dp;">
                            <button class="button" style="display: inline-block; margin: 0dp 16dp 0dp 0dp; nav-up: auto; nav-down: auto; nav-left: #tab_jfg_profiles; nav-right: auto;" data-event-click="save_profile()">Save as New Profile</button>
                            <span data-if="profile_deletable"><button class="button" style="display: inline-block; margin: 0dp; nav-up: auto; nav-down: auto; nav-left: auto; nav-right: auto;" data-event-click="delete_profile()">Delete This Profile</button></span>
                        </div>
                        <div class="jfg-note" style="display: block; margin-top: 14dp; font-size: 20dp; line-height: 28dp;">{{profile_message}}</div>
                    </div>
                </div>
                <div class="config__wrapper">
                    <p>A profile is a whole set of the port's settings (the General, Gameplay, Controls and Graphics tabs, and the weapon settings of the F10 tools). Choosing one first puts every setting back to the original game, then applies the profile.</p>
                    <p><b>Default</b>: the original game. <b>Modern</b>: modern controls and visuals at 60 fps. <b>Modern and Balanced</b>: Modern with rebalanced weapons (a subjective take on the balance). <b>Extreme</b>: everything turned up, just for fun.</p>
                    <p>Your own profiles are kept in bin\\profiles; changes made while one is active are saved to it. Not part of profiles: cheats, the Textures tab (texture pack, dumping), the volume, the Controller settings, the window, resolution, downsampling and anti-aliasing, and the button bindings.</p>
                    <p>Controller: Up/Down and Left/Right move between buttons, A chooses.</p>
                </div>
            </div>
        </form>
    </body>
</template>
'''

GENERAL_NOTE = '''                    <div class="jfg-note" style="display: block; margin: 32dp 12dp 16dp; font-size: 18dp; line-height: 26dp;">F10: the tools bar (cheats, level warp, items, weapons, texture packs). F9 or ` (backquote): the all-in-one debug window.</div>'''

# (name, items, extra markup after the options, markup before them). Each is assets/config_menu/<name>.rml, bound to the
# data model <name>_model, its tab is tab_<name>.
OPTION_TABS = (
    ("jfg_general", GENERAL, GENERAL_NOTE, ""),
    ("jfg_gameplay", GAMEPLAY, "", ""),
    ("jfg_controls", CONTROLS, "", ""),
    ("jfg_graphics", GRAPHICS, "", ""),
    ("jfg_textures", TEXTURES, "", TEXTURE_PACKS),
)

def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    cm = os.path.join(root, "assets", "config_menu")
    for name, items, extra, before in OPTION_TABS:
        with open(os.path.join(cm, name + ".rml"), "w", newline="\n") as f:
            f.write(options_tab(name, items, "tab_" + name, extra_markup=extra, before_markup=before))
    with open(os.path.join(cm, "jfg_profiles.rml"), "w", newline="\n") as f:
        f.write(PROFILES)
    with open(os.path.join(root, "src", "jfg", "jfg_config_keys.inc"), "w", newline="\n") as f:
        f.write("// Generated by tools/gen_jfg_config_rml.py: the option keys each port tab of the config menu binds.\n")
        for name, items, _, _ in OPTION_TABS:
            keys = [k for kind, k, *_ in items if kind != "group"]
            f.write(f"static const char* const {name}_keys[] = {{ " + ", ".join(f'"{k}"' for k in keys) + " };\n")
        # The element Down on a tab goes to (the options are in a scrolling box; see ConfigTabsetListener): the first
        # option, or on the Textures tab the first texture pack button.
        f.write("struct JfgTabFirst { const char* tab; const char* first; };\n")
        f.write("static const JfgTabFirst jfg_tab_first_ids[] = {\n")
        for name, items, _, before in OPTION_TABS:
            f.write(f'    {{ "tab_{name}", "{"jfg_pack_0" if before else first_id(items)}" }},\n')
        f.write("};\n")

if __name__ == "__main__":
    main()
