/**
 * @file trace.h
 *
 * # Tracing: core's decisions as self-contained records on `tel`
 *
 * Core reports what it decided and the data it changed (ADR 0067) as
 * `tel.trace` records on the system's `tel` ring, a lossy `ta_ring` (ADR
 * 0098). This unit decides which records are built, builds each one with
 * every name and value as text beside the IDs it names, stamps it with the
 * host's wall clock, and writes it to the ring; the interpreter, actors,
 * session images and the system say what happened, as a `ta_trace_entry`.
 * It also reads records back for the host (`ta_system_tel_peek`, which
 * decodes a record's IDs and flags and leaves its texts in the ring for
 * `ta_trace_record_text`), and holds trace contexts: `traceparent` text,
 * the trace-ID generator and sampling (ADR 0073).
 *
 * ## The control block and the ring
 *
 * The system's control block holds the `tel` ring: its positions (`head`,
 * `tail`, the next seq) and its dropped count, which is what a host reads
 * of it (`ta_system_tel_status`). `ta_trace_open` makes the ring at system
 * creation, `telBytes` bytes with records of at most `maxRecordBytes`
 * (both rounded down to a multiple of 8), beside a scratch area of
 * `maxRecordBytes` where a record's composed texts are laid out before it
 * is reserved. With `telBytes` 0 there is neither, and the masks stay zero:
 * `ta_system_set_trace_masks` refuses anything else.
 *
 * ## Contract and invariants
 *
 * - **Masks decide what is built.** Three masks of `TA_TRACE_BIT(kind)`
 *   (ADR 0073): a macrostep takes `kinds`, `watched_kinds` or
 *   `unsampled_kinds` from its context at MACROSTEP_BEGIN
 *   (`ta_trace_macrostep_kinds`); a lifecycle record is looked up in
 *   `watched_kinds` or `kinds` (`ta_trace_lifecycle_wanted`). A kind that
 *   is off costs the caller one test, before it builds anything, so the hot
 *   path has no branch for a missing ring.
 * - **A record is self-contained.** Beside each ID it carries the name:
 *   the session's ID as text (`<incarnation>.<session>.<family>`, as
 *   `_sessionid` holds it, or the decimal session when its family is
 *   unknown), its named address, the state, transition and action at the
 *   position and the ones the kind names, the event's name, sendid and
 *   invokeid, the send and invoke IDs, the fault's error event name, and
 *   every list entry. A state's name is its author ID; a transition's is
 *   its label, as "idle → busy on go [guarded]", "busy on poke
 *   (internal)", "initial of p → a" or "initial of the document → a"; an
 *   action's is its element and its block, as "<assign> in onentry of
 *   busy" or "<raise> in then of <if> in onexit of idle". The kinds that
 *   open a run (SPAWNED, LOADED, MACROSTEP_BEGIN, the DEFINITION_* kinds)
 *   also carry the definition's family, revision, document name and the
 *   source its first state was written in. A record carries no atom: an
 *   atom means nothing outside the process, so a session's family is in
 *   its ID text.
 * - **Values are text.** A value, the event's data and the fault's data
 *   are rendered by the actor's data model (`value_text` with
 *   `trace_bytes`) and cut at the masks' `value_bytes` on a UTF-8 boundary,
 *   setting `value_cut` or `event_data_cut`. `ta_system_set_trace_masks`
 *   keeps at most `telemetryValueBytes` of it; with 0 no data model is
 *   asked and no value is rendered. The reason a failure gives (the
 *   fault's data, an error event's data) is cut at `telemetryValueBytes`
 *   whatever the masks say: a log of the failure needs it. Each text is
 *   copied into the scratch area as soon as it is rendered, so a data
 *   model may render every value of a record into one buffer. A record never carries a value
 *   reference: a dropped record could not hand it back.
 * - **A record fits `maxRecordBytes`.** Its texts are cut in this order
 *   until they fit beside the 384-byte fixed part: the value, the event's
 *   data, the fault's data and message, the source, the document name, then
 *   the rest from the last. The lists take what is left, whole from the
 *   first, the last list cut first; the counts say how many entries are
 *   there. Any cut sets `truncated`.
 * - **The ring is lossy.** A full ring drops the record, counts it and
 *   consumes its seq. Before the next record, core reserves a `tel.dropped`
 *   report and only then takes the drops into it, so a refused report is
 *   counted with the rest; the ring's seqs have a gap exactly where a
 *   report says there is one.
 * - **A record is measured once and written once.** Its texts are composed
 *   into the scratch area, cut to fit and summed by `ta_trace_fit`; the
 *   record is reserved at that size, its fixed part filled in the
 *   reservation, and the codec copies the texts behind it, the ones
 *   composed end to end in one run.
 * - **Texts consecutive records repeat are kept, not composed again:** the
 *   session's ID text of the last session named, the last transition label
 *   and the last action label composed (a microstep's records name its
 *   transition in the list, the position and an action's label), and the
 *   document name and source of the last definition a record that opens a
 *   run named. Labels and facts are keyed by the definition's ID and
 *   forgotten when the definition is freed (`ta_trace_forget_definition`),
 *   so no record names another definition's.
 * - **The clock is called once per record written,** after its room is
 *   reserved, so a dropped record or a kind that is off costs no call.
 *   Without a clock, `wall_ns` is 0.
 * - **Every record is committed when it is written,** so a host reading
 *   between calls sees all of them.
 *
 * ## Memory
 *
 * The ring's buffer and the scratch area come from the system's allocator
 * in `ta_trace_open`, at creation, and go in `ta_trace_close`. The kept
 * session text and labels are copies in `ta_tracer`, part of the system;
 * the kept facts borrow the definition's texts until it is freed. Nothing
 * else here allocates: building and writing a record (`ta_trace_write`,
 * `ta_trace_lifecycle`) only fills the scratch area and the ring, so it
 * may run inside a macrostep. A record borrows what it names (atom texts,
 * the definition, the event) only for the call; the data model's text of a
 * value is copied into the scratch area before the data model is called
 * again. A record read with
 * `ta_system_tel_peek`, and every text `ta_trace_record_text` answers,
 * point into the ring until it is released.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | the mask tests | O(1) |
 * | `ta_trace_write` | O(the record's size) plus one `value_text` per value, and per action label the depth of its blocks |
 * | `ta_system_tel_peek`, `ta_system_tel_release` | O(1) per record, plus the pads skipped |
 * | `ta_trace_record_text` | O(1) |
 * | `ta_trace_forget_definition` | O(1) |
 * | `ta_trace_open`, `ta_trace_close` | two allocations, two frees |
 *
 * ## Failure
 *
 * Writing a record cannot fail and cannot change the run: a record without
 * room is dropped and counted, a value its data model cannot render is left
 * empty, and a text that does not fit is cut. `ta_trace_open` answers
 * `TA_ERR_NOMEM` when the allocator refuses, freeing what it made.
 *
 * ## Users
 *
 * The interpreter (macrosteps, microsteps, guards, actions), actors
 * (SPAWNED, DESTROYED, WITHDRAWN), session images (LOADED, UNLOADED), the
 * system (the DEFINITION_* kinds; teardown sets the masks to zero first,
 * so a system being destroyed writes nothing), and the host,
 * which reads `tel` between calls: the kernel's OTLP encoder, the test
 * host's journal. ADRs 0067, 0072, 0073, 0080, 0082, 0098, 0104.
 */
