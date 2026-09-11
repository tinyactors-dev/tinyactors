#ifndef TINYACTORS_H_INCLUDED
#define TINYACTORS_H_INCLUDED

/**
 * @file tinyactors.h
 * @brief Public contract for an embeddable, sans-I/O SCXML actor runtime.
 *
 * This interface represents the execution semantics in ../scxml.md, sections
 * 3--6 and appendix C.1. There is no XML parser, DOM, expression language, VM,
 * transport, event loop, thread, clock, or telemetry exporter in the core.
 * Author definitions as nested C structs with ta_definition_build, or supply
 * normalized tables with ta_definition_prepare. Execution uses shared immutable
 * tables in both cases; executable expressions are host tokens.
 * The header declares the interface; it does not contain the runtime engine.
 * XML syntax, XML validation, and the standard ECMAScript language binding are
 * outside this contract. A host supplies native-language or C data-model code.
 *
 * The implementation must be freestanding C99 and must not call libc, allocate
 * through malloc, use errno, or depend on OS/TLS/atomic facilities. The only
 * standard headers used here supply freestanding types. uint8_t, uint32_t,
 * uint64_t and uintptr_t must exist. C++ callers use the same C ABI. All modules
 * must use the same ABI/compiler layout; these structures are not wire formats.
 *
 * Vocabulary and ordering:
 * - An actor is an SCXML session: one configuration, one data-model instance
 *   (native object handle or owned C bytes), history, FIFOs and invoked services.
 * - A definition supplies states, transitions, executable content and invokes.
 *   "Document order" means the order of entries in these tables, never hash
 *   iteration order. Configuration excludes the synthetic root and history.
 * - Transition selection visits active atomic states in document order, looks
 *   at each state's transitions in order, then searches its ancestors. A
 *   descendant source wins conflicts; otherwise the earlier selecting atomic
 *   state wins. Conflicts mean intersecting exit sets, not matching events.
 * - A microstep exits the union of exit sets in reverse document order, runs
 *   selected transition blocks in document order, then enters the entry set
 *   in document order. Targetless transitions have empty entry/exit sets.
 * - A macrostep consumes at most one external event and reaches stability by
 *   preferring eventless transitions over internal FIFO events. Initialization
 *   also runs to stability. No next external event is consumed before stability.
 * - Invokes start, in document order, only for states still active at stability;
 *   any resulting internal errors are drained before taking an external event.
 * - Parallel regions are simultaneously active, not concurrent C executions.
 *   Independent actors may interleave only at the scheduling boundaries below.
 *
 * All calls on one system, including completions and reference operations, are
 * serialized by its host. Callbacks run synchronously on that same thread and
 * must return promptly. Do not longjmp, throw, block, or recursively run the
 * system. Only ta_context_* operations and arena operations on an explicitly
 * supplied arena are callable from execution callbacks. Other callbacks must
 * not call back into the same system. Use separate systems plus host transports
 * for multiple threads/VM isolates; native handles never cross systems implicitly.
 * A simulation system and renderer system can have different definitions,
 * storage layouts, clocks and budgets. Transfer byte messages or explicitly
 * encode/decode native values at the host boundary, preserving trace context.
 * Each system has its own actor identities; use a game entity ID in the payload
 * to correlate simulation and renderer actors. No shared mutable state is implied.
 * Concurrent systems need separate root arenas, or host synchronization covering
 * shared arena accounting as well as any shared allocator. Serializing each
 * system alone does not make a shared parent arena thread-safe.
 * Every function and callback in this interface takes at most three arguments.
 *
 * Ownership convention: inputs are borrowed through the call unless stated
 * otherwise; accepted asynchronous inputs are copied/rooted before return.
 * Output views are borrowed, not ownership transfers. A returned owned object
 * has an explicit release/close/complete operation. On failure no output is
 * valid unless that function documents one, and no input reference is consumed.
 * Pointer arguments are required unless documented as optional. A counted array
 * may use NULL only when its count is zero. Unused tagged-union fields are zero.
 * Ordinary API failures are status values. Errors during execution are SCXML
 * events, except the documented resource-exhaustion fail-stop policy.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef TA_API
/** Override when building a shared library; do not change calling convention. */
#define TA_API extern
#endif

/** Compiler-visible ABI version, usable in integer constant expressions. */
enum ta_abi_version { TA_ABI_VERSION = 3 };

/** Full-width sentinel values. These are typed const objects, not macros or
 * enum members narrowed to C's int range. Each translation unit has its own
 * constant objects; compare their values, never their addresses. Unlike enum
 * members, these objects are not C99 integer constant expressions: do not use
 * them for case labels, array bounds or portable static scalar initializers.
 * Assign such fields at run time, or use the nested chart form, which derives
 * the synthetic root and its missing parent without an authored sentinel ID.
 */
static const uint32_t TA_INDEX_NONE = UINT32_MAX;
static const uint64_t TA_REVISION_ANY = UINT64_MAX;
static const uint64_t TA_TIME_NEVER = UINT64_MAX;
static const size_t TA_UNLIMITED = (size_t)-1;

/** Standard processor/service URIs as named, NUL-terminated character arrays.
 * Array addresses are valid static initializers for registration type fields;
 * sizeof(array) - 1 gives a compile-time byte length for a ta_text_t slice.
 * Contents identify protocols; array addresses have no routing significance.
 */
static const char TA_SCXML_IOPROCESSOR[] =
    "http://www.w3.org/TR/scxml/#SCXMLEventProcessor";
static const char TA_SCXML_INVOKE_TYPE[] = "http://www.w3.org/TR/scxml/";
static const char TA_BASIC_HTTP_IOPROCESSOR[] =
    "http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor";

/** Non-NUL-terminated UTF-8 slice. {NULL,0} means absent; non-NULL with size 0
 * means present but empty. Names are case-sensitive byte strings, without NUL.
 * No locale processing, normalization, interning, or strlen is required.
 */
typedef struct ta_text { const char *data; size_t size; } ta_text_t;

/** Borrows a NUL-terminated string as a counted slice; NULL means absent and a
 * non-NULL empty string remains present. Scans through the first NUL without
 * libc, allocation or copying. The caller supplies valid terminated storage
 * and keeps it alive while the returned slice is used. For an already-counted
 * buffer, initialize ta_text_t directly to avoid scanning. For static text use
 * { "name", sizeof("name") - 1u }; a function call is not a C99 static initializer.
 */
static inline ta_text_t ta_text_from_cstr(const char *string)
{
    ta_text_t text = { string, 0u };
    if (string != NULL) {
        while (string[text.size] != '\0') ++text.size;
    }
    return text;
}

/** A native-language value handle, never a C representation of an object.
 * Zero means "no value"; a language's null/undefined must use a nonzero handle.
 * A binding may store a pointer, tagged immediate, or persistent handle-table
 * index here. Moving GCs must use stable roots/indirection. A programming-language
 * actor's state is exactly one such handle. Plain C actors may instead use the
 * owned byte-storage path below, without native roots or VM allocation.
 */
typedef struct ta_value { uintptr_t handle; } ta_value_t;

/** Recurring size/alignment request. Alignment is a nonzero power of two.
 * Use sizeof(T) and the implementation's alignment of T for a C entity struct.
 */
typedef struct ta_layout { size_t size; size_t alignment; } ta_layout_t;

/** Borrowed byte ranges. Empty input is {NULL,0}; nonempty input needs data.
 * Mutable views are granted only inside callbacks allowed to modify actor data.
 */
typedef struct ta_bytes { const void *data; size_t size; } ta_bytes_t;
typedef struct ta_mut_bytes { void *data; size_t size; } ta_mut_bytes_t;

typedef enum ta_data_storage {
    TA_DATA_NATIVE = 0,
    TA_DATA_BYTES
} ta_data_storage_t;

/** Per-definition actor data layout. NATIVE uses the host's object handle;
 * bytes_layout and initial_bytes must be empty. BYTES owns one aligned block
 * per actor, allocated together with its control record where alignment permits.
 * The core copies initial_bytes, or zero-fills if absent. A supplied image must
 * have exactly bytes_layout.size bytes. A zero-size byte model is allowed with
 * alignment 1. schema is an optional host identifier, not a reflection table.
 *
 * A C model can be struct { float x,y,vx,vy; uint32_t sprite,entity; }. No field
 * map, object wrapper or per-field callback is required: a run callback obtains
 * ta_context_bytes and updates the struct directly. Its storage lasts until
 * actor release or upgrade; state exits do not discard its fields.
 *
 * Bytes must be safe to copy as values. Initialize them as valid C objects before
 * reading typed fields (all-zero bytes are not portable null pointers/floats).
 * Prefer asset/entity IDs for sprite references. Byte copying does not retain
 * resources or copy mutable pointees. Owned pointers need native value hooks or
 * explicit host lifecycle handling; raw addresses are not a cross-system format.
 */
typedef struct ta_data_layout {
    ta_data_storage_t storage;
    ta_layout_t bytes_layout;
    ta_bytes_t initial_bytes;
    ta_text_t schema;
} ta_data_layout_t;

/** Borrowed data-model view shared by execution, inspection and migration.
 * Only the member selected by storage is valid. Native values are rooted;
 * byte views do not grant ownership and must not escape their documented scope.
 */
typedef struct ta_data_view {
    ta_data_storage_t storage;
    union { ta_value_t native; ta_bytes_t bytes; } as;
} ta_data_view_t;

typedef enum ta_payload_kind {
    TA_PAYLOAD_NONE = 0,
    TA_PAYLOAD_NATIVE,
    TA_PAYLOAD_BYTES
} ta_payload_kind_t;

/** Message content, separate from actor storage choice. NATIVE uses snapshot
 * hooks. BYTES is copied directly into the message lifetime before admission,
 * bypassing native value hooks; receivers get isolated copies exposed read-only.
 * Byte payloads obey the same copy-safe rule as C state and have no implicit
 * pointer/VM conversion. Native bind_event may translate bytes or report a
 * conversion error; C handlers can inspect the tagged content directly.
 * Byte ranges have alignment 1; copy into an aligned local struct before typed
 * access. A host-defined byte format can bridge simulation and renderer systems
 * without sharing either actor's mutable state or exposing a native handle.
 */
typedef struct ta_payload {
    ta_payload_kind_t kind;
    union { ta_value_t native; ta_bytes_t bytes; } as;
} ta_payload_t;

/** Opaque compiled expression/action token, interpreted only by the host.
 * Zero means absent. The containing definition roots its code_owner value.
 */
typedef uintptr_t ta_code_t;
typedef uint32_t ta_state_id_t;
typedef uint32_t ta_transition_id_t;

/** Runtime-local identities. Zero is invalid. IDs are never reused within a
 * system lifetime, including after actor deletion; exhaustion rejects creation.
 * They must be used with their originating system and are not native pointers.
 */
typedef struct ta_actor_ref { uint64_t id; } ta_actor_ref_t;
typedef struct ta_invocation_ref { uint64_t id; } ta_invocation_ref_t;
typedef struct ta_request_ref { uint64_t id; } ta_request_ref_t;

typedef struct ta_system ta_system_t;
typedef struct ta_arena ta_arena_t;
typedef struct ta_definition ta_definition_t;
typedef struct ta_context ta_context_t;
typedef struct ta_event ta_event_t;
typedef struct ta_definition_desc ta_definition_desc_t;

/** Expected results, not errno values. EMPTY means no item; AGAIN means a
 * caller-controlled boundary has not been reached. Neither is an SCXML error.
 */
typedef enum ta_status {
    TA_OK = 0,
    TA_EMPTY,
    TA_AGAIN,
    TA_INVALID,
    TA_UNSUPPORTED,
    TA_NOT_FOUND,
    TA_STALE,
    TA_CONFLICT,
    TA_BUSY,
    TA_NO_MEMORY,
    TA_LIMIT,
    TA_EXECUTION_ERROR,
    TA_COMMUNICATION_ERROR,
    TA_CANCELLED,
    TA_FAULTED
} ta_status_t;

/** Allocator-owned chunk descriptor, returned intact to release. */
typedef struct ta_chunk { void *data; ta_layout_t layout; } ta_chunk_t;

