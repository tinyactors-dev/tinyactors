/*
 * Tinyactors public API.
 *
 * This file is the complete, authored public interface of Tinyactors: every
 * type, constant, and function an embedder or binding may use is declared
 * here, and nothing else is. Implementation modules under src/ include this
 * header and add private declarations; the generated amalgamation
 * (tinyactors.c) gives every private function internal linkage.
 *
 * Every public function is declared with TA_API. Define TA_API before
 * including this header to add export attributes, e.g.
 * __attribute__((visibility("default"))) or __declspec(dllexport).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifndef TA_API
#define TA_API
#endif

/*
 * API annotations.
 *
 * These macros record what C types cannot say about the API: which pointers
 * are single elements, lists, or caller buffers; which may be null; which
 * outputs are valid after which results and who releases them; how handles
 * are created and destroyed; and the stable symbols of enum constants.
 * `mise run api` reads them to write include/tinyactors.api.json (see
 * docs/internal/api-ir.md). They expand to nothing unless TA_API_EXTRACT is
 * defined, so they never change the compiled API.
 */
#ifdef TA_API_EXTRACT
#define TA_ANNOTATION __attribute__((annotate("tinyactors")))
#else
#define TA_ANNOTATION
#endif

/* Types. */
#define TA_HANDLE TA_ANNOTATION                      /* opaque object owned through a handle */
#define TA_VIEW TA_ANNOTATION                        /* opaque object borrowed from the core */
#define TA_STRING TA_ANNOTATION                      /* {ptr, len} text */
#define TA_BYTES TA_ANNOTATION                       /* {ptr, len} bytes */
#define TA_HOST_VALUE TA_ANNOTATION                  /* host value, see ta_value */
#define TA_ADAPTER TA_ANNOTATION                     /* {self, operations} implemented by the embedder */
#define TA_VTABLE TA_ANNOTATION                      /* the operations of an adapter */
#define TA_TAGGED(tag, union_) TA_ANNOTATION         /* field `tag` selects the member of `union_` */
#define TA_ZERO_IS_ABSENT TA_ANNOTATION              /* members equal to zero are absent */
#define TA_SYMBOLS(prefix) TA_ANNOTATION             /* enum symbols: names without `prefix`, lowercase */
#define TA_CONSTANTS_OF(type, prefix) TA_ANNOTATION  /* anonymous enum: the values of integer type `type` */
#define TA_FLAG_SET(name, prefix) TA_ANNOTATION      /* anonymous enum: a named set of bit flags */
#define TA_SUCCESS TA_ANNOTATION                     /* enum constant: the result that means success */

/* Members. */
#define TA_WHEN(constant) TA_ANNOTATION              /* union member selected by `constant` */
#define TA_FLAGS(set) TA_ANNOTATION                  /* integer holding flags of `set` */
#define TA_RELEASE_IF(function, flag) TA_ANNOTATION  /* owned value; release with `function` when `flag` */

/* Pointers: exactly one shape. */
#define TA_IN TA_ANNOTATION                          /* one element the receiver reads */
#define TA_OUT TA_ANNOTATION                         /* one element the callee writes */
#define TA_INOUT TA_ANNOTATION                       /* one element the callee reads and writes */
#define TA_TERMINATED_BY(field) TA_ANNOTATION        /* list ending at the first element whose `field` is zero */
#define TA_COUNTED_BY(field) TA_ANNOTATION           /* list of `field` elements */
#define TA_BUFFER(capacity, needed) TA_ANNOTATION    /* caller array of `capacity` elements; total in `needed` */
#define TA_VIEW_OUT TA_ANNOTATION                    /* the callee stores a pointer to a borrowed graph */
#define TA_ARENA TA_ANNOTATION                       /* caller arena that receives the outputs */

/* Pointer facts. */
#define TA_OPTIONAL TA_ANNOTATION                    /* may be null; null means absent */
#define TA_ALWAYS TA_ANNOTATION                      /* output valid after every result */
#define TA_VALID_ON(constant) TA_ANNOTATION          /* output valid only after result `constant` */
#define TA_RELEASE(function) TA_ANNOTATION           /* the receiver releases the output with `function` */
#define TA_LIFETIME(scope) TA_ANNOTATION             /* borrowed data stays valid for `scope` */
#define TA_DESTROYS TA_ANNOTATION                    /* the function destroys this handle */
#define TA_CONSUMED_ON_OK TA_ANNOTATION              /* the callee takes ownership of this handle on TA_OK */

/* Functions. */
#define TA_VALUE_MODEL(kind, path) TA_ANNOTATION     /* ta_value inputs belong to the data model of `path` */
#define TA_ANY_VALUE_MODEL TA_ANNOTATION             /* ta_value inputs may belong to any data model */

