#ifndef TINYACTORS_OTLP_H
#define TINYACTORS_OTLP_H

/**
 * @file
 * tinyactors-otlp: core's trace records to OTLP protobuf (napi ADR 0016).
 *
 * The encoder reads the records core writes to its `tel` ring (core ADRs
 * 0067 and 0098) through core's public API, tao_read_tel() between core
 * calls, maps them to OpenTelemetry spans, keeps the spans still open
 * between turns, and writes the spans that ended as one
 * `ExportTraceServiceRequest` (opentelemetry-proto, trace/v1), which an
 * OTLP/HTTP `/v1/traces` endpoint accepts as it is. Log records go the
 * same way, as one `ExportLogsServiceRequest` for `/v1/logs`. It exports
 * nothing: where the bytes go is the host's business.
 *
 * A record names everything it refers to, so the encoder never calls into
 * core for a trace record: names, values and the fault are the record's
 * text, and times are the `wall_ns` core stamped it with, or its logical
 * time without a clock. The facts that name a definition's scope come only
 * on the records that open a run; the encoder keeps them per definition
 * until tao_forget_definition(). When core reports records it dropped, the
 * open spans end (core ADR 0098 section 9).
 *
 * Span IDs are core's: a step's span ID is ta_step_key_span_id() of its
 * step key, and a span's parent or link is that of its cause, so a span ID
 * a host computes for a step (an exemplar, a traceparent) names the same
 * span.
 *
 * Portability: it uses only core's public header and the host interface
 * below, links no C library (only the compiler's memory builtins may stay
 * undefined), and has no language's API in it, so every binding links it.
 *
 * The mapping, lossless over the records core writes:
 * - MACROSTEP_BEGIN opens a span, MACROSTEP_END ends it;
 * - MICROSTEP opens a child span, ended by the next MICROSTEP or the END;
 * - every other record inside a macrostep is a span event on the step it
 *   happened in;
 * - a lifecycle record is a span of no duration.
 * The attribute table is in napi ADR 0016's implementation notes. A
 * request holds one ScopeSpans per definition, whose scope attributes name
 * it by family, revision, ID, name and source (napi ADR 0028).
 *
 * Memory: every buffer keeps its capacity, so once the busiest turn has been
 * seen, encoding allocates nothing. Reading cannot fail: a record that does
 * not fit because an allocation failed is counted as dropped.
 *
 * Staging: the span staging is the encoder's own setting, `staging_bytes`
 * (napi's traceStagingBytes parameter, napi ADRs 0035 and 0036), reserved
 * whole at tao_create(). A host ends a turn early, or hands the requests
 * on, once tao_staging_full() says so.
 *
 * Output (napi ADR 0018): the encoder never owns a request. A host asks for
 * a request's size, which is free (the spans and log records keep a running
 * total), allocates exactly that many bytes in memory its own language owns,
 * and has the encoder write the request into them in one pass.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

/* The span staging's bounds and default, in bytes. */
#define TAO_STAGING_BYTES_DEFAULT 1048576u
#define TAO_STAGING_BYTES_MIN 65536u
#define TAO_STAGING_BYTES_MAX 1073741824u

/* What the encoder asks of its host. */
typedef struct {
    void *self;
    /* Wall-clock time, in nanoseconds since the Unix epoch, for what is not a
     * trace record and so carries no time of its own: <log> records, host
     * logs, definition logs, and the spans tao_end_all() ends. */
    uint64_t (*now)(void *self);
    /* A new trace ID, for a span whose record carries none. 16 bytes. */
    void (*trace_id)(void *self, uint8_t out[16]);
    /* Optional: text for a <log> record's value, valid until the next call;
     * false when there is none. A trace record's values are text already
     * (core ADR 0098). `*cut`: the text was cut at the host's limit. */
    bool (*value_text)(void *self, const ta_value *value, ta_str *out, bool *cut);
} tao_host;

typedef struct {
    /* The system whose logs these are: a <log> record, a host log and a
     * definition log name what they refer to by ID, and the encoder asks
     * core for the names. Trace records never need it. */
    const ta_system *core;
    ta_allocator allocator;
    tao_host host;
    /* Parent a macrostep under its cause (false), or make it a root and
     * link it to its cause (true). */
    bool link_parents;
    /* Put values (event data, assigned values) in attributes, as core
     * rendered them (telemetryValueBytes). */
    bool values;
    /* The instrumentation scope's version. */
    ta_str version;
    /* The kinds the user asked for (TA_TRACE_BIT). The sink also asks core
     * for UNLOADED and DESTROYED, which end open spans, and writes their
     * spans only when they are wanted. */
    uint32_t wanted_kinds;
    uint32_t wanted_watched_kinds; /* the same, for watched contexts */
    uint32_t wanted_unsampled_kinds; /* the same, for unsampled ones */
    /* Logs (napi ADR 0017 section 7): failures and <log> become OTLP log
     * records, at most `log_limit` per definition and name per second;
     * 0 writes no logs. */
    uint32_t log_limit;
    /* The span staging, reserved whole at creation and shrunk back to it
     * after a burst; tao_staging_full() compares the staged requests with
     * it. 0 for TAO_STAGING_BYTES_DEFAULT. */
    size_t staging_bytes;
} tao_config;

typedef struct tao_encoder tao_encoder;

/* Creates an encoder from `config`'s allocator, and reserves its span
 * staging whole (napi ADR 0036 section 2). TA_ERR_NOMEM when the allocator
 * refuses either; nothing is then kept. */
