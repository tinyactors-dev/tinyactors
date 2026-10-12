/* tinyactors/otlp.h — trace records and stats to OTLP protobuf, and back (docs/adr/0006
 * in the repository root, section 8).
 *
 * A codec, like tinyactors/basic_http.h: the host (the "telemetry" of its
 * config, tinyactors/host.h) drains its systems' trace records (ta_system_trace)
 * and feeds them to the encoder, which turns
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
 *
 * It also writes ExportMetricsServiceRequests (`/v1/metrics`, docs/adr/0007):
 * a system's and a host's stats (tinyactors/stats.h) as metrics, and
 * metrics a host or a binding describes itself.
 */
#ifndef TINYACTORS_OTLP_H
#define TINYACTORS_OTLP_H

#include <tinyactors/allocator.h>
#include <tinyactors/datamodel.h>
#include <tinyactors/image.h>
#include <tinyactors/stats.h>
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
    TA_OTLP_NO_LOGS = 1u << 2,      /* write no log records */
    TA_OTLP_SPAN_METRICS =
        1u << 3 /* keep macrostep durations per chart (ta_otlp_add_span_metrics) */
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
    uint32_t render_bytes;   /* the longest value (its CBOR, as the datamodel's to_wire gives it)
                                rendered at all; a longer one is only marked cut (16 KiB) */
    uint32_t resource_bytes; /* the resource attributes, encoded (4 KiB) */
    uint32_t metric_bytes;   /* metrics, encoded (64 KiB) */
    uint32_t span_metric_charts; /* charts whose macrostep durations it keeps (64) */
} ta_otlp_config;

typedef struct ta_otlp ta_otlp;

/* The bytes ta_otlp_init needs for `config`. */
TA_API ta_status ta_otlp_layout(const ta_otlp_config *config, size_t *bytes);
TA_API ta_status ta_otlp_init(ta_memory memory, const ta_otlp_config *config, ta_otlp **out);
/* Move into another block with other capacities (at least what is in use):
 * what is open and staged moves with it, and *otlp is the encoder in its new
 * block. TA_ERROR_INVALID if it would not fit. The old block is dead
 * afterwards. */
TA_API ta_status ta_otlp_relocate(ta_otlp **otlp, ta_memory memory, const ta_otlp_config *config);

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
 * datamodel bindings render values with TA_OTLP_VALUES: their to_wire, as
 * CBOR diagnostic notation (ta_cbor_diagnostic). */
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

/* The ExportTraceServiceRequest of the spans that ended since the last put,
 * written into `out` (ta_buffer; length 0 when none did); the spans are
 * forgotten only on TA_OK, so TA_ERROR_NO_MEMORY asks for room and nothing
 * changes. The same for log records. */
TA_API ta_status ta_otlp_put_traces(ta_otlp *otlp, ta_buffer *out);
TA_API ta_status ta_otlp_put_logs(ta_otlp *otlp, ta_buffer *out);

/* A log record from the caller, staged beside the records ta_otlp_read
 * writes (a host's definition logs, docs/architecture.md). Whole or not at
 * all: TA_ERROR_NO_MEMORY when the staging (log_bytes) has no room (put the
 * logs and add again). With TA_OTLP_NO_LOGS it is TA_OK and nothing. */
enum { TA_OTLP_SEVERITY_DEBUG = 5, TA_OTLP_SEVERITY_INFO = 9, TA_OTLP_SEVERITY_WARN = 13 };
typedef struct ta_otlp_integer_attribute {
    ta_string_view key;
    int64_t value;
} ta_otlp_integer_attribute;
typedef struct ta_otlp_log {
    uint64_t time_ns;  /* since the Unix epoch */
    uint32_t severity; /* TA_OTLP_SEVERITY_* */
    uint32_t text_count, integer_count;
    ta_string_view event_name, body;
    const ta_otlp_attribute *texts; /* attributes with text values */
    const ta_otlp_integer_attribute *integers;
} ta_otlp_log;
TA_API ta_status ta_otlp_add_log(ta_otlp *otlp, const ta_otlp_log *log);

/* ---- metrics (docs/adr/0007 in the repository root) ---------------------------
 * One ExportMetricsServiceRequest (`/v1/metrics`) gathers the metrics added
 * since the last put, under one scope (`tinyactors`) and the resource of
 * ta_otlp_set_resource. Sums are cumulative and monotonic; times are
 * nanoseconds since the Unix epoch. An add is whole or not at all:
 * TA_ERROR_NO_MEMORY when the staging (metric_bytes) has no room (put the
 * request, or relocate into a bigger block, and add again). */
enum { TA_OTLP_METRIC_SUM = 1, TA_OTLP_METRIC_GAUGE = 2, TA_OTLP_METRIC_HISTOGRAM = 3 };
enum { TA_OTLP_POINT_DOUBLE = 1u << 0 }; /* a sum's or gauge's value is `number`, not `integer` */

/* A link from a point to a trace: the span a value was measured in. */
typedef struct ta_otlp_exemplar {
    ta_trace_id trace;
    ta_span_id span;
    uint64_t time; /* ns */
    double value;
} ta_otlp_exemplar;

