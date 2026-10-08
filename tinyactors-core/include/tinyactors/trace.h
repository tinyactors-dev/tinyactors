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
#define TA_TRACEPARENT_LENGTH   55u
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

/* ---- the record schema ------------------------------------------------------------------
 *
 * What core and the system write into a system's trace journal, and what a
 * reader (the otlp codec, a binding, a C host) reads back with
 * ta_trace_next. A record is a header and a body; its size is a multiple of
 * 8, and records follow each other without gaps.
 *
 * Names are not copied into records. States, transitions, charts and static
 * event names are indices into the system's image; strings are references:
 * an image string id, or, with TA_TRACE_STRING_DYNAMIC set, a dynamic string
 * in the system's string storage (ta_trace_dynamic_string); TA_NIL is none.
 * Payloads are datamodel handles, with the index of the datamodel binding
 * that owns them. Both stay valid while their record is unacknowledged.
 */
enum {
    TA_TRACE_DROPPED = 1,   /* the records lost since the last one (ta_trace_dropped) */
    TA_TRACE_SPAWNED = 2,   /* ta_trace_spawned */
    TA_TRACE_FINISHED = 3,  /* reached a top-level final state (ta_trace_finished) */
    TA_TRACE_DESTROYED = 4, /* ta_trace_lifecycle */
    TA_TRACE_UNLOADED = 5,  /* hibernated (ta_trace_lifecycle) */
    TA_TRACE_LOADED = 6,    /* woken (ta_trace_lifecycle) */
    TA_TRACE_MACROSTEP_BEGIN = 7,
    TA_TRACE_MACROSTEP_END = 8,
    TA_TRACE_MICROSTEP = 9,
    TA_TRACE_CONDITION = 10,
    TA_TRACE_RAISED = 11,
    TA_TRACE_SENT = 12,
    TA_TRACE_INVOKED = 13,
    TA_TRACE_CANCELLED = 14,
    TA_TRACE_ASSIGN = 15,
    TA_TRACE_LOG = 16,
    TA_TRACE_ACTION_FAILED = 17,
    TA_TRACE_KIND_COUNT = 18
};

/* Masks of kinds (a system's trace_kinds). TA_TRACE_DROPPED is always
 * written. TA_TRACE_VALUES adds values: a macrostep's event data, and the
 * value an assignment assigned (captured as a payload). */
#define TA_TRACE_BIT(kind) (UINT32_C(1) << (kind))
#define TA_TRACE_VALUES    (UINT32_C(1) << 31)
#define TA_TRACE_LIFECYCLE                                                                         \
    (TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_FINISHED) |                            \
     TA_TRACE_BIT(TA_TRACE_DESTROYED) | TA_TRACE_BIT(TA_TRACE_UNLOADED) |                          \
     TA_TRACE_BIT(TA_TRACE_LOADED))
/* A span per macrostep, lifecycle spans and failures. */
#define TA_TRACE_SUMMARY                                                                           \
    (TA_TRACE_LIFECYCLE | TA_TRACE_BIT(TA_TRACE_MACROSTEP_BEGIN) |                                 \
     TA_TRACE_BIT(TA_TRACE_MACROSTEP_END) | TA_TRACE_BIT(TA_TRACE_ACTION_FAILED))
/* And microsteps, conditions and raised events. */
#define TA_TRACE_DECISIONS                                                                         \
    (TA_TRACE_SUMMARY | TA_TRACE_BIT(TA_TRACE_MICROSTEP) | TA_TRACE_BIT(TA_TRACE_CONDITION) |      \
     TA_TRACE_BIT(TA_TRACE_RAISED))
/* Every kind. Values (event data, assigned values) are asked for apart, with
 * TA_TRACE_VALUES: they can hold personal data, and a record keeps its value
 * alive until it is read. */
#define TA_TRACE_FULL                                                                              \
    (TA_TRACE_DECISIONS | TA_TRACE_BIT(TA_TRACE_SENT) | TA_TRACE_BIT(TA_TRACE_INVOKED) |           \
     TA_TRACE_BIT(TA_TRACE_CANCELLED) | TA_TRACE_BIT(TA_TRACE_ASSIGN) |                            \
     TA_TRACE_BIT(TA_TRACE_LOG))

#define TA_TRACE_STRING_DYNAMIC 0x80000000u

/* Lifecycle records are outside every macrostep; their step keys are fixed
 * (session, 0, TA_TRACE_STEP_*), or (session, macrostep, TA_TRACE_STEP_*)
 * for hibernation, so each has a span ID of its own. */
#define TA_TRACE_STEP_SPAWNED   1u
#define TA_TRACE_STEP_FINISHED  2u
#define TA_TRACE_STEP_DESTROYED 3u
#define TA_TRACE_STEP_UNLOADED  0xFFFFFFFEu
#define TA_TRACE_STEP_LOADED    0xFFFFFFFFu

/* The header: 88 bytes. `span` is ta_step_key_span_id(key); `parent` the
 * span's parent: the cause of a macrostep (0 for a root), the macrostep of a
 * microstep and of what happened in it. `fuel` is the system's fuel clock
 * when the record was written (docs/adr/0006, section 9) and `now` its
 * logical time. */
typedef struct ta_trace_record {
    uint16_t kind;  /* TA_TRACE_* */
    uint16_t flags; /* the trace's W3C flags */
    uint32_t size;  /* the whole record, header included */
    uint32_t actor; /* the actor's handle in its system */
    uint32_t chart; /* its chart in the image */
    ta_step_key key;
    ta_trace_id trace;
    ta_span_id span;
    ta_span_id parent;
    uint64_t fuel;
    uint64_t now;
} ta_trace_record;

