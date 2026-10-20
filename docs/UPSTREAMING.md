# Publication and upstreaming strategy

## Recommended project home

Publish the complete recomp as its own GPLv3 repository, with explicit lineage from Dinosaur Planet: Recompiled and clear credit to the DKR decompilation. Do not propose the entire native port to `DavidSM64/Diddy-Kong-Racing`: that repository is a matching decompilation, while this project is an application runtime, renderer integration, packaging layer, and generated-code pipeline.

The separate repository should be the place users download source and releases, report native-port bugs, and find the supported ROM revision and build instructions. The matching decompilation remains the authoritative home for matching C, symbols, structures, and documentation about the retail program.

## Focused upstream contributions

| Destination | Suitable contribution | Keep out of that PR |
| --- | --- | --- |
| `DavidSM64/Diddy-Kong-Racing` | verified symbol, structure, comment, or matching-code corrections | launcher, RT64, platform packaging, native-only workarounds |
| `rt64/rt64` | F3DDKR command support and renderer fixes with microcode evidence | DKR launcher policy and game-specific packaging |
| `N64Recomp/N64Recomp` | generally useful RSP instruction and control-flow support | DKR-specific scene behavior |
| `N64Recomp/N64ModernRuntime` | task and memory-lifetime fixes that apply beyond DKR | renderer command decoding |
| Plume | backend resource-ownership fixes reproducible without DKR | game-state or microcode logic |
| This repository | native DKR shims, configuration, input defaults, diagnostics, tests, and releases | unrelated changes to matching retail code |

## Suggested sequence

1. Publish a source-only alpha repository with no ROM, extracted assets, generated CPU source, private logs, or build products.
2. Preserve the current working dependency state as pinned submodules plus synchronized files under `submodule-patches/`.
3. Open a discussion or issue with the DKR decomp maintainers introducing the project, disclosing the substantial AI assistance, and asking how they prefer verified decomp corrections to be submitted.
4. Split the N64Recomp and N64ModernRuntime patches into one root cause per PR, with minimal tests.
5. Split RT64 work into F3DDKR command support, lifetime/safety fixes, transform correctness, and performance policy. Avoid a single renderer mega-PR.
6. Replace each local dependency patch with the accepted upstream commit, or pin a clearly named project fork when upstream scope differs.

## Review evidence

Every upstream PR should state:

- the observable failure and root cause;
- why the owning repository is the right layer;
- the smallest relevant diff;
- the exact platforms, scenes, and test duration exercised;
- whether behavior was compared with retail or the matching decompilation;
- what OpenAI Codex contributed and what the submitter reviewed;
- remaining uncertainty.

The initial public repository can be honest about being an alpha. It should not be described as fully compatible until the incomplete Adventure, multiplayer, boss, vehicle, character, and course coverage in `docs/TESTING.md` is completed.