#pragma once

#include "../include/tinyactors.h"

typedef struct ta_actor ta_actor;

/** A trace record's fixed part, in bytes: the size of `ta_tel_trace`. */
#define TA_TRACE_FIXED_BYTES 384u
/** The longest session ID text a record carries; a longer one is the session's number. */
#define TA_TRACE_SESSION_TEXT 255u
/** What `telemetryValueBytes` leaves of `maxRecordBytes`: the fixed part and room for names. */
#define TA_TRACE_RESERVED_BYTES 512u

/** The longest transition or action label the tracer keeps for the records that follow. */
#define TA_TRACE_LABEL_TEXT 255u

/** What a kept label names: a definition (0: nothing) and a transition or action of it. */
typedef struct {
    ta_definition_id definition;
    uint32_t id;
} ta_trace_label_key;

/** The last label composed of one sort, and what it names. */
typedef struct {
    ta_trace_label_key key;
    uint32_t len;
    char text[TA_TRACE_LABEL_TEXT];
} ta_trace_label;

/** The texts of the last definition a record that opens a run named, looked up once. */
typedef struct {
    /** The definition they are of; 0 for none. */
    ta_definition_id definition;
    /** Its document name and the source its first state was written in; borrowed from the definition. */
    ta_str document_name;
    ta_str source;
} ta_trace_facts;

/**
 * The system's tracing state, beside the control block that holds the
 * ring: the masks, the clock, the scratch area and the value limit, and
 * the texts consecutive records repeat.
 */
typedef struct {
    ta_trace_masks masks;
    ta_wall_clock clock;
    /** `maxRecordBytes` bytes where a record's composed texts are laid out; NULL without a ring. */
    unsigned char *scratch;
    uint32_t scratch_bytes;
    /** `telemetryValueBytes`: the most of a value's text the masks may ask for. */
    uint32_t value_bytes;
    /** The session whose ID text `session_text` holds, and its family: most
     * records in a row are one session's, so its text is written once. */
    uint64_t text_session;
    ta_atom text_family;
    uint32_t session_text_len;
    char session_text[TA_TRACE_SESSION_TEXT];
    /** The last transition label and the last action label composed: a
     * microstep's records name its transition three times (the list, the
     * position, inside an action's label) and its actions in turn. */
    ta_trace_label transition_label;
    ta_trace_label action_label;
    /** The definition facts the last record that opened a run carried. */
    ta_trace_facts facts;
} ta_tracer;

