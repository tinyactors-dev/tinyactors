#include "continuation.h"

/* Offsets of the arrays that follow a continuation's header, in order. */
typedef struct {
    size_t sets;
    size_t decls;
    size_t history;
    size_t enabled;
    size_t frames;
    size_t end;
} ta_continuation_layout;

static ta_continuation_layout ta_continuation_offsets(const ta_continuation *shape) {
    ta_continuation_layout layout;
    layout.sets = ta_align_up(sizeof(ta_continuation), _Alignof(uint64_t));
    layout.decls = layout.sets + (size_t)shape->words * 4u * sizeof(uint64_t);
    layout.history = ta_align_up(layout.decls + (size_t)shape->decl_words * sizeof(uint64_t), _Alignof(ta_state_id));
    layout.enabled = ta_align_up(layout.history + (size_t)shape->state_count * sizeof(ta_state_id),
                                 _Alignof(ta_transition_id));
    layout.frames = ta_align_up(layout.enabled + (size_t)shape->enabled_count * sizeof(ta_transition_id),
                                _Alignof(ta_exec_frame));
    layout.end = layout.frames + (size_t)shape->frame_count * sizeof(ta_exec_frame);
    return layout;
}

size_t ta_continuation_bytes(const ta_continuation *shape) {
    return ta_continuation_offsets(shape).end;
}

uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + ta_continuation_offsets(continuation).sets);
}

uint64_t *ta_continuation_decls(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + ta_continuation_offsets(continuation).decls);
}

ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)((unsigned char *)continuation + ta_continuation_offsets(continuation).history);
}

ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)((unsigned char *)continuation + ta_continuation_offsets(continuation).enabled);
}

ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)((unsigned char *)continuation + ta_continuation_offsets(continuation).frames);
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
