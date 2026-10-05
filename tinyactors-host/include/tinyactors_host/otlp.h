#pragma once
/*
 * Trace records to OTLP protobuf (napi ADR 0016).
 *
 * The encoder reads the records core writes to its `tel` ring (core ADRs
 * 0067 and 0098), as the host drains them between core calls, maps them to
 * OpenTelemetry spans, keeps the spans still open between turns, and writes
 * the spans that ended as one `ExportTraceServiceRequest`
 * (opentelemetry-proto, trace/v1), which an OTLP/HTTP `/v1/traces` endpoint
 * accepts as it is. It exports nothing: where the bytes go is the host's
 * business.
 *
 * A record names everything it refers to, so the encoder never calls into
 * core for a record: names, values and the fault are the record's text, and
 * times are the `wall_ns` core stamped it with, or its logical time without
 * a clock. The facts that name a definition's scope come only on the
 * records that open a run; the encoder keeps them per definition until
 * tah_otlp_forget_definition(). When core reports records it dropped,
 * tah_otlp_dropped() ends the open spans (core ADR 0098 section 9).
 *
 * Portability: it uses only core's public header and the host interface
 * below. It has no Node-API in it (the NODE_API_SOURCES check keeps it
 * so) and none of napi's own modules, so it can move to wherever a second
 * host needs it (the host kernel, or a small library of its own).
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
 * seen, encoding allocates nothing. Writing cannot fail: a record that does
 * not fit because an allocation failed is counted in `dropped`.
 *
 * Output (napi ADR 0018): the encoder never owns a request. A host asks for
 * a request's size, which is free (the spans and log records keep a running
 * total), allocates exactly that many bytes in memory its own language owns,
 * and has the encoder write the request into them in one pass.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "host_metrics.h"
#include "tinyactors.h"

/* What the encoder asks of its host. */
typedef struct {
    void *self;
    /* Wall-clock time, in nanoseconds since the Unix epoch, for what is not a
     * trace record and so carries no time of its own: <log> records, host
     * logs, definition logs, and the spans tah_otlp_end_all() ends. */
    uint64_t (*now)(void *self);
    /* A new trace ID, for a span whose record carries none. 16 bytes. */
    void (*trace_id)(void *self, uint8_t out[16]);
    /* Optional: text for a <log> record's value, valid until the next call;
     * false when there is none. A trace record's values are text already
     * (core ADR 0098). `*cut`: the text was cut at the host's limit. */
    bool (*value_text)(void *self, const ta_value *value, ta_str *out, bool *cut);
} tah_otlp_host;

typedef struct {
    /* The system whose logs these are: a <log> record, a host log and a
     * definition log name what they refer to by ID, and the encoder asks
     * core for the names. Trace records never need it. */
    const ta_system *core;
    ta_allocator allocator;
    tah_otlp_host host;
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
    /* The span staging, reserved whole when the sink is installed and
     * shrunk back to it after a burst: the traceStagingBytes parameter
     * (napi ADRs 0035 and 0036); 0 for 1 MiB. */
    size_t keep_bytes;
} tah_otlp_config;

typedef struct tah_otlp tah_otlp;

/* Sets up the encoder, and reserves the span staging whole: the
 * traceStagingBytes parameter (napi ADR 0036 section 2). False when the
 * allocator refuses it; nothing is then kept. */
bool tah_otlp_init(tah_otlp *otlp, const tah_otlp_config *config);

/* One resource attribute, a string. */
typedef struct {
    ta_str key;
    ta_str value;
} tah_otlp_attribute;

/* Sets the resource attributes (`service.name`, ...) every request carries. */
ta_result tah_otlp_set_resource(tah_otlp *otlp, const tah_otlp_attribute *attributes, uint32_t count);

/* Takes one record read from tel; borrows it for the call. Cannot fail: a
 * record the encoder has no memory for is counted in `dropped`. */
void tah_otlp_write(tah_otlp *otlp, const ta_trace_record *record);
/* Takes a report that core dropped `count` records on tel: every open span
 * ends, as an error marked with the count, and the rest of its macrostep is
 * skipped (core ADR 0098 section 9). */
void tah_otlp_dropped(tah_otlp *otlp, uint64_t count);

/*
 * The size in bytes of the ExportTraceServiceRequest the spans that ended
 * since the last put make; 0 when none ended. It walks nothing.
 */
size_t tah_otlp_traces_size(const tah_otlp *otlp);

/*
 * Writes that request into `out`, which holds `size` bytes, and forgets the
 * spans. `size` must be what tah_otlp_traces_size() reports now: otherwise
 * nothing is written or forgotten and the result is 0. Returns the bytes
 * written, which is `size`: the write never goes past it.
 */
size_t tah_otlp_put_traces(tah_otlp *otlp, uint8_t *out, size_t size);

/* A <log> as an INFO log record; its value's text comes from the host. */
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
    ta_event_name_id event;     /* scxml.event.name, when has_event */
    ta_definition_id definition; /* 0 when unknown */
    uint64_t session;            /* 0 when unknown */
    ta_str key;                  /* one more attribute: the reason or code */
    ta_str value;
} tah_otlp_host_log_params;
void tah_otlp_host_log(tah_otlp *otlp, const tah_otlp_host_log_params *log);

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
} tah_otlp_definition_log_params;
void tah_otlp_definition_log(tah_otlp *otlp, const tah_otlp_definition_log_params *log);
void tah_otlp_log(tah_otlp *otlp, const ta_log_record *record);

/* The same pair for the log records written since the last put, as one
 * ExportLogsServiceRequest for `/v1/logs`. */
size_t tah_otlp_logs_size(const tah_otlp *otlp);
size_t tah_otlp_put_logs(tah_otlp *otlp, uint8_t *out, size_t size);

/* Drops the names cached for a definition core has freed (core ADR 0079). */
void tah_otlp_forget_definition(tah_otlp *otlp, ta_definition_id definition);

/* Ends every open span now, marked `scxml.incomplete`, so the next put
 * carries them: the sink is going away. */
/*
 * At the end of every pump turn, whether or not it wrote a request: a
 * staging buffer larger than traceStagingBytes and more than four times a typical
 * turn's use (as it stood before this turn) is freed. A burst, such as a
 * turn of spawns each tracing its bootstrap, is far above typical, so its
 * buffer goes as soon as the turn ends instead of staying for the system's
 * life. Steady turns, and the idle turns between them, keep their buffer
 * and allocate nothing; a new steady load regrows it a few times while the
 * typical use catches up.
 */
void tah_otlp_end_turn(tah_otlp *otlp);
void tah_otlp_end_all(tah_otlp *otlp);

/* Changes the kinds the user asked for (configureTraces). */
void tah_otlp_set_wanted(tah_otlp *otlp, const ta_trace_masks *masks);
/* Spans and log records staged since the last tah_otlp_end_turn(). */
uint32_t tah_otlp_staged_records(const tah_otlp *otlp);
/* Whether the encoder keeps log records (its config's log_limit is not 0). */
bool tah_otlp_logs_enabled(const tah_otlp *otlp);
/* Whether the encoder puts values in attributes (its config's `values`):
 * when it does not, a binding asks core for no value text, so core renders
 * none (ta_trace_masks.value_bytes 0, core ADR 0098). */
bool tah_otlp_values(const tah_otlp *otlp);

void tah_otlp_destroy(tah_otlp *otlp);
