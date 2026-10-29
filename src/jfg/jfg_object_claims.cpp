// Jet Force Gemini port: objects a mod claims as its own (mod API jfg_object_claim), so two mods don't both drive the
// same object (a plasma ball one mod bounces and another pulls, a grenade one mod takes as its weapon's).
//
// A claim is kept by the mod's index. It goes when the mod lets go (jfg_object_unclaim), at a level change, and when the
// object is freed: after each objDoFrees, claims on objects no longer in either object list are dropped, before the
// address can be given to a new object. A plasma ball a mod makes with jfg_fire_plasma is that mod's from the start.
#include <algorithm>
#include <unordered_map>

#include "librecomp/overlays.hpp"
#include "jfg_api.h"
#include "jfg_game.h"
#include "jfg_hooks.h"
#include "jfg_port.h"

namespace jfg {

namespace {
constexpr uint32_t ADDR_NoAddObjList = 0x800F2CB4, ADDR_NoAddObjListCount = 0x800F2CB8;
constexpr int OBJECTS_MAX = 512, NO_ADD_MAX = 64;

std::unordered_map<uint32_t, size_t> g_claims; // object -> the mod that claimed it

// jfg_object_claim(void* obj) -> 1 if the object is now (or already was) the calling mod's, 0 if another mod has it.
void object_claim(uint8_t* rdram_, recomp_context* ctx, size_t mod_index) {
    g_rdram = rdram_;
    const uint32_t obj = (uint32_t)ctx->r4;
    ctx->r2 = 0;
    if (!valid_ptr(obj)) return;
    const auto [it, added] = g_claims.try_emplace(obj, mod_index);
    ctx->r2 = it->second == mod_index ? 1 : 0;
}

// jfg_object_claimed(void* obj) -> 0 no mod's, 1 the calling mod's, 2 another mod's.
void object_claimed(uint8_t*, recomp_context* ctx, size_t mod_index) {
    const auto it = g_claims.find((uint32_t)ctx->r4);
    ctx->r2 = it == g_claims.end() ? 0 : (it->second == mod_index ? 1 : 2);
}

// jfg_object_unclaim(void* obj): the calling mod lets go of it.
void object_unclaim(uint8_t*, recomp_context* ctx, size_t mod_index) {
    const auto it = g_claims.find((uint32_t)ctx->r4);
    if (it != g_claims.end() && it->second == mod_index) g_claims.erase(it);
}

void claims_level_change(uint8_t*, recomp_context*) { g_claims.clear(); }
} // namespace

void claim_for_mod(uint32_t obj, size_t mod_index) {
    if (valid_ptr(obj)) g_claims[obj] = mod_index;
}

// Whether a claimed object is still there (not freed since it was claimed).
bool claim_alive(uint32_t obj) {
    return g_claims.count(obj) != 0;
}

void unclaim(uint32_t obj) {
    g_claims.erase(obj);
}

// After objDoFrees: claims on freed objects go.
void claims_after_frees(uint8_t* rdram_) {
    if (g_claims.empty()) return;
    g_rdram = rdram_;
    std::unordered_map<uint32_t, size_t> alive;
    const uint32_t lists[2][3] = { { ADDR_ObjList, ADDR_ObjListCount, OBJECTS_MAX },
                                   { ADDR_NoAddObjList, ADDR_NoAddObjListCount, NO_ADD_MAX } };
    for (const auto& l : lists) {
        const uint32_t list = rd32(l[0]);
        const int count = std::min((int)(int32_t)rd32(l[1]), (int)l[2]);
        if (!valid_ptr(list)) continue;
        for (int i = 0; i < count; i++) {
            const auto it = g_claims.find(rd32(list + 4u * (uint32_t)i));
            if (it != g_claims.end()) alive.insert(*it);
        }
    }
    g_claims.swap(alive);
}

void register_claim_api() {
    recomp::overlays::register_ext_base_export("jfg_object_claim", object_claim);
    recomp::overlays::register_ext_base_export("jfg_object_claimed", object_claimed);
    recomp::overlays::register_ext_base_export("jfg_object_unclaim", object_unclaim);
    api::add_internal(api::Event::LevelChange, claims_level_change);
}

} // namespace jfg
