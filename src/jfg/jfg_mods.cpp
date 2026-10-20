// Jet Force Gemini port: support for code mods (.nrm).
//
// 1. Overlay hooks. To hook a function the runtime recompiles it again at start-up from the ROM it gets from
//    GameEntry::decompression_routine (librecomp's mods.cpp; nothing else uses it). The port recompiles overlays from
//    a copy of the ROM in which every overlay is pre-linked at a private address (tools/prepare_overlays.py), so the
//    live recompiler has to see that copy too, or it reads unlinked overlay code and crashes. jfg_prelink_rom makes it
//    from the player's ROM with the same algorithm (the game's runLink.c, as prepare_overlays.py implements it).
//    Functions whose code materialises another overlay's data address can't be regenerated correctly; the build
//    makes them unhookable (tools/refuse_mod_hooks.py). See Recomp-Docs/MOD_SUPPORT.md.
// 2. Functions mods can import ("*"): recomp_printf, written to jfg_mods.log and stderr.
// (Mods that replace game data rather than code: jfg_assets.cpp.)
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <vector>

#include "librecomp/game.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"
#include "jfg_api.h"

namespace jfg {
void jfg_log_line(const char* file, const char* text); // jfg_diagnostics.cpp
}

namespace {

// --- pre-linking (tools/prepare_overlays.py, Linker) -------------------------------------------------------------
// US ROM layout (ver/splat/jfg.us.yaml) and main-segment symbols (ver/symbols/symbol_addrs.us.txt).
constexpr uint32_t OVERLAY_ROM_TABLE = 0x1ED0270, OVERLAY_TABLE = 0x1ED2780, OVERLAY_CODE = 0x1ED3B20;
constexpr uint32_t CODE_START = 0x80000450;        // __CODE_SECTION_START
constexpr uint32_t DATA_START = 0x800A0660;        // __DATA_SECTION_START
constexpr uint32_t BSS_START = 0x800B0B50;         // __BSS_SECTION_START
constexpr uint32_t TRAP_DANGLING_JUMP = 0x80054530;
constexpr uint32_t UNRESOLVED_SYMBOL = 0x800FEAD4; // &gUnresolvedSymbolAddr
constexpr uint32_t LINK_BASE = 0x82000000;         // private link addresses; must match prepare_overlays.py

constexpr uint32_t PATCH_WORD = 2, PATCH_JAL = 4, PATCH_HI16 = 5, PATCH_LO16 = 6;
constexpr uint32_t TYPE_EXTERNAL = 0, TYPE_LOCAL = 1, TYPE_DATA = 3; // 2 = jump within the overlay
constexpr uint32_t SECTION_DATA_1 = 0xFFD, SECTION_DATA_2 = 0xFFE, SECTION_BSS = 0xFFF;

struct Overlay { uint32_t num, rom, text, data, bss, rel1, rel2, link; };
struct Reloc { uint32_t sym, off, patch, type; };

uint32_t be32(std::span<const uint8_t> b, size_t o) {
    return (uint32_t)b[o] << 24 | (uint32_t)b[o + 1] << 16 | (uint32_t)b[o + 2] << 8 | b[o + 3];
}
uint16_t be16(std::span<const uint8_t> b, size_t o) { return (uint16_t)(b[o] << 8 | b[o + 1]); }
int32_t sext16(uint32_t v) { return (int32_t)(int16_t)(v & 0xFFFF); }

class Linker {
public:
    explicit Linker(std::span<const uint8_t> rom) : rom_(rom), out_(rom.begin(), rom.end()) {
        for (uint32_t o = OVERLAY_ROM_TABLE; o < OVERLAY_TABLE; o += 4) {
            ort_.push_back(be32(rom, o));
        }
        uint32_t link = LINK_BASE;
        for (uint32_t i = 0; i < (OVERLAY_CODE - OVERLAY_TABLE) / 0x20; i++) {
            const size_t e = OVERLAY_TABLE + i * 0x20; // OverlayHeader: ?, RomAddress, text, data, bss, rel1, rel2, ...
            Overlay ov{i + 1, OVERLAY_CODE + be32(rom, e + 4), be32(rom, e + 8), be32(rom, e + 12), be32(rom, e + 16),
                       be16(rom, e + 20), be16(rom, e + 22), link};
            link += (ov.text + ov.data + ov.bss + ov.rel1 + 0xFFFF) & ~0xFFFFu;
            overlays_.push_back(ov);
        }
    }

