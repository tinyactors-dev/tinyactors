#include "trace.h"
#include "system.h"

/* ------------------------------------------------------------------------
 * The null sink (ADR 0067 section 2)
 */

static void ta_null_trace_write(void *self, const ta_trace_record *record) {
    (void)self;
    (void)record;
}

static const ta_trace_sink_vtable ta_null_trace_vtable = { .write = ta_null_trace_write };

ta_trace_sink ta_null_trace_sink(void) {
    return (ta_trace_sink){ .self = NULL, .v = &ta_null_trace_vtable, .kinds = 0, .unsampled = false };
}

ta_result ta_system_set_trace_sink(ta_system *system, ta_trace_sink sink) {
    if (system == NULL || sink.v == NULL || sink.v->write == NULL) {
        return TA_ERR_INVALID;
    }
    system->trace = sink;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Step keys as span IDs (section 6)
 */

/* The finalizer of MurmurHash3's 64-bit variant: every input bit reaches
 * every output bit. Fixed, so every host derives the same span IDs. */
static uint64_t ta_trace_mix(uint64_t value) {
    value ^= value >> 33;
    value *= 0xff51afd7ed558ccdull;
    value ^= value >> 33;
    value *= 0xc4ceb9fe1a85ec53ull;
    value ^= value >> 33;
    return value;
}

uint64_t ta_step_key_span_id(ta_step_key key) {
    if (key.session == 0) {
        return key.macrostep;
    }
    uint64_t span = ta_trace_mix(key.session);
    span = ta_trace_mix(span ^ key.macrostep);
    span = ta_trace_mix(span ^ ((uint64_t)key.microstep | ((uint64_t)1u << 32)));
    return span == 0 ? 1u : span;
}

/* ------------------------------------------------------------------------
 * What is built
 */

uint32_t ta_trace_macrostep_kinds(const ta_trace_sink *sink, const ta_trace_context *context) {
    if (ta_trace_context_empty(context) || (context->flags & TA_TRACE_FLAG_SAMPLED) != 0 || sink->unsampled) {
        return sink->kinds;
    }
    return sink->kinds & TA_TRACE_LIFECYCLE;
}

ta_step_key ta_trace_lifecycle_key(ta_actor *actor) {
    actor->trace_lifecycle += 1u;
    return (ta_step_key){ .session = actor->session_id, .microstep = actor->trace_lifecycle };
}

void ta_trace_lifecycle(ta_system *system, ta_actor *actor, ta_trace_kind kind,
                        const ta_trace_parent *parent) {
    ta_step_key key = ta_trace_lifecycle_key(actor);
    if ((system->trace.kinds & TA_TRACE_BIT(kind)) == 0) {
        return;
    }
    ta_trace_record record = ta_trace_record_blank(kind);
    record.key = key;
    record.actor = actor->record.id;
    record.definition = actor->definition->id;
    record.now = ta_clock_now(&system->clock);
    if (parent != NULL) {
        record.context = parent->context;
        record.cause = parent->cause;
    }
    ta_trace_write(&system->trace, &record);
}

ta_trace_record ta_trace_record_blank(ta_trace_kind kind) {
    return (ta_trace_record){
        .kind = kind,
        .position = { .at = TA_TRACE_AT_NONE, .state = TA_STATE_NONE, .transition = TA_TRANSITION_NONE,
                      .action = TA_ACTION_NONE },
        .state = TA_STATE_NONE,
        .expr = TA_EXPR_ID_NONE,
        .action = TA_ACTION_NONE,
        .invoke = TA_INVOKE_ID_NONE,
    };
}