/** Whether a context has a trace ID. */
static inline bool ta_trace_context_empty(const ta_trace_context *context) {
    return context->trace_id_high == 0 && context->trace_id_low == 0;
}

/** Whether a parent carries nothing at all. */
static inline bool ta_trace_parent_empty(const ta_trace_parent *parent) {
    return ta_trace_context_empty(&parent->context) && parent->context.flags == 0 && parent->cause.session == 0 &&
           parent->cause.macrostep == 0 && parent->cause.microstep == 0;
}

/**
 * The kinds a macrostep under `context` builds (ADR 0073 section 4):
 * `unsampled_kinds` when it has a trace ID that is not sampled, otherwise
 * `watched_kinds` when it is marked watched, and `kinds` when not. Never
 * allocates.
 */
uint32_t ta_trace_macrostep_kinds(const ta_trace_masks *masks, const ta_trace_context *context);

/** Whether lifecycle record `kind` is wanted under `context`: from
 * `watched_kinds` when the context is watched, else from `kinds`.
 * Lifecycle records are not sampled (ADR 0073 section 4). */
static inline bool ta_trace_lifecycle_wanted(const ta_trace_masks *masks, ta_trace_kind kind,
                                             const ta_trace_context *context) {
    uint32_t kinds = (context->detail & TA_TRACE_DETAIL_WATCHED) != 0 ? masks->watched_kinds : masks->kinds;
    return (kinds & TA_TRACE_BIT(kind)) != 0;
}

/** Every kind any macrostep may build: what scratch must be reserved for. */
static inline uint32_t ta_trace_any_kinds(const ta_trace_masks *masks) {
    return masks->kinds | masks->watched_kinds | masks->unsampled_kinds;
}

/**
 * What a caller knows of one record, which `ta_trace_write` turns into a
 * `tel.trace`. It borrows everything it points at for the call. `actor`
 * renders values through its data model and names the session; it is NULL
 * for a definition record, which names `definition` alone. The other
 * fields are the record's (see `ta_trace_record` in the public header);
 * FOREACH's iteration is `position.iteration`.
 */
typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    ta_trace_position position;
    ta_trace_context context;
    ta_step_key cause;
    const ta_actor *actor;
    const ta_definition *definition;
    ta_time now;

    const ta_event *event;
    ta_state_id state;
    ta_expr_id expr;
    ta_str name;
    ta_action_id action;
    ta_send_id send;
    ta_invoke_id invoke;
    bool result;
    bool has_value;
    ta_value value;
    bool has_fault;
    ta_fault fault;
    uint32_t dropped;

    const ta_transition_id *transitions;
    uint32_t transition_count;
    const ta_state_id *exited;
    uint32_t exited_count;
    const ta_state_id *entered;
    uint32_t entered_count;
    const ta_state_id *history;
    uint32_t history_count;
    const ta_state_id *states;
    uint32_t state_count;
} ta_trace_entry;

/** An entry with every ID set to NONE and everything else zero. Never allocates. */
ta_trace_entry ta_trace_entry_blank(ta_trace_kind kind);

/**
 * Makes the control block's tel ring and the scratch area, as the
 * system's `telBytes`, `maxRecordBytes` and `telemetryValueBytes` say;
 * nothing when `telBytes` is 0. Runs at system creation only. Allocates;
 * answers `TA_ERR_NOMEM`, freeing what it made, when the allocator refuses.
 */
ta_result ta_trace_open(ta_system *system);

/** Frees the ring and the scratch area. Never fails; a closed tracer writes nothing. */
void ta_trace_close(ta_system *system);

/**
 * Forgets the labels and facts kept for `definition`, which is being freed: its ID
 * names nothing from now on, and the slot's next definition must not find
 * them. Never allocates.
 */
void ta_trace_forget_definition(ta_tracer *tracer, ta_definition_id definition);

/**
 * Writes the record `entry` describes to `tel`, after a `tel.dropped` for
 * the records dropped before it, when there were any. The caller tested
 * the masks first. Never allocates; may run inside a macrostep. Cannot fail:
 * a record without room is dropped and counted.
 */
void ta_trace_write(ta_system *system, const ta_trace_entry *entry);

/**
 * Writes the lifecycle record `kind` for `actor` (SPAWNED, LOADED,
 * UNLOADED or DESTROYED), with `parent` as its context and cause when
 * there is one, and moves the counters and occupancy every residency
 * change moves (ADR 0072). Lifecycle records are outside macrosteps and
 * never sampled. Never allocates.
 */
void ta_trace_lifecycle(ta_system *system, ta_actor *actor, ta_trace_kind kind, const ta_trace_parent *parent);

/**
 * The key of the actor's next lifecycle record: (session, 0, n), where n
 * counts the session's lifecycle records from 1. It moves whether or not
 * the record is wanted, so keys are the same with tracing on or off.
 */
ta_step_key ta_trace_lifecycle_key(ta_actor *actor);
