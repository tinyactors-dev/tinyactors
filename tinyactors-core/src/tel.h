#pragma once

#include "types.h"
#include "trace.h"

/*
 * Package tel is the producer of the telemetry ring: it turns a trace record
 * into a tel.trace record on the ring, and reports what the ring could not
 * hold in a tel.dropped (ADR 0098; ring protocol proposal,
 * docs/proposals/ring-proto/reference/telemetry.md). The public side of tel is
 * in include/tinyactors.h: ta_trace_masks, ta_system_set_trace_masks, the
 * decoded types and ta_tel_decode, which a host calls on what
 * ta_ring_peek shows it.
 *
 * What it is for. Core used to hand every trace record to a host callback in
 * the middle of a macrostep. That made telemetry a host call per record, let
 * a slow sink slow the system, and let a sink hold references into core. A
 * ring has none of these: the producer writes bytes into memory core owns, the
 * consumer reads them later, and when there is no room the record is dropped
 * and counted. This unit is the writing half. The interpreter and the
 * lifecycle sites still build a ta_trace_record exactly as before (trace.h
 * decides which are built at all, from the masks); the only thing that
 * changed is where they send it.
 *
 * Contract.
 *   - ta_tel_write_trace never fails, never waits, never calls the host and
 *     never changes what the macrostep does. A record that cannot be
 *     written is dropped, and counted.
 *   - Order. Records are on the ring in the order they were written. A
 *     tel.dropped, when one is due, is written before the record that
 *     follows the gap.
 *   - Seq. Every record the producer decides to write takes the next seq,
 *     whether it fits or not (ring.h, lossy mode), so the consumer sees a
 *     jump in seq exactly where a record was lost, and ta_ring_record.gap
 *     says how many. A tel.dropped takes a seq of its own, as any record.
 *   - The drop protocol. A record that does not fit is not written: the ring
 *     counts it (shared.dropped, the ADR's tel_dropped) and this unit adds
 *     it to a pending count, remembering the seq the first lost record would
 *     have had. Before the next record, if the ring has room for a
 *     tel.dropped (checked first, ta_ring_would_fit, so that checking costs
 *     no seq), it is written with the pending count and that first seq, and
 *     the pending count restarts at zero. If it does not fit either, the
 *     next record is lost too and the pending count grows. The pending count
 *     is therefore the number of records lost since the last tel.dropped that
 *     reached the ring, and the sum of every tel.dropped count plus the
 *     pending count equals shared.dropped.
 *   - Text, not references. A record carries names and values as text,
 *     rendered when the record is written: event names, send and invoke
 *     IDs, data names, a fault's detail, message and data, a value. Nothing
 *     in a tel record names memory: a lossy ring that kept a reference
 *     would leak it on every drop. IDs that name parts of a definition
 *     (states, transitions, actions, expressions) are numbers. The origin of
 *     an event is not carried (nothing reads it).
 *   - Values are rendered by the data model's value_trace_text, only for a
 *     record whose kind the mask enables (the caller checks that), only when
 *     the masks ask for values, or for a fault's data, or for the data of an
 *     error.* event; they are cut at telemetryValueBytes and at the room the
 *     record has left, and the record says which was cut (truncated for the
 *     value, event_data_truncated, fault_data_truncated). A data model
 *     without the op, a failing op, or a zero limit leaves the value out.
 *
 * Lifetimes.
 *   - system: the ring, the control block and the staging area (system.c's
 *     one allocation; this unit neither allocates nor frees them) and the
 *     ta_tel_state, held by value in the ta_system. Nothing here outlives
 *     the system or points into it from outside.
 *   - call: everything else. The ta_trace_record and the ta_tel_values are
 *     the caller's, borrowed for the call and not kept. The tel.trace data
 *     struct is a stack value that borrows the record's texts and lists and
 *     the staged texts; the generated writer copies every span into the
 *     reservation before the call returns. The staging area holds the
 *     texts this unit renders (formatted names and IDs, a copy of each
 *     value text), valid from their rendering to the end of that one call;
 *     the next record overwrites it.
 *   - data-model text: owned by the data model, valid until its next call
 *     (value_trace_text), so it is copied into staging at once, before the
 *     next value is asked for.
 *   - record: a record on the ring is the consumer's from commit until the
 *     host releases it with ta_ring_release.
 *
 * When it allocates: never. Not at creation (system.c makes the one
 * allocation), not per record, not when the ring is full. The data model's
 * own memory for rendering is the data model's business, not core's, and
 * every other buffer here is the staging area, fixed at creation. A test
 * (tel_test.c) runs a macrostep that traces every kind under an allocator
 * that counts, and checks the count did not move.
 *
 * Invariants (between calls).
 *   T1. At most one reservation of the tel ring is open, and only inside
 *       ta_tel_write_trace; none between calls.
 *   T2. pending == 0 exactly when no record has been lost since the last
 *       tel.dropped that reached the ring; first_seq is meaningful only
 *       while pending > 0, and is then the seq of the oldest lost record.
 *   T3. staging is never read between calls.
 *
 * Complexity. O(the record's size): the fixed part is filled once, each text
 * is formatted or copied once into staging, and the generated writer copies
 * the fixed part and each span once into the reservation. One record costs
 * a single copy of its variable part into staging and one into the ring. A
 * kind that is off costs the caller one branch and never reaches here.
 *
 * Failure behaviour. Nothing here returns a result. A failure of any step
 * (a text that does not fit staging, a record over maxRecordBytes after its
 * values were cut, a full ring, a corrupt consumer tail) ends the same way:
 * the record is lost and counted, and the ring is as it would be had the
 * record never been tried (no partial record is ever visible: bytes are
 * written into a reservation, which is committed only when whole, and
 * abandoned otherwise).
 *
 * Usage (core's own call sites):
 *
 *	// the interpreter, for a record its mask enables:
 *	ta_tel_values values = { .datamodel = &definition->datamodel,
 *		.shared = definition->datamodel_shared,
 *		.instance = actor->datamodel_instance, .host = &actor->host };
 *	ta_tel_write_trace(system, &record, &values);
 *
 *	// a lifecycle record, outside any macrostep: no data model runs
 *	ta_tel_write_trace(system, &record, NULL);
 */

