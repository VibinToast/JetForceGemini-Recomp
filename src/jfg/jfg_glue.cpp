// Jet Force Gemini port glue.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "recomp.h"

// N64Recomp only emits this when the toml has an `entrypoint`; ours declares the entrypoint as a manual
// function because the decomp ELF marks `entrypoint` as an absolute symbol.
gpr get_entrypoint_address() { return (gpr)(int32_t)0x80000400u; }

// --- Overlays ----------------------------------------------------------------------------------------------------
// The game's runtime linker (src/runLink.c) still loads, relocates and frees overlays in emulated RDRAM exactly as on
// hardware. These hooks (see [[patches.hook]] in jfg.us.toml) tell the recomp runtime where an overlay's code now
// lives so that address-based calls (TrapDanglingJump's tail call, patched jals, function pointers) reach the
// natively compiled functions, and so RELOC_HI16/LO16 in overlay code use the right base.
extern "C" void load_overlays(uint32_t rom, int32_t ram_addr, uint32_t size);
extern "C" void unload_overlays(int32_t ram_addr, uint32_t size);

namespace {
// OverlayHeader (src/runLink.h), 0x20 bytes
constexpr int OVL_VRAM_BASE = 0x00, OVL_ROM_ADDRESS = 0x04, OVL_TEXT_SIZE = 0x08, OVL_DATA_SIZE = 0x0C;
bool jfg_log_overlays() {
    static const bool enabled = std::getenv("JFG_LOG_OVERLAYS") != nullptr;
    return enabled;
}
}

// Called from runlinkDownloadCode / runlinkResumeCode once the overlay's code is in memory.
// $fp = its OverlayHeader (RomAddress is absolute by then).
namespace {
struct LoadedOverlay { uint32_t rom; uint32_t size; };
std::unordered_map<uint32_t, LoadedOverlay> jfg_loaded_overlays;   // load address -> rom address and section size

// librecomp's unload_overlays(ram, size) treats a section that starts exactly at ram + size as overlapping (its
// end test is `>=`), decides that section is only "partially" unloaded and exits the game. The game's allocator
// often places one overlay right after another, so freeing the first one ended the game with no message (the
// "crash in a loading zone": a 0x30-byte overlay at 0x8034C620 freed while another was loaded at 0x8034C650).
// Work around it without changing the library: take the overlays that directly follow this one out first (the
// furthest one first, so none of them ever has a neighbour right behind it), unload this one, then put them back.
void unload_overlay_safely(uint32_t ram) {
    auto self = jfg_loaded_overlays.find(ram);
    if (self == jfg_loaded_overlays.end()) {
        return;
    }
    std::vector<std::pair<uint32_t, LoadedOverlay>> followers;
    uint32_t next = ram + self->second.size;
    for (auto it = jfg_loaded_overlays.find(next); it != jfg_loaded_overlays.end() && it->second.size != 0;
         it = jfg_loaded_overlays.find(next)) {
        followers.emplace_back(it->first, it->second);
        next = it->first + it->second.size;
    }
    for (auto f = followers.rbegin(); f != followers.rend(); ++f) {
        unload_overlays((int32_t)f->first, f->second.size);
    }
    unload_overlays((int32_t)ram, self->second.size);
    jfg_loaded_overlays.erase(self);
    for (const auto& f : followers) {
        load_overlays(f.second.rom, (int32_t)f.first, f.second.size);
    }
    if (!followers.empty() && jfg_log_overlays()) {
        fprintf(stderr, "[jfg] overlay unload at 0x%08X: re-registered %zu overlay(s) directly after it\n", ram,
                followers.size());
    }
}
}

// 1 when an overlay is loaded at this address (jfg_engine_wrappers.cpp, mmFree's wrapper).
extern "C" int jfg_overlay_is_loaded(uint32_t ram) {
    return jfg_loaded_overlays.find(ram) != jfg_loaded_overlays.end() ? 1 : 0;
}

// Where the overlay with this ROM address is loaded now, or 0 (jfg_mods.cpp, TrapDanglingJump's wrapper).
extern "C" uint32_t jfg_overlay_loaded_at(uint32_t rom) {
    for (const auto& [ram, ovl] : jfg_loaded_overlays) {
        if (ovl.rom == rom) return ram;
    }
    return 0;
}

extern "C" void jfg_overlay_loaded(uint8_t* rdram, recomp_context* ctx) {
    gpr header = ctx->r30;
    uint32_t ram = (uint32_t)MEM_W(OVL_VRAM_BASE, header);
    uint32_t rom = (uint32_t)MEM_W(OVL_ROM_ADDRESS, header);
    uint32_t size = (uint32_t)MEM_W(OVL_TEXT_SIZE, header) + (uint32_t)MEM_W(OVL_DATA_SIZE, header);
    if (jfg_log_overlays()) {
        fprintf(stderr, "[jfg] overlay load: rom 0x%08X -> ram 0x%08X (0x%X bytes)\n", rom, ram, size);
    }
    unload_overlay_safely(ram);
    jfg_loaded_overlays[ram] = LoadedOverlay{rom, size};
    load_overlays(rom, (int32_t)ram, size);
}

