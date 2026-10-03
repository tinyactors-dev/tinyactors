/* Generated from catalog.rb by render.rb. Do not edit. A sketch of the
 * layout; the real header would be part of tinyactors.h. */
#pragma once
#include <stdint.h>

typedef struct { uint32_t offset; uint32_t length; } trp_span;

/* The header's `kind`: the message code. 0x0000 is the pad record. */
typedef enum {
    TRP_PAD = 0x0000,
    TRP_H2C_TIME = 0x0101,
    TRP_H2C_EVENT = 0x0102,
    TRP_H2C_SPAWN = 0x0103,
    TRP_H2C_PUBLISH = 0x0104,
    TRP_H2C_LOAD = 0x0105,
    TRP_H2C_LOAD_FAILED = 0x0106,
    TRP_H2C_UNLOAD = 0x0107,
    TRP_H2C_CHECKPOINT = 0x0108,
    TRP_H2C_INVOKE_REPORT = 0x0109,
    TRP_H2C_DESTROY = 0x010A,
    TRP_H2C_DEFINITION_REGISTER = 0x010B,
    TRP_H2C_DEFINITION_RELEASE = 0x010C,
    TRP_H2C_DEFINITION_RETIRE = 0x010D,
    TRP_H2C_WITHDRAW = 0x010E,
    TRP_C2H_SPAWNED = 0x0201,
    TRP_C2H_INPUT = 0x0202,
    TRP_C2H_SEND = 0x0203,
    TRP_C2H_INVOKE = 0x0204,
    TRP_C2H_INVOKE_CANCEL = 0x0205,
    TRP_C2H_INVOKE_FORWARD = 0x0206,
    TRP_C2H_LOG = 0x0207,
    TRP_C2H_MACROSTEP_ENDED = 0x0208,
    TRP_C2H_WAKE = 0x0209,
    TRP_C2H_BOUNCED = 0x020A,
    TRP_C2H_LOAD_NEEDED = 0x020B,
    TRP_C2H_LOADED = 0x020C,
    TRP_C2H_UNLOADED = 0x020D,
    TRP_C2H_STATE = 0x020E,
    TRP_C2H_PUBLISHED = 0x020F,
    TRP_C2H_FINISHED = 0x0210,
    TRP_C2H_DESTROYED = 0x0211,
    TRP_C2H_DEFINITION_RESULT = 0x0212,
    TRP_C2H_INPUT_REFUSED = 0x0213,
    TRP_C2H_INPUT_COMPLETED = 0x0214,
    TRP_C2H_LOAD_RESULT = 0x0215,
    TRP_TEL_TRACE = 0x0301,
    TRP_TEL_DROPPED = 0x0302,
} trp_kind;

/* Common flags, bits 8..15 of the header's `flags`. */
typedef enum {
    TRP_FLAG_REPLAYING = 1 << 8,
    TRP_FLAG_RECORDED = 1 << 9,
} trp_common_flag;

/* h2c.spawn's own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    TRP_H2C_SPAWN_REPLAYING_FROM_CLOCK = 1 << 0,
} trp_h2c_spawn_flag;

/* c2h.wake's own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    TRP_C2H_WAKE_NONE = 1 << 0,
} trp_c2h_wake_flag;

/* c2h.unloaded's own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    TRP_C2H_UNLOADED_HAS_WAKE = 1 << 0,
} trp_c2h_unloaded_flag;

/* tel.trace's own flags, bits 0..7 of the header's `flags`. */
typedef enum {
    TRP_TEL_TRACE_TRUNCATED = 1 << 0,
    TRP_TEL_TRACE_SUPPRESSED = 1 << 1,
} trp_tel_trace_flag;

/* What _event.type says (SCXML 5.10.1). Stored as uint8_t. */
typedef enum {
    TRP_EVENT_TYPE_PLATFORM = 0,
    TRP_EVENT_TYPE_INTERNAL = 1,
    TRP_EVENT_TYPE_EXTERNAL = 2,
} trp_event_type;

