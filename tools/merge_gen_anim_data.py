#!/usr/bin/env python3
"""Post-process N64Recomp output for the gen_anim_data hand-written assembly blob.

src/hasm/gen_anim_data.s (VRAM 0x80073E80-0x80074E50) has 16 entry points whose code branches freely across each
other, which N64Recomp's single-entry function model cannot express. jfg.us.toml recompiles the whole blob as one
function named gen_anim_data. This script:
  1. renames it to a static multi-entry function taking the entry VRAM,
  2. inserts a label at each entry point and a dispatch switch at the top,
  3. appends one wrapper per entry point with the original symbol name.
No instruction semantics are changed. Run after every N64Recomp invocation:  merge_gen_anim_data.py RecompiledFuncs
"""
import glob, re, sys


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

ENTRIES = {  # US ROM; from `readelf -s build/jfg.us.elf`
    "gen_anim_data": 0x80073E80, "func_800749AC": 0x8007422C, "func_80074A68": 0x800742E8,
    "func_80074AA4": 0x80074324, "func_80074AE0": 0x80074360, "func_80074B2C": 0x800743AC,
    "func_80074B50": 0x800743D0, "func_80074D4C": 0x800745CC, "func_80074D60": 0x800745E0,
    "func_80074FD0": 0x80074850, "func_80075050": 0x800748D0, "func_800750F0": 0x80074970,
    "func_8007524C": 0x80074ACC, "func_8007532C": 0x80074BAC, "func_800753BC": 0x80074C3C,
    "func_80075528": 0x80074DA8,
}
HEAD = "RECOMP_FUNC void gen_anim_data(uint8_t* rdram, recomp_context* ctx) {"
MARK = "/* merged by merge_gen_anim_data.py */"

def main(out_dir):
    for path in sorted(glob.glob(out_dir + "/funcs_*.c")):
        text = open(path).read()
        if MARK in text:
            print("already merged:", path); return 0
        if HEAD in text:
            break
    else:
        sys.exit("gen_anim_data not found in " + out_dir)

    start = text.index(HEAD)
    end = text.index("\n;}\n", start) + 4  # N64Recomp terminates every function with ";}"
    body = text[start:end]
    body = body.replace(HEAD, MARK + "\nstatic void gen_anim_data_blob(uint8_t* rdram, recomp_context* ctx, uint32_t jfg_entry) {", 1)

    for name, vram in ENTRIES.items():
        if name == "gen_anim_data":
            continue
        pat = re.compile(r"^([ \t]*)// 0x%08X: " % vram, re.M)
        hits = pat.findall(body)
        if len(hits) != 1:
            sys.exit("entry %s (0x%08X): expected exactly one instruction comment, found %d" % (name, vram, len(hits)))
        if hits[0] != "    ":
            sys.exit("entry %s (0x%08X) is nested inside a block (delay slot?); cannot place a label" % (name, vram))
        body = pat.sub("JFG_ENTRY_%08X:;\n    // 0x%08X: " % (vram, vram), body, count=1)

    switch = "    switch (jfg_entry) {\n" + "".join(
        "        case 0x%08Xu: goto JFG_ENTRY_%08X;\n" % (v, v) for n, v in ENTRIES.items() if n != "gen_anim_data"
    ) + "        default: break;\n    }\n"
    # The dispatch goes after the function preamble. The port base's N64Recomp fork also emits a
    # recomp_enter_function() call there (crash-report call stack); every entry point must run it exactly once,
    # because each return path calls recomp_exit_function().
    m = re.search(r"    int c1cs = 0;\n(    recomp_enter_function\([^\n]*\n)?", body)
    if not m or body.count("    int c1cs = 0;\n") != 1:
        sys.exit("unexpected function preamble")
    body = body[:m.end()] + switch + body[m.end():]

    wrappers = "\n".join(
        "RECOMP_FUNC void %s(uint8_t* rdram, recomp_context* ctx) {\n    gen_anim_data_blob(rdram, ctx, 0x%08Xu);\n}\n" % (n, v)
        for n, v in ENTRIES.items())
    # Entry points listed under `ignored` in the toml are not declared in funcs.h; declare them before the blob.
    protos = "".join("void %s(uint8_t* rdram, recomp_context* ctx);\n" % n for n in ENTRIES)
    write_text(path, text[:start] + protos + body + "\n" + wrappers + text[end:])
    print("merged gen_anim_data blob in", path, "(%d entry points)" % len(ENTRIES))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1] if len(sys.argv) > 1 else "RecompiledFuncs"))
