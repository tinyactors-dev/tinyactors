#pragma once

#include "types.h"

/*
 * Tracing (ADR 0067, ADR 0098): the record the call sites build and the rules
 * they share. The interpreter builds the records; this module decides which
 * ones are built at all, from the masks in the system (ta_trace_masks), and
 * tel.h writes them to the telemetry ring.
 */

/*
 * One trace record, as the interpreter and the lifecycle sites build it and
 * ta_tel_write_trace (tel.h) writes it to the tel ring (ADR 0098). Private
 * to core: it borrows events, values and fault data that no host may hold,
 * and a host reads a ta_tel_trace (include/tinyactors.h) instead. Only the
 * fields its kind names are meaningful; the rest are zero, or NONE for IDs.
 * Everything is borrowed for the call: retain a value to keep it (ADR 0006). `context` is the running macrostep's; `cause`, for
 * MACROSTEP_BEGIN and SPAWNED, is the key of the step that caused it. A
 * fault carries its `detail`, `code` and `message`. Its data went into the
 * error.execution event of the RAISED record that follows; ACTION_FAILED
 * also borrows it, so a record can say why an action failed.
 * Writing a record never releases its fault data. A MICROSTEP that carries
 * `states` was an operator transition's (ADR 0082), which a chart's never
 * does: `states` are the targets the operator named, history states
 * included, `state` the source (TA_STATE_NONE: the root), and there are no
 * `transitions`. `exited`, `entered` and `history` are as for any
 * microstep.
 */
typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    ta_trace_position position;
    ta_trace_context context;
    ta_step_key cause;
    ta_actor_id actor;
    ta_definition_id definition;
    /* The definition's identity, for the DEFINITION_* kinds; borrowed. */
    ta_definition_identity identity;
    ta_time now;

    TA_IN TA_OPTIONAL const ta_event *event;
    ta_state_id state;
    ta_expr_id expr;
    ta_str name;
    ta_action_id action;
    uint32_t iteration;
    ta_send_id send;
    ta_invoke_id invoke;
    bool result;
    bool has_value;
    ta_value value;
    bool has_fault;
    ta_fault fault;
    uint32_t dropped;

    TA_COUNTED_BY(transition_count) const ta_transition_id *transitions;
    uint32_t transition_count;
    TA_COUNTED_BY(exited_count) const ta_state_id *exited;
    uint32_t exited_count;
    TA_COUNTED_BY(entered_count) const ta_state_id *entered;
    uint32_t entered_count;
    TA_COUNTED_BY(history_count) const ta_state_id *history;
    uint32_t history_count;
    TA_COUNTED_BY(state_count) const ta_state_id *states;
    uint32_t state_count;
} ta_trace_record;


static inline bool ta_trace_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

static inline bool ta_trace_parent_empty(const ta_trace_parent *parent) {
    return ta_trace_context_empty(&parent->context) && parent->context.flags == 0 && parent->cause.session == 0 &&
           parent->cause.macrostep == 0 && parent->cause.microstep == 0;
}

/*
 * The kinds a macrostep under `context` builds (ADR 0073 section 4):
 * `unsampled_kinds` when it has a trace ID that is not sampled, otherwise
 * `watched_kinds` when it is marked watched, and `kinds` when not.
 */
uint32_t ta_trace_macrostep_kinds(const ta_trace_masks *masks, const ta_trace_context *context);

/* Whether the masks want lifecycle record `kind` under `context`: from
 * `watched_kinds` when the context is watched, else from `kinds`. Lifecycle
 * records are not sampled (ADR 0073 section 4). */
static inline bool ta_trace_lifecycle_wanted(const ta_trace_masks *masks, ta_trace_kind kind,
                                             const ta_trace_context *context) {
    uint32_t kinds = (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? masks->watched_kinds : masks->kinds;
    return (kinds & TA_TRACE_BIT(kind)) != 0;
}

/* Every kind any macrostep may build: what scratch must be reserved for. */
static inline uint32_t ta_trace_any_kinds(const ta_trace_masks *masks) {
    return masks->kinds | masks->watched_kinds | masks->unsampled_kinds;
}

/* A record with every ID set to NONE and everything else zero. */
ta_trace_record ta_trace_record_blank(ta_trace_kind kind);

struct ta_actor;

/*
 * Writes the lifecycle record `kind` for `actor` (SPAWNED, LOADED,
 * UNLOADED or DESTROYED), with `parent` as its context and cause when
 * there is one. Lifecycle records are outside macrosteps and never sampled.
 */
void ta_trace_lifecycle(ta_system *system, struct ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent);

/*
 * The key of the actor's next lifecycle record: (session, 0, n), where n
 * counts the session's lifecycle records from 1. It moves whether or not a
 * mask wants the record, so keys are the same with tracing on or off.
 */
ta_step_key ta_trace_lifecycle_key(struct ta_actor *actor);
