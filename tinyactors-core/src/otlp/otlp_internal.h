/* otlp_internal.h — the encoder's block (tinyactors/otlp.h) and the protobuf
 * primitives the encoder and the decoder share. Everything in the block is
 * found by offsets from its header, so it moves with memcpy. */
#ifndef TINYACTORS_OTLP_OTLP_INTERNAL_H
#define TINYACTORS_OTLP_OTLP_INTERNAL_H

#include <tinyactors/otlp.h>

#include "base/config.h"

/* Field numbers of opentelemetry-proto v1 (common, resource, trace, logs and
 * the collector's requests). Stable since OTLP 1.0. */
enum {
    F_REQUEST_RESOURCE = 1, /* ExportTraceServiceRequest.resource_spans, ...Logs.resource_logs */
    F_RS_RESOURCE = 1,      /* ResourceSpans / ResourceLogs */
    F_RS_SCOPES = 2,
    F_RESOURCE_ATTRIBUTES = 1,
    F_SS_SCOPE = 1, /* ScopeSpans / ScopeLogs */
    F_SS_ITEMS = 2,
    F_SCOPE_NAME = 1, /* InstrumentationScope */
    F_SCOPE_VERSION = 2,
    F_SCOPE_ATTRIBUTES = 3,
    F_SPAN_TRACE_ID = 1, /* Span */
    F_SPAN_SPAN_ID = 2,
    F_SPAN_TRACE_STATE = 3,
    F_SPAN_PARENT_SPAN_ID = 4,
    F_SPAN_NAME = 5,
    F_SPAN_KIND = 6,
    F_SPAN_START = 7,
    F_SPAN_END = 8,
    F_SPAN_ATTRIBUTES = 9,
    F_SPAN_EVENTS = 11,
    F_SPAN_LINKS = 13,
    F_SPAN_STATUS = 15,
    F_SPAN_FLAGS = 16,
    F_EVENT_TIME = 1, /* Span.Event */
    F_EVENT_NAME = 2,
    F_EVENT_ATTRIBUTES = 3,
    F_LINK_TRACE_ID = 1, /* Span.Link */
    F_LINK_SPAN_ID = 2,
    F_LINK_ATTRIBUTES = 4,
    F_STATUS_MESSAGE = 2, /* Status */
    F_STATUS_CODE = 3,
    F_KEY_VALUE_KEY = 1, /* KeyValue */
    F_KEY_VALUE_VALUE = 2,
    F_ANY_STRING = 1, /* AnyValue */
    F_ANY_BOOL = 2,
    F_ANY_INT = 3,
    F_ANY_DOUBLE = 4,
    F_ANY_ARRAY = 5,
    F_ANY_LIST = 6,
    F_ANY_BYTES = 7,
    F_ARRAY_VALUES = 1, /* ArrayValue */
    F_LOG_TIME = 1,     /* LogRecord */
    F_LOG_SEVERITY_NUMBER = 2,
    F_LOG_SEVERITY_TEXT = 3,
    F_LOG_BODY = 5,
    F_LOG_ATTRIBUTES = 6,
    F_LOG_FLAGS = 8,
    F_LOG_TRACE_ID = 9,
    F_LOG_SPAN_ID = 10,
    F_LOG_OBSERVED_TIME = 11,
    F_LOG_EVENT_NAME = 12
};
enum { WIRE_VARINT = 0, WIRE_I64 = 1, WIRE_LENGTH = 2, WIRE_I32 = 5 };

TA_INLINE uint32_t otlp_varint_size(uint64_t value) {
    uint32_t size = 1;
    while (value >= 0x80u) {
        value >>= 7;
        size++;
    }
    return size;
}

/* A length-delimited field of `length` bytes, its tag and length included. */
TA_INLINE uint32_t otlp_field_size(uint32_t field, uint32_t length) {
    return otlp_varint_size((uint64_t)field << 3) + otlp_varint_size(length) + length;
}

/* ---- the block ------------------------------------------------------------------------ */

#define OTLP_MAGIC         0x504C544Fu /* "OTLP" */
#define OTLP_CHUNK_PAYLOAD 248u
#define OTLP_MARKS         64u
#define OTLP_SCOPE_BYTES   512u

/* What gathers for a span while it is open: a chain of chunks. */
typedef struct otlp_chunk {
    uint32_t next; /* chunk index, TA_NIL: the last */
    uint32_t used;
    unsigned char data[OTLP_CHUNK_PAYLOAD];
} otlp_chunk;

typedef struct otlp_chain {
    uint32_t first, last; /* chunk indices, TA_NIL: empty */
    uint32_t bytes;
    uint32_t pad;
} otlp_chain;

typedef struct otlp_span {
    ta_trace_id trace;
    uint64_t span_id, parent_id, link_id; /* link_id: a link to the cause instead of a parent */
    uint64_t start;                       /* wall ns */
    otlp_chain fields;                    /* encoded Span fields: name, attributes, events */
    uint32_t flags;                       /* W3C */
    uint32_t error;
    uint32_t microstep;
    uint32_t pad;
} otlp_span;

/* A session with an open macrostep (and maybe a microstep). */
typedef struct otlp_open {
    uint64_t session, macrostep;
    uint32_t used, source, chart, has_micro;
    otlp_chain common; /* the attributes every span of the session has */
    otlp_span macro, micro;
} otlp_open;

typedef struct otlp_skip { /* a macrostep a drop ended: its later records are skipped */
    uint64_t session, macrostep;
} otlp_skip;

typedef struct otlp_entry { /* a finished span in the staging */
    uint32_t at, length, next;
    uint32_t pad;
} otlp_entry;

typedef struct otlp_scope { /* the spans of one (system, chart) in a request */
    uint32_t source, chart;
    uint32_t first, last;            /* entry indices + 1, 0: none */
    uint32_t spans;                  /* bytes of its ScopeSpans.spans fields */
    uint32_t bytes_at, bytes_length; /* its encoded InstrumentationScope */
    uint32_t pad;
} otlp_scope;

struct ta_otlp {
    uint32_t magic, pad;
    uint64_t block_bytes;
    ta_otlp_config config;
    uint32_t entry_capacity, index_capacity;
    int64_t open_relative, index_relative, skips_relative, chunks_relative, spans_relative,
        entries_relative, scopes_relative, scope_bytes_relative, logs_relative, resource_relative,
        scratch_relative, render_relative;
    uint32_t scratch_bytes, open_count, skip_count, chunk_free, chunk_high, chunks_used;
    uint32_t spans_used, entry_count, scope_count, scope_bytes_used, logs_used, log_count;
    uint32_t resource_used, span_count, mark_head, mark_count;
    uint64_t mark_fuel[OTLP_MARKS], mark_wall[OTLP_MARKS];
    uint64_t records, dropped, skipped, truncated;
};

#endif /* TINYACTORS_OTLP_OTLP_INTERNAL_H */
