/* tracing.c — the system's tracing policy (docs/adr/0006 in the repository
 * root, section 5): starting and sampling traces, lifecycle records, and
 * draining the journal. The kernel writes the step records itself. */
#include "base/memory.h"
#include "system/system_internal.h"
#include "trace/record.h"

uint32_t ta_system_start_trace(ta_system *system, ta_string_view traceparent,
                               ta_trace_context *value) {
    ta_memset(value, 0, sizeof *value);
    if (system->config.tracing == TA_TRACING_DISABLED) return TA_NIL;
    ta_step_key cause = {0, 0, 0, 0};
    if (traceparent.length && ta_traceparent_parse(traceparent, value) == TA_OK) {
        cause.macrostep = value->parent; /* a host's cause: its span ID */
    } else {
        value->trace = ta_trace_generator_next(&system->generator);
        value->parent = 0;
        value->flags = TA_TRACE_FLAG_RANDOM;
        if (ta_trace_sampled(value->trace, system->trace_threshold))
            value->flags |= TA_TRACE_FLAG_SAMPLED;
    }
    if (!(value->flags & TA_TRACE_FLAG_SAMPLED)) return TA_NIL; /* unsampled: no context */
    return ta_trace_context_new(&system->core, value->trace, value->flags, cause);
}

ta_trace_record *ta_system_trace_lifecycle(ta_system *system, uint32_t index, uint32_t kind,
                                           uint32_t size, const ta_trace_context *context,
                                           uint64_t macrostep, uint32_t step) {
    ta_core *core = &system->core;
    if (!ta_core_traces(core, kind) || !core->journal) return (ta_trace_record *)0;
    ta_trace_record *record =
        ta_trace_record_reserve(core->journal, kind, size, core->trace_clock, core->trace_now);
    if (!record) return record;
    const ta_actor *actor = ta_actor_at(system, index);
    record->flags = (uint16_t)context->flags;
    record->actor = ta_pool_handle(&system->actors, index);
    record->chart = ta_actor_chart(system, actor);
    record->key.session = actor->serial;
    record->key.macrostep = macrostep;
    record->key.microstep = step;
    record->trace = context->trace;
    if (!record->trace.high && !record->trace.low) { /* written whatever the sampling */
        record->trace = ta_trace_generator_next(&system->generator);
        record->flags = TA_TRACE_FLAG_RANDOM | TA_TRACE_FLAG_SAMPLED;
    }
    record->span = ta_step_key_span_id(record->key);
    record->parent = context->parent;
    record->fuel = core->trace_clock;
    record->now = core->trace_now;
    return record;
}

uint32_t ta_system_trace(ta_system *system, ta_trace_view *view) {
    ta_memset(view, 0, sizeof *view);
    if (system->config.tracing == TA_TRACING_DISABLED) return 0;
    view->length = ta_journal_peek(&system->journal, &view->records);
    view->strings = (const unsigned char *)ta_slab_pointer(&system->slab, 0);
    return view->length;
}

void ta_system_trace_ack(ta_system *system, uint32_t bytes) {
    if (system->config.tracing == TA_TRACING_DISABLED || system->in_call) return;
    ta_trace_view view;
    uint32_t length = ta_system_trace(system, &view), cursor = 0;
    if (bytes > length) bytes = length;
    /* core wrote these records: walked by their sizes, not checked again
     * as a reader of someone else's bytes would (ta_trace_next) */
    while (bytes - cursor >= sizeof(ta_trace_record)) {
        const ta_trace_record *record =
            (const ta_trace_record *)ta_pointer_add(view.records, cursor);
        if (record->size > bytes - cursor) break; /* whole records only */
        ta_core_trace_release(&system->core, record);
        cursor += record->size;
    }
    ta_journal_release(&system->journal, cursor);
}

void ta_system_trace_clock(ta_system *system, uint64_t at_least) {
    if (system->in_call) return;
    if (at_least > system->fuel_clock) system->fuel_clock = at_least;
    system->core.trace_clock = system->fuel_clock;
}

ta_status ta_system_trace_set(ta_system *system, uint32_t kinds, uint32_t sample_parts,
                              uint32_t sample_whole) {
    if (system->in_call) return TA_ERROR_STATE;
    if (system->config.tracing == TA_TRACING_DISABLED) return TA_ERROR_STATE;
    if (!sample_whole) sample_parts = sample_whole = 1;
    system->config.trace_kinds = kinds ? kinds : TA_TRACE_FULL;
    system->config.trace_sample_parts = sample_parts;
    system->config.trace_sample_whole = sample_whole;
    system->core.trace_kinds = system->config.trace_kinds;
    system->trace_threshold = ta_trace_ratio_threshold(sample_parts, sample_whole);
    return TA_OK;
}
