// Jet Force Gemini port: "live relocation" helpers used by generated code after tools/fix_dynamic_relocs.py.
//
// The game's runtime linker (src/runLink.c) keeps patching certain instructions in RDRAM as overlays are loaded and
// unloaded: calls into other overlays (`jal`) and address materialisation of other overlays' symbols (`lui`/`lo16`).
// Statically recompiled code cannot have those values baked in, so at exactly those instructions it reads the
// current instruction word from RDRAM instead. The game's own linker therefore remains the single source of truth.
#ifndef JFG_DYNAMIC_H
#define JFG_DYNAMIC_H

// Current 16-bit immediate of the instruction at `site` (big-endian word: immediate is the low half).
#define JFG_DYN_IMM(site) ((uint16_t)MEM_HU(2, (gpr)(int32_t)(site)))

// `jal` whose target the linker patches: TrapDanglingJump while the callee's overlay is unloaded, the real function
// once it is loaded. Sets $ra like the hardware would (TrapDanglingJump identifies its caller from $ra - 8).
#define JFG_DYN_CALL(site)                                                                    \
    do {                                                                                      \
        uint32_t jfg_site_ = (uint32_t)(site);                                                \
        uint32_t jfg_word_ = (uint32_t)MEM_W(0, (gpr)(int32_t)jfg_site_);                     \
        ctx->r31 = (gpr)(int32_t)(jfg_site_ + 8u);                                            \
        LOOKUP_FUNC((int32_t)(0x80000000u | ((jfg_word_ & 0x03FFFFFFu) << 2)))(rdram, ctx);   \
    } while (0)

#endif
