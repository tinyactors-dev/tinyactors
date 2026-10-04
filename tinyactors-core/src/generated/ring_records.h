/**
 * @file ring_records.h
 *
 * # The ring protocol's records
 *
 * Generated from `protocol/catalog.rb` by `mise run protocol` in
 * tinyactors-core. Do not edit: change the catalog and render again.
 * `mise run protocol --check`, which `mise run test` runs, fails when
 * this file is not what the catalog renders.
 *
 * Every record that crosses the h2c, c2h and tel rings is a struct here,
 * laid out exactly as it lies in a ring: `ta_<ring>_<name>` for the
 * record `<ring>.<name>` (`ta_h2c_event` for `h2c.event`). Beside the
 * structs are the record kinds (`ta_record_kind`, the 16-bit code in the
 * header), the rings (`ta_record_ring`), the common flags
 * (`ta_record_flag`, bits 8..15 of the header's `flags`), each record's
 * own flags (`ta_<ring>_<name>_flag`, bits 0..7) and the catalog's enums
 * (`ta_record_<enum>`). `src/generated/MESSAGES.md` says what every
 * record and field means.
 *
 * ## Layout
 *
 * - Every record starts with `ta_record_header`, 32 bytes: `kind`,
 *   `flags`, `size`, `seq`, `session`, `at`.
 * - The fixed fields follow in the order the catalog declares them,
 *   each at its natural alignment. Named `pad` arrays fill the gaps, so
 *   a struct has no padding of the compiler's, and `{0}` zeroes every
 *   byte of it.
 * - Text and bytes are `ta_span32` spans: an offset from the record's
 *   first byte and a length, into the tail that follows the fixed part.
 * - A record's size is a multiple of 8, and so is its fixed part,
 *   `sizeof` its struct.
 * - Fields are fixed-width integers, because the size of a C enum is up
 *   to the compiler; a comment names the enum a field holds.
 * - `_Static_assert`s pin every struct's size and every field's offset
 *   to the catalog's layout.
 *
 * Code 0x0000 is the pad `ta_ring` writes at the end of its buffer; it
 * is no record.
 *
 * ## Memory
 *
 * Types and constants only: nothing here allocates or runs.
 *
 * ## Users
 *
 * The producers and consumers of h2c, c2h and tel, through the record
 * codec (`ring_codec.h`), which writes and checks these records; the
 * rings themselves are `ta_ring`s, which frame records by their `kind`,
 * `size` and `seq`.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "../span.h"

/** The common header's size: every record starts with `ta_record_header`. */
#define TA_RECORD_HEADER_BYTES 32u
/** How many rings there are; `ta_record_ring` numbers them from 1. */
#define TA_RECORD_RING_COUNT 3u

/** The ring a record crosses. */
typedef enum {
    /** host to core, lossless */
    TA_RECORD_RING_H2C = 1,
    /** core to host, lossless */
    TA_RECORD_RING_C2H = 2,
    /** core to host, lossy */
    TA_RECORD_RING_TEL = 3,
} ta_record_ring;

/** The header's `kind`: the message code. */
typedef enum {
    /** the pad at the end of a ring buffer; no record */
    TA_RECORD_KIND_PAD = 0x0000,
    /** `h2c.time`: Move the system clock */
    TA_RECORD_KIND_H2C_TIME = 0x0101,
    /** `h2c.event`: An external event for a session */
    TA_RECORD_KIND_H2C_EVENT = 0x0102,
    /** `h2c.spawn`: Create a session */
    TA_RECORD_KIND_H2C_SPAWN = 0x0103,
    /** `h2c.publish`: End a replay and put the session on the route */
    TA_RECORD_KIND_H2C_PUBLISH = 0x0104,
    /** `h2c.load`: Bring an unloaded session back */
    TA_RECORD_KIND_H2C_LOAD = 0x0105,
    /** `h2c.load_failed`: The session cannot be loaded */
    TA_RECORD_KIND_H2C_LOAD_FAILED = 0x0106,
    /** `h2c.unload`: Page a stable session out */
    TA_RECORD_KIND_H2C_UNLOAD = 0x0107,
    /** `h2c.checkpoint`: Ask for a recorded session's state */
    TA_RECORD_KIND_H2C_CHECKPOINT = 0x0108,
    /** `h2c.invoke_report`: An invoked service finished or failed */
    TA_RECORD_KIND_H2C_INVOKE_REPORT = 0x0109,
    /** `h2c.destroy`: End a session now */
    TA_RECORD_KIND_H2C_DESTROY = 0x010A,
    /** `h2c.definition_register`: Compile and register a complete definition */
    TA_RECORD_KIND_H2C_DEFINITION_REGISTER = 0x010B,
    /** `h2c.definition_release`: Release one registration hold */
    TA_RECORD_KIND_H2C_DEFINITION_RELEASE = 0x010C,
    /** `h2c.definition_retire`: End a definition’s availability */
    TA_RECORD_KIND_H2C_DEFINITION_RETIRE = 0x010D,
    /** `h2c.withdraw`: Take back an admitted input that has not been taken */
    TA_RECORD_KIND_H2C_WITHDRAW = 0x010E,
    /** `h2c.type_declare`: Declare an I/O processor or invoker type */
    TA_RECORD_KIND_H2C_TYPE_DECLARE = 0x010F,
    /** `c2h.spawned`: A session exists */
    TA_RECORD_KIND_C2H_SPAWNED = 0x0201,
    /** `c2h.input`: A recorded session took an input */
    TA_RECORD_KIND_C2H_INPUT = 0x0202,
    /** `c2h.send`: A send for an I/O processor */
    TA_RECORD_KIND_C2H_SEND = 0x0203,
    /** `c2h.invoke`: Start an invocation */
    TA_RECORD_KIND_C2H_INVOKE = 0x0204,
    /** `c2h.invoke_cancel`: Cancel an invocation */
    TA_RECORD_KIND_C2H_INVOKE_CANCEL = 0x0205,
    /** `c2h.invoke_forward`: Autoforward an event to an invocation */
    TA_RECORD_KIND_C2H_INVOKE_FORWARD = 0x0206,
    /** `c2h.log`: A <log> */
    TA_RECORD_KIND_C2H_LOG = 0x0207,
    /** `c2h.macrostep_ended`: A macrostep ended */
    TA_RECORD_KIND_C2H_MACROSTEP_ENDED = 0x0208,
    /** `c2h.wake`: Call core back at this time */
    TA_RECORD_KIND_C2H_WAKE = 0x0209,
    /** `c2h.bounced`: An h2c record was not run */
    TA_RECORD_KIND_C2H_BOUNCED = 0x020A,
    /** `c2h.load_needed`: Envelopes wait for an unloaded address */
    TA_RECORD_KIND_C2H_LOAD_NEEDED = 0x020B,
    /** `c2h.loaded`: A session is resident again */
    TA_RECORD_KIND_C2H_LOADED = 0x020C,
    /** `c2h.unloaded`: A session left memory */
    TA_RECORD_KIND_C2H_UNLOADED = 0x020D,
    /** `c2h.state`: A recorded session's state at a point of its log */
    TA_RECORD_KIND_C2H_STATE = 0x020E,
    /** `c2h.published`: A replayed session is live */
    TA_RECORD_KIND_C2H_PUBLISHED = 0x020F,
    /** `c2h.finished`: A session reached a top-level final state */
    TA_RECORD_KIND_C2H_FINISHED = 0x0210,
    /** `c2h.destroyed`: A session is gone */
    TA_RECORD_KIND_C2H_DESTROYED = 0x0211,
    /** `c2h.definition_result`: A definition request completed */
    TA_RECORD_KIND_C2H_DEFINITION_RESULT = 0x0212,
    /** `c2h.input_refused`: An admitted host input was not taken */
    TA_RECORD_KIND_C2H_INPUT_REFUSED = 0x0213,
    /** `c2h.input_completed`: A host input and its startup follow-up settled */
    TA_RECORD_KIND_C2H_INPUT_COMPLETED = 0x0214,
    /**
     * `c2h.load_result`: A load-failure response finished recycling waiting
     * inputs
     */
    TA_RECORD_KIND_C2H_LOAD_RESULT = 0x0215,
    /** `tel.trace`: One trace record (core ADR 0067) */
    TA_RECORD_KIND_TEL_TRACE = 0x0301,
    /** `tel.dropped`: Telemetry records were dropped */
    TA_RECORD_KIND_TEL_DROPPED = 0x0302,
} ta_record_kind;

