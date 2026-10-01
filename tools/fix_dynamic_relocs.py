#!/usr/bin/env python3
"""Post-process N64Recomp output: make linker-patched instructions read their live value from RDRAM.

Input: the generated C directory, jfg.us.dynamic_relocs.json (from prepare_overlays.py) and the pre-linked
recompiler ROM (to know which immediate each site was generated with). See include/jfg_dynamic.h for the why.

  jal  site:  TrapDanglingJump(rdram, ctx);            ->  JFG_DYN_CALL(<site>);
  lui  site:  ctx->rN = S32(0XHHHH << 16);             ->  ctx->rN = S32(JFG_DYN_IMM(<site>) << 16);
  lo16 site:  ... 0XLLLL / -0XLLLL ...                 ->  ... (int16_t)JFG_DYN_IMM(<site>) ...

<site> is the instruction's runtime address: a constant in the main segment, `section_addresses[k] + offset` in an
overlay. Every site must be found and must match its expected pattern, otherwise the script stops with an error.

Usage: fix_dynamic_relocs.py <RecompiledFuncs dir> <dynamic_relocs.json> <recomp rom> <symbols.toml>
"""
import glob, json, re, struct, sys


def write_text(path, text):
    """Write a file, retrying for a few seconds if something else has it open.

    On Windows a virus scanner, the search indexer or a cloud-sync client often opens a freshly written file for a
    moment, and writing it again right then fails with "Permission denied".
    """
    import time
    for attempt in range(40):
        try:
            with open(path, "w") as f:
                f.write(text)
            return
        except PermissionError:
            if attempt == 39:
                raise
            time.sleep(0.25)

MARK = "/* jfg live reloc */"
COMMENT = re.compile(r"^\s*// 0x([0-9A-F]{8}): ")
LITERAL = re.compile(r"-?0X[0-9A-F]+")
MAIN_ROM, MAIN_VRAM = 0x1000, 0x80000400

def main(out_dir, dyn_path, rom_path, syms_path):
    dyn = json.load(open(dyn_path))
    rom = open(rom_path, "rb").read()
    order = re.findall(r'^name = "(\.[\w]+)"', open(syms_path).read(), re.M)   # section index = order in the toml
    sites = {}   # link vram -> (kind, site expression, expected imm16)
    def add(vram, kind, expr, rom_off):
        word = struct.unpack_from(">I", rom, rom_off)[0]
        if vram in sites and sites[vram][0] != kind:
            sys.exit("conflicting dynamic site kinds at 0x%08X" % vram)
        sites[vram] = (kind, expr, word & 0xFFFF)
    for s in dyn["main"]["sites"]:
        vram = dyn["main"]["vram"] + s["off"]
        add(vram, s["kind"], "0x%08Xu" % vram, MAIN_ROM + vram - MAIN_VRAM)
    for num, ov in dyn["overlays"].items():
        idx = order.index(".overlay_%s" % num)
        for s in ov["sites"]:
            add(ov["link"] + s["off"], s["kind"], "(uint32_t)section_addresses[%d] + 0x%Xu" % (idx, s["off"]), ov["rom"] + s["off"])

    found = set()
    for path in sorted(glob.glob(out_dir + "/funcs_*.c")):
        text = open(path).read()
        if MARK in text:
            sys.exit(path + ": already processed; regenerate with N64Recomp first")
        lines = text.split("\n")
        changed = False
        i = 0
        while i < len(lines):
            m = COMMENT.match(lines[i])
            vram = int(m.group(1), 16) if m else None
            if vram not in sites:
                i += 1; continue
            kind, expr, imm = sites[vram]
            j = i + 1   # statement lines of this instruction: up to the next instruction comment
            while j < len(lines) and not COMMENT.match(lines[j]): j += 1
            body = range(i + 1, j)
            if kind == "jal":
                # the call is emitted after the delay slot instruction, i.e. after the NEXT comment
                k = j
                while k < len(lines) and lines[k].strip() != "TrapDanglingJump(rdram, ctx);":
                    if k > j and COMMENT.match(lines[k]) and k - j > 12: break
                    k += 1
                if k >= len(lines) or lines[k].strip() != "TrapDanglingJump(rdram, ctx);":
                    sys.exit("%s: jal site 0x%08X: call statement not found" % (path, vram))
                lines[k] = lines[k].replace("TrapDanglingJump(rdram, ctx);", "JFG_DYN_CALL(%s); %s" % (expr, MARK))
            elif kind == "hi":
                want = "S32(0X%X << 16)" % imm
                hits = [b for b in body if want in lines[b]]
                if len(hits) != 1: sys.exit("%s: lui site 0x%08X: expected `%s`, got %r" % (path, vram, want, [lines[b] for b in body]))
                lines[hits[0]] = lines[hits[0]].replace(want, "S32(JFG_DYN_IMM(%s) << 16)" % expr) + " " + MARK
            else:
                signed = imm - 0x10000 if imm & 0x8000 else imm
                want = ("-0X%X" % -signed) if signed < 0 else ("0X%X" % signed)
                hits = [(b, mm) for b in body for mm in LITERAL.finditer(lines[b]) if mm.group(0) == want]
                if len(hits) != 1: sys.exit("%s: lo16 site 0x%08X: expected one `%s`, got %r" % (path, vram, want, [lines[b] for b in body]))
                b, mm = hits[0]
                lines[b] = lines[b][:mm.start()] + "(int16_t)JFG_DYN_IMM(%s)" % expr + lines[b][mm.end():] + " " + MARK
            found.add(vram); changed = True
            i += 1
        if changed:
            write_text(path, '#include "jfg_dynamic.h"\n' + "\n".join(lines))
    missing = sorted(set(sites) - found)
    print("fix_dynamic_relocs: %d of %d dynamic sites patched" % (len(found), len(sites)))
    if missing:
        print("sites not present in generated code (not inside any recompiled function):")
        for v in missing: print("  0x%08X %s" % (v, sites[v][0]))
    return 0

if __name__ == "__main__":
    if len(sys.argv) != 5: sys.exit(__doc__)
    sys.exit(main(*sys.argv[1:]))
