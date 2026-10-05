#pragma once
/*
 * Package tel_reader is how a core test reads the telemetry ring (ADR 0098).
 * Core used to hand each trace record to a sink the test installed, in the
 * middle of the macrostep; now core writes them to the tel ring, and a test
 * sets the masks (ta_system_set_trace_masks) and reads after the calls that
 * ran an actor. This header is the second half: one function that drains tel
 * through the public API (ta_ring_peek, ta_tel_decode, ta_ring_release) and
 * hands each decoded trace record to a callback.
 *
 * It uses only include/tinyactors.h, so a test that links the library and one
 * that includes the amalgamation can both include it. It is test
 * infrastructure, never part of the library.
 *
 * Lifetime: a ta_tel_entry given to the callback is a view into the ring and
 * dies when the callback returns (the record is released right after); a test
 * that keeps anything copies it. A tel_reader is a plain value owned by the
 * test, holds no memory and needs no destroy.
 *
 * Usage:
 *
 *	static void visit(void *context, const ta_tel_entry *entry) { ... entry->trace ... }
 *	tel_reader reader = { .visit = visit };
 *	ta_system_set_trace_masks(system, &(ta_trace_masks){ .kinds = UINT32_MAX });
 *	ta_run(system, &params);
 *	tel_read(system, &reader);          // after every call into core that may trace
 *	CHECK(reader.bad == 0);
 */
#include "../include/tinyactors.h"

typedef void (*tel_visit)(void *context, const ta_tel_entry *entry);

typedef struct tel_reader {
    tel_visit visit;  /* called with each tel.trace entry, in order; may be NULL */
    void *context;    /* passed to visit */
    uint64_t records; /* tel.trace records read */
    uint64_t dropped; /* the sum of the counts of the tel.dropped records read */
    uint64_t gaps;    /* the sum of the seq gaps read: records core lost before the ones read */
    uint32_t bad;     /* records that did not decode: always a bug */
} tel_reader;

/*
 * tel_read drains the tel ring of `system` into `reader`: it peeks each record,
 * decodes it, calls reader->visit for a tel.trace (and only counts a
 * tel.dropped), and releases it. It returns how many records it read.
 */
static inline uint32_t tel_read(ta_system *system, tel_reader *reader) {
    uint32_t count = 0;
    ta_ring_record raw;
    while (ta_ring_peek(system, TA_RING_TEL, &raw) == TA_OK) {
        ta_tel_entry entry;
        if (ta_tel_decode(&raw, &entry) != TA_OK) {
            reader->bad += 1u;
        } else {
            reader->gaps += entry.gap;
            if (entry.type == TA_TEL_DROPPED) {
                reader->dropped += entry.dropped.count;
            } else {
                reader->records += 1u;
                if (reader->visit != NULL) {
                    reader->visit(reader->context, &entry);
                }
            }
        }
        ta_ring_release(system, TA_RING_TEL, raw.size);
        count += 1u;
    }
    return count;
}