/** Common flags, bits 8..15 of the header's `flags`. */
typedef enum {
    /**
     * part of a replay. On c2h and tel: written by a replaying session. On h2c:
     * an input of a replay; under a whole-system replay every h2c record
     * carries it, sessions it spawns replay, mail between them is delivered as
     * envelopes, and effects that would leave the process are suppressed until
     * h2c.publish
     */
    TA_RECORD_FLAG_REPLAYING = 1 << 8,
    /** the session is recorded: its inputs appear as c2h.input */
    TA_RECORD_FLAG_RECORDED = 1 << 9,
} ta_record_flag;

/** `h2c.spawn`'s own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    /** the session replays; `clock` is where its run began */
    TA_H2C_SPAWN_FLAG_REPLAYING_FROM_CLOCK = 1 << 0,
} ta_h2c_spawn_flag;

/** `c2h.wake`'s own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    /** nothing is due: cancel the timer */
    TA_C2H_WAKE_FLAG_NONE = 1 << 0,
} ta_c2h_wake_flag;

/** `c2h.unloaded`'s own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    /** wake_at is meaningful */
    TA_C2H_UNLOADED_FLAG_HAS_WAKE = 1 << 0,
} ta_c2h_unloaded_flag;

/** `c2h.input_completed`'s own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    /**
     * the input event enabled no transition, so the chart ignored it (core ADR
     * 0089 section 8). A request/response I/O processor answers its client at
     * once instead of waiting for a timeout
     */
    TA_C2H_INPUT_COMPLETED_FLAG_IGNORED = 1 << 0,
} ta_c2h_input_completed_flag;

/** `tel.trace`'s own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    /**
     * value_text was cut, or a list in the tail was cut to fit maxRecordBytes
     */
    TA_TEL_TRACE_FLAG_TRUNCATED = 1 << 0,
    /** an effect a replaying session did not perform */
    TA_TEL_TRACE_FLAG_SUPPRESSED = 1 << 1,
} ta_tel_trace_flag;

/** What _event.type says (SCXML 5.10.1). Stored as uint8_t. */
typedef enum {
    TA_RECORD_EVENT_TYPE_PLATFORM = 0,
    TA_RECORD_EVENT_TYPE_INTERNAL = 1,
    TA_RECORD_EVENT_TYPE_EXTERNAL = 2,
} ta_record_event_type;

/** Where the envelope behind a c2h.input came from. Stored as uint8_t. */
typedef enum {
    /** a host input; source_seq is its h2c seq */
    TA_RECORD_INPUT_SOURCE_H2C = 0,
    /** a <send> from another local session; from_session names it */
    TA_RECORD_INPUT_SOURCE_MAIL = 1,
    /** a delayed send that became due; from_session sent it */
    TA_RECORD_INPUT_SOURCE_TIMER = 2,
    /**
     * core made it: error.communication for undeliverable mail, done.invoke,
     * ...
     */
    TA_RECORD_INPUT_SOURCE_PLATFORM = 3,
    /** an h2c.invoke_report */
    TA_RECORD_INPUT_SOURCE_INVOKE_REPORT = 4,
} ta_record_input_source;

/** How a macrostep ended. Stored as uint8_t. */
typedef enum {
    /**
     * stabilization ended; invocation startup and internal follow-up may still
     * follow
     */
    TA_RECORD_MACROSTEP_STATUS_STABLE = 0,
    /** the session reached a top-level final state; c2h.finished follows */
    TA_RECORD_MACROSTEP_STATUS_DONE = 1,
    /**
     * the chain limit or an internal-queue overflow stopped it and core
     * abandoned the input; the configuration after the last completed microstep
     * stays
     */
    TA_RECORD_MACROSTEP_STATUS_RUNAWAY = 2,
    /** an operator abandoned it (core ADR 0082) */
    TA_RECORD_MACROSTEP_STATUS_ABANDONED = 3,
} ta_record_macrostep_status;

/**
 * How an input and its invocation-startup follow-up settled. Stored as uint8_t.
 */
typedef enum {
    /** ready for another external input */
    TA_RECORD_INPUT_COMPLETION_STATUS_STABLE = 0,
    /** the session finished */
    TA_RECORD_INPUT_COMPLETION_STATUS_DONE = 1,
    /**
     * the chain limit or an internal-queue overflow stopped it; core abandoned
     * the input and the session takes its next one
     */
    TA_RECORD_INPUT_COMPLETION_STATUS_RUNAWAY = 2,
    /** an operator abandoned execution */
    TA_RECORD_INPUT_COMPLETION_STATUS_ABANDONED = 3,
} ta_record_input_completion_status;

/**
 * What stopped a runaway input; tel.trace RUNAWAY carries it. Stored as
 * uint8_t.
 */
typedef enum {
    /** the input’s chain reached microstepLimit */
    TA_RECORD_RUNAWAY_CAUSE_CHAIN_LIMIT = 1,
    /**
     * a mandatory internal event fit neither the floor nor a free ordinary
     * entry
     */
    TA_RECORD_RUNAWAY_CAUSE_INTERNAL_QUEUE = 2,
} ta_record_runaway_cause;

/** Why an admitted host input was discarded before take. Stored as uint8_t. */
typedef enum {
    TA_RECORD_INPUT_REFUSAL_REASON_LOAD_FAILED = 1,
    TA_RECORD_INPUT_REFUSAL_REASON_DESTROYED = 2,
    /** copy_out could not produce the recording snapshot */
    TA_RECORD_INPUT_REFUSAL_REASON_SNAPSHOT_FAILED = 3,
    /** the service instance is no longer active */
    TA_RECORD_INPUT_REFUSAL_REASON_STALE_INVOCATION = 4,
    /** validation at take failed, for example a backwards replay clock */
    TA_RECORD_INPUT_REFUSAL_REASON_INVALID = 5,
    /** h2c.withdraw named it before it was taken */
    TA_RECORD_INPUT_REFUSAL_REASON_WITHDRAWN = 6,
} ta_record_input_refusal_reason;

/** Why core handed an h2c record back unrun. Stored as uint8_t. */
typedef enum {
    /** no session has the address and its family has no loader */
    TA_RECORD_BOUNCE_REASON_NO_SUCH_ADDRESS = 1,
    /** the revision is not registered */
    TA_RECORD_BOUNCE_REASON_NO_SUCH_DEFINITION = 2,
    /** h2c.spawn for an address that is resident */
    TA_RECORD_BOUNCE_REASON_ADDRESS_TAKEN = 3,
    /** h2c.unload for a session with envelopes waiting */
    TA_RECORD_BOUNCE_REASON_HAS_MAIL = 4,
    /** h2c.publish for a session that is not replaying */
    TA_RECORD_BOUNCE_REASON_NOT_REPLAYING = 5,
    /** the record breaks the protocol; detail says how */
    TA_RECORD_BOUNCE_REASON_INVALID = 6,
    /** admission watermark, envelope quota or load restoration capacity */
    TA_RECORD_BOUNCE_REASON_POOL = 7,
    /** admission watermark or insufficient name-area storage */
    TA_RECORD_BOUNCE_REASON_NAMES = 8,
    /** allocation outside macrosteps failed without committing the operation */
    TA_RECORD_BOUNCE_REASON_ALLOCATION = 9,
    /** unload of a local invocation owner or active invoked child */
    TA_RECORD_BOUNCE_REASON_ACTIVE_LOCAL_INVOKE = 10,
    /** publish attempted before supplied replay inputs settled */
    TA_RECORD_BOUNCE_REASON_PENDING_INPUTS = 11,
    /** a response does not match the outstanding load attempt */
    TA_RECORD_BOUNCE_REASON_STALE_REQUEST = 12,
    /** mandatory output can never fit the configured record or ring bounds */
    TA_RECORD_BOUNCE_REASON_RECORD = 13,
    /**
     * h2c.unload or h2c.checkpoint for a session that holds a macrostep slot
     */
    TA_RECORD_BOUNCE_REASON_ACTIVE = 14,
    /**
     * h2c.type_declare for a name core owns, such as scxml or one of its
     * aliases
     */
    TA_RECORD_BOUNCE_REASON_RESERVED_TYPE = 15,
} ta_record_bounce_reason;

