#pragma once

#include "types.h"

/*
 * Tracing (ADR 0067): the sink a system writes records to, and the rules
 * every call site shares. The interpreter builds the records; this module
 * decides which ones are built at all.
 */

/* The kinds written outside macrosteps, which sampling never removes. */
#define TA_TRACE_LIFECYCLE                                                                                    \
    (TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_LOADED) | TA_TRACE_BIT(TA_TRACE_UNLOADED) |       \
     TA_TRACE_BIT(TA_TRACE_FINISHED) | TA_TRACE_BIT(TA_TRACE_DESTROYED))

static inline bool ta_trace_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

static inline bool ta_trace_parent_empty(const ta_trace_parent *parent) {
    return ta_trace_context_empty(&parent->context) && parent->context.flags == 0 && parent->cause.session == 0 &&
           parent->cause.macrostep == 0 && parent->cause.microstep == 0;
}

/*
 * The kinds a macrostep under `context` builds: all the sink wants when the
 * context has no trace ID, is sampled, or the sink traces unsampled ones;
 * otherwise only lifecycle kinds (section 6).
 */
uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context);

/* A record with every ID set to NONE and everything else zero. */
ta_trace_record ta_trace_record_blank(ta_trace_kind kind);

struct ta_actor;

/*
 * Writes the lifecycle record `kind` for `actor` (SPAWNED, LOADED,
 * UNLOADED or DESTROYED), with `parent` as its context and cause when
 * there is one. Lifecycle records are outside macrosteps and never sampled.
 */
void ta_trace_lifecycle(ta_system *system, const struct ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent);

/* Writes a record the caller built because `sink.kinds` asked for it. */
static inline void ta_trace_write(const ta_trace_sink *sink, const ta_trace_record *record) {
    sink->v->write(sink->self, record);
}