// Called from runlinkFreeCode / runlinkUnloadOverlay right before the overlay's memory is freed. $a0 = its address.
extern "C" void jfg_overlay_unloading(uint8_t* rdram, recomp_context* ctx) {
    uint32_t ram = (uint32_t)ctx->r4;
    if (jfg_loaded_overlays.find(ram) == jfg_loaded_overlays.end()) {
        fprintf(stderr, "[jfg] overlay unload at 0x%08X, but nothing was registered there\n", ram);
        return;
    }
    if (jfg_log_overlays()) {
        fprintf(stderr, "[jfg] overlay unload: ram 0x%08X\n", ram);
    }
    unload_overlay_safely(ram);
}

// --- CIC-6105 challenge/response (anti-piracy) ------------------------------------------------------------------
// arithmeticFunction (src/joy.c) -> osCic6105SendData / osCic6105StartGetData / osCic6105GetReadData (overlay 45)
// exchange a 64-byte PIF RAM image with the console's PIF through __osSiRawStartDma. With command bit 0x02 set in
// byte 0x3F, the PIF forwards the 30-nibble challenge at 0x30..0x3E to the cartridge's CIC-NUS-6105 and stores the
// chip's response in the same place. The game code is left untouched; this emulates that one PIF service.
// Response algorithm: the well-known CIC-NUS-6105 routine published by X-Scale and used by N64 emulators.
namespace {
uint8_t jfg_pif_ram[0x40];

void cic_nus_6105(const uint8_t* chl, uint8_t* rsp, int len) {
    static const uint8_t lut0[0x10] = {0x4, 0x7, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0xF, 0x8, 0xF, 0x6, 0x3, 0x6, 0x9};
    static const uint8_t lut1[0x10] = {0x4, 0x1, 0xA, 0x7, 0xE, 0x5, 0xE, 0x1, 0xC, 0x9, 0x8, 0x5, 0x6, 0x3, 0xC, 0x9};
    const uint8_t* lut = lut0;
    uint8_t key = 0xB;
    for (int i = 0; i < len; i++) {
        rsp[i] = (uint8_t)((key + 5 * chl[i]) & 0xF);
        key = lut[rsp[i]];
        int sgn = (rsp[i] >> 3) & 0x1;
        int mag = ((sgn == 1) ? ~rsp[i] : rsp[i]) & 0x7;
        int mod = (mag % 3 == 1) ? sgn : 1 - sgn;
        if (lut == lut1 && (rsp[i] == 0x1 || rsp[i] == 0x9)) mod = 1;
        if (lut == lut1 && (rsp[i] == 0xB || rsp[i] == 0xE)) mod = 0;
        lut = (mod == 1) ? lut1 : lut0;
    }
}

void pif_process_challenge() {
    uint8_t challenge[30], response[30];
    for (int i = 0; i < 15; i++) {
        challenge[i * 2] = (jfg_pif_ram[0x30 + i] >> 4) & 0xF;
        challenge[i * 2 + 1] = jfg_pif_ram[0x30 + i] & 0xF;
    }
    cic_nus_6105(challenge, response, 30);
    jfg_pif_ram[0x2E] = 0;
    jfg_pif_ram[0x2F] = 0;
    for (int i = 0; i < 15; i++) {
        jfg_pif_ram[0x30 + i] = (uint8_t)((response[i * 2] << 4) | response[i * 2 + 1]);
    }
}
}

namespace ultramodern { void send_si_message(); }

// s32 __osSiRawStartDma(s32 direction, void* dramAddr): OS_READ = 0 (PIF RAM -> RDRAM), OS_WRITE = 1.
extern "C" void __osSiRawStartDma_recomp(uint8_t* rdram, recomp_context* ctx) {
    const bool write = ctx->r4 != 0;
    const gpr buf = ctx->r5;
    if (write) {
        for (int i = 0; i < 0x40; i++) jfg_pif_ram[i] = (uint8_t)MEM_BU(i, buf);
        if (jfg_pif_ram[0x3F] & 0x02) {
            pif_process_challenge();
            jfg_pif_ram[0x3F] &= (uint8_t)~0x02;
        } else {
            fprintf(stderr, "[jfg] __osSiRawStartDma: unexpected PIF command byte 0x%02X (only the CIC challenge is emulated)\n",
                jfg_pif_ram[0x3F]);
        }
    } else {
        for (int i = 0; i < 0x40; i++) MEM_B(i, buf) = (int8_t)jfg_pif_ram[i];
    }
    ultramodern::send_si_message();   // the SI interrupt that completes the transfer
    ctx->r2 = 0;
}

