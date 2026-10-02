/* Generated from catalog.rb by render.rb. Do not edit. A sketch of the
 * layout; the real header would be part of tinyactors.h. */
#pragma once
#include <stdint.h>

typedef struct { uint32_t offset; uint32_t length; } trp_span;

typedef struct {
    uint16_t kind;
    uint16_t flags;
    uint32_t size;
    uint64_t seq;
    uint64_t session;
    uint64_t at;
} trp_header;
_Static_assert(sizeof(trp_header) == 32, "header");

#define TRP_FLAG_REPLAYING (1u << 8)
#define TRP_FLAG_RECORDED (1u << 9)

#define TRP_EVENT_TYPE_PLATFORM 0u
#define TRP_EVENT_TYPE_INTERNAL 1u
#define TRP_EVENT_TYPE_EXTERNAL 2u
#define TRP_INPUT_SOURCE_H2C 0u
#define TRP_INPUT_SOURCE_MAIL 1u
#define TRP_INPUT_SOURCE_TIMER 2u
#define TRP_INPUT_SOURCE_PLATFORM 3u
#define TRP_INPUT_SOURCE_INVOKE_REPORT 4u
#define TRP_MACROSTEP_STATUS_STABLE 0u
#define TRP_MACROSTEP_STATUS_DONE 1u
#define TRP_MACROSTEP_STATUS_RUNAWAY 2u
#define TRP_MACROSTEP_STATUS_ABANDONED 3u
#define TRP_INPUT_COMPLETION_STATUS_STABLE 0u
#define TRP_INPUT_COMPLETION_STATUS_DONE 1u
#define TRP_INPUT_COMPLETION_STATUS_RUNAWAY 2u
#define TRP_INPUT_COMPLETION_STATUS_ABANDONED 3u
#define TRP_INPUT_REFUSAL_REASON_LOAD_FAILED 1u
#define TRP_INPUT_REFUSAL_REASON_DESTROYED 2u
#define TRP_INPUT_REFUSAL_REASON_SNAPSHOT_FAILED 3u
#define TRP_INPUT_REFUSAL_REASON_STALE_INVOCATION 4u
#define TRP_INPUT_REFUSAL_REASON_INVALID 5u
#define TRP_BOUNCE_REASON_NO_SUCH_ADDRESS 1u
#define TRP_BOUNCE_REASON_NO_SUCH_DEFINITION 2u
#define TRP_BOUNCE_REASON_ADDRESS_TAKEN 3u
#define TRP_BOUNCE_REASON_HAS_MAIL 4u
#define TRP_BOUNCE_REASON_NOT_REPLAYING 5u
#define TRP_BOUNCE_REASON_INVALID 6u
#define TRP_BOUNCE_REASON_POOL 7u
#define TRP_BOUNCE_REASON_NAMES 8u
#define TRP_BOUNCE_REASON_ALLOCATION 9u
#define TRP_BOUNCE_REASON_ACTIVE_LOCAL_INVOKE 10u
#define TRP_BOUNCE_REASON_PENDING_INPUTS 11u
#define TRP_BOUNCE_REASON_STALE_REQUEST 12u
#define TRP_BOUNCE_REASON_RECORD 13u
#define TRP_CAPACITY_RESOURCE_POOL 1u
#define TRP_CAPACITY_RESOURCE_NAMES 2u
#define TRP_CAPACITY_RESOURCE_C2H 3u
#define TRP_CAPACITY_RESOURCE_INTERNAL 4u
#define TRP_CAPACITY_RESOURCE_RECORD 5u
#define TRP_INVOKE_REPORT_KIND_DONE 0u
#define TRP_INVOKE_REPORT_KIND_ERROR 1u
#define TRP_DESTROY_REASON_FINISHED 0u
#define TRP_DESTROY_REASON_OPERATOR 1u
#define TRP_DESTROY_REASON_PARENT_EXITED 2u
#define TRP_DEFINITION_STATUS_OK 0u
#define TRP_DEFINITION_STATUS_COMPILE_ERROR 1u
#define TRP_DEFINITION_STATUS_CAPACITY 2u
#define TRP_DEFINITION_STATUS_MISMATCH 3u
#define TRP_DEFINITION_STATUS_NOT_FOUND 4u
#define TRP_DEFINITION_STATUS_INVALID 5u

