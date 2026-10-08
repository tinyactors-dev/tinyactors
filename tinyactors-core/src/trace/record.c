/* record.c — the trace record schema: reading records (tinyactors/trace.h)
 * and writing them into a journal (record.h). */
#include "trace/record.h"
#include "base/memory.h"

TA_STATIC_ASSERT(sizeof(ta_trace_record) == 88, trace_record_header_is_88_bytes);

static const uint32_t trace_body_sizes[TA_TRACE_KIND_COUNT] = {
    [TA_TRACE_DROPPED] = sizeof(ta_trace_dropped),
    [TA_TRACE_SPAWNED] = sizeof(ta_trace_spawned),
    [TA_TRACE_FINISHED] = sizeof(ta_trace_finished),
    [TA_TRACE_DESTROYED] = 0,
    [TA_TRACE_UNLOADED] = 0,
    [TA_TRACE_LOADED] = 0,
    [TA_TRACE_MACROSTEP_BEGIN] = sizeof(ta_trace_macrostep_begin),
    [TA_TRACE_MACROSTEP_END] = sizeof(ta_trace_macrostep_end),
    [TA_TRACE_MICROSTEP] = sizeof(ta_trace_microstep),
    [TA_TRACE_CONDITION] = sizeof(ta_trace_condition),
    [TA_TRACE_RAISED] = sizeof(ta_trace_raised),
    [TA_TRACE_SENT] = sizeof(ta_trace_sent),
    [TA_TRACE_INVOKED] = sizeof(ta_trace_invoked),
    [TA_TRACE_CANCELLED] = sizeof(ta_trace_cancelled),
    [TA_TRACE_ASSIGN] = sizeof(ta_trace_assign),
    [TA_TRACE_LOG] = sizeof(ta_trace_log),
    [TA_TRACE_ACTION_FAILED] = sizeof(ta_trace_action_failed),
};

uint32_t ta_trace_body_size(uint32_t kind) {
    return kind && kind < TA_TRACE_KIND_COUNT ? trace_body_sizes[kind] : 0;
}

const void *ta_trace_body(const ta_trace_record *record) {
    return record + 1;
}

const uint32_t *ta_trace_list(const ta_trace_record *record) {
    return (const uint32_t *)ta_pointer_add(record, (int64_t)sizeof *record +
                                                        ta_trace_body_size(record->kind));
}

/* The u32 entries the lists after a record's body hold. */
static uint64_t trace_list_count(const ta_trace_record *record) {
    if (record->kind == TA_TRACE_MACROSTEP_END)
        return ((const ta_trace_macrostep_end *)ta_trace_body(record))->state_count;
    if (record->kind == TA_TRACE_MICROSTEP) {
        const ta_trace_microstep *micro = (const ta_trace_microstep *)ta_trace_body(record);
        return (uint64_t)micro->transition_count + micro->exited_count + micro->entered_count;
    }
    return 0;
}

const ta_trace_record *ta_trace_next(const ta_trace_view *view, uint32_t *cursor) {
    uint32_t at = *cursor;
    if (at >= view->length || view->length - at < sizeof(ta_trace_record) || (at & 7u))
        return (const ta_trace_record *)0;
    const ta_trace_record *record = (const ta_trace_record *)ta_pointer_add(view->records, at);
    uint32_t kind = record->kind, size = record->size;
    if (!kind || kind >= TA_TRACE_KIND_COUNT || (size & 7u) || size > view->length - at)
        return (const ta_trace_record *)0;
    uint64_t need = sizeof *record + (uint64_t)trace_body_sizes[kind];
    if (size < need) return (const ta_trace_record *)0;
    if (size < need + 4u * trace_list_count(record)) return (const ta_trace_record *)0;
    *cursor = at + size;
    return record;
}

ta_string_view ta_trace_dynamic_string(const ta_trace_view *view, uint32_t reference) {
    ta_string_view out = {(const char *)0, 0};
    if (reference == TA_NIL || !(reference & TA_TRACE_STRING_DYNAMIC) || !view->strings)
        return out;
    const uint32_t *header = (const uint32_t *)ta_pointer_add(
        view->strings, (int64_t)(reference & ~TA_TRACE_STRING_DYNAMIC) << 3);
    out.length = header[1];
    out.pointer = (const char *)(header + 2);
    return out;
}

ta_trace_record *ta_trace_record_reserve(ta_journal *journal, uint32_t kind, uint32_t size,
                                         uint64_t fuel, uint64_t now) {
    if (journal->lost) {
        const uint32_t dropped_size = sizeof(ta_trace_record) + sizeof(ta_trace_dropped);
        ta_trace_record *dropped = (ta_trace_record *)ta_journal_reserve(journal, dropped_size);
        if (!dropped) return dropped; /* the journal counted it: the next report has it */
        ta_memset(dropped, 0, dropped_size);
        dropped->kind = TA_TRACE_DROPPED;
        dropped->size = dropped_size;
        dropped->fuel = fuel;
        dropped->now = now;
        ((ta_trace_dropped *)ta_trace_record_body(dropped))->count = ta_journal_take_lost(journal);
        ta_journal_commit(journal, dropped_size);
    }
    size = ta_journal_size(size);
    ta_trace_record *record = (ta_trace_record *)ta_journal_reserve(journal, size);
    if (!record) return record;
    ta_memset(record, 0, sizeof *record);
    record->kind = (uint16_t)kind;
    record->size = size;
    return record;
}