/** Allocation uses linked chunks, with bump allocation inside each chunk.
 * acquire returns TA_OK with a chunk at least the requested size/alignment, or
 * TA_NO_MEMORY with no block. release receives precisely the returned descriptor.
 * No realloc or individual-object free is required. If allocator rounding
 * makes the actual size exceed a cap, the core releases the unused
 * block and fails the request. Both callbacks are required and outlive the root
 * arena. They may use a fixed slab/block pool on an MCU or a host allocator in
 * a server; no libc fallback
 * exists. Alignment arguments are nonzero powers of two.
 */
typedef struct ta_chunk_allocator {
    void *user;
    ta_status_t (*acquire)(void *user, const ta_layout_t *minimum,
                           ta_chunk_t *out_chunk);
    void (*release)(void *user, const ta_chunk_t *chunk);
} ta_chunk_allocator_t;

/** Configured byte caps count acquired capacity, including headers, padding,
 * and unused chunk tails, not just requested payload bytes. Each allocation is
 * checked against this arena and every ancestor before it becomes visible.
 * initial_chunk_bytes is a growth preference, never permission to exceed a
 * cap. Values must satisfy initial <= maximum_chunk and
 * maximum_allocation <= maximum_chunk; zero capacity cannot create an arena.
 * Initial chunks are acquired lazily except for the arena's own control record.
 * A child's record lives in its own chunks, so closing children does not leave
 * an ever-growing trail of metadata in a long-lived parent.
 */
typedef struct ta_arena_options {
    size_t initial_chunk_bytes; /**< Preferred first chunk, including metadata. */
    size_t maximum_chunk_bytes; /**< Hard cap on any chunk; no oversized escape. */
    size_t maximum_bytes;       /**< Hard cap for this arena and its descendants. */
    size_t maximum_allocation;  /**< Cap for one bump allocation. */
} ta_arena_options_t;

typedef struct ta_memory_stats {
    size_t reserved_bytes;      /**< Current capacity across the subtree. */
    size_t peak_reserved_bytes;
    size_t used_bytes;          /**< Bumps, headers and cleanup records in use. */
    size_t chunk_count;
    size_t descendant_count;
    uint64_t failed_allocations;
} ta_memory_stats_t;

/** Root allocator and children form a lifetime tree. Each child owns a separate
 * chunk chain and inherits its root's allocator. Siblings can close in any
 * order, without moving or invalidating the other's objects. No allocation can
 * outlive its arena or be "promoted" by pointer; copy into an ancestor instead.
 * All output arenas are owned by the caller until close, unless marked borrowed.
 */
TA_API ta_status_t ta_arena_create(const ta_chunk_allocator_t *allocator,
                                 const ta_arena_options_t *options,
                                 ta_arena_t **out_arena);
TA_API ta_status_t ta_arena_open(ta_arena_t *parent,
                               const ta_arena_options_t *options,
                               ta_arena_t **out_child);
TA_API ta_status_t ta_arena_alloc(ta_arena_t *arena, const ta_layout_t *layout,
                                void **out_memory);

/** Registers infallible, nonblocking cleanup, executed once in reverse
 * registration order. Register before publishing a resource; if registration
 * fails the caller still owns the resource. Cleanup must not allocate in or
 * mutate the closing tree. On close, children close in reverse creation order,
 * then this arena's cleanups run while its memory and ancestors are still live,
 * then its chunks are released. Cleanup can release host handles/resources.
 */
TA_API ta_status_t ta_arena_defer(ta_arena_t *arena,
                                void (*cleanup)(void *), void *argument);
TA_API void ta_arena_close(ta_arena_t *arena);
TA_API void ta_arena_stats(const ta_arena_t *arena, ta_memory_stats_t *out_stats);

/** Lowering a cap below current usage fails atomically. Arithmetic overflow,
 * unsupported alignment and zero-byte allocation are rejected; nothing wraps.
 * Raising a cap cannot override an ancestor cap or allocator exhaustion.
 */
TA_API ta_status_t ta_arena_set_limit(ta_arena_t *arena, size_t maximum_bytes);

/** Arena profiles control granularity, not preallocation. Optional features
 * allocate only when used. Arenas borrowed from the runtime must never be
 * closed by the host. System-owned chunks must not be reset behind its back.
 * The tree below describes lifetimes, not mandatory allocations. Actor control,
 * initial arena bookkeeping and C state share the actor's first chunk. Logical
 * activation/macrostep scopes are materialized only when storage is requested;
 * entering a state alone does not allocate an activation arena. A host chunk
 * allocator may obtain small actor chunks from a reusable slab pool. Closing
 * an actor returns its chunk to that pool; no record remains in a parent bump.
 *
 * system
 *   definition revision / registry entry
 *   actor instance
 *     native-root cleanup / history / late-binding flags
 *     state activation (nested under its active parent)
 *     queued message / delayed send / current _event
 *     macrostep scratch (survives scheduler yields)
 *   outstanding host request / external invocation / trace span
 *   upgrade staging
 *
 * Message siblings close individually. The last dequeued event is kept until
 * the next event replaces _event, even when the actor is idle. Activation
 * arenas close after onexit and invocation cancellation; declarations and
 * history belong to the actor, not to activation arenas. Outstanding host
 * request memory belongs to the system, so a host lease can outlive its actor.
 */
typedef enum ta_lifetime {
    TA_LIFETIME_SYSTEM = 0,
    TA_LIFETIME_DEFINITION,
    TA_LIFETIME_ACTOR,
    TA_LIFETIME_ACTIVATION,
    TA_LIFETIME_MESSAGE,
    TA_LIFETIME_MACROSTEP,
    TA_LIFETIME_IO,
    TA_LIFETIME_TRACE,
    TA_LIFETIME_UPGRADE,
    TA_LIFETIME_COUNT
} ta_lifetime_t;

/** C-owned memory and host-native memory are different budgets. These counters
 * include core-owned C state and byte message payloads. A VM must separately
 * bound GC heap, native object size, code, callback stack, serialization and
 * exporter queues. External resources referenced by bytes are not charged.
 * Borrowed definition tables are also outside acquired-byte accounting.
 * Zero is an actual zero limit (feature disabled); TA_UNLIMITED removes a cap.
 */
typedef struct ta_actor_limits {
    size_t maximum_bytes;          /**< Actor subtree, including queued payload metadata. */
    size_t external_events;
    size_t internal_events;        /**< Excludes one reserved platform-error slot. */
    size_t queued_event_bytes;     /**< Sum of message arena capacities in both FIFOs. */
    size_t delayed_sends;
    size_t invocations;
    size_t io_requests;            /**< Charged to originator even if memory is detached. */
    size_t io_request_bytes;       /**< Detached request/invocation capacity, per originator. */
    size_t microsteps_per_macrostep;
    size_t actions_per_macrostep;  /**< Includes foreach iterations and nested actions. */
} ta_actor_limits_t;

typedef struct ta_system_limits {
    size_t maximum_bytes;          /**< Entire system subtree, all revisions included. */
    size_t actors;                 /**< Includes terminated actors not yet released. */
    size_t definitions;            /**< Live revision objects, not just registry keys. */
    size_t names;
    size_t queued_events;
    size_t delayed_sends;
    size_t invocations;
    size_t io_requests;            /**< Pending, taken, and reserved cancellation records. */
    size_t definition_states;
    size_t definition_transitions;
    size_t definition_actions;
    size_t state_depth;            /**< Explicit traversal stack, not C recursion. */
    size_t action_depth;
    size_t event_name_bytes;
    size_t trace_metadata_bytes;   /**< Per-message tracestate plus baggage. */
} ta_system_limits_t;

/** Out-of-band report, usable when even an error event cannot be allocated.
 * Text is borrowed through the callback; callbacks must not allocate in the
 * failing system. These reports are observations, not events sent to actors.
 */
typedef struct ta_diagnostic {
    ta_status_t status;
    ta_actor_ref_t actor;          /**< Zero for definition/system errors. */
    uint64_t revision;
    ta_state_id_t state;          /**< TA_INDEX_NONE when inapplicable. */
    ta_transition_id_t transition;
    ta_text_t detail;
} ta_diagnostic_t;

/** W3C propagation data, optional on each message (no permanent per-actor span).
 * IDs are byte arrays in wire order, not integers; all-zero IDs mean absent.
 * trace_flags carries sampling bits, including unsampled propagation. Host
 * adapters validate/extract/inject traceparent, tracestate and baggage; the core
 * copies these slices and never parses transport headers. Metadata caps apply.
 * See https://www.w3.org/TR/trace-context/ .
 */
typedef struct ta_trace_context {
    uint8_t trace_id[16];
    uint8_t span_id[8];
    uint8_t trace_flags;
    uint8_t remote;
    ta_text_t tracestate;
    ta_text_t baggage;
} ta_trace_context_t;

typedef enum ta_event_type {
    TA_EVENT_EXTERNAL = 0,
    TA_EVENT_INTERNAL,
    TA_EVENT_PLATFORM
} ta_event_type_t;

/** SCXML _event view. Queue placement and event type are separate concepts:
 * platform errors go into the internal queue; autoforward copies all seven
 * SCXML fields unchanged. Strings and values belong to the containing message.
 * origin/origintype are absent on internal/platform events. invokeid is assigned
 * by authenticated invocation routing, not accepted as proof from an ingress.
 * message_id and trace are platform metadata exposed under _x, not new SCXML
 * fields. An unmatched event still replaces _event.
 */
struct ta_event {
    ta_text_t name;
    ta_event_type_t type;
    ta_text_t sendid;
    ta_text_t origin;
    ta_text_t origintype;
    ta_text_t invokeid;
    ta_payload_t data;
    uint64_t message_id;
    const ta_trace_context_t *trace;
};

/** Concrete address. NAME denotes the current instance registered under a name;
 * INSTANCE denotes exactly one incarnation and never follows replacement.
 * SCXML _name is definition metadata, not a registry key or an instance name.
 * Missing raw target means self's external FIFO; #_internal is never "self".
 * NAME is an API extension and has no reserved SCXML string spelling.
 */
typedef enum ta_target_kind {
    TA_TARGET_SELF = 0,
    TA_TARGET_INTERNAL,
    TA_TARGET_INSTANCE,
    TA_TARGET_NAME,
    TA_TARGET_PARENT,
    TA_TARGET_INVOKE,
    TA_TARGET_URI
} ta_target_kind_t;

typedef struct ta_target {
    ta_target_kind_t kind;
    ta_actor_ref_t instance; /**< INSTANCE only. */
    ta_text_t text;          /**< Name, invoke ID, or processor-specific URI. */
} ta_target_t;

/** SCXML param/namelist data is an ordered sequence, not a C map; duplicates
 * must survive until the receiving binding/processor applies its own mapping.
 */
typedef struct ta_parameter { ta_text_t name; ta_value_t value; } ta_parameter_t;

typedef enum ta_body_kind {
    TA_BODY_NONE = 0,
    TA_BODY_CONTENT,
    TA_BODY_PARAMETERS
} ta_body_kind_t;

typedef struct ta_body {
    ta_body_kind_t kind;
    ta_payload_t content;
    const ta_parameter_t *parameters;
    size_t parameter_count;
} ta_body_t;

/** Literal-or-expression operands. Expression and literal are mutually
 * exclusive; both absent represents omission, not an empty string. A text
 * expression is evaluated and converted by the binding at the execution point.
 */
typedef struct ta_text_expr { ta_text_t literal; ta_code_t expression; } ta_text_expr_t;

/** Named value source. Exactly one of expression or location is nonzero.
 * A namelist entry is represented by name plus location. Invoke finalize uses
 * location entries for its automatic return-value assignments.
 */
typedef struct ta_parameter_expr {
    ta_text_t name;
    ta_code_t expression;
    ta_code_t location;
} ta_parameter_expr_t;

/** Normalized content OR ordered param/namelist list. Host compilers resolve
 * resources needed by synchronous expressions before execution; the core never
 * fetches a data/script src or blocks a macrostep waiting for one.
 * For CONTENT exactly one of content/bytes_content is nonzero and parameters
 * are absent. For PARAMETERS both content tokens are zero. Byte expressions
 * produce snapshots at send/invoke evaluation time, just like native content.
 * Native expressions/parameter values require installed native value hooks even
 * when the actor itself uses byte storage. NONE has no content or parameters.
 */
typedef struct ta_body_expr {
    ta_body_kind_t kind;
    ta_code_t content;             /**< Native expression; exclusive with bytes_content. */
    ta_code_t bytes_content;       /**< Byte expression, evaluated via evaluate_bytes. */
    const ta_parameter_expr_t *parameters;
    uint32_t parameter_count;
} ta_body_expr_t;

