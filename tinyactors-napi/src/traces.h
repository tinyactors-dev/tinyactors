#pragma once
/*
 * Trace records waiting for JavaScript (core ADR 0067, napi ADR 0015).
 *
 * Core writes a record inside a core call, and everything in it is borrowed
 * for that call. The trace sink adapter (adapters/trace.h) copies it here:
 * the record, its state and transition lists into a pool of IDs, its name
 * into a pool of text, and a reference to its event and its value. The pump
 * hands the whole queue to the JavaScript hook at the end of the turn, as
 * one array, after the last core call (ADR 0007 point 5). A record written
 * outside a turn (a spawn or a destroy JavaScript called) waits for the
 * next turn, which the queue asks for.
 *
 * Memory (ADR 0014): the records and the pools are vectors that keep their
 * capacity from turn to turn, so a steady stream of records makes no
 * allocator call once the busiest turn has been seen. (The turn arena
 * would not do: it finds room block by block, and a busy turn's records
 * fill hundreds of blocks.)
 *
 * Only what core's mask asked for arrives here, so an untraced system never
 * touches this module: it runs with core's null sink.
 *
 * This module has no Node-API in it.
 */
#include <stdbool.h>
#include <stdint.h>

#include "containers.h"
#include "tinyactors.h"

typedef struct {
    /* The record. Its lists and name point into the pools, and its event at
     * `event`, only once tna_traces_view() has run; until then they are
     * null and the offsets below say where they are. */
    ta_trace_record record;
    /* A retained copy of the record's event, when it has one. */
    ta_event event;
    /* Where its lists start in the ID pool, in the order transitions,
     * exited, entered, history, states; and its name in the text pool. */
    uint32_t ids_at;
    uint32_t name_at;
    /* Wall-clock time the record was written, in milliseconds since the
     * epoch: core's `now` is logical time. */
    double wall_time;
} tna_trace_entry;

typedef struct {
    tna_vec entries; /* tna_trace_entry */
    tna_vec ids;     /* uint32_t: the records' state and transition IDs */
    tna_vec text;    /* char: the records' names */
    bool closed;      /* the system is closing: new records are dropped */
    uint64_t dropped; /* records that could not be queued (no memory, or closing) */
} tna_traces;

void tna_traces_init(tna_traces *traces, ta_allocator allocator);
static inline uint32_t tna_traces_count(const tna_traces *traces) { return traces->entries.count; }
/* Queues a copy of `record`. False when it was dropped (no memory, or closing). */
bool tna_traces_push(tna_traces *traces, const ta_trace_record *record);
/*
 * The queued records, oldest first, with their pointers filled in; they
 * stay valid until tna_traces_clear(). `*count` is how many there are.
 */
tna_trace_entry *tna_traces_view(tna_traces *traces, uint32_t *count);
/* Releases what the queued records retain and empties the queue, keeping
 * its capacity. */
void tna_traces_clear(tna_traces *traces);
/* Closes the queue, releases every queued record, and frees the storage. */
void tna_traces_close(tna_traces *traces);
