// Jet Force Gemini port: pre-linking the game's overlays (jfg_prelink.h). The same algorithm as
// tools/prepare_overlays.py's Linker; tools/prelink_check.cpp checks that both give the same bytes.
#include "jfg_prelink.h"

#include <cstddef>

namespace jfg::prelink {
namespace {

// US ROM layout (ver/splat/jfg.us.yaml) and main-segment symbols (ver/symbols/symbol_addrs.us.txt).
constexpr uint32_t OVERLAY_ROM_TABLE = 0x1ED0270, OVERLAY_TABLE = 0x1ED2780, OVERLAY_CODE = 0x1ED3B20;
constexpr uint32_t CODE_START = 0x80000450;        // __CODE_SECTION_START
constexpr uint32_t DATA_START = 0x800A0660;        // __DATA_SECTION_START
constexpr uint32_t BSS_START = 0x800B0B50;         // __BSS_SECTION_START
constexpr uint32_t TRAP_DANGLING_JUMP = 0x80054530;
constexpr uint32_t UNRESOLVED_SYMBOL = 0x800FEAD4; // &gUnresolvedSymbolAddr

constexpr uint32_t PATCH_WORD = 2, PATCH_JAL = 4, PATCH_HI16 = 5, PATCH_LO16 = 6;
constexpr uint32_t TYPE_EXTERNAL = 0, TYPE_LOCAL = 1, TYPE_DATA = 3; // 2 = jump within the overlay
constexpr uint32_t SECTION_DATA_1 = 0xFFD, SECTION_DATA_2 = 0xFFE, SECTION_BSS = 0xFFF;

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

} // namespace

std::vector<Overlay> overlays(std::span<const uint8_t> rom) {
    return Linker(rom).overlays();
}

int link(std::span<const uint8_t> rom, std::vector<uint8_t>& out) {
    Linker lk(rom);
    const int n = lk.link_all();
    out = n < 0 ? std::vector<uint8_t>(rom.begin(), rom.end()) : lk.take();
    return n;
}

} // namespace jfg::prelink
