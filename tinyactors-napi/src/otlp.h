#pragma once
/*
 * Trace records to OTLP protobuf (napi ADR 0016).
 *
 * The encoder is a core trace sink's back end: it takes the records core
 * writes (core ADR 0067), maps them to OpenTelemetry spans, keeps the
 * spans still open between turns, and writes the spans that ended as one
 * `ExportTraceServiceRequest` (opentelemetry-proto, trace/v1), which an
 * OTLP/HTTP `/v1/traces` endpoint accepts as it is. It exports nothing:
 * where the bytes go is the host's business.
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
 * The attribute table is in napi ADR 0016's implementation notes.
 *
 * Memory: every buffer keeps its capacity, so once the busiest turn has been
 * seen, encoding allocates nothing. Writing cannot fail: a record that does
 * not fit because an allocation failed is counted in `dropped`.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

/* What the encoder asks of its host. */
typedef struct {
    void *self;
    /* Wall-clock time, in nanoseconds since the Unix epoch (core has only
     * logical time). */
    uint64_t (*now)(void *self);
    /* A new trace ID, for a span whose record carries none. 16 bytes. */
    void (*trace_id)(void *self, uint8_t out[16]);
    /* Optional: text for a value (event data, an assigned value), valid until
     * the next call; false when there is none. Used only with `values`. */
    bool (*value_text)(void *self, const ta_value *value, ta_str *out);
} tna_otlp_host;

typedef struct {
    /* The system whose records these are: names come from its definitions
     * (core ADR 0071) and its interned event names. */
    const ta_system *core;
    ta_allocator allocator;
    tna_otlp_host host;
    /* Parent a macrostep under its cause (false), or make it a root and
     * link it to its cause (true). */
    bool link_parents;
    /* Put values (event data, assigned values) in attributes. */
    bool values;
    /* The instrumentation scope's version. */
    ta_str version;
    /* The kinds the user asked for (TA_TRACE_BIT). The sink also asks core
     * for UNLOADED and DESTROYED, which end open spans, and writes their
     * spans only when they are wanted. */
    uint32_t wanted_kinds;
    uint32_t wanted_watched_kinds; /* the same, for watched contexts */
} tna_otlp_config;

/* A growable byte buffer over an allocator. */
typedef struct {
    ta_allocator allocator;
    uint8_t *data;
    size_t len;
    size_t cap;
} tna_otlp_buf;

typedef struct {
    uint8_t trace_id[16];
    uint64_t span_id;
    uint64_t parent_span_id; /* 0: none */
    uint8_t link_trace_id[16];
    uint64_t link_span_id;   /* 0: no link */
    uint64_t start;
    uint32_t microstep;      /* for a microstep span */
    bool error;
    tna_otlp_buf name;
    tna_otlp_buf attributes; /* encoded Span.attributes entries */
    tna_otlp_buf events;     /* encoded Span.events entries */
} tna_otlp_span;

/* A session's open macrostep, and its open microstep. */
typedef struct {
    uint64_t session;
    bool has_micro;
    tna_otlp_span macro;
    tna_otlp_span micro;
} tna_otlp_open;

typedef struct {
    uint64_t definition;
    uint32_t tag; /* what is named: kind in the top two bits, the ID below */
    uint32_t len;
    size_t at;    /* in `names` */
    bool used;
} tna_otlp_name;

typedef struct {
    tna_otlp_config config;
    tna_otlp_open *open;       /* the first `open_count` are open; the rest are spare */
    uint32_t open_count;
    uint32_t open_capacity;
    uint32_t *open_index;      /* open addressing by session: index + 1 into `open`, 0 empty */
    uint32_t open_index_capacity; /* a power of two, at least twice `open_capacity` */
    uint64_t last_now;         /* the clock at the last span boundary: span events use it */
    tna_otlp_span lifecycle;   /* a lifecycle span while it is built */
    tna_otlp_name *name_slots; /* open addressing, a power of two */
    uint32_t name_capacity;
    uint32_t name_count;
    tna_otlp_buf names;        /* the cached labels' text */
    tna_otlp_buf resource;     /* encoded Resource.attributes entries */
    tna_otlp_buf spans;        /* encoded ScopeSpans.spans entries that ended */
    tna_otlp_buf request;      /* the last request tna_otlp_take() built */
    tna_otlp_buf scratch;      /* an event's attributes while it is built */
    tna_otlp_buf label;        /* a label while it is built */
    tna_otlp_buf text;         /* an event name or ID while it is formatted */
    uint32_t span_count;       /* spans in `spans` */
    uint64_t dropped;          /* records lost to a failed allocation */
} tna_otlp;

void tna_otlp_init(tna_otlp *otlp, const tna_otlp_config *config);

/* One resource attribute, a string. */
typedef struct {
    ta_str key;
    ta_str value;
} tna_otlp_attribute;

/* Sets the resource attributes (`service.name`, ...) every request carries. */
ta_result tna_otlp_set_resource(tna_otlp *otlp, const tna_otlp_attribute *attributes, uint32_t count);

/* Takes one record, as a trace sink's `write` does. Cannot fail. */
void tna_otlp_write(tna_otlp *otlp, const ta_trace_record *record);

/*
 * The spans that ended since the last take, as one encoded
 * ExportTraceServiceRequest, and forgets them. Empty (len 0) when none
 * ended. The bytes stay valid until the next write or take.
 */
ta_str tna_otlp_take(tna_otlp *otlp);

/* Ends every open span now, marked `scxml.incomplete`, so the next take
 * carries them: the sink is going away. */
void tna_otlp_end_all(tna_otlp *otlp);

/* Changes the kinds the user asked for (configureTraces). */
static inline void tna_otlp_set_wanted(tna_otlp *otlp, uint32_t kinds, uint32_t watched_kinds) {
    otlp->config.wanted_kinds = kinds;
    otlp->config.wanted_watched_kinds = watched_kinds;
}

/* Spans still open (for tests). */
static inline uint32_t tna_otlp_open_count(const tna_otlp *otlp) { return otlp->open_count; }

void tna_otlp_destroy(tna_otlp *otlp);
