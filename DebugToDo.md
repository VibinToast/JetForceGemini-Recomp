## Debug tools to add
In order of value for the effort. Existing switches: Recomp-Docs/REFERENCE.md; how testing works: Recomp-Docs/TESTING.md.

- Scripted test timeline (one file instead of one-shot switches) (medium)
    - e.g. warp 92 -> wait for player -> teleport x,y,z facing -> hold forward 2s -> screenshot -> warp 21 entrance 1 -> screenshot -> quit
    - wait on events (player exists, level loaded, N ticks after arrival) instead of wall-clock seconds
- Renderer screenshots (small-medium)
    - RT64 saves the frame as PNG at a tick or event; works with the game window in the background (replaces window capture)
- Input record & replay (medium)
    - owner plays with recording on; agent replays the exact session (crashes, "the door turned me round")
    - drifts if the game isn't deterministic; still good for the first minutes after a warp
- Door / exit list + teleport facing (small)
    - log each level's exits: position, target level/entrance, entrance facing
    - add facing to JFG_DEBUG_TELEPORT, so real doors can be walked into (not simulated with a second warp)
- Level tour (small, after timeline + screenshots)
    - visit every level in widescreen, screenshot each, flag sides that differ from the 4:3 middle (missing textures, black areas, 4:3 boxes)
- One-frame draw call dump switch (small)
    - every draw call of one frame: type, rect, scissor, widened or not; replaces the temporary RT64 logs written for each renderer bug
- Save / load state (large, fragile)
    - all game memory + audio + renderer state; only after the above (also in ROADMAP.md)