/* Where the envelope behind a c2h.input came from. Stored as uint8_t. */
typedef enum {
    TRP_INPUT_SOURCE_H2C = 0,
    TRP_INPUT_SOURCE_MAIL = 1,
    TRP_INPUT_SOURCE_TIMER = 2,
    TRP_INPUT_SOURCE_PLATFORM = 3,
    TRP_INPUT_SOURCE_INVOKE_REPORT = 4,
} trp_input_source;

/* How a macrostep ended. Stored as uint8_t. */
typedef enum {
    TRP_MACROSTEP_STATUS_STABLE = 0,
    TRP_MACROSTEP_STATUS_DONE = 1,
    TRP_MACROSTEP_STATUS_RUNAWAY = 2,
    TRP_MACROSTEP_STATUS_ABANDONED = 3,
} trp_macrostep_status;

/* How an input and its invocation-startup follow-up settled. Stored as uint8_t. */
typedef enum {
    TRP_INPUT_COMPLETION_STATUS_STABLE = 0,
    TRP_INPUT_COMPLETION_STATUS_DONE = 1,
    TRP_INPUT_COMPLETION_STATUS_RUNAWAY = 2,
    TRP_INPUT_COMPLETION_STATUS_ABANDONED = 3,
} trp_input_completion_status;

/* Why an admitted host input was discarded before take. Stored as uint8_t. */
typedef enum {
    TRP_INPUT_REFUSAL_REASON_LOAD_FAILED = 1,
    TRP_INPUT_REFUSAL_REASON_DESTROYED = 2,
    TRP_INPUT_REFUSAL_REASON_SNAPSHOT_FAILED = 3,
    TRP_INPUT_REFUSAL_REASON_STALE_INVOCATION = 4,
    TRP_INPUT_REFUSAL_REASON_INVALID = 5,
    TRP_INPUT_REFUSAL_REASON_WITHDRAWN = 6,
} trp_input_refusal_reason;

/* Why core handed an h2c record back unrun. Stored as uint8_t. */
typedef enum {
    TRP_BOUNCE_REASON_NO_SUCH_ADDRESS = 1,
    TRP_BOUNCE_REASON_NO_SUCH_DEFINITION = 2,
    TRP_BOUNCE_REASON_ADDRESS_TAKEN = 3,
    TRP_BOUNCE_REASON_HAS_MAIL = 4,
    TRP_BOUNCE_REASON_NOT_REPLAYING = 5,
    TRP_BOUNCE_REASON_INVALID = 6,
    TRP_BOUNCE_REASON_POOL = 7,
    TRP_BOUNCE_REASON_NAMES = 8,
    TRP_BOUNCE_REASON_ALLOCATION = 9,
    TRP_BOUNCE_REASON_ACTIVE_LOCAL_INVOKE = 10,
    TRP_BOUNCE_REASON_PENDING_INPUTS = 11,
    TRP_BOUNCE_REASON_STALE_REQUEST = 12,
    TRP_BOUNCE_REASON_RECORD = 13,
    TRP_BOUNCE_REASON_ACTIVE = 14,
} trp_bounce_reason;

/* The last segment of error.execution.capacity.<resource>, the event an action raises when a bounded resource has no room for it. Its data names the action (reference/macrostep.md, "Two kinds of exhaustion"). Stored as uint8_t. */
typedef enum {
    TRP_CAPACITY_RESOURCE_POOL = 1,
    TRP_CAPACITY_RESOURCE_NAMES = 2,
    TRP_CAPACITY_RESOURCE_C2H = 3,
    TRP_CAPACITY_RESOURCE_INTERNAL = 4,
    TRP_CAPACITY_RESOURCE_RECORD = 5,
} trp_capacity_resource;

/*  Stored as uint8_t. */
typedef enum {
    TRP_INVOKE_REPORT_KIND_DONE = 0,
    TRP_INVOKE_REPORT_KIND_ERROR = 1,
} trp_invoke_report_kind;

/*  Stored as uint8_t. */
typedef enum {
    TRP_DESTROY_REASON_FINISHED = 0,
    TRP_DESTROY_REASON_OPERATOR = 1,
    TRP_DESTROY_REASON_PARENT_EXITED = 2,
} trp_destroy_reason;