#ifdef __cplusplus
extern "C" {
#endif

/* ========================================================================
 * Scalar handles, results, and plain data
 */


typedef int32_t ta_result;
typedef uint32_t ta_atom;
typedef uint32_t ta_event_name_id;
typedef uint32_t ta_state_id;
typedef uint32_t ta_transition_id;
typedef uint32_t ta_expr_id;
typedef uint32_t ta_action_id;
typedef uint32_t ta_action_block_id;
typedef uint32_t ta_initial_transition_id;
typedef uint32_t ta_data_decl_id;
typedef uint32_t ta_initializer_id;
typedef uint32_t ta_content_id;
typedef uint32_t ta_param_id;
typedef uint32_t ta_done_data_id;
typedef uint32_t ta_invoke_decl_id;
/*
 * A resident actor: its table slot in the low 32 bits and the slot's
 * generation in the high 32 bits (ADR 0059). Zero is never a valid ID, and
 * no slot is ever issued generation 0, so a host may use generation 0 for
 * "no actor". When an actor goes away its slot's generation moves on, so its
 * ID goes stale (TA_ERR_NOT_FOUND) and never names the slot's next occupant.
 * Split and join IDs with ta_actor_id_slot(), ta_actor_id_generation() and
 * ta_actor_id_make() rather than with bit arithmetic.
 */
typedef uint64_t ta_actor_id;
/*
 * A registered definition: its table slot in the low 32 bits and the slot's
 * generation in the high 32 bits (ADR 0043). Zero is never a valid ID. A
 * freed definition's ID goes stale and never names another definition.
 */
typedef uint64_t ta_definition_id;
typedef uint64_t ta_invoke_id;
typedef uint64_t ta_send_id;
typedef uint64_t ta_timer_id;
/* Monotonic logical time in milliseconds, supplied by the embedder. */
typedef uint64_t ta_time;

#define TA_STATE_NONE ((ta_state_id)UINT32_MAX)
#define TA_INITIAL_TRANSITION_NONE ((ta_initial_transition_id)UINT32_MAX)
#define TA_INITIALIZER_NONE ((ta_initializer_id)UINT32_MAX)
#define TA_DONE_DATA_NONE ((ta_done_data_id)UINT32_MAX)
#define TA_INVOKE_ID_NONE ((ta_invoke_id)UINT64_MAX)
/* Not an expression: a rejection that is about no single expression. */
#define TA_EXPR_ID_NONE ((ta_expr_id)UINT32_MAX)

enum TA_CONSTANTS_OF(ta_result, "TA_") {
    TA_OK TA_SUCCESS = 0,
    TA_ERR_INVALID = -1,
    TA_ERR_NOMEM = -2,
    TA_ERR_NOT_FOUND = -3,
    TA_ERR_NOT_RECONSTRUCTIBLE = -4,
    TA_ERR_BUSY = -5,
    TA_ERR_UNSUPPORTED = -6,
    TA_ERR_CORRUPT = -7,
    TA_ERR_DEFINITION_MISMATCH = -8,
    TA_ERR_UNLOADED = -9,
    TA_ERR_STALE = -10,
    TA_ERR_DONE = -11
};

typedef struct { const char *ptr; uint32_t len; } ta_str TA_STRING;
typedef struct { TA_COUNTED_BY(count) const ta_atom *atoms; uint32_t count; } ta_atom_list;
typedef struct { const uint8_t *ptr; uint32_t len; } ta_bytes TA_BYTES;
typedef struct { ta_str name; } ta_definition_family;
typedef struct { ta_definition_family family; ta_str name; } ta_actor_address;
/*
 * A named address that core holds (ADR 0065): one record per distinct
 * (family, name) pair in a system, reference-counted. Two equal addresses in
 * one system are the same handle, so pointer equality is address equality.
 * Core interns a ta_actor_address a host supplies (spawn, find, unload,
 * load); every named reference core stores or hands out is a handle. A
 * handle core hands out is borrowed for the call; a host that keeps one
 * retains it. Release every reference before destroying the system.
 */
typedef struct ta_address ta_address TA_HANDLE;
typedef enum { TA_ACTOR_REF_RUNTIME = 0, TA_ACTOR_REF_NAMED = 1 } ta_actor_ref_kind TA_SYMBOLS("TA_ACTOR_REF_");
typedef struct {
    ta_actor_ref_kind kind;
    union {
        TA_WHEN(TA_ACTOR_REF_RUNTIME) ta_actor_id runtime;
        TA_WHEN(TA_ACTOR_REF_NAMED) ta_address *named;
    } as;
} ta_actor_ref TA_TAGGED(kind, as);

typedef struct ta_system ta_system TA_HANDLE;
typedef struct ta_definition ta_definition TA_HANDLE;
typedef struct ta_dm_host ta_dm_host TA_VIEW;
typedef struct ta_value ta_value TA_HOST_VALUE;
typedef struct ta_session_image ta_session_image TA_HANDLE;

/* Finds or adds the address; *out holds a new reference. Allocates only the
 * first time a pair is seen, so only then can it fail with TA_ERR_NOMEM. */
TA_API ta_result ta_address_intern(ta_system *system,
                                   TA_IN const ta_actor_address *address,
                                   ta_address **out);
/* Retain and release cannot fail. Release accepts null. The record is freed
 * with its last reference. */
TA_API void ta_address_retain(ta_address *address);
TA_API void ta_address_release(TA_DESTROYS TA_OPTIONAL ta_address *address);
/* The strings, valid while the caller holds a reference. */
TA_API ta_actor_address ta_address_view(const ta_address *address);



typedef struct { void *ptr; size_t size; size_t alignment; } ta_allocator_free_params;
typedef struct ta_allocator_vtable {
    void *(*alloc)(void *self, size_t size, size_t alignment);
    void (*free)(void *self, TA_IN const ta_allocator_free_params *params);
} ta_allocator_vtable TA_VTABLE;
typedef struct { void *self; const ta_allocator_vtable *v; } ta_allocator TA_ADAPTER;


/* ========================================================================
 * Arenas
 *
 * A growable bump allocator over a ta_allocator. Adapters and hosts may use
 * it for request- or definition-scoped storage; it performs no I/O and
 * frees everything at reset or destroy.
 */

typedef struct ta_arena_block ta_arena_block TA_VIEW;
typedef struct {
    ta_allocator backing;
    ta_arena_block *head;
    size_t block_size;
} ta_arena;

TA_API ta_result ta_arena_init(TA_OUT TA_RELEASE(ta_arena_destroy) ta_arena *arena,
                                ta_allocator backing,
                                size_t block_size);
TA_API void ta_arena_reset(TA_INOUT ta_arena *arena);
TA_API void ta_arena_destroy(TA_INOUT ta_arena *arena);
TA_API void *ta_arena_alloc(TA_INOUT ta_arena *arena, size_t size, size_t alignment);
/*
 * An allocator that draws from `arena` and whose free() does nothing, so an
 * arena can back anything that wants a ta_allocator (ADR 0053). What it
 * allocates lives until the arena is reset or destroyed.
 */
TA_API ta_allocator ta_arena_allocator(TA_INOUT ta_arena *arena);
/* ========================================================================
 * Host values
 */


typedef struct ta_value_vtable {
    void (*retain)(void *self);
    void (*release)(void *self);
} ta_value_vtable TA_VTABLE;
struct ta_value { void *self; const ta_value_vtable *v; };


static inline void ta_value_retain(TA_INOUT TA_OPTIONAL ta_value *value) TA_ANY_VALUE_MODEL {
    if (value && value->v && value->v->retain) value->v->retain(value->self);
}

static inline void ta_value_release(TA_INOUT TA_OPTIONAL ta_value *value) TA_ANY_VALUE_MODEL {
    if (value && value->v && value->v->release) value->v->release(value->self);
}

/* ========================================================================
 * Events and faults
 */


typedef enum { TA_EVENT_EXTERNAL = 0, TA_EVENT_INTERNAL = 1, TA_EVENT_PLATFORM = 2 } ta_event_type TA_SYMBOLS("TA_EVENT_");
typedef enum { TA_ORIGIN_NONE = 0, TA_ORIGIN_ACTOR = 1, TA_ORIGIN_IO = 2, TA_ORIGIN_INVOKE = 3 } ta_origin_kind TA_SYMBOLS("TA_ORIGIN_");
/*
 * How long the URI of an event that came through an I/O processor lives, and
 * who stores it (ADR 0068). The processor chooses the class when it builds
 * the origin with ta_io_origin_make():
 * - COMPUTED: the origin holds a token, and the processor renders the URI
 *   from it with its `text` callback. Nothing is stored. For peer links, and
 *   connections whose URI follows a formula.
 * - SCOPED: the origin names a scope (ta_io_scope_open()) that stores the URI
 *   once, for a connection or a session. After ta_io_scope_close() the
 *   origin renders empty.
 * - MESSAGE: core stores the URI in a reference-counted entry that lives as
 *   long as any copy of the event holding it. For one request or message.
 */
typedef enum { TA_IO_ORIGIN_COMPUTED = 0, TA_IO_ORIGIN_SCOPED = 1, TA_IO_ORIGIN_MESSAGE = 2 } ta_io_origin_class TA_SYMBOLS("TA_IO_ORIGIN_");
/* A stored URI of a MESSAGE origin; core owns it. */
typedef struct ta_io_origin_entry ta_io_origin_entry TA_VIEW;
/* A scope of a SCOPED origin: stale once closed, never another scope's. */
typedef struct { uint32_t index; uint32_t generation; } ta_io_scope;
typedef struct {
    /* The processor's registry index (ADR 0068); never 0, the SCXML processor. */
    uint32_t processor;
    ta_io_origin_class origin_class;
    union {
        TA_WHEN(TA_IO_ORIGIN_COMPUTED) uint64_t token;
        TA_WHEN(TA_IO_ORIGIN_SCOPED) ta_io_scope scope;
        TA_WHEN(TA_IO_ORIGIN_MESSAGE) ta_io_origin_entry *entry;
    } as;
} ta_io_origin TA_TAGGED(origin_class, as);
/*
 * An event from an invoked service (ADR 0068). Core fills it: `invoker` is
 * the registry index of the invocation's invoker, so the origin keeps its
 * type after the invocation ends.
 */
typedef struct { ta_invoke_id invoke; uint32_t invoker; } ta_invoke_origin;
typedef struct {
    ta_origin_kind kind;
    union {
        TA_WHEN(TA_ORIGIN_ACTOR) ta_actor_ref actor;
        TA_WHEN(TA_ORIGIN_IO) ta_io_origin io;
        TA_WHEN(TA_ORIGIN_INVOKE) ta_invoke_origin invoke;
    } as;
} ta_origin TA_TAGGED(kind, as);
typedef struct { ta_event_name_id name; ta_event_type type; ta_send_id send_id; ta_origin origin; ta_invoke_id invoke_id; ta_value data; } ta_event;
/* invoke_id is TA_INVOKE_ID_NONE unless an invoked child generated the event. */
/* A receiver of a fault owns its data; the message is borrowed. */
typedef struct { ta_event_name_id detail; int32_t code; ta_str message; TA_RELEASE(ta_value_release) ta_value data; } ta_fault;
/*
 * What a data-model, invoker or I/O-processor operation answers. ERROR is
 * the chart's failure: core raises the event SCXML defines for it. NOMEM
 * says the operation could not get memory and changed nothing: core stops
 * the run where it stands, ta_actor_resume() answers TA_ERR_NOMEM, and the
 * operation is called again when the host resumes the actor (ADR 0047,
 * ADR 0070). An operation that may have changed something before it ran
 * out of memory answers ERROR.
 */
typedef enum { TA_OP_OK TA_SUCCESS = 0, TA_OP_ERROR = 1, TA_OP_NOMEM = 2 } ta_op_status TA_SYMBOLS("TA_OP_");
typedef enum { TA_RUN_STABLE = 0, TA_RUN_PAUSED = 1, TA_RUN_DONE = 2, TA_RUN_ERROR = 3 } ta_run_status TA_SYMBOLS("TA_RUN_");

/*
 * Trace context and cause (ADR 0067). They travel next to an event, not in
 * it: an SCXML event has no such field, so data models never see them, and
 * internal queue entries do not carry them.
 *
 * A step key names one step of one session: macrostep `macrostep` of session
 * `session` (ADR 0066), and microstep `microstep` within it, where 0 is the
 * macrostep itself. Macrosteps count from 1 per session, and 0 is outside a
 * macrostep. The counters run whether or not a trace sink is installed, so
 * keys are the same with tracing on or off. A cause with `session` 0 is the
 * host's: `macrostep` holds its parent span ID, or 0 for none. A lifecycle
 * record (SPAWNED, LOADED, UNLOADED, FINISHED, DESTROYED) is outside every
 * macrostep: its key is (session, 0, n), where n counts the session's
 * lifecycle records from 1 across residency, so each has its own span ID.
 * FINISHED names the step that finished as its `cause`.
 */
typedef struct { uint64_t session; uint64_t macrostep; uint32_t microstep; } ta_step_key;
/*
 * A W3C trace context: the trace-id is `trace_id_high` then `trace_id_low`,
 * each big-endian, and all zero means none; `flags` are the W3C
 * trace-flags. Core copies a context to everything a step produces, reads
 * only its sampled flag and `detail`, and creates one only when a host asks
 * (ta_system_new_trace, ADR 0073).
 *
 * `detail` is local, not W3C: it never appears in traceparent text, so it
 * does not leave the process. TA_TRACE_DETAIL_WATCHED makes a sampled
 * macrostep build the sink's `watched_kinds` instead of its `kinds`.
 */
typedef struct { uint64_t trace_id_high; uint64_t trace_id_low; uint8_t flags; uint8_t detail; } ta_trace_context;
#define TA_TRACE_FLAG_SAMPLED ((uint8_t)0x01u)
/* W3C Trace Context Level 2: the trace ID's rightmost 56 bits are random. */
#define TA_TRACE_FLAG_RANDOM ((uint8_t)0x02u)
#define TA_TRACE_DETAIL_WATCHED ((uint8_t)0x01u)
/*
 * What a step inherits from what caused it: the trace context, and the key
 * of the step that produced it. All zero is no parent.
 */
typedef struct { ta_trace_context context; ta_step_key cause; } ta_trace_parent;
/*
 * An 8-byte span ID for a step, derived the same way in every host (ADR
 * 0067): a fixed 64-bit mix of the key, never 0. A host cause (`session` 0)
 * returns `macrostep`, the host's own span ID, unchanged.
 */
TA_API uint64_t ta_step_key_span_id(ta_step_key key);

/*
 * W3C `traceparent` text (ADR 0073): version-traceid-parentid-flags, in
 * lowercase hex. Parsing ignores spaces and tabs around the text, rejects
 * version ff, an all-zero trace ID or parent ID, and anything malformed
 * (TA_ERR_INVALID), and reads a future version by its first 55
 * characters. The parent ID becomes a host cause: `session` 0 and
 * `macrostep` holding the span ID (ADR 0067). `detail` is 0.
 */
typedef struct { ta_str text; TA_OUT ta_trace_parent *out; } ta_traceparent_parse_params;
TA_API ta_result ta_traceparent_parse(TA_IN const ta_traceparent_parse_params *params);

typedef struct {
    ta_trace_parent parent;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_traceparent_format_params;

/*
 * Writes version 00, the context's trace ID, the span ID of the cause
 * (ta_step_key_span_id) and the context's W3C flags, following the buffer
 * convention (`*needed` is 56, NUL included; a too-small buffer yields
 * TA_ERR_NOMEM). A parent with no trace ID or no cause is TA_ERR_INVALID.
 */
TA_API ta_result ta_traceparent_format(TA_IN const ta_traceparent_format_params *params);

typedef struct {
    ta_trace_context context;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_trace_id_format_params;
/* A trace ID as 32 lowercase hex digits (`*needed` is 33). */
TA_API ta_result ta_trace_id_format(TA_IN const ta_trace_id_format_params *params);

typedef struct {
    uint64_t span_id;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_span_id_format_params;
/* A span ID as 16 lowercase hex digits (`*needed` is 17). */
TA_API ta_result ta_span_id_format(TA_IN const ta_span_id_format_params *params);

/*
 * OpenTelemetry's consistent probability sampling (ADR 0073): the
 * randomness R is the trace ID's rightmost 56 bits, the threshold is
 * T = round((1 - ratio) * 2^56), and a trace is sampled when R >= T. The
 * ratio is `parts` / `whole`, so every host computes the same threshold
 * without floating point; `parts` above `whole` counts as `whole`, and a
 * `whole` of 0 as a ratio of 0.
 */
TA_API uint64_t ta_trace_ratio_threshold(uint32_t parts, uint32_t whole);
TA_API bool ta_trace_sampled_at(TA_IN const ta_trace_context *context, uint64_t threshold);


typedef struct {
    ta_event event;
    TA_FLAGS(ta_event_flags) uint32_t flags;
} ta_queued_event;

enum TA_FLAG_SET(ta_event_flags, "TA_EVENT_FLAG_") {
    TA_EVENT_FLAG_OWNS_DATA = 1u << 0,
    TA_EVENT_FLAG_GENERATED = 1u << 1
};

/*
 * An event owns its data, a reference to its origin's address when that is
 * a named actor (ADR 0065), and a reference to its origin's entry when that
 * is a MESSAGE I/O origin (ADR 0068). Retaining copies the envelope and
 * retains all of them; it cannot fail. Releasing gives them back.
 */
TA_API void ta_event_retain(TA_IN const ta_event *event, TA_OUT TA_RELEASE(ta_event_release) ta_event *out)
    TA_ANY_VALUE_MODEL;
TA_API void ta_event_release(TA_INOUT ta_event *event) TA_ANY_VALUE_MODEL;
/* ========================================================================
 * External SCXML event routing
 */


/*
 * A fully normalized event destined for another SCXML external event queue,
 * with the trace parent the receiver's macrostep runs under: the sending
 * macrostep's context, and the key of the step that sent it (ADR 0067). A
 * router passes `trace` on to ta_actor_deliver().
 */
typedef struct {
    ta_actor_ref destination;
    ta_event event;
    ta_trace_parent trace;
} ta_external_delivery;

/*
 * Keeps a delivery past the submit call: retains its destination, its
 * origin and its data (ADR 0050, ADR 0065). It never allocates and cannot
 * fail. Release gives all three back.
 */
TA_API void ta_external_delivery_retain(TA_IN const ta_external_delivery *delivery,
                                        TA_OUT TA_RELEASE(ta_external_delivery_release) ta_external_delivery *out)
    TA_ANY_VALUE_MODEL;
TA_API void ta_external_delivery_release(TA_INOUT ta_external_delivery *delivery) TA_ANY_VALUE_MODEL;

typedef struct {
    TA_IN const ta_external_delivery *delivery;
    TA_OUT TA_VALID_ON(TA_EXTERNAL_EVENT_ERROR) TA_LIFETIME(call) ta_fault *fault;
} ta_external_event_submit_params;

typedef enum {
    /* Delivery was accepted; an asynchronous router copied borrowed fields. */
    TA_EXTERNAL_EVENT_ACCEPTED TA_SUCCESS = 0,
    /* Nothing was accepted; core retains and retries the delivery. */
    TA_EXTERNAL_EVENT_RETRY,
    /* Nothing was accepted; fault describes a communication failure. */
    TA_EXTERNAL_EVENT_ERROR
} ta_external_event_submit_status TA_SYMBOLS("TA_EXTERNAL_EVENT_");

typedef struct ta_external_event_router_vtable {
    ta_external_event_submit_status (*submit)(
        void *self,
        TA_IN const ta_external_event_submit_params *params);
} ta_external_event_router_vtable TA_VTABLE;

typedef struct {
    void *self;
    /* Null uninstalls the router. */
    TA_OPTIONAL const ta_external_event_router_vtable *v;
} ta_external_event_router TA_ADAPTER;


/* ========================================================================
 * Expressions and definition identity
 */

typedef enum {
    TA_EXPRESSION_NONE = 0,
    TA_EXPRESSION_SOURCE,
    TA_EXPRESSION_HOST
} ta_expression_kind TA_SYMBOLS("TA_EXPRESSION_");

typedef struct ta_expression_host_vtable {
    ta_result (*retain)(void *self, void *value, ta_allocator allocator, void **out);
    void (*release)(void *self, void *value, ta_allocator allocator);
} ta_expression_host_vtable TA_VTABLE;

typedef struct {
    void *self;
    const ta_expression_host_vtable *v;
    void *value;
} ta_expression_host TA_ADAPTER;

/*
 * A frontend expression retained by a compiled definition. Source expressions
 * are copied by the definition compiler. Host expressions are retained and
 * released through their adapter; for example, a Node-API adapter retains a
 * napi_ref rather than a callback-scoped napi_value.
 */
typedef struct {
    ta_expression_kind kind;
    union {
        TA_WHEN(TA_EXPRESSION_SOURCE) ta_str source;
        TA_WHEN(TA_EXPRESSION_HOST) ta_expression_host host;
    } as;
} ta_expression TA_TAGGED(kind, as);

/*
 * Process-independent identity of one exact immutable definition revision.
 * `family` is the stable logical statechart family; `revision` is an opaque
 * fingerprint copied/owned by the compiled definition.  Any change that can
 * alter interpretation of a session image must produce a different revision.
 */
typedef struct {
    ta_definition_family family;
    ta_bytes revision;
} ta_definition_identity;

/* ========================================================================
 * Data models
 */

/*
 * Prepares one compiled definition for a data model. `expressions` is the
 * definition's expression table, indexed by ta_expr_id. It is borrowed and
 * remains valid until destroy_definition() for the same `shared` state, so the
 * adapter may keep the pointer instead of copying. The adapter never sees the
 * compiled definition itself.
 */
/*
 * Which expression a data model refused at prepare_definition(), and where in
 * its source (ADR 0058). `offset` and `length` are bytes within that
 * expression's source text; both zero when the whole expression is meant.
 * `message` stays the adapter's: it must stay valid until core next calls
 * the adapter, and core copies it before then.
 */
typedef struct {
    ta_expr_id expression;
    uint32_t offset;
    uint32_t length;
    ta_str message;
} ta_expression_rejection;

typedef struct {
    TA_COUNTED_BY(expression_count) TA_OPTIONAL TA_LIFETIME(definition) const ta_expression *expressions;
    uint32_t expression_count;
    ta_allocator allocator;
    void **shared;
    /*
     * Set by an adapter that returns TA_ERR_INVALID or TA_ERR_UNSUPPORTED
     * because of one expression; `expression` stays TA_EXPR_ID_NONE
     * otherwise. Null when the caller wants no detail.
     */
    TA_OUT TA_OPTIONAL TA_LIFETIME(call) ta_expression_rejection *rejected;
} ta_datamodel_prepare_definition_params;
/* Core keeps at most this many bytes of a rejection's message, cut at a
 * UTF-8 boundary. */
#define TA_COMPILE_ERROR_MESSAGE_MAX ((uint32_t)256)
typedef struct { void *shared; void *instance; TA_IN TA_OPTIONAL const ta_value *binding; const ta_dm_host *host; } ta_datamodel_init_instance_params;
/*
 * Named top-level data bindings (SCXML <data>). `name` is the author's data ID,
 * borrowed for the call. declare() creates the binding with the data model's
 * uninitialized representation; set_named() installs an initializer's value.
 * Late binding declares every binding at bootstrap and sets each one only when
 * its state is first entered (ADR 0032).
 */
/*
 * `bound` may be null. declare() sets `*bound` when the instance's binding
 * already supplies a value for `name` (e.g. <param>s passed to an invoked
 * child); the core then skips that binding's initializer.
 */
typedef struct { void *shared; void *instance; ta_str name; const ta_dm_host *host; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; TA_OUT TA_OPTIONAL bool *bound; } ta_datamodel_declare_params;
typedef struct { void *shared; void *instance; ta_str name; TA_IN const ta_value *value; const ta_dm_host *host; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_set_named_params;
typedef struct { void *shared; void *instance; ta_expr_id expr; const ta_dm_host *host; TA_OUT bool *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_eval_condition_params;
typedef struct { void *shared; void *instance; ta_expr_id expr; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_eval_value_params;
typedef struct { void *shared; void *instance; ta_expr_id location; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_read_location_params;
typedef struct { void *shared; void *instance; ta_expr_id location; TA_IN const ta_value *value; const ta_dm_host *host; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_assign_params;
typedef struct { void *shared; void *instance; ta_expr_id script; const ta_dm_host *host; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_execute_script_params;
typedef enum {
    TA_SCXML_TARGET_INVALID = 0,
    TA_SCXML_TARGET_INTERNAL,
    TA_SCXML_TARGET_SELF,
    TA_SCXML_TARGET_PARENT,
    TA_SCXML_TARGET_INVOKE,
    TA_SCXML_TARGET_ACTOR
} ta_scxml_target_kind TA_SYMBOLS("TA_SCXML_TARGET_");
typedef struct {
    ta_scxml_target_kind kind;
    union {
        TA_WHEN(TA_SCXML_TARGET_INVOKE) ta_invoke_id invoke;
        TA_WHEN(TA_SCXML_TARGET_ACTOR) ta_actor_ref actor;
    } as;
} ta_scxml_target TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;

/*
 * The SCXML Event I/O Processor target URIs (ADR 0044). Core owns the
 * grammar, so a literal `target`, an evaluated `targetexpr`, and the text
 * core writes for `$_event.origin` agree by construction:
 *
 *   #_internal                 the sender's own internal queue
 *   #_parent                   the invoking parent session
 *   #_scxml_<session>          a session by its session ID (ADR 0066)
 *   #_actor_<family>/<name>    a named actor by its logical address
 *   #_<invokeid>               an invocation of the sending session
 *
 * A session URI whose text is not a session ID names no session: it parses
 * as runtime actor 0, which never exists, so a send to it is inaccessible
 * (SCXML C.1) rather than invalid. TA_ERR_INVALID means an invalid target,
 * which a send reports as error.execution.
 *
 * A named actor target holds a reference to its interned address (ADR
 * 0065); release the target with ta_scxml_target_release(). Interning a new
 * address can fail with TA_ERR_NOMEM.
 */
TA_API ta_result ta_scxml_target_parse(ta_system *system,
                                       ta_str text,
                                       TA_OUT TA_RELEASE(ta_scxml_target_release) ta_scxml_target *out);
/* Releases a parsed target's address reference; accepts any target. */
TA_API void ta_scxml_target_release(TA_INOUT ta_scxml_target *target);

typedef struct {
    ta_scxml_target target;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_scxml_target_format_params;

/* The URI of a target. TA_ERR_NOT_FOUND when its kind has no URI. */
TA_API ta_result ta_scxml_target_format(const ta_system *system,
                                        TA_IN const ta_scxml_target_format_params *params);

/*
 * Send IDs as text (ADR 0048). A literal `<send id>` is the author's text; a
 * generated ID is `#_send.<n>`, the sending actor's counter. Core writes this
 * text for `idlocation` and `_event.sendid` and reads it back for
 * `<cancel sendidexpr>`, so the two forms name the same send by
 * construction: a host holding a `ta_send_id`, such as an I/O processor
 * looking at `ta_send.send_id`, can render the text its chart holds, and a
 * host holding that text can name the send core knows.
 *
 * Parsing the generated form always succeeds, whether or not such a send
 * exists, as a session URI does. A literal ID must already be interned in
 * this system, since no send of it can carry text it has never seen:
 * otherwise TA_ERR_NOT_FOUND. TA_ERR_INVALID means the text is no send ID at
 * all, such as an empty string.
 */
TA_API ta_result ta_send_id_parse(const ta_system *system,
                                  ta_str text,
                                  TA_OUT ta_send_id *out);

typedef struct {
    ta_send_id send_id;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_send_id_format_params;

/*
 * The text of a send ID, following the buffer convention: `*needed` receives
 * the size including the terminating NUL, and a too-small buffer yields
 * TA_ERR_NOMEM without writing, so a zero capacity is a size query.
 * TA_ERR_NOT_FOUND when the send has no ID.
 */
TA_API ta_result ta_send_id_format(const ta_system *system,
                                   TA_IN const ta_send_id_format_params *params);
/* One <param>: `name` is the author's parameter name, borrowed for the call. */
typedef struct { ta_str name; ta_value value; } ta_named_value;
typedef struct {
    void *shared;
    void *instance;
    TA_COUNTED_BY(count) TA_OPTIONAL const ta_named_value *values;
    uint32_t count;
    const ta_dm_host *host;
    TA_OUT TA_RELEASE(ta_value_release) ta_value *out;
    TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault;
} ta_datamodel_make_params_params;
typedef struct { void *shared; void *instance; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; } ta_datamodel_inspect_params;
/*
 * Conversions the SCXML `*expr` attribute forms need. value_text reads an
 * evaluated value as text (eventexpr, typeexpr, sendidexpr, srcexpr,
 * delayexpr, targetexpr); `*out` is borrowed from `value` and valid while it
 * is retained, so core can parse a target URI out of it and keep the named
 * address a send needs (ADR 0044).
 * text_value makes a value from text, e.g. a generated send or invoke ID
 * stored through idlocation.
 */
typedef struct { void *shared; void *instance; TA_IN const ta_value *value; const ta_dm_host *host; TA_OUT TA_LIFETIME(input) ta_str *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_value_text_params;
typedef struct { void *shared; void *instance; ta_str text; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_text_value_params;
/* Imports literal <content> or <data> content into a legal data-model value. */
typedef struct { void *shared; void *instance; ta_bytes content; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_import_content_params;
/*
 * <foreach>: foreach_snapshot evaluates `array` once and returns a shallow
 * copy and its length; the core keeps the snapshot while iterating.
 * foreach_bind assigns item `index` of the snapshot to the `item` variable
 * and, when index_name.ptr is non-null, the index to that variable.
 */
typedef struct { void *shared; void *instance; ta_expr_id array; const ta_dm_host *host; TA_OUT TA_RELEASE(ta_value_release) ta_value *out; TA_OUT uint32_t *count; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_foreach_snapshot_params;
typedef struct { void *shared; void *instance; TA_IN const ta_value *snapshot; uint32_t index; ta_str item; ta_str index_name; const ta_dm_host *host; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_datamodel_foreach_bind_params;
typedef struct ta_datamodel_vtable {
    /*
     * The SCXML data-model name this adapter implements, e.g. "null" or
     * "ecmascript". Compared with ta_scxml_spec.datamodel at compile time.
     */
    ta_str name;
    ta_result (*prepare_definition)(void *self, TA_IN const ta_datamodel_prepare_definition_params *params);
    void (*destroy_definition)(void *self, void *shared, ta_allocator allocator);
    uint32_t (*instance_size)(void *self, void *shared);
    uint32_t (*instance_alignment)(void *self, void *shared);
    ta_result (*init_instance)(void *self, TA_IN const ta_datamodel_init_instance_params *params);
    void (*destroy_instance)(void *self, void *shared, void *instance);
    ta_op_status (*declare)(void *self, TA_IN const ta_datamodel_declare_params *params);
    ta_op_status (*set_named)(void *self, TA_IN const ta_datamodel_set_named_params *params);
    ta_op_status (*eval_condition)(void *self, TA_IN const ta_datamodel_eval_condition_params *params);
    ta_op_status (*eval_value)(void *self, TA_IN const ta_datamodel_eval_value_params *params);
    ta_op_status (*read_location)(void *self, TA_IN const ta_datamodel_read_location_params *params);
    ta_op_status (*assign)(void *self, TA_IN const ta_datamodel_assign_params *params);
    ta_op_status (*execute_script)(void *self, TA_IN const ta_datamodel_execute_script_params *params);
    ta_op_status (*make_params)(void *self, TA_IN const ta_datamodel_make_params_params *params);
    ta_result (*inspect)(void *self, TA_IN const ta_datamodel_inspect_params *params);
    ta_op_status (*value_text)(void *self, TA_IN const ta_datamodel_value_text_params *params);
    ta_op_status (*text_value)(void *self, TA_IN const ta_datamodel_text_value_params *params);
    ta_op_status (*import_content)(void *self, TA_IN const ta_datamodel_import_content_params *params);
    ta_op_status (*foreach_snapshot)(void *self, TA_IN const ta_datamodel_foreach_snapshot_params *params);
    ta_op_status (*foreach_bind)(void *self, TA_IN const ta_datamodel_foreach_bind_params *params);
} ta_datamodel_vtable TA_VTABLE;
typedef struct { void *self; const ta_datamodel_vtable *v; } ta_datamodel TA_ADAPTER;


/* Queries available to a data-model adapter while core calls into it. */
TA_API bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state);
/*
 * The state of the running definition with this author ID, for In() (ADR
 * 0049). TA_ERR_NOT_FOUND when the chart has no such state: SCXML B.1 makes
 * In() of it simply false, so an adapter answers that without a lookup of
 * its own.
 */
TA_API ta_result ta_dm_host_find_state(const ta_dm_host *host,
                                       ta_str id,
                                       TA_OUT ta_state_id *out);
TA_API const ta_event *ta_dm_host_event(const ta_dm_host *host) TA_OPTIONAL TA_LIFETIME(call);
TA_API ta_actor_id ta_dm_host_actor(const ta_dm_host *host);

/*
 * The SCXML Null data model (SCXML B.1, ADR 0057), named "null". Its only
 * expression is the In(state) predicate in a condition, with the ID quoted in
 * ' or " or bare; In() of a state the chart does not have is false. A chart
 * with any other expression fails to compile, and the error names the
 * expression (ADR 0058). Every operation that needs data fails at run time
 * as error.execution. It is stateless (`.self` is null) and allocates nothing
 * per actor.
 */
TA_API ta_datamodel ta_null_datamodel(void);

/*
 * Text of the SCXML system variables, for data models that expose them
 * (SCXML 5.10, ADR 0068). `*out` is a view, not NUL-terminated, valid until
 * the interpreter run that is calling the data model ends: core renders it
 * into the run's scratch arena, or points at text it stores. Copy it before
 * returning to core. TA_ERR_NOT_FOUND means the field has no value, e.g. no
 * event has been processed yet, the current event has no send ID or origin,
 * or `io_index` is past the last processor. TA_ERR_NOMEM means the arena
 * could not grow.
 *
 * `_ioprocessors` is enumerated by reading TA_DM_TEXT_IO_TYPE for io_index
 * 0, 1, 2, ... until TA_ERR_NOT_FOUND. Index 0 is the SCXML processor.
 */
typedef enum {
    /* _event.name, dotted. */
    TA_DM_TEXT_EVENT_NAME = 0,
    /* _event.type: "platform", "internal", or "external". */
    TA_DM_TEXT_EVENT_TYPE,
    /* _event.sendid: the author's send ID, or the generated one. */
    TA_DM_TEXT_EVENT_SENDID,
    /* _event.origin, a URI that can be used as a send target. */
    TA_DM_TEXT_EVENT_ORIGIN,
    /* _event.origintype, the URI of the processor for origin. */
    TA_DM_TEXT_EVENT_ORIGINTYPE,
    /* _event.invokeid. */
    TA_DM_TEXT_EVENT_INVOKEID,
    /* _sessionid. */
    TA_DM_TEXT_SESSION_ID,
    /* _name: the <scxml> name attribute. */
    TA_DM_TEXT_SESSION_NAME,
    /* _ioprocessors: the type URI of the processor at io_index. */
    TA_DM_TEXT_IO_TYPE,
    /* _ioprocessors: this session's location through the processor at io_index. */
    TA_DM_TEXT_IO_LOCATION
} ta_dm_text_kind TA_SYMBOLS("TA_DM_TEXT_");
typedef struct {
    ta_dm_text_kind kind;
    /* TA_DM_TEXT_IO_TYPE and TA_DM_TEXT_IO_LOCATION: the processor's registry index. */
    uint32_t io_index;
    TA_OUT TA_LIFETIME(run) ta_str *out;
} ta_dm_text_params;
TA_API ta_result ta_dm_host_text(const ta_dm_host *host, TA_IN const ta_dm_text_params *params);
/* The author's ID of a state; TA_ERR_NOT_FOUND ends iteration over 0, 1, 2, ... */
TA_API ta_result ta_dm_host_state_name(const ta_dm_host *host,
                                       ta_state_id state,
                                       TA_OUT TA_LIFETIME(definition) ta_str *out);
/* ========================================================================
 * I/O processors
 */


/*
 * One send to a host I/O processor. `target` is the evaluated `targetexpr`;
 * `target_text` is a literal `target`, the text the chart wrote, which is
 * never imported into the data model, so a chart on the null data model can
 * name one. At most one of them is present. `data` is the evaluated
 * `<content expr>`, namelist or <param>s; `content` is a literal <content>,
 * delivered as the bytes the chart holds and not imported into the data
 * model (ADR 0053).
 * `content_type` is the literal's media type, empty when nothing said. At
 * most one of `data` and `content` is present. `trace` is the sending
 * macrostep's context and the key of the step that sent it (ADR 0067), for
 * the processor to carry, as a `traceparent` header for example. Everything
 * is borrowed for the call.
 */
typedef struct { ta_actor_ref source; ta_send_id send_id; ta_event_name_id event; ta_atom io_type; ta_value target;
                 ta_str target_text; ta_value data; ta_str content_type; ta_bytes content; ta_trace_parent trace; } ta_send;
/*
 * What an I/O processor renders (ADR 0068): the URI of one of its COMPUTED
 * origins, or this session's location through it (`_ioprocessors`).
 */
typedef enum { TA_IO_TEXT_ORIGIN = 0, TA_IO_TEXT_LOCATION = 1 } ta_io_text_kind TA_SYMBOLS("TA_IO_TEXT_");
typedef struct {
    ta_io_text_kind kind;
    /* The session the text is for (ADR 0066); 0 when there is none. */
    uint64_t session_id;
    /* Its named address, or null. */
    TA_OPTIONAL ta_address *address;
    /* TA_IO_TEXT_ORIGIN: the COMPUTED origin's token. */
    uint64_t token;
    /* Where the text must be allocated. */
    ta_allocator arena;
    TA_OUT TA_LIFETIME(arena) ta_str *out;
} ta_io_text_params;
/*
 * `text` is optional. Its contract (ADR 0068):
 * - pure: the text depends only on the processor's registration, the
 *   session and the token, for the life of the system, so a replayed
 *   history reads the same text;
 * - it makes no calls into core: it runs while a data model evaluates an
 *   expression inside a macrostep;
 * - all memory for the text comes from `arena`, and it keeps no pointer into
 *   it;
 * - it answers TA_OK with the text, TA_ERR_NOT_FOUND when there is no such
 *   text (the field is then empty), or TA_ERR_NOMEM when the arena cannot
 *   grow.
 * A processor without `text` has no location and cannot make COMPUTED
 * origins. A Basic HTTP processor must render a location (SCXML C.2.3).
 */
typedef struct ta_io_vtable {
    ta_op_status (*send)(void *self,
                         TA_IN const ta_send *send,
                         TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault);
    TA_OPTIONAL ta_result (*text)(void *self, TA_IN const ta_io_text_params *params);
} ta_io_vtable TA_VTABLE;
typedef struct { void *self; const ta_io_vtable *v; } ta_io TA_ADAPTER;

/*
 * Origins of events that come through an I/O processor (ADR 0068). A host
 * builds one before it delivers the event, and puts it in the event. A
 * MESSAGE origin comes back holding one reference, which the event then
 * owns: ta_event_release() gives it back. An origin that never went into a
 * delivered event is released with ta_origin_release().
 */
typedef struct {
    /* The processor's registry index, from ta_system_register_io(). */
    uint32_t processor;
    ta_io_origin_class origin_class;
    /* COMPUTED: rendered by the processor's `text` callback. */
    uint64_t token;
    /* SCOPED: an open scope of this processor. */
    ta_io_scope scope;
    /* MESSAGE: copied into core's storage. */
    ta_str uri;
    TA_OUT TA_RELEASE(ta_origin_release) ta_origin *out;
} ta_io_origin_make_params;
TA_API ta_result ta_io_origin_make(ta_system *system, TA_IN const ta_io_origin_make_params *params);
/* Releases a made origin that never went into a delivered event; accepts any origin. */
TA_API void ta_origin_release(TA_INOUT ta_origin *origin);

/*
 * A scope stores one URI for a connection or a session. Events refer to it
 * without holding it: once closed, an origin naming it renders empty, so a
 * late reply fails. Closing is idempotent for a stale scope.
 */
typedef struct { uint32_t processor; ta_str uri; TA_OUT ta_io_scope *out; } ta_io_scope_open_params;
TA_API ta_result ta_io_scope_open(ta_system *system, TA_IN const ta_io_scope_open_params *params);
TA_API void ta_io_scope_close(ta_system *system, ta_io_scope scope);

/*
 * The text of any origin, for hosts that show or log events outside a data
 * model. `*type` is the processor type (empty for no origin) and lives with
 * the system. `*uri` is rendered into `arena` for actor, invoke and COMPUTED
 * origins, and is a view of stored text for SCOPED and MESSAGE ones, valid
 * while the origin is held and its scope is open. TA_ERR_NOT_FOUND when the
 * origin has no text: none, or a closed scope.
 */
typedef struct {
    TA_IN const ta_origin *origin;
    /* The session the origin was delivered to, for the processor's `text`; 0 when unknown. */
    uint64_t session_id;
    ta_allocator arena;
    TA_OUT TA_LIFETIME(system) ta_str *type;
    TA_OUT TA_LIFETIME(arena) ta_str *uri;
} ta_origin_text_params;
TA_API ta_result ta_origin_text(const ta_system *system, TA_IN const ta_origin_text_params *params);


/* ========================================================================
 * Logging
 *
 * SCXML <log> (SCXML 4.7): a label and an optional value, with an
 * implementation-dependent rendering. The record carries the evaluated
 * value rather than text, as ta_send does, so the host renders it with the
 * data model it already owns (ADR 0045).
 */


typedef struct {
    /* The actor that ran the <log>. */
    ta_actor_ref source;
    /* <log label>; empty when the author wrote none. */
    ta_str label;
    /* False when the <log> has no expr, or when evaluating it failed. */
    bool has_value;
    /* Borrowed for the call; retain it to keep it (ADR 0006). */
    ta_value value;
} ta_log_record;

/*
 * Writing a log has no result: SCXML 4.7 requires <log> to have no effect
 * on document interpretation, so a sink cannot fail the action.
 */
typedef struct ta_log_sink_vtable {
    void (*write)(void *self, TA_IN const ta_log_record *record);
} ta_log_sink_vtable TA_VTABLE;

typedef struct {
    void *self;
    /* Null uninstalls the sink; <log> then only evaluates its expression. */
    TA_OPTIONAL const ta_log_sink_vtable *v;
} ta_log_sink TA_ADAPTER;


/* ========================================================================
 * Tracing (ADR 0067)
 *
 * Core reports its decisions and the data they changed to one trace sink
 * per system, as flat records. A record is written once, when the step
 * that produced it has committed: a step that runs out of memory and runs
 * again (ADR 0070) writes its records on the run that commits, and the step
 * counters move only then, so an interrupted run and an uninterrupted one
 * write the same records apart from PAUSED, RESUMED and STOPPED.
 */

typedef enum {
    /* Lifecycle; outside macrosteps, and never sampled. */
    TA_TRACE_SPAWNED = 0,       /* `cause`: the spawn's; bootstrap follows as macrostep 1 */
    TA_TRACE_LOADED,
    TA_TRACE_UNLOADED,
    TA_TRACE_FINISHED,          /* `state`: the top-level final state (ADR 0056) */
    TA_TRACE_DESTROYED,

    /* Macrosteps. */
    TA_TRACE_MACROSTEP_BEGIN,   /* `event`: the external event, or none; `cause`: the macrostep's */
    TA_TRACE_MACROSTEP_END,     /* stable, or finished; `states`: the active configuration */

    /* Decisions. */
    TA_TRACE_CONDITION,         /* `expr`, `result` or `fault`; a guard's `transition`, or an <if>'s */
    TA_TRACE_MICROSTEP,         /* `event` (none: eventless), `transitions`, `exited`, `entered`, `history` */
    TA_TRACE_RAISED,            /* `event`: one core queued internally */

    /* Data. */
    TA_TRACE_DATA_INIT,         /* `name`, `expr` or none, `value` or `fault`: a <data> */
    TA_TRACE_ASSIGN,            /* `expr` (the location), `value` */
    TA_TRACE_SCRIPT,            /* `expr` (the script), success or `fault` */
    TA_TRACE_FOREACH,           /* `value` (the snapshot), `iteration` */
    TA_TRACE_ACTION_FAILED,     /* `action`, `fault`; the error.execution is the RAISED record that follows */

    /* Effects that leave the actor. */
    TA_TRACE_SENT,              /* `send`, `event`; its cause is this record's key */
    TA_TRACE_CANCELLED,         /* `send` */
    TA_TRACE_INVOKED,           /* `invoke` */
    TA_TRACE_INVOKE_CANCELLED,  /* `invoke` */

    /* Execution control. */
    TA_TRACE_PAUSED,            /* a budget ran out */
    TA_TRACE_RESUMED,           /* a run continues a macrostep begun before */
    TA_TRACE_STOPPED            /* an allocation failed; the run stopped where it stood (ADR 0070) */
} ta_trace_kind TA_SYMBOLS("TA_TRACE_");

/* The bit of `kind` in a sink's mask. */
#define TA_TRACE_BIT(kind) ((uint32_t)1u << (uint32_t)(kind))

/* Where in a macrostep a record was written. */
typedef enum {
    TA_TRACE_AT_NONE = 0,       /* not inside a step                                */
    TA_TRACE_AT_SELECT,         /* selecting transitions (guards)                   */
    TA_TRACE_AT_EXIT,           /* onexit of `state`                                */
    TA_TRACE_AT_TRANSITION,     /* content of `transition`, whose source is `state` */
    TA_TRACE_AT_ENTER,          /* onentry, initial, history or late data of `state` */
    TA_TRACE_AT_BOOTSTRAP,      /* the bootstrap's <data> and top-level <script>    */
    TA_TRACE_AT_FINALIZE,       /* <finalize>, and autoforwarding                   */
    TA_TRACE_AT_INVOKE          /* starting the invocations of `state`              */
} ta_trace_at TA_SYMBOLS("TA_TRACE_AT_");

#define TA_TRANSITION_NONE ((ta_transition_id)UINT32_MAX)
#define TA_ACTION_NONE ((ta_action_id)UINT32_MAX)

/*
 * The public view of where the interpreter stands (ADR 0060, ADR 0061).
 * IDs are the definition's compiled IDs: `action` indexes its actions, and
 * `iteration` is the <foreach> iteration, from 0, of the innermost loop.
 */
typedef struct {
    ta_trace_at at;
    ta_state_id state;
    ta_transition_id transition;
    ta_action_id action;
    uint32_t iteration;
} ta_trace_position;

/*
 * One record. Only the fields its kind names are meaningful; the rest are
 * zero, or NONE for IDs. Everything is borrowed for the call: retain a value
 * to keep it (ADR 0006). `context` is the running macrostep's; `cause`, for
 * MACROSTEP_BEGIN and SPAWNED, is the key of the step that caused it. A
 * fault carries its `detail` and `code`; its data went into the
 * error.execution event of the RAISED record that follows.
 */
typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    ta_trace_position position;
    ta_trace_context context;
    ta_step_key cause;
    ta_actor_id actor;
    ta_definition_id definition;
    ta_time now;

    TA_IN TA_OPTIONAL const ta_event *event;
    ta_state_id state;
    ta_expr_id expr;
    ta_str name;
    ta_action_id action;
    uint32_t iteration;
    ta_send_id send;
    ta_invoke_id invoke;
    bool result;
    bool has_value;
    ta_value value;
    bool has_fault;
    ta_fault fault;

    TA_COUNTED_BY(transition_count) const ta_transition_id *transitions;
    uint32_t transition_count;
    TA_COUNTED_BY(exited_count) const ta_state_id *exited;
    uint32_t exited_count;
    TA_COUNTED_BY(entered_count) const ta_state_id *entered;
    uint32_t entered_count;
    TA_COUNTED_BY(history_count) const ta_state_id *history;
    uint32_t history_count;
    TA_COUNTED_BY(state_count) const ta_state_id *states;
    uint32_t state_count;
} ta_trace_record;

/*
 * Writing a record cannot fail and cannot change the run. A sink must not
 * call back into the system for the actor that is running; it may read the
 * record, retain values it keeps, and inspect other actors.
 */
typedef struct ta_trace_sink_vtable {
    void (*write)(void *self, TA_IN const ta_trace_record *record);
} ta_trace_sink_vtable TA_VTABLE;

/*
 * Three masks of TA_TRACE_BIT(kind) (ADR 0073): core builds no record whose
 * bit the macrostep's mask lacks. A macrostep picks its mask at
 * MACROSTEP_BEGIN from its context: `kinds` when it has no trace ID or is
 * sampled, `watched_kinds` when it is sampled and marked
 * TA_TRACE_DETAIL_WATCHED, and `unsampled_kinds` when it is not sampled.
 * Lifecycle records are outside macrosteps and are written when their bit
 * is in `kinds` or `watched_kinds`.
 */
typedef struct {
    void *self;
    const ta_trace_sink_vtable *v;
    uint32_t kinds;
    uint32_t watched_kinds;
    uint32_t unsampled_kinds;
} ta_trace_sink TA_ADAPTER;

/* The sink a system starts with: it wants nothing and writes nothing. */
TA_API ta_trace_sink ta_null_trace_sink(void);


/* ========================================================================
 * Invokers
 */


/*
 * One invocation to start. `type` is the resolved invoker type; the SCXML
 * aliases "scxml" and "http://www.w3.org/TR/scxml" are normalized to
 * "http://www.w3.org/TR/scxml/". `src` is the evaluated src/srcexpr, `data`
 * the evaluated `<content expr>`, and `params` the data model's aggregate of
 * <param>/namelist values; each is empty (null vtable) when absent. A literal
 * <content> is not imported: it arrives as `content`, the bytes the chart
 * holds, with `content_type` its media type (empty when nothing said), and
 * interpreting it is the invoker's business (ADR 0053, SCXML 6.4.2). Values
 * and bytes are borrowed for the call. A service for an SCXML session spawns its child with
 * ta_actor_spawn() and a `parent` of {parent, invoke_id}; the core then routes
 * `#_<invokeid>` sends, autoforwarding, and done.invoke itself. `trace` is the
 * invoking macrostep's context and the key of the step that invoked, which
 * such a service passes to ta_actor_spawn(), so the child's bootstrap runs
 * under it (ADR 0067).
 */
typedef struct { ta_system *system; ta_actor_id parent; ta_invoke_id invoke_id; ta_atom type; ta_value src; ta_value data;
                 ta_str content_type; ta_bytes content; ta_value params; bool autoforward; ta_trace_parent trace; } ta_invoke_request;
typedef struct { TA_IN const ta_invoke_request *request; TA_OUT uint64_t *service_handle; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_invoker_start_params;
typedef struct { uint64_t service_handle; TA_IN const ta_event *event; TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault; } ta_invoker_forward_params;

/*
 * Asynchronous outcome of a previously successful invoker start.  EVENT and
 * DONE enter the actor's external-event flow.  UNAVAILABLE retires the service
 * and appends error.communication.invoke.<id>.unavailable to the internal queue.
 */
typedef enum {
    TA_INVOKE_REPORT_EVENT = 0,
    TA_INVOKE_REPORT_DONE,
    TA_INVOKE_REPORT_UNAVAILABLE
} ta_invoke_report_kind TA_SYMBOLS("TA_INVOKE_REPORT_");

typedef struct {
    ta_actor_id actor;
    ta_invoke_id invoke_id;
    ta_invoke_report_kind kind;
    union {
        TA_WHEN(TA_INVOKE_REPORT_EVENT) TA_IN const ta_event *event;
        TA_WHEN(TA_INVOKE_REPORT_DONE) TA_IN TA_OPTIONAL const ta_value *done_data;
        TA_WHEN(TA_INVOKE_REPORT_UNAVAILABLE) TA_IN TA_OPTIONAL const ta_fault *fault;
    } as;
} ta_invoke_report_params TA_TAGGED(kind, as);

typedef struct ta_invoker_vtable {
    /* start must not report reentrantly through ta_actor_report_invoke(). */
    ta_op_status (*start)(void *self, TA_IN const ta_invoker_start_params *params);
    ta_op_status (*cancel)(void *self,
                           uint64_t service_handle,
                           TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault);
    ta_op_status (*forward)(void *self, TA_IN const ta_invoker_forward_params *params);
} ta_invoker_vtable TA_VTABLE;
typedef struct { void *self; const ta_invoker_vtable *v; } ta_invoker TA_ADAPTER;

/* Externally synchronize this call; do not call it reentrantly from start. */
TA_API ta_result ta_actor_report_invoke(ta_system *system,
                                        TA_IN const ta_invoke_report_params *params)
    TA_VALUE_MODEL(actor, params.actor);


typedef struct {
    ta_invoke_id id;
    ta_atom type;
    /* Runtime-only: session images do not retain or reconstruct this handle. */
    uint64_t service_handle;
    bool autoforward;
    bool active;
} ta_active_invoke;
/* ========================================================================
 * Definitions: declarative SCXML specifications
 */

/*
 * Declarative SCXML authoring tree.
 *
 * Every child list is terminated by a zero-initialized element: `{0}`. The
 * spec enums deliberately reserve zero for that terminator; do not use the
 * corresponding runtime enums here, because some valid runtime values are
 * zero. The compiler consumes the tree synchronously and retains no pointer
 * into it.
 */

/*
 * Storage for dynamically authored specification trees. A tree owns all
 * allocations made through it until ta_tree_destroy(). Its first allocation
 * failure is retained and reported by ta_tree_result(); subsequent writes are
 * ignored so import code can report one result at its outer boundary.
 */
typedef struct {
    ta_allocator allocator;
    ta_result result;
    void *impl;
} ta_tree;

/* A temporary, type-erased child list owned by a ta_tree. */
typedef struct {
    TA_INOUT ta_tree *tree;
    size_t element_size;
    size_t element_alignment;
    void *impl;
} ta_tree_list;

typedef struct {
    size_t element_size;
    size_t element_alignment;
} ta_tree_list_layout;

/**
 * Initialize storage used while dynamically constructing a specification.
 *
 * @code{.c}
 * ta_tree tree;
 * ta_tree_list states;
 * ta_tree_init(&tree, allocator);
 * ta_tree_list_init(&states, &tree,
 *                   &(ta_tree_list_layout){
 *                       .element_size = sizeof(ta_scxml_state_spec),
 *                       .element_alignment = _Alignof(ta_scxml_state_spec),
 *                   });
 *
 * ta_scxml_state_spec idle = {
 *     .kind = TA_SCXML_STATE_ATOMIC,
 *     .id = { .ptr = "idle", .len = 4 },
 * };
 * ta_tree_put(&states, &idle);
 *
 * const ta_scxml_state_spec *state_list = ta_tree_close(&states);
 * if (ta_tree_result(&tree) != TA_OK) {
 *     ta_tree_destroy(&tree);
 *     return TA_ERR_NOMEM;
 * }
 * @endcode
 */
TA_API void ta_tree_init(TA_OUT TA_RELEASE(ta_tree_destroy) ta_tree *tree, ta_allocator allocator);
TA_API void ta_tree_destroy(TA_INOUT ta_tree *tree);
TA_API void *ta_tree_alloc(TA_INOUT ta_tree *tree, size_t size, size_t alignment);
TA_API ta_result ta_tree_result(TA_IN const ta_tree *tree);

TA_API void ta_tree_list_init(TA_OUT TA_LIFETIME(input) ta_tree_list *list,
                              TA_INOUT ta_tree *tree,
                              TA_IN const ta_tree_list_layout *layout);
TA_API void ta_tree_put(TA_INOUT ta_tree_list *list, const void *element);
TA_API const void *ta_tree_close(TA_INOUT ta_tree_list *list);

typedef enum {
    TA_SCXML_VALUE_NONE = 0,
    TA_SCXML_VALUE_EXPR,
    TA_SCXML_VALUE_LOCATION,
    TA_SCXML_VALUE_CONTENT
} ta_scxml_value_kind TA_SYMBOLS("TA_SCXML_VALUE_");

typedef struct {
    ta_scxml_value_kind kind;
    union {
        TA_WHEN(TA_SCXML_VALUE_EXPR) ta_expression expr;
        TA_WHEN(TA_SCXML_VALUE_LOCATION) ta_expression location;
        /*
         * Literal <content> (ADR 0053). A data model imports it for <data>,
         * <assign>, <raise>, <donedata> and a <send> to the SCXML Event I/O
         * Processor; a host I/O processor and an invoker receive the bytes.
         */
        TA_WHEN(TA_SCXML_VALUE_CONTENT) struct {
            /* What these bytes are, as the codec that produced them knows it;
             * the decoding codec's info.name. Empty when nothing said. Opaque
             * to core. */
            ta_str media_type;
            ta_bytes bytes;
        } content;
    } as;
} ta_scxml_value_spec TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;

/*
 * An SCXML attribute pair of the form `x` / `xexpr`: either literal text
 * written by the author or an expression evaluated when the element executes.
 * A zero-initialized value is absent.
 */
typedef enum {
    TA_SCXML_STRING_NONE = 0,
    TA_SCXML_STRING_LITERAL,
    TA_SCXML_STRING_EXPR
} ta_scxml_string_kind TA_SYMBOLS("TA_SCXML_STRING_");

typedef struct {
    ta_scxml_string_kind kind;
    union {
        TA_WHEN(TA_SCXML_STRING_LITERAL) ta_str literal;
        TA_WHEN(TA_SCXML_STRING_EXPR) ta_expression expr;
    } as;
} ta_scxml_string_spec TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;

/*
 * The `id` / `idlocation` pair of <send> and <invoke>. A literal ID is the
 * author's string; LOCATION stores the ID the processor generates each time
 * the element executes. A zero-initialized value is absent, in which case
 * the processor still generates an ID but does not store it.
 */
typedef enum {
    TA_SCXML_ID_NONE = 0,
    TA_SCXML_ID_LITERAL,
    TA_SCXML_ID_LOCATION
} ta_scxml_id_kind TA_SYMBOLS("TA_SCXML_ID_");

typedef struct {
    ta_scxml_id_kind kind;
    union {
        TA_WHEN(TA_SCXML_ID_LITERAL) ta_str literal;
        TA_WHEN(TA_SCXML_ID_LOCATION) ta_expression location;
    } as;
} ta_scxml_id_spec TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;

typedef enum {
    TA_SCXML_EVENT_NONE = 0,
    TA_SCXML_EVENT_NAME,
    TA_SCXML_EVENT_EXPR
} ta_scxml_event_kind TA_SYMBOLS("TA_SCXML_EVENT_");

typedef struct {
    ta_scxml_event_kind kind;
    union {
        TA_WHEN(TA_SCXML_EVENT_NAME) ta_str name;
        TA_WHEN(TA_SCXML_EVENT_EXPR) ta_expression expr;
    } as;
} ta_scxml_event_spec TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;

typedef enum {
    TA_SCXML_ACTION_NONE = 0,
    TA_SCXML_ACTION_RAISE,
    TA_SCXML_ACTION_SEND,
    TA_SCXML_ACTION_CANCEL,
    TA_SCXML_ACTION_ASSIGN,
    TA_SCXML_ACTION_SCRIPT,
    TA_SCXML_ACTION_LOG,
    TA_SCXML_ACTION_IF,
    TA_SCXML_ACTION_FOREACH,
    TA_SCXML_ACTION_INVOKE
} ta_scxml_action_kind TA_SYMBOLS("TA_SCXML_ACTION_");

typedef enum {
    TA_SCXML_TRANSITION_NONE = 0,
    TA_SCXML_TRANSITION_EXTERNAL,
    TA_SCXML_TRANSITION_INTERNAL
} ta_scxml_transition_kind TA_SYMBOLS("TA_SCXML_TRANSITION_");

typedef enum {
    TA_SCXML_STATE_NONE = 0,
    TA_SCXML_STATE_ATOMIC,
    TA_SCXML_STATE_COMPOUND,
    TA_SCXML_STATE_PARALLEL,
    TA_SCXML_STATE_FINAL,
    TA_SCXML_STATE_HISTORY_SHALLOW,
    TA_SCXML_STATE_HISTORY_DEEP
} ta_scxml_state_kind TA_SYMBOLS("TA_SCXML_STATE_");

/* Zero selects the SCXML default, early binding. */
typedef enum {
    TA_SCXML_BINDING_DEFAULT = 0,
    TA_SCXML_BINDING_EARLY,
    TA_SCXML_BINDING_LATE
} ta_scxml_binding_kind TA_SYMBOLS("TA_SCXML_BINDING_");

typedef enum {
    TA_SCXML_INVOKE_NONE = 0,
    TA_SCXML_INVOKE
} ta_scxml_invoke_kind TA_SYMBOLS("TA_SCXML_");

typedef struct ta_scxml_action_spec ta_scxml_action_spec TA_TAGGED(kind, as) TA_ZERO_IS_ABSENT;
typedef struct ta_scxml_transition_spec ta_scxml_transition_spec TA_ZERO_IS_ABSENT;
typedef struct ta_scxml_state_spec ta_scxml_state_spec TA_ZERO_IS_ABSENT;

typedef struct {
    ta_str name;
    ta_scxml_value_spec value;
} ta_scxml_param_spec TA_ZERO_IS_ABSENT;

/*
 * `type` names a registered I/O processor (`type` / `typeexpr`); absent
 * selects the built-in SCXML Event I/O Processor. A literal `delay` is an
 * SCXML duration such as "500ms" or "5s" and is compiled to logical
 * milliseconds; a `delayexpr` is evaluated when the send executes. SCXML
 * `namelist` is lowered to `params`: each name becomes a parameter whose value
 * is a TA_SCXML_VALUE_LOCATION of the same name.
 */
typedef struct {
    ta_scxml_id_spec id;
    ta_scxml_string_spec type;
    ta_scxml_event_spec event;
    ta_scxml_string_spec target;
    ta_scxml_string_spec delay;
    ta_scxml_value_spec data;
    TA_TERMINATED_BY(name) TA_OPTIONAL const ta_scxml_param_spec *params;
} ta_scxml_send_spec TA_ZERO_IS_ABSENT;

/*
 * `type` names a registered invoker (`type` / `typeexpr`); `src` is the
 * `src` / `srcexpr` pair; `data` is the inline <content>. SCXML `namelist` is
 * lowered to `params` as for <send>.
 */
typedef struct {
    ta_scxml_invoke_kind kind;
    ta_scxml_id_spec id;
    ta_scxml_string_spec type;
    ta_scxml_string_spec src;
    ta_scxml_value_spec data;
    bool autoforward;
    TA_TERMINATED_BY(name) TA_OPTIONAL const ta_scxml_param_spec *params;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *finalize;
} ta_scxml_invoke_spec TA_ZERO_IS_ABSENT;

struct ta_scxml_action_spec {
    ta_scxml_action_kind kind;
    union {
        TA_WHEN(TA_SCXML_ACTION_RAISE) struct { ta_str event; ta_scxml_value_spec data; } raise;
        TA_WHEN(TA_SCXML_ACTION_SEND) ta_scxml_send_spec send;
        /* `sendid` / `sendidexpr`; exactly one is required. */
        TA_WHEN(TA_SCXML_ACTION_CANCEL) struct { ta_scxml_string_spec sendid; } cancel;
        TA_WHEN(TA_SCXML_ACTION_ASSIGN) struct { ta_expression location; ta_scxml_value_spec value; } assign;
        TA_WHEN(TA_SCXML_ACTION_SCRIPT) struct { ta_expression script; } script;
        /* Both `label` and `expr` are optional. */
        TA_WHEN(TA_SCXML_ACTION_LOG) struct { ta_str label; ta_expression expr; } log;
        TA_WHEN(TA_SCXML_ACTION_IF) struct {
            ta_expression condition;
            TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *then_actions;
            TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *else_actions;
        } conditional;
        TA_WHEN(TA_SCXML_ACTION_FOREACH) struct {
            ta_expression array;
            ta_str item;
            ta_str index;
            TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *body;
        } foreach;
        TA_WHEN(TA_SCXML_ACTION_INVOKE) ta_scxml_invoke_spec invoke;
    } as;
};

typedef struct {
    TA_TERMINATED_BY(ptr) TA_OPTIONAL const ta_str *targets;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *actions;
} ta_scxml_initial_spec TA_ZERO_IS_ABSENT;

struct ta_scxml_transition_spec {
    ta_scxml_transition_kind kind;
    ta_str event;
    ta_expression condition;
    TA_TERMINATED_BY(ptr) TA_OPTIONAL const ta_str *targets;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_action_spec *actions;
};

/*
 * One <onentry> or <onexit> handler. SCXML 3.8 and 3.9 require each handler
 * to be a separate block of executable content: when an action fails, the
 * rest of that handler is skipped, and the handlers after it still run. A
 * state lists its handlers in document order.
 *
 * `actions` is required, because a null `actions` terminates the handler
 * list. An empty handler (`<onentry/>`) points to an empty action list,
 * `(const ta_scxml_action_spec[]){{0}}`, or is left out.
 */
typedef struct {
    TA_TERMINATED_BY(kind) const ta_scxml_action_spec *actions;
} ta_scxml_handler_spec TA_ZERO_IS_ABSENT;

/*
 * SCXML `<data src>` is resolved by the importer, which performs any I/O and
 * passes the result as TA_SCXML_VALUE_CONTENT; the core performs no I/O.
 */
typedef struct {
    ta_str name;
    ta_scxml_value_spec initializer;
} ta_scxml_data_spec TA_ZERO_IS_ABSENT;

/* Exactly one of content or params may be present, as required by SCXML. */
typedef struct {
    ta_scxml_value_spec content;
    TA_TERMINATED_BY(name) TA_OPTIONAL const ta_scxml_param_spec *params;
} ta_scxml_done_data_spec TA_ZERO_IS_ABSENT;

struct ta_scxml_state_spec {
    ta_scxml_state_kind kind;
    ta_str id;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_state_spec *states;
    TA_IN TA_OPTIONAL const ta_scxml_initial_spec *initial;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_transition_spec *transitions;
    TA_TERMINATED_BY(actions) TA_OPTIONAL const ta_scxml_handler_spec *onentry;
    TA_TERMINATED_BY(actions) TA_OPTIONAL const ta_scxml_handler_spec *onexit;
    TA_TERMINATED_BY(name) TA_OPTIONAL const ta_scxml_data_spec *data;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_invoke_spec *invokes;
    TA_IN TA_OPTIONAL const ta_scxml_done_data_spec *done_data;
};

typedef struct {
    ta_str name;
    /*
     * The <scxml> `datamodel` attribute: the data model this document
     * requires. Absent means no requirement. The compiler returns
     * TA_ERR_UNSUPPORTED when it differs from the adapter's name.
     */
    ta_str datamodel;
    TA_IN TA_OPTIONAL const ta_definition_identity *identity;
    ta_scxml_binding_kind binding;
    TA_IN TA_OPTIONAL const ta_scxml_initial_spec *initial;
    TA_TERMINATED_BY(kind) TA_OPTIONAL const ta_scxml_state_spec *states;
    TA_TERMINATED_BY(name) TA_OPTIONAL const ta_scxml_data_spec *data;
    /*
     * The <scxml> <script> child (SCXML 3.2.2, 5.8), absent when zero. Each
     * session runs it once at initialization: after early-bound data is
     * initialized and before the initial transition is taken. A runtime
     * failure queues error.execution. <script src> is fetched by the importer
     * and passed as source text; the core performs no I/O.
     */
    ta_expression script;
} ta_scxml_spec TA_ZERO_IS_ABSENT;

/**
 * Compile a sentinel-terminated SCXML specification into an immutable
 * definition.
 *
 * The compiler resolves state IDs and event descriptors, interns named send
 * events, I/O processor and invoker types, and literal send and invoke IDs,
 * validates the complete tree, and copies all data it retains. Thus the
 * specification may be a nested compound literal with automatic storage.
 * Every optional field is absent when zero-initialized; no field takes a
 * runtime identifier such as `ta_expr_id`, `ta_send_id`, or `ta_atom`.
 *
 * @code{.c}
 * // These expressions can carry source text or host-language handles.
 * ta_expression request_reply_to, unauthorized_response, authorization_redirect;
 * ta_expression callback_state_is_valid, token_exchange_request, callback_reply_to;
 * ta_expression invalid_callback_response, access_token, event_access_token;
 * ta_expression login_complete_response, token_failure_response;
 * ta_expression authorized_response, no_access_token;
 * // Names under which the embedder registered its I/O processors.
 * ta_scxml_string_spec http_io = { .kind = TA_SCXML_STRING_LITERAL,
 *     .as.literal = { .ptr = "http-response", .len = 13 } };
 * ta_scxml_string_spec oauth_io = { .kind = TA_SCXML_STRING_LITERAL,
 *     .as.literal = { .ptr = "oauth-token-client", .len = 18 } };
 * ta_result result = ta_definition_compile_spec(
 *     system,
 *     &(ta_definition_compile_params){
 *       .spec = &(ta_scxml_spec){
 *         .name = { .ptr = "oauth-login", .len = 11 },
 *         .initial = &(ta_scxml_initial_spec){
 *             .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *         },
 *         .states = (const ta_scxml_state_spec[]){
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "idle", .len = 4 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     // A protected request with no session receives 401.
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_INTERNAL,
 *                         .event = { .ptr = "http.resource", .len = 13 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = unauthorized_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "login.requested", .len = 15 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             // Generates state + PKCE data and sends 302.
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = authorization_redirect },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "authorizing", .len = 11 }, {0}},
 *                     },
 *                     {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "authorizing", .len = 11 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.callback", .len = 14 },
 *                         .condition = callback_state_is_valid,
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = oauth_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "oauth.token.exchange", .len = 20 } },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = token_exchange_request },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "exchanging", .len = 10 }, {0}},
 *                     },
 *                     // A bad callback is answered; it is not left pending.
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.callback", .len = 14 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = invalid_callback_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     },
 *                     {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "exchanging", .len = 10 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.token.success", .len = 19 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_ASSIGN,
 *                                 .as.assign = { .location = access_token,
 *                                     .value = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = event_access_token } },
 *                             },
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = login_complete_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "authenticated", .len = 13 }, {0}},
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "oauth.token.failure", .len = 19 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = callback_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = token_failure_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     }, {0},
 *                 },
 *             },
 *             {
 *                 .kind = TA_SCXML_STATE_ATOMIC,
 *                 .id = { .ptr = "authenticated", .len = 13 },
 *                 .transitions = (const ta_scxml_transition_spec[]){
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_INTERNAL,
 *                         .event = { .ptr = "http.resource", .len = 13 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_SEND,
 *                                 .as.send = {
 *                                     .type = http_io,
 *                                     .event = { .kind = TA_SCXML_EVENT_NAME,
 *                                         .as.name = { .ptr = "http.response", .len = 13 } },
 *                                     .target = { .kind = TA_SCXML_STRING_EXPR,
 *                                         .as.expr = request_reply_to },
 *                                     .data = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = authorized_response },
 *                                 },
 *                             }, {0},
 *                         },
 *                     },
 *                     {
 *                         .kind = TA_SCXML_TRANSITION_EXTERNAL,
 *                         .event = { .ptr = "logout.requested", .len = 16 },
 *                         .condition = {0},
 *                         .actions = (const ta_scxml_action_spec[]){
 *                             {
 *                                 .kind = TA_SCXML_ACTION_ASSIGN,
 *                                 .as.assign = { .location = access_token,
 *                                     .value = { .kind = TA_SCXML_VALUE_EXPR,
 *                                         .as.expr = no_access_token } },
 *                             }, {0},
 *                         },
 *                         .targets = (const ta_str[]){{ .ptr = "idle", .len = 4 }, {0}},
 *                     }, {0},
 *                 },
 *             },
 *             {0},
 *         },
 *       },
 *         .datamodel = datamodel,
 *         .allocator = allocator,
 *         .out = &definition,
 *     });
 * @endcode
 */
/*
 * Compile errors (ADR 0058)
 *
 * A refused spec is described by what is wrong (the code and message), where
 * in the spec (a path of steps from the root), and which authored text is at
 * fault (`subject`, the spec's own pointer, never a copy). When the spec came
 * from a codec that keeps text as slices of its input, `subject` points into
 * that input, which is how ta_definition_import() turns it into a line and
 * column.
 *
 * One step from a spec's root toward the element an error is about: a member
 * of the element reached so far and, for a list member, the index in it.
 */
typedef enum {
    TA_SPEC_MEMBER_NONE = 0,
    /* Lists. */
    TA_SPEC_MEMBER_STATES,
    TA_SPEC_MEMBER_TRANSITIONS,
    TA_SPEC_MEMBER_TARGETS,
    TA_SPEC_MEMBER_ONENTRY,
    TA_SPEC_MEMBER_ONEXIT,
    TA_SPEC_MEMBER_ACTIONS,
    TA_SPEC_MEMBER_THEN,
    TA_SPEC_MEMBER_ELSE,
    TA_SPEC_MEMBER_BODY,
    TA_SPEC_MEMBER_FINALIZE,
    TA_SPEC_MEMBER_INVOKES,
    TA_SPEC_MEMBER_DATA,
    TA_SPEC_MEMBER_PARAMS,
    /* Single members. */
    TA_SPEC_MEMBER_INITIAL,
    TA_SPEC_MEMBER_DONE_DATA,
    TA_SPEC_MEMBER_SCRIPT,
    TA_SPEC_MEMBER_ID,
    TA_SPEC_MEMBER_NAME,
    TA_SPEC_MEMBER_EVENT,
    TA_SPEC_MEMBER_CONDITION,
    TA_SPEC_MEMBER_TYPE,
    TA_SPEC_MEMBER_SRC,
    TA_SPEC_MEMBER_TARGET,
    TA_SPEC_MEMBER_DELAY,
    TA_SPEC_MEMBER_DATA_VALUE,
    TA_SPEC_MEMBER_LOCATION,
    TA_SPEC_MEMBER_EXPR,
    TA_SPEC_MEMBER_ITEM,
    TA_SPEC_MEMBER_INDEX,
    TA_SPEC_MEMBER_SENDID,
    TA_SPEC_MEMBER_DATAMODEL,
    TA_SPEC_MEMBER_IDENTITY
} ta_spec_member TA_SYMBOLS("TA_SPEC_MEMBER_");

/* The index of a step into a single member. */
#define TA_SPEC_INDEX_NONE ((uint32_t)UINT32_MAX)

typedef struct {
    ta_spec_member member;
    uint32_t index;
} ta_spec_step;

typedef enum {
    TA_COMPILE_ERROR_NONE = 0,
    TA_COMPILE_ERROR_MALFORMED_SPEC,        /* a tag out of range; a required pointer absent */
    TA_COMPILE_ERROR_MISSING_VALUE,         /* required text or expression absent or empty */
    TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS,     /* params with content; src with content */
    TA_COMPILE_ERROR_DUPLICATE_STATE,
    TA_COMPILE_ERROR_UNKNOWN_STATE,         /* a target or initial names no state */
    TA_COMPILE_ERROR_TARGET_OUTSIDE,        /* an initial or history target outside its owner */
    TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR,
    TA_COMPILE_ERROR_BAD_DURATION,
    TA_COMPILE_ERROR_BAD_STRUCTURE,         /* children, <initial> or <donedata> where the kind forbids */
    TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
    TA_COMPILE_ERROR_NO_STATES,
    TA_COMPILE_ERROR_NO_FAMILY,
    TA_COMPILE_ERROR_DATAMODEL_MISMATCH,
    TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, /* TA_SCXML_ACTION_INVOKE */
    TA_COMPILE_ERROR_LIMIT,                 /* a bound core enforces was reached */
    TA_COMPILE_ERROR_HOST_EXPRESSION,       /* a host expression's retain() failed */
    TA_COMPILE_ERROR_EXPRESSION_REJECTED    /* the data model refused an expression */
} ta_compile_error_code TA_SYMBOLS("TA_COMPILE_ERROR_");

typedef struct {
    ta_compile_error_code code;
    /* From the spec's root to the element at fault, in the error arena.
     * Empty when no arena was given or the error is about the whole chart. */
    TA_COUNTED_BY(path_length) TA_OPTIONAL const ta_spec_step *path;
    uint32_t path_length;
    /* The authored text at fault, borrowed from the spec exactly as given:
     * the unknown target, the duplicate ID, the bad duration, the refused
     * expression's source. Empty when the error is not about text. */
    ta_str subject;
    /* Within `subject`, the bytes at fault when narrower than all of it: an
     * event descriptor's empty segment, or the range a data model reported.
     * Both zero when all of `subject` is meant. */
    uint32_t subject_offset;
    uint32_t subject_length;
    /* A second place the error involves, borrowed from the spec: the first
     * state with a duplicate ID, the state a target must lie within, or the
     * existing state ID nearest to an unknown target. Empty when none. */
    ta_str related;
    /* Why, in words: core's fixed phrase, or the data model's message copied
     * into the error arena (empty without one). */
    ta_str message;
} ta_compile_error;

typedef struct {
    TA_IN const ta_scxml_spec *spec;
    ta_datamodel datamodel;
    ta_allocator allocator;
    ta_definition **out;
    /*
     * Written after every result. When the result is TA_ERR_INVALID or
     * TA_ERR_UNSUPPORTED because of the spec, it holds the first fault in
     * compile order (ADR 0058 section 2). Success, argument errors and
     * TA_ERR_NOMEM leave `code` TA_COMPILE_ERROR_NONE.
     * Borrows from `spec` and `error_arena`.
     */
    TA_OUT TA_OPTIONAL TA_ALWAYS TA_LIFETIME(input) ta_compile_error *error;
    /* Receives the error's path and any copied message; untouched on
     * success. Without one, `error` has no path and no data-model message. */
    TA_OPTIONAL TA_ARENA ta_arena *error_arena;
} ta_definition_compile_params;

TA_API ta_result ta_definition_compile_spec(ta_system *system,
                                            TA_IN const ta_definition_compile_params *params);
TA_API void ta_definition_destroy(TA_DESTROYS ta_definition *definition);

/*
 * Writes a compile error as one line of text, for people. The line has no
 * stable grammar; tools read the struct. It is never longer than
 * TA_COMPILE_ERROR_TEXT_MAX bytes, however deep the path or long the text:
 * - a path of more than 8 steps shows its first 2 and last 6 steps, and says
 *   how many it left out;
 * - each ID, subject, related text and message is cut at a UTF-8 boundary
 *   and ends in "..." when it is too long;
 * - control characters are escaped everywhere, and quotes and backslashes
 *   are escaped inside the quoted subject and related text, so the line
 *   stays one line.
 * A state step shows its ID:
 *   states[2](idle).transitions[0].targets[0]: unknown state "idel"; nearest is "idle"
 * `spec` is the spec the error came from; it may be null, and then no IDs
 * are shown.
 */
typedef struct {
    TA_IN TA_OPTIONAL const ta_scxml_spec *spec;
    TA_IN const ta_compile_error *error;
    TA_ARENA ta_arena *arena;
    TA_OUT TA_LIFETIME(arena) ta_str *out;
} ta_compile_error_format_params;
#define TA_COMPILE_ERROR_TEXT_MAX ((uint32_t)1024)
TA_API ta_result ta_compile_error_format(TA_IN const ta_compile_error_format_params *params);

/*
 * Rebuilds the author-level spec of a registered definition (ADR 0042).
 * Arrays and joined event names are allocated in `arena`; other text and
 * expressions are borrowed from the definition and the system. Host
 * expressions are the definition's retained handles, not retained again.
 * The result stays valid until the arena is reset or the definition is
 * freed, whichever comes first.
 *
 * Export is faithful: compiling the exported spec yields the same revision
 * as the original. `identity` is null when the definition was fingerprinted
 * and points to the definition's explicit identity otherwise. Empty lists are
 * exported as null, except that a handler's `actions` is never null.
 *
 * TA_ERR_NOT_FOUND: no existing definition has this ID. A retired definition
 * exports until it is freed.
 */
typedef struct {
    ta_definition_id definition;
    TA_ARENA ta_arena *arena;
    TA_VIEW_OUT TA_LIFETIME(arena) const ta_scxml_spec **out;
} ta_definition_export_params;
TA_API ta_result ta_definition_export(const ta_system *system,
                                      TA_IN const ta_definition_export_params *params);

/* ========================================================================
 * Chart codecs (ADR 0053)
 *
 * A codec turns bytes into a ta_scxml_spec and back. Core defines the
 * adapter and the in-memory form and links no parser: SCXML XML, a JSON
 * encoding and a tree-sitter grammar are all codecs, and none is in core.
 *
 * Core calls a codec only from ta_definition_import() and
 * ta_definition_encode(). An embedder may also decode and then compile with
 * ta_definition_compile_spec() itself.
 */

typedef struct { uint64_t offset; uint32_t line; uint32_t column; } ta_chart_position;

typedef enum {
    TA_CHART_ERROR_NONE = 0,
    TA_CHART_ERROR_SYNTAX,                /* not well-formed in this codec's syntax */
    TA_CHART_ERROR_NOT_CHART,             /* well-formed, but not a chart */
    TA_CHART_ERROR_UNKNOWN_CONSTRUCT,
    TA_CHART_ERROR_MISPLACED_CONSTRUCT,   /* <else> before an <elseif>; <param> with <content> */
    TA_CHART_ERROR_UNKNOWN_ATTRIBUTE,
    TA_CHART_ERROR_MISSING_ATTRIBUTE,
    TA_CHART_ERROR_CONFLICTING_ATTRIBUTE, /* both `event` and `eventexpr` */
    TA_CHART_ERROR_BAD_VALUE,             /* a duration, a boolean, an ID list */
    TA_CHART_ERROR_FOREIGN_CONTENT,       /* SCXML 4.10 content, no lowering (ADR 0046) */
    TA_CHART_ERROR_SRC_UNRESOLVED,        /* a src= and no resolver */
    TA_CHART_ERROR_LIMIT,                 /* a bound this codec enforces was reached */
    TA_CHART_ERROR_UNREPRESENTABLE        /* encode: the spec has no form in this syntax */
} ta_chart_error_code TA_SYMBOLS("TA_CHART_ERROR_");

typedef struct {
    ta_chart_error_code code;
    ta_chart_position at;
    ta_str construct;  /* the element or production the error is about */
    ta_str attribute;  /* empty when it is not about one */
    ta_str message;    /* a fixed phrase owned by the codec */
} ta_chart_error;

enum TA_FLAG_SET(ta_chart_codec_flags, "TA_CHART_CODEC_") {
    TA_CHART_CODEC_DECODES = 1u << 0,
    TA_CHART_CODEC_ENCODES = 1u << 1
};

/*
 * What a codec is, for a host choosing one and a tool reporting what it did.
 * Every field is fixed for the codec's lifetime.
 */
typedef struct {
    /* The representation this codec reads and writes: the registered media
     * type where one exists ("application/scxml+xml"), otherwise a stable
     * identifier of the implementer's choosing. It is also what a decode
     * writes into a literal content's media_type. */
    ta_str name;
    /* Conventional file extensions without the dot, terminated by an empty
     * one; absent when the representation has none. */
    TA_TERMINATED_BY(ptr) TA_OPTIONAL const ta_str *extensions;
    /* One line, for a tool's --help. */
    ta_str description;
    uint32_t flags TA_FLAGS(ta_chart_codec_flags);
} ta_chart_codec_info;

/*
 * Resolves <data src> and <script src> during a decode: the one route to the
 * outside world, and the embedder's, never core's and never the codec's. For
 * <script> the bytes are source text. TA_ERR_NOT_FOUND when `src` names
 * nothing the resolver knows. An <invoke src> is not fetched: it is a URI the
 * invoker interprets at run time (SCXML 6.4), and stays in the spec.
 */
typedef struct ta_chart_src_resolver_vtable {
    ta_result (*fetch)(void *self, ta_str src, TA_ARENA ta_arena *arena,
                       TA_OUT TA_LIFETIME(arena) ta_bytes *out);
} ta_chart_src_resolver_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_src_resolver_vtable *v; } ta_chart_src_resolver TA_ADAPTER;

/*
 * Lowers SCXML 4.10 platform-specific executable content to a <send> to a
 * registered I/O processor, the only form core has for it (ADR 0046).
 * `element` is the element as written. Returning TA_ERR_NOT_FOUND refuses
 * the document, which SCXML A.2 permits. An absent adapter refuses every
 * such element: skipping one would silently drop behaviour the author wrote.
 */
typedef struct ta_chart_lowering_vtable {
    ta_result (*lower)(void *self,
                       TA_IN const ta_chart_position *at,
                       ta_bytes element,
                       TA_ARENA ta_arena *arena,
                       TA_OUT TA_LIFETIME(arena) ta_scxml_send_spec *out);
} ta_chart_lowering_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_lowering_vtable *v; } ta_chart_lowering TA_ADAPTER;

/*
 * Renders TA_EXPRESSION_HOST back to source during an encode. A spec decoded
 * from a document carries only TA_EXPRESSION_SOURCE and needs none; a spec
 * built from host handles is TA_CHART_ERROR_UNREPRESENTABLE without it.
 */
typedef struct ta_chart_expression_printer_vtable {
    ta_result (*print)(void *self, TA_IN const ta_expression_host *host,
                       TA_ARENA ta_arena *arena, TA_OUT TA_LIFETIME(arena) ta_str *out);
} ta_chart_expression_printer_vtable TA_VTABLE;
typedef struct { void *self; TA_OPTIONAL const ta_chart_expression_printer_vtable *v; } ta_chart_expression_printer TA_ADAPTER;

typedef struct {
    ta_bytes input;
    /*
     * The operation's arena. Everything the decode allocates lives here — the
     * codec's own parse, the spec, and the transients in between — and
     * nothing outlives it. Reset or destroy it once the spec is compiled.
     */
    TA_ARENA ta_arena *arena;
    ta_chart_src_resolver resolver;
    ta_chart_lowering lowering;
    /* No chart syntax has a form for a definition's identity, so it is
     * supplied here or not at all; absent means the compiler fingerprints. */
    TA_IN TA_OPTIONAL const ta_definition_identity *identity;
    TA_VIEW_OUT TA_LIFETIME(arena) const ta_scxml_spec **out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) TA_LIFETIME(arena) ta_chart_error *error;
} ta_chart_decode_params;

typedef struct {
    TA_IN const ta_scxml_spec *spec;
    /* The operation's arena: the output bytes and everything the encode
     * needed to produce them. */
    TA_ARENA ta_arena *arena;
    ta_chart_expression_printer expressions;
    TA_OUT TA_LIFETIME(arena) ta_bytes *out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) TA_LIFETIME(arena) ta_chart_error *error;
} ta_chart_encode_params;

typedef struct ta_chart_codec_vtable {
    ta_chart_codec_info info;
    /*
     * Decode `input` into a spec.
     *
     * **The spec may point into `input`.** A codec is not required to copy,
     * because a codec over a syntax tree keeps expression text as exact
     * slices of the source, which is what lets an embedder map a compiled
     * expression back to the construct it came from and run static checks on
     * it before compiling. So `input` must stay valid as long as the spec
     * does — until ta_definition_compile_spec() has copied it. Callers keep
     * the buffer; codecs that do copy make that harmless.
     *
     * A codec bounds its own input: nesting depth, document size, expansion.
     * Reaching one of its bounds is TA_CHART_ERROR_LIMIT. The interface sets
     * no bound of its own, so two codecs need not accept the same documents.
     *
     * A literal <content> is decoded to bytes plus this codec's `info.name`
     * as their media type. What those bytes are is the codec's rule: an XML
     * codec serializes the content subtree with namespaces resolved as SCXML
     * G.6 requires, rather than slicing the source, because what a recipient
     * must see is a re-serialization and not the author's bytes.
     *
     * TA_OK, TA_ERR_INVALID with `error` saying why, or TA_ERR_NOMEM. Null
     * when this codec only writes.
     */
    TA_OPTIONAL ta_result (*decode)(void *self, TA_IN const ta_chart_decode_params *params);
    /*
     * Write `spec` in this codec's syntax. `spec->identity` is not written:
     * identity has no chart syntax, so a round trip that must preserve an
     * explicit one carries it beside the document and feeds it back through
     * ta_chart_decode_params. A literal content whose media type is not this
     * codec's own is TA_CHART_ERROR_UNREPRESENTABLE unless the codec can
     * carry it verbatim. Null when this codec only reads.
     */
    TA_OPTIONAL ta_result (*encode)(void *self, TA_IN const ta_chart_encode_params *params);
} ta_chart_codec_vtable TA_VTABLE;

typedef struct { void *self; const ta_chart_codec_vtable *v; } ta_chart_codec TA_ADAPTER;

/* Which stage of ta_definition_import() failed. */
typedef enum {
    TA_IMPORT_STAGE_NONE = 0,
    TA_IMPORT_STAGE_DECODE,
    TA_IMPORT_STAGE_COMPILE
} ta_import_stage TA_SYMBOLS("TA_IMPORT_STAGE_");

typedef struct {
    ta_import_stage stage;
    union {
        TA_WHEN(TA_IMPORT_STAGE_DECODE) ta_chart_error decode;
        TA_WHEN(TA_IMPORT_STAGE_COMPILE) ta_compile_error compile; /* ADR 0058 */
    } as;
    /* Whether `at` says where in `input` the error is. A decode error always
     * has a position. A compile error has one when its subject, or the
     * nearest element above it with text, lies within `input` (ADR 0058). */
    bool located;
    ta_chart_position at;
} ta_import_error TA_TAGGED(stage, as);

/*
 * Decodes `input` with `codec`, compiles the spec, and registers the
 * definition (ADR 0053 section 4). `out` is the new definition's ID.
 *
 * The spec may point into `input` only until it is compiled, which happens
 * inside this call, so `input` need only outlive the call, and `error` when
 * it is read. `identity` is given to the codec and set on the spec: no chart
 * syntax carries one. If registration fails, the compiled definition is
 * destroyed here; the caller never holds an unregistered one.
 *
 * TA_ERR_UNSUPPORTED when the codec cannot decode (its `decode` is null), or
 * when the compiler refuses the spec's data model. TA_ERR_INVALID with
 * `error` saying which stage refused the chart. TA_ERR_NOMEM.
 */
typedef struct {
    ta_chart_codec codec;
    ta_bytes input;
    /* The operation's arena: the decode, its transients, and anything
     * `error` refers to. Core does not reset it. Reset it once the error has
     * been read. */
    TA_ARENA ta_arena *arena;
    ta_chart_src_resolver resolver;
    ta_chart_lowering lowering;
    TA_IN TA_OPTIONAL const ta_definition_identity *identity;
    ta_datamodel datamodel;
    ta_allocator allocator;
    TA_OUT ta_definition_id *out;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) TA_LIFETIME(arena) ta_import_error *error;
} ta_definition_import_params;

TA_API ta_result ta_definition_import(ta_system *system,
                                      TA_IN const ta_definition_import_params *params);

/*
 * Writes a registered definition in `codec`'s syntax: ta_definition_export()
 * followed by the codec's `encode` (ADR 0053 section 4). `identity`, when
 * asked for, is the definition's explicit identity, which no chart syntax
 * carries, or null when the definition was fingerprinted: feed it back as
 * ta_definition_import()'s `identity` and the import has the same revision.
 * A retired definition encodes until it is freed, as it exports.
 *
 * TA_ERR_UNSUPPORTED when the codec cannot encode (its `encode` is null).
 * TA_ERR_NOT_FOUND when no definition has the ID. TA_ERR_INVALID with
 * `error` from the codec, for example TA_CHART_ERROR_UNREPRESENTABLE.
 */
typedef struct {
    ta_definition_id definition;
    ta_chart_codec codec;
    /* Holds the exported spec, the output and the codec's transients. */
    TA_ARENA ta_arena *arena;
    ta_chart_expression_printer expressions;
    TA_OUT TA_LIFETIME(arena) ta_bytes *out;
    TA_VIEW_OUT TA_OPTIONAL TA_LIFETIME(definition) const ta_definition_identity **identity;
    TA_OUT TA_OPTIONAL TA_VALID_ON(TA_ERR_INVALID) TA_LIFETIME(arena) ta_chart_error *error;
} ta_definition_encode_params;

TA_API ta_result ta_definition_encode(const ta_system *system,
                                      TA_IN const ta_definition_encode_params *params);


/* ========================================================================
 * Actors
 */

typedef enum {
    TA_ACTOR_UNLOADED,
    TA_ACTOR_IDLE,
    TA_ACTOR_RUNNING,
    TA_ACTOR_DONE
} ta_actor_residency TA_SYMBOLS("TA_ACTOR_");

/* The kind of a compiled state, as reported by ta_definition_state(). */
typedef enum {
    TA_STATE_ATOMIC,
    TA_STATE_COMPOUND,
    TA_STATE_PARALLEL,
    TA_STATE_FINAL,
    TA_STATE_HISTORY_SHALLOW,
    TA_STATE_HISTORY_DEEP
} ta_state_kind TA_SYMBOLS("TA_STATE_");

typedef enum {
    TA_ACTOR_DIRECTORY_NONE = 0,
    TA_ACTOR_DIRECTORY_RESIDENT,
    TA_ACTOR_DIRECTORY_UNLOADED
} ta_actor_directory_entry_kind TA_SYMBOLS("TA_ACTOR_DIRECTORY_");

typedef struct {
    ta_actor_directory_entry_kind kind;
    /* Meaningful only for TA_ACTOR_DIRECTORY_RESIDENT. */
    ta_actor_id actor;
} ta_actor_directory_entry;
typedef struct { ta_address *address; ta_actor_directory_entry entry; } ta_actor_directory_claim_params;
typedef struct { ta_address *address; TA_OUT ta_actor_directory_entry *out; } ta_actor_directory_resolve_params;
typedef struct {
    ta_address *address;
    ta_actor_directory_entry expected;
    ta_actor_directory_entry replacement;
} ta_actor_directory_replace_params;
typedef struct {
    ta_address *address;
    ta_actor_directory_entry expected;
} ta_actor_directory_remove_params;
/*
 * The directory is keyed by address handle (ADR 0065): equal addresses are
 * the same pointer, so a directory can key its map by the pointer. The
 * handle is borrowed for the call; a directory that keeps an entry retains
 * the address (ta_address_retain) and releases it when the entry goes.
 */
typedef struct ta_actor_directory_vtable {
    /* claim succeeds only when the address has no entry. */
    ta_result (*claim)(void *self, TA_IN const ta_actor_directory_claim_params *params);
    ta_result (*resolve)(void *self, TA_IN const ta_actor_directory_resolve_params *params);
    /* replace/remove are atomic comparisons against params->expected. */
    ta_result (*replace)(void *self, TA_IN const ta_actor_directory_replace_params *params);
    ta_result (*remove)(void *self, TA_IN const ta_actor_directory_remove_params *params);
} ta_actor_directory_vtable TA_VTABLE;
typedef struct { void *self; const ta_actor_directory_vtable *v; } ta_actor_directory TA_ADAPTER;

typedef struct {
    ta_actor_id actor;
    ta_invoke_id invoke_id;
} ta_actor_parent;

/*
 * `trace` is what the bootstrap macrostep runs under (ADR 0067): the trace
 * context, and the cause, the key of the step that spawned it or the
 * host's parent span. All zero is none.
 */
typedef struct {
    ta_definition_id definition;
    TA_IN TA_OPTIONAL const ta_actor_address *address;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_IN TA_OPTIONAL const ta_actor_parent *parent;
    TA_OUT ta_actor_id *out;
    ta_trace_parent trace;
} ta_actor_spawn_params;
typedef struct { TA_IN const ta_actor_address *address; TA_OUT ta_actor_id *out; } ta_actor_find_params;
/*
 * `budget` is the number of microsteps this call may run (ADR 0009).
 * `action_budget`, when not zero, is the number of units of executable
 * content it may run before it pauses inside a microstep (ADR 0061): each
 * action of a block, an <if> and a <foreach> included, and each <foreach>
 * iteration. Either one reached answers TA_RUN_PAUSED, and the next call
 * continues where this one stopped. Zero leaves pausing to `budget` alone.
 * A pause that cannot save its position keeps running to the next unit.
 */
typedef struct {
    ta_actor_id actor;
    uint32_t budget;
    TA_OUT ta_run_status *status;
    uint32_t action_budget;
} ta_actor_resume_params;
typedef struct {
    ta_actor_id actor;
    const ta_session_image *image;
} ta_actor_unload_params;
typedef struct { TA_IN const ta_actor_address *address; } ta_actor_remove_unloaded_params;
typedef struct {
    const ta_session_image *image;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_OUT ta_actor_id *out;
} ta_actor_load_params;
typedef struct {
    const ta_session_image *image;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_OUT ta_actor_id *out;
} ta_actor_reconstruct_params;
typedef struct {
    ta_actor_id incumbent;
    ta_definition_id definition;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_OUT ta_actor_id *out;
} ta_actor_prepare_replacement_params;
typedef struct { ta_actor_id incumbent; ta_actor_id replacement; } ta_actor_activate_replacement_params;
/* Release data with ta_value_release() when has_data is true. */
typedef struct { bool has_data; TA_RELEASE_IF(ta_value_release, has_data) ta_value data; } ta_actor_completion;

TA_API ta_result ta_actor_spawn(ta_system *system, TA_IN const ta_actor_spawn_params *params)
    TA_VALUE_MODEL(definition, params.definition);
/* Creates an unpublished actor and holds bootstrap effects until activation. */
TA_API ta_result ta_actor_prepare_replacement(ta_system *system,
                                              TA_IN const ta_actor_prepare_replacement_params *params)
    TA_VALUE_MODEL(definition, params.definition);
/* Atomically replaces the route, then releases held effects in SCXML order. */
TA_API ta_result ta_actor_activate_replacement(ta_system *system,
                                               TA_IN const ta_actor_activate_replacement_params *params);
TA_API ta_result ta_actor_find(ta_system *system, TA_IN const ta_actor_find_params *params);
typedef struct {
    ta_actor_id actor;
    TA_IN const ta_event *event;
    /* What the macrostep that takes the event runs under (ADR 0067); a
     * router passes ta_external_delivery.trace. All zero is none. */
    ta_trace_parent trace;
} ta_actor_deliver_params;
/*
 * Offers an external event to a resident actor: the admission test
 * (ADR 0055). The result alone tells a host what to do with the message,
 * so it never needs to inspect the actor first:
 * - TA_OK: accepted; the actor holds the event, so resume it.
 * - TA_ERR_BUSY: the actor is inside a macrostep, including one paused by
 *   ta_actor_resume()'s budgets (ADR 0061), or already holds an event.
 *   Keep the message queued for this actor.
 * - TA_ERR_DONE: the actor has reached a top-level final state. Drop the
 *   message or route it to a dead-letter policy.
 * - TA_ERR_NOT_FOUND: no resident actor has this ID (unknown, stale or
 *   unloaded). The receiver is gone from here; drop or route the message.
 * - TA_ERR_INVALID: a caller error only: a null event, an event name this
 *   system never issued, or the root name.
 * - TA_ERR_NOMEM: no memory for the event block the event would live in
 *   until its macrostep ends (ADR 0069). Keep the message and try again
 *   later; ta_system_set_event_block_cache() keeps blocks for reuse.
 * On any result but TA_OK nothing is retained and the event is untouched.
 * A malformed event is TA_ERR_INVALID whatever the actor's state, and a
 * finished actor is TA_ERR_DONE, never TA_ERR_BUSY: finishing is permanent,
 * so a host that kept a message queued would wait forever.
 */
TA_API ta_result ta_actor_deliver(ta_system *system, TA_IN const ta_actor_deliver_params *params)
    TA_VALUE_MODEL(actor, params.actor);
TA_API ta_result ta_actor_resume(ta_system *system, TA_IN const ta_actor_resume_params *params);
/*
 * Evicts a named actor without terminating it. The image must be a fresh
 * capture of actor; TA_ERR_STALE means the actor changed after capture.
 */
TA_API ta_result ta_actor_unload(ta_system *system, TA_IN const ta_actor_unload_params *params);
/* Restores an unloaded named actor and atomically republishes its route. */
TA_API ta_result ta_actor_load(ta_system *system, TA_IN const ta_actor_load_params *params)
    TA_VALUE_MODEL(session_image, params.image);
/*
 * Reconstructs an image under a fresh runtime ID without reading or changing
 * the actor directory. For a named image, the logical self-address is kept;
 * sends to that address follow the directory's current route. The image's
 * session continues, keeping its session ID, unless a resident session holds
 * that ID; then this is a new session (ADR 0066). ta_actor_load() always
 * continues the session.
 */
TA_API ta_result ta_actor_reconstruct(ta_system *system,
                                      TA_IN const ta_actor_reconstruct_params *params)
    TA_VALUE_MODEL(session_image, params.image);
/* Releases the name of an unloaded actor; the host then deletes its checkpoint. */
TA_API ta_result ta_actor_remove_unloaded(ta_system *system,
                                          TA_IN const ta_actor_remove_unloaded_params *params);
TA_API ta_result ta_actor_get_completion(ta_system *system,
                                         ta_actor_id actor,
                                         TA_OUT ta_actor_completion *out);
/*
 * Retries an invoked child's completion after TA_ERR_BUSY. TA_ERR_NOT_FOUND
 * retires it when the parent invocation was already cancelled.
 */
TA_API ta_result ta_actor_publish_completion(ta_system *system, ta_actor_id actor);
TA_API ta_result ta_actor_destroy(ta_system *system, ta_actor_id actor);



/*
 * One pending delayed send owned by an actor (ADR 0054). `destination` is the
 * resolved target for the built-in SCXML processor; custom processors
 * receive `target`, or a literal target's text, and leave `destination`
 * unused. `has_target_text` means the target is literal: `target` is absent,
 * and `target_text` names the text in the owner's definition, which
 * ta_definition_content() reads. `submitted` means the
 * router refused the send once and it waits to be offered again (ADR 0026,
 * ADR 0040).
 * Everything is borrowed; retain `target` and `data` with ta_value_retain()
 * to keep them past the synchronization window.
 */
typedef struct {
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_value target;
    ta_value data;
    bool submitted;
    bool has_target_text;
    ta_content_id target_text;
} ta_delayed_send_info;

/*
 * A read-only operational view of one resident interpreter instance.
 *
 * Fixed-size facts are fields; the internal queue, the invocations and the
 * delayed sends are borrowed arrays; the active configuration and history
 * are read with the ta_actor_* list functions below, whose counts are
 * reported here so a host can size buffers once. Inspecting one actor costs
 * O(size of that actor), never O(size of the system) (ADR 0054).
 *
 * Core pointers are borrowed and valid only while the actor is externally
 * synchronized against mutation. The completion pointer and its data are
 * borrowed and must not be released. Inspection never calls the data-model
 * adapter; read the data model with ta_actor_datamodel_root().
 */
typedef struct {
    ta_actor_id actor;
    /*
     * The SCXML session this actor holds, as _sessionid reads it (ADR 0066):
     * the runtime ID the session was spawned with, kept across unload and
     * load. It equals `actor` until the session has been reloaded.
     */
    uint64_t session_id;
    ta_actor_ref self;
    ta_definition_id definition_id;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;
    /* Number of states in the active configuration, ancestors included. */
    uint32_t active_state_count;
    /* True while a macrostep is paused mid-way (ta_actor_resume() continues it). */
    bool macrostep_in_progress;
    /* True while it is paused inside a microstep, in executable content
     * (ADR 0061). The active states are then those after the last completed
     * microstep. */
    bool microstep_in_progress;
    /* The external event of the paused macrostep, or null. */
    TA_IN TA_OPTIONAL const ta_event *current_event;
    TA_COUNTED_BY(internal_count) TA_OPTIONAL const ta_queued_event *internal_events;
    uint32_t internal_count;
    TA_COUNTED_BY(invoke_count) TA_OPTIONAL const ta_active_invoke *invokes;
    uint32_t invoke_count;
    /* Pending delayed sends, ordered by due time, then send order. */
    TA_COUNTED_BY(delayed_send_count) TA_OPTIONAL const ta_delayed_send_info *delayed_sends;
    uint32_t delayed_send_count;
    bool has_completion;
    TA_IN TA_OPTIONAL const ta_actor_completion *completion;
    /*
     * The top-level <final> whose entry ended the session, when `residency`
     * is TA_ACTOR_DONE; TA_STATE_NONE otherwise, including an invoked child
     * that was cancelled and an actor paused inside a microstep (ADR 0056).
     * The active configuration stays empty after finishing: this names the
     * outcome, it does not make the state active. Resolve it with
     * ta_definition_state().
     */
    ta_state_id final_state;
    /*
     * Bytes the core currently allocates for this actor: the actor record,
     * session storage (configuration and history), the data-model instance,
     * sparse state, and queue capacities. Excludes memory the data-model
     * adapter allocates itself. A measurement: it may change between releases.
     */
    size_t allocated_bytes;
} ta_actor_inspection;

typedef ta_result (*ta_actor_inspection_visitor)(void *self,
                                                 TA_IN const ta_actor_inspection *inspection);
typedef struct { void *self; ta_actor_inspection_visitor visit; } ta_actor_inspector TA_ADAPTER;

TA_API ta_result ta_actor_inspect(ta_system *system,
                                  ta_actor_id actor,
                                  TA_OUT TA_LIFETIME(system) ta_actor_inspection *out);
/*
 * The two halves of an actor ID, and an ID from its halves (ADR 0059). Pure:
 * no system, no failure. A made ID is checked where it is used, like any
 * other: a slot that holds no actor, or a generation that is not the slot's
 * current one, is TA_ERR_NOT_FOUND there.
 */
TA_API uint32_t ta_actor_id_slot(ta_actor_id id);
TA_API uint32_t ta_actor_id_generation(ta_actor_id id);
TA_API ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation);
/* The position of a finished walk: nothing is left to visit. */
#define TA_ACTOR_VISIT_END ((uint32_t)UINT32_MAX)

/*
 * Walks the resident actors (ADR 0062), invoked children included, handing
 * `inspector` the same view ta_actor_inspect() gives for each. Actors are
 * visited in increasing position order, starting at `from`. A position is
 * an actor's slot, ta_actor_id_slot() of its ID: pass 0 to start, then the
 * previous call's `next`, or the slot of an actor to resume at. No slot is
 * TA_ACTOR_VISIT_END. A walk of everything is `from` 0 and `limit` 0, and
 * may leave `next` null.
 *
 * - The walk reached the end: TA_OK, and `next` is TA_ACTOR_VISIT_END.
 * - `limit` visits answered TA_OK (0 means no limit): TA_OK, and `next` is
 *   the position after the last actor visited.
 * - A visit answered anything else: the walk stops with that result, and
 *   `next` is that actor's position, so calling again offers it again. An
 *   inspector ends a walk early this way.
 * - A null `params` is TA_ERR_INVALID; so is a null `visit`, with `next`
 *   set to `from`.
 *
 * Between calls, and from inside a visit, the system may change. An actor
 * resident for the whole iteration is visited exactly once; an actor
 * destroyed before the walk reaches it is not visited; one spawned is
 * visited only if its position is at or after the cursor. No position is
 * visited twice in one iteration. A call costs O(actors visited + empty
 * slots passed).
 */
typedef struct {
    ta_actor_inspector inspector;
    uint32_t from;
    uint32_t limit;
    TA_OUT TA_OPTIONAL TA_ALWAYS uint32_t *next;
} ta_actor_visit_params;
TA_API ta_result ta_system_visit_actors(ta_system *system, TA_IN const ta_actor_visit_params *params);
/*
 * The data model's inspection root (its `inspect` operation). Release it
 * with ta_value_release(). TA_ERR_UNSUPPORTED when the adapter has none.
 */
TA_API ta_result ta_actor_datamodel_root(ta_system *system,
                                         ta_actor_id actor,
                                         TA_OUT TA_RELEASE(ta_value_release) ta_value *out);

/*
 * List outputs share one convention: the callee writes at most `capacity`
 * elements to the caller's array and always stores the total in `*count`.
 * When the array is too small it returns TA_ERR_NOMEM and writes nothing, so
 * `capacity = 0` with a null array is a size query.
 */

/*
 * The active configuration: every active state, ancestors included, in
 * document order. Resolve IDs with ta_definition_state().
 */
typedef struct {
    ta_actor_id actor;
    TA_BUFFER(capacity, count) TA_OPTIONAL ta_state_id *states;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *count;
} ta_actor_active_states_params;
TA_API ta_result ta_actor_active_states(ta_system *system,
                                        TA_IN const ta_actor_active_states_params *params);

/*
 * The states recorded by one history pseudo-state, in document order.
 * `*count` is zero when the history has never been recorded; its default
 * transition then applies.
 */
typedef struct {
    ta_actor_id actor;
    ta_state_id history;
    TA_BUFFER(capacity, count) TA_OPTIONAL ta_state_id *states;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *count;
} ta_actor_history_params;
TA_API ta_result ta_actor_history(ta_system *system, TA_IN const ta_actor_history_params *params);

/*
 * Static facts about one state of a registered definition. `id` is the
 * author's state ID and is borrowed from the system. `parent` is
 * TA_STATE_NONE for top-level states. TA_ERR_NOT_FOUND ends iteration over
 * state IDs 0, 1, 2, ... in document order; it is also the result when the
 * definition ID is unknown or stale. A retired definition is readable until
 * it is freed.
 */
typedef struct {
    ta_str id;
    ta_state_id parent;
    ta_state_kind kind;
} ta_state_info;
typedef struct {
    ta_definition_id definition;
    ta_state_id state;
    TA_OUT TA_LIFETIME(definition) ta_state_info *out;
} ta_definition_state_params;
TA_API ta_result ta_definition_state(const ta_system *system,
                                     TA_IN const ta_definition_state_params *params);

typedef struct {
    ta_definition_id definition;
    /* The author's state ID, as `ta_definition_state` reports it. */
    ta_str id;
    TA_OUT ta_state_id *out;
} ta_definition_find_state_params;

/*
 * The state of a definition with this author ID: the inverse of
 * ta_definition_state (ADR 0049). TA_ERR_NOT_FOUND when the definition has
 * no such state, which is a question, not a failure.
 */
TA_API ta_result ta_definition_find_state(const ta_system *system,
                                          TA_IN const ta_definition_find_state_params *params);

/*
 * Lookups by ID, in the style of ta_definition_state: literal content, and
 * transitions, actions and expressions (ADR 0071). Each takes the definition
 * and one ID and writes one view, whose text and arrays are borrowed from
 * the definition. TA_ERR_NOT_FOUND ends a walk over IDs 0, 1, 2, ... (the
 * counts are in ta_definition_inspection) and is also the answer for an
 * unknown or stale definition. A retired definition is readable until it is
 * freed.
 */

/*
 * The bytes of a definition's literal content: a <content> body or a literal
 * send target, as the chart holds them (ADR 0053). Inspection names content
 * by ID, as ta_delayed_send_info.target_text does.
 */
typedef struct {
    /* The media type the chart gave, empty when it gave none. */
    ta_str media_type;
    ta_bytes bytes;
} ta_content_info;
typedef struct {
    ta_definition_id definition;
    ta_content_id content;
    TA_OUT TA_LIFETIME(definition) ta_content_info *out;
} ta_definition_content_params;
TA_API ta_result ta_definition_content(const ta_system *system,
                                       TA_IN const ta_definition_content_params *params);

/*
 * Transition IDs: the <transition> elements come first, and they are the
 * only transitions trace records name. After them come the initial
 * transitions, one per compound state and one for the document: an
 * <initial> element, an `initial` attribute, or SCXML's default (the first
 * child state). The document's is the one the bootstrap takes.
 */
typedef enum {
    TA_TRANSITION_AUTHORED = 0,   /* a <transition> element                            */
    TA_TRANSITION_INITIAL,        /* a compound state's initial transition             */
    TA_TRANSITION_BOOTSTRAP       /* the document's initial transition, which bootstrap takes */
} ta_transition_origin TA_SYMBOLS("TA_TRANSITION_");

typedef struct {
    ta_transition_origin origin;
    /* The state it leaves: its <transition>'s parent, or the state an initial
     * transition belongs to; TA_STATE_NONE for BOOTSTRAP. */
    ta_state_id source;
    /* EXTERNAL or INTERNAL; initial transitions are EXTERNAL. */
    ta_scxml_transition_kind type;
    /* The event descriptor as authored; empty for an eventless or initial one. */
    ta_str event;
    TA_COUNTED_BY(target_count) TA_LIFETIME(definition) const ta_state_id *targets;
    uint32_t target_count;
    /* True when the targets are SCXML's default, the first child state, and
     * not authored. Only an initial transition can have default targets. */
    bool default_targets;
    /* TA_EXPR_ID_NONE when unguarded; initial transitions never have a guard. */
    ta_expr_id condition;
    /* The size of its block of executable content. */
    uint32_t action_count;
    /* Its place among the <transition> elements of the document; 0 for an
     * initial transition. */
    uint32_t document_order;
} ta_transition_info;
typedef struct {
    ta_definition_id definition;
    ta_transition_id transition;
    TA_OUT TA_LIFETIME(definition) ta_transition_info *out;
} ta_definition_transition_params;
TA_API ta_result ta_definition_transition(const ta_system *system,
                                          TA_IN const ta_definition_transition_params *params);

/* The block of executable content an action sits in, and what `owner` is. */
typedef enum {
    TA_ACTION_IN_ONENTRY = 0,     /* owner: a state; block_index: which <onentry>         */
    TA_ACTION_IN_ONEXIT,          /* owner: a state; block_index: which <onexit>          */
    TA_ACTION_IN_TRANSITION,      /* owner: a transition ID, initial transitions included */
    TA_ACTION_IN_THEN,            /* owner: an <if> action                                */
    TA_ACTION_IN_ELSE,            /* owner: an <if> action; an <elseif> is an <if> here   */
    TA_ACTION_IN_FOREACH,         /* owner: a <foreach> action                            */
    TA_ACTION_IN_FINALIZE,        /* owner: the <invoke>'s state; block_index: which <invoke> */
    TA_ACTION_IN_SCRIPT           /* the document's top-level <script>; owner: TA_STATE_NONE */
} ta_action_block TA_SYMBOLS("TA_ACTION_IN_");

typedef struct {
    /* The element: raise, send, cancel, assign, script, log, if or foreach. */
    ta_scxml_action_kind kind;
    ta_action_block block;
    /* A state, transition or action ID, as `block` says. */
    uint32_t owner;
    /* Which of the owner's blocks of that kind, from 0, in document order:
     * the <onentry> or <onexit> element, or the <invoke> of FINALIZE; 0 for
     * every other block. */
    uint32_t block_index;
    /* Its place in the block, from 0. */
    uint32_t index;
} ta_action_info;
typedef struct {
    ta_definition_id definition;
    ta_action_id action;
    TA_OUT ta_action_info *out;
} ta_definition_action_params;
TA_API ta_result ta_definition_action(const ta_system *system,
                                      TA_IN const ta_definition_action_params *params);

/*
 * An expression as the definition stores it: SOURCE text the compiler
 * copied, or a HOST expression's retained value, which only the host that
 * built it can read. Core never invents text for a host expression.
 */
typedef struct {
    ta_definition_id definition;
    ta_expr_id expression;
    TA_OUT TA_LIFETIME(definition) ta_expression *out;
} ta_definition_expression_params;
TA_API ta_result ta_definition_expression(const ta_system *system,
                                          TA_IN const ta_definition_expression_params *params);

/*
 * A read-only view of one registered definition. Text and the adapter are
 * borrowed from the system and stay valid while the definition exists (a
 * retired definition exists until its last resident actor is gone).
 */
typedef struct {
    ta_definition_id id;
    /* The exact revision (ADR 0028, ADR 0038). */
    ta_definition_identity identity;
    /* The <scxml name> attribute, which _name reads; empty when absent. */
    ta_str document_name;
    /*
     * The adapter instance the definition was compiled with. `self`
     * identifies the instance and `v->name` the SCXML data-model name.
     * Use it to identify the adapter; never call its operations.
     */
    ta_datamodel datamodel;
    /* TA_SCXML_BINDING_EARLY or TA_SCXML_BINDING_LATE, never DEFAULT. */
    ta_scxml_binding_kind binding;
    /* States readable with ta_definition_state(). */
    uint32_t state_count;
    /* Transitions readable with ta_definition_transition(), initial ones included. */
    uint32_t transition_count;
    /* Actions readable with ta_definition_action(). */
    uint32_t action_count;
    /* Expressions readable with ta_definition_expression(). */
    uint32_t expression_count;
    /* Literal content readable with ta_definition_content(). */
    uint32_t content_count;
    /* True after the definition is retired (ADR 0043). */
    bool retired;
    /* Resident actors using this definition, unpublished replacements included. */
    uint32_t actor_count;
    /*
     * Bytes the core allocates for each actor of this definition before it
     * processes events: the actor record, session storage (configuration and
     * history), and the data-model instance. Excludes queues, the sparse
     * state of invoked actors, and memory the data-model adapter allocates
     * itself. A measurement: it may change between releases.
     */
    size_t actor_fixed_bytes;
    /* Bytes of the definition's own storage (its arena). A measurement. */
    size_t definition_bytes;
} ta_definition_inspection;

/*
 * TA_ERR_NOT_FOUND when the ID names no existing definition: never
 * registered, freed after retirement, or stale.
 */
TA_API ta_result ta_definition_inspect(const ta_system *system,
                                       ta_definition_id definition,
                                       TA_OUT TA_LIFETIME(definition) ta_definition_inspection *out);


/* ========================================================================
 * Session images
 */


/*
 * A session image is an owned in-memory representation of Tinyactors-managed
 * interpreter state.  It is not a persistence wire format and does not contain
 * the host-owned data-model object graph.  Host values referenced by
 * interpreter-owned events, timers, or continuation state are retained as host
 * references according to ta_value lifetime rules.
 *
 * Invoker service handles are runtime-only and are not rebound.  Restoring an
 * image retires every invocation that was active in it and appends one
 * `error.communication.invoke.<id>.unavailable` platform event to the internal
 * queue.  The event name identifies the invocation; its invoke_id field remains
 * TA_INVOKE_ID_NONE because the event was generated by the processor, not the
 * invoked service.  Those events follow internal events already captured by the
 * image.  An active invocation therefore does not make capture or restoration
 * return TA_ERR_NOT_RECONSTRUCTIBLE.

 * ta_actor_reconstruct() restores an image without participating in named
 * routing. ta_actor_load() wraps the same reconstruction in the named-actor
 * residency protocol and publishes it only when the directory still says
 * UNLOADED. An image is still the host's to hold, to index by
 * ta_session_image_next_deadline(), and to destroy.
 *
 * An unpublished prepared replacement is not a routable session yet and cannot
 * be captured; ta_actor_capture() returns TA_ERR_BUSY until activation.
 */
typedef struct {
    ta_actor_id actor;
    ta_session_image **out;
} ta_actor_capture_params;

TA_API ta_result ta_actor_capture(ta_system *system,
                                  TA_IN const ta_actor_capture_params *params);
/* Returns TA_ERR_NOT_FOUND when the captured actor owns no delayed send. */
TA_API ta_result ta_session_image_next_deadline(const ta_session_image *image,
                                                TA_OUT ta_time *out);
/*
 * The registered definition an image must be restored against: the exact
 * revision it was captured under. TA_ERR_DEFINITION_MISMATCH when only another
 * revision of its family is registered, TA_ERR_NOT_FOUND when none is. A host
 * uses it to build the data-model binding with that definition's data model
 * before ta_actor_load() or ta_actor_reconstruct().
 */
TA_API ta_result ta_session_image_definition(const ta_system *system,
                                             const ta_session_image *image,
                                             TA_OUT ta_definition_id *out);
TA_API void ta_session_image_destroy(TA_DESTROYS ta_session_image *image);


/* ========================================================================
 * Systems
 */


typedef struct { ta_allocator allocator; ta_time initial_time; } ta_system_config;
typedef struct {
    ta_event_name_id name;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_event_name_format_params;

TA_API ta_result ta_system_create(TA_IN const ta_system_config *config, ta_system **out);
TA_API void ta_system_destroy(TA_DESTROYS ta_system *system);
TA_API ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory);
/*
 * How many empty event blocks the system keeps for reuse (ADR 0064,
 * ADR 0069); 4 when a system is created. An actor holds a block from
 * ta_actor_deliver() until its macrostep ends, so a host that delivers a
 * batch of events before it resumes the actors needs a cache as large as
 * the batch for delivery to make no allocator call. The cache takes no
 * memory until blocks are returned to it, and it keeps only small blocks,
 * so it holds at most `blocks` of them. A smaller setting frees the cached
 * blocks above it.
 */
TA_API ta_result ta_system_set_event_block_cache(ta_system *system, uint32_t blocks);
/* Installs the scheduler for normalized external SCXML-session events. */
TA_API ta_result ta_system_set_external_event_router(ta_system *system,
                                                      ta_external_event_router router);
/* Registration takes ownership of definition only when it returns TA_OK. */
TA_API ta_result ta_system_register_definition(ta_system *system,
                                               TA_CONSUMED_ON_OK ta_definition *definition,
                                               TA_OUT ta_definition_id *out);
/*
 * The registered, non-retired definition with exactly this identity; family
 * and revision are compared bytewise. When several match, the most recently
 * registered wins. TA_ERR_NOT_FOUND when none matches.
 */
TA_API ta_result ta_system_find_definition(const ta_system *system,
                                           TA_IN const ta_definition_identity *identity,
                                           TA_OUT ta_definition_id *out);
/*
 * Ends a definition's availability (ADR 0043). ta_actor_spawn() and
 * ta_actor_prepare_replacement() naming it fail with TA_ERR_NOT_FOUND, and
 * ta_system_find_definition() skips it. Resident actors that use it keep
 * running, and ta_definition_inspect(), ta_definition_state(), and
 * ta_definition_export() keep working. The system frees the definition (its
 * data-model state, its storage, and its ID) when no resident actor uses it:
 * here if none does, otherwise when the last one is destroyed. Idempotent:
 * retiring a retired definition, even one already freed, returns TA_OK.
 * TA_ERR_NOT_FOUND when the ID was never issued.
 */
TA_API ta_result ta_system_retire_definition(ta_system *system, ta_definition_id definition);
/*
 * The I/O processor registry (ADR 0068). Index 0 is always the SCXML
 * processor; registered processors get 1, 2, 3, ... in registration order.
 * An index never changes while the system lives: registering a type again
 * replaces its adapter at the same index. The atom returned by
 * ta_system_scxml_io_type() is reserved.
 */
typedef struct {
    ta_atom type;
    ta_io io;
    TA_OUT TA_OPTIONAL uint32_t *index;
} ta_io_registration_params;
TA_API ta_result ta_system_register_io(ta_system *system, TA_IN const ta_io_registration_params *params);
/* The registry index of a processor type; TA_ERR_NOT_FOUND when none. */
TA_API ta_result ta_system_find_io(const ta_system *system, ta_atom type, TA_OUT uint32_t *index);
/* Installs where <log> goes; without one, a <log> only evaluates its expr. */
TA_API ta_result ta_system_set_log_sink(ta_system *system, ta_log_sink sink);
/*
 * Installs the trace sink (ADR 0067); ta_null_trace_sink() stops tracing.
 * TA_ERR_INVALID for a sink whose `v` or `v->write` is null.
 */
TA_API ta_result ta_system_set_trace_sink(ta_system *system, ta_trace_sink sink);

/*
 * The system's trace-ID generator (ADR 0073): xoshiro256** seeded through
 * SplitMix64. Core never reads the OS: the host seeds each system once,
 * from random bytes in production or a fixed value in tests, and a seed
 * gives the same sequence on every host. An unseeded system uses seed 0.
 */
TA_API void ta_system_seed_trace_ids(ta_system *system, uint64_t seed);
/*
 * A new trace: a random, never-zero trace ID with TA_TRACE_FLAG_RANDOM,
 * and TA_TRACE_FLAG_SAMPLED when the system's ratio samples it. `detail`
 * is 0.
 */
TA_API ta_trace_context ta_system_new_trace(ta_system *system);
/* A random, never-zero span ID, for a host's own spans. */
TA_API uint64_t ta_system_new_span_id(ta_system *system);
/* The ratio ta_system_new_trace samples at, `parts` / `whole` (see
 * ta_trace_ratio_threshold); 1 (every trace) by default. */
TA_API void ta_system_set_trace_ratio(ta_system *system, uint32_t parts, uint32_t whole);
TA_API ta_result ta_system_register_invoker(ta_system *system,
                                            ta_atom type,
                                            ta_invoker invoker);
TA_API ta_result ta_system_intern_atom(ta_system *system, ta_str text, TA_OUT ta_atom *out);
/*
 * The text an atom interns (ADR 0049), borrowed from the system's atom table
 * and valid while the system is. Atoms are system-local (ADR 0007): one of
 * another system is TA_ERR_NOT_FOUND, not undefined. This is the inverse of
 * ta_system_intern_atom, so a host that is handed `ta_send.io_type` or
 * `ta_invoke_request.type` can say what it was given.
 */
TA_API ta_result ta_system_atom_text(const ta_system *system,
                                     ta_atom atom,
                                     TA_OUT TA_LIFETIME(system) ta_str *out);
/*
 * The text of an invocation's ID (ADR 0049): the author's `<invoke id>`, or
 * the `<stateid>.<platformid>` core generated for one without. It is what
 * `done.invoke.<id>` names and what the `#_<invokeid>` target spells, so a
 * host holding `ta_active_invoke.id` can say which invocation it has.
 * Borrowed from the system. TA_ERR_NOT_FOUND for an ID it never issued.
 */
TA_API ta_result ta_system_invoke_id_text(const ta_system *system,
                                          ta_invoke_id invoke,
                                          TA_OUT TA_LIFETIME(system) ta_str *out);
TA_API ta_result ta_event_name_intern(ta_system *system,
                                      TA_IN const ta_atom_list *segments,
                                      TA_OUT ta_event_name_id *out);
TA_API bool ta_event_name_matches(const ta_system *system, ta_event_name_id event, ta_event_name_id prefix);
TA_API ta_result ta_event_name_format(const ta_system *system,
                                      TA_IN const ta_event_name_format_params *params);
TA_API ta_result ta_system_set_time(ta_system *system, ta_time now);
TA_API ta_time ta_system_time(const ta_system *system);
/* Returns the built-in SCXML Event I/O Processor's system-local type atom. */
TA_API ta_atom ta_system_scxml_io_type(const ta_system *system);


/* ========================================================================
 * Delayed sends
 */


typedef struct {
    /* Must be nonzero; limits synchronous host callbacks in this call. */
    uint32_t budget;
    TA_OUT uint32_t *dispatched;
    TA_OUT bool *more_due;
} ta_due_dispatch_params;

/* TA_ERR_NOT_FOUND means that no resident delayed send is pending. */
TA_API ta_result ta_system_next_deadline(const ta_system *system, TA_OUT ta_time *out);
TA_API ta_result ta_system_dispatch_due(ta_system *system,
                                        TA_IN const ta_due_dispatch_params *params);

#ifdef __cplusplus
}
#endif
