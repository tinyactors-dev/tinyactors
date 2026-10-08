/* tinyactors/otlp.h — trace records to OTLP protobuf, and back (docs/adr/0006
 * in the repository root, section 8).
 *
 * A codec, like tinyactors/basic_http.h: a host or a binding drains a system's
 * trace records (ta_system_trace) and feeds them to the encoder, which turns
 * them into spans and log records and writes one ExportTraceServiceRequest
 * (`/v1/traces`) and one ExportLogsServiceRequest (`/v1/logs`) into memory
 * the caller owns. It never runs inside ta_system_run, does no I/O and keeps
 * no clock: it maps a record's fuel stamp to wall time with the marks the
 * host gives it (ta_otlp_mark). Freestanding: one block of memory, sized at
 * creation (ta_otlp_layout, ta_otlp_init) and moved into a bigger one to grow
 * (ta_otlp_relocate); nothing is allocated after that.
 *
 * The mapping (granary's vocabulary, its ADRs 0042, 0062 and 0155):
 *   - MACROSTEP_BEGIN opens a span "scxml.macrostep <event>" ("bootstrap"),
 *     a child of its cause (or, with TA_OTLP_LINK_PARENTS, a root linked to
 *     it); MACROSTEP_END ends it with the configuration (`scxml.state`);
 *   - each microstep is a span "scxml.microstep <event>" ("(eventless)"),
 *     a child of its macrostep, with its transitions and the states it
 *     exited and entered;
 *   - conditions, raised and sent events, assignments, invocations, cancels
 *     and logs are span events of the step they happened in; a failed action
 *     marks its step as an error;
 *   - spawned, finished, destroyed, unloaded and loaded are spans of no
 *     duration;
 *   - every span of an actor has `tinyactors.actor.name`,
 *     `tinyactors.chart.name` and `tinyactors.span.kind`;
 *   - a DROPPED record ends the open spans as errors marked with the count
 *     (`tinyactors.telemetry.dropped`), and the rest of their macrosteps is
 *     skipped;
 *   - <log> and failed actions are also log records.
 * One request has one ScopeSpans per (system, chart): its scope names the
 * chart. Span IDs are the records' (ta_step_key_span_id), so an ID a host
 * computes for a step names the same span.
 */
#ifndef TINYACTORS_OTLP_H
#define TINYACTORS_OTLP_H

#include <tinyactors/datamodel.h>
#include <tinyactors/image.h>
#include <tinyactors/trace.h>
#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The staging of finished spans: its default and bounds, in bytes. */
#define TA_OTLP_STAGING_BYTES_DEFAULT 1048576u
#define TA_OTLP_STAGING_BYTES_MIN     65536u
#define TA_OTLP_STAGING_BYTES_MAX     1073741824u

enum {
    TA_OTLP_LINK_PARENTS = 1u << 0, /* a macrostep is a root, linked to its cause */
    TA_OTLP_VALUES = 1u << 1,       /* event data and assigned values in attributes */
    TA_OTLP_NO_LOGS = 1u << 2       /* write no log records */
};

/* Capacities; 0 is the default. */
typedef struct ta_otlp_config {
    uint32_t size;
    uint32_t flags;          /* TA_OTLP_* */
    uint32_t staging_bytes;  /* finished spans, encoded (TA_OTLP_STAGING_BYTES_DEFAULT) */
    uint32_t log_bytes;      /* log records, encoded (a quarter of staging_bytes) */
    uint32_t open_spans;     /* sessions with an open macrostep (1024) */
    uint32_t chunks;         /* 256-byte chunks for what open spans gathered (8 per open span) */
    uint32_t scopes;         /* charts one request names (64) */
    uint32_t value_limit;    /* bytes of a value in an attribute, cut there (256) */
    uint32_t render_bytes;   /* a value as the datamodel writes it, before the cut (16 KiB) */
    uint32_t resource_bytes; /* the resource attributes, encoded (4 KiB) */
} ta_otlp_config;

typedef struct ta_otlp ta_otlp;

/* The bytes ta_otlp_init needs for `config`. */
TA_API ta_status ta_otlp_layout(const ta_otlp_config *config, size_t *bytes);
TA_API ta_status ta_otlp_init(void *memory, size_t length, const ta_otlp_config *config,
                              ta_otlp **out);