/* The result of a definition request. Stored as uint8_t. */
typedef enum {
    TRP_DEFINITION_STATUS_OK = 0,
    TRP_DEFINITION_STATUS_COMPILE_ERROR = 1,
    TRP_DEFINITION_STATUS_CAPACITY = 2,
    TRP_DEFINITION_STATUS_MISMATCH = 3,
    TRP_DEFINITION_STATUS_NOT_FOUND = 4,
    TRP_DEFINITION_STATUS_INVALID = 5,
} trp_definition_status;

typedef struct {
    uint16_t kind; /* trp_kind */
    uint16_t flags;
    uint32_t size;
    uint64_t seq;
    uint64_t session;
    uint64_t at;
} trp_header;
_Static_assert(sizeof(trp_header) == 32, "header");

/* h2c.time: Move the system clock. */
typedef struct {
    trp_header header;
} trp_h2c_time;
_Static_assert(sizeof(trp_h2c_time) == 32, "h2c.time");

/* h2c.event: An external event for a session. */
typedef struct {
    trp_header header;
    trp_span target;
    trp_span name;
    trp_span sendid;
    trp_span origin;
    trp_span origin_type;
    trp_span invokeid;
    uint64_t invocation_seq;
    uint64_t data;
    uint8_t type; /* trp_event_type */
    uint8_t pad_end[7];
} trp_h2c_event;
_Static_assert(sizeof(trp_h2c_event) == 104, "h2c.event");

/* h2c.spawn: Create a session. */
typedef struct {
    trp_header header;
    trp_span address;
    trp_span revision;
    uint64_t data;
    uint64_t clock;
    uint64_t state;
    uint64_t request_seq;
} trp_h2c_spawn;
_Static_assert(sizeof(trp_h2c_spawn) == 80, "h2c.spawn");

/* h2c.publish: End a replay and put the session on the route. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t at;
} trp_h2c_publish;
_Static_assert(sizeof(trp_h2c_publish) == 48, "h2c.publish");

/* h2c.load: Bring an unloaded session back. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t image;
    uint64_t request_seq;
} trp_h2c_load;
_Static_assert(sizeof(trp_h2c_load) == 56, "h2c.load");

/* h2c.load_failed: The session cannot be loaded. */
typedef struct {
    trp_header header;
    trp_span address;
    trp_span reason;
    uint64_t request_seq;
} trp_h2c_load_failed;
_Static_assert(sizeof(trp_h2c_load_failed) == 56, "h2c.load_failed");

/* h2c.unload: Page a stable session out. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t image;
} trp_h2c_unload;
_Static_assert(sizeof(trp_h2c_unload) == 48, "h2c.unload");

/* h2c.checkpoint: Ask for a recorded session's state. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t image;
} trp_h2c_checkpoint;
_Static_assert(sizeof(trp_h2c_checkpoint) == 48, "h2c.checkpoint");

/* h2c.invoke_report: An invoked service finished or failed. */
typedef struct {
    trp_header header;
    trp_span invokeid;
    uint64_t invocation_seq;
    trp_span reason;
    uint64_t data;
    uint8_t kind; /* trp_invoke_report_kind */
    uint8_t ends;
    uint8_t pad_end[6];
} trp_h2c_invoke_report;
_Static_assert(sizeof(trp_h2c_invoke_report) == 72, "h2c.invoke_report");

/* h2c.destroy: End a session now. */
typedef struct {
    trp_header header;
    trp_span address;
} trp_h2c_destroy;
_Static_assert(sizeof(trp_h2c_destroy) == 40, "h2c.destroy");

/* h2c.definition_register: Compile and register a complete definition. */
typedef struct {
    trp_header header;
    uint64_t spec;
    uint64_t datamodel;
    uint64_t diagnostic;
} trp_h2c_definition_register;
_Static_assert(sizeof(trp_h2c_definition_register) == 56, "h2c.definition_register");

/* h2c.definition_release: Release one registration hold. */
typedef struct {
    trp_header header;
    uint64_t definition;
} trp_h2c_definition_release;
_Static_assert(sizeof(trp_h2c_definition_release) == 40, "h2c.definition_release");

