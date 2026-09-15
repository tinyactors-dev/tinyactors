#pragma once

#include "event.h"

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
    TA_FAULT_CONTEXT_INVOKE_FORWARD
} ta_fault_context;

ta_event_name_id ta_fault_event_name(ta_system *system,
                                     ta_fault_context context,
                                     const ta_fault *fault);
ta_result ta_fault_to_internal_event(ta_system *system,
                                     ta_fault_context context,
                                     ta_send_id send_id,
                                     const ta_fault *fault,
                                     ta_event *out);