/* Move into another block with other capacities (at least what is in use):
 * what is open and staged moves with it. TA_ERROR_INVALID if it would not
 * fit. The old block is dead afterwards. */
TA_API ta_status ta_otlp_relocate(ta_otlp *otlp, void *memory, size_t length,
                                  const ta_otlp_config *config, ta_otlp **out);

/* A resource attribute (`service.name`, ...): strings. */
typedef struct ta_otlp_attribute {
    ta_string_view key, value;
} ta_otlp_attribute;
/* The attributes every request's resource carries. TA_ERROR_NO_MEMORY if
 * they do not fit resource_bytes (the old ones stay). */
TA_API ta_status ta_otlp_set_resource(ta_otlp *otlp, const ta_otlp_attribute *attributes,
                                      uint32_t count);
/* Change the flags (TA_OTLP_*) while running: open spans are kept. */
TA_API void ta_otlp_set_flags(ta_otlp *otlp, uint32_t flags);

/* A time mark: the system's fuel clock (ta_system_stats.fuel_clock, or a
 * host's ta_host_stats.fuel_clock) was `fuel` at wall time `wall_ns`
 * (nanoseconds since the Unix epoch). Mark before and after every run, and
 * read the run's records after the second mark: a record's time is
 * interpolated between the marks around its fuel stamp. Marks go forward. */
TA_API void ta_otlp_mark(ta_otlp *otlp, uint64_t fuel, uint64_t wall_ns);

/* Records of one system to read. `source` names the system (a host's
 * segment index): with `image`, its records' charts and strings. The
 * datamodel bindings render values (their to_wire) with TA_OTLP_VALUES. */
typedef struct ta_otlp_input {
    uint32_t size;
    uint32_t source;
    ta_trace_view view;
    const ta_image *image;
    const ta_datamodel_binding *datamodels;
    uint32_t datamodel_count;
    uint32_t pad;
} ta_otlp_input;

enum {
    TA_OTLP_READ = 0,  /* every record was read */
    TA_OTLP_FLUSH = 1, /* the staging is full: put the requests, then read on from *cursor */
    TA_OTLP_GROW = 2   /* open spans, chunks or scopes are full: relocate into a bigger block
                          (the staging being empty does not help), then read on */
};
/* Read the records of `input->view` from *cursor (start at 0), advancing it
 * past each record read. A record is read whole or not at all. Returns
 * TA_OTLP_*; a malformed record ends the reading (TA_OTLP_READ, *cursor at
 * it). */
TA_API uint32_t ta_otlp_read(ta_otlp *otlp, const ta_otlp_input *input, uint32_t *cursor);

/* End every open span now, at `wall_ns`, marked `scxml.incomplete`: the
 * encoder or the system is going away. They go into the next request. */
TA_API void ta_otlp_end_all(ta_otlp *otlp, uint64_t wall_ns);

/* The size of the ExportTraceServiceRequest of the spans that ended since
 * the last put (0 when none did), then the request itself, written into
 * `out` of exactly that size; the spans are forgotten. A size that is not
 * the current one writes nothing (0). The same for log records. */
TA_API size_t ta_otlp_traces_size(const ta_otlp *otlp);
TA_API size_t ta_otlp_put_traces(ta_otlp *otlp, void *out, size_t size);
TA_API size_t ta_otlp_logs_size(const ta_otlp *otlp);
TA_API size_t ta_otlp_put_logs(ta_otlp *otlp, void *out, size_t size);

typedef struct ta_otlp_stats {
    uint32_t size;
    uint32_t open, open_capacity;     /* sessions with an open macrostep */
    uint32_t chunks, chunks_capacity; /* chunks in use */
    uint32_t spans;                   /* spans staged */
    uint32_t staging_bytes, staging_capacity;
    uint32_t log_records, log_bytes, log_capacity;
    uint32_t pad;
    uint64_t records;   /* records read */
    uint64_t dropped;   /* records the systems reported lost */
    uint64_t skipped;   /* records of macrosteps a drop ended */
    uint64_t truncated; /* attributes that did not fit and were left out */
} ta_otlp_stats;
TA_API void ta_otlp_stats_get(const ta_otlp *otlp, ta_otlp_stats *out);