/**
 * The last segment of error.execution.capacity.<resource>, the event an action
 * raises when a bounded resource has no room for it. Its data names the action.
 * A full c2h is not one of them: the slot waits for the host to drain. Stored
 * as uint8_t.
 */
typedef enum {
    /** no free envelope for mail or a delayed send (poolEnvelopes) */
    TA_RECORD_CAPACITY_RESOURCE_POOL = 1,
    /** no room in the name area for a run-time name (nameAreaBytes) */
    TA_RECORD_CAPACITY_RESOURCE_NAMES = 2,
    /** the internal queue is full; <raise> failed */
    TA_RECORD_CAPACITY_RESOURCE_INTERNAL = 4,
    /** the record would be larger than maxRecordBytes */
    TA_RECORD_CAPACITY_RESOURCE_RECORD = 5,
} ta_record_capacity_resource;

/** Stored as uint8_t. */
typedef enum {
    /** the invoked service finished: done.invoke.<id> */
    TA_RECORD_INVOKE_REPORT_KIND_DONE = 0,
    /** it failed: error.communication.invoke.<id>.<reason> (core ADR 0081) */
    TA_RECORD_INVOKE_REPORT_KIND_ERROR = 1,
} ta_record_invoke_report_kind;

/** Stored as uint8_t. */
typedef enum {
    TA_RECORD_DESTROY_REASON_FINISHED = 0,
    /** h2c.destroy */
    TA_RECORD_DESTROY_REASON_OPERATOR = 1,
    /** its invoking state exited */
    TA_RECORD_DESTROY_REASON_PARENT_EXITED = 2,
} ta_record_destroy_reason;

/** The result of a definition request. Stored as uint8_t. */
typedef enum {
    TA_RECORD_DEFINITION_STATUS_OK = 0,
    /** the complete spec was refused; diagnostic describes the first fault */
    TA_RECORD_DEFINITION_STATUS_COMPILE_ERROR = 1,
    /**
     * the diagnostic did not fit completely in its memory (nothing partial is
     * written), or allocation or definition capacity ran out
     */
    TA_RECORD_DEFINITION_STATUS_CAPACITY = 2,
    /** an explicit identity names different content (core ADR 0077) */
    TA_RECORD_DEFINITION_STATUS_MISMATCH = 3,
    /** an unknown or stale definition ID */
    TA_RECORD_DEFINITION_STATUS_NOT_FOUND = 4,
    /**
     * a malformed or unknown spec or adapter handle, or an invalid release
     * request
     */
    TA_RECORD_DEFINITION_STATUS_INVALID = 5,
} ta_record_definition_status;

/** Which type registry a declared type belongs to. Stored as uint8_t. */
typedef enum {
    /** I/O processor types: <send type>, _ioprocessors */
    TA_RECORD_TYPE_REGISTRY_IO = 1,
    /** invocation types: <invoke type> */
    TA_RECORD_TYPE_REGISTRY_INVOKER = 2,
} ta_record_type_registry;

/** The common header every record starts with. */
typedef struct {
    /** the message's code */
    uint16_t kind; /* ta_record_kind */
    /** the message's flags (bits 0..7) and the common flags (bits 8..15) */
    uint16_t flags;
    /** the whole record in bytes, header and tail included, a multiple of 8 */
    uint32_t size;
    /**
     * the sequence number in this ring, from 1, with no gaps on a lossless ring
     */
    uint64_t seq;
    /** the session the record is about, or 0 */
    uint64_t session;
    /** the system clock when it was written (h2c: the input's time) */
    uint64_t at;
} ta_record_header;
_Static_assert(sizeof(ta_record_header) == 32, "ta_record_header");
_Static_assert(offsetof(ta_record_header, kind) == 0, "header kind");
_Static_assert(offsetof(ta_record_header, flags) == 2, "header flags");
_Static_assert(offsetof(ta_record_header, size) == 4, "header size");
_Static_assert(offsetof(ta_record_header, seq) == 8, "header seq");
_Static_assert(offsetof(ta_record_header, session) == 16, "header session");
_Static_assert(offsetof(ta_record_header, at) == 24, "header at");

/** `h2c.time` (0x0101, on h2c): Move the system clock. */
typedef struct {
    ta_record_header header;
} ta_h2c_time;
_Static_assert(sizeof(ta_h2c_time) == 32, "h2c.time");

/** `h2c.event` (0x0102, on h2c): An external event for a session. */
typedef struct {
    ta_record_header header;
    /** the address (family/name), when session is 0 */
    ta_span32 target; /* text */
    /** the dotted event name, as bytes */
    ta_span32 name; /* text */
    /** _event.sendid, or empty */
    ta_span32 sendid; /* text */
    /** _event.origin, or empty */
    ta_span32 origin; /* text */
    /** _event.origintype, or empty */
    ta_span32 origin_type; /* text */
    /** _event.invokeid for an invoked service, or empty */
    ta_span32 invokeid; /* text */
    /**
     * c2h.invoke seq for a live service instance; 0 for ordinary input or
     * replay seeds
     */
    uint64_t invocation_seq;
    /** _event.data */
    uint64_t data;
    /**
     * the W3C trace ID, high 64 bits; with trace_id_low all zero means no trace
     * context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /** the W3C parent-id the macrostep runs under; 0 for none */
    uint64_t parent_span_id;
    /** almost always external */
    uint8_t type; /* ta_record_event_type */
    /** the W3C trace-flags (sampled, random) */
    uint8_t trace_flags;
    /** the local detail byte (watched); never W3C, never in a traceparent */
    uint8_t trace_detail;
    uint8_t pad_end[5];
} ta_h2c_event;
_Static_assert(sizeof(ta_h2c_event) == 128, "h2c.event");
_Static_assert(offsetof(ta_h2c_event, target) == 32, "h2c.event target");
_Static_assert(offsetof(ta_h2c_event, name) == 40, "h2c.event name");
_Static_assert(offsetof(ta_h2c_event, sendid) == 48, "h2c.event sendid");
_Static_assert(offsetof(ta_h2c_event, origin) == 56, "h2c.event origin");
_Static_assert(offsetof(ta_h2c_event, origin_type) == 64, "h2c.event origin_type");
_Static_assert(offsetof(ta_h2c_event, invokeid) == 72, "h2c.event invokeid");
_Static_assert(offsetof(ta_h2c_event, invocation_seq) == 80, "h2c.event invocation_seq");
_Static_assert(offsetof(ta_h2c_event, data) == 88, "h2c.event data");
_Static_assert(offsetof(ta_h2c_event, trace_id_high) == 96, "h2c.event trace_id_high");
_Static_assert(offsetof(ta_h2c_event, trace_id_low) == 104, "h2c.event trace_id_low");
_Static_assert(offsetof(ta_h2c_event, parent_span_id) == 112, "h2c.event parent_span_id");
_Static_assert(offsetof(ta_h2c_event, type) == 120, "h2c.event type");
_Static_assert(offsetof(ta_h2c_event, trace_flags) == 121, "h2c.event trace_flags");
_Static_assert(offsetof(ta_h2c_event, trace_detail) == 122, "h2c.event trace_detail");

