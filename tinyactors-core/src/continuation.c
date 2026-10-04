#include "continuation.h"

/*
 * The sets are the first array and start right after the header, so their
 * offset is not stored: the header is a multiple of the sets' alignment.
 */
_Static_assert(sizeof(ta_continuation) % _Alignof(uint64_t) == 0, "the sets start at sizeof(ta_continuation)");

ta_result ta_continuation_bytes(const ta_continuation *shape, size_t *bytes, ta_continuation_offsets *offsets) {
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_continuation), _Alignof(ta_continuation), 1u });
    /* Four sets of `words` 64-bit words each, at sizeof(ta_continuation). */
    ta_layout_add(&layout, &(ta_layout_entry){ 4u * sizeof(uint64_t), _Alignof(uint64_t), shape->words });
    size_t decls = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(uint64_t), _Alignof(uint64_t), shape->decl_words });
    size_t history = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_state_id), _Alignof(ta_state_id),
                                                                shape->state_count });
    size_t enabled = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_transition_id), _Alignof(ta_transition_id),
                                                                shape->enabled_count });
    size_t frames = ta_layout_add(&layout, &(ta_layout_entry){ sizeof(ta_exec_frame), _Alignof(ta_exec_frame),
                                                               shape->frame_count });
    size_t end = 0;
    if (ta_layout_finish(&layout, &end, NULL) != TA_OK || end > UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    *bytes = end;
    if (offsets != NULL) {
        *offsets = (ta_continuation_offsets){ .decls = (uint32_t)decls, .history = (uint32_t)history,
                                              .enabled = (uint32_t)enabled, .frames = (uint32_t)frames };
    }
    return TA_OK;
}

uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + sizeof(ta_continuation));
}

uint64_t *ta_continuation_decls(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + continuation->offsets.decls);
}

ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)((unsigned char *)continuation + continuation->offsets.history);
}

ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)((unsigned char *)continuation + continuation->offsets.enabled);
}

ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)((unsigned char *)continuation + continuation->offsets.frames);
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
