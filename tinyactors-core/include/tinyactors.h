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
    TA_ERR_STALE = -10
};

typedef struct { const char *ptr; uint32_t len; } ta_str TA_STRING;
typedef struct { TA_COUNTED_BY(count) const ta_atom *atoms; uint32_t count; } ta_atom_list;
typedef struct { const uint8_t *ptr; uint32_t len; } ta_bytes TA_BYTES;
typedef struct { ta_str name; } ta_definition_family;
typedef struct { ta_definition_family family; ta_str name; } ta_actor_address;
typedef enum { TA_ACTOR_REF_RUNTIME = 0, TA_ACTOR_REF_NAMED = 1 } ta_actor_ref_kind TA_SYMBOLS("TA_ACTOR_REF_");
typedef struct {
    ta_actor_ref_kind kind;
    union {
        TA_WHEN(TA_ACTOR_REF_RUNTIME) ta_actor_id runtime;
        TA_WHEN(TA_ACTOR_REF_NAMED) ta_actor_address named;
    } as;
} ta_actor_ref TA_TAGGED(kind, as);

typedef struct ta_system ta_system TA_HANDLE;
typedef struct ta_definition ta_definition TA_HANDLE;
typedef struct ta_dm_host ta_dm_host TA_VIEW;
typedef struct ta_value ta_value TA_HOST_VALUE;
typedef struct ta_session_image ta_session_image TA_HANDLE;



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
/* ========================================================================
 * Host values
 */


typedef struct ta_value_vtable {
    void (*retain)(void *self);
    void (*release)(void *self);
    ta_result (*clone)(void *self, TA_OUT TA_RELEASE(ta_value_release) ta_value *out);
} ta_value_vtable TA_VTABLE;
struct ta_value { void *self; const ta_value_vtable *v; };


static inline void ta_value_retain(TA_INOUT TA_OPTIONAL ta_value *value) TA_ANY_VALUE_MODEL {
    if (value && value->v && value->v->retain) value->v->retain(value->self);
}

static inline void ta_value_release(TA_INOUT TA_OPTIONAL ta_value *value) TA_ANY_VALUE_MODEL {
    if (value && value->v && value->v->release) value->v->release(value->self);
}

TA_API ta_result ta_value_clone(TA_IN const ta_value *value,
                                TA_OUT TA_RELEASE(ta_value_release) ta_value *out)
    TA_ANY_VALUE_MODEL;
/* ========================================================================
 * Events and faults
 */


typedef enum { TA_EVENT_EXTERNAL = 0, TA_EVENT_INTERNAL = 1, TA_EVENT_PLATFORM = 2 } ta_event_type TA_SYMBOLS("TA_EVENT_");
typedef enum { TA_ORIGIN_NONE = 0, TA_ORIGIN_ACTOR = 1, TA_ORIGIN_IO = 2, TA_ORIGIN_INVOKE = 3 } ta_origin_kind TA_SYMBOLS("TA_ORIGIN_");
typedef struct {
    ta_origin_kind kind;
    union {
        TA_WHEN(TA_ORIGIN_ACTOR) ta_actor_ref actor;
        TA_WHEN(TA_ORIGIN_INVOKE) ta_invoke_id invoke;
        TA_WHEN(TA_ORIGIN_IO) uint64_t opaque;
    } as;
} ta_origin TA_TAGGED(kind, as);
typedef struct { ta_event_name_id name; ta_event_type type; ta_send_id send_id; ta_origin origin; ta_invoke_id invoke_id; ta_value data; } ta_event;
/* invoke_id is TA_INVOKE_ID_NONE unless an invoked child generated the event. */
/* A receiver of a fault owns its data; the message is borrowed. */
typedef struct { ta_event_name_id detail; int32_t code; ta_str message; TA_RELEASE(ta_value_release) ta_value data; } ta_fault;
typedef enum { TA_OP_OK TA_SUCCESS = 0, TA_OP_ERROR = 1 } ta_op_status TA_SYMBOLS("TA_OP_");
typedef enum { TA_RUN_STABLE = 0, TA_RUN_PAUSED = 1, TA_RUN_DONE = 2, TA_RUN_ERROR = 3 } ta_run_status TA_SYMBOLS("TA_RUN_");