    // Returns the number of overlays linked, or -1 if the ROM doesn't look like the US ROM.
    int link_all() {
        int n = 0;
        for (const Overlay& ov : overlays_) {
            if (ov.text == 0) {
                continue;
            }
            if ((size_t)ov.rom + ov.text + ov.data + ov.rel1 + ov.rel2 > rom_.size()) {
                return -1;
            }
            link_overlay(ov);
            n++;
        }
        return n;
    }

    std::vector<uint8_t> take() { return std::move(out_); }
    const std::vector<Overlay>& overlays() const { return overlays_; }

private:
    std::span<const uint8_t> rom_;
    std::vector<uint8_t> out_;
    std::vector<uint32_t> ort_;
    std::vector<Overlay> overlays_;

    uint32_t rd(size_t o) const { return be32(out_, o); }
    void wr(size_t o, uint32_t v) {
        out_[o] = (uint8_t)(v >> 24); out_[o + 1] = (uint8_t)(v >> 16); out_[o + 2] = (uint8_t)(v >> 8); out_[o + 3] = (uint8_t)v;
    }
    void put_hi(size_t o, uint32_t a) { wr(o, (rd(o) & 0xFFFF0000) | ((((a >> 16) + ((a & 0x8000) ? 1 : 0))) & 0xFFFF)); }
    void put_lo(size_t o, uint32_t a) { wr(o, (rd(o) & 0xFFFF0000) | (a & 0xFFFF)); }

    std::vector<Reloc> read_relocs(size_t start, size_t len) const {
        std::vector<Reloc> out;
        for (size_t k = 0; k + 8 <= len; k += 8) {
            const uint32_t w = be32(rom_, start + k + 4);
            out.push_back({be32(rom_, start + k), w >> 8, (w >> 4) & 0xF, w & 0xF});
        }
        return out;
    }

    // ResolveRelocAddress with only main and overlay `cur` loaded. Returns the address; `loaded` false when the target
    // is in another overlay (then the game's placeholder: TrapDanglingJump for calls, gUnresolvedSymbolAddr for data).
    uint32_t resolve(const Reloc& r, const Overlay& cur, uint32_t word, bool& loaded) const {
        loaded = true;
        if (r.type == TYPE_EXTERNAL || r.type == TYPE_DATA) {
            const uint32_t entry = ort_[r.sym];
            uint32_t num = entry >> 20, off = entry & 0xFFFFF, adj = 0;
            if (num == SECTION_DATA_1 || num == SECTION_DATA_2) { num = 0; adj = DATA_START - CODE_START; }
            else if (num == SECTION_BSS) { num = 0; adj = BSS_START - CODE_START; }
            if (num == 0) return CODE_START + off + adj;
            if (num == cur.num) return cur.link + off;
            loaded = false;
            return (r.patch == PATCH_JAL || r.patch == PATCH_WORD) ? TRAP_DANGLING_JUMP : UNRESOLVED_SYMBOL;
        }
        if (r.type == TYPE_LOCAL) {
            const uint32_t a = cur.link + r.sym;
            return r.patch == PATCH_WORD ? a + word : a;
        }
        // TYPE_JUMP
        return ((word & 0x03FFFFFF) << 2) + cur.link;
    }