typedef struct ta_trace_dropped {
    uint64_t count;
} ta_trace_dropped;

typedef struct ta_trace_spawned {
    uint32_t name;   /* the actor's name (a string) */
    uint32_t parent; /* the invoking actor's handle, 0 for a root */
} ta_trace_spawned;

typedef struct ta_trace_finished {
    uint32_t final_state; /* the top-level <final>, TA_NIL if none */
    uint32_t name;        /* the actor's name (a string) */
} ta_trace_finished;

/* destroyed, unloaded, loaded */
typedef struct ta_trace_lifecycle {
    uint32_t name; /* the actor's name (a string) */
    uint32_t pad;
} ta_trace_lifecycle;

/* The event a macrostep took; for the bootstrap `name` is TA_NIL. */
typedef struct ta_trace_macrostep_begin {
    uint32_t name;           /* a string */
    uint32_t name_id;        /* its event-name node in the image */
    uint32_t type;           /* TA_EVENT_TYPE_* */
    uint32_t sendid;         /* a string, TA_NIL if none */
    uint32_t origin;         /* an external origin (a string), TA_NIL if none */
    uint32_t datamodel;      /* the binding owning `payload`, TA_NIL if none */
    uint64_t origin_session; /* the sending session, 0 if none */
    ta_step_key cause;       /* the step that sent it; session 0: a host's (all 0: none) */
    uint64_t payload;        /* the event's data with TA_TRACE_VALUES, else 0 */
    uint32_t actor_name;     /* the actor's name (a string) */
    uint32_t pad;
} ta_trace_macrostep_begin;

/* The configuration after a macrostep: `state_count` active states in
 * document order (the root left out) follow. */
typedef struct ta_trace_macrostep_end {
    uint32_t state_count;
    uint32_t pad;
} ta_trace_macrostep_end;

/* A microstep: its transitions (chart-local indices), the states it exited
 * in exit order and entered in entry order follow, in that order. */
typedef struct ta_trace_microstep {
    uint64_t begin_fuel; /* the fuel clock when it began */
    uint32_t transition_count, exited_count, entered_count;
    uint32_t name; /* the event its transitions took (a string), TA_NIL: eventless */
} ta_trace_microstep;

typedef struct ta_trace_condition {
    uint32_t transition; /* chart-local */
    uint32_t result;     /* 1 true, 0 false or failed */
} ta_trace_condition;

typedef struct ta_trace_raised {
    uint32_t name; /* a string */
    uint32_t name_id;
} ta_trace_raised;

typedef struct ta_trace_sent {
    uint32_t target; /* a string, TA_NIL for the sender itself */
    uint32_t name;   /* a string */
    uint32_t name_id;
    uint32_t sendid;          /* a string, TA_NIL if none */
    uint64_t delay;           /* nanoseconds */
    ta_trace_context context; /* the new event's; no trace if untraced */
} ta_trace_sent;

typedef struct ta_trace_invoked {
    uint32_t type;     /* a string, TA_NIL for SCXML */
    uint32_t source;   /* a string, TA_NIL if none */
    uint32_t invokeid; /* a string */
    uint32_t child;    /* the child's handle, 0 if the host runs it */
    uint64_t child_session;
} ta_trace_invoked;

typedef struct ta_trace_cancelled {
    uint32_t sendid; /* a string */
    uint32_t pad;
} ta_trace_cancelled;

typedef struct ta_trace_assign {
    uint32_t location;  /* the location expression (chart-local) */
    uint32_t datamodel; /* the binding owning `payload`, TA_NIL if none */
    uint64_t payload;   /* the value assigned, 0 without TA_TRACE_VALUES */
} ta_trace_assign;

typedef struct ta_trace_log {
    uint32_t label; /* a string, TA_NIL if none */
    uint32_t text;  /* a string, TA_NIL if none */
} ta_trace_log;

typedef struct ta_trace_action_failed {
    uint32_t opcode;     /* the executable content that failed (image opcode) */
    uint32_t expression; /* its expression, TA_NIL if none */
} ta_trace_action_failed;

/* A batch of records to read, as a system hands it out (ta_system_trace):
 * the records, and the storage their dynamic strings are in. */
typedef struct ta_trace_view {
    const unsigned char *records;
    uint32_t length;
    uint32_t pad;
    const unsigned char *strings;
} ta_trace_view;

/* The size of a kind's body, without the lists that follow it; 0 for an
 * unknown kind. */
TA_API uint32_t ta_trace_body_size(uint32_t kind);
/* The record at *cursor (start at 0), advancing it past the record; NULL at
 * the end, or when the record is malformed (unknown kind, a size too small
 * for its kind and lists, not a multiple of 8, or past the end). */
TA_API const ta_trace_record *ta_trace_next(const ta_trace_view *view, uint32_t *cursor);
/* The body after a record's header. */
TA_API const void *ta_trace_body(const ta_trace_record *record);
/* The u32 lists after the body of a MACROSTEP_END (the states) or a
 * MICROSTEP (transitions, then exited, then entered). */
TA_API const uint32_t *ta_trace_list(const ta_trace_record *record);
/* A dynamic string (TA_TRACE_STRING_DYNAMIC set) of a view; an empty view for
 * anything else (resolve image strings with ta_image_string). Its bytes are
 * at `strings + (reference & ~TA_TRACE_STRING_DYNAMIC) * 8`: a u32 reference
 * count, a u32 length, then the bytes and a NUL. */
TA_API ta_string_view ta_trace_dynamic_string(const ta_trace_view *view, uint32_t reference);

#ifdef __cplusplus
}
#endif

#endif /* TINYACTORS_TRACE_H */