typedef struct ta_action ta_action_t;
typedef struct ta_block { const ta_action_t *actions; uint32_t count; } ta_block_t;
typedef struct ta_blocks { const ta_block_t *blocks; uint32_t count; } ta_blocks_t;

/** Each onentry/onexit block is a distinct SCXML error boundary. An error aborts
 * the rest of that block, including enclosing if/foreach bodies, but does not
 * skip other blocks, transitions or required entry/exit work in the microstep.
 * Completed actions are not rolled back. Guard failure is false plus a queued
 * error.execution. A missing guard is true.
 */
typedef struct ta_branch { ta_code_t condition; ta_block_t body; } ta_branch_t;

/** Send arguments are evaluated once when executed, including payload, target,
 * type, delay and idlocation. Snapshots are made then, not when a timer fires.
 * An omitted type selects TA_SCXML_IOPROCESSOR. Literal delay is nanoseconds;
 * an expression is converted by to_delay (e.g. SCXML "1.5s" or "20ms").
 * delay_expression and nonzero delay_ns are mutually exclusive. IDs without
 * idlocation are literals; both absent means no SCXML sendid, though the core
 * still assigns an internal message_id. Explicit duplicate sendids are allowed.
 *
 * For NAME, evaluate name_expression (or use target.text), retaining the name
 * until dispatch. Resolve it once at actual dispatch; queue admission pins that
 * incarnation. Rebinding cannot redirect already queued messages. For URI,
 * target_expression replaces target.text. Other kinds forbid expressions.
 */
typedef struct ta_send_action {
    ta_text_expr_t event;
    ta_text_expr_t type;
    ta_target_t target;
    ta_code_t target_expression;
    ta_body_expr_t body;
    uint64_t delay_ns;
    ta_code_t delay_expression;
    ta_text_t id;
    ta_code_t idlocation;
} ta_send_action_t;

typedef enum ta_action_kind {
    TA_ACTION_RAISE = 0,
    TA_ACTION_ASSIGN,
    TA_ACTION_SCRIPT,
    TA_ACTION_IF,
    TA_ACTION_FOREACH,
    TA_ACTION_LOG,
    TA_ACTION_SEND,
    TA_ACTION_CANCEL,
    TA_ACTION_CUSTOM
} ta_action_kind_t;

/** Typed executable content. Unknown action kinds are rejected during prepare;
 * opaque code tokens are checked by validate_code, or when first executed.
 * IF evaluates branches in order, selecting the first true condition; condition
 * zero is an optional last else. FOREACH takes a shallow iteration snapshot;
 * changes to the source collection do not change iteration membership/order.
 * item/index are host variable names, declared on demand, not location tokens.
 * LOG is a host observation callback, not stdout. CUSTOM obeys the same state
 * isolation/error rules as SCRIPT and cannot directly change configuration.
 */
struct ta_action {
    ta_action_kind_t kind;
    union {
        struct { ta_text_t event; } raise;
        struct { ta_code_t location; ta_code_t value; } assign;
        struct { ta_code_t code; } script;
        struct { const ta_branch_t *branches; uint32_t count; } conditional;
        struct {
            ta_code_t collection;
            ta_text_t item;
            ta_text_t index;
            ta_block_t body;
        } foreach_loop;
        struct { ta_text_t label; ta_code_t value; } log;
        ta_send_action_t send;
        struct { ta_text_expr_t sendid; } cancel;
        struct { ta_code_t code; } custom;
    } as;
};

typedef enum ta_state_kind {
    TA_STATE_ROOT = 0,
    TA_STATE_ATOMIC,
    TA_STATE_COMPOUND,
    TA_STATE_PARALLEL,
    TA_STATE_FINAL,
    TA_STATE_HISTORY_SHALLOW,
    TA_STATE_HISTORY_DEEP
} ta_state_kind_t;

typedef enum ta_transition_kind {
    TA_TRANSITION_EXTERNAL = 0,
    TA_TRANSITION_INTERNAL
} ta_transition_kind_t;

/** Index list is a legal state specification, not necessarily a full active
 * configuration. Targets must be distinct and cannot contain ancestor/child
 * pairs. Filling in ancestors and default descendants must give one active
 * child per compound state and all regions of each active parallel state.
 */
typedef struct ta_targets { const ta_state_id_t *states; uint32_t count; } ta_targets_t;

/** Initial/history defaults have targets and actions but no event or guard.
 * A compound/root with an empty initial list defaults to its first real child;
 * that shorthand must have an empty block. Explicit initial targets are proper
 * descendants of their owner. History requires a nonempty default: immediate
 * real children of its parent for shallow history, proper descendants for deep
 * history, forming a legal state specification in either case.
 */
typedef struct ta_initial { ta_targets_t targets; ta_block_t content; } ta_initial_t;

/** Descriptor count zero means eventless, never wildcard. Each descriptor is
 * one token-prefix pattern, not a whitespace-separated list. "a", "a." and
 * "a.*" match "a" and "a.b", but not "ab"; "*" matches any named event.
 * No eventless pseudo-event is ever inserted in a FIFO or bound to _event.
 * A transition must specify at least one descriptor, condition, or target.
 *
 * An internal transition preserves its source only when that source is compound
 * and all effective targets are proper descendants. Otherwise its domain is
 * the least common compound ancestor (LCCA), just like an external transition.
 * The LCCA is a proper ancestor and is never a parallel state. History targets
 * are resolved before domain/exit-set calculation.
 */
typedef struct ta_transition_desc {
    ta_state_id_t source;
    ta_transition_kind_t kind;
    const ta_text_t *events;
    uint32_t event_count;
    ta_code_t condition;
    ta_targets_t targets;
    ta_block_t content;
} ta_transition_desc_t;

/** Declaration identity is stable text, not an arena address. All declarations
 * are created at actor initialization, even with late binding. Zero initializer
 * requests the binding's empty value. Reserved system-variable names beginning
 * with '_' cannot be declared. Declarations live for the entire session.
 */
typedef struct ta_data_desc { ta_text_t id; ta_code_t initializer; } ta_data_desc_t;

typedef enum ta_finalize_kind {
    TA_FINALIZE_ABSENT = 0,
    TA_FINALIZE_AUTOMATIC, /**< Present but empty: assign matching returned params. */
    TA_FINALIZE_BLOCK
} ta_finalize_kind_t;

/** Invocation is a state-owned service lifetime, not a send action. It starts
 * once after entry stabilizes. Exiting the owner cancels it after onexit. Each
 * re-entry gets a fresh runtime invocation identity even with a reused text id.
 * id/idlocation are exclusive; generated IDs use state-name.unique-suffix.
 *
 * SCXML invocation type (also "scxml", and the default) starts a local actor.
 * Its arena is a system child and its limits copy actor_defaults; parent limits
 * bound the invocation record, not its descendants' memory. Global actor/byte
 * caps therefore also bound recursive invocation. Cancellation of a parent
 * requests child stop without requiring the parent's arena to stay allocated.
 * source is a definition registry key resolved at invocation time, or
 * inline_definition pins an immutable prepared revision; these are exclusive.
 * This is the non-XML equivalent of src/inline executable document content.
 * For other service types source/body are interpreted by the host service.
 * SCXML body parameters override only matching root data declarations; unmatched
 * parameters are not implicitly added to the child's data model. For SCXML,
 * body is NONE or PARAMETERS; inline_definition represents executable content,
 * so BODY_CONTENT is not a second executable-document representation.
 *
 * Finalize runs only for this live invocation's returned events, after binding
 * _event and before transition selection. Automatic finalize assigns matching
 * names to the original location entries. An absent finalize does nothing.
 * Finalize may manipulate data but must not raise/send/cancel or request external
 * actions, including indirectly through scripts/custom code. The core rejects
 * forbidden context operations in this phase; the host must enforce the same
 * restriction on native code. Autoforward copies every dequeued external event
 * after finalize, preserving all SCXML fields and creation trace context.
 */
typedef struct ta_invoke_desc {
    ta_text_expr_t type;
    ta_text_expr_t source;
    ta_definition_t *inline_definition;
    ta_body_expr_t body;
    ta_text_t id;
    ta_code_t idlocation;
    uint8_t autoforward;
    ta_finalize_kind_t finalize_kind;
    ta_block_t finalize;
} ta_invoke_desc_t;

/** states[0] is the synthetic root (parent TA_INDEX_NONE). Remaining entries
 * are preorder, parents precede children, and subtrees are contiguous. State
 * names are unique stable IDs, including history pseudo-states. Final states
 * have no children/transitions/invokes/data. Parallel states have real children
 * but no initial transition. History states have only name/parent/kind/default.
 * Root children are atomic/compound/parallel/final; final's parent must be root
 * or compound; history's parent must be compound or parallel. Every compound
 * and parallel has real children; root has at least one real child.
 * A history child records immediate active children (shallow) or active atomic
 * descendants (deep) BEFORE any exits; it is never itself active. Restoration
 * uses the stored set or executes the default content after parent entry and
 * initial content. History defaults must not form recursive target cycles.
 *
 * Entering a final child queues done.state.<parent> after onentry, propagating
 * completion through parallel ancestors when every region is final. A top-level
 * final ends the session, runs termination exits, and sends done.invoke.<id>
 * with its done_data to the parent only on normal completion. Cancellation runs
 * exits but never sends done.invoke. No events execute after termination.
 * Non-top-level done_data is evaluated after final onentry; top-level done_data
 * is evaluated after that final's termination onexit, when returning the result.
 */
typedef struct ta_state_desc {
    ta_text_t name;
    ta_state_id_t parent;
    ta_state_kind_t kind;
    ta_initial_t initial;          /**< Also history's default transition. */
    uint32_t first_transition;     /**< Slice of definition.transitions. */
    uint32_t transition_count;
    ta_blocks_t onentry;
    ta_blocks_t onexit;
    const ta_data_desc_t *data;
    uint32_t data_count;
    const ta_invoke_desc_t *invokes;
    uint32_t invoke_count;
    ta_body_expr_t done_data;      /**< Final states only. */
} ta_state_desc_t;

typedef enum ta_binding { TA_BINDING_EARLY = 0, TA_BINDING_LATE } ta_binding_t;

/** State data initializers use document order. Early binding initializes every
 * declaration before root_script. Late binding initializes root data before
 * root_script and other data on the owner's FIRST entry, before onentry. Exit
 * does not undeclare/reset data. Root has no onentry/onexit/invokes/transitions;
 * its initial content, if any, is the normalized startup transition.
 */
struct ta_definition_desc {
    ta_text_t name;                /**< SCXML _name; immutable for a session. */
    ta_text_t data_model;          /**< Must equal the installed binding's name. */
    ta_value_t code_owner;         /**< Retained native compiled-code/module handle. */
    ta_data_layout_t data_layout;
    ta_binding_t binding;
    const ta_state_desc_t *states;
    uint32_t state_count;
    const ta_transition_desc_t *transitions;
    uint32_t transition_count;
    ta_code_t root_script;
};

typedef enum ta_definition_storage {
    TA_DEFINITION_COPY = 0,        /**< Deep-copy tables, text and nested blocks. */
    TA_DEFINITION_BORROW          /**< Tables in ROM or host-pinned immutable storage. */
} ta_definition_storage_t;

/** Borrowed descriptor graph must remain immutable until storage_release runs
 * exactly once after the last definition reference. Success assumes that lease;
 * failure does not call storage_release. NULL release permits permanent ROM.
 * The descriptor itself and callback userdata follow the same lifetime rule.
 * COPY also copies the initial byte image and schema text, and ignores
 * storage_user/storage_release. Both modes retain nonzero code_owner and
 * inline definitions; immutable metadata is shared by all actors of a revision.
 */
typedef struct ta_definition_options {
    ta_definition_storage_t storage;
    void *storage_user;
    void (*storage_release)(void *user);
} ta_definition_options_t;

/** Preparation input groups the descriptor and its storage lease. */
typedef struct ta_definition_input {
    const ta_definition_desc_t *description;
    ta_definition_options_t options;
} ta_definition_input_t;

/** definition is owned on success, NULL on failure. error is populated on
 * failure and has only static/borrowed detail valid through the next API call.
 */
typedef struct ta_definition_result {
    ta_definition_t *definition;
    ta_diagnostic_t error;
} ta_definition_result_t;

