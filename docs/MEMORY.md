# Memory: how the port gives the game more than the N64 had

Jet Force Gemini was written for 4 MB of RAM, with the Expansion Pak's second 4 MB mostly unused. The port runs the
game's own code, so it keeps the game's limits unless we lift them on purpose. This is how it does that so far, and
where there's room for more. Bigger custom levels, more objects, and anything else that needs memory build on this.

## The memory map

The game sees one block of RAM (the runtime commits 512 MB). Addresses as the game uses them:

| Range | Size | What's there |
|---|---|---|
| `0x80000000`–`0x80400000` | 4 MB | The game's own code, data and heap (`mmEndRam` was `0x80400000`). |
| `0x80400000`–`0x80800000` | 4 MB | The Expansion Pak's second 4 MB: now part of the game's main heap (`jfg_memory.cpp`). |
| `0x80800000`–`0x80C00000` | 4 MB | Mods' graphics data (`jfg_gfx_alloc`, `jfg_api.cpp`): handed out from the bottom, kept for the run (the Minimap mod's pictures take 32 KB). The rest is free. |
| `0x80C00000`–`0x81000000` | 4 MB | The gore blocks: display lists that draw the extra blood splats, 2 MB each (`jfg_gore.cpp`). |
| `0x81000000` upward | to 512 MB | Mods: their code and data, then the runtime's heap (`recomp_alloc`), to the end. |
| `0x82000000` upward | | Not memory: the overlays' private code addresses, where hooked code runs (`jfg_mods.cpp`). |

**The 16 MB line.** The RSP, RDP and VI take 24-bit addresses, so anything the graphics or video read (display
lists, vertices, textures, frame buffers) must sit below `0x81000000`. Data only the CPU reads (object state, tables,
lists) can live anywhere, up to 512 MB. Audio is the exception: the port lets audio DMA read all of it
(`lib/N64ModernRuntime/librecomp/src/ai.cpp`), so a mod's sounds can sit above `0x81000000`.

## Four ways to give the game more

1. **A bigger heap.** `mempool_init` is wrapped: when the game makes its main pool (ending at `mmEndRam`), the pool
   is grown to end at `0x80800000` and `mmEndRam` is moved with it. Everything the game `mmAlloc`s (levels, objects,
   models, sounds) gets twice the room. `jfg_memory.log` notes it once. The 4 MB above it is mods' graphics data now
   (`jfg_gfx_alloc`); growing the heap into it would need the game's own size checks (`osMemSize`, 8 MB) checked first.
2. **A bigger fixed block.** Some of the game's buffers are one `mmAlloc` of a fixed size with no check on what
   goes in. The `mmAlloc` and `mmAlloc2` wrappers (`jfg_diagnostics.cpp`) call `jfg_memory_alloc_size`, which
   asks for more for the ones we know. The track buffer was `0x9F000` (651 KB): a pack level's track and its
   load-time tables (about 44 bytes per triangle) wrote past it and crashed the game. It's now `0x140000`
   (1.25 MB); the loader shrinks the block to what the track used, so stock levels end up the same. The packed
   track and the unpacked one together must still fit `0x9F000` (the constant at overlay 24 `+0x378`/`+0x380`),
   which hasn't been raised yet.
3. **Keep it on the host, draw it in passes (blood splats).** The game has a pool of 64 blood entries and frees the
   oldest. "Blood stays" copies each splat that would be freed into a list on the host side, and runs the game's own
   `bloodSpurtsDraw` again for them, 64 at a time: the newest kept splats are put into the pool, drawn, and the pool
   is put back. Those extra passes write their display lists into the gore blocks at `0x80C00000`, because the
   level's own frame buffers are sized for its normal drawing. So the game's 64-entry limit stays in its code, and
   the port shows as many as the player sets (Blood Splat Limit) without touching the game's memory.
4. **Mod memory.** A mod's own data can be allocated with `recomp_alloc` (above `0x81000000`, CPU-only), which the
   game's heap never sees.

## Limits we know of, and what to check before relying on more

- Past `0x81000000`, nothing the RSP, RDP or VI read (see the 16 MB line).
- The packed-track constant (`0x9F000`, overlay 24) still bounds a level's track: Studio checks it on import.
- The object region holds 100 KB and 511 allocations, separately from the heap.
- The main heap past `0x80800000` would need the game's `osMemSize` (8 MB) and anything sized from it checked.

Studio's level checks (track size, the track buffer) and its guides follow these numbers. When one changes, change
Studio (`jfgstudio/importer.py` `TRACK_BUFFER`, `level_design.md` "Stay in the game's memory") too.