/* h2c.definition_retire: End a definition’s availability. */
typedef struct {
    trp_header header;
    uint64_t definition;
} trp_h2c_definition_retire;
_Static_assert(sizeof(trp_h2c_definition_retire) == 40, "h2c.definition_retire");

/* h2c.withdraw: Take back an admitted input that has not been taken. */
typedef struct {
    trp_header header;
    uint64_t target_seq;
} trp_h2c_withdraw;
_Static_assert(sizeof(trp_h2c_withdraw) == 40, "h2c.withdraw");

/* c2h.spawned: A session exists. */
typedef struct {
    trp_header header;
    trp_span address;
    trp_span revision;
    uint64_t parent;
    uint64_t cause_seq;
} trp_c2h_spawned;
_Static_assert(sizeof(trp_c2h_spawned) == 64, "c2h.spawned");

/* c2h.input: A recorded session took an input. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    uint64_t source_seq;
    uint64_t from_session;
    trp_span name;
    trp_span sendid;
    trp_span origin;
    trp_span origin_type;
    trp_span invokeid;
    uint64_t data;
    uint8_t source; /* trp_input_source */
    uint8_t type; /* trp_event_type */
    uint8_t pad11[2];
    trp_span report_invokeid;
    trp_span report_reason;
    uint8_t report_kind; /* trp_invoke_report_kind */
    uint8_t report_ends;
    uint8_t pad_end[2];
} trp_c2h_input;
_Static_assert(sizeof(trp_c2h_input) == 128, "c2h.input");

/* c2h.send: A send for an I/O processor. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    trp_span sendid;
    trp_span io_type;
    trp_span target;
    trp_span name;
    uint64_t data;
    uint64_t content;
} trp_c2h_send;
_Static_assert(sizeof(trp_c2h_send) == 88, "c2h.send");

/* c2h.invoke: Start an invocation. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    trp_span invokeid;
    trp_span type;
    trp_span src;
    uint64_t data;
    uint64_t content;
    uint8_t autoforward;
    uint8_t pad_end[7];
} trp_c2h_invoke;
_Static_assert(sizeof(trp_c2h_invoke) == 88, "c2h.invoke");

/* c2h.invoke_cancel: Cancel an invocation. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    trp_span invokeid;
    uint64_t invocation_seq;
} trp_c2h_invoke_cancel;
_Static_assert(sizeof(trp_c2h_invoke_cancel) == 56, "c2h.invoke_cancel");

/* c2h.invoke_forward: Autoforward an event to an invocation. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    trp_span invokeid;
    uint64_t invocation_seq;
    trp_span name;
    trp_span sendid;
    trp_span origin;
    trp_span origin_type;
    trp_span event_invokeid;
    uint64_t data;
    uint8_t type; /* trp_event_type */
    uint8_t pad_end[7];
} trp_c2h_invoke_forward;
_Static_assert(sizeof(trp_c2h_invoke_forward) == 112, "c2h.invoke_forward");

/* c2h.log: A <log>. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    trp_span label;
    uint64_t value;
} trp_c2h_log;
_Static_assert(sizeof(trp_c2h_log) == 56, "c2h.log");

/* c2h.macrostep_ended: A macrostep ended. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    uint8_t source; /* trp_input_source */
    uint8_t pad2[7];
    uint64_t source_seq;
    uint32_t effects;
    uint32_t mail;
    uint8_t status; /* trp_macrostep_status */
    uint8_t pad_end[7];
} trp_c2h_macrostep_ended;
_Static_assert(sizeof(trp_c2h_macrostep_ended) == 72, "c2h.macrostep_ended");

/* c2h.wake: Call core back at this time. */
typedef struct {
    trp_header header;
    uint64_t due;
} trp_c2h_wake;
_Static_assert(sizeof(trp_c2h_wake) == 40, "c2h.wake");

/* c2h.bounced: An h2c record was not run. */
typedef struct {
    trp_header header;
    uint64_t source_seq;
    trp_span detail;
    uint8_t reason; /* trp_bounce_reason */
    uint8_t pad_end[7];
} trp_c2h_bounced;
_Static_assert(sizeof(trp_c2h_bounced) == 56, "c2h.bounced");

