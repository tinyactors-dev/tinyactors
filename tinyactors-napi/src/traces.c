#include "traces.h"

#include <string.h>
#include <time.h>

void tna_traces_init(tna_traces *traces, ta_allocator allocator) {
    *traces = (tna_traces){ 0 };
    tna_vec_init(&traces->entries, allocator, TNA_LAYOUT(tna_trace_entry));
    tna_vec_init(&traces->ids, allocator, TNA_LAYOUT(uint32_t));
    tna_vec_init(&traces->text, allocator, TNA_LAYOUT(char));
}

static double tna_wall_time(void) {
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 0;
    }
    return (double)now.tv_sec * 1000.0 + (double)now.tv_nsec / 1e6;
}

/* Appends `count` IDs to the pool. */
static bool tna_traces_put_ids(tna_traces *traces, const uint32_t *ids, uint32_t count) {
    if (count == 0) {
        return true;
    }
    if (tna_vec_reserve(&traces->ids, traces->ids.count + count) != TA_OK) {
        return false;
    }
    memcpy((uint32_t *)traces->ids.items + traces->ids.count, ids, (size_t)count * sizeof(uint32_t));
    traces->ids.count += count;
    return true;
}

bool tna_traces_push(tna_traces *traces, const ta_trace_record *record) {
    uint32_t ids_before = traces->ids.count;
    uint32_t text_before = traces->text.count;
    uint32_t lists = record->transition_count + record->exited_count + record->entered_count +
                     record->history_count + record->state_count;
    bool room = !traces->closed && tna_vec_reserve(&traces->entries, traces->entries.count + 1u) == TA_OK &&
                tna_vec_reserve(&traces->ids, traces->ids.count + lists) == TA_OK &&
                tna_vec_reserve(&traces->text, traces->text.count + record->name.len) == TA_OK;
    if (!room) {
        traces->dropped += 1u;
        return false;
    }
    /* The reservations above make every append below succeed. */
    (void)tna_traces_put_ids(traces, record->transitions, record->transition_count);
    (void)tna_traces_put_ids(traces, record->exited, record->exited_count);
    (void)tna_traces_put_ids(traces, record->entered, record->entered_count);
    (void)tna_traces_put_ids(traces, record->history, record->history_count);
    (void)tna_traces_put_ids(traces, record->states, record->state_count);
    if (record->name.len != 0) {
        memcpy((char *)traces->text.items + traces->text.count, record->name.ptr, record->name.len);
        traces->text.count += record->name.len;
    }
    tna_trace_entry *entry = (tna_trace_entry *)traces->entries.items + traces->entries.count;
    *entry = (tna_trace_entry){ .record = *record, .ids_at = ids_before, .name_at = text_before,
                                .wall_time = tna_wall_time() };
    ta_trace_record *copy = &entry->record;
    copy->transitions = NULL;
    copy->exited = NULL;
    copy->entered = NULL;
    copy->history = NULL;
    copy->states = NULL;
    copy->name.ptr = NULL;
    /* A fault's data went into the error event; only its detail and code
     * cross (core ADR 0067). */
    copy->fault = (ta_fault){ .detail = record->fault.detail, .code = record->fault.code };
    if (record->event != NULL) {
        ta_event_retain(record->event, &entry->event);
        copy->event = &entry->event; /* fixed up again by tna_traces_view(): the vector may move */
    }
    if (record->has_value) {
        ta_value_retain(&copy->value);
    } else {
        copy->value = (ta_value){0};
    }
    traces->entries.count += 1u;
    return true;
}

tna_trace_entry *tna_traces_view(tna_traces *traces, uint32_t *count) {
    tna_trace_entry *entries = traces->entries.items;
    const uint32_t *ids = traces->ids.items;
    const char *text = traces->text.items;
    for (uint32_t index = 0; index < traces->entries.count; index += 1u) {
        tna_trace_entry *entry = &entries[index];
        ta_trace_record *record = &entry->record;
        const uint32_t *at = ids == NULL ? NULL : ids + entry->ids_at;
        record->transitions = record->transition_count != 0 ? at : NULL;
        at += record->transition_count;
        record->exited = record->exited_count != 0 ? at : NULL;
        at += record->exited_count;
        record->entered = record->entered_count != 0 ? at : NULL;
        at += record->entered_count;
        record->history = record->history_count != 0 ? at : NULL;
        at += record->history_count;
        record->states = record->state_count != 0 ? at : NULL;
        record->name.ptr = record->name.len != 0 ? text + entry->name_at : NULL;
        if (record->event != NULL) {
            record->event = &entry->event;
        }
    }
    *count = traces->entries.count;
    return entries;
}

void tna_traces_clear(tna_traces *traces) {
    tna_trace_entry *entries = traces->entries.items;
    for (uint32_t index = 0; index < traces->entries.count; index += 1u) {
        ta_trace_record *record = &entries[index].record;
        if (record->event != NULL) {
            ta_event_release(&entries[index].event);
        }
        ta_value_release(&record->value);
    }
    traces->entries.count = 0;
    traces->ids.count = 0;
    traces->text.count = 0;
}

void tna_traces_close(tna_traces *traces) {
    traces->closed = true;
    tna_traces_clear(traces);
    tna_vec_destroy(&traces->entries);
    tna_vec_destroy(&traces->ids);
    tna_vec_destroy(&traces->text);
}
