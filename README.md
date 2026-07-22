# Diddy Kong Racing: Recompiled

An experimental native port of the Nintendo 64 version of **Diddy Kong Racing**, built with [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) and [RT64](https://github.com/rt64/rt64).

> [!IMPORTANT]
> This project is an early alpha. It is not affiliated with Nintendo, Rare, or Microsoft, and it is not an emulator. A legally obtained, unmodified game ROM is required. No ROM, extracted game asset, or other proprietary game data belongs in this repository or its releases.

## Current status

The port boots, reaches the frontend and Adventure mode, and can complete normal races. The most thoroughly exercised targets are:

| Platform | Status | Renderer |
| --- | --- | --- |
| macOS on Apple silicon | Playable alpha | Metal |
| Steam Deck / Linux x86-64 | Playable alpha | Vulkan |
| Windows x86-64 | Inherited build support; not yet validated for DKR | Direct3D 12 |

Only **Diddy Kong Racing USA (En,Fr) Rev 1**, decomp version `us.v80`, is accepted. The normalized big-endian ROM has SHA-1 `6d96743d46f8c0cd0edb0ec5600b003c89b93755`.

Validated functionality includes:

- boot, attract mode, frontend menus, Tracks mode, and Adventure startup;
- the custom ABI3 audio microcode;
- the F3DDKR XBUS, FIFO, and DRAM graphics microcodes;
- eight-racer gameplay and post-race results;
- EEPROM save data;
- basic Xbox-compatible controller input and rumble;
- widescreen gameplay with automatic 4:3 presentation for authored menu and results pages;
- macOS application bundles and Steam Deck AppImage packaging.

Full Adventure progression, multiplayer, bosses, every vehicle, and every course have not yet been exhaustively tested. See [Known issues](docs/KNOWN_ISSUES.md) and [milestones](DKR_MILESTONES.md) before treating a build as release-ready.

## Playing a development build

1. Start the application.
2. Choose **Select ROM** in the launcher.
3. Select an unmodified USA Rev 1 ROM matching the SHA-1 above.
4. Configure controls if desired, then choose **Start Game**.

The runtime validates the selected file and stores a normalized private copy in the platform user-data directory shown below. That copy remains local and outside the application bundle; it must never be included when sharing logs, saves, or configuration archives.

### Default Xbox-style controls

| N64 input | Default modern input |
| --- | --- |
| Control Stick | Left stick |
| A | A / south face button |
| B | B or X / east or west face button |
| Start | Menu / Start |
| Z | Left trigger |
| R | Right trigger |
| L | Left shoulder |
| C buttons | Right stick, with selected D-pad alternatives |
| Recomp menu | View / Back |

All mappings can be changed in the launcher. Steam Input should use a normal gamepad layout rather than a keyboard-emulating Desktop layout.

## User data and logs

| Platform | Default directory |
| --- | --- |
| macOS | `~/Library/Application Support/DiddyKongRacingRecompiled` |
| Linux / Steam Deck | `~/.config/DiddyKongRacingRecompiled` |
| Windows | `%LOCALAPPDATA%\DiddyKongRacingRecompiled` |

The Steam Deck AppImage writes `runtime.log`, `runtime.previous.log`, and `last-exit.log` to that directory. Native crash handling may also create `crash.log` there. Save and configuration data should be backed up before testing experimental builds.

The directory also contains the locally stored verified ROM used for later launches. Treat the entire directory as private unless its contents have been inspected carefully.

## Documentation

- [Building from source](BUILDING.md)
- [Architecture and patch boundaries](docs/ARCHITECTURE.md)
- [Testing and release checks](docs/TESTING.md)
- [Known issues](docs/KNOWN_ISSUES.md)
- [Contributing and upstream strategy](CONTRIBUTING.md)
- [Upstream contribution map](docs/UPSTREAMING.md)
- [AI-assisted development disclosure](docs/AI_ASSISTANCE.md)
- [Release history](CHANGELOG.md)
- [Compatibility milestones](DKR_MILESTONES.md)

## Relationship to the decompilation

[DavidSM64/Diddy-Kong-Racing](https://github.com/DavidSM64/Diddy-Kong-Racing) is the matching decompilation and the source of symbols, structure information, and essential reverse-engineering knowledge. This native recomp is a separate application: most retail CPU instructions are translated automatically from the user's ROM, while selected hardware-facing behavior is implemented by the native runtime.

Corrections that improve the matching decompilation should be submitted there as focused changes. Runtime, renderer, and packaging work belongs in this project or its respective upstream dependency.

The complete native port is intentionally maintained as a separate project. See the [upstream contribution map](docs/UPSTREAMING.md) for the pieces that can be proposed to the decompilation, RT64, N64Recomp, N64ModernRuntime, and Plume without asking any maintainer to review a monolithic conversion.

## Development provenance

This codebase began as a GPLv3-derived adaptation of [Dinosaur Planet: Recompiled](https://github.com/DinosaurPlanetRecomp/dino-recomp), which itself builds on Zelda64Recomp, N64Recomp, N64ModernRuntime, and RT64. The DKR conversion has been developed with substantial assistance from OpenAI Codex under human direction and gameplay testing. See [AI-assisted development](docs/AI_ASSISTANCE.md) for the disclosure and review policy.

## Credits

- [DavidSM64 and DKR decomp contributors](https://github.com/DavidSM64/Diddy-Kong-Racing) for the matching decompilation and documentation.
- [Dinosaur Planet: Recompiled](https://github.com/DinosaurPlanetRecomp/dino-recomp) for the application/runtime foundation used by this fork.
- [N64: Recompiled](https://github.com/N64Recomp/N64Recomp) for static recompilation.
- [N64ModernRuntime](https://github.com/N64Recomp/N64ModernRuntime) for libultra and host-runtime services.
- [RT64](https://github.com/rt64/rt64) and Plume for graphics translation and platform rendering.
- [Zelda64Recomp](https://github.com/Zelda64Recomp/Zelda64Recomp) for the project lineage and UI/runtime foundations.
- RmlUi, SDL, FreeType, lunasvg, SDL_GameControllerDB, and the other third-party projects retained in the source tree.

## License

The native project code is distributed under the GNU General Public License v3.0; see [COPYING](COPYING). Submodules and bundled third-party components retain their own licenses. The license does not grant rights to Diddy Kong Racing, its ROM, or its assets.