/** Validates graph shape, ordering, bounds, legal target sets/defaults, duplicate
 * IDs, expression availability, action nesting and finalize restrictions before
 * exposing a definition. It precomputes shared topology/dispatch metadata. No
 * actor actions run. Failure fills result.error with indices and detail.
 * A successful prepared definition owns one caller reference and a unique
 * revision number; preparation does not publish it in the registry.
 * Revisions reserve zero and TA_REVISION_ANY. Descriptions returned by the
 * accessor are borrowed while a definition reference remains live.
 */
TA_API ta_status_t ta_definition_prepare(ta_system_t *system,
                                       const ta_definition_input_t *input,
                                       ta_definition_result_t *out_result);

/** Nested authoring form, corresponding to state, parallel, final and history.
 * STATE infers atomic versus compound from the presence of real children;
 * history children alone do not make a valid compound state. The root is
 * implicit in ta_chart_t and is never supplied as a child. Other kinds retain
 * the structural restrictions documented on ta_state_desc_t.
 * Authoring-only const char * fields below are NUL-terminated UTF-8 strings;
 * NULL means absent. This lets designated initializers use string literals
 * directly. Build scans/copies them without libc into the runtime's ta_text_t
 * slices, subject to memory and event-name limits. Other reused semantic structs
 * continue to use counted text slices. Input strings must have valid terminators.
 */
typedef enum ta_chart_state_kind {
    TA_CHART_STATE = 0,
    TA_CHART_PARALLEL,
    TA_CHART_FINAL,
    TA_CHART_HISTORY_SHALLOW,
    TA_CHART_HISTORY_DEEP
} ta_chart_state_kind_t;

/** Initial transition, also used for a history node's default transition.
 * target names one or more state IDs separated by ASCII space, tab, CR or LF.
 * It is resolved at build time, not at actor entry. An absent target and empty
 * content select the first real child for a root/compound state. A history
 * default requires targets. Other restrictions are those of ta_initial_t.
 */
typedef struct ta_chart_initial {
    const char *target;
    ta_block_t content;
} ta_chart_initial_t;

/** A transition belongs to the state containing it; no source index is needed.
 * event is a whitespace-separated list of SCXML event descriptors; target is
 * a whitespace-separated list of definition-global state IDs. The separators
 * are exactly ASCII space, tab, CR and LF. Surrounding/repeated separators are
 * ignored, but a present empty/whitespace-only list is invalid. Omit event for
 * an eventless transition; omit target for a targetless transition. Patterns
 * retain ta_transition_desc_t semantics; target IDs are exact, not patterns,
 * actor names, relative paths or runtime address expressions. Forward references
 * are allowed. condition is a host code token, never a parsed C/string expression.
 * Zero kind means EXTERNAL; targetless transitions still perform no exit/entry.
 */
typedef struct ta_chart_transition {
    ta_transition_kind_t kind;
    const char *event;
    ta_code_t condition;
    const char *target;
    ta_block_t content;
} ta_chart_transition_t;

typedef struct ta_chart_state ta_chart_state_t;

/** Data declarations and invokes reuse the counted semantic descriptor form.
 * Unlike state/transition authoring lists, these views have explicit counts.
 */
typedef struct ta_chart_data {
    const ta_data_desc_t *items;
    uint32_t count;
} ta_chart_data_t;
typedef struct ta_chart_invokes {
    const ta_invoke_desc_t *items;
    uint32_t count;
} ta_chart_invokes_t;

/** One nested state element. id is a nonempty, unique definition-global ID
 * without list separators; "$root" is reserved for the generated root. Child
 * order defines preorder state IDs. states and transitions point to ordered
 * arrays, each terminated by a final {0} element; NULL means an empty list.
 * A state terminator has every field absent/default, including a NULL id; any
 * non-terminator needs an id. A transition terminator likewise has every field
 * absent/default, including NULL event/target, zero condition, EXTERNAL kind
 * and empty content. A target or condition therefore makes an eventless
 * transition a real element, not a terminator. These are typed zero initializers,
 * not bytewise tests of struct padding or null-pointer representations.
 * The caller must provide valid storage through the terminator; the builder
 * stops there and never examines subsequent elements. No authored topology
 * counts, source indices, parent indices or transition ranges are required.
 *
 * Transitions within each state preserve array order. onentry/onexit reuse
 * counted lists of blocks, not one concatenated
 * action list: an error aborts one block without skipping subsequent blocks.
 * Actions, data declarations, invokes, finalize and done_data reuse the same
 * semantic structs as the normalized form. An inline invoke refers to a
 * separately prepared definition, allowing immutable revisions to be shared.
 * The tree does not acquire a separate expression language or data model.
 */
struct ta_chart_state {
    const char *id;
    ta_chart_state_kind_t kind;
    ta_chart_initial_t initial;
    const ta_chart_state_t *states;       /**< Child array ending in {0}, or NULL. */
    const ta_chart_transition_t *transitions; /**< Array ending in {0}, or NULL. */
    ta_blocks_t onentry;
    ta_blocks_t onexit;
    ta_chart_data_t data;
    ta_chart_invokes_t invokes;
    ta_body_expr_t done_data;
};

/** The non-XML equivalent of an scxml document. name is SCXML _name, not a
 * state ID or actor registry name. states are the root's children; initial and
 * data belong to that root. All other metadata has ta_definition_desc_t meaning.
 * Default binding is EARLY. Changing this input later never changes a built
 * definition: build a new revision and publish/upgrade through the normal API.
 */
typedef struct ta_chart {
    const char *name;
    const char *data_model;
    ta_value_t code_owner;
    ta_data_layout_t data_layout;
    ta_binding_t binding;
    ta_chart_initial_t initial;
    const ta_chart_state_t *states;       /**< Root children, ending in {0}. */
    ta_chart_data_t data;
    ta_code_t root_script;
} ta_chart_t;

/** Builds one shared, immutable definition from nested named initializers.
 * Synthesizes states[0] with name "$root", assigns remaining state IDs in
 * child preorder, derives parents and contiguous subtrees, and concatenates
 * transition lists in source-state preorder to derive sources/ranges. It splits
 * event lists and resolves all target/initial/history IDs, including forward
 * references, before applying all ta_definition_prepare validation. The input
 * must be an acyclic tree: reused child nodes/multiple parents, duplicate IDs,
 * unknown IDs and illegal target configurations are errors, not implicit clones.
 * Generated root is not a legal target. Root/child defaults, parallel regions,
 * history and executable block boundaries retain their normalized semantics.
 *
 * This is always a deep-copy operation. The entire authoring tree, strings,
 * byte image and nested executable descriptors need live only through this
 * call; success retains code_owner and inline definition references as prepare
 * does. It never retains authoring-node pointers. For already normalized tables
 * in ROM, use ta_definition_prepare with BORROW to avoid copying those tables.
 * File-scope C99 compound literals have static lifetime; block-scope literals
 * last through their enclosing block. Both work without lifetime extensions.
 * C++ callers can instead use named sentinel-terminated arrays and ordinary
 * aggregate initialization. No construction macros are used.
 *
 * Traversal uses a bounded explicit stack, not C recursion. Temporary name
 * resolution/traversal storage lives in a nested staging arena and is reclaimed
 * before return, including on failure. Staging and output count against system
 * byte caps and the definition arena profile; state/transition/action/depth
 * limits apply during construction, before unbounded traversal/allocation.
 * List scanning is bounded by those limits, not an unchecked search for a
 * terminator; limits do not make a missing terminator or invalid C pointer safe.
 * No per-actor construction tree or name-resolution overhead is introduced.
 *
 * Success returns one caller-owned revision, not a running actor; publish it or
 * pass it to spawn. No actor code executes and nothing is published implicitly.
 * Failure releases all staging/output resources and retained references, leaving
 * existing definitions/actors unchanged. out_result follows prepare's contract;
 * diagnostics use generated preorder indices where available, otherwise
 * TA_INDEX_NONE. Diagnostic detail must not dangle into released authoring data.
 */
TA_API ta_status_t ta_definition_build(ta_system_t *system,
                                     const ta_chart_t *chart,
                                     ta_definition_result_t *out_result);

TA_API void ta_definition_retain(ta_definition_t *definition);
TA_API void ta_definition_release(ta_definition_t *definition);
TA_API uint64_t ta_definition_revision(const ta_definition_t *definition);
TA_API const ta_definition_desc_t *ta_definition_description(const ta_definition_t *definition);

/** Baseline for a freshly spawned anonymous actor, before it is first run,
 * with no seed, events, tracing or optional processor locations. No callbacks
 * or actor allocation occur. control_bytes includes mandatory arena/control/
 * alignment overhead; state_bytes is the owned byte block, zero for native
 * storage. minimum_chunk_bytes is their combined allocation request including
 * all required padding; it is independent of the preferred initial chunk size.
 * Actual allocator rounding and later configuration/history/queue/scratch use
 * are additional and visible in statistics. Native VM memory is excluded.
 * Use this to check the requested baseline for 200000 actors before admitting
 * them; it reports an implementation fact, not an unmeasured fixed-size promise.
 */
typedef struct ta_actor_footprint {
    size_t control_bytes;
    size_t state_bytes;
    size_t minimum_chunk_bytes;
} ta_actor_footprint_t;

TA_API void ta_definition_footprint(const ta_definition_t *definition,
                                   ta_actor_footprint_t *out_footprint);

/** Registry key plus optimistic revision check, reused for publish/unpublish. */
typedef struct ta_definition_binding {
    ta_text_t key;
    uint64_t expected_revision;
} ta_definition_binding_t;

/** Registry compare-and-swap, serialized with sends/spawns. expected_revision
 * zero means absent, TA_REVISION_ANY means unconditional. The system retains
 * the new definition before releasing the previous binding. Existing actors,
 * inline invokes, requests and explicit references pin their old revisions.
 * This operation executes no migration or actor actions. Unpublish prevents
 * future lookup but never invalidates a pinned definition. Keys are copied.
 */
TA_API ta_status_t ta_definition_publish(ta_system_t *system,
                                       const ta_definition_binding_t *binding,
                                       ta_definition_t *definition);
TA_API ta_status_t ta_definition_unpublish(ta_system_t *system,
                                         const ta_definition_binding_t *binding);
/** Lookup returns an OWNED reference, released by ta_definition_release. */
TA_API ta_status_t ta_definition_lookup(ta_system_t *system, ta_text_t key,
                                      ta_definition_t **out_definition);

/** Processor locations are fixed for an actor's session and exposed through
 * _ioprocessors[canonical_type].location. The built-in SCXML entry is always
 * present; its location is #_scxml_<sessionid>. Additional locations are host
 * values. A missing location means that processor is send-only for this actor.
 */
typedef struct ta_io_location { ta_text_t type; ta_text_t location; } ta_io_location_t;

/** System values are read-only in the native model; _x contains platform
 * extensions. _event starts unbound and remains the last dequeued event until
 * replaced, including during eventless transitions and idle periods.
 */
typedef struct ta_system_values {
    ta_text_t sessionid;
    ta_text_t name;
    const ta_io_location_t *ioprocessors;
    size_t ioprocessor_count;
    const ta_event_t *event;       /**< NULL means _event is still unbound. */
    ta_actor_ref_t actor;          /**< Expose underneath _x. */
    uint64_t revision;             /**< Expose underneath _x; can change on upgrade. */
} ta_system_values_t;

/** Execution phase lets bindings enforce restrictions, attribute exceptions and
 * expose In(state-name). A context and all its borrowed views expire when the
 * callback returns. Saving a context pointer for an async operation is invalid.
 */
typedef enum ta_phase {
    TA_PHASE_INITIALIZE = 0,
    TA_PHASE_ROOT_SCRIPT,
    TA_PHASE_GUARD,
    TA_PHASE_ONENTRY,
    TA_PHASE_ONEXIT,
    TA_PHASE_TRANSITION,
    TA_PHASE_FINALIZE,
    TA_PHASE_INVOKE,
    TA_PHASE_DONE_DATA,
    TA_PHASE_MIGRATE
} ta_phase_t;

typedef struct ta_context_view {
    ta_actor_ref_t actor;
    const ta_definition_t *definition;
    ta_data_view_t state;          /**< Borrowed native handle or read-only C bytes. */
    const ta_system_values_t *system;
    ta_phase_t phase;
    ta_state_id_t state_id;        /**< Owner of currently executing content. */
    ta_transition_id_t transition_id;
    uint64_t now_ns;               /**< Last host-supplied monotonic time. */
    const ta_trace_context_t *trace; /**< Active macrostep's context, if present. */
} ta_context_view_t;

