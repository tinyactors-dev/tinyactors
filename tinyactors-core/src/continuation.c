#include "continuation.h"
#include "layout.h"
#include "memory.h"
#include "../include/tinyactors.h"

/* The regions of a continuation's block, in the order they are laid out. */
enum {
    TA_CONTINUATION_HEADER = 0,
    TA_CONTINUATION_SETS,
    TA_CONTINUATION_DECLS,
    TA_CONTINUATION_HISTORY,
    TA_CONTINUATION_ENABLED,
    TA_CONTINUATION_FRAMES,
    TA_CONTINUATION_REGIONS
};

/* Lays out a continuation of `shape`'s counts: each region's offset into
 * `offsets`, and the block's size and alignment into `*layout`. */
static ta_result ta_continuation_lay_out(const ta_continuation *shape, size_t offsets[TA_CONTINUATION_REGIONS],
                                         ta_layout *layout) {
    const ta_layout_entry entries[TA_CONTINUATION_REGIONS] = {
        [TA_CONTINUATION_HEADER] = { .size = sizeof(ta_continuation), .alignment = _Alignof(ta_continuation),
                                     .count = 1 },
        /* active, to exit, to enter and default entry, `words` each, side by side */
        [TA_CONTINUATION_SETS] = { .size = 4u * sizeof(uint64_t), .alignment = _Alignof(uint64_t),
                                   .count = shape->words },
        [TA_CONTINUATION_DECLS] = { .size = sizeof(uint64_t), .alignment = _Alignof(uint64_t),
                                    .count = shape->decl_words },
        [TA_CONTINUATION_HISTORY] = { .size = sizeof(ta_state_id), .alignment = _Alignof(ta_state_id),
                                      .count = shape->state_count },
        [TA_CONTINUATION_ENABLED] = { .size = sizeof(ta_transition_id), .alignment = _Alignof(ta_transition_id),
                                      .count = shape->enabled_count },
        [TA_CONTINUATION_FRAMES] = { .size = sizeof(ta_exec_frame), .alignment = _Alignof(ta_exec_frame),
                                     .count = shape->frame_count },
    };
    ta_layout_init(layout);
    for (uint32_t region = 0; region < TA_CONTINUATION_REGIONS; region += 1u) {
        ta_result result = ta_layout_add(layout, &entries[region], &offsets[region]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* Where `region` of an allocated continuation starts. Its counts are no
 * larger than those its allocation was sized for, so laying it out again
 * cannot fail. */
static unsigned char *ta_continuation_region(ta_continuation *continuation, uint32_t region) {
    size_t offsets[TA_CONTINUATION_REGIONS] = { 0 };
    ta_layout layout;
    (void)ta_continuation_lay_out(continuation, offsets, &layout);
    return (unsigned char *)continuation + offsets[region];
}

ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *out) {
    size_t offsets[TA_CONTINUATION_REGIONS];
    ta_layout layout;
    ta_result result = ta_continuation_lay_out(shape, offsets, &layout);
    if (result == TA_OK) {
        *out = layout.bytes;
    }
    return result;
}

uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)ta_continuation_region(continuation, TA_CONTINUATION_SETS);
}

uint64_t *ta_continuation_decls(ta_continuation *continuation) {
    return (uint64_t *)ta_continuation_region(continuation, TA_CONTINUATION_DECLS);
}

ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)ta_continuation_region(continuation, TA_CONTINUATION_HISTORY);
}

ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)ta_continuation_region(continuation, TA_CONTINUATION_ENABLED);
}

ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)ta_continuation_region(continuation, TA_CONTINUATION_FRAMES);
}

void ta_continuation_retain(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_retain(&frames[index].iteration_snapshot);
        }
    }
}

void ta_continuation_release(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frames[index].iteration_snapshot);
        }
    }
}

void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation) {
    ta_continuation *held = *continuation;
    if (held == NULL) {
        return;
    }
    ta_continuation_release(held);
    ta_mem_free(allocator, held, &(ta_allocator_free_params){ .size = held->bytes, .alignment = TA_CONTINUATION_ALIGN });
    *continuation = NULL;
}

bool ta_run_phase_in_microstep(ta_run_phase phase) {
    return phase == TA_PHASE_START || phase == TA_PHASE_EXIT || phase == TA_PHASE_TRANSITIONS ||
           phase == TA_PHASE_ENTER;
}