/** `h2c.spawn` (0x0103, on h2c): Create a session. */
typedef struct {
    ta_record_header header;
    /** family/name */
    ta_span32 address; /* text */
    /** the definition's revision hash */
    ta_span32 revision; /* bytes */
    /** the data model binding, or 0 */
    uint64_t data;
    /** with replaying_from_clock: the replayed run’s start */
    uint64_t clock;
    /** a checkpoint c2h.state wrote, to restore; 0 for none */
    uint64_t state;
    /**
     * c2h.load_needed seq when answering a virtual-family load; 0 for an
     * unsolicited spawn
     */
    uint64_t request_seq;
    /**
     * the W3C trace ID, high 64 bits; with trace_id_low all zero means no trace
     * context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /** the W3C parent-id the macrostep runs under; 0 for none */
    uint64_t parent_span_id;
    /** the W3C trace-flags (sampled, random) */
    uint8_t trace_flags;
    /** the local detail byte (watched); never W3C, never in a traceparent */
    uint8_t trace_detail;
    uint8_t pad_end[6];
} ta_h2c_spawn;
_Static_assert(sizeof(ta_h2c_spawn) == 112, "h2c.spawn");
_Static_assert(offsetof(ta_h2c_spawn, address) == 32, "h2c.spawn address");
_Static_assert(offsetof(ta_h2c_spawn, revision) == 40, "h2c.spawn revision");
_Static_assert(offsetof(ta_h2c_spawn, data) == 48, "h2c.spawn data");
_Static_assert(offsetof(ta_h2c_spawn, clock) == 56, "h2c.spawn clock");
_Static_assert(offsetof(ta_h2c_spawn, state) == 64, "h2c.spawn state");
_Static_assert(offsetof(ta_h2c_spawn, request_seq) == 72, "h2c.spawn request_seq");
_Static_assert(offsetof(ta_h2c_spawn, trace_id_high) == 80, "h2c.spawn trace_id_high");
_Static_assert(offsetof(ta_h2c_spawn, trace_id_low) == 88, "h2c.spawn trace_id_low");
_Static_assert(offsetof(ta_h2c_spawn, parent_span_id) == 96, "h2c.spawn parent_span_id");
_Static_assert(offsetof(ta_h2c_spawn, trace_flags) == 104, "h2c.spawn trace_flags");
_Static_assert(offsetof(ta_h2c_spawn, trace_detail) == 105, "h2c.spawn trace_detail");

/**
 * `h2c.publish` (0x0104, on h2c): End a replay and put the session on the
 * route.
 */
typedef struct {
    ta_record_header header;
    /** family/name, or empty for every replaying session */
    ta_span32 address; /* text */
    /** the time chosen for the seeded session */
    uint64_t at;
} ta_h2c_publish;
_Static_assert(sizeof(ta_h2c_publish) == 48, "h2c.publish");
_Static_assert(offsetof(ta_h2c_publish, address) == 32, "h2c.publish address");
_Static_assert(offsetof(ta_h2c_publish, at) == 40, "h2c.publish at");

/** `h2c.load` (0x0105, on h2c): Bring an unloaded session back. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the bytes c2h.unloaded gave, in this process */
    uint64_t image;
    /** c2h.load_needed seq; 0 for an unsolicited timer-driven load */
    uint64_t request_seq;
} ta_h2c_load;
_Static_assert(sizeof(ta_h2c_load) == 56, "h2c.load");
_Static_assert(offsetof(ta_h2c_load, address) == 32, "h2c.load address");
_Static_assert(offsetof(ta_h2c_load, image) == 40, "h2c.load image");
_Static_assert(offsetof(ta_h2c_load, request_seq) == 48, "h2c.load request_seq");

/** `h2c.load_failed` (0x0106, on h2c): The session cannot be loaded. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    ta_span32 reason; /* text */
    /** the outstanding c2h.load_needed seq; a stale attempt changes nothing */
    uint64_t request_seq;
} ta_h2c_load_failed;
_Static_assert(sizeof(ta_h2c_load_failed) == 56, "h2c.load_failed");
_Static_assert(offsetof(ta_h2c_load_failed, address) == 32, "h2c.load_failed address");
_Static_assert(offsetof(ta_h2c_load_failed, reason) == 40, "h2c.load_failed reason");
_Static_assert(offsetof(ta_h2c_load_failed, request_seq) == 48, "h2c.load_failed request_seq");

/** `h2c.unload` (0x0107, on h2c): Page a stable session out. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** host-provided writable sink, pinned until unloaded or bounced */
    uint64_t image;
} ta_h2c_unload;
_Static_assert(sizeof(ta_h2c_unload) == 48, "h2c.unload");
_Static_assert(offsetof(ta_h2c_unload, address) == 32, "h2c.unload address");
_Static_assert(offsetof(ta_h2c_unload, image) == 40, "h2c.unload image");

/** `h2c.checkpoint` (0x0108, on h2c): Ask for a recorded session's state. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** host-provided writable sink, pinned until state or bounced */
    uint64_t image;
} ta_h2c_checkpoint;
_Static_assert(sizeof(ta_h2c_checkpoint) == 48, "h2c.checkpoint");
_Static_assert(offsetof(ta_h2c_checkpoint, address) == 32, "h2c.checkpoint address");
_Static_assert(offsetof(ta_h2c_checkpoint, image) == 40, "h2c.checkpoint image");

/**
 * `h2c.invoke_report` (0x0109, on h2c): An invoked service finished or failed.
 */
typedef struct {
    ta_record_header header;
    ta_span32 invokeid; /* text */
    /** the c2h.invoke seq; checked at take, 0 only for replay seeds */
    uint64_t invocation_seq;
    /** for error: the reason segment */
    ta_span32 reason; /* text */
    /** done data or error data */
    uint64_t data;
    /**
     * the W3C trace ID, high 64 bits; with trace_id_low all zero means no trace
     * context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /** the W3C parent-id the macrostep runs under; 0 for none */
    uint64_t parent_span_id;
    uint8_t kind; /* ta_record_invoke_report_kind */
    /**
     * for error: retire the matching instance at take after validation and
     * internal notification enqueue; false preserves it
     */
    uint8_t ends;
    /** the W3C trace-flags (sampled, random) */
    uint8_t trace_flags;
    /** the local detail byte (watched); never W3C, never in a traceparent */
    uint8_t trace_detail;
    uint8_t pad_end[4];
} ta_h2c_invoke_report;
_Static_assert(sizeof(ta_h2c_invoke_report) == 96, "h2c.invoke_report");
_Static_assert(offsetof(ta_h2c_invoke_report, invokeid) == 32, "h2c.invoke_report invokeid");
_Static_assert(offsetof(ta_h2c_invoke_report, invocation_seq) == 40, "h2c.invoke_report invocation_seq");
_Static_assert(offsetof(ta_h2c_invoke_report, reason) == 48, "h2c.invoke_report reason");
_Static_assert(offsetof(ta_h2c_invoke_report, data) == 56, "h2c.invoke_report data");
_Static_assert(offsetof(ta_h2c_invoke_report, trace_id_high) == 64, "h2c.invoke_report trace_id_high");
_Static_assert(offsetof(ta_h2c_invoke_report, trace_id_low) == 72, "h2c.invoke_report trace_id_low");
_Static_assert(offsetof(ta_h2c_invoke_report, parent_span_id) == 80, "h2c.invoke_report parent_span_id");
_Static_assert(offsetof(ta_h2c_invoke_report, kind) == 88, "h2c.invoke_report kind");
_Static_assert(offsetof(ta_h2c_invoke_report, ends) == 89, "h2c.invoke_report ends");
_Static_assert(offsetof(ta_h2c_invoke_report, trace_flags) == 90, "h2c.invoke_report trace_flags");
_Static_assert(offsetof(ta_h2c_invoke_report, trace_detail) == 91, "h2c.invoke_report trace_detail");

/** `h2c.destroy` (0x010A, on h2c): End a session now. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
} ta_h2c_destroy;
_Static_assert(sizeof(ta_h2c_destroy) == 40, "h2c.destroy");
_Static_assert(offsetof(ta_h2c_destroy, address) == 32, "h2c.destroy address");

/**
 * `h2c.definition_register` (0x010B, on h2c): Compile and register a complete
 * definition.
 */
typedef struct {
    ta_record_header header;
    /**
     * process-local handle of a complete native ta_scxml_spec built by the
     * binding
     */
    uint64_t spec;
    /** process-local handle of a native-installed data model adapter */
    uint64_t datamodel;
    /**
     * host memory, owned and bounded by the host, that core writes core ADR
     * 0058's native compile-error struct (ta_compile_error) into
     */
    uint64_t diagnostic;
} ta_h2c_definition_register;
_Static_assert(sizeof(ta_h2c_definition_register) == 56, "h2c.definition_register");
_Static_assert(offsetof(ta_h2c_definition_register, spec) == 32, "h2c.definition_register spec");
_Static_assert(offsetof(ta_h2c_definition_register, datamodel) == 40, "h2c.definition_register datamodel");
_Static_assert(offsetof(ta_h2c_definition_register, diagnostic) == 48, "h2c.definition_register diagnostic");

/** `h2c.definition_release` (0x010C, on h2c): Release one registration hold. */
typedef struct {
    ta_record_header header;
    /** the system-local definition ID returned by registration */
    uint64_t definition;
} ta_h2c_definition_release;
_Static_assert(sizeof(ta_h2c_definition_release) == 40, "h2c.definition_release");
_Static_assert(offsetof(ta_h2c_definition_release, definition) == 32, "h2c.definition_release definition");

