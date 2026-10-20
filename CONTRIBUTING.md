# Contributing

Diddy Kong Racing: Recompiled is an early compatibility project. Small, reviewable changes with clear evidence are much more useful than broad rewrites.

## Before changing code

1. Read [BUILDING.md](BUILDING.md), [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), and [docs/TESTING.md](docs/TESTING.md).
2. Reproduce the issue with the supported USA Rev 1 ROM.
3. Identify which repository owns the behavior before editing a vendored dependency.
4. For a significant change, open an issue or describe the intended scope before investing in a large patch.

## Repository boundaries

Changes should be proposed at the narrowest correct layer:

| Change | Primary home |
| --- | --- |
| Matching game code, symbols, structures, or documentation | [DKR decompilation](https://github.com/DavidSM64/Diddy-Kong-Racing) |
| DKR launcher, configuration, native shims, and packaging | This repository |
| F3DDKR command decoding or RT64 renderer behavior | RT64 |
| Metal or Vulkan resource lifetime behavior | Plume |
| Recompiler instruction generation | N64Recomp / RSPRecomp |
| libultra scheduling or host runtime behavior | N64ModernRuntime |

During the prototype phase, dependency changes are mirrored under `submodule-patches/` so a clean clone can reproduce the tested tree. The long-term goal is to replace those patches with reviewed upstream commits or pinned project forks.

See [docs/UPSTREAMING.md](docs/UPSTREAMING.md) for the proposed PR sequence and the evidence expected with each dependency change.

## Private game data

Never commit or upload:

- ROM files in any byte order;
- extracted textures, models, audio, or other game assets;
- decompilation build outputs containing proprietary data;
- generated files copied from a private ROM into a tracked path;
- logs or archives that accidentally contain any of the above.

`RecompiledFuncs/`, `RecompiledPatches/`, and local ROM names are ignored for safety. Always inspect the staged diff and the finished package anyway. Packaging scripts must retain their explicit ROM-payload checks.

## Patch expectations

Each contribution should:

- explain the observable problem and the root cause;
- avoid unrelated cleanup;
- preserve existing naming and formatting unless the change is specifically a cleanup;
- document non-obvious N64 memory, microcode, or renderer semantics near the code;
- include the platform and scene used to validate it;
- add or update a deterministic test where practical;
- leave `git diff --check` clean;
- contain no build products or machine-specific absolute paths.

Renderer and scheduler fixes should be tested on both a fast host and Steam Deck-class hardware when they affect threading, task completion, shader compilation, or framebuffer lifetime.

## Commit structure

Do not submit the current prototype as one monolithic change. A publishable history should separate at least:

1. DKR project identity and build configuration;
2. CPU and RSP recompilation inputs;
3. native hardware and Controller Pak shims;
4. F3DDKR renderer support;
5. input and launcher behavior;
6. platform packaging;
7. individual correctness and performance fixes;
8. documentation and tests.

Generated source should not be hand-edited. Update its TOML input, symbol manifest, recompiler, or native override instead.

## Testing

Follow [docs/TESTING.md](docs/TESTING.md). A successful compile alone is not sufficient for changes involving race rendering, post-race viewports, audio tasks, input polling, or save data.

Reports should include:

- project revision and build type;
- platform, CPU, GPU, and graphics API;
- ROM revision without attaching the ROM;
- exact menu/course/vehicle/character path;
- whether the behavior reproduces at original refresh rate;
- `runtime.log` or `crash.log` with personal paths reviewed before sharing.

## AI-assisted contributions

AI assistance is allowed, but it must be disclosed when it materially contributed to implementation or analysis. The submitter remains responsible for understanding the change, removing speculative code, validating behavior, and responding to review.

Do not submit unreviewed generated patches, invented test results, or claims that a problem is fixed without reproducing the relevant path. See [docs/AI_ASSISTANCE.md](docs/AI_ASSISTANCE.md) for this project's disclosure and validation policy.

## Licensing and attribution

Contributions to this project are accepted under GPLv3. Preserve third-party notices and attribution. The DKR decompilation is separately offered under CC0; its license does not cover Nintendo or Rare game data.