typedef struct {
    ta_event event;
    TA_FLAGS(ta_event_flags) uint32_t flags;
} ta_queued_event;

enum TA_FLAG_SET(ta_event_flags, "TA_EVENT_FLAG_") {
    TA_EVENT_FLAG_OWNS_DATA = 1u << 0,
    TA_EVENT_FLAG_GENERATED = 1u << 1
};

TA_API ta_result ta_event_clone(TA_IN const ta_event *event,
                                TA_OUT TA_RELEASE(ta_event_release) ta_event *out)
    TA_ANY_VALUE_MODEL;
TA_API void ta_event_release(TA_INOUT ta_event *event) TA_ANY_VALUE_MODEL;
/* ========================================================================
 * External SCXML event routing
 */


/* A fully normalized event destined for another SCXML external event queue. */
typedef struct {
    ta_actor_ref destination;
    ta_event event;
} ta_external_delivery;

/* Deep-copies a named destination and clones event data. */
TA_API ta_result ta_external_delivery_clone(TA_IN const ta_external_delivery *delivery,
                                            ta_allocator allocator,
                                            TA_OUT TA_RELEASE(ta_external_delivery_release)
                                                ta_external_delivery *out)
    TA_ANY_VALUE_MODEL;
TA_API void ta_external_delivery_release(TA_INOUT ta_external_delivery *delivery,
                                         ta_allocator allocator) TA_ANY_VALUE_MODEL;

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
typedef struct {
    TA_COUNTED_BY(expression_count) TA_OPTIONAL TA_LIFETIME(definition) const ta_expression *expressions;
    uint32_t expression_count;
    ta_allocator allocator;
    void **shared;
} ta_datamodel_prepare_definition_params;
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
 *   #_scxml_<session>          a session by its runtime ID
 *   #_actor_<family>/<name>    a named actor by its logical address
 *   #_<invokeid>               an invocation of the sending session
 *
 * A session URI whose text is not a session ID names no session: it parses
 * as runtime actor 0, which never exists, so a send to it is inaccessible
 * (SCXML C.1) rather than invalid. TA_ERR_INVALID means an invalid target,
 * which a send reports as error.execution.
 *
 * A named address borrows its family and name from `text`, which must stay
 * valid for as long as the target is used.
 */
TA_API ta_result ta_scxml_target_parse(ta_system *system,
                                       ta_str text,
                                       TA_OUT TA_LIFETIME(input) ta_scxml_target *out);

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
 * Text of the SCXML system variables, for data models that expose them
 * (SCXML 5.10). Each field is formatted into the caller's buffer following
 * the list-output convention: `*needed` always receives the size including
 * the terminating NUL, and a too-small buffer yields TA_ERR_NOMEM without
 * writing. TA_ERR_NOT_FOUND means the field has no value, e.g. no event has
 * been processed yet, or the current event has no send ID or origin.
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
    /* _ioprocessors: the SCXML Event I/O Processor's location for this session. */
    TA_DM_TEXT_SCXML_IO_LOCATION,
    /* _ioprocessors: the SCXML Event I/O Processor's type URI. */
    TA_DM_TEXT_SCXML_IO_TYPE
} ta_dm_text_kind TA_SYMBOLS("TA_DM_TEXT_");
typedef struct {
    ta_dm_text_kind kind;
    TA_BUFFER(capacity, needed) TA_OPTIONAL char *buffer;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *needed;
} ta_dm_text_params;
TA_API ta_result ta_dm_host_text(const ta_dm_host *host, TA_IN const ta_dm_text_params *params);
/* The author's ID of a state; TA_ERR_NOT_FOUND ends iteration over 0, 1, 2, ... */
TA_API ta_result ta_dm_host_state_name(const ta_dm_host *host,
                                       ta_state_id state,
                                       TA_OUT TA_LIFETIME(definition) ta_str *out);
/* ========================================================================
 * I/O processors
 */


