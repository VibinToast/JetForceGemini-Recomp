# Known issues

Problems we know about in the current release. Items marked **Fix planned** are on the [Roadmap](ROADMAP.md).
Found something that isn't listed here? See [Reporting bugs](README.md#reporting-bugs).

## Stability

- **Rare crash or freeze with Frame Interpolation on.** Several causes were fixed in 1.2 and none have been seen
  since, but if it happens to you, please report it with your `crash.log`. Turning Frame Interpolation off avoids it.
- **Game Frame Rate 60 is experimental:** some enemy groups and bosses can behave differently than at 30 (for
  example a wave counts as cleared one enemy early). If something seems off, try 30.

## Cutscenes and camera

- **The camera can move during some cutscenes.** With dual-stick controls the camera can sometimes be turned during
  a door-unlock cutscene, and during the Mizar boss intro before the fight starts.
- **The screen can shake while walking over uneven ground with Frame Interpolation on**, for example at the entrance of
  Mizar's Palace. **Fix planned.**
- **One cutscene can't be skipped:** the one in level 398 (the level number is shown with Show Level on, in the General tab).

## Graphics

- **Shadows look wrong in the intro cutscene of the Tribals level.** **Fix planned.**
- **The Floyd screen just before a Floyd mission has a visual glitch.** **Fix planned.**
- **With level of detail turned off, ants in cutscenes still use their low-detail models.**
- **Uneven frame times with Frame Interpolation on a high-end GPU when anti-aliasing (MSAA) is off.** Keep MSAA on (2x is the default), or set NVIDIA's "Power management mode" to "Prefer maximum performance" for the game.

## Menus and launcher

- **In the launcher, pressing LB right after RB shows the previous page for about a second.** **Fix planned.**

## Steam Deck

- **The touchscreen doesn't work in the settings menu.** Use the Deck's controls instead. **Fix planned.**
- **The Modern profiles turn on Mouse and Keyboard Controls.** After choosing one, turn Mouse and Keyboard Controls off
  again in the Controls tab (see [Steam Deck](README.md#steam-deck)).