TA_API void ta_context_inspect(const ta_context_t *context, ta_context_view_t *out_view);
TA_API uint8_t ta_context_in(const ta_context_t *context, ta_text_t state_name);
/** Data callbacks obtain their embedding's userdata from this context. */
TA_API void *ta_context_host_user(const ta_context_t *context);

/** Direct mutable access to a C actor's state, valid through this callback.
 * Returns UNSUPPORTED for native storage and INVALID in read-only phases such
 * as guards. A C script/custom handler can cast data to its definition's struct
 * type and update position/velocity in one call, without field dispatch.
 */
TA_API ta_status_t ta_context_bytes(ta_context_t *context, ta_mut_bytes_t *out_bytes);

/** Lazily materializes a borrowed actor, activation or macrostep arena. Other
 * lifetimes are invalid here. Actor storage persists to release. Activation
 * returns NOT_FOUND unless the owner is active (including during its onexit).
 * A transition's source may already be exited; use macrostep scratch instead.
 * Allocation failure is explicit and follows the execution resource policy.
 * Do not store scratch pointers in longer-lived objects.
 */
TA_API ta_status_t ta_context_arena(ta_context_t *context, ta_lifetime_t lifetime,
                                  ta_arena_t **out_arena);

/** Value operations run in one binding/VM domain per system. retain/release are
 * infallible root operations (they may be no-ops for immediates). All three
 * callbacks are required together when native values are used. A pure byte
 * system can omit the entire table; byte state/messages never call these hooks.
 * A library-owned reference
 * receives exactly one release when its arena/lifetime ends, even on failure.
 * Callbacks returning out_value return ONE OWNED root on TA_OK, or no root on
 * failure. Input values are borrowed and may not be consumed.
 *
 * snapshot must create an isolation-safe copy of the entire reachable mutable
 * value, or an immutable/COW equivalent. Retaining the same mutable object is
 * insufficient. It preserves native value semantics and rejects uncopyable
 * values. Called when accepting sends/posts/invoke arguments, before a delay;
 * each receiver gets independent mutable data, even within a shared VM. The
 * first snapshot is also isolated from subsequent sender mutation. The core
 * does not serialize, trace, or inspect a native object to implement this.
 */
typedef struct ta_value_ops {
    void (*retain)(void *user, ta_value_t value);
    void (*release)(void *user, ta_value_t value);
    ta_status_t (*snapshot)(void *user, ta_value_t value, ta_value_t *out_value);
} ta_value_ops_t;

/** Declaration and initialization share owner/location/override information.
 * override_value is absent for declare and for an ordinary initializer.
 */
typedef struct ta_data_binding {
    ta_state_id_t owner;
    const ta_data_desc_t *declaration;
    ta_value_t override_value;
} ta_data_binding_t;

typedef struct ta_iteration { ta_value_t snapshot; size_t count; } ta_iteration_t;
typedef struct ta_iteration_step {
    ta_value_t snapshot;
    size_t ordinal;
    ta_text_t item;
    ta_text_t index;
} ta_iteration_step_t;

typedef struct ta_finalize_call {
    const ta_parameter_expr_t *parameters;
    size_t count;
    ta_payload_t returned_data;
} ta_finalize_call_t;

/** Transient log record; neither context nor value may be retained implicitly. */
typedef struct ta_log_record {
    ta_context_view_t context;
    ta_text_t label;
    ta_value_t value;
} ta_log_record_t;

/** The native data-model adapter supplies language semantics, not scheduling.
 * Required operations depend on features present in a definition; prepare
 * rejects definitions needing absent callbacks. Native storage requires create,
 * bind_system and bind_event. declare/initialize are needed when declarations
 * exist. Byte storage is allocated/initialized by the core; create is skipped,
 * and bind_system/bind_event may be NULL because protected values are available
 * through ta_context_inspect. Byte definitions without data declarations or
 * expressions need only run for their C actions and test for any C guards.
 *
 * Callbacks with a context use ta_context_host_user to obtain host_user;
 * callbacks with an explicit user argument receive system.config.host_user.
 * create builds a fresh native object, optionally using a borrowed seed. It
 * must not share mutable state with another actor. The returned owned root is
 * the actor's state. During create, context.state is absent, but session/system
 * metadata is available. Next bind_system, when installed, binds values, then
 * declare creates all declaration slots without initializing
 * them; initialize fills exactly the requested declaration. override_value is
 * nonzero only for a matching root invoke/spawn parameter and takes precedence
 * over the declaration initializer. On initialization error the adapter leaves
 * the declared slot empty and the core queues error.execution, then continues.
 * Failure of create/declare/bind_system prevents a valid native model and
 * terminates initialization as FAILED; no state entry actions have run yet.
 *
 * bind_system installs the protected system variables before initialization.
 * bind_event replaces _event. On TA_EXECUTION_ERROR it MUST still expose event
 * name/metadata and leave only data unbound; the original event continues and
 * the core queues error.execution. Incoming processor-format failures instead
 * use ta_io_receive_error and are rejected before event binding.
 *
 * evaluate/read_location/assign implement value/location expressions; test
 * performs language boolean conversion. to_text/to_delay convert evaluated
 * operands. from_text makes a native string for generated send/invoke IDs
 * assigned to idlocation; it is required when a definition uses idlocation.
 * to_text/evaluate_bytes allocate transient output with ta_context_arena using
 * TA_LIFETIME_MACROSTEP, or borrow immutable definition data. Outputs must remain
 * valid until the current executable action/guard finishes, not point at a C
 * callback's stack. The core copies escaping output before its lifetime ends.
 * No native stringify fallback exists. run handles scripts and custom actions,
 * distinguished by action_kind. Native code obeys context restrictions and cannot reconfigure
 * the actor directly. Native objects referenced by read-only system values
 * must not permit user replacement of those system bindings.
 *
 * iteration_open returns a rooted shallow snapshot and item count together.
 * iteration_bind declares/assigns item and optional index for one ordinal, in
 * language iteration order. Its snapshot is released even when a loop aborts.
 * pack_body maps PARAMETERS to receiver-native _event.data, including duplicate
 * parameters where supported. CONTENT already contains a tagged payload and
 * bypasses packing, including C byte payloads. Native conversion, if needed,
 * happens in bind_event. Byte state never requires a hidden native object.
 * finalize assigns returned fields only to matching location parameters.
 *
 * Exceptions are caught by the binding and reported as TA_EXECUTION_ERROR;
 * callbacks must not also enqueue the same error. TA_NO_MEMORY/TA_LIMIT invoke
 * the resource policy. Callbacks never return TA_AGAIN to suspend execution:
 * asynchronous work must be expressed as sends/invokes and later events.
 */
typedef struct ta_data_model_ops {
    ta_status_t (*create)(ta_context_t *context, ta_value_t seed, ta_value_t *out_state);
    ta_status_t (*declare)(ta_context_t *context, const ta_data_binding_t *binding);
    ta_status_t (*initialize)(ta_context_t *context, const ta_data_binding_t *binding);
    ta_status_t (*bind_system)(ta_context_t *context, const ta_system_values_t *values);
    ta_status_t (*bind_event)(ta_context_t *context, const ta_event_t *event);
    ta_status_t (*evaluate)(ta_context_t *context, ta_code_t code, ta_value_t *out_value);
    ta_status_t (*evaluate_bytes)(ta_context_t *context, ta_code_t code,
                                  ta_bytes_t *out_bytes);
    ta_status_t (*test)(ta_context_t *context, ta_code_t code, uint8_t *out_true);
    ta_status_t (*read_location)(ta_context_t *context, ta_code_t location,
                                 ta_value_t *out_value);
    ta_status_t (*assign)(ta_context_t *context, ta_code_t location, ta_value_t value);
    ta_status_t (*to_text)(ta_context_t *context, ta_value_t value, ta_text_t *out_text);
    ta_status_t (*from_text)(void *user, ta_text_t text, ta_value_t *out_value);
    ta_status_t (*to_delay)(ta_context_t *context, ta_value_t value,
                            uint64_t *out_delay_ns);
    ta_status_t (*run)(ta_context_t *context, ta_action_kind_t action_kind, ta_code_t code);
    ta_status_t (*iteration_open)(ta_context_t *context, ta_code_t collection,
                                  ta_iteration_t *out_iteration);
    ta_status_t (*iteration_bind)(ta_context_t *context, const ta_iteration_step_t *step);
    ta_status_t (*pack_body)(void *user, const ta_body_t *body, ta_value_t *out_value);
    ta_status_t (*finalize)(ta_context_t *context, const ta_finalize_call_t *call);
    void (*log)(void *user, const ta_log_record_t *record);
} ta_data_model_ops_t;

/** Validates host code tokens without executing them. Optional when validation
 * is deferred to execution. Tokens remain owned by definition.code_owner.
 */
typedef ta_status_t (*ta_validate_code_fn)(void *user,
                                         const ta_definition_desc_t *definition,
                                         ta_diagnostic_t *out_error);

/** Recurring session identity and temporary storage for processor setup. */
typedef struct ta_location_request {
    ta_actor_ref_t actor;
    ta_text_t sessionid;
    ta_arena_t *scratch;
} ta_location_request_t;

/** Optional event I/O processor registrations, written as plain nested C
 * initializers. config.ioprocessors points to an array ending in {0}; NULL
 * means no additional processors. A real entry needs a nonempty canonical type
 * URI. type and aliases are NUL-terminated UTF-8 strings, copied without libc
 * at system creation into the runtime's counted text representation. aliases
 * is a NULL-terminated array of nonempty strings; NULL means no aliases. There
 * are no registration/alias counts. A descriptor terminator has all fields
 * absent, not just a missing type. Input arrays/strings must remain valid
 * through their terminators and through ta_system_create, but not beyond it.
 *
 * The built-in SCXML processor and "scxml" alias are always installed and
 * cannot be replaced. Duplicate canonical types/aliases in this namespace,
 * including collisions with built-ins, are invalid. Requests expose canonical
 * type URIs even when a send uses an alias. Registrations describe routing;
 * the host still drains ta_io_next and dispatches requests to its own drivers.
 * No transport executes inside a registration callback.
 *
 * location is optional: NULL makes this processor send-only for every actor,
 * without allocating endpoint text or calling a no-op hook. Otherwise it is
 * called once per actor, before bind_system, with no data-model dependency.
 * Output is copied into actor storage; return {NULL,0} when this actor has no
 * inbound endpoint. Callback code and user must outlive the system; copying
 * the registration does not copy or own the object pointed to by user.
 *
 * Registering TA_BASIC_HTTP_IOPROCESSOR means the host implements appendix C.2:
 * POST, default form encoding, _scxmleventname mapping, content-body mapping and
 * a success response only after ta_io_receive has accepted the event. The core
 * provides the processor boundary, not HTTP parsing, sockets or response I/O.
 */
typedef struct ta_ioprocessor_desc {
    const char *type;
    const char *const *aliases;
    void *user;
    ta_status_t (*location)(void *user, const ta_location_request_t *request,
                            ta_text_t *out_location);
} ta_ioprocessor_desc_t;

/** External invoke service types are registered independently of event I/O
 * processors. SCXML invocation is built in. A service's event_processor chooses
 * transport for messages to its #_<invokeid> address. Both registrations must
 * exist; host request records identify the concrete invocation as well.
 * config.services likewise ends in {0}, and aliases ends in NULL. Strings are
 * NUL-terminated, nonempty and copied at system creation. event_processor must
 * name a registered processor's canonical URI or alias; resolution occurs
 * after all processors are registered, independent of array order. Service
 * types/aliases occupy a separate namespace; duplicates within that namespace
 * or attempts to replace the built-in SCXML service/alias are invalid.
 */
typedef struct ta_service_desc {
    const char *type;
    const char *const *aliases;
    const char *event_processor;
} ta_service_desc_t;

typedef enum ta_span_operation {
    TA_SPAN_SEND = 0,
    TA_SPAN_PROCESS,
    TA_SPAN_INVOKE,
    TA_SPAN_UPGRADE
} ta_span_operation_t;

typedef enum ta_span_kind {
    TA_SPAN_INTERNAL = 0,
    TA_SPAN_PRODUCER,
    TA_SPAN_CONSUMER,
    TA_SPAN_CLIENT
} ta_span_kind_t;

