#pragma once

#include "../include/tinyactors.h"

/*
 * Tracing (ADR 0067): the sink a system writes records to, and the rules
 * every call site shares. The interpreter builds the records; this module
 * decides which ones are built at all.
 */

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
uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context);

/* Whether the sink wants lifecycle record `kind` under `context`: from
 * `watched_kinds` when the context is watched, else from `kinds`. Lifecycle
 * records are not sampled (ADR 0073 section 4). */
static inline bool ta_trace_lifecycle_wanted(const ta_trace_sink *sink, ta_trace_kind kind,
                                             const ta_trace_context *context) {
    uint32_t kinds = (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? sink->watched_kinds : sink->kinds;
    return (kinds & TA_TRACE_BIT(kind)) != 0;
}

/* Every kind any macrostep may build: what scratch must be reserved for. */
static inline uint32_t ta_trace_any_kinds(const ta_trace_sink *sink) {
    return sink->kinds | sink->watched_kinds | sink->unsampled_kinds;
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
 * sink wants the record, so keys are the same with tracing on or off.
 */
ta_step_key ta_trace_lifecycle_key(struct ta_actor *actor);

/* Writes a record the caller built because `sink.kinds` asked for it. */
static inline void ta_trace_write(const ta_trace_sink *sink, const ta_trace_record *record) {
    sink->v->write(sink->self, record);
}