typedef struct { ta_actor_ref source; ta_send_id send_id; ta_event_name_id event; ta_atom io_type; ta_value target; ta_value data; } ta_send;
typedef struct ta_io_vtable {
    ta_op_status (*send)(void *self,
                         TA_IN const ta_send *send,
                         TA_OUT TA_VALID_ON(TA_OP_ERROR) TA_LIFETIME(call) ta_fault *fault);
} ta_io_vtable TA_VTABLE;
typedef struct { void *self; const ta_io_vtable *v; } ta_io TA_ADAPTER;


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
 * Invokers
 */


/*
 * One invocation to start. `type` is the resolved invoker type; the SCXML
 * aliases "scxml" and "http://www.w3.org/TR/scxml" are normalized to
 * "http://www.w3.org/TR/scxml/". `src` is the evaluated src/srcexpr, `data`
 * the evaluated inline <content>, and `params` the data model's aggregate of
 * <param>/namelist values; each is empty (null vtable) when absent. Values are
 * borrowed for the call. A service for an SCXML session spawns its child with
 * ta_actor_spawn() and a `parent` of {parent, invoke_id}; the core then routes
 * `#_<invokeid>` sends, autoforwarding, and done.invoke itself.
 */
typedef struct { ta_system *system; ta_actor_id parent; ta_invoke_id invoke_id; ta_atom type; ta_value src; ta_value data; ta_value params; bool autoforward; } ta_invoke_request;
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
        TA_WHEN(TA_SCXML_VALUE_CONTENT) ta_bytes content;
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
typedef struct {
    TA_IN const ta_scxml_spec *spec;
    ta_datamodel datamodel;
    ta_allocator allocator;
    ta_definition **out;
} ta_definition_compile_params;

TA_API ta_result ta_definition_compile_spec(ta_system *system,
                                            TA_IN const ta_definition_compile_params *params);
TA_API void ta_definition_destroy(TA_DESTROYS ta_definition *definition);

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
typedef struct { TA_IN const ta_actor_address *address; ta_actor_directory_entry entry; } ta_actor_directory_claim_params;
typedef struct { TA_IN const ta_actor_address *address; TA_OUT ta_actor_directory_entry *out; } ta_actor_directory_resolve_params;
typedef struct {
    TA_IN const ta_actor_address *address;
    ta_actor_directory_entry expected;
    ta_actor_directory_entry replacement;
} ta_actor_directory_replace_params;
typedef struct {
    TA_IN const ta_actor_address *address;
    ta_actor_directory_entry expected;
} ta_actor_directory_remove_params;
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

typedef struct {
    ta_definition_id definition;
    TA_IN TA_OPTIONAL const ta_actor_address *address;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_IN TA_OPTIONAL const ta_actor_parent *parent;
    TA_OUT ta_actor_id *out;
} ta_actor_spawn_params;
typedef struct { TA_IN const ta_actor_address *address; TA_OUT ta_actor_id *out; } ta_actor_find_params;
typedef struct { ta_actor_id actor; uint32_t budget; TA_OUT ta_run_status *status; } ta_actor_resume_params;
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
TA_API ta_result ta_actor_deliver(ta_system *system, ta_actor_id actor, TA_IN const ta_event *event)
    TA_VALUE_MODEL(actor, actor);
TA_API ta_result ta_actor_resume(ta_system *system, TA_IN const ta_actor_resume_params *params);
/*
 * Evicts a named actor without terminating it. The image must be a fresh
 * capture of actor; TA_ERR_STALE means the actor changed after capture.
 */
TA_API ta_result ta_actor_unload(ta_system *system, TA_IN const ta_actor_unload_params *params);
/* Restores an unloaded named actor and atomically republishes its route. */
TA_API ta_result ta_actor_load(ta_system *system, TA_IN const ta_actor_load_params *params)
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
 * A read-only operational view of one resident interpreter instance.
 *
 * Fixed-size facts are fields; variable-length lists (active states, history,
 * delayed sends) are read with the ta_actor_* list functions below, whose
 * counts are reported here so a host can size buffers once.
 *
 * Core pointers are borrowed and valid only while the actor is externally
 * synchronized against mutation. The completion pointer and its data are
 * borrowed and must not be released. Inspection never calls the data-model
 * adapter; read the data model with ta_actor_datamodel_root().
 */