typedef struct ta_span_start {
    ta_arena_t *arena;             /**< Storage valid through the corresponding end. */
    ta_span_operation_t operation;
    ta_span_kind_t kind;
    ta_actor_ref_t actor;
    uint64_t revision;
    uint64_t message_id;
    ta_text_t event_name;
    ta_text_t destination;         /**< Logical name/address, not payload data. */
    ta_text_t processor;
    uint64_t now_ns;
    const ta_trace_context_t *parent;
    const ta_trace_context_t *links;
    size_t link_count;
} ta_span_start_t;

/** Host span token may be zero for a non-recording span while context remains
 * valid for propagation. start allocates output text in its supplied span arena
 * or uses storage valid through end. The runtime copies propagation context
 * into any longer-lived message. IDs, sampling, wall-clock timestamps, attribute
 * mapping and export are entirely the host's responsibility.
 */
typedef struct ta_span { uintptr_t handle; ta_trace_context_t context; } ta_span_t;

/** Completion facts shared by all tracing operations. */
typedef struct ta_span_end {
    const ta_span_t *span;
    uint64_t now_ns;
    ta_status_t status;
} ta_span_end_t;

/** Optional OTel adapter. Single-message sends create PRODUCER spans whose
 * context is propagated. With an existing creation context (e.g. forwarding),
 * send uses CLIENT and links to that context without overwriting it. Processing
 * creates CONSUMER spans at dequeue, links to message creation context, and ends
 * only at stability/termination/fault, including all internal follow-up steps
 * and scheduler yields. Links are supplied at start for head sampling.
 *
 * The host may choose a process parent using the link; no ambient thread-local
 * context is assumed. Outgoing sends inherit the active process span as parent.
 * A send ends at queue admission or host completion; an invoke span covers start
 * dispatch, not the whole service lifetime. Scheduled sends retain context
 * across delay and cancellation. Each successful start has exactly one end,
 * including system destruction. Unmatched events still have a process span.
 *
 * status is the operation result, not an OTel status enum. error callbacks add
 * observations for SCXML execution errors even if the actor handles the event.
 * Tracing failure drops telemetry and increments a counter; it never changes
 * transition selection or generates SCXML error events. NULL start/end disables
 * recording while explicit incoming contexts still propagate. No actor payload
 * or native state is exported automatically. A host should bound cardinality.
 * Conventions: https://opentelemetry.io/docs/specs/semconv/messaging/messaging-spans/
 */
typedef struct ta_trace_ops {
    void *user;
    ta_status_t (*start)(void *user, const ta_span_start_t *start, ta_span_t *out_span);
    void (*error)(void *user, uintptr_t span, const ta_diagnostic_t *error);
    void (*end)(void *user, const ta_span_end_t *end);
} ta_trace_ops_t;

typedef enum ta_actor_status {
    TA_ACTOR_STARTING = 0,
    TA_ACTOR_READY,
    TA_ACTOR_SUSPENDED,            /**< Mid-macrostep scheduler yield, not stable. */
    TA_ACTOR_STOPPING,
    TA_ACTOR_DONE,
    TA_ACTOR_STOPPED,
    TA_ACTOR_FAILED
} ta_actor_status_t;

typedef struct ta_actor_termination {
    ta_actor_ref_t actor;
    ta_actor_status_t status;
    ta_status_t reason;
} ta_actor_termination_t;

/** Called once when an actor becomes terminal, including fail-stop. State is
 * still rooted during the callback. Do not release/reenter here; record the ref
 * and call ta_actor_release later. Invoked local children are released by the
 * runtime after notification, so their refs are observational only at this point.
 */
typedef void (*ta_actor_terminal_fn)(void *user, const ta_actor_termination_t *termination);

/** All host callback userdata and function code remain valid until system
 * destruction. Registration arrays, alias arrays and their strings are copied
 * during ta_system_create, so block-scope compound literals are safe if create
 * is called before their enclosing block ends. A helper returning a config
 * must instead use static or caller-owned registration storage; it must not
 * return pointers into its own automatic arrays. Use ta_system_config_init before
 * overrides; zero-initialization alone intentionally does not mean "unlimited".
 */
typedef struct ta_system_config {
    uint32_t abi_version;
    size_t struct_size;
    ta_text_t session_prefix;      /**< Host-unique prefix when bridging systems. */
    ta_text_t data_model;
    void *host_user;
    ta_value_ops_t values;
    ta_data_model_ops_t data;
    ta_validate_code_fn validate_code;
    void (*diagnostic)(void *user, const ta_diagnostic_t *diagnostic);
    ta_actor_terminal_fn terminal;
    ta_trace_ops_t trace;
    const ta_ioprocessor_desc_t *ioprocessors; /**< Optional array ending in {0}. */
    const ta_service_desc_t *services;        /**< Optional array ending in {0}. */
    ta_arena_options_t arenas[TA_LIFETIME_COUNT];
    ta_system_limits_t limits;
    ta_actor_limits_t actor_defaults;
} ta_system_config_t;

/** Default profile: no count/aggregate byte cap, 256-byte preferred first
 * chunks, 64 KiB maximum chunks/allocations, state/action depth 64, event name
 * 1024 bytes, trace metadata 4096 bytes, and 100000 microsteps/1000000 actions
 * per macrostep. No tracing or optional processors/services. These are tuning
 * defaults, not a memory commitment or an ESP32/server size guarantee.
 */
TA_API void ta_system_config_init(ta_system_config_t *config);

/** Creates a child arena under parent, never takes ownership of parent. The
 * parent must outlive the system. Empty registries/queues have no preallocated
 * maximum-size buffers. Session IDs combine prefix and a monotonic instance ID.
 * The prefix must use [A-Za-z0-9_.-] and contain at least one character; a host
 * chooses globally distinct prefixes when it exposes SCXML sessions remotely.
 * Registration normalization/copying is charged to system memory limits as
 * entries are scanned; it does not first count an arbitrarily large list or
 * allocate a maximum-capacity table. A limit does not make missing terminators
 * or invalid C pointers safe. Registration errors fail creation without exposing
 * a partial system or invoking per-actor location callbacks.
 */
TA_API ta_status_t ta_system_create(ta_arena_t *parent,
                                  const ta_system_config_t *config,
                                  ta_system_t **out_system);

/** Updates count/byte caps atomically, rejecting caps below current use.
 * Does not change actor defaults or structural limits of prepared definitions.
 * Definition size/depth limits apply to subsequently prepared definitions.
 */
TA_API ta_status_t ta_system_set_limits(ta_system_t *system,
                                      const ta_system_limits_t *limits);

/** No threads/stacks/VMs per actor. Runnable actors are scheduled from a ready
 * queue; timers from a deadline structure, without scanning all idle actors on
 * every run. Definition indexes are shared; actor records contain only mutable
 * configuration/history/queue/lifetime state. No O(states squared) table is
 * allocated per actor. Capacity grows in bounded chunks and released instance
 * storage is reusable without lifetime-long growth of the system arena.
 *
 * Every game entity may be an actor, including entities updated each frame.
 * A simple C actor requires one control record and its byte state. Empty queues
 * have no backing arrays; unused history/invocation/name/trace metadata has no
 * separately allocated storage. Sequential configurations use a compact active
 * path; parallel membership/history buffers exist only where needed. Immutable
 * state names, code and topology remain shared. System-variable views can be
 * synthesized on access; byte actors do not allocate native _event/_ioprocessors
 * wrappers. Retaining the last message preserves SCXML _event semantics but
 * does not require a full event structure embedded in every control record.
 * Use footprint/statistics and actual frame workloads to verify memory/latency.
 */
typedef struct ta_spawn_options {
    ta_definition_t *definition;   /**< Exactly one of definition/definition_key. */
    ta_text_t definition_key;      /**< Current published revision at spawn. */
    ta_value_t seed;               /**< Borrowed; create produces independent state. */
    ta_bytes_t initial_bytes;      /**< BYTES only; exact-size override of the default image. */
    const ta_parameter_t *parameters;
    size_t parameter_count;        /**< Matching root declarations only. */
    ta_text_t name;                /**< Optional name; that registry key must be unbound. */
    const ta_actor_limits_t *limits; /**< NULL copies system.actor_defaults. */
    const ta_trace_context_t *trace;
} ta_spawn_options_t;

/** Reserves identity/resources/name, snapshots inputs, and queues initialization.
 * It executes no user statechart actions. Failures roll back reservation and
 * name binding. Success transfers one actor lifecycle obligation to the host:
 * stop it if necessary and release it when terminal. Initialization failure is
 * subsequently reported by the terminal callback/status, not retroactively.
 * Name routes may enqueue while STARTING; initialization precedes those events.
 * BYTES allocates its block during admission and copies initial_bytes before
 * returning. Its seed must be absent. NATIVE requires absent initial_bytes and
 * creates its native root during initialization. Byte images are independent
 * for every actor; no per-instance value retain/snapshot callback is involved.
 */
TA_API ta_status_t ta_actor_spawn(ta_system_t *system,
                                const ta_spawn_options_t *options,
                                ta_actor_ref_t *out_actor);

typedef struct ta_actor_info {
    ta_actor_status_t status;
    ta_status_t reason;
    uint64_t revision;
    ta_text_t sessionid;
    ta_text_t registered_name;
    ta_data_view_t state;          /**< Borrowed native root or owned byte-block view. */
    uint8_t stable;
    size_t external_events;
    size_t internal_events;
    size_t delayed_sends;
    size_t invocations;
    ta_memory_stats_t memory;
} ta_actor_info_t;

/** Host inspection is read-only. state.as.native may be retained with the host
 * retain callback but may not be mutated outside execution/migration callbacks.
 * All returned views expire at the next API call on this system, since formatted
 * identities may use a reusable buffer. Byte state is read-only here;
 * copy selected fields into host-owned storage to keep a frame snapshot. Native
 * retained objects are not immutable snapshots: use host copying for that.
 * Active configurations and histories are streamed by ta_system_inspect below.
 */
TA_API ta_status_t ta_actor_inspect(ta_system_t *system, ta_actor_ref_t actor,
                                  ta_actor_info_t *out_info);
/** Limit changes are atomic and reject values below current use. Hard
 * macrostep ceilings cannot be reduced below counts already spent in a
 * suspended macrostep. Actor limits do not override system/ancestor caps.
 */
TA_API ta_status_t ta_actor_set_limits(ta_system_t *system, ta_actor_ref_t actor,
                                     const ta_actor_limits_t *limits);

typedef enum ta_inspect_kind {
    TA_INSPECT_ACTOR = 0,
    TA_INSPECT_ACTIVE_STATE,
    TA_INSPECT_HISTORY_STATE,
    TA_INSPECT_CURRENT_EVENT
} ta_inspect_kind_t;

/** Optional detail flags. The actor record is always emitted. Data means the
 * borrowed native root/C byte block, not an allocated snapshot or field map.
 */
typedef enum ta_inspect_detail {
    TA_INSPECT_DATA = 1,
    TA_INSPECT_ACTIVE = 2,
    TA_INSPECT_HISTORY = 4,
    TA_INSPECT_EVENT = 8
} ta_inspect_detail_t;

/** One streamed record. Actor metadata comes first, then active states in
 * document order, history entries in document/target order, then last _event.
 * history.recorded distinguishes unset history from a stored configuration;
 * unset history emits one record with target == TA_INDEX_NONE. There is no
 * current-event record until the actor has dequeued its first event.
 */
typedef struct ta_inspect_record {
    ta_inspect_kind_t kind;
    ta_actor_ref_t actor;
    const ta_definition_t *definition;
    union {
        ta_actor_info_t info;
        struct { ta_state_id_t id; const ta_state_desc_t *description; } active;
        struct {
            ta_state_id_t id;
            uint8_t recorded;
            ta_state_id_t target;
            size_t ordinal;
        } history;
        ta_event_t event;
    } as;
} ta_inspect_record_t;

typedef enum ta_visit {
    TA_VISIT_CONTINUE = 0,
    TA_VISIT_PAUSE,
    TA_VISIT_STOP
} ta_visit_t;

/** Callback sink with borrowed records. No record, text, definition, native
 * root or byte pointer is retained on the host's behalf. Copy the data needed
 * by a renderer/debugger during the callback. The callback must not modify or
 * reenter the inspected system, including reference operations and native state
 * mutation. It may enqueue a copied byte message into a DIFFERENT system if it
 * owns that system's thread and no callback reenters the inspected system.
 */