ta_result tao_create(const tao_config *config, tao_encoder **out);
/* Frees the encoder and everything it holds; NULL does nothing. */
void tao_destroy(tao_encoder *otlp);

/* One resource attribute, a string. */
typedef struct {
    ta_str key;
    ta_str value;
} tao_attribute;

/* Sets the resource attributes (`service.name`, ...) every request carries. */
ta_result tao_set_resource(tao_encoder *otlp, const tao_attribute *attributes, uint32_t count);

/*
 * Reads every record waiting on `core`'s tel ring (ta_system_tel_peek(),
 * ta_system_tel_release()): a `tel.trace` becomes spans and span events, a
 * `tel.dropped` ends every open span as an error marked with the count and
 * skips the rest of its macrostep (core ADR 0098 section 9). Call it
 * between core calls, never from inside one. Returns how many spans ended
 * while reading, so a host knows whether a request is waiting.
 */
uint32_t tao_read_tel(tao_encoder *otlp, ta_system *core);

/*
 * The size in bytes of the ExportTraceServiceRequest the spans that ended
 * since the last put make; 0 when none ended. It walks nothing.
 */
size_t tao_traces_size(const tao_encoder *otlp);

/*
 * Writes that request into `out`, which holds `size` bytes, and forgets the
 * spans. `size` must be what tao_traces_size() reports now: otherwise
 * nothing is written or forgotten and the result is 0. Returns the bytes
 * written, which is `size`: the write never goes past it.
 */
size_t tao_put_traces(tao_encoder *otlp, uint8_t *out, size_t size);

/* The same pair for the log records written since the last put, as one
 * ExportLogsServiceRequest for `/v1/logs`. */
size_t tao_logs_size(const tao_encoder *otlp);
size_t tao_put_logs(tao_encoder *otlp, uint8_t *out, size_t size);

/* Whether the staged requests (tao_traces_size() plus tao_logs_size())
 * reach the staging bound: a host then ends its turn, or hands them on. */
bool tao_staging_full(const tao_encoder *otlp);

/* A <log> as an INFO log record; its value's text comes from the host. */
void tao_log(tao_encoder *otlp, const ta_log_record *record);

/*
 * A WARN log the host writes about an actor or a message, not a record core
 * wrote: a quarantine or a dead letter (napi ADR 0017 section 7). It goes
 * through the same rate limit, per definition and event name. The body is
 * "<event name> <body>" when `has_event`, else `body`.
 */
typedef struct {
    ta_str event_name;          /* LogRecord.event_name */
    ta_str body;
    bool has_event;
    ta_str event;               /* scxml.event.name, when has_event; borrowed for the call */
    ta_definition_id definition; /* 0 when unknown */
    uint64_t session;            /* 0 when unknown */
    ta_str key;                  /* one more attribute: the reason or code */
    ta_str value;
} tao_host_log_params;
void tao_host_log(tao_encoder *otlp, const tao_host_log_params *log);

/*
 * A definition lifecycle event as a log (core ADR 0079): DEBUG for
 * registered and freed, INFO for retired, with the family, revision,
 * origin ("host", "nested" or "invoke") and ID. Rate-limited per family
 * and event name, not per definition, so a family that churns through
 * invoke-time definitions stays within the limit.
 */
typedef struct {
    const ta_definition_event *event;
    ta_str origin;
} tao_definition_log_params;
void tao_definition_log(tao_encoder *otlp, const tao_definition_log_params *log);

/* Drops the names cached for a definition core has freed (core ADR 0079). */
void tao_forget_definition(tao_encoder *otlp, ta_definition_id definition);

/*
 * At the end of every pump turn, whether or not it wrote a request: a
 * staging buffer larger than the staging bound and more than four times a
 * typical turn's use (as it stood before this turn) is freed. A burst, such
 * as a turn of spawns each tracing its bootstrap, is far above typical, so
 * its buffer goes as soon as the turn ends instead of staying for the
 * system's life. Steady turns, and the idle turns between them, keep their
 * buffer and allocate nothing; a new steady load regrows it a few times
 * while the typical use catches up.
 */
void tao_end_turn(tao_encoder *otlp);
/* Ends every open span now, marked `scxml.incomplete`, so the next put
 * carries them: the sink is going away. */
void tao_end_all(tao_encoder *otlp);

/* Changes the kinds the user asked for (configureTraces). */
void tao_set_wanted(tao_encoder *otlp, const ta_trace_masks *masks);
/* Spans and log records staged since the last tao_end_turn(). */
uint32_t tao_staged_records(const tao_encoder *otlp);
/* Whether the encoder keeps log records (its config's log_limit is not 0). */
bool tao_logs_enabled(const tao_encoder *otlp);
/* Whether the encoder puts values in attributes (its config's `values`):
 * when it does not, a binding asks core for no value text, so core renders
 * none (ta_trace_masks.value_bytes 0, core ADR 0098). */
bool tao_values(const tao_encoder *otlp);

/* A macrostep span the encoder opened: its trace, span ID and start time. */
typedef struct {
    ta_trace_context context;
    uint64_t span_id;
    uint64_t time_ns;
} tao_span_ref;
/* The latest macrostep span opened since the last call, for a host's
 * metric exemplars; false when none was. */
bool tao_take_last_macrostep(tao_encoder *otlp, tao_span_ref *out);

#endif
