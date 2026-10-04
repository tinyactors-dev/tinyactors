#pragma once

#include "event.h"
#include "../include/tinyactors.h"

typedef enum {
    TA_FAULT_CONTEXT_CONDITION,
    TA_FAULT_CONTEXT_VALUE,
    TA_FAULT_CONTEXT_LOCATION,
    TA_FAULT_CONTEXT_ASSIGN,
    TA_FAULT_CONTEXT_SCRIPT,
    TA_FAULT_CONTEXT_SEND_EVAL,
    TA_FAULT_CONTEXT_SEND_DISPATCH,
    TA_FAULT_CONTEXT_INVOKE_START,
    TA_FAULT_CONTEXT_INVOKE_CANCEL,
    TA_FAULT_CONTEXT_INVOKE_FORWARD,
    TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE
} ta_fault_context;

typedef struct {
    ta_fault_context context;
    ta_send_id send_id;
    /* Used for naming/bookkeeping; does not populate ta_event.invoke_id. */
    ta_invoke_id invoke_id;
    /* May be null only for a core-generated invocation-unavailable failure. */
    const ta_fault *fault;
    ta_event *out;
} ta_fault_event_params;

ta_event_name_id ta_fault_event_name(ta_system *system,
                                     const ta_fault_event_params *params);
ta_result ta_fault_to_internal_event(ta_system *system,
                                     const ta_fault_event_params *params);
