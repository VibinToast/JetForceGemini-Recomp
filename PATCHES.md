# Dependency patch inventory

The DKR conversion currently carries four reviewed patch files so a clean clone can reproduce the exact dependency state used by the tested builds. Apply them with:

```bash
./scripts/apply-submodule-patches.sh
```

The script detects patches that are already applied. Do not apply these files manually on top of a dirty dependency checkout.

| Patch | Dependency | Scope |
| --- | --- | --- |
| `submodule-patches/n64recomp-dkr.patch` | N64Recomp / RSPRecomp | DKR RSP control flow, vector control registers, and DPC instruction support |
| `submodule-patches/n64modernruntime-dkr.patch` | N64ModernRuntime | empty audio-task handling and synchronous graphics-parse lifetime |
| `submodule-patches/rt64-f3ddkr.patch` | RT64 | F3DDKR command decoding, framebuffer safety, transform correctness, batching, and bounded shader preflight support |
| `submodule-patches/plume-sdl2-compat.patch` | Plume | Metal/Vulkan resource ownership and SDL compatibility |

These are reproducibility snapshots, not the desired long-term dependency model. Each patch should be split by root cause, validated against its owning project's current branch, and proposed upstream independently. Until accepted upstream, a public DKR repository should pin the exact submodule revisions recorded in `.gitmodules` and keep these patches byte-for-byte synchronized with the dirty submodule diffs.

DKR compatibility work lives in `dkr.us.v80.toml`, `src/recomp_api/`, the RSP TOML inputs, and the dependency patches above. Generated CPU and RSP sources remain private local build output.