// --- Hardware the game reads directly (KSEG1) ------------------------------------------------------------------
// A few game functions read hardware registers or boot leftovers through KSEG1 instead of through libultra. Recompiled
// code turns 0xA0000000 + x into rdram + 0x20000000 + x, past the emulated RDRAM, in the runtime's reserved but
// inaccessible range. Those pages are opened here and hold what genuine hardware delivers, so the game's own code
// runs unchanged:
// - osBootRamTest1_6105 / 2 (boot): words the CIC-6105 IPL3 leaves at physical 0x2FB1F4 / 0x2FE1C0, 0xAD090010 and
//   0xAD170014. The results are kept in D_800A331C / D_800A3320 and consulted later (mainSetDefaultGame,
//   mainSetGameFlag, diCpu); FALSE makes the game sabotage itself.
// - osRamTest4_6105 (overlay 156): the IPL3 leftover 0xC86E2000 at physical 0x2E8.
// - osCartTest3_6105 / 4 (overlays 111, 133): PI_STATUS (0xA4600010) reads idle, and the cartridge words at
//   0xB0000454 / 0xB0000A5C (the boot code) come from the user's ROM, so the comparison runs against real data.
// - __amHandleFrameMsg reads AI_LEN (0xA4500004) inlined; its wrapper (jfg_engine_wrappers.cpp) puts the runtime's
//   audio backend's value there first.
namespace recomp { std::span<const uint8_t> get_rom(); }

namespace {
bool open_hardware_page(uint8_t* rdram, uint32_t kseg1) {
    uint8_t* page = rdram + ((uint64_t)kseg1 - 0x80000000u) / 0x1000 * 0x1000;
#ifdef _WIN32
    DWORD old_protect;
    return VirtualProtect(page, 0x1000, PAGE_READWRITE, &old_protect) != 0;
#else
    return mprotect(page, 0x1000, PROT_READ | PROT_WRITE) == 0;
#endif
}

void map_hardware(uint8_t* rdram) {
    const uint32_t pages[] = { 0xA0000000u, 0xA02FB000u, 0xA02FE000u, 0xA4500000u, 0xA4600000u, 0xB0000000u };
    for (uint32_t page : pages) {
        if (!open_hardware_page(rdram, page)) fprintf(stderr, "[jfg] could not open the page for 0x%08X\n", page);
    }
    MEM_W(0, (gpr)(int32_t)0xA02FB1F4u) = (int32_t)0xAD090010u;
    MEM_W(0, (gpr)(int32_t)0xA02FE1C0u) = (int32_t)0xAD170014u;
    MEM_W(0, (gpr)(int32_t)0xA00002E8u) = (int32_t)0xC86E2000u;
    // the first 4 KB of the cartridge, as the words a KSEG1 read gets (the ROM image is big-endian)
    std::span<const uint8_t> rom = recomp::get_rom();
    uint32_t offset = address & 0x0FFFFFFFu;
    if ((size_t)offset + 4 > rom.size()) {
        fprintf(stderr, "[jfg] cartridge read outside ROM: 0x%08X\n", address);
        return 0;
    for (uint32_t off = 0; off + 4 <= 0x1000 && off + 4 <= rom.size(); off += 4) {
        const uint32_t word = ((uint32_t)rom[off] << 24) | ((uint32_t)rom[off + 1] << 16) | ((uint32_t)rom[off + 2] << 8) | rom[off + 3];
        MEM_W(0, (gpr)(int32_t)(0xB0000000u + off)) = (int32_t)word;
    }
    return ((uint32_t)rom[offset] << 24) | ((uint32_t)rom[offset + 1] << 16) | ((uint32_t)rom[offset + 2] << 8) | rom[offset + 3];
}
} // namespace

// --- osCicId ----------------------------------------------------------------------------------------------------
// IPL3 stores the CIC type it booted with at 0x80000310 (osCicId). The runtime's boot emulation leaves it 0, and
// the game checks it for 6105 in two places: controlPlayer (0x80033440; on mismatch it zeroes the current weapon's
// ammo with probability 1/2500 per tick - the "ammo randomly drops to 000" bug) and the crash handler
// func_80066EB0_67AB0 (infinite loop on mismatch). Provide the value a real cartridge boot leaves there.
extern "C" void jfg_mods_alias_overlays(void); // jfg_mods.cpp

extern "C" void jfg_on_init(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    MEM_W(0, (gpr)(int32_t)0x80000310) = 6105;
    map_hardware(rdram);
    // Mod support: overlay functions are also callable at their private link addresses (see jfg_mods.cpp). The
    // runtime calls this after building its function table and before loading mods.
    jfg_mods_alias_overlays();
}