/* ---- decoding: OTLP protobuf traces to plain views ------------------------
 * For tests, for the bindings' decodeTraces, and for applications that read
 * their own spans. Every view points into the bytes given. */
enum {
    TA_OTLP_SPAN_UNSPECIFIED = 0,
    TA_OTLP_SPAN_INTERNAL = 1,
    TA_OTLP_SPAN_SERVER = 2,
    TA_OTLP_SPAN_CLIENT = 3,
    TA_OTLP_SPAN_PRODUCER = 4,
    TA_OTLP_SPAN_CONSUMER = 5
};
enum { TA_OTLP_STATUS_UNSET = 0, TA_OTLP_STATUS_OK = 1, TA_OTLP_STATUS_ERROR = 2 };

/* Repeated fields of an encoded message: the attributes of a span (9), an
 * event (3), a link (4), a resource (1) or a scope (3); a span's events (11)
 * or links (13); an array's values (1). */
typedef struct ta_otlp_fields {
    ta_string_view message;
    uint32_t field;
    uint32_t pad;
} ta_otlp_fields;

typedef struct ta_otlp_span {
    ta_trace_id trace;
    ta_span_id span, parent; /* parent 0: none */
    uint64_t start, end;     /* nanoseconds since the Unix epoch */
    uint32_t kind;           /* TA_OTLP_SPAN_* */
    uint32_t status;         /* TA_OTLP_STATUS_* */
    uint32_t flags;          /* Span.flags */
    uint32_t pad;
    ta_string_view name, status_message;
    ta_otlp_fields attributes, events, links; /* ta_otlp_next_attribute / _event / _link */
    ta_otlp_fields resource;                  /* its resource's attributes */
    ta_string_view scope_name, scope_version;
    ta_otlp_fields scope_attributes;
} ta_otlp_span;

/* Where a walk through a request is; zero it to start. */
typedef struct ta_otlp_cursor {
    uint64_t at[6];
} ta_otlp_cursor;

/* The next span of an ExportTraceServiceRequest (or TracesData): 1, or 0 at
 * the end, or TA_ERROR_INVALID when the bytes are malformed. */
TA_API int32_t ta_otlp_next_span(const void *bytes, size_t length, ta_otlp_cursor *cursor,
                                 ta_otlp_span *out);

enum {
    TA_OTLP_VALUE_EMPTY = 0,
    TA_OTLP_VALUE_STRING = 1,
    TA_OTLP_VALUE_BOOL = 2,
    TA_OTLP_VALUE_INT = 3,
    TA_OTLP_VALUE_DOUBLE = 4,
    TA_OTLP_VALUE_ARRAY = 5, /* `items`: its values (ta_otlp_next_value) */
    TA_OTLP_VALUE_LIST = 6,  /* a key-value list; `items`: ta_otlp_next_attribute */
    TA_OTLP_VALUE_BYTES = 7
};
typedef struct ta_otlp_value {
    uint32_t kind; /* TA_OTLP_VALUE_* */
    uint32_t pad;
    int64_t integer;      /* INT; BOOL: 0 or 1 */
    double number;        /* DOUBLE */
    ta_string_view bytes; /* STRING, BYTES */
    ta_otlp_fields items; /* ARRAY, LIST */
} ta_otlp_value;

/* The next attribute, from *at (start at 0): 1, 0 at the end, or
 * TA_ERROR_INVALID. */
TA_API int32_t ta_otlp_next_attribute(ta_otlp_fields attributes, uint32_t *at, ta_string_view *key,
                                      ta_otlp_value *value);
/* The next value of an array. */
TA_API int32_t ta_otlp_next_value(ta_otlp_fields array, uint32_t *at, ta_otlp_value *value);

typedef struct ta_otlp_event {
    uint64_t time;
    ta_string_view name;
    ta_otlp_fields attributes;
} ta_otlp_event;
TA_API int32_t ta_otlp_next_event(ta_otlp_fields events, uint32_t *at, ta_otlp_event *out);

typedef struct ta_otlp_link {
    ta_trace_id trace;
    ta_span_id span;
    ta_otlp_fields attributes;
} ta_otlp_link;
TA_API int32_t ta_otlp_next_link(ta_otlp_fields links, uint32_t *at, ta_otlp_link *out);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_OTLP_H */
