#pragma once

#include "event.h"
#include "routing.h"

typedef enum {
    TA_CONT_IDLE = 0,
    TA_CONT_SELECT_TRANSITIONS,
    TA_CONT_EXIT_STATES,
    TA_CONT_EXEC_TRANSITIONS,
    TA_CONT_ENTER_STATES,
    TA_CONT_PROCESS_INTERNAL,
    TA_CONT_INVOKE_STATES,
    TA_CONT_FINISH_MACROSTEP
} ta_continuation_phase;

typedef enum {
    TA_EXEC_FRAME_ACTIONS = 0,
    TA_EXEC_FRAME_FOREACH
} ta_exec_frame_kind;

typedef struct {
    ta_exec_frame_kind kind;
    ta_span32 actions;
    uint32_t cursor;
    uint32_t iteration;
    uint32_t iteration_count;
    ta_value iteration_snapshot;
} ta_exec_frame;

typedef struct ta_continuation ta_continuation;
struct ta_continuation {
    uint32_t pc;
    uint32_t cursor;
    uint32_t aux;
    ta_continuation_phase phase;
    bool has_external_event;
    ta_event external_event;
    /* Owned deep clone; null unless the external router requested retry. */
    ta_external_delivery *pending_external_delivery;
    ta_transition_id *candidates;
    uint32_t candidate_count;
    ta_transition_id *selected;
    uint32_t selected_count;
    ta_state_id *states_to_exit;
    uint32_t exit_count;
    ta_state_id *states_to_enter;
    uint32_t entry_count;
    ta_state_id *states_to_invoke;
    uint32_t invoke_count;
    ta_exec_frame *frames;
    uint32_t frame_count;
};
