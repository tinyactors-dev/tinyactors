#pragma once

#include "system.h"
#include "datamodel.h"

/*
 * Per-call interpreter scratch. The actor holds all state between microsteps,
 * so a budget pauses only at microstep boundaries, which the actor fully
 * describes: configuration, history, internal queue, and pending event.
 */
typedef struct {
    ta_system *system;
    ta_actor *actor;
    const ta_definition *definition;
    uint32_t budget;
    uint32_t steps;
    uint32_t words;
    uint64_t *active;
    uint64_t *to_exit;
    uint64_t *to_enter;
    uint64_t *default_entry;
    uint64_t *domain_targets;
    uint64_t *entry_targets;
    uint64_t *exit_left;
    uint64_t *exit_right;
    ta_state_id *default_history;
    ta_transition_id *enabled;
    uint32_t enabled_count;
    ta_transition_id *filtered;
    bool *remove;
} ta_interpreter;

ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/* Takes ownership of the event's data. */
ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send);
void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);

/* Send IDs as text: literal IDs are their author's text; generated ones round-trip. */
void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer);
/* Returns 0 when the text names no send ID. */
ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text);