typedef struct ta_otlp_point {
    const ta_otlp_attribute *attributes;
    uint32_t attribute_count;
    uint32_t flags; /* TA_OTLP_POINT_* */
    int64_t integer;
    double number;
    /* histograms: `bucket_count` counts, the last above the `bucket_count -
     * 1` inclusive upper bounds; min and max are written when count > 0 */
    uint64_t count;
    double sum, min, max;
    const double *bounds;
    const uint64_t *buckets;
    uint32_t bucket_count, exemplar_count;
    const ta_otlp_exemplar *exemplars;
} ta_otlp_point;

typedef struct ta_otlp_metric {
    ta_string_view name, unit, description;
    uint32_t kind; /* TA_OTLP_METRIC_* */
    uint32_t point_count;
    const ta_otlp_point *points;
    uint64_t start_ns, time_ns; /* the stream's start (sums, histograms), and now */
} ta_otlp_metric;

TA_API ta_status ta_otlp_add_metric(ta_otlp *otlp, const ta_otlp_metric *metric);

/* What a system or a host counted (tinyactors/stats.h), as the metrics of
 * docs/adr/0007: per chart (`tinyactors.chart.name`) its actors, spawns,
 * ends, macrosteps, fuel, errors and faults; per system the pools, memory,
 * the run queue, overdue timers, delivered events, lost trace records and
 * traces started and sampled, `systems` summed; per host the messages
 * routed and actors migrated. */
typedef struct ta_otlp_stats_input {
    uint32_t size;
    uint32_t chart_count;
    const ta_chart_stats *charts;
    const ta_system_stats *systems;
    uint32_t system_count, pad;
    const ta_host_stats *host; /* or NULL */
    uint64_t ticks_per_second; /* the systems' clock (overdue timers) */
    uint64_t start_ns, time_ns;
} ta_otlp_stats_input;
TA_API ta_status ta_otlp_add_stats(ta_otlp *otlp, const ta_otlp_stats_input *input);

/* With TA_OTLP_SPAN_METRICS, the encoder keeps, per (system, chart), a
 * histogram of the durations of the macrostep spans it writes, in seconds,
 * each bucket's exemplar the latest span that fell in it. This adds them to
 * its metrics as `tinyactors.macrostep.duration`, cumulative since
 * `start_ns`, a point per chart (`tinyactors.chart.name`). They are the
 * recorded macrosteps': under sampling, a distribution, not a count. */
TA_API ta_status ta_otlp_add_span_metrics(ta_otlp *otlp, uint64_t start_ns, uint64_t time_ns);

/* The ExportMetricsServiceRequest of the metrics added since the last put,
 * as for traces. */
TA_API ta_status ta_otlp_put_metrics(ta_otlp *otlp, ta_buffer *out);

typedef struct ta_otlp_stats {
    uint32_t size;
    uint32_t open, open_capacity;     /* sessions with an open macrostep */
    uint32_t chunks, chunks_capacity; /* chunks in use */
    uint32_t spans;                   /* spans staged */
    uint32_t staging_bytes, staging_capacity;
    uint32_t log_records, log_bytes, log_capacity;
    uint32_t metrics, metric_bytes, metric_capacity; /* metrics staged */
    uint32_t span_metric_charts, span_metric_capacity;
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
    uint32_t kind; /* a metric's points: the metric's TA_OTLP_METRIC_* */
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
TA_API int32_t ta_otlp_next_span(ta_bytes request, ta_otlp_cursor *cursor, ta_otlp_span *out);

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

typedef struct ta_otlp_key_value {
    ta_string_view key;
    ta_otlp_value value;
} ta_otlp_key_value;
/* The next attribute, from *at (start at 0): 1, 0 at the end, or
 * TA_ERROR_INVALID. */
TA_API int32_t ta_otlp_next_attribute(ta_otlp_fields attributes, uint32_t *at,
                                      ta_otlp_key_value *out);
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

/* A metric of an ExportMetricsServiceRequest (or MetricsData). */
typedef struct ta_otlp_metric_view {
    ta_string_view name, unit, description;
    uint32_t kind;        /* TA_OTLP_METRIC_*, 0 for one this reader does not know */
    uint32_t temporality; /* 1 delta, 2 cumulative, 0 none (gauges) */
    uint32_t monotonic, pad;
    ta_otlp_fields points;   /* ta_otlp_next_point */
    ta_otlp_fields resource; /* its resource's attributes */
    ta_string_view scope_name, scope_version;
} ta_otlp_metric_view;
TA_API int32_t ta_otlp_next_metric(ta_bytes request, ta_otlp_cursor *cursor,
                                   ta_otlp_metric_view *out);

typedef struct ta_otlp_point_view {
    uint64_t start, time; /* ns */
    uint32_t flags;       /* TA_OTLP_POINT_DOUBLE when the value is `number` */
    uint32_t has_min_max;
    int64_t integer;
    double number;
    uint64_t count; /* histograms */
    double sum, min, max;
    ta_string_view buckets, bounds;       /* packed little-endian fixed64 counts and doubles */
    ta_otlp_fields attributes, exemplars; /* ta_otlp_next_attribute / _exemplar */
} ta_otlp_point_view;
/* The next point of a metric (points.kind is the metric's), from *at (start at 0). */
TA_API int32_t ta_otlp_next_point(ta_otlp_fields points, uint32_t *at, ta_otlp_point_view *out);
TA_API int32_t ta_otlp_next_exemplar(ta_otlp_fields exemplars, uint32_t *at, ta_otlp_exemplar *out);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_OTLP_H */
