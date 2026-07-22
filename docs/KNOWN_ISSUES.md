# Known issues and limitations

This list describes the current alpha, not the behavior of the original game.

## Compatibility scope

- Only the unmodified USA (En,Fr) Rev 1 ROM is accepted.
- macOS testing currently covers Apple silicon only.
- Steam Deck is the primary Linux target; other x86-64 Linux systems may work but are not part of the regular gameplay matrix.
- Windows has not yet received a complete DKR build and gameplay pass.

## Gameplay coverage

- Normal kart races and their results flow have been exercised, but full Adventure progression is not certified.
- Multiplayer, boss races, trophy races, every vehicle, every character, and every course still need structured regression coverage.
- EEPROM saves are supported. Controller Pak operations currently report that no Pak is connected.

## Rendering and performance

- Steam Deck defaults to the game's original refresh rate. RT64-generated frame interpolation can reuse dynamic racer display-list state incorrectly, producing duplicated or displaced racers.
- Steam Deck preloads the boot-through-Ancient-Lake specialized shader set and avoids splitting a draw batch when DKR repeats an unchanged geometry mode. Materials outside the preload table use the universal fallback.
- Authored 4:3 menu and post-race pages intentionally pillarbox even when gameplay uses widescreen.
- Player Select uses one music-clock snapshot for all dancers in a game frame. This removes the Deck-only pose hiccup without changing clouds or other scene animation.
- New RT64 or driver versions can invalidate assumptions around shader compilation, framebuffer overlap, or task completion. Re-test scene transitions and water-heavy attract scenes after dependency updates.

## Distribution

- macOS builds are ad-hoc signed development bundles, not notarized releases. Gatekeeper behavior can therefore differ between machines.
- The macOS package is currently arm64 rather than universal.
- Steam Deck users should receive the tarball containing the AppImage; some browsers and cloud providers remove the executable bit from a raw AppImage.

## Development status

- Some internal namespaces and inactive source directories still reflect the Dinosaur Planet application foundation. They are not used as DKR game data or generated into the DKR CPU translation.
- Dependency work is carried as patch files against dirty submodules until it is split into upstream contributions or pinned forks.
- Diagnostic animation and RT64 tracing paths remain in the source but are disabled by default.

See [DKR_MILESTONES.md](../DKR_MILESTONES.md) for the compatibility checklist and [TESTING.md](TESTING.md) for the regression plan.