/* c2h.load_needed: Envelopes wait for an unloaded address. */
typedef struct {
    trp_header header;
    trp_span address;
    uint32_t waiting;
    uint8_t pad_end[4];
} trp_c2h_load_needed;
_Static_assert(sizeof(trp_c2h_load_needed) == 48, "c2h.load_needed");

/* c2h.loaded: A session is resident again. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t cause_seq;
} trp_c2h_loaded;
_Static_assert(sizeof(trp_c2h_loaded) == 48, "c2h.loaded");

/* c2h.unloaded: A session left memory. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t cause_seq;
    uint64_t image;
    uint64_t image_bytes;
    uint64_t wake_at;
} trp_c2h_unloaded;
_Static_assert(sizeof(trp_c2h_unloaded) == 72, "c2h.unloaded");

/* c2h.state: A recorded session's state at a point of its log. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t cause_seq;
    uint64_t image;
    uint64_t image_bytes;
    uint64_t input_seq;
} trp_c2h_state;
_Static_assert(sizeof(trp_c2h_state) == 72, "c2h.state");

/* c2h.published: A replayed session is live. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t cause_seq;
    uint64_t replayed;
    uint64_t dropped_timers;
    uint64_t retired_invokes;
} trp_c2h_published;
_Static_assert(sizeof(trp_c2h_published) == 72, "c2h.published");

/* c2h.finished: A session reached a top-level final state. */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    uint64_t donedata;
} trp_c2h_finished;
_Static_assert(sizeof(trp_c2h_finished) == 48, "c2h.finished");

/* c2h.destroyed: A session is gone. */
typedef struct {
    trp_header header;
    trp_span address;
    uint64_t cause_seq;
    uint8_t reason; /* trp_destroy_reason */
    uint8_t pad_end[7];
} trp_c2h_destroyed;
_Static_assert(sizeof(trp_c2h_destroyed) == 56, "c2h.destroyed");

/* c2h.definition_result: A definition request completed. */
typedef struct {
    trp_header header;
    uint64_t cause_seq;
    uint64_t definition;
    uint32_t diagnostic_bytes;
    uint8_t status; /* trp_definition_status */
    uint8_t pad_end[3];
} trp_c2h_definition_result;
_Static_assert(sizeof(trp_c2h_definition_result) == 56, "c2h.definition_result");

/* c2h.input_refused: An admitted host input was not taken. */
typedef struct {
    trp_header header;
    uint64_t source_seq;
    uint8_t reason; /* trp_input_refusal_reason */
    uint8_t pad_end[7];
} trp_c2h_input_refused;
_Static_assert(sizeof(trp_c2h_input_refused) == 48, "c2h.input_refused");

/* c2h.input_completed: A host input and its startup follow-up settled. */
typedef struct {
    trp_header header;
    uint64_t source_seq;
    uint64_t macrostep;
    uint8_t status; /* trp_input_completion_status */
    uint8_t pad_end[7];
} trp_c2h_input_completed;
_Static_assert(sizeof(trp_c2h_input_completed) == 56, "c2h.input_completed");

/* c2h.load_result: A load-failure response finished recycling waiting inputs. */
typedef struct {
    trp_header header;
    uint64_t cause_seq;
    uint64_t request_seq;
} trp_c2h_load_result;
_Static_assert(sizeof(trp_c2h_load_result) == 48, "c2h.load_result");

/* tel.trace: One trace record (core ADR 0067). */
typedef struct {
    trp_header header;
    uint64_t macrostep;
    uint64_t trace_id_high;
    uint64_t trace_id_low;
    uint64_t cause_session;
    uint64_t cause_macrostep;
    uint32_t microstep;
    uint32_t state;
    uint32_t transition;
    uint32_t action;
    uint32_t iteration;
    uint16_t trace_kind;
    uint8_t position;
    uint8_t result;
    trp_span name;
    trp_span value_text;
    trp_span states;
} trp_tel_trace;
_Static_assert(sizeof(trp_tel_trace) == 120, "tel.trace");

/* tel.dropped: Telemetry records were dropped. */
typedef struct {
    trp_header header;
    uint64_t count;
    uint64_t first_seq;
} trp_tel_dropped;
_Static_assert(sizeof(trp_tel_dropped) == 48, "tel.dropped");