/**
 * `h2c.definition_retire` (0x010D, on h2c): End a definition’s availability.
 */
typedef struct {
    ta_record_header header;
    /** the system-local definition ID */
    uint64_t definition;
} ta_h2c_definition_retire;
_Static_assert(sizeof(ta_h2c_definition_retire) == 40, "h2c.definition_retire");
_Static_assert(offsetof(ta_h2c_definition_retire, definition) == 32, "h2c.definition_retire definition");

/**
 * `h2c.withdraw` (0x010E, on h2c): Take back an admitted input that has not
 * been taken.
 */
typedef struct {
    ta_record_header header;
    /** the h2c seq of the input to withdraw */
    uint64_t target_seq;
} ta_h2c_withdraw;
_Static_assert(sizeof(ta_h2c_withdraw) == 40, "h2c.withdraw");
_Static_assert(offsetof(ta_h2c_withdraw, target_seq) == 32, "h2c.withdraw target_seq");

/**
 * `h2c.type_declare` (0x010F, on h2c): Declare an I/O processor or invoker
 * type.
 */
typedef struct {
    ta_record_header header;
    uint8_t registry; /* ta_record_type_registry */
    uint8_t pad1[3];
    /** the type name as bytes; interned as an atom when declared */
    ta_span32 type; /* text */
    uint8_t pad_end[4];
} ta_h2c_type_declare;
_Static_assert(sizeof(ta_h2c_type_declare) == 48, "h2c.type_declare");
_Static_assert(offsetof(ta_h2c_type_declare, registry) == 32, "h2c.type_declare registry");
_Static_assert(offsetof(ta_h2c_type_declare, type) == 36, "h2c.type_declare type");

/** `c2h.spawned` (0x0201, on c2h): A session exists. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    ta_span32 revision; /* bytes */
    /** the invoking session, or 0 */
    uint64_t parent;
    /** the h2c.spawn, or 0 for an <invoke> */
    uint64_t cause_seq;
} ta_c2h_spawned;
_Static_assert(sizeof(ta_c2h_spawned) == 64, "c2h.spawned");
_Static_assert(offsetof(ta_c2h_spawned, address) == 32, "c2h.spawned address");
_Static_assert(offsetof(ta_c2h_spawned, revision) == 40, "c2h.spawned revision");
_Static_assert(offsetof(ta_c2h_spawned, parent) == 48, "c2h.spawned parent");
_Static_assert(offsetof(ta_c2h_spawned, cause_seq) == 56, "c2h.spawned cause_seq");

/** `c2h.input` (0x0202, on c2h): A recorded session took an input. */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    /** source h2c: the h2c seq */
    uint64_t source_seq;
    /** source mail or timer: the sender */
    uint64_t from_session;
    ta_span32 name; /* text */
    ta_span32 sendid; /* text */
    ta_span32 origin; /* text */
    ta_span32 origin_type; /* text */
    ta_span32 invokeid; /* text */
    uint64_t data;
    /**
     * the W3C trace ID, high 64 bits; with trace_id_low all zero means no trace
     * context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /**
     * the span ID of the cause (ta_step_key_span_id): the host's parent-id, or
     * the sending step of mail and timers; 0 for none
     */
    uint64_t parent_span_id;
    uint8_t source; /* ta_record_input_source */
    uint8_t type; /* ta_record_event_type */
    /** the W3C trace-flags (sampled, random) */
    uint8_t trace_flags;
    /** the local detail byte (watched); never W3C, never in a traceparent */
    uint8_t trace_detail;
    /**
     * source invoke_report: original report invocation ID, separate from event
     * invokeid
     */
    ta_span32 report_invokeid; /* text */
    /** source invoke_report/error: original reason token */
    ta_span32 report_reason; /* text */
    /** source invoke_report only; otherwise ignored */
    uint8_t report_kind; /* ta_record_invoke_report_kind */
    /** source invoke_report/error: original retirement decision */
    uint8_t report_ends;
    uint8_t pad_end[2];
} ta_c2h_input;
_Static_assert(sizeof(ta_c2h_input) == 152, "c2h.input");
_Static_assert(offsetof(ta_c2h_input, macrostep) == 32, "c2h.input macrostep");
_Static_assert(offsetof(ta_c2h_input, source_seq) == 40, "c2h.input source_seq");
_Static_assert(offsetof(ta_c2h_input, from_session) == 48, "c2h.input from_session");
_Static_assert(offsetof(ta_c2h_input, name) == 56, "c2h.input name");
_Static_assert(offsetof(ta_c2h_input, sendid) == 64, "c2h.input sendid");
_Static_assert(offsetof(ta_c2h_input, origin) == 72, "c2h.input origin");
_Static_assert(offsetof(ta_c2h_input, origin_type) == 80, "c2h.input origin_type");
_Static_assert(offsetof(ta_c2h_input, invokeid) == 88, "c2h.input invokeid");
_Static_assert(offsetof(ta_c2h_input, data) == 96, "c2h.input data");
_Static_assert(offsetof(ta_c2h_input, trace_id_high) == 104, "c2h.input trace_id_high");
_Static_assert(offsetof(ta_c2h_input, trace_id_low) == 112, "c2h.input trace_id_low");
_Static_assert(offsetof(ta_c2h_input, parent_span_id) == 120, "c2h.input parent_span_id");
_Static_assert(offsetof(ta_c2h_input, source) == 128, "c2h.input source");
_Static_assert(offsetof(ta_c2h_input, type) == 129, "c2h.input type");
_Static_assert(offsetof(ta_c2h_input, trace_flags) == 130, "c2h.input trace_flags");
_Static_assert(offsetof(ta_c2h_input, trace_detail) == 131, "c2h.input trace_detail");
_Static_assert(offsetof(ta_c2h_input, report_invokeid) == 132, "c2h.input report_invokeid");
_Static_assert(offsetof(ta_c2h_input, report_reason) == 140, "c2h.input report_reason");
_Static_assert(offsetof(ta_c2h_input, report_kind) == 148, "c2h.input report_kind");
_Static_assert(offsetof(ta_c2h_input, report_ends) == 149, "c2h.input report_ends");

/** `c2h.send` (0x0203, on c2h): A send for an I/O processor. */
typedef struct {
    ta_record_header header;
    /** 0 for a delayed send that became due */
    uint64_t macrostep;
    ta_span32 sendid; /* text */
    ta_span32 io_type; /* text */
    ta_span32 target; /* text */
    ta_span32 name; /* text */
    /** the namelist and params, aggregated by the data model */
    uint64_t data;
    /** <content>, or 0 */
    uint64_t content;
    /**
     * the sending step’s W3C trace ID, high 64 bits; all zero with trace_id_low
     * when it has no trace context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /**
     * the sending step’s span ID (ta_step_key_span_id), the parent-id of a
     * traceparent header
     */
    uint64_t span_id;
    /** the W3C trace-flags */
    uint8_t trace_flags;
    uint8_t pad_end[7];
} ta_c2h_send;
_Static_assert(sizeof(ta_c2h_send) == 120, "c2h.send");
_Static_assert(offsetof(ta_c2h_send, macrostep) == 32, "c2h.send macrostep");
_Static_assert(offsetof(ta_c2h_send, sendid) == 40, "c2h.send sendid");
_Static_assert(offsetof(ta_c2h_send, io_type) == 48, "c2h.send io_type");
_Static_assert(offsetof(ta_c2h_send, target) == 56, "c2h.send target");
_Static_assert(offsetof(ta_c2h_send, name) == 64, "c2h.send name");
_Static_assert(offsetof(ta_c2h_send, data) == 72, "c2h.send data");
_Static_assert(offsetof(ta_c2h_send, content) == 80, "c2h.send content");
_Static_assert(offsetof(ta_c2h_send, trace_id_high) == 88, "c2h.send trace_id_high");
_Static_assert(offsetof(ta_c2h_send, trace_id_low) == 96, "c2h.send trace_id_low");
_Static_assert(offsetof(ta_c2h_send, span_id) == 104, "c2h.send span_id");
_Static_assert(offsetof(ta_c2h_send, trace_flags) == 112, "c2h.send trace_flags");