typedef struct ta_inspect_sink {
    void *user;
    ta_visit_t (*visit)(void *user, const ta_inspect_record_t *record);
} ta_inspect_sink_t;

typedef struct ta_inspect_options {
    ta_actor_ref_t actor;          /**< Zero scans all actors, including local children. */
    uint64_t revision;             /**< Zero accepts every revision. */
    uint8_t stable_only;           /**< Omit STARTING/in-progress actors, not wait for them. */
    uint32_t details;             /**< Bitwise OR of ta_inspect_detail_t values. */
    size_t work_units;             /**< Bounds examined actors + emitted detail records. */
    ta_inspect_sink_t sink;
} ta_inspect_options_t;

/** Caller-owned continuation. Initialize to all zero to begin; otherwise do
 * not inspect/modify opaque fields. Owns no arena or actor references and can
 * be abandoned without cleanup. Reuse only with the same system, filters and
 * details (budget/sink can change). A zero cursor after completion starts anew.
 * Cursor generation counters never wrap: inspection returns TA_LIMIT if its
 * generation space is exhausted rather than validating an obsolete cursor.
 */
typedef struct ta_inspect_cursor { uint64_t opaque[6]; } ta_inspect_cursor_t;

/** Streams inspection with O(1) auxiliary space in the number of actors, no
 * materialized actor list, no payload snapshots and no callbacks into the data
 * model. Actor traversal order is unspecified; each selected actor occurs once
 * in an uninterrupted scan. To inspect one configuration, set options.actor
 * and TA_INSPECT_ACTIVE. Read-only views are valid only during the visit call.
 * No native object is created to expose a C struct's fields.
 *
 * The implementation traverses live storage, not the lifetime history of IDs;
 * scans do not grow slower with retired-actor churn. Each actor examined costs
 * a work unit even when filtered out; each detail record costs another. This
 * also bounds sparse-filter scans. An actor record counts with its examination.
 * Nonzero work_units is required. No lock or extra storage persists on pause.
 *
 * TA_OK means exhausted and resets cursor. TA_AGAIN means budget exhausted or
 * PAUSE requested; the last delivered record is consumed, and the cursor resumes
 * after it. STOP resets cursor and returns TA_CANCELLED. Any mutation of this
 * system between calls invalidates a started cursor; return TA_STALE before
 * visiting another record, then reset explicitly to restart. Invalid actor
 * filters return STALE, invalid options return INVALID, without visiting.
 *
 * A full scan, or paused scan with no intervening mutation, sees one coherent
 * system state. stable_only is a filter, not a frame barrier: the host must
 * finish the intended simulation work before exporting a complete frame. Stream
 * into a host frame buffer or stage byte messages in the renderer system and
 * advance that system after the scan. The core neither freezes another system
 * nor implies an atomic multi-system frame commit.
 */
TA_API ta_status_t ta_system_inspect(ta_system_t *system,
                                   const ta_inspect_options_t *options,
                                   ta_inspect_cursor_t *cursor);

/** Stop is idempotent for a live ref; it rejects further ingress immediately.
 * It takes priority over FIFO events and executes termination exits at the next
 * microstep boundary (immediately on the next run if idle). A suspended actor
 * does not finish an unbounded macrostep first. Children are canceled, pending
 * delayed sends discarded, and no done.invoke is sent. Already-dispatched
 * messages are not recalled. SCXML action errors in exits follow block rules.
 */
TA_API ta_status_t ta_actor_stop(ta_system_t *system, ta_actor_ref_t actor);

/** Only terminal top-level actors can be released. Removes remaining queues,
 * last _event, history, state roots, and definition reference. Implicit name
 * removal uses identity comparison, so releasing an old actor cannot unbind a
 * replacement. Taken host I/O leases are independent; their completion remains
 * required and cannot target a reused actor identity. Stale refs return STALE.
 */
TA_API ta_status_t ta_actor_release(ta_system_t *system, ta_actor_ref_t actor);

/** Name plus optimistic incarnation check, reused by bind and unbind. */
typedef struct ta_name_binding {
    ta_text_t name;
    ta_actor_ref_t expected;
} ta_name_binding_t;

/** One registry name per actor, scoped to this system. Binding an already
 * differently named actor fails. expected.id == 0 requires an unbound name;
 * otherwise it must match the current binding. A terminal/stopping actor cannot
 * be bound. Replacement removes the previous actor's binding but does not stop
 * it. Lookup is a borrowed incarnation ref, not a reference-count operation.
 * Routing, comparison and enqueue are atomic with respect to serialized calls.
 */
TA_API ta_status_t ta_name_bind(ta_system_t *system, const ta_name_binding_t *binding,
                              ta_actor_ref_t replacement);
TA_API ta_status_t ta_name_unbind(ta_system_t *system, const ta_name_binding_t *binding);
TA_API ta_status_t ta_name_lookup(ta_system_t *system, ta_text_t name,
                                ta_actor_ref_t *out_actor);

/** Resolve the session-id part of a #_scxml_<sessionid> address, including local
 * invoked actors. Hosts use this when decoding remote SCXML ingress. Returns
 * only a live, accepting incarnation; never falls back to name lookup. The
 * borrowed ref remains subject to normal stale/terminated checks at admission.
 */
TA_API ta_status_t ta_session_lookup(ta_system_t *system, ta_text_t sessionid,
                                   ta_actor_ref_t *out_actor);

/** Decodes raw SCXML targets without allocation: absent -> SELF, #_internal,
 * #_parent, #_scxml_<sessionid>, and #_<invokeid>. Session strings remain URI
 * targets until routing resolves them; other URIs go to the selected processor.
 * Text slices borrow raw. Resolution never conflates a name with a session ID.
 * The SCXML router applies this decoding to URI targets, including the result
 * of target_expression, before dispatch. SELF/INTERNAL/INSTANCE/NAME/PARENT
 * are SCXML-only targets. INVOKE can also route to a registered external
 * service's event processor; other processors otherwise accept only URI.
 */
TA_API ta_status_t ta_target_parse(ta_text_t raw, ta_target_t *out_target);

/** Host-originated external message. No caller-supplied type/invokeid/sendid;
 * processors use ta_io_receive for protocol metadata, invoked services use the
 * invocation APIs. data and trace are snapshotted before success is returned.
 * Target must be INSTANCE or NAME. TA_OK means queue admission, not processing
 * or successful application handling. Failure does not enqueue a partial event
 * or mutate a destination. There is no implicit retry, eviction or silent drop.
 */
typedef struct ta_message {
    ta_text_t name;
    ta_payload_t data;
    const ta_trace_context_t *trace;
} ta_message_t;

/** Delivery input reusable by host queues and cross-system bridges. */
typedef struct ta_post_options { ta_target_t target; ta_message_t message; } ta_post_options_t;

TA_API ta_status_t ta_post(ta_system_t *system, const ta_post_options_t *delivery,
                         uint64_t *out_message_id);

/** Callback-only raised internal event, the native equivalent of raise. SCXML
 * raise carries just a literal name; data is a native API extension. No origin
 * is set. On failure the calling action returns the status; it must not enqueue
 * a second error. All context effects (raise/send/cancel) are forbidden in
 * guards, data/system binding callbacks, migration, done-data evaluation and
 * finalize. Root scripts and ordinary script/custom action callbacks may use
 * them. Bindings must propagate a failed context-effect status to their caller;
 * the core then emits the corresponding error once at the block boundary.
 */
TA_API ta_status_t ta_context_raise(ta_context_t *context, ta_text_t event,
                                  const ta_payload_t *data);

/** Evaluated native equivalent of send. All slices/body values are borrowed.
 * A nonzero delay uses the system's last supplied monotonic now. Overflow is
 * rejected. Generated idlocation assignment belongs to TA_ACTION_SEND; this
 * entry point accepts an already chosen sendid or absence. Unsupported type or
 * malformed target is error.execution; valid but unreachable/full destinations
 * produce error.communication, including the sendid. Successful asynchronous
 * acceptance may be followed by a later communication error.
 */
typedef struct ta_send {
    ta_text_t event;
    ta_text_t type;
    ta_target_t target;
    ta_text_t sendid;
    ta_body_t body;
    uint64_t delay_ns;
} ta_send_t;

TA_API ta_status_t ta_context_send(ta_context_t *context, const ta_send_t *send,
                                 uint64_t *out_message_id);

/** Cancels ALL not-yet-dispatched delayed sends with this sendid from THIS
 * session only. No match is success. A send survives its originating state's
 * exit, but not actor termination. Already dispatched delivery is not undone.
 * Dispatch means admission to a local FIFO or creation of a SEND request in the
 * host outbox. A matured send in that outbox is dispatched even before next
 * leases it; it survives sender termination and still requires completion.
 */
TA_API ta_status_t ta_context_cancel(ta_context_t *context, ta_text_t sendid);

/** Execution resource policy: admission fails with TA_LIMIT/TA_NO_MEMORY before
 * taking ownership. During an action, the core aborts the current block and
 * enqueues error.platform.limit or error.platform.memory using a reserved,
 * allocation-free per-actor slot (with a static name, appended in FIFO order).
 * If that slot is already occupied, or interpreter configuration bookkeeping
 * cannot finish a microstep, the actor fail-stops as FAILED and reports a
 * diagnostic; it does not attempt further user exit actions or synthesize done.
 * C/native cleanup and service cancellation still run. The microstep/action
 * hard ceilings also fail-stop, bounding livelock across scheduler yields.
 * These resource ceilings are explicit platform extensions to SCXML. Budgets
 * for scheduling below only yield and never inject an error into the machine.
 */
typedef struct ta_run_budget {
    size_t work_units;             /**< Total across the system; zero does no work. */
    size_t actor_quantum;          /**< Nonzero work units before round-robin yield. */
} ta_run_budget_t;

/** Recurring host scheduling input: monotonic time and a cooperative budget. */
typedef struct ta_run_options { uint64_t now_ns; ta_run_budget_t budget; } ta_run_options_t;

typedef struct ta_run_result {
    size_t work_units;
    size_t microsteps;
    size_t external_events;
    size_t terminal_actors;
    uint8_t runnable;              /**< Call run again without waiting. */
    uint8_t io_ready;              /**< Untaken host requests exist. */
    uint64_t next_deadline_ns;     /**< TA_TIME_NEVER if no pending timer. */
} ta_run_result_t;

/** Drives ready actors and timers without sleeping or reading a clock. now_ns
 * must be nondecreasing and less than TA_TIME_NEVER. The first call establishes
 * time; messages accepted before it have no scheduled delay. Due timers are
 * dispatched in (deadline, insertion sequence) order. Timer deadlines use a
 * checked 64-bit monotonic nanosecond domain; MCU hosts extend wrapping counters.
 *
 * One unit is a microstep, an unmatched event dequeue/selection pass, an invoke
 * start, actor initialization/termination phase, or timer dispatch. A microstep
 * and synchronous callback are not preempted. The core never consumes a second
 * external event for an actor paused mid-macrostep. _event, internal FIFO,
 * pending invokes, scratch, trace span and hard-limit counters survive yields.
 * Ready actors and due timers receive fair turns; idle actors consume no units.
 * A budget is not a wall-clock deadline; host code must bound individual calls.
 * Returns TA_OK even if individual actors fault; use diagnostics/status for
 * those. Invalid time/budget is rejected before work and does not advance time.
 */
TA_API ta_status_t ta_system_run(ta_system_t *system, const ta_run_options_t *options,
                               ta_run_result_t *out_result);
TA_API uint64_t ta_system_next_deadline(const ta_system_t *system);

typedef enum ta_io_request_kind {
    TA_IO_SEND = 0,
    TA_IO_INVOKE_START,
    TA_IO_INVOKE_CANCEL
} ta_io_request_kind_t;

/** A request is an owned core snapshot leased to the host by ta_io_next. Every
 * field remains valid through ta_io_complete, even if the actor exits/upgrades
 * or its definition is unpublished. The host must copy anything needed longer.
 * Routing keys, origin actor/revision and invocation identity are captured, not
 * recomputed from the current name binding. request is distinct from sendid.
 * SEND body preserves ordered parameter duplicates, and includes the source
 * SCXML event metadata. Host converts it to the transport's representation.
 * START carries evaluated type/source/body. CANCEL identifies the service only.
 */