typedef struct {
    ta_actor_id actor;
    ta_actor_ref self;
    ta_definition_id definition_id;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;
    /* Number of states in the active configuration, ancestors included. */
    uint32_t active_state_count;
    /* True while a macrostep is paused mid-way (ta_actor_resume() continues it). */
    bool macrostep_in_progress;
    /* The external event of the paused macrostep, or null. */
    TA_IN TA_OPTIONAL const ta_event *current_event;
    TA_COUNTED_BY(internal_count) TA_OPTIONAL const ta_queued_event *internal_events;
    uint32_t internal_count;
    TA_COUNTED_BY(invoke_count) TA_OPTIONAL const ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t delayed_send_count;
    bool has_completion;
    TA_IN TA_OPTIONAL const ta_actor_completion *completion;
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
 * Visits every resident actor once, invoked children included, with the
 * same view ta_actor_inspect() gives for it. The walk stops at the first
 * visit that answers anything but TA_OK and returns that result, so an
 * inspector can end it early; a null `visit` is TA_ERR_INVALID. The order
 * of the walk is not part of the contract.
 */
TA_API ta_result ta_system_visit_actors(ta_system *system, ta_actor_inspector inspector);
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
 * One pending delayed send owned by an actor. `target` and `data` are
 * borrowed; retain them with ta_value_retain() to keep them past the call's
 * synchronization window.
 */
typedef struct {
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_value target;
    ta_value data;
} ta_delayed_send_info;

/* Pending delayed sends of one actor, ordered by due time, then send order. */
typedef struct {
    ta_actor_id actor;
    TA_BUFFER(capacity, count) TA_OPTIONAL TA_LIFETIME(system) ta_delayed_send_info *sends;
    uint32_t capacity;
    TA_OUT TA_ALWAYS uint32_t *count;
} ta_actor_delayed_sends_params;
TA_API ta_result ta_actor_delayed_sends(ta_system *system,
                                        TA_IN const ta_actor_delayed_sends_params *params);

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
 * Invoker service handles are runtime-only and are not rebound.  Reconstruction
 * retires every invocation that was active in the image and appends one
 * `error.communication.invoke.<id>.unavailable` platform event to the internal
 * queue.  The event name identifies the invocation; its invoke_id field remains
 * TA_INVOKE_ID_NONE because the event was generated by the processor, not the
 * invoked service.  Those events follow internal events already captured by the
 * image.  An active invocation therefore does not make capture or reconstruction
 * return TA_ERR_NOT_RECONSTRUCTIBLE.
 *
 * An unpublished prepared replacement is not a routable session yet and cannot
 * be captured; ta_actor_capture() returns TA_ERR_BUSY until activation.
 */
typedef struct {
    ta_actor_id actor;
    ta_session_image **out;
} ta_actor_capture_params;

typedef struct {
    const ta_session_image *image;
    TA_IN TA_OPTIONAL const ta_value *datamodel_binding;
    TA_OUT ta_actor_id *out;
} ta_actor_reconstruct_params;

TA_API ta_result ta_actor_capture(ta_system *system,
                                  TA_IN const ta_actor_capture_params *params);
/* Named directory-managed images are restored with ta_actor_load(). */
TA_API ta_result ta_actor_reconstruct(ta_system *system,
                                      TA_IN const ta_actor_reconstruct_params *params)
    TA_VALUE_MODEL(session_image, params.image);
/* Returns TA_ERR_NOT_FOUND when the captured actor owns no delayed send. */
TA_API ta_result ta_session_image_next_deadline(const ta_session_image *image,
                                                TA_OUT ta_time *out);
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
/* The atom returned by ta_system_scxml_io_type() is reserved. */
TA_API ta_result ta_system_register_io(ta_system *system, ta_atom type, ta_io io);
/* Installs where <log> goes; without one, a <log> only evaluates its expr. */
TA_API ta_result ta_system_set_log_sink(ta_system *system, ta_log_sink sink);
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
