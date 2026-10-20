#!/usr/bin/env python3
"""Prepare Jet Force Gemini's overlays for static recompilation.

The ROM's overlays are relocatable modules linked at address 0. The game's runtime linker (src/runLink.c) loads them
to heap addresses and patches their instructions from two relocation tables per overlay. N64Recomp needs
(a) instruction immediates that make sense statically and (b) to know which instructions depend on the load address.

This tool reads the linker tables straight from the ROM and writes:
  1. <out_rom>   A copy of the ROM in which every overlay has been pre-linked at a private, unique address
                 (LINK_BASE upward) using exactly the algorithm in runLink.c, with all other overlays treated as
                 "not loaded". It is recompiler INPUT ONLY; the port always runs from the real ROM.
  2. <out_syms>  Symbol-toml sections for the overlays (functions from the decomp ELF, relocations for references
                 an overlay makes to itself) to append to the main-segment symbols.
  3. <out_dyn>   JSON list of "dynamic" sites: instructions the game's linker re-patches whenever some OTHER
                 overlay is loaded or unloaded (in the main segment and in overlays). tools/fix_dynamic_relocs.py
                 makes the generated C read those instructions' live values from emulated RDRAM.

Usage: prepare_overlays.py <rom> <decomp jfg.us.elf | -> <out_rom> <out_syms.toml | -> <out_dyn.json>
       With "-" for the ELF only the pre-linked ROM and the JSON are written (the checked-in symbols are reused).
"""
import json, struct, subprocess, sys, collections

# --- US ROM layout (ver/splat/jfg.us.yaml) and main-segment symbols (ver/symbols/symbol_addrs.us.txt) ---------
MAIN_RELOC_TABLE, OVERLAY_ROM_TABLE, OVERLAY_TABLE, OVERLAY_CODE = 0x1ECF220, 0x1ED0270, 0x1ED2780, 0x1ED3B20
MAIN_ROM, MAIN_VRAM = 0x1000, 0x80000400
CODE_START = 0x80000450          # __CODE_SECTION_START (amSetMuteMode)
DATA_START = 0x800A0660          # __DATA_SECTION_START (tuneSeqPlayer)
BSS_START = 0x800B0B50           # __BSS_SECTION_START
TRAP_DANGLING_JUMP = 0x80054530
UNRESOLVED_SYMBOL = 0x800FEAD4   # &gUnresolvedSymbolAddr
LINK_BASE = 0x82000000           # private link addresses for overlays; outside RDRAM, never used at runtime

PATCH_WORD, PATCH_JAL, PATCH_HI16, PATCH_LO16 = 2, 4, 5, 6
TYPE_EXTERNAL, TYPE_LOCAL, TYPE_JUMP, TYPE_DATA = 0, 1, 2, 3
SECTION_UNUSED, SECTION_DATA_1, SECTION_DATA_2, SECTION_BSS = 0xFFB, 0xFFD, 0xFFE, 0xFFF

Overlay = collections.namedtuple("Overlay", "num rom text data bss rel1 rel2 init resume link")
Reloc = collections.namedtuple("Reloc", "sym off patch type")

def u32(b, o): return struct.unpack_from(">I", b, o)[0]
def sext16(v): return v - 0x10000 if v & 0x8000 else v

def read_relocs(data):
    return [Reloc(u32(data, k), u32(data, k + 4) >> 8, (u32(data, k + 4) >> 4) & 0xF, u32(data, k + 4) & 0xF)
            for k in range(0, len(data) - 7, 8)]

