#pragma once

#include "event.h"

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

typedef struct {
    uint32_t pc;
    uint32_t cursor;
    uint32_t aux;
    ta_continuation_phase phase;
    bool has_external_event;
    ta_event external_event;
} ta_continuation;