typedef struct ta_io_request {
    ta_request_ref_t request;
    ta_io_request_kind_t kind;
    ta_actor_ref_t actor;
    uint64_t revision;
    ta_invocation_ref_t invocation;
    ta_text_t invokeid;
    ta_text_t type;                /**< Processor URI for SEND, service URI otherwise. */
    ta_text_t target;              /**< Destination for SEND, source for START. */
    ta_event_t event;              /**< SEND envelope; data is absent, use body. */
    ta_body_t body;
    const ta_trace_context_t *trace;
    uint64_t accepted_ns;
    uint64_t due_ns;
} ta_io_request_t;

/** Untaken requests are FIFO. next marks one taken and returns its stable
 * pointer; EMPTY returns NULL. The host performs/schedules I/O outside core
 * callbacks, then completes the lease. A taken request counts against caps
 * until completed. Host-owned transport buffers count against host budgets.
 */
TA_API ta_status_t ta_io_next(ta_system_t *system, const ta_io_request_t **out_request);

/** Completes exactly once. SEND success means dispatched, not processed by the
 * peer. START success means the external service exists and may emit events.
 * Failure queues error.communication to the still-live originating actor.
 * A cancellation race suppresses late errors/events and still releases the
 * request. Duplicate/unknown IDs return TA_STALE; no actor identity is reused.
 * Accepted statuses are OK, COMMUNICATION_ERROR and CANCELLED.
 */
TA_API ta_status_t ta_io_complete(ta_system_t *system, ta_request_ref_t request,
                                ta_status_t result);

/** Processor and resolved local destination used for ingress and rejections. */
typedef struct ta_io_route { ta_text_t processor; ta_actor_ref_t destination; } ta_io_route_t;

/** Ingress decoded by an I/O processor. The core verifies a registered type,
 * snapshots fields/data and enqueues an EXTERNAL event. It ignores untrusted
 * invokeid/message_id and assigns its own identity; use invocation APIs for
 * authenticated child returns. SCXML origintype must be "scxml", origin must
 * be the sending session's advertised SCXML location, and sendid is preserved.
 * Internal/platform origins are cleared by core-generated event construction.
 * Cross-VM decoding and isolation are the host's responsibility before ingress.
 */
TA_API ta_status_t ta_io_receive(ta_system_t *system, const ta_io_route_t *route,
                               const ta_event_t *event);

/** Reports a rejected processor message (e.g. impossible payload conversion).
 * Queues error.communication on the destination's internal FIFO with sendid;
 * the host is responsible for any negative transport acknowledgment to sender.
 */
TA_API ta_status_t ta_io_receive_error(ta_system_t *system,
                                     const ta_io_route_t *route, ta_text_t sendid);

/** External service returns after START completion. emit sets invokeid from the
 * live invocation token, origin/type from its service route, and snapshots data.
 * done atomically accepts the final result and queues done.invoke.<invokeid>;
 * failure leaves it live for retry. No emit is accepted after done succeeds.
 * The done event remains eligible for finalize until consumed; exiting the owner
 * first invalidates it. The invocation record remains live until that event is
 * consumed or discarded on cancellation, even if a local child is released.
 * A stale/canceled token rejects all later returns even if the same textual
 * invokeid is used on re-entry. Local child actors use this
 * routing internally and cannot fabricate a sibling's invocation provenance.
 */
TA_API ta_status_t ta_invocation_emit(ta_system_t *system,
                                    ta_invocation_ref_t invocation,
                                    const ta_message_t *message);
TA_API ta_status_t ta_invocation_done(ta_system_t *system,
                                    ta_invocation_ref_t invocation,
                                    const ta_body_t *result);

/** Reports failure AFTER successful service start. The host must have stopped
 * that service before calling. Retires its identity and queues internal
 * error.communication to the parent without done.invoke or finalize. Late
 * returns are rejected. This also happens internally when a local invoked
 * actor fail-stops. Error-event allocation follows the reserved-slot/fail-stop
 * policy, so retirement cannot be undone by a full parent's internal queue.
 */
TA_API ta_status_t ta_invocation_fail(ta_system_t *system,
                                    ta_invocation_ref_t invocation);

/** Cancellation is control-plane work and has a reserved record allocated at
 * invocation creation, so a full outbox cannot suppress it. This reservation
 * counts against io_requests and io_request_bytes even before cancellation;
 * starting an external service needs capacity for START plus that reservation.
 * If START was never taken it is removed without exposing a service. Otherwise
 * CANCEL is exposed
 * after the taken START is completed, even if completion races cancellation.
 * The host stops the service and acknowledges CANCEL; all late returns are
 * rejected immediately from the moment cancellation was requested. Ordinary
 * already-dispatched SEND requests remain leases and must still be completed.
 */
TA_API ta_status_t ta_invocation_is_cancelled(ta_system_t *system,
                                            ta_invocation_ref_t invocation,
                                            uint8_t *out_cancelled);

/** One mapping for each replacement state, in replacement document order.
 * from is TA_INDEX_NONE for a new state. data_initialized is the candidate's
 * post-commit initialization flag: true for root, all states with early binding,
 * or a mapped state's previously initialized data; otherwise false. This lets
 * native migration code distinguish inactive-but-initialized data from slots
 * awaiting their first entry. History entries always have this flag unset.
 */
typedef struct ta_migration_state {
    ta_state_id_t from;
    ta_state_id_t to;
    uint8_t data_initialized;
} ta_migration_state_t;

/** Live upgrade is a platform extension, separate from definition publication.
 * It is allowed only at a stable boundary with no pending internal events,
 * active/pending invocations, delayed sends, outstanding originating I/O, or
 * stop request. External FIFO entries and last _event may remain. BUSY changes
 * nothing; it never forces an actor through an infinite macrostep to upgrade.
 *
 * The core maps configuration, history and first-entry flags by stable state
 * names, checking kinds, ancestry and legality in the replacement. It rejects
 * incompatible active/history topology; it never silently falls back to initial
 * states. New states receive initialization flags according to state_map below.
 * The migration hook receives both descriptions and this complete mapping so
 * it can reconcile new/removed declarations, including inactive visited states.
 *
 * For a NATIVE replacement, migrate reads old_state without modifying it and
 * returns an OWNED independent native object. For BYTES, the core allocates a
 * candidate new_bytes block initialized from the replacement's image (or zero);
 * migrate fills it and
 * returns an absent out_state. Old and new storage modes may differ when the
 * callback explicitly performs the conversion. A failed candidate is discarded.
 * Core-owned candidates and staging storage count against actor and system byte
 * limits; insufficient upgrade headroom leaves the old actor unchanged.
 * No I/O, sends, external side effects or mutation of the old data are allowed.
 * For each state_map entry with data_initialized set, the
 * hook initializes any new data; other declarations are created but left for
 * late initialization. scratch is staging-only: the new object must not retain
 * pointers into it or any old activation arena being retired at commit.
 *
 * migrate may be NULL only for BYTES -> BYTES with identical size/alignment,
 * matching nonempty schema IDs and no data declarations in either definition;
 * then the core copies the bytes unchanged. Other changes require a callback,
 * including adding/removing fields. Commit can copy the candidate into an
 * existing inline block or replace a separately owned block. Retired separate
 * blocks are reclaimed, not accumulated on each upgrade. Original inline space
 * remains charged with the control chunk until actor release. No host pointer
 * to new_bytes remains valid after the migration callback returns.
 *
 * All allocations, structural checks, migration, and protected-variable binding
 * to the candidate state happen before commit. Installed bind_system/bind_event run
 * against the candidate context in TA_PHASE_MIGRATE; In() sees the candidate
 * configuration. All borrowed values except candidate new_bytes stay untouched.
 * Any binding failure aborts the upgrade. Failure releases staging and leaves
 * old state/revision/FIFO/history intact. Commit replaces the data-model state and
 * definition atomically, retaining actor identity, name, _sessionid, original
 * _name and _ioprocessors. No old exits/new entries/root script run. Eventless
 * selection is scheduled before the next external dequeue. Old activation
 * arenas close on commit and replacement activation arenas begin empty.
 * Definition schema indexes never serve as persistent IDs across revisions.
 */
typedef struct ta_migration {
    ta_actor_ref_t actor;
    const ta_definition_t *from;
    const ta_definition_t *to;
    ta_data_view_t old_state;
    ta_mut_bytes_t new_bytes;      /**< Candidate BYTES block; empty for NATIVE. */
    ta_arena_t *scratch;           /**< Staging-only lifetime. */
    const ta_state_id_t *old_configuration;
    const ta_state_id_t *new_configuration;
    size_t configuration_count;
    const ta_migration_state_t *state_map;
    size_t state_map_count;
    const ta_system_values_t *system;
} ta_migration_t;

typedef struct ta_upgrade_options {
    ta_actor_ref_t actor;
    ta_definition_t *replacement;
    uint64_t expected_revision;    /**< Required current revision, or ANY. */
    void *user;
    ta_status_t (*migrate)(void *user, const ta_migration_t *migration,
                           ta_value_t *out_state);
} ta_upgrade_options_t;

TA_API ta_status_t ta_actor_upgrade(ta_system_t *system, const ta_upgrade_options_t *options);

typedef struct ta_system_stats {
    ta_memory_stats_t memory;
    size_t actors;
    size_t runnable_actors;
    size_t definitions;
    size_t names;
    size_t queued_events;
    size_t delayed_sends;
    size_t invocations;
    size_t io_requests;
    uint64_t accepted_messages;
    uint64_t rejected_messages;
    uint64_t completed_macrosteps;
    uint64_t microsteps;
    uint64_t faulted_actors;
    uint64_t dropped_trace_records;
} ta_system_stats_t;

/** Snapshots counters without allocation. uint64_t cumulative counters saturate
 * at UINT64_MAX; identities and deadlines instead reject exhaustion/overflow.
 */
TA_API void ta_system_stats(const ta_system_t *system, ta_system_stats_t *out_stats);

/** Graceful shutdown rejects new spawns/posts/publications and requests stop for
 * every actor. Continue run, drain host requests, acknowledge cancellations and
 * release top-level actors/definition references. Completions and reads remain
 * allowed. No implicit wait, transport shutdown or wall-clock timeout occurs.
 */
TA_API ta_status_t ta_system_shutdown(ta_system_t *system);

/** Destroys a quiescent system and releases registry-held definitions. BUSY if
 * any actor, taken/untaken I/O request, invocation or caller-held definition ref
 * remains. It never invalidates a host lease. After success the system pointer
 * is invalid; parent arena remains caller-owned. A host that abandons I/O must
 * first stop the actual service and explicitly complete each taken request with
 * CANCELLED. Closing the parent early is invalid, not an alternative shutdown.
 */
TA_API ta_status_t ta_system_destroy(ta_system_t *system);

/** Typical host integration (all failure paths must be handled by the host):
 *
 * 1. Supply a chunk pool/allocator; create a root arena. Initialize system
 *    config, install C actions or a native binding, set limits, create the system.
 * 2. Prepare immutable definition tables; publish a revision under a key.
 *    Spawn actors from that key; release unnecessary caller definition refs.
 * 3. Admit external messages with ta_post/ta_io_receive. Call ta_system_run
 *    with monotonic time and a bounded budget. If result.runnable is true,
 *    schedule another turn; otherwise wait using the host's own event loop and
 *    result.next_deadline_ns. Wake on ingress, I/O completion or that deadline.
 *    At a simulation frame boundary, stream entity data through ta_system_inspect
 *    into a host buffer or renderer system. Resume scans without mutating the
 *    simulation; then drive the renderer with its own budget and clock.
 * 4. Drain ta_io_next, perform asynchronous host I/O, and serialize completions
 *    back onto this system's owner thread. Never retain a borrowed context.
 * 5. Publish new definitions while running. Explicitly upgrade eligible actors,
 *    or spawn replacements and compare-and-swap their name bindings. Messages
 *    already admitted to an old instance remain there until processed/stopped.
 * 6. Shutdown, drain/cancel host I/O, release terminal actors and caller-owned
 *    definitions, destroy the system, then close the root arena.
 *
 * On ESP32 the allocator can be a bounded pool and telemetry/optional I/O can
 * be omitted. For a game, select TA_DATA_BYTES and give the definition an entity
 * struct layout/image; use one actor per entity, direct C run callbacks and
 * compact byte messages. The same API can own separate simulation/renderer
 * systems. In Node.js, TA_DATA_NATIVE names a persistent native JS handle;
 * callbacks enter the correct isolate, and the host loop drives these same
 * APIs. Neither embedding requires changing the SCXML execution semantics.
 */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TINYACTORS_H_INCLUDED */