/** `c2h.invoke` (0x0204, on c2h): Start an invocation. */
typedef struct {
    ta_record_header header;
    /** the stabilization macrostep that enabled this startup */
    uint64_t macrostep;
    ta_span32 invokeid; /* text */
    ta_span32 type; /* text */
    ta_span32 src; /* text */
    uint64_t data;
    /** evaluated content, or 0 */
    uint64_t content;
    /**
     * the sending step’s W3C trace ID, high 64 bits; all zero with trace_id_low
     * when it has no trace context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /**
     * the sending step’s span ID (ta_step_key_span_id), the parent-id of a
     * traceparent header
     */
    uint64_t span_id;
    uint8_t autoforward;
    /** the W3C trace-flags */
    uint8_t trace_flags;
    uint8_t pad_end[6];
} ta_c2h_invoke;
_Static_assert(sizeof(ta_c2h_invoke) == 112, "c2h.invoke");
_Static_assert(offsetof(ta_c2h_invoke, macrostep) == 32, "c2h.invoke macrostep");
_Static_assert(offsetof(ta_c2h_invoke, invokeid) == 40, "c2h.invoke invokeid");
_Static_assert(offsetof(ta_c2h_invoke, type) == 48, "c2h.invoke type");
_Static_assert(offsetof(ta_c2h_invoke, src) == 56, "c2h.invoke src");
_Static_assert(offsetof(ta_c2h_invoke, data) == 64, "c2h.invoke data");
_Static_assert(offsetof(ta_c2h_invoke, content) == 72, "c2h.invoke content");
_Static_assert(offsetof(ta_c2h_invoke, trace_id_high) == 80, "c2h.invoke trace_id_high");
_Static_assert(offsetof(ta_c2h_invoke, trace_id_low) == 88, "c2h.invoke trace_id_low");
_Static_assert(offsetof(ta_c2h_invoke, span_id) == 96, "c2h.invoke span_id");
_Static_assert(offsetof(ta_c2h_invoke, autoforward) == 104, "c2h.invoke autoforward");
_Static_assert(offsetof(ta_c2h_invoke, trace_flags) == 105, "c2h.invoke trace_flags");

/** `c2h.invoke_cancel` (0x0205, on c2h): Cancel an invocation. */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    ta_span32 invokeid; /* text */
    /** the canceled c2h.invoke instance */
    uint64_t invocation_seq;
} ta_c2h_invoke_cancel;
_Static_assert(sizeof(ta_c2h_invoke_cancel) == 56, "c2h.invoke_cancel");
_Static_assert(offsetof(ta_c2h_invoke_cancel, macrostep) == 32, "c2h.invoke_cancel macrostep");
_Static_assert(offsetof(ta_c2h_invoke_cancel, invokeid) == 40, "c2h.invoke_cancel invokeid");
_Static_assert(offsetof(ta_c2h_invoke_cancel, invocation_seq) == 48, "c2h.invoke_cancel invocation_seq");

/**
 * `c2h.invoke_forward` (0x0206, on c2h): Autoforward an event to an invocation.
 */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    /** destination invocation, not the forwarded event invokeid */
    ta_span32 invokeid; /* text */
    /** destination c2h.invoke instance */
    uint64_t invocation_seq;
    ta_span32 name; /* text */
    ta_span32 sendid; /* text */
    ta_span32 origin; /* text */
    /** the original event origintype */
    ta_span32 origin_type; /* text */
    /** the original event invokeid */
    ta_span32 event_invokeid; /* text */
    uint64_t data;
    /**
     * the forwarding step’s W3C trace ID, high 64 bits; all zero with
     * trace_id_low when it has no trace context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /**
     * the forwarding step’s span ID (ta_step_key_span_id), the parent-id of a
     * traceparent header
     */
    uint64_t span_id;
    uint8_t type; /* ta_record_event_type */
    /** the W3C trace-flags */
    uint8_t trace_flags;
    uint8_t pad_end[6];
} ta_c2h_invoke_forward;
_Static_assert(sizeof(ta_c2h_invoke_forward) == 136, "c2h.invoke_forward");
_Static_assert(offsetof(ta_c2h_invoke_forward, macrostep) == 32, "c2h.invoke_forward macrostep");
_Static_assert(offsetof(ta_c2h_invoke_forward, invokeid) == 40, "c2h.invoke_forward invokeid");
_Static_assert(offsetof(ta_c2h_invoke_forward, invocation_seq) == 48, "c2h.invoke_forward invocation_seq");
_Static_assert(offsetof(ta_c2h_invoke_forward, name) == 56, "c2h.invoke_forward name");
_Static_assert(offsetof(ta_c2h_invoke_forward, sendid) == 64, "c2h.invoke_forward sendid");
_Static_assert(offsetof(ta_c2h_invoke_forward, origin) == 72, "c2h.invoke_forward origin");
_Static_assert(offsetof(ta_c2h_invoke_forward, origin_type) == 80, "c2h.invoke_forward origin_type");
_Static_assert(offsetof(ta_c2h_invoke_forward, event_invokeid) == 88, "c2h.invoke_forward event_invokeid");
_Static_assert(offsetof(ta_c2h_invoke_forward, data) == 96, "c2h.invoke_forward data");
_Static_assert(offsetof(ta_c2h_invoke_forward, trace_id_high) == 104, "c2h.invoke_forward trace_id_high");
_Static_assert(offsetof(ta_c2h_invoke_forward, trace_id_low) == 112, "c2h.invoke_forward trace_id_low");
_Static_assert(offsetof(ta_c2h_invoke_forward, span_id) == 120, "c2h.invoke_forward span_id");
_Static_assert(offsetof(ta_c2h_invoke_forward, type) == 128, "c2h.invoke_forward type");
_Static_assert(offsetof(ta_c2h_invoke_forward, trace_flags) == 129, "c2h.invoke_forward trace_flags");

/** `c2h.log` (0x0207, on c2h): A <log>. */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    ta_span32 label; /* text */
    uint64_t value;
    /**
     * the logging step’s W3C trace ID, high 64 bits; all zero with trace_id_low
     * when it has no trace context
     */
    uint64_t trace_id_high;
    /** the W3C trace ID, low 64 bits */
    uint64_t trace_id_low;
    /**
     * the logging step’s span ID (ta_step_key_span_id), the parent-id of a
     * traceparent header
     */
    uint64_t span_id;
    /** the W3C trace-flags */
    uint8_t trace_flags;
    uint8_t pad_end[7];
} ta_c2h_log;
_Static_assert(sizeof(ta_c2h_log) == 88, "c2h.log");
_Static_assert(offsetof(ta_c2h_log, macrostep) == 32, "c2h.log macrostep");
_Static_assert(offsetof(ta_c2h_log, label) == 40, "c2h.log label");
_Static_assert(offsetof(ta_c2h_log, value) == 48, "c2h.log value");
_Static_assert(offsetof(ta_c2h_log, trace_id_high) == 56, "c2h.log trace_id_high");
_Static_assert(offsetof(ta_c2h_log, trace_id_low) == 64, "c2h.log trace_id_low");
_Static_assert(offsetof(ta_c2h_log, span_id) == 72, "c2h.log span_id");
_Static_assert(offsetof(ta_c2h_log, trace_flags) == 80, "c2h.log trace_flags");

/** `c2h.macrostep_ended` (0x0208, on c2h): A macrostep ended. */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    uint8_t source; /* ta_record_input_source */
    uint8_t pad2[7];
    /**
     * originating h2c input, including internal startup follow-up; 0 for local
     * work
     */
    uint64_t source_seq;
    /** records this macrostep wrote to c2h, this end report excluded */
    uint32_t effects;
    /** envelopes it put in the pool */
    uint32_t mail;
    uint8_t status; /* ta_record_macrostep_status */
    uint8_t pad_end[7];
} ta_c2h_macrostep_ended;
_Static_assert(sizeof(ta_c2h_macrostep_ended) == 72, "c2h.macrostep_ended");
_Static_assert(offsetof(ta_c2h_macrostep_ended, macrostep) == 32, "c2h.macrostep_ended macrostep");
_Static_assert(offsetof(ta_c2h_macrostep_ended, source) == 40, "c2h.macrostep_ended source");
_Static_assert(offsetof(ta_c2h_macrostep_ended, source_seq) == 48, "c2h.macrostep_ended source_seq");
_Static_assert(offsetof(ta_c2h_macrostep_ended, effects) == 56, "c2h.macrostep_ended effects");
_Static_assert(offsetof(ta_c2h_macrostep_ended, mail) == 60, "c2h.macrostep_ended mail");
_Static_assert(offsetof(ta_c2h_macrostep_ended, status) == 64, "c2h.macrostep_ended status");

