/* tinyactors/trace.h — trace values (docs/adr/0006 in the repository root).
 *
 * The one implementation of the values every layer and binding shares: trace
 * and span IDs, step keys and the span IDs derived from them, W3C trace
 * contexts and their `traceparent` text, the seeded trace ID generator, and
 * ratio sampling. Freestanding (ta_trace): nothing here allocates or keeps
 * state of its own; the generator's state is the caller's.
 *
 * A step key names one step of one session: macrostep `macrostep` of session
 * `session`, and microstep `microstep` within it, where 0 is the macrostep
 * itself. Macrosteps count from 1 per session (the bootstrap is 1). A key
 * with session 0 is a host's cause: `macrostep` holds the host's span ID.
 */
#ifndef TINYACTORS_TRACE_H
#define TINYACTORS_TRACE_H

#include <tinyactors/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A W3C trace ID: `high` then `low`, each big-endian in text and on the
 * wire. All zero is none. */
typedef struct ta_trace_id {
    uint64_t high, low;
} ta_trace_id;

/* A span ID; 0 is none. */
typedef uint64_t ta_span_id;

typedef struct ta_step_key {
    uint64_t session, macrostep;
    uint32_t microstep, pad;
} ta_step_key;

/* What a step inherits from what caused it: the trace, the span of the
 * cause (the parent of the step's span), and the W3C trace flags. */
typedef struct ta_trace_context {
    ta_trace_id trace;
    ta_span_id parent;
    uint32_t flags; /* TA_TRACE_FLAG_* */
    uint32_t pad;
} ta_trace_context;

/* W3C trace-flags. RANDOM is Trace Context Level 2's: the trace ID's
 * rightmost 56 bits are random. */
enum { TA_TRACE_FLAG_SAMPLED = 0x01u, TA_TRACE_FLAG_RANDOM = 0x02u };

/* "00-" 32 hex "-" 16 hex "-" 2 hex */
#define TA_TRACEPARENT_LENGTH 55u
#define TA_TRACE_ID_TEXT_LENGTH 32u
#define TA_SPAN_ID_TEXT_LENGTH  16u

/* The span ID of a step: a fixed 64-bit mix of the key, the same in every
 * host, never 0. A host's cause (session 0) returns `macrostep`, its own
 * span ID, unchanged. */
TA_API ta_span_id ta_step_key_span_id(ta_step_key key);

/* The context of a step's children: the same trace and flags, with the
 * step's span as the parent. */
TA_API ta_trace_context ta_trace_context_child(const ta_trace_context *context, ta_step_key key);

/* Parse W3C `traceparent` text: version-traceid-parentid-flags in lowercase
 * hex. Spaces and tabs around it are ignored. Version ff, an all-zero trace
 * ID or parent ID, version 00 with anything after the flags, and anything
 * malformed are TA_ERROR_INVALID; a later version is read by its first four
 * fields when a dash follows the flags. `out->parent` is the caller's span. */
TA_API ta_status ta_traceparent_parse(ta_string_view text, ta_trace_context *out);
/* Write version 00, the trace ID, `parent` and the flags' low byte
 * (TA_TRACEPARENT_LENGTH bytes, no terminator). Returns the length, or 0 if
 * the context has no trace or no parent, or capacity is too small. */
TA_API uint32_t ta_traceparent_format(const ta_trace_context *context, char *buffer,
                                      uint32_t capacity);
/* Lowercase hex, no terminator: 32 digits for a trace ID, 16 for a span ID.
 * Return the length, or 0 if capacity is too small. */
TA_API uint32_t ta_trace_id_format(ta_trace_id trace, char *buffer, uint32_t capacity);
TA_API uint32_t ta_span_id_format(ta_span_id span, char *buffer, uint32_t capacity);

/* The seeded trace ID generator: xoshiro256** whose state is seeded through
 * splitmix64, so a seed gives the same IDs on every host. The state is the
 * caller's to keep (a system keeps it in its region). */
typedef struct ta_trace_generator {
    uint64_t state[4];
} ta_trace_generator;
TA_API void ta_trace_generator_seed(ta_trace_generator *generator, uint64_t seed);
/* The next trace ID, never all zero. */
TA_API ta_trace_id ta_trace_generator_next(ta_trace_generator *generator);

/* OpenTelemetry's TraceIDRatioBased, as consistent probability sampling:
 * the randomness is the trace ID's rightmost 56 bits, the threshold
 * round((1 - parts / whole) * 2^56) in integers, and a trace is sampled when
 * its randomness is at least the threshold. `parts` above `whole` counts as
 * `whole`; `whole` 0 or `parts` 0 samples nothing. */
TA_API uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole);
TA_API int ta_trace_sampled(ta_trace_id trace, uint64_t threshold);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_TRACE_H */
