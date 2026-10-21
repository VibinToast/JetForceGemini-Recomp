// Jet Force Gemini port: object IDs for RT64's frame interpolation ("60 + Interpolated" and the Graphics menu's
// Display/Manual rates). RT64 makes the in-between frames by matching each matrix to the same one in the frame before.
// The game writes its matrices into a heap that is reused every other frame, so a matrix's address says nothing about
// which object it belongs to. Every model matrix the game writes (camPushModelMtx, camPushFloatModelMtx,
// camPushMuzzleMtx, camScaleModelMtx) is tagged for RT64 (lib/rt64 src/hle/rt64_rsp.cpp, jfgMatrixId) with:
// - an ID, while objPrintObject is drawing an object: the object, the place the matrix was pushed from and what it
//   draws (so its body, weapon parts and muzzle flash differ, and one that comes and goes does not shift the others).
//   RT64 gives the object's bones IDs of their own from it. Matrices pushed outside an object's drawing (particles,
//   sprites) get none and are not interpolated, as before;
// - the float model matrix the game multiplied with the camera. RT64 needs the model matrix without the camera, and
//   working it out from the fixed-point result gives one that differs a little every frame (objects shake).
#include <cstdint>
#include <cstring>

#include "jfg_game.h"
#include "jfg_hooks.h"

uint32_t rt64_jfg_matrix_check(const uint8_t* rdram, uint32_t address); // lib/rt64 src/hle/rt64_rsp.cpp (JFG port)
void rt64_jfg_tag_matrix(uint32_t address, uint32_t id, uint32_t check, const float* model);
void rt64_jfg_tag_bone_buffer(uint32_t address, uint32_t id);

namespace jfg {

namespace {
// The objects objPrintObject is drawing (it can draw other objects while drawing one). Game thread only.
uint32_t drawn_objects[16];
int drawn_count = 0;
uint32_t push_caller = 0; // return address of the push function running now
uint32_t push_source = 0; // its third argument: what it draws (transform, source matrix, position; the scale's bits)

constexpr uint32_t OBJ_DEFINITION = 0x40;     // Object: pointer to its definition (objects.h)
constexpr uint32_t OBJ_MODELS = 0x6C;         // Object: its model instances (ModelInstance_JFG*[], models.h)
constexpr uint32_t OBJ_MODEL_INDEX = 0x3A;    // Object: s8, the one drawn (segment.unk3A)
constexpr uint32_t INST_BONES = 0x10;         // ModelInstance_JFG: f32 *unk10[2], its two bone buffers (one per frame)

uint32_t object_id(uint32_t object) {
    // The object's address with its definition, so an object created where another one was freed differs.
    return (object * 2654435761u) ^ rd32(object + OBJ_DEFINITION);
}
constexpr uint32_t ADDR_modelMtx = 0x800FAAB0; // D_800FAAB0: the float model matrix the push functions multiply with the camera
constexpr uint32_t ADDR_lastMtx = 0x800FAB30;  // D_800FAB30: the Mtx the push function just wrote
}

// Before objPrintObject(dList, mtx, vtx, obj) (its wrapper, jfg_api.cpp). A character's model has no model matrix of its own: its bones are
// world-space matrices the game converts into a bone buffer of the model instance, and the model's display lists
// multiply them onto the camera from there (G_DMAOFFSETS points at the buffer). Both of the instance's buffers are
// tagged with the object, so RT64 can tell whose bones they are.
extern "C" void jfg_interp_object(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    const uint32_t object = (uint32_t)ctx->r7;
    if (drawn_count < 16) drawn_objects[drawn_count] = object;
    drawn_count++;
    if (!valid_ptr(object)) return;

    const uint32_t models = rd32(object + OBJ_MODELS);
    const int index = (int8_t)rd8(object + OBJ_MODEL_INDEX);
    if (!valid_ptr(models) || index < 0 || index > 15) return;
    const uint32_t instance = rd32(models + (uint32_t)index * 4);
    if (!valid_ptr(instance)) return;
    const uint32_t id = (object_id(object) ^ ((uint32_t)index * 2246822519u)) | 1u; // never RT64's 0 ("not interpolated")
    for (uint32_t i = 0; i < 2; i++) {
        const uint32_t buffer = rd32(instance + INST_BONES + i * 4);
        if (valid_ptr(buffer)) rt64_jfg_tag_bone_buffer(buffer & 0x00FFFFFFu, id == 0xFFFFFFFFu ? 1u : id);
    }
}

// After objPrintObject (its wrapper, jfg_api.cpp).
extern "C" void jfg_interp_object_done(uint8_t* rdram_, recomp_context* ctx) {
    (void)rdram_; (void)ctx;
    if (drawn_count > 0) drawn_count--;
}

// Entry of each push function: where it was called from, and its third argument ($a2), which tells apart several
// Before each push function: where it was called from, and its third argument ($a2), which tells apart several
// matrices pushed from the same place (a character's weapon parts): camPushModelMtx's transform, camPushFloatModelMtx's
// source matrix, camPushMuzzleMtx's position, camScaleModelMtx's scale.
extern "C" void jfg_interp_push(uint8_t* rdram_, recomp_context* ctx) {
    g_rdram = rdram_;
    push_caller = (uint32_t)ctx->r31;
    push_source = (uint32_t)ctx->r6;
}

// Each push function right after it noted the matrix it wrote (D_800FAB30). A matrix pushed outside an object's
// Each push function after it wrote the matrix and noted it (D_800FAB30; its wrapper). A matrix pushed outside an object's
// drawing is tagged with ID 0, so a tag left at that address by an earlier frame cannot apply to it.
extern "C" void jfg_interp_pushed(uint8_t* rdram_, recomp_context* ctx) {
    (void)ctx;
    g_rdram = rdram_;
    const uint32_t mtx = rd32(ADDR_lastMtx);
    if (!valid_ptr(mtx)) return;

    uint32_t id = 0;
    const uint32_t object = (drawn_count > 0 && drawn_count <= 16) ? drawn_objects[drawn_count - 1] : 0;
    if (valid_ptr(object)) {
        // The object, the place the matrix was pushed from and what it draws.
        id = object_id(object) ^ (push_caller * 40503u) ^ (push_source * 2246822519u);
        if (id == 0 || id == 0xFFFFFFFFu) id = 1; // RT64's "not interpolated" and "match automatically" values
    }

    float model[16];
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t bits = rd32(ADDR_modelMtx + i * 4);
        std::memcpy(&model[i], &bits, sizeof(float));
    }

    const uint32_t address = mtx & 0x00FFFFFFu;
    rt64_jfg_tag_matrix(address, id, rt64_jfg_matrix_check(rdram_, address), model);
}

} // namespace jfg

// The four matrix push functions: jfg_interp_push before, jfg_interp_pushed after.
JFG_WRAP(camPushModelMtx) {
    jfg_interp_push(rdram, ctx);
    camPushModelMtx_orig(rdram, ctx);
    jfg_interp_pushed(rdram, ctx);
}

JFG_WRAP(camPushFloatModelMtx) {
    jfg_interp_push(rdram, ctx);
    camPushFloatModelMtx_orig(rdram, ctx);
    jfg_interp_pushed(rdram, ctx);
}