    // ProcessRelocationEntry over the secondary then the primary table, as runlinkDownloadCode does.
    void link_overlay(const Overlay& ov) {
        const size_t tables = (size_t)ov.rom + ov.text + ov.data;
        const std::vector<Reloc> sec = read_relocs(tables + ov.rel1, ov.rel2);
        const std::vector<Reloc> prim = read_relocs(tables, ov.rel1);
        for (const std::vector<Reloc>* table : {&sec, &prim}) {
            for (size_t k = 0; k < table->size(); k++) {
                const Reloc& r = (*table)[k];
                const size_t o = (size_t)ov.rom + (r.type == TYPE_DATA ? ov.text : 0) + r.off;
                const uint32_t word = rd(o);
                bool loaded;
                uint32_t addr = resolve(r, ov, word, loaded);
                if (r.patch == PATCH_HI16 && k + 1 < table->size()) {
                    const Reloc& r2 = (*table)[k + 1];
                    const size_t o2 = (size_t)ov.rom + r2.off;
                    if (!loaded) addr = UNRESOLVED_SYMBOL;
                    const uint32_t addend = (uint32_t)(((word & 0xFFFF) << 16) + sext16(rd(o2)));
                    if (addend != UNRESOLVED_SYMBOL) addr += addend;
                    put_hi(o, addr);
                    put_lo(o2, addr);
                    k++;
                } else if (r.patch == PATCH_LO16) {
                    if (!loaded) addr = UNRESOLVED_SYMBOL;
                    put_lo(o, addr + (word & 0xFFFF));
                } else if (r.patch == PATCH_WORD) {
                    wr(o, addr);
                } else if (r.patch == PATCH_JAL) {
                    wr(o, (word & 0xFC000000) | ((addr >> 2) & 0x03FFFFFF));
                }
            }
        }
    }
};

// --- recomp_printf ---------------------------------------------------------------------------------------------
// o32 varargs: argument slot i is $a0..$a3 for i < 4, then the stack at $sp + 4 * i. A double (and a long long)
// starts at an even slot and takes two, high word first.
struct VarArgs {
    uint8_t* rdram;
    recomp_context* ctx;
    unsigned slot = 1; // slot 0 is the format string
    uint32_t word(unsigned i) const {
        switch (i) {
            case 0: return (uint32_t)ctx->r4;
            case 1: return (uint32_t)ctx->r5;
            case 2: return (uint32_t)ctx->r6;
            case 3: return (uint32_t)ctx->r7;
            default: return (uint32_t)MEM_W(4 * i, ctx->r29);
        }
    }
    uint32_t next32() { return word(slot++); }
    uint64_t next64() {
        slot = (slot + 1) & ~1u;
        const uint64_t hi = word(slot), lo = word(slot + 1);
        slot += 2;
        return hi << 32 | lo;
    }
};

std::string read_guest_string(uint8_t* rdram, gpr addr, size_t limit = 4096) {
    std::string s;
    for (size_t i = 0; i < limit; i++) {
        const char c = (char)MEM_B(i, addr);
        if (c == '\0') break;
        s.push_back(c);
    }
    return s;
}

std::string guest_format(uint8_t* rdram, recomp_context* ctx) {
    const std::string fmt = read_guest_string(rdram, ctx->r4);
    VarArgs args{rdram, ctx};
    std::string out;
    char buf[512];
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out.push_back(fmt[i]); continue; }
        // %[flags][width][.precision][length]conversion; '*' width/precision not supported
        size_t j = i + 1;
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) j++;
        while (j < fmt.size() && (std::isdigit((unsigned char)fmt[j]) || fmt[j] == '.')) j++;
        int longs = 0;
        bool half = false;
        while (j < fmt.size() && std::strchr("hlzjt", fmt[j])) { if (fmt[j] == 'l') longs++; if (fmt[j] == 'h') half = true; j++; }
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        std::string spec = fmt.substr(i, j - i);
        spec.erase(std::remove_if(spec.begin(), spec.end(), [](char c) { return std::strchr("hlzjt", c) != nullptr; }), spec.end());
        switch (conv) {
            case '%': out.push_back('%'); break;
            case 'd': case 'i':
                if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)args.next64());
                else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), half ? (int)(int16_t)args.next32() : (int)(int32_t)args.next32());
                out += buf; break;
            case 'u': case 'x': case 'X': case 'o':
                if (longs >= 2) std::snprintf(buf, sizeof buf, (spec + "ll" + conv).c_str(), (unsigned long long)args.next64());
                else std::snprintf(buf, sizeof buf, (spec + conv).c_str(), half ? (unsigned)(uint16_t)args.next32() : (unsigned)args.next32());
                out += buf; break;
            case 'c': std::snprintf(buf, sizeof buf, (spec + 'c').c_str(), (int)(char)args.next32()); out += buf; break;
            case 'p': std::snprintf(buf, sizeof buf, "0x%08X", args.next32()); out += buf; break;
            case 's': {
                const uint32_t p = args.next32();
                const std::string s = p ? read_guest_string(rdram, (gpr)(int32_t)p) : std::string("(null)");
                std::snprintf(buf, sizeof buf, (spec + 's').c_str(), s.c_str());
                out += buf; break;
            }
            case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
                const uint64_t bits = args.next64();
                double d;
                std::memcpy(&d, &bits, sizeof d);
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), d);
                out += buf; break;
            }
            default: out += fmt.substr(i, j - i + 1); break;
        }
        i = j;
    }
    return out;
}

} // namespace

