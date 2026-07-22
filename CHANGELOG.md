# Changelog

All notable changes to the public alpha are documented here. Versions before `0.1.17` were private development builds and are summarized rather than presented as supported releases.

## 0.1.17 - 2026-07-22

First publication-candidate source layout.

### Playability

- Boots the supported DKR USA Rev 1 ROM on Apple-silicon macOS and Steam Deck-class Linux.
- Supports frontend, attract mode, Tracks mode, Adventure startup, normal races, race completion, EEPROM saves, Xbox-compatible controls, and rumble.
- Implements ABI3 audio and F3DDKR XBUS, FIFO, and DRAM graphics microcodes.

### Correctness and stability

- Fixes close-up racer model stretching caused by incorrect custom-matrix identity changes.
- Holds the submitted graphics arena alive until RT64 completes synchronous display-list parsing.
- Bounds DKR DMA display lists and framebuffer copies to prevent invalid memory access.
- Uses a frame-coherent Player Select music clock so all dancers receive the same pose snapshot.
- Presents authored track-select and post-race pages at 4:3 while restoring the configured gameplay aspect ratio afterward.

### Steam Deck performance

- Preloads a bounded boot-through-Ancient-Lake specialized shader set.
- Keeps the universal shader as fallback and disables competing background compilation during gameplay.
- Avoids redundant geometry-mode batch splits, substantially reducing draw-call overhead in heavy scenes.

### Distribution

- Adds an ad-hoc signed macOS application bundle and a self-checking Steam Deck AppImage/tarball pipeline.
- Rejects ROM-like payloads during packaging and documents the private-data boundary.

### Known limitations

- Windows, full Adventure progression, multiplayer, bosses, every vehicle, every character, and every course remain incomplete test coverage.
- macOS builds are arm64 development bundles and are not notarized.
- Controller Pak support currently reports no Pak connected.
