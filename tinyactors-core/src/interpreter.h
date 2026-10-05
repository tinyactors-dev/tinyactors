#pragma once

#include "system.h"
#include "datamodel.h"
#include "mail.h"

/* A generated send ID (`#_send.<n>`, ADR 0048) is this bit and the session's counter n. */
#define TA_GENERATED_SEND_ID_BIT ((ta_send_id)1u << 63)

/*
 * Per-call interpreter scratch. Between steps the actor holds all state:
 * configuration, history, internal queue, and pending event. A run that
 * stops inside a phase, paused (ADR 0061) or out of memory (ADR 0070),
 * moves the rest, the position, the sets and the executor's frames, into
 * the actor's continuation.
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
    /* The operator transition being taken, while its microstep begins
     * (ADR 0082); null otherwise. */
    const struct ta_operator_jump *jump;
    ta_transition_id *filtered;
    bool *remove;
    /*
     * Scratch for the state-tree queries (ADR 0060 section 4), sized from
     * the definition when the interpreter starts, so no query allocates.
     */
    struct ta_entry_frame *entry_frames;
    uint32_t entry_capacity;
    ta_state_id *history_stack;
    uint64_t *history_seen;
    uint64_t *history_busy;
    uint64_t *final_states;
    /*
     * The executor's frames (ADR 0060 section 2): one per open block and one
     * per running <foreach>, sized from the definition's deepest nesting of
     * blocks when the interpreter starts, so running actions never allocates
     * frames.
     */
    ta_exec_frame *exec_frames;
    uint32_t exec_count;
    uint32_t exec_capacity;
    /*
     * Where the run stands inside a phase (ADR 0061, ADR 0070), the units of
     * executable content this call may run and has run, whether the block
     * running now is one a pause may stop, and whether a microstep this
     * call begins pauses at its start (ADR 0086).
     */
    ta_micro_position at;
    uint32_t action_budget;
    uint32_t actions;
    bool pausable;
    bool in_microstep;
    bool pause_at_start;
    /* The continuation this call resumed from, which the actor no longer
     * holds: the run stops into it again, or gives it to the system. */
    ta_continuation *resumed;
    /* The invocations of the macrostep's states just started: eventless
     * transitions are selected again only if that queued an internal event. */
    bool after_invokes;
    /* The data declarations the instance's binding already supplies, which
     * the bootstrap does not initialize (`decl_words` words). */
    uint64_t *bound_decls;
    uint32_t decl_words;
    /*
     * The taken event's name as atoms (name_resolver.h), `event_atom_count`
     * of the `event_atom_capacity` that fit. Lifetime: macrostep/unit scratch,
     * one buffer per interpreter. Allocated once, in ta_interpreter_init, from
     * system->scratch with eventNameDepth entries; never during a macrostep.
     * Written once per take, before selection; read only by
     * ta_select_transitions and the matcher, which borrow it for the call.
     * Nothing keeps a pointer into it: the arena is reset by the next init.
     */
    ta_atom *event_atoms;
    uint32_t event_atom_count;
    uint32_t event_atom_capacity;
    /*
     * Tracing (ADR 0067). `trace_kinds` is what the running macrostep builds,
     * from the sink's mask and the macrostep's context. `trace_action` is the
     * unit of executable content running now. `trace_states` is scratch for
     * a record's state lists (3 * state_count), and `conditions` holds the
     * guards of the selection in progress, written only once it completes;
     * both are null when the sink wants no record that needs them.
     */
    uint32_t trace_kinds;
    ta_trace_context trace_context;
    ta_action_id trace_action;
    ta_state_id *trace_states;
    struct ta_trace_condition *conditions;
    uint32_t condition_count;
    uint32_t condition_capacity;
    /* Microsteps the running macrostep may run (ADR 0075); 0 for no limit. */
    uint32_t microstep_limit;
    /* The macrostep this call ended took an external event that enabled no
     * transition (ADR 0089 section 8). */
    bool ended_ignored;
} ta_interpreter;

