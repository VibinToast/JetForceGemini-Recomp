#!/usr/bin/env python3
"""Post-process N64Recomp output: make functions mods can't hook safely report "cannot be hooked".

To hook a function the runtime recompiles it again at start-up from the ROM (src/jfg/jfg_mods.cpp gives it the
pre-linked copy). The regenerated function has only what the ROM has, so it would lose:
  1. The port's own changes: every function with a [[patches.hook]] or [[patches.instruction]] in jfg.us.toml, and
     gen_anim_data (rewritten by tools/merge_gen_anim_data.py). A mod hook on one of them would silently turn that
     port feature off for the whole run (it happened to the 60 fps squad cadence, now done outside those functions).
  2. Live linker values: the offline build reads the instructions the game's linker patches while it runs from RDRAM
     (tools/fix_dynamic_relocs.py, include/jfg_dynamic.h); a regenerated function bakes them. Calls (`jal`) are fine
     baked, they go through TrapDanglingJump, the game's own lazy linker. Addresses of another overlay's data
     (`lui`/`lo16`) are not.
Each such function gets rom_size 0 in recomp_overlays.inl, which the runtime (librecomp mods.cpp) refuses to hook with
CannotBeHooked, naming the function's section and address. Replacing them (RECOMP_PATCH) is unaffected; a mod that
replaces one of the port's functions takes over that function, port change and all.

Usage: refuse_mod_hooks.py <RecompiledFuncs dir> <dynamic_relocs.json> <symbols.toml> <jfg.us.toml>
"""
import bisect, json, os, re, sys, tomllib


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


def data_site_functions(dyn_path, secs):
    dyn = json.load(open(dyn_path))
    out = set()
    for s in dyn["main"]["sites"]:
        if s["kind"] in ("hi", "lo"):
            out.add(owner(secs["main"], dyn["main"]["vram"] + s["off"]))
    for num, ov in dyn["overlays"].items():
        for s in ov["sites"]:
            if s["kind"] in ("hi", "lo"):
                out.add(owner(secs["overlay_" + num], ov["link"] + s["off"]))
    if None in out:
        sys.exit("refuse_mod_hooks: a data site is outside every function")
    return out


def port_changed_functions(config_path):
    cfg = tomllib.load(open(config_path, "rb"))
    patches = cfg.get("patches", {})
    out = {h["func"] for h in patches.get("hook", [])} | {p["func"] for p in patches.get("instruction", [])}
    out.add("gen_anim_data")
    return out


def main(out_dir, dyn_path, syms_path, config_path):
    secs = functions_by_section(syms_path)
    data_sites = data_site_functions(dyn_path, secs)
    port = port_changed_functions(config_path)
    path = os.path.join(out_dir, "recomp_overlays.inl")
    text = open(path).read()
    refused, missing = [], []
    for name in sorted(data_sites | port):
        pat = re.compile(r"(\{ \.func = " + re.escape(name) + r", \.offset = 0x[0-9A-Fa-f]+, \.rom_size = )0x[0-9A-Fa-f]+")
        text, n = pat.subn(r"\g<1>0x00000000", text)
        if n == 1:
            refused.append(name)
        elif name in data_sites:
            sys.exit("refuse_mod_hooks: %s found %d times in %s" % (name, n, path))
        else:
            missing.append(name)  # a port-changed function that isn't in the table (ignored, stubbed, renamed)
    with open(path, "w") as f:
        f.write(text)
    print("refuse_mod_hooks: %d functions can't be hooked by mods (%d changed by the port, %d with live data addresses)"
          % (len(refused), len(port - set(missing)), len(data_sites)))
    if missing:
        print("refuse_mod_hooks: not in the function table, nothing to do: " + ", ".join(missing))


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
