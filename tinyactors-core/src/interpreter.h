#pragma once

#include "system.h"
#include "datamodel.h"

typedef struct {
    ta_system *system;
    ta_actor *actor;
    const ta_definition *definition;
    ta_dm_host dm_host;
    uint32_t budget;
    uint32_t steps;
} ta_interpreter;

ta_result ta_interpreter_deliver(ta_interpreter *it, const ta_event *event);
ta_result ta_interpreter_resume(ta_interpreter *it, ta_run_status *status);
ta_result ta_interpreter_microstep(ta_interpreter *it);
ta_result ta_interpreter_select_transitions(ta_interpreter *it);
ta_result ta_interpreter_exit_states(ta_interpreter *it);
ta_result ta_interpreter_execute_transitions(ta_interpreter *it);
ta_result ta_interpreter_enter_states(ta_interpreter *it);
ta_result ta_interpreter_process_internal(ta_interpreter *it);