ta_time ta_interpreter_macrostep_time(const ta_system *system, const ta_actor *actor);

/* How one pass of ta_interpreter_run_input ended. */
typedef enum ta_step_status {
    TA_STEP_STABLE,  /* the macrostep ended and the session waits for its next input */
    TA_STEP_PAUSED,  /* the budget, the action budget or a pause stopped it; the next pass continues */
    TA_STEP_DONE,    /* the session reached a top-level final state */
    TA_STEP_RUNAWAY  /* the macrostep ran its microstep limit; the caller abandons it (ADR 0099 section 7) */
} ta_step_status;

/* What one pass may spend (call lifetime). */
typedef struct ta_input_run {
    uint32_t budget;        /* microsteps this pass may run; at least 1 */
    uint32_t action_budget; /* units of executable content before a pause inside a microstep; 0 for none */
    bool pause_at_start;    /* also pause at the start of every microstep (ADR 0086) */
} ta_input_run;

/* What one pass did (call lifetime). Valid when the pass answered TA_OK. */
typedef struct ta_input_result {
    ta_step_status status;
    uint32_t steps;  /* microsteps it ran, for the caller's budget over several inputs */
    bool ignored;    /* STABLE or DONE: the external event enabled no transition (ADR 0089 section 8) */
} ta_input_result;

/*
 * ta_interpreter_run_input runs the actor's macrostep from where it stands
 * (ADR 0047, ADR 0061, ADR 0070): it takes the macrostep's logical time on
 * its first pass, and stops when the macrostep ends, the budget is spent, a
 * pause is asked for, or the microstep limit is reached. An idle actor answers
 * STABLE with no step. TA_ERR_NOMEM stops it where it stands (the next pass
 * continues) or, with nothing open, leaves nothing to continue; `*out` is then
 * not valid. It allocates only as a macrostep always has.
 */
ta_result ta_interpreter_run_input(ta_system *system, ta_actor *actor, const ta_input_run *run, ta_input_result *out);
/*
 * Whether an operator transition is well formed for the definition (ADR
 * 0082): at least one target, known states, and targets that can form a
 * legal configuration. Whether its source is active is the caller's check.
 */
bool ta_interpreter_jump_valid(const ta_definition *definition, const ta_operator_transition *transition);
/* ta_actor_abandon() for a resident actor (ADR 0082). */
ta_result ta_interpreter_abandon(ta_system *system, ta_actor *actor);
/*
 * Runs SCXML exitInterpreter for a cancelled invoked session, without
 * done.invoke, and for every session it invoked in turn, without recursion
 * (ADR 0060). A session that is gone or finished is skipped.
 */
void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id);
/* Takes ownership of the event's data. */
ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/*
 * ta_dispatch is the request and the answer of ta_interpreter_dispatch_delayed.
 * Lifetime: call.
 */
typedef struct ta_dispatch {
    /*
     * In: the sender's own run dispatches it (an immediate <send>): a failure
     * is then raised in that run, and the unit that sent it counts and records
     * it. Otherwise (a timer, a held send released) the failure is raised
     * outside a run, and counted and recorded here (ADR 0072).
     */
    bool in_run;
    bool mailed;    /* out: core placed the send as mail, or was told by mail that it cannot be placed */
    bool delivered; /* out: with `mailed`, an envelope exists for it (it is queued or waits for an address) */
    bool sent;      /* out: the send went out, or was dropped; false: the sender was told it failed */
} ta_dispatch;

/*
 * ta_interpreter_dispatch_delayed dispatches one due or immediate send, whose
 * values the call consumes: a send to the built-in SCXML processor becomes
 * mail (package mail), one to a host processor is handed to it. A send whose
 * sender is gone, or whose invoking parent cancelled it, is dropped. A send
 * that fails is reported to its sender as error.communication; one for which
 * mail found no envelope, inside the sender's run, as error.execution. Outside
 * a run, mail to a resident or a waiting destination is made at once from the
 * pool's free quota: the caller has called ta_mail_prepare for it (a safe
 * point).
 */
void ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send, ta_dispatch *dispatch);
/*
 * A literal content as a host adapter receives it (ADR 0053): its media type
 * and its bytes. The bytes' pointer is non-null even for empty content, so an
 * adapter can tell empty content from none.
 */
void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes);
void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);
/*
 * The interpreter scratch a definition needs (ADR 0092 section 3), in
 * `*out`. A pure computation with call lifetime that never allocates; it
 * answers TA_ERR_NOMEM, and leaves `*out` alone, when the figure does not
 * fit in a size_t.
 */
ta_result ta_interpreter_scratch_need(const ta_definition *definition, size_t *out);
/* The fault of a temporary that does not fit the render area (ADR 0092). */
void ta_interpreter_render_fault(ta_fault *fault);
/* ta_interpreter_workspace_free frees the system's spare event block (its
 * `workspace`, ADR 0099 section 8), which ta_system_destroy does. It never
 * allocates. */
void ta_interpreter_workspace_free(ta_system *system);
/* The largest continuation a run of the definition can save: its shape with
 * the offsets of its arrays, and its size; TA_ERR_NOMEM, outputs untouched,
 * when that does not fit. Never allocates. */
ta_result ta_continuation_capacity(const ta_definition *definition, ta_continuation *shape, size_t *bytes);

/* Frees the continuation and sparse state the system keeps for a run that
 * stops (ADR 0070), at teardown. */
void ta_interpreter_release_spares(ta_system *system);
/* Lends the actor an event block if it holds none: a cached one, else a
 * header only. TA_ERR_NOMEM changes nothing (ADR 0069). */
ta_result ta_interpreter_hold_block(ta_system *system, ta_actor *actor);
/* The bytes an event block with room for this many internal events takes. */
size_t ta_event_block_bytes(uint32_t capacity);

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
/*
 * Replaces *value, which is about to leave the session, with the data
 * model's copy_out of it (ADR 0097). An absent value stays as it is, and so
 * does every value of a data model without copy_out, which shares. On
 * failure *value is unchanged and still the caller's.
 */
ta_op_status ta_interpreter_copy_out(ta_interpreter *it, ta_value *value, ta_fault *fault);
/* Queues error.execution; returns TA_OK unless queueing itself failed. */
ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault);
/* Makes sure the actor's queue has room for one more internal event, so
 * queueing it cannot fail (ADR 0070). */
ta_result ta_interpreter_queue_room(ta_system *system, ta_actor *actor);
/* Room for `count` more internal events, so that queueing them cannot fail. */
ta_result ta_interpreter_queue_reserve(ta_system *system, ta_actor *actor, uint32_t count);
/* How many events the actor's queue holds, a taken head included: a mark
 * a step's RAISED records start from. */
uint32_t ta_interpreter_queue_mark(const ta_actor *actor);
/* Writes a RAISED record for every event queued after `mark`, once the step
 * that queued them has committed (ADR 0067). */
void ta_interpreter_trace_raised(ta_interpreter *it, uint32_t mark);
/* Writes an INVOKED or INVOKE_CANCELLED record for `invoke`. */
void ta_interpreter_trace_invoke(ta_interpreter *it, ta_trace_kind kind, ta_invoke_id invoke);
/* Records that `event` was autoforwarded to `invoke` (ADR 0067 open point 3). */
void ta_interpreter_trace_forward(ta_interpreter *it, ta_invoke_id invoke, const ta_event *event);
/* What a step running now passes to what it produces: the macrostep's
 * context, and the step's own key as the cause (ADR 0067 section 6). */