#define TRP_H2C_TIME 0x0101u
#define TRP_H2C_EVENT 0x0102u
#define TRP_H2C_SPAWN 0x0103u
#define TRP_H2C_SPAWN_REPLAYING_FROM_CLOCK (1u << 0)
#define TRP_H2C_PUBLISH 0x0104u
#define TRP_H2C_LOAD 0x0105u
#define TRP_H2C_LOAD_FAILED 0x0106u
#define TRP_H2C_UNLOAD 0x0107u
#define TRP_H2C_CHECKPOINT 0x0108u
#define TRP_H2C_INVOKE_REPORT 0x0109u
#define TRP_H2C_DESTROY 0x010au
#define TRP_H2C_DEFINITION_REGISTER 0x010bu
#define TRP_H2C_DEFINITION_RELEASE 0x010cu
#define TRP_H2C_DEFINITION_RETIRE 0x010du
#define TRP_C2H_SPAWNED 0x0201u
#define TRP_C2H_INPUT 0x0202u
#define TRP_C2H_SEND 0x0203u
#define TRP_C2H_INVOKE 0x0204u
#define TRP_C2H_INVOKE_CANCEL 0x0205u
#define TRP_C2H_INVOKE_FORWARD 0x0206u
#define TRP_C2H_LOG 0x0207u
#define TRP_C2H_MACROSTEP_ENDED 0x0208u
#define TRP_C2H_WAKE 0x0209u
#define TRP_C2H_WAKE_NONE (1u << 0)
#define TRP_C2H_BOUNCED 0x020au
#define TRP_C2H_LOAD_NEEDED 0x020bu
#define TRP_C2H_LOADED 0x020cu
#define TRP_C2H_UNLOADED 0x020du
#define TRP_C2H_UNLOADED_HAS_WAKE (1u << 0)
#define TRP_C2H_STATE 0x020eu
#define TRP_C2H_PUBLISHED 0x020fu
#define TRP_C2H_FINISHED 0x0210u
#define TRP_C2H_DESTROYED 0x0211u
#define TRP_C2H_DEFINITION_RESULT 0x0212u
#define TRP_C2H_INPUT_REFUSED 0x0213u
#define TRP_C2H_INPUT_COMPLETED 0x0214u
#define TRP_C2H_LOAD_RESULT 0x0215u
#define TRP_TEL_TRACE 0x0301u
#define TRP_TEL_TRACE_TRUNCATED (1u << 0)
#define TRP_TEL_TRACE_SUPPRESSED (1u << 1)
#define TRP_TEL_DROPPED 0x0302u

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
    uint8_t type;
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
    uint8_t kind;
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
    uint8_t source;
    uint8_t type;
    uint8_t pad11[2];
    trp_span report_invokeid;
    trp_span report_reason;
    uint8_t report_kind;
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
    uint8_t type;
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
    uint8_t source;
    uint8_t pad2[7];
    uint64_t source_seq;
    uint32_t effects;
    uint32_t mail;
    uint8_t status;
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
    uint8_t reason;
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
    uint8_t reason;
    uint8_t pad_end[7];
} trp_c2h_destroyed;
_Static_assert(sizeof(trp_c2h_destroyed) == 56, "c2h.destroyed");

/* c2h.definition_result: A definition request completed. */
typedef struct {
    trp_header header;
    uint64_t cause_seq;
    uint64_t definition;
    uint32_t diagnostic_bytes;
    uint8_t status;
    uint8_t pad_end[3];
} trp_c2h_definition_result;
_Static_assert(sizeof(trp_c2h_definition_result) == 56, "c2h.definition_result");

/* c2h.input_refused: An admitted host input was not taken. */
typedef struct {
    trp_header header;
    uint64_t source_seq;
    uint8_t reason;
    uint8_t pad_end[7];
} trp_c2h_input_refused;
_Static_assert(sizeof(trp_c2h_input_refused) == 48, "c2h.input_refused");

/* c2h.input_completed: A host input and its startup follow-up settled. */
typedef struct {
    trp_header header;
    uint64_t source_seq;
    uint64_t macrostep;
    uint8_t status;
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