class Linker:
    def __init__(self, rom):
        self.rom = rom
        self.out = bytearray(rom)
        self.ort = [u32(rom, o) for o in range(OVERLAY_ROM_TABLE, OVERLAY_TABLE, 4)]
        self.overlays = {}
        link = LINK_BASE
        for i in range((OVERLAY_CODE - OVERLAY_TABLE) // 0x20):
            _, ra, ts, ds, bs, r1, r2, ini, res = struct.unpack_from(">IIIIIHHii", rom, OVERLAY_TABLE + i * 0x20)
            self.overlays[i + 1] = Overlay(i + 1, OVERLAY_CODE + ra, ts, ds, bs, r1, r2, ini, res, link)
            link += (ts + ds + bs + r1 + 0xFFFF) & ~0xFFFF
        count = u32(rom, MAIN_RELOC_TABLE)
        self.main_relocs = read_relocs(rom[MAIN_RELOC_TABLE + 4: MAIN_RELOC_TABLE + 4 + count * 8])

    def target_overlay(self, sym):
        n = self.ort[sym] >> 20
        return 0 if n > SECTION_UNUSED else n

    def resolve(self, r, cur, word):
        """ResolveRelocAddress with only main and overlay `cur` loaded. Returns (address, loaded)."""
        if r.type in (TYPE_EXTERNAL, TYPE_DATA):
            entry = self.ort[r.sym]
            num, off, adj = entry >> 20, entry & 0xFFFFF, 0
            if num in (SECTION_DATA_1, SECTION_DATA_2): num, adj = 0, DATA_START - CODE_START
            elif num == SECTION_BSS: num, adj = 0, BSS_START - CODE_START
            if num == 0: return (CODE_START + off + adj) & 0xFFFFFFFF, True
            if num == cur: return (self.overlays[cur].link + off) & 0xFFFFFFFF, True
            return (TRAP_DANGLING_JUMP if r.patch in (PATCH_JAL, PATCH_WORD) else UNRESOLVED_SYMBOL), False
        if r.type == TYPE_LOCAL:
            a = self.overlays[cur].link + r.sym
            return ((a + word) if r.patch == PATCH_WORD else a) & 0xFFFFFFFF, True
        if r.type == TYPE_JUMP:
            return (((word & 0x03FFFFFF) << 2) + self.overlays[cur].link) & 0xFFFFFFFF, True
        raise ValueError("unknown reloc type %d" % r.type)

    def link_overlay(self, ov):
        """ProcessRelocationEntry over the secondary then the primary table, as runlinkDownloadCode does."""
        tables = ov.rom + ov.text + ov.data
        sec = read_relocs(self.rom[tables + ov.rel1: tables + ov.rel1 + ov.rel2])
        prim = read_relocs(self.rom[tables: tables + ov.rel1])
        self_relocs, dynamic = [], []
        def loc(r):  # ROM offset of the patched word
            return ov.rom + (ov.text if r.type == TYPE_DATA else 0) + r.off
        def rd(o): return u32(self.out, o)
        def wr(o, v): struct.pack_into(">I", self.out, o, v & 0xFFFFFFFF)
        def put_hi(o, a): wr(o, (rd(o) & 0xFFFF0000) | (((a >> 16) + (1 if a & 0x8000 else 0)) & 0xFFFF))
        def put_lo(o, a): wr(o, (rd(o) & 0xFFFF0000) | (a & 0xFFFF))
        for table in (sec, prim):
            k = 0
            while k < len(table):
                r = table[k]; o = loc(r); word = rd(o)
                addr, loaded = self.resolve(r, ov.num, word)
                in_text = r.type != TYPE_DATA
                foreign = r.type in (TYPE_EXTERNAL, TYPE_DATA) and self.target_overlay(r.sym) not in (0, ov.num)
                if r.patch == PATCH_HI16:
                    r2 = table[k + 1]
                    assert r2.patch == PATCH_LO16, "HI16 without LO16 in overlay %d" % ov.num
                    o2 = ov.rom + r2.off
                    if not loaded: addr = UNRESOLVED_SYMBOL
                    addend = ((word & 0xFFFF) << 16) + sext16(rd(o2) & 0xFFFF) & 0xFFFFFFFF
                    if addend != UNRESOLVED_SYMBOL: addr = (addr + addend) & 0xFFFFFFFF
                    put_hi(o, addr); put_lo(o2, addr)
                    if foreign:
                        dynamic += [{"off": r.off, "kind": "hi"}, {"off": r2.off, "kind": "lo"}]
                    elif self.is_self(r, ov):
                        self_relocs += [(r.off, addr, "R_MIPS_HI16"), (r2.off, addr, "R_MIPS_LO16")]
                    k += 2; continue
                if r.patch == PATCH_LO16:
                    if not loaded: addr = UNRESOLVED_SYMBOL
                    addr = (addr + (word & 0xFFFF)) & 0xFFFFFFFF
                    put_lo(o, addr)
                    if foreign: dynamic.append({"off": r.off, "kind": "lo"})
                    elif self.is_self(r, ov): self_relocs.append((r.off, addr, "R_MIPS_LO16"))
                elif r.patch == PATCH_WORD:
                    wr(o, addr)
                elif r.patch == PATCH_JAL:
                    wr(o, (word & 0xFC000000) | ((addr >> 2) & 0x03FFFFFF))
                    if foreign and in_text: dynamic.append({"off": r.off, "kind": "jal"})
                else:
                    raise ValueError("unknown patch op %d" % r.patch)
                k += 1
        return self_relocs, dynamic

    def is_self(self, r, ov):
        return r.type == TYPE_LOCAL or (r.type == TYPE_EXTERNAL and self.target_overlay(r.sym) == ov.num)

    def main_dynamic(self):
        out = []
        for r in self.main_relocs:
            if r.type == TYPE_DATA: continue   # pointers in main .data: patched in RDRAM by the game, read as data
            kind = {PATCH_JAL: "jal", PATCH_HI16: "hi", PATCH_LO16: "lo"}.get(r.patch)
            if kind is None: raise ValueError("unexpected main reloc patch %d" % r.patch)
            out.append({"off": r.off, "kind": kind})
        return out

def elf_overlay_functions(elf):
    secs = {}
    for line in subprocess.run(["readelf", "-SW", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.replace("[", " ").replace("]", " ").split()
        if len(p) > 2 and p[0].isdigit() and p[1].startswith(".overlay_") and not p[1].endswith("_bss"):
            secs[int(p[0])] = int(p[1].split("_")[1])
    funcs = collections.defaultdict(list)
    for line in subprocess.run(["readelf", "-sW", elf], capture_output=True, text=True, check=True).stdout.splitlines():
        p = line.split()
        if len(p) == 8 and p[3] == "FUNC" and p[6].isdigit() and int(p[6]) in secs:
            funcs[secs[int(p[6])]].append((int(p[1], 16), int(p[2]), p[7]))
    return funcs

def main(rom_path, elf_path, out_rom, out_syms, out_dyn):
    rom = open(rom_path, "rb").read()
    lk = Linker(rom)
    funcs = elf_overlay_functions(elf_path) if elf_path != "-" else None
    dyn = {"main": {"vram": CODE_START, "sites": lk.main_dynamic()}, "overlays": {}}
    toml = []
    for num, ov in sorted(lk.overlays.items()):
        if ov.text == 0: continue
        self_relocs, dynamic = lk.link_overlay(ov)
        dyn["overlays"][str(num)] = {"link": ov.link, "rom": ov.rom, "text": ov.text, "data": ov.data,
                                     "bss": ov.bss, "sites": sorted(dynamic, key=lambda d: d["off"])}
        if funcs is None: continue
        fl = sorted(funcs.get(num, []))
        if not fl: raise SystemExit("overlay %d has code but no function symbols in the ELF" % num)
        toml.append("[[section]]\nname = \".overlay_%d\"\nrom = 0x%08X\nvram = 0x%08X\nsize = 0x%X\n\nfunctions = [" %
                    (num, ov.rom, ov.link, ov.text + ov.data))
        seen = set()
        for off, size, name in fl:
            if size == 0: raise SystemExit("size-0 function %s in overlay %d" % (name, num))
            if off >= ov.text: raise SystemExit("function %s outside text of overlay %d" % (name, num))
            if name in seen: continue
            seen.add(name)
            toml.append("    { name = \"%s\", vram = 0x%08X, size = 0x%X }," % (name, ov.link + off, size))
        toml.append("]\n\nrelocs = [")
        for off, target, typ in sorted(set(r for r in self_relocs if r[0] < ov.text)):
            toml.append("    { type = \"%s\", vram = 0x%08X, target_vram = 0x%08X }," % (typ, ov.link + off, target))
        toml.append("]\n")
    open(out_rom, "wb").write(lk.out)
    if funcs is not None: open(out_syms, "w").write("\n".join(toml) + "\n")
    json.dump(dyn, open(out_dyn, "w", newline="\n"), indent=0)   # LF on every OS: this file is committed (tools/)
    n_dyn = sum(len(o["sites"]) for o in dyn["overlays"].values())
    print("overlays with code: %d; self relocs: %d; dynamic sites: main %d, overlays %d" %
          (len(dyn["overlays"]), sum(t.count("R_MIPS_") for t in toml), len(dyn["main"]["sites"]), n_dyn))

if __name__ == "__main__":
    if len(sys.argv) != 6: sys.exit(__doc__)
    main(*sys.argv[1:])
