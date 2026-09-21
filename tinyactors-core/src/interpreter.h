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
    /* Bootstrap only: data declarations the instance's binding already supplies. */
    uint64_t *bound_decls;
} ta_interpreter;

ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/* Runs SCXML exitInterpreter for a cancelled invoked session, without done.invoke. */
ta_result ta_interpreter_cancel(ta_system *system, ta_actor *actor);
/* Takes ownership of the event's data. */
ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send);
void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);

/* Send IDs as text: literal IDs are their author's text; generated ones round-trip. */
void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer);
/* Returns 0 when the text names no send ID. */
ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text);
/* ------------------------------------------------------------------------
 * Interpreter services used by invoke.c
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_interpreter_value_params;

ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor);
ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params);
/* Queues error.execution; returns TA_OK unless queueing itself failed. */
ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault);
/* Runs a block; a failed block has already queued its error. */
ta_result ta_interpreter_run_actions(ta_interpreter *it, ta_span32 block);
bool ta_interpreter_internal_pending(const ta_actor *actor);

/* ------------------------------------------------------------------------
 * Invoke hooks, implemented in invoke.c
 */

/* Interns an SCXML invoke ID's text; the runtime ID is its atom + 1. */
ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out);
ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke);
ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state);
ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state);
/* Starts the invokes of states entered during the finished macrostep. */
ta_result ta_invoke_start_pending(ta_interpreter *it);
/* Applies <finalize> and autoforwarding to the actor's current (external) event. */
ta_result ta_invoke_before_external(ta_interpreter *it);
/* Cancels every invocation and publishes done.invoke to the parent, if any. */
ta_result ta_invoke_finished(ta_interpreter *it);
bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out);
/* The invoke ID an event from `sender` to `destination` carries: set for child-to-parent events. */
ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination);
/* False after a parent has retired the invocation that identifies this child. */
bool ta_invoke_parent_accepts(const ta_system *system,
                              const ta_actor *sender,
                              const ta_actor_ref *destination);
void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child);
/* Cancels the actor's services and frees its invoke bookkeeping. */
void ta_invoke_release(ta_system *system, ta_actor *actor);
