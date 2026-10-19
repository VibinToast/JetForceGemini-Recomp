#!/usr/bin/env python3
"""Post-process N64Recomp output: make functions mods can't hook safely report "cannot be hooked".

To hook a function the runtime recompiles it again at start-up from the ROM (src/jfg/jfg_mods.cpp gives it the
pre-linked copy). That copy can't hold the values the game's linker patches in while the game runs: the offline build
reads those from RDRAM instead (tools/fix_dynamic_relocs.py, include/jfg_dynamic.h), a regenerated function would bake
them. Calls (`jal`) are fine baked: they go through TrapDanglingJump, the game's own lazy linker. Addresses of another
overlay's data (`lui`/`lo16`) are not: a regenerated function would use a wrong address. So every function with such a
site gets rom_size 0 in recomp_overlays.inl, which the runtime (librecomp mods.cpp) refuses to hook with
CannotBeHooked, naming the function's section and address. Replacing them (RECOMP_PATCH) is unaffected.

Usage: refuse_mod_hooks.py <RecompiledFuncs dir> <dynamic_relocs.json> <symbols.toml>
"""
import bisect, json, os, re, sys


def functions_by_section(syms_path):
    secs, cur = {}, None
    for line in open(syms_path):
        m = re.match(r'name = "\.(\w+)"', line)
        if m:
            cur = m.group(1)
            secs[cur] = []
        m = re.match(r'\s+\{ name = "([^"]+)", vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
        if m:
            secs[cur].append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    return {k: sorted(v) for k, v in secs.items()}


def owner(funcs, addr):
    i = bisect.bisect_right([f[0] for f in funcs], addr) - 1
    if i >= 0 and funcs[i][0] <= addr < funcs[i][0] + funcs[i][1]:
        return funcs[i][2]
    return None


def main(out_dir, dyn_path, syms_path):
    dyn = json.load(open(dyn_path))
    secs = functions_by_section(syms_path)
    refuse = set()
    main_ = dyn["main"]
    for s in main_["sites"]:
        if s["kind"] in ("hi", "lo"):
            refuse.add(owner(secs["main"], main_["vram"] + s["off"]))
    for num, ov in dyn["overlays"].items():
        for s in ov["sites"]:
            if s["kind"] in ("hi", "lo"):
                refuse.add(owner(secs["overlay_" + num], ov["link"] + s["off"]))
    if None in refuse:
        sys.exit("refuse_mod_hooks: a data site is outside every function in " + syms_path)

    path = os.path.join(out_dir, "recomp_overlays.inl")
    text = open(path).read()
    for name in sorted(refuse):
        pat = re.compile(r"(\{ \.func = " + re.escape(name) + r", \.offset = 0x[0-9A-Fa-f]+, \.rom_size = )0x[0-9A-Fa-f]+")
        text, n = pat.subn(r"\g<1>0x00000000", text)
        if n != 1:
            sys.exit("refuse_mod_hooks: %s found %d times in %s" % (name, n, path))
    with open(path, "w") as f:
        f.write(text)
    print("refuse_mod_hooks: %d functions can't be hooked by mods: %s" % (len(refuse), ", ".join(sorted(refuse))))


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