ta_trace_parent ta_interpreter_trace_here(const ta_interpreter *it);
/* The parent the actor's current macrostep runs under; all zero for none. */
ta_trace_parent ta_actor_macrostep_parent(const ta_actor *actor);
/* The key of the step the actor stands in: its macrostep and microstep
 * while a macrostep has begun, the session alone otherwise (ADR 0067). */
ta_step_key ta_actor_step(const ta_actor *actor);
/* Where the actor's stopped macrostep stands, as its last PAUSED or STOPPED
 * record said (ADR 0083); nowhere when no phase is open. */
ta_trace_position ta_actor_position(const ta_actor *actor);
/* Adds the actor's active states to its definition's occupancy, or takes
 * them away (ADR 0072), when it becomes resident or stops being so. */
void ta_interpreter_occupancy(ta_actor *actor, bool arrive);
/* Counts one error event by its kind (ADR 0072); other events count nothing. */
void ta_interpreter_count_error(ta_system *system, const ta_actor *actor, ta_event_name_id name);
/* Counts and records an event core queued outside a run (ADR 0072), with
 * `parent` as its cause when there is one. */
void ta_interpreter_raised_outside(ta_system *system, ta_actor *actor, ta_event_name_id name,
                                   const ta_trace_parent *parent);
/*
 * Makes an idle actor run a macrostep for work queued outside any run, such
 * as a failed send's error, under `trace` (null: none). An actor already in
 * a macrostep is left alone. The session joins the started queue of ta_run
 * (run.h), so the caller must have reserved room with ta_run_reserve_start
 * before the step that made the work, where failing leaves nothing behind.
 */
void ta_interpreter_start_macrostep(ta_system *system, ta_actor *actor, const ta_trace_parent *trace);
bool ta_interpreter_internal_pending(const ta_actor *actor);

/* ------------------------------------------------------------------------
 * Invoke hooks, implemented in invoke.c
 */

/* Interns an SCXML invoke ID's text; the runtime ID is its atom + 1. */
ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out);
ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke);
ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state);
ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state);
/* An invocation taken out of an actor's table, to be cancelled. */
typedef struct {
    ta_active_invoke active;
    ta_invoke_meta meta;
} ta_invoke_taken;
/* Forgets the not-yet-started invocations of `state`. */
void ta_invoke_forget_pending(ta_actor *actor, ta_state_id state);
/* Takes the last invocation of `state` below index `*cursor` out of the
 * actor's table; false when there is none. */
bool ta_invoke_take(ta_actor *actor, ta_state_id state, uint32_t *cursor, ta_invoke_taken *out);
/* Tells the invoker to cancel the service of a taken invocation. */
void ta_invoke_cancel_service(const ta_invoke_taken *taken);
/* Whether states the macrostep entered have invocations still to start. */
bool ta_invoke_has_pending(const ta_actor *actor);
/*
 * Starts the invocations of the states entered during the finished
 * macrostep, from state `*state` and <invoke> declaration `*decl` on. On
 * TA_ERR_NOMEM they name the invocation that did not start, and nothing of
 * it happened; the next call starts from there (ADR 0070).
 */
ta_result ta_invoke_start_pending(ta_interpreter *it, uint32_t *state, uint32_t *decl);
/* Whether <finalize> and autoforwarding apply to an external event just
 * taken: not for an event of an invocation cancelled since it was queued. */
bool ta_invoke_before_external(const ta_actor *actor, const ta_event *event);
/* The <finalize> actions of the invocation `event` came from; false when it
 * came from none that is still active. */
bool ta_invoke_finalize_actions(const ta_actor *actor, const ta_event *event, ta_span32 *out);
/* Autoforwards `event` to every invocation that asks for it, from index
 * `*cursor` on; on TA_ERR_NOMEM `*cursor` names the one to forward next. */
ta_result ta_invoke_autoforward(ta_interpreter *it, const ta_event *event, uint32_t *cursor);
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