/** `c2h.wake` (0x0209, on c2h): Call core back at this time. */
typedef struct {
    ta_record_header header;
    uint64_t due;
} ta_c2h_wake;
_Static_assert(sizeof(ta_c2h_wake) == 40, "c2h.wake");
_Static_assert(offsetof(ta_c2h_wake, due) == 32, "c2h.wake due");

/** `c2h.bounced` (0x020A, on c2h): An h2c record was not run. */
typedef struct {
    ta_record_header header;
    uint64_t source_seq;
    ta_span32 detail; /* text */
    uint8_t reason; /* ta_record_bounce_reason */
    uint8_t pad_end[7];
} ta_c2h_bounced;
_Static_assert(sizeof(ta_c2h_bounced) == 56, "c2h.bounced");
_Static_assert(offsetof(ta_c2h_bounced, source_seq) == 32, "c2h.bounced source_seq");
_Static_assert(offsetof(ta_c2h_bounced, detail) == 40, "c2h.bounced detail");
_Static_assert(offsetof(ta_c2h_bounced, reason) == 48, "c2h.bounced reason");

/**
 * `c2h.load_needed` (0x020B, on c2h): Envelopes wait for an unloaded address.
 */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** envelopes waiting for it */
    uint32_t waiting;
    uint8_t pad_end[4];
} ta_c2h_load_needed;
_Static_assert(sizeof(ta_c2h_load_needed) == 48, "c2h.load_needed");
_Static_assert(offsetof(ta_c2h_load_needed, address) == 32, "c2h.load_needed address");
_Static_assert(offsetof(ta_c2h_load_needed, waiting) == 40, "c2h.load_needed waiting");

/** `c2h.loaded` (0x020C, on c2h): A session is resident again. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the h2c.load */
    uint64_t cause_seq;
} ta_c2h_loaded;
_Static_assert(sizeof(ta_c2h_loaded) == 48, "c2h.loaded");
_Static_assert(offsetof(ta_c2h_loaded, address) == 32, "c2h.loaded address");
_Static_assert(offsetof(ta_c2h_loaded, cause_seq) == 40, "c2h.loaded cause_seq");

/** `c2h.unloaded` (0x020D, on c2h): A session left memory. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the h2c.unload */
    uint64_t cause_seq;
    uint64_t image;
    /** complete valid bytes written into the requested sink */
    uint64_t image_bytes;
    /** its earliest delayed send */
    uint64_t wake_at;
} ta_c2h_unloaded;
_Static_assert(sizeof(ta_c2h_unloaded) == 72, "c2h.unloaded");
_Static_assert(offsetof(ta_c2h_unloaded, address) == 32, "c2h.unloaded address");
_Static_assert(offsetof(ta_c2h_unloaded, cause_seq) == 40, "c2h.unloaded cause_seq");
_Static_assert(offsetof(ta_c2h_unloaded, image) == 48, "c2h.unloaded image");
_Static_assert(offsetof(ta_c2h_unloaded, image_bytes) == 56, "c2h.unloaded image_bytes");
_Static_assert(offsetof(ta_c2h_unloaded, wake_at) == 64, "c2h.unloaded wake_at");

/**
 * `c2h.state` (0x020E, on c2h): A recorded session's state at a point of its
 * log.
 */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the h2c.checkpoint */
    uint64_t cause_seq;
    uint64_t image;
    /** complete valid bytes written into the requested sink */
    uint64_t image_bytes;
    /** the last c2h.input it includes */
    uint64_t input_seq;
} ta_c2h_state;
_Static_assert(sizeof(ta_c2h_state) == 72, "c2h.state");
_Static_assert(offsetof(ta_c2h_state, address) == 32, "c2h.state address");
_Static_assert(offsetof(ta_c2h_state, cause_seq) == 40, "c2h.state cause_seq");
_Static_assert(offsetof(ta_c2h_state, image) == 48, "c2h.state image");
_Static_assert(offsetof(ta_c2h_state, image_bytes) == 56, "c2h.state image_bytes");
_Static_assert(offsetof(ta_c2h_state, input_seq) == 64, "c2h.state input_seq");

/** `c2h.published` (0x020F, on c2h): A replayed session is live. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the h2c.publish */
    uint64_t cause_seq;
    /** inputs it replayed; total for all-session publication */
    uint64_t replayed;
    /** held delayed sends due by `at` */
    uint64_t dropped_timers;
    uint64_t retired_invokes;
} ta_c2h_published;
_Static_assert(sizeof(ta_c2h_published) == 72, "c2h.published");
_Static_assert(offsetof(ta_c2h_published, address) == 32, "c2h.published address");
_Static_assert(offsetof(ta_c2h_published, cause_seq) == 40, "c2h.published cause_seq");
_Static_assert(offsetof(ta_c2h_published, replayed) == 48, "c2h.published replayed");
_Static_assert(offsetof(ta_c2h_published, dropped_timers) == 56, "c2h.published dropped_timers");
_Static_assert(offsetof(ta_c2h_published, retired_invokes) == 64, "c2h.published retired_invokes");

/**
 * `c2h.finished` (0x0210, on c2h): A session reached a top-level final state.
 */
typedef struct {
    ta_record_header header;
    uint64_t macrostep;
    uint64_t donedata;
} ta_c2h_finished;
_Static_assert(sizeof(ta_c2h_finished) == 48, "c2h.finished");
_Static_assert(offsetof(ta_c2h_finished, macrostep) == 32, "c2h.finished macrostep");
_Static_assert(offsetof(ta_c2h_finished, donedata) == 40, "c2h.finished donedata");

/** `c2h.destroyed` (0x0211, on c2h): A session is gone. */
typedef struct {
    ta_record_header header;
    ta_span32 address; /* text */
    /** the h2c.destroy, or 0 for execution-driven destruction */
    uint64_t cause_seq;
    uint8_t reason; /* ta_record_destroy_reason */
    uint8_t pad_end[7];
} ta_c2h_destroyed;
_Static_assert(sizeof(ta_c2h_destroyed) == 56, "c2h.destroyed");
_Static_assert(offsetof(ta_c2h_destroyed, address) == 32, "c2h.destroyed address");
_Static_assert(offsetof(ta_c2h_destroyed, cause_seq) == 40, "c2h.destroyed cause_seq");
_Static_assert(offsetof(ta_c2h_destroyed, reason) == 48, "c2h.destroyed reason");

/** `c2h.definition_result` (0x0212, on c2h): A definition request completed. */
typedef struct {
    ta_record_header header;
    /** the definition request’s h2c seq */
    uint64_t cause_seq;
    /** on successful register: the definition ID; otherwise 0 */
    uint64_t definition;
    /**
     * bytes written to the request’s diagnostic buffer; 0 without a compile
     * error
     */
    uint32_t diagnostic_bytes;
    uint8_t status; /* ta_record_definition_status */
    uint8_t pad_end[3];
} ta_c2h_definition_result;
_Static_assert(sizeof(ta_c2h_definition_result) == 56, "c2h.definition_result");
_Static_assert(offsetof(ta_c2h_definition_result, cause_seq) == 32, "c2h.definition_result cause_seq");
_Static_assert(offsetof(ta_c2h_definition_result, definition) == 40, "c2h.definition_result definition");
_Static_assert(offsetof(ta_c2h_definition_result, diagnostic_bytes) == 48, "c2h.definition_result diagnostic_bytes");
_Static_assert(offsetof(ta_c2h_definition_result, status) == 52, "c2h.definition_result status");

/**
 * `c2h.input_refused` (0x0213, on c2h): An admitted host input was not taken.
 */
typedef struct {
    ta_record_header header;
    /** the originating h2c input */
    uint64_t source_seq;
    uint8_t reason; /* ta_record_input_refusal_reason */
    uint8_t pad_end[7];
} ta_c2h_input_refused;
_Static_assert(sizeof(ta_c2h_input_refused) == 48, "c2h.input_refused");
_Static_assert(offsetof(ta_c2h_input_refused, source_seq) == 32, "c2h.input_refused source_seq");
_Static_assert(offsetof(ta_c2h_input_refused, reason) == 40, "c2h.input_refused reason");