/*
 * What the writer needs to ask the data model for a value's text: the
 * binding of the actor that is running. A call-lifetime value built by the
 * interpreter for the record it writes; all of it is borrowed.
 */
typedef struct ta_tel_values {
    const ta_datamodel *datamodel; /* the running definition's data model; NULL: values cannot be rendered */
    void *shared;                  /* the definition's shared data-model state */
    void *instance;                /* the actor's data-model instance */
    const ta_dm_host *host;        /* the host view the data model calls back through */
} ta_tel_values;

/*
 * The producer's memory of what it lost (invariant T2): held by value in the
 * ta_system, system lifetime, zero at creation.
 */
typedef struct ta_tel_state {
    uint64_t pending;   /* records lost since the last tel.dropped that reached the ring */
    uint64_t first_seq; /* the seq the oldest of them would have had; meaningful while pending > 0 */
} ta_tel_state;

/*
 * ta_tel_write_trace writes the tel.trace record for `record` to the tel
 * ring of `system`, after the tel.dropped that is due, or drops it and
 * counts it. `record` is the trace record exactly as the caller built it
 * for its mask; `values` is the running actor's data-model binding, or NULL
 * when no data model runs (lifecycle records, definition records, errors
 * raised outside a run). Both are borrowed for the call. It may be called
 * at any point the interpreter or a lifecycle site writes a record; it
 * allocates nothing, calls no host code but the data model's
 * value_trace_text, and returns nothing. See the package comment for the
 * order, seq and drop rules.
 */
void ta_tel_write_trace(ta_system *system, const ta_trace_record *record, const ta_tel_values *values);
