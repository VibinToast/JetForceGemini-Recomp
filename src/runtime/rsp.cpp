#include "rsp.hpp"

#include <cinttypes>

#include "ultramodern/ultra64.h"
#include "librecomp/rsp.hpp"

extern RspUcodeFunc aspMain;

namespace dino::runtime {

static RspExitReason empty_rsp_task(uint8_t*, uint32_t) {
    return RspExitReason::Broke;
}

RspUcodeFunc* get_rsp_microcode(const OSTask* task) {
    switch (task->t.type) {
    case M_AUDTASK:
        // DKR occasionally submits an audio task with no commands so the
        // scheduler can keep its normal completion flow. The retail RSP has
        // no work to perform; entering the generated ABI3 interpreter would
        // instead read a stale command and jump through an empty table slot.
        if (task->t.data_size == 0) {
            return empty_rsp_task;
        }
        return aspMain;
    default:
        fprintf(stderr, "Unknown task: %" PRIu32 "\n", task->t.type);
        return nullptr;
    }
}

}