/**
 * `c2h.input_completed` (0x0214, on c2h): A host input and its startup
 * follow-up settled.
 */
typedef struct {
    ta_record_header header;
    /** the originating h2c input */
    uint64_t source_seq;
    /** the last macrostep in the input execution */
    uint64_t macrostep;
    uint8_t status; /* ta_record_input_completion_status */
    uint8_t pad_end[7];
} ta_c2h_input_completed;
_Static_assert(sizeof(ta_c2h_input_completed) == 56, "c2h.input_completed");
_Static_assert(offsetof(ta_c2h_input_completed, source_seq) == 32, "c2h.input_completed source_seq");
_Static_assert(offsetof(ta_c2h_input_completed, macrostep) == 40, "c2h.input_completed macrostep");
_Static_assert(offsetof(ta_c2h_input_completed, status) == 48, "c2h.input_completed status");

/**
 * `c2h.load_result` (0x0215, on c2h): A load-failure response finished
 * recycling waiting inputs.
 */
typedef struct {
    ta_record_header header;
    /** the h2c.load_failed */
    uint64_t cause_seq;
    /** the answered c2h.load_needed */
    uint64_t request_seq;
} ta_c2h_load_result;
_Static_assert(sizeof(ta_c2h_load_result) == 48, "c2h.load_result");
_Static_assert(offsetof(ta_c2h_load_result, cause_seq) == 32, "c2h.load_result cause_seq");
_Static_assert(offsetof(ta_c2h_load_result, request_seq) == 40, "c2h.load_result request_seq");

/** `tel.trace` (0x0301, on tel): One trace record (core ADR 0067). */
typedef struct {
    ta_record_header header;
    /**
     * the host clock when core wrote the record: wall-clock nanoseconds since
     * the Unix epoch; 0 when the host installed no clock
     */
    uint64_t wall_ns;
    /**
     * with header session and microstep, the step key; 0 outside a macrostep
     */
    uint64_t macrostep;
    uint64_t trace_id_high;
    uint64_t trace_id_low;
    /**
     * the session of the step that caused this one; 0 for a host-delivered
     * cause
     */
    uint64_t cause_session;
    /**
     * the macrostep of the causing step; with cause_session 0, the host's
     * parent span ID (W3C parent-id), or 0
     */
    uint64_t cause_macrostep;
    /** from 1 per macrostep; 0 for the macrostep itself */
    uint32_t microstep;
    /**
     * the microstep of the causing step; with cause_session and
     * cause_macrostep, the cause's step key, whose span ID is the parent span
     */
    uint32_t cause_microstep;
    uint32_t state;
    uint32_t transition;
    uint32_t action;
    uint32_t iteration;
    /** the fault’s code, with fault_detail */
    int32_t fault_code;
    /** ta_trace_kind */
    uint16_t trace_kind;
    /** ta_trace_at */
    uint8_t position;
    uint8_t result;
    /** the trace context’s flags (sampled) */
    uint8_t trace_flags;
    /** the trace context’s detail (watched) */
    uint8_t trace_detail;
    /** RUNAWAY only: what stopped the input */
    uint8_t runaway_cause; /* ta_record_runaway_cause */
    uint8_t pad19[1];
    /** the session’s family/name */
    ta_span32 address; /* text */
    /**
     * the definition’s document name and source, on the kinds that open a run
     */
    ta_span32 definition; /* text */
    /** the name of `state` */
    ta_span32 state_name; /* text */
    /** the name of `transition` */
    ta_span32 transition_name; /* text */
    /** the name of `action` */
    ta_span32 action_name; /* text */
    /** the event name, or the name the kind carries */
    ta_span32 name; /* text */
    /** the send ID, for SENT and its kin */
    ta_span32 send_id; /* text */
    /** the invoke ID, for INVOKED and its kin */
    ta_span32 invoke_id; /* text */
    ta_span32 value_text; /* text */
    /** the fault’s error event name */
    ta_span32 fault_detail; /* text */
    ta_span32 fault_message; /* text */
    /**
     * the microstep’s transitions: a u32 count, then per transition its u32 ID,
     * then its name as a u32 length and UTF-8 bytes
     */
    ta_span32 transitions; /* bytes */
    /**
     * exited, entered, history, states: per list a u32 count, then per state
     * its u32 ID, then its name as a u32 length and UTF-8 bytes
     */
    ta_span32 states; /* bytes */
    uint8_t pad_end[4];
} ta_tel_trace;
_Static_assert(sizeof(ta_tel_trace) == 224, "tel.trace");
_Static_assert(offsetof(ta_tel_trace, wall_ns) == 32, "tel.trace wall_ns");
_Static_assert(offsetof(ta_tel_trace, macrostep) == 40, "tel.trace macrostep");
_Static_assert(offsetof(ta_tel_trace, trace_id_high) == 48, "tel.trace trace_id_high");
_Static_assert(offsetof(ta_tel_trace, trace_id_low) == 56, "tel.trace trace_id_low");
_Static_assert(offsetof(ta_tel_trace, cause_session) == 64, "tel.trace cause_session");
_Static_assert(offsetof(ta_tel_trace, cause_macrostep) == 72, "tel.trace cause_macrostep");
_Static_assert(offsetof(ta_tel_trace, microstep) == 80, "tel.trace microstep");
_Static_assert(offsetof(ta_tel_trace, cause_microstep) == 84, "tel.trace cause_microstep");
_Static_assert(offsetof(ta_tel_trace, state) == 88, "tel.trace state");
_Static_assert(offsetof(ta_tel_trace, transition) == 92, "tel.trace transition");
_Static_assert(offsetof(ta_tel_trace, action) == 96, "tel.trace action");
_Static_assert(offsetof(ta_tel_trace, iteration) == 100, "tel.trace iteration");
_Static_assert(offsetof(ta_tel_trace, fault_code) == 104, "tel.trace fault_code");
_Static_assert(offsetof(ta_tel_trace, trace_kind) == 108, "tel.trace trace_kind");
_Static_assert(offsetof(ta_tel_trace, position) == 110, "tel.trace position");
_Static_assert(offsetof(ta_tel_trace, result) == 111, "tel.trace result");
_Static_assert(offsetof(ta_tel_trace, trace_flags) == 112, "tel.trace trace_flags");
_Static_assert(offsetof(ta_tel_trace, trace_detail) == 113, "tel.trace trace_detail");
_Static_assert(offsetof(ta_tel_trace, runaway_cause) == 114, "tel.trace runaway_cause");
_Static_assert(offsetof(ta_tel_trace, address) == 116, "tel.trace address");
_Static_assert(offsetof(ta_tel_trace, definition) == 124, "tel.trace definition");
_Static_assert(offsetof(ta_tel_trace, state_name) == 132, "tel.trace state_name");
_Static_assert(offsetof(ta_tel_trace, transition_name) == 140, "tel.trace transition_name");
_Static_assert(offsetof(ta_tel_trace, action_name) == 148, "tel.trace action_name");
_Static_assert(offsetof(ta_tel_trace, name) == 156, "tel.trace name");
_Static_assert(offsetof(ta_tel_trace, send_id) == 164, "tel.trace send_id");
_Static_assert(offsetof(ta_tel_trace, invoke_id) == 172, "tel.trace invoke_id");
_Static_assert(offsetof(ta_tel_trace, value_text) == 180, "tel.trace value_text");
_Static_assert(offsetof(ta_tel_trace, fault_detail) == 188, "tel.trace fault_detail");
_Static_assert(offsetof(ta_tel_trace, fault_message) == 196, "tel.trace fault_message");
_Static_assert(offsetof(ta_tel_trace, transitions) == 204, "tel.trace transitions");
_Static_assert(offsetof(ta_tel_trace, states) == 212, "tel.trace states");

/** `tel.dropped` (0x0302, on tel): Telemetry records were dropped. */
typedef struct {
    ta_record_header header;
    uint64_t count;
    /** the seq the first dropped record would have had */
    uint64_t first_seq;
} ta_tel_dropped;
_Static_assert(sizeof(ta_tel_dropped) == 48, "tel.dropped");
_Static_assert(offsetof(ta_tel_dropped, count) == 32, "tel.dropped count");
_Static_assert(offsetof(ta_tel_dropped, first_seq) == 40, "tel.dropped first_seq");