// int recomp_printf(const char* fmt, ...): mods import it from "*" (recomputils.h in the mod templates).
extern "C" void jfg_recomp_printf(uint8_t* rdram, recomp_context* ctx) {
    std::string text = guest_format(rdram, ctx);
    const int len = (int)text.size();
    // One log line per line of output; the logger adds the time stamp and the newline.
    size_t start = 0;
    while (start < text.size()) {
        size_t nl = text.find('\n', start);
        if (nl == std::string::npos) nl = text.size();
        if (nl > start) jfg::jfg_log_line("jfg_mods.log", text.substr(start, nl - start).c_str());
        start = nl + 1;
    }
    ctx->r2 = len;
}

// Called from jfg_on_init (jfg_glue.cpp): after the runtime has built its function table, before mods are loaded.
// A regenerated (hooked) overlay function calls the other functions of its own overlay by address lookup
// (librecomp's apply_regenlist sets use_lookup_for_all_function_calls), and in the pre-linked code those addresses
// are the private link addresses (0x82000000 upward), which the runtime's table doesn't have: the lookup failed and
// the game quit. So every overlay function is also entered under its private address. That's safe: the recompiled
// code finds its section's real load address at run time, whichever address it was called through, and it never
// collides with a real one (RDRAM ends at 0x80800000).
extern "C" void jfg_mods_alias_overlays(void) {
    Linker lk(recomp::get_rom());
    const auto& by_rom = recomp::overlays::get_vrom_to_section_map();
    int sections = 0, funcs = 0;
    for (const Overlay& ov : lk.overlays()) {
        auto it = by_rom.find(ov.rom);
        if (ov.text == 0 || it == by_rom.end()) {
            continue;
        }
        if (recomp::overlays::get_section_ram_addr(it->second) != ov.link) {
            continue; // not laid out the way prepare_overlays.py did it: leave this section alone
        }
        sections++;
        for (uint32_t off = 0; off < ov.text; off += 4) {
            if (recomp_func_t* f = recomp::overlays::get_func_by_section_index_function_offset(it->second, off)) {
                recomp::overlays::add_loaded_function((int32_t)(ov.link + off), f);
                funcs++;
            }
        }
    }
    char line[128];
    std::snprintf(line, sizeof line, "mods: %d functions of %d overlays callable at their private addresses", funcs,
                  sections);
    jfg::jfg_log_line("jfg_mods.log", line);
}

extern "C" void jfg_register_asset_mods(void); // jfg_assets.cpp

// Called from src/main.cpp before the runtime starts.
// RecompiledFuncs/jfg_wrapped.c (tools/wrap_functions.py): every game function the port wraps, with its original.
extern "C" void jfg_register_wrapped(void (*reg)(recomp_func_t* wrapped, recomp_func_t* original));

extern "C" void jfg_register_mod_exports(void) {
    recomp::overlays::register_base_export("recomp_printf", jfg_recomp_printf);
    jfg_register_wrapped(recomp::overlays::register_base_wrapper); // mods patch and hook the originals, not the wrappers
    jfg::api::register_api(); // the JFG mod API's events and helpers (jfg_api.cpp)
    jfg_register_asset_mods(); // mods that replace game data (jfg_assets.json)
}

// GameEntry::decompression_routine (src/main.cpp): the ROM the runtime regenerates hooked functions from.
std::vector<uint8_t> jfg_prelink_rom(std::span<const uint8_t> rom) {
    Linker lk(rom);
    const int n = lk.link_all();
    if (n < 0) {
        jfg::jfg_log_line("jfg_mods.log", "mods: the ROM's overlay tables don't fit; hooks use the ROM as it is");
        return std::vector<uint8_t>(rom.begin(), rom.end());
    }
    char line[128];
    std::snprintf(line, sizeof line, "mods: pre-linked %d overlays for hooks (private addresses from 0x%08X)", n,
                  LINK_BASE);
    jfg::jfg_log_line("jfg_mods.log", line);
    return lk.take();
}
