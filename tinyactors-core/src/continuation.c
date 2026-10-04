#include "continuation.h"

/*
 * Offsets of the arrays that follow a continuation's header, in order, from
 * the start of the allocation. `end` is the unpadded end of the frames, which
 * is the number of bytes the shape needs.
 */
typedef struct {
    size_t sets;
    size_t decls;
    size_t history;
    size_t enabled;
    size_t frames;
    size_t end;
} ta_continuation_layout;

/*
 * Lays out the shape's arrays after the header, each at its own alignment.
 * The layout has call lifetime and nothing is stored: it is recomputed from
 * the header's counts on every use, so a continuation carries no offsets
 * that could disagree with them. It answers TA_ERR_NOMEM, and fills nothing
 * in `*out`, when the shape does not fit in a size_t.
 */
static ta_result ta_continuation_offsets(const ta_continuation *shape, ta_continuation_layout *out) {
    ta_layout layout;
    ta_continuation_layout offsets;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_continuation), _Alignof(ta_continuation), 1u });
    /* Four sets of `words` 64-bit words each. */
    offsets.sets = ta_layout_add(&layout, &(ta_layout_entry){ 4u * sizeof(uint64_t), _Alignof(uint64_t), shape->words });
    offsets.decls = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint64_t), _Alignof(uint64_t), shape->decl_words });
    offsets.history = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_state_id), _Alignof(ta_state_id), shape->state_count });
    offsets.enabled = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_transition_id), _Alignof(ta_transition_id),
                                                                 shape->enabled_count });
    offsets.frames = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_exec_frame), _Alignof(ta_exec_frame), shape->frame_count });
    ta_result result = ta_layout_finish(&layout, &offsets.end, NULL);
    if (result == TA_OK) {
        *out = offsets;
    }
    return result;
}

size_t ta_continuation_bytes(const ta_continuation *shape) {
    ta_continuation_layout offsets;
    return ta_continuation_offsets(shape, &offsets) == TA_OK ? offsets.end : 0;
}

/*
 * The offsets of a continuation that exists. Its counts passed
 * ta_continuation_bytes when it was allocated (the one place that refuses an
 * overflowing shape), so this cannot fail. If it ever did, zeroed offsets
 * would make every accessor answer the header's own address and the caller
 * would write over it, so it traps instead of answering.
 */
static ta_continuation_layout ta_continuation_existing(const ta_continuation *continuation) {
    ta_continuation_layout offsets = { 0 };
    if (ta_continuation_offsets(continuation, &offsets) != TA_OK) {
        __builtin_trap();
    }
    return offsets;
}

uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + ta_continuation_existing(continuation).sets);
}

uint64_t *ta_continuation_decls(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + ta_continuation_existing(continuation).decls);
}

ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)((unsigned char *)continuation + ta_continuation_existing(continuation).history);
}

ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)((unsigned char *)continuation + ta_continuation_existing(continuation).enabled);
}

ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)((unsigned char *)continuation + ta_continuation_existing(continuation).frames);
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
