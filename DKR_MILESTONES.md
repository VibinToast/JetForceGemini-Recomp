# Diddy Kong Racing: Recompiled milestones

This fork is being converted from the proven Dinosaur Planet recomp runtime
into a native Diddy Kong Racing port. The matching DKR decompilation provides
symbols and reverse-engineering context, but the shipped application must load
game data from a user-supplied, verified ROM.

## Compatibility milestones

- [x] Generate all 1,959 CPU functions for the US Rev 1 ROM.
- [x] Compile the generated CPU code as a native Apple-silicon application.
- [x] Enter DKR's original `entrypoint` through N64ModernRuntime.
- [x] Reach libultra/thread initialization without an unhandled call.
- [x] Run the custom `aspMain` audio microcode and feed CoreAudio through SDL.
- [x] Support the F3DDKR XBUS, FIFO, and DRAM graphics microcodes.
- [x] Display the boot/logo and full attract sequences with correct scene flow.
- [x] Reach the title screen with keyboard input, controller plumbing, and audio output active.
- [x] Support a basic Xbox-compatible controller layout and rumble.
- [x] Create an Adventure save, pass the retail hardware checks, and enter the island hub.
- [x] Enter and drive a live eight-racer Ancient Lake race from Tracks mode.
- [x] Complete a race and reach the Race Times and Race Order flow.
- [x] Correct close-up racer rendering for large attached character groups.
- [x] Package and smoke-test a Steam Deck x86-64 AppImage.
- [x] Precompile the bounded boot-through-Ancient-Lake shader set during Steam Deck launcher preflight.
- [ ] Validate save persistence and progression through a structured Adventure test.
- [ ] Validate Adventure, Tracks, multiplayer, bosses, and progression.
- [x] Package a self-contained arm64 macOS application that launches from Finder.
- [ ] Sign and notarize a distributable universal macOS application.

## Legal boundary

ROMs, extracted assets, and generated source derived from a private ROM are
local inputs and must never be committed or included in a source archive. A
native executable necessarily contains translated game instructions, while
retail data is loaded from the user's verified ROM at first launch. Every
source and binary package must be inspected independently before publication.
