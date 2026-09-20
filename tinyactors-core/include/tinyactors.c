/*
 * GENERATED FILE. Do not edit; run `mise run amalgamate` in tinyactors-core.
 * The public API is include/tinyactors.h. Private module headers and sources
 * follow in the order listed by src/internal.h.
 */
#include "tinyactors.h"

/*
 * Functions not declared in tinyactors.h are private to this file. Private
 * helpers may be unused in a given build, so they do not warn.
 */
#ifndef TA_PRIVATE
#if defined(__GNUC__) || defined(__clang__)
#define TA_PRIVATE static __attribute__((unused))
#else
#define TA_PRIVATE static
#endif
#endif


/* Private base header: every implementation module reaches the public API here. */

#define TA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define TA_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

typedef struct {
    uint32_t offset;
    uint32_t length;
} ta_span32;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} ta_actor_key;

/* Freestanding memory helpers over an injected ta_allocator. */
TA_PRIVATE void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment);
TA_PRIVATE void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout);
TA_PRIVATE void ta_mem_copy(void *dst, const void *src, size_t size);
TA_PRIVATE void ta_mem_zero(void *dst, size_t size);
TA_PRIVATE bool ta_str_equal(ta_str left, ta_str right);

/*
 * A growable array of fixed-size elements. `items` is reallocated as it
 * grows, so callers keep indices, never element pointers, across a push.
 */
typedef struct {
    ta_allocator allocator;
    void *items;
    uint32_t count;
    uint32_t capacity;
    size_t element_size;
    size_t element_alignment;
} ta_vec;

TA_PRIVATE void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout);
TA_PRIVATE void ta_vec_destroy(ta_vec *vec);
TA_PRIVATE ta_result ta_vec_reserve(ta_vec *vec, uint32_t count);
/* Appends `count` zeroed elements and stores the index of the first in `*out`. */
TA_PRIVATE ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out);
TA_PRIVATE ta_result ta_vec_push(ta_vec *vec, const void *element);
TA_PRIVATE void *ta_vec_at(const ta_vec *vec, uint32_t index);

/* Bitsets of 64-bit words, used for state sets. */
TA_PRIVATE inline uint32_t ta_bits_words(uint32_t bits) { return (bits + 63u) / 64u; }
TA_PRIVATE inline bool ta_bits_get(const uint64_t *words, uint32_t bit) {
    return (words[bit / 64u] >> (bit % 64u)) & 1u;
}
TA_PRIVATE inline void ta_bits_put(uint64_t *words, uint32_t bit, bool value) {
    uint64_t mask = (uint64_t)1u << (bit % 64u);
    if (value) {
        words[bit / 64u] |= mask;
    } else {
        words[bit / 64u] &= ~mask;
    }
}

#define TA_VEC_LAYOUT(T) (&(ta_tree_list_layout){ .element_size = sizeof(T), .element_alignment = _Alignof(T) })

/*
 * Bounded text output: writes while there is room and always counts the full
 * length, so one pass both sizes and fills a caller's buffer.
 */
typedef struct {
    char *buffer;
    uint32_t capacity;
    uint32_t length;
} ta_text_writer;

TA_PRIVATE void ta_text_put(ta_text_writer *writer, ta_str text);
TA_PRIVATE void ta_text_put_u64(ta_text_writer *writer, uint64_t value);
/* NUL-terminates; TA_ERR_NOMEM (and nothing useful written) when it did not fit. */
TA_PRIVATE ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed);
TA_PRIVATE inline ta_str ta_str_from_cstr(const char *text) {
    ta_str out = { .ptr = text, .len = 0 };
    while (text[out.len] != '\0') {
        out.len += 1;
    }
    return out;
}



struct ta_arena_block {
    struct ta_arena_block *next;
    size_t allocation_size;
    size_t alignment;
    size_t capacity;
    size_t used;
    unsigned char data[];
};

/* Copies text into the arena. An empty string yields a non-null empty ta_str. */
TA_PRIVATE ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out);
/* Bytes the arena holds from its backing allocator, block headers included. */
TA_PRIVATE size_t ta_arena_allocated_bytes(const ta_arena *arena);



/*
 * Storage behind the public ta_tree / ta_tree_list (include/tinyactors.h).
 *
 * An importer authors a specification tree bottom-up: the transitions of a
 * state are opened, filled and closed while the state list is still open. A
 * single bump region cannot serve that, because the inner list's elements
 * would land between the outer list's. Each open list therefore stages its
 * elements in a chain of chunks of its own, and ta_tree_close() copies the
 * chain into one contiguous, zero-terminated array. Every allocation, the
 * staging chunks included, comes from one arena the tree owns, so
 * ta_tree_destroy() returns all of it and no list needs a matching close.
 */

typedef struct ta_tree_chunk {
    struct ta_tree_chunk *next;
    unsigned char *elements;
    size_t count;
    size_t capacity;
} ta_tree_chunk;

/* One open list: its chunks in insertion order, and what it has staged. */
typedef struct {
    ta_tree_chunk *first;
    ta_tree_chunk *last;
    size_t count;
    size_t next_capacity;
} ta_tree_list_state;

typedef struct {
    ta_arena arena;
} ta_tree_state;



/* Atom text lives in arena blocks, so a returned ta_str never moves. */
typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_str text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_vec entries;
    uint32_t *buckets;
    uint32_t bucket_count;
    ta_arena strings;
} ta_atom_table;

#define TA_ATOM_NONE UINT32_MAX

TA_PRIVATE ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
TA_PRIVATE void ta_atom_table_destroy(ta_atom_table *table);
TA_PRIVATE ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out);
TA_PRIVATE ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
/* Looks text up without interning it; TA_ERR_NOT_FOUND when it is not an atom. */
TA_PRIVATE ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out);
TA_PRIVATE bool ta_atom_valid(const ta_atom_table *table, ta_atom atom);
TA_PRIVATE uint32_t ta_hash_bytes(uint32_t seed, const void *bytes, size_t size);



typedef struct {
    ta_event_name_id parent;
    ta_atom segment;
    uint16_t depth;
    uint16_t flags;
    uint32_t hash_next;
} ta_event_name_node;

typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    ta_vec nodes;
    uint32_t *buckets;
    uint32_t bucket_count;
} ta_event_name_table;

enum {
    TA_EVENT_NAME_ROOT = 0u,
    TA_EVENT_NAME_ERROR = 1u,
    TA_EVENT_NAME_ERROR_EXECUTION = 2u,
    TA_EVENT_NAME_ERROR_COMMUNICATION = 3u,
    TA_EVENT_NAME_ERROR_PLATFORM = 4u,
    TA_EVENT_NAME_DONE = 5u,
    TA_EVENT_NAME_DONE_STATE = 6u,
    TA_EVENT_NAME_DONE_INVOKE = 7u
};

#define TA_EVENT_NAME_MAX_DEPTH 64u

TA_PRIVATE ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
TA_PRIVATE void ta_event_name_table_destroy(ta_event_name_table *table);
TA_PRIVATE ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
/* Interns one more segment below `parent`. */
TA_PRIVATE ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out);
/* Interns a dotted name; used by the compiler for authored names only. */
TA_PRIVATE ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out);
TA_PRIVATE bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix);
TA_PRIVATE bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name);
/* Writes the name's segments root-first; returns the segment count. */
TA_PRIVATE uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]);



/* ta_value is fully public; this module has no private declarations. */



/* Events, faults, and queued events are fully public. */



typedef enum {
    TA_FAULT_CONTEXT_CONDITION,
    TA_FAULT_CONTEXT_VALUE,
    TA_FAULT_CONTEXT_LOCATION,
    TA_FAULT_CONTEXT_ASSIGN,
    TA_FAULT_CONTEXT_SCRIPT,
    TA_FAULT_CONTEXT_SEND_EVAL,
    TA_FAULT_CONTEXT_SEND_DISPATCH,
    TA_FAULT_CONTEXT_INVOKE_START,
    TA_FAULT_CONTEXT_INVOKE_CANCEL,
    TA_FAULT_CONTEXT_INVOKE_FORWARD,
    TA_FAULT_CONTEXT_INVOKE_UNAVAILABLE
} ta_fault_context;

typedef struct {
    ta_fault_context context;
    ta_send_id send_id;
    /* Used for naming/bookkeeping; does not populate ta_event.invoke_id. */
    ta_invoke_id invoke_id;
    /* May be null only for a core-generated invocation-unavailable failure. */
    const ta_fault *fault;
    ta_event *out;
} ta_fault_event_params;

ta_event_name_id ta_fault_event_name(ta_system *system,
                                     const ta_fault_event_params *params);
ta_result ta_fault_to_internal_event(ta_system *system,
                                     const ta_fault_event_params *params);



/*
 * The SCXML Event I/O Processor target URI grammar (ADR 0044).
 *
 * Core owns it in both directions, so a literal `target`, an evaluated
 * `targetexpr`, and the text core writes for `$_event.origin` all mean the
 * same thing. `ta_scxml_target_parse()` and `ta_scxml_target_format()` are
 * the public entry points; this is the writer the data-model host view
 * shares with them.
 */

/* Writes the URI of `target`; false when its kind has no URI (SELF, INVALID). */
TA_PRIVATE bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target);



/* External delivery and the router boundary are fully public. */

/*
 * A named ta_actor_ref borrows its family and name text from whoever built
 * it. An owner that outlives that text — a delivery the router retains, a
 * named actor's own self reference — copies it with ta_address_copy() and
 * gives it back with ta_address_release(). Both ignore a runtime ref.
 */
TA_PRIVATE ta_result ta_address_copy(ta_actor_ref *ref, ta_allocator allocator);
TA_PRIVATE void ta_address_release(ta_actor_ref *ref, ta_allocator allocator);



typedef struct {
    ta_time now;
} ta_clock;

TA_PRIVATE inline ta_time ta_clock_now(const ta_clock *clock) { return clock->now; }
TA_PRIVATE inline void ta_clock_set(ta_clock *clock, ta_time now) { clock->now = now; }



typedef enum {
    TA_CONFIG_SINGLE_LEAF,
    TA_CONFIG_BITMAP32,
    TA_CONFIG_BITMAP64,
    TA_CONFIG_WORDS
} ta_configuration_kind;

/*
 * The compiler currently always selects TA_CONFIG_WORDS: one bit per state ID,
 * of which only atomic and final states are ever set. `atomic_state_count` is
 * the number of addressable bits, `bytes` the storage size in whole words.
 */
typedef struct {
    ta_configuration_kind kind;
    uint16_t bytes;
    uint16_t alignment;
    uint32_t atomic_state_count;
} ta_configuration_layout;

typedef struct ta_configuration ta_configuration;
struct ta_configuration {
    void *storage;
    const ta_configuration_layout *layout;
};

TA_PRIVATE bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
TA_PRIVATE void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
TA_PRIVATE void ta_configuration_clear(ta_configuration *config);



/*
 * One slot per history pseudo-state: a state bitmap of `bytes` at `offset`
 * within the history storage. An empty bitmap means never recorded.
 */
typedef struct {
    uint32_t offset;
    uint16_t bytes;
    uint16_t flags;
} ta_history_slot;

typedef struct {
    ta_history_slot *slots;
    uint32_t slot_count;
    uint32_t storage_bytes;
} ta_history_layout;

typedef struct ta_history_state ta_history_state;
struct ta_history_state {
    void *storage;
    const ta_history_layout *layout;
};

TA_PRIVATE uint64_t *ta_history_slot_words(const ta_history_state *history, uint32_t slot);



typedef enum {
    TA_CONT_IDLE = 0,
    TA_CONT_SELECT_TRANSITIONS,
    TA_CONT_EXIT_STATES,
    TA_CONT_EXEC_TRANSITIONS,
    TA_CONT_ENTER_STATES,
    TA_CONT_PROCESS_INTERNAL,
    TA_CONT_INVOKE_STATES,
    TA_CONT_FINISH_MACROSTEP
} ta_continuation_phase;

typedef enum {
    TA_EXEC_FRAME_ACTIONS = 0,
    TA_EXEC_FRAME_FOREACH
} ta_exec_frame_kind;

typedef struct {
    ta_exec_frame_kind kind;
    ta_span32 actions;
    uint32_t cursor;
    uint32_t iteration;
    uint32_t iteration_count;
    ta_value iteration_snapshot;
} ta_exec_frame;

typedef struct ta_continuation ta_continuation;
struct ta_continuation {
    uint32_t pc;
    uint32_t cursor;
    uint32_t aux;
    ta_continuation_phase phase;
    bool has_external_event;
    ta_event external_event;
    /* Owned deep clone; null unless the external router requested retry. */
    ta_external_delivery *pending_external_delivery;
    ta_transition_id *candidates;
    uint32_t candidate_count;
    ta_transition_id *selected;
    uint32_t selected_count;
    ta_state_id *states_to_exit;
    uint32_t exit_count;
    ta_state_id *states_to_enter;
    uint32_t entry_count;
    ta_state_id *states_to_invoke;
    uint32_t invoke_count;
    ta_exec_frame *frames;
    uint32_t frame_count;
};



/*
 * An evaluated delayed send. For the built-in SCXML processor `destination`
 * is the resolved external target; custom processors receive `target`.
 */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_actor_ref destination;
    ta_value target;
    ta_value data;
    uint32_t heap_index;
    bool active;
    /*
     * Already sent: the router accepted nothing and the send waits here to be
     * offered again. <cancel> and session termination leave it in place.
     */
    bool submitted;
    /* Submitted sends only: the envelope, which outlives the sending session. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
} ta_delayed_send;

/* A binary min-heap ordered by (due, id); ids increase in scheduling order. */
typedef struct {
    ta_allocator allocator;
    ta_delayed_send *items;
    uint32_t count;
    uint32_t capacity;
    ta_timer_id next_id;
} ta_timer_heap;

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Takes ownership of the send's values; assigns send->id when it is zero. */
TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send);
/* Removes and releases every pending, not yet submitted send of owner with send_id. */
TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id);
/* Like ta_timer_cancel(), but also removes submitted sends. */
TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap,
                            ta_actor_id owner,
                            ta_send_id send_id);
/* Removes and releases every not yet submitted send of owner (session termination). */
TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_actor_id owner);
/* Removes and releases every pending send of owner. */
TA_PRIVATE void ta_timer_cancel_owner(ta_timer_heap *heap, ta_actor_id owner);
TA_PRIVATE uint32_t ta_timer_count_owner(const ta_timer_heap *heap, ta_actor_id owner);
TA_PRIVATE const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap);
/* Transfers ownership of the earliest send's values to *out. */
TA_PRIVATE ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);



typedef struct ta_actor ta_actor;

/* The data-model host view resolves everything through the calling actor. */
typedef struct {
    ta_system *system;
    ta_actor *actor;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};



typedef struct {
    ta_atom type;
    ta_io io;
} ta_io_registration;



typedef struct {
    ta_atom type;
    ta_invoker invoker;
} ta_invoker_registration;

/*
 * Private bookkeeping for one active invocation, kept parallel to the public
 * ta_active_invoke array of the actor's sparse state. `child` is set when the
 * service spawned an SCXML session with this invocation as its parent.
 */
typedef struct {
    ta_invoke_decl_id decl;
    ta_state_id state;
    ta_invoker invoker;
    ta_actor_id child;
    bool has_child;
} ta_invoke_meta;

/* The send ID of a child's queued done.invoke completion record. */
#define TA_SEND_ID_COMPLETION (UINT64_MAX - 1u)



/* Functions over compiled definitions, shared by the compiler and interpreter. */

typedef enum {
    TA_TRANSITION_EXTERNAL,
    TA_TRANSITION_INTERNAL
} ta_transition_type;

typedef enum {
    TA_BINDING_EARLY,
    TA_BINDING_LATE
} ta_datamodel_binding_mode;

typedef enum {
    TA_VALUE_SOURCE_NONE,
    TA_VALUE_SOURCE_EXPR,
    TA_VALUE_SOURCE_LOCATION,
    TA_VALUE_SOURCE_CONTENT
} ta_value_source_kind;

typedef struct {
    ta_value_source_kind kind;
    union {
        ta_expr_id expr;
        ta_expr_id location;
        ta_content_id content;
    } as;
} ta_value_source;

typedef enum {
    TA_EVENT_SOURCE_NAME,
    TA_EVENT_SOURCE_EXPR
} ta_event_source_kind;

typedef struct {
    ta_event_source_kind kind;
    union {
        ta_event_name_id name;
        ta_expr_id expr;
    } as;
} ta_event_source;

/*
 * A compiled `x` / `xexpr` attribute pair. ATOM holds interned literal text;
 * NONE is used only where the attribute is optional.
 */
typedef enum {
    TA_TEXT_SOURCE_NONE,
    TA_TEXT_SOURCE_ATOM,
    TA_TEXT_SOURCE_EXPR
} ta_text_source_kind;

typedef struct {
    ta_text_source_kind kind;
    union {
        ta_atom atom;
        ta_expr_id expr;
    } as;
} ta_text_source;

/*
 * A compiled <send>/<invoke> `id` / `idlocation`. LITERAL holds the author's
 * interned ID. GENERATED and LOCATION make the runtime generate an ID on each
 * execution; LOCATION also assigns it to the location.
 */
typedef enum {
    TA_ID_SOURCE_GENERATED,
    TA_ID_SOURCE_LITERAL,
    TA_ID_SOURCE_LOCATION
} ta_id_source_kind;

typedef struct {
    ta_id_source_kind kind;
    union {
        ta_atom literal;
        ta_expr_id location;
    } as;
} ta_id_source;

/*
 * Index into ta_definition.authored_texts: authored text the compiler
 * normalizes and the definition keeps for ta_definition_export().
 */
typedef uint32_t ta_authored_text_id;
#define TA_AUTHORED_TEXT_NONE UINT32_MAX

/*
 * LITERAL is a duration parsed at compile time, in ta_time milliseconds;
 * `text` is the authored duration (e.g. "2.5s"), which the parse discards.
 */
typedef enum {
    TA_DELAY_SOURCE_NONE,
    TA_DELAY_SOURCE_LITERAL,
    TA_DELAY_SOURCE_EXPR
} ta_delay_source_kind;

typedef struct {
    ta_delay_source_kind kind;
    ta_authored_text_id text;
    union {
        ta_time literal;
        ta_expr_id expr;
    } as;
} ta_delay_source;

typedef enum {
    TA_ACTION_RAISE,
    TA_ACTION_SEND,
    TA_ACTION_CANCEL,
    TA_ACTION_ASSIGN,
    TA_ACTION_SCRIPT,
    TA_ACTION_LOG,
    TA_ACTION_IF,
    TA_ACTION_FOREACH,
    TA_ACTION_INVOKE
} ta_action_kind;

typedef struct {
    ta_event_name_id event;
    ta_value_source data;
} ta_raise_action;

/*
 * A NONE type selects the built-in SCXML Event I/O Processor. A literal
 * target is TA_VALUE_SOURCE_CONTENT.
 */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_event_source event;
    ta_value_source target;
    ta_delay_source delay;
    ta_value_source data;
    ta_span32 params;
} ta_send_action;

/* Never NONE: SCXML requires exactly one of sendid or sendidexpr. */
typedef struct {
    ta_text_source sendid;
} ta_cancel_action;

typedef struct {
    ta_atom label;
    bool has_label;
    ta_expr_id expr;
    bool has_expr;
} ta_log_action;

typedef struct {
    ta_expr_id location;
    ta_value_source value;
} ta_assign_action;

typedef struct {
    ta_expr_id condition;
    ta_span32 then_actions;
    ta_span32 else_actions;
} ta_if_action;

typedef struct {
    ta_expr_id array;
    ta_atom item;
    ta_atom index;
    bool has_index;
    ta_span32 body_actions;
} ta_foreach_action;

typedef struct {
    ta_action_kind kind;
    union {
        ta_raise_action raise;
        ta_send_action send;
        ta_cancel_action cancel;
        ta_assign_action assign;
        ta_expr_id script;
        ta_log_action log;
        ta_if_action conditional;
        ta_foreach_action foreach;
        ta_invoke_decl_id invoke;
    } as;
} ta_action_def;

typedef struct {
    ta_atom name;
    ta_value_source value;
} ta_param_def;

typedef struct {
    ta_atom name;
    ta_state_id owner_state;
    ta_initializer_id initializer;
} ta_data_decl;

typedef struct {
    ta_value_source value;
} ta_initializer_def;

typedef struct {
    ta_value_source content;
    ta_span32 params;
} ta_done_data_def;

typedef struct {
    ta_bytes bytes;
} ta_content_def;

/* A literal src is TA_VALUE_SOURCE_CONTENT. */
typedef struct {
    ta_id_source id;
    ta_text_source type;
    ta_value_source src;
    ta_value_source data;
    ta_span32 params;
    ta_span32 finalize_actions;
    bool autoforward;
    /* The state whose entry starts this invocation. */
    ta_state_id owner;
} ta_invoke_decl;

typedef struct {
    ta_atom type;
    ta_bytes payload;
} ta_extension_def;

typedef struct {
    ta_atom name;
    ta_state_id parent;
    ta_state_kind kind;
    ta_span32 children;
    ta_span32 transitions;
    ta_initial_transition_id initial;
    /* Ranges of definition.handlers: one action block per <onentry>/<onexit>. */
    ta_span32 onentry_handlers;
    ta_span32 onexit_handlers;
    ta_event_name_id done_event;
    ta_done_data_id done_data;
    /* Index into history.slots for history states, else UINT32_MAX. */
    uint32_t history_slot;
} ta_state_def;

typedef struct {
    ta_span32 segments;
} ta_event_pattern;

typedef struct {
    ta_atom atom;
    uint32_t flags;
} ta_event_pattern_segment;

enum {
    TA_EVENT_PATTERN_SEGMENT_WILDCARD = 1u << 0
};

typedef struct {
    const ta_event_pattern_segment *segments;
    uint32_t segment_count;
} ta_event_pattern_spec;

typedef struct {
    ta_state_id source;
    ta_span32 targets;
    ta_span32 event_patterns;
    ta_expr_id condition;
    ta_transition_type type;
    ta_span32 actions;
    uint32_t document_order;
    /*
     * The authored event descriptor text, which event_patterns normalize
     * (whitespace, ".*" forms); TA_AUTHORED_TEXT_NONE when absent.
     */
    ta_authored_text_id event_text;
} ta_transition_def;

typedef struct {
    ta_state_id owner;
    ta_span32 targets;
    ta_span32 actions;
    /* False when `targets` is the SCXML default (the first child), not authored. */
    bool authored_targets;
} ta_initial_transition_def;

struct ta_definition {
    ta_allocator allocator;
    /* Owns every array and string below. */
    ta_arena arena;
    /* The system whose atoms and event names this definition uses. */
    const ta_system *system;
    bool registered;
    /* ta_state_def.children ranges index this array. */
    ta_state_id *children;
    uint32_t child_count;
    ta_definition_id id;
    ta_atom name;
    ta_definition_identity identity;
    /* The <scxml name> attribute, exposed as _name; TA_ATOM_NONE when absent. */
    ta_atom document_name;
    ta_configuration_layout configuration;
    ta_history_layout history;
    ta_state_def *states;
    uint32_t state_count;
    ta_transition_def *transitions;
    uint32_t transition_count;
    ta_initial_transition_def *initial_transitions;
    uint32_t initial_transition_count;
    ta_initial_transition_id root_initial;
    /*
     * The <scxml> <script>: a block of zero or one TA_ACTION_SCRIPT in the
     * action stream, run once per session before the root initial transition.
     * A block, not a bare ta_expr_id, so it shares executable-content error
     * semantics (SCXML 4.9) and continuation frames with every other block.
     */
    ta_span32 global_script_actions;
    ta_event_pattern *event_patterns;
    uint32_t event_pattern_count;
    ta_event_pattern_segment *event_pattern_segments;
    uint32_t event_pattern_segment_count;
    ta_state_id *targets;
    uint32_t target_count;
    /* Action blocks are ta_span32 ranges of action_defs. */
    ta_action_def *action_defs;
    uint32_t action_count;
    /*
     * The action block of each <onentry>/<onexit> handler. SCXML runs every
     * handler as a separate block (3.8, 3.9), so the handlers of a state are
     * not merged into one block.
     */
    ta_span32 *handlers;
    uint32_t handler_count;
    ta_param_def *params;
    uint32_t param_count;
    ta_data_decl *data_decls;
    uint32_t data_decl_count;
    ta_initializer_def *initializers;
    uint32_t initializer_count;
    ta_content_def *contents;
    uint32_t content_count;
    ta_done_data_def *done_data;
    uint32_t done_data_count;
    ta_invoke_decl *invokes;
    uint32_t invoke_decl_count;
    ta_extension_def *extensions;
    uint32_t extension_count;
    ta_expression *expressions;
    uint32_t expression_count;
    /*
     * Authored input the compiler normalizes, kept so ta_definition_export()
     * rebuilds a spec with the same fingerprint (ADR 0042).
     */
    ta_str *authored_texts;
    uint32_t authored_text_count;
    /* The authored <scxml binding>; DEFAULT and EARLY both compile to early. */
    ta_scxml_binding_kind authored_binding;
    /* The authored <scxml datamodel>; empty when absent. */
    ta_str authored_datamodel;
    /* True when `identity.revision` is the compiler's fingerprint, not an explicit identity. */
    bool fingerprinted;
    ta_datamodel_binding_mode binding_mode;
    ta_datamodel datamodel;
    void *datamodel_shared;
    bool datamodel_prepared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
    /* Resident actors whose `definition` points here; see ta_actor_free(). */
    uint32_t actor_count;
    /*
     * Set by ta_system_retire_definition(): no new actors, and the system
     * frees the definition once actor_count reaches zero (ADR 0043).
     */
    bool retired;
    /* Registration order; see ta_system_find_definition(). */
    uint64_t registration_serial;
};

/* A registered definition, retired or not; NULL for an unknown or stale ID. */
TA_PRIVATE const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id);
/* Bytes of one actor's session storage: two configurations and history. */
TA_PRIVATE size_t ta_definition_session_bytes(const ta_definition *definition);
/* Bytes allocated for one actor's data-model instance (never zero). */
TA_PRIVATE size_t ta_definition_instance_bytes(const ta_definition *definition);
TA_PRIVATE bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state);
TA_PRIVATE bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor);
TA_PRIVATE bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event);



/* The declarative specification tree and ta_tree are fully public. */

/* Parses an SCXML duration ("500ms", "2.5s") into logical milliseconds. */
TA_PRIVATE ta_result ta_duration_parse(ta_str text, ta_time *out);



/*
 * ta_definition_export() rebuilds a ta_scxml_spec from a compiled definition.
 * It reads the compiled tables and the authored text the compiler keeps for
 * it (ta_definition.authored_texts and the other `authored_*` fields); see
 * docs/internal/definition-inspection.md.
 */



typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_actor_ref self;
    ta_actor_residency residency;
} ta_actor_record;

typedef struct ta_activation_effects ta_activation_effects;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
    ta_vec internal_events; /* ta_queued_event, FIFO */
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
    /* Parallel to `invokes`; see invoke.h. */
    ta_invoke_meta *invoke_meta;
    /* States entered in this macrostep whose invokes start when it ends. */
    ta_vec pending_invokes;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    bool has_parent;
    bool completion_pending;
} ta_actor_sparse_state;

struct ta_actor {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration, history, and late-binding bits. */
    void *session_storage;
    size_t session_bytes;
    ta_configuration configuration;
    ta_history_state history;
    /* Late binding: one bit per state whose data has been initialized. */
    uint64_t *late_initialized;
    void *datamodel_instance;
    bool datamodel_initialized;
    ta_actor_sparse_state *sparse;
    ta_actor_id replacement_of;
    uint64_t session_epoch;
    bool published;
    ta_dm_host_impl host_impl;
    ta_dm_host host;

    /* Execution state between microsteps; a macrostep resumes from here. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    bool has_external_event;
    ta_event external_event;
    bool has_current_event;
    ta_event current_event;
    uint64_t next_send_id;
    /*
     * While the interpreter runs, the full active set it is updating (ancestors
     * included), so In() sees states as SCXML defines them mid-microstep.
     */
    const uint64_t *live_active;
};

typedef struct {
    ta_actor *actor;
    uint32_t generation;
} ta_actor_slot;

TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id);
TA_PRIVATE bool ta_actor_state_active(const ta_actor *actor, ta_state_id state);



/* Session images are opaque; struct ta_session_image is private to session_image.c. */



/*
 * Tinyactors core deliberately defines no persistence-store protocol.  This
 * compatibility module remains empty while durable checkpointing, command
 * sourcing, and storage adapters live in optional higher-level components.
 */



typedef struct {
    ta_definition *definition;
    uint32_t generation;
    /* While free: the next free slot, or UINT32_MAX. */
    uint32_t next_free;
} ta_definition_slot;

struct ta_system {
    ta_allocator allocator;
    ta_clock clock;
    ta_atom_table atoms;
    ta_event_name_table event_names;

    /*
     * ta_definition_slot; a definition ID is (generation << 32) | slot, as
     * for actors. Freed slots form a list through `next_free`.
     */
    ta_vec definitions;
    uint32_t free_definition_slot;
    /* Orders registrations, so the newest identity match wins after slot reuse. */
    uint64_t next_definition_serial;

    /* ta_actor_slot; an actor ID is (generation << 32) | slot. */
    ta_vec actors;

    ta_timer_heap timers;

    ta_vec io_processors; /* ta_io_registration */
    ta_vec invokers;      /* ta_invoker_registration */

    ta_actor_directory directory;
    ta_external_event_router external_events;
    ta_log_sink log_sink;
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    /* Invoke: the canonical SCXML invoker type and the generated-ID counter. */
    ta_atom scxml_invoke_type;
    uint64_t next_invoke_serial;
    ta_arena scratch;
};

/*
 * The registered definition behind a live ID, retired or not; NULL for an
 * unknown or stale ID.
 */
TA_PRIVATE ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id);
/* Frees a registered definition and its slot; no actor may use it. */
TA_PRIVATE void ta_system_free_definition(ta_system *system, ta_definition *definition);
TA_PRIVATE const ta_io *ta_system_find_io(const ta_system *system, ta_atom type);
TA_PRIVATE bool ta_system_is_scxml_io(const ta_system *system, ta_atom type);



/*
 * Per-call interpreter scratch. The actor holds all state between microsteps,
 * so a budget pauses only at microstep boundaries, which the actor fully
 * describes: configuration, history, internal queue, and pending event.
 */
typedef struct {
    ta_system *system;
    ta_actor *actor;
    const ta_definition *definition;
    uint32_t budget;
    uint32_t steps;
    uint32_t words;
    uint64_t *active;
    uint64_t *to_exit;
    uint64_t *to_enter;
    uint64_t *default_entry;
    uint64_t *domain_targets;
    uint64_t *entry_targets;
    uint64_t *exit_left;
    uint64_t *exit_right;
    ta_state_id *default_history;
    ta_transition_id *enabled;
    uint32_t enabled_count;
    ta_transition_id *filtered;
    bool *remove;
    /* Bootstrap only: data declarations the instance's binding already supplies. */
    uint64_t *bound_decls;
} ta_interpreter;

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/* Takes ownership of the event's data. */
TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send);
TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);

/* Send IDs as text: literal IDs are their author's text; generated ones round-trip. */
TA_PRIVATE void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer);
/* Returns 0 when the text names no send ID. */
TA_PRIVATE ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text);
/* ------------------------------------------------------------------------
 * Interpreter services used by invoke.c
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_interpreter_value_params;

TA_PRIVATE ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor);
TA_PRIVATE ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params);
/* Queues error.execution; returns TA_OK unless queueing itself failed. */
TA_PRIVATE ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault);
/* Runs a block; a failed block has already queued its error. */
TA_PRIVATE ta_result ta_interpreter_run_actions(ta_interpreter *it, ta_span32 block);
TA_PRIVATE bool ta_interpreter_internal_pending(const ta_actor *actor);

/* ------------------------------------------------------------------------
 * Invoke hooks, implemented in invoke.c
 */

/* Interns an SCXML invoke ID's text; the runtime ID is its atom + 1. */
TA_PRIVATE ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out);
TA_PRIVATE ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke);
TA_PRIVATE ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state);
TA_PRIVATE ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state);
/* Starts the invokes of states entered during the finished macrostep. */
TA_PRIVATE ta_result ta_invoke_start_pending(ta_interpreter *it);
/* Applies <finalize> and autoforwarding to the actor's current (external) event. */
TA_PRIVATE ta_result ta_invoke_before_external(ta_interpreter *it);
/* Cancels every invocation and publishes done.invoke to the parent, if any. */
TA_PRIVATE ta_result ta_invoke_finished(ta_interpreter *it);
TA_PRIVATE bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out);
/* The invoke ID an event from `sender` to `destination` carries: set for child-to-parent events. */
TA_PRIVATE ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination);
TA_PRIVATE void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child);
/* Cancels the actor's services and frees its invoke bookkeeping. */
TA_PRIVATE void ta_invoke_release(ta_system *system, ta_actor *actor);


TA_PRIVATE void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment) {
    if (allocator.v == NULL || allocator.v->alloc == NULL || size == 0) {
        return NULL;
    }
    return allocator.v->alloc(allocator.self, size, alignment);
}

TA_PRIVATE void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout) {
    if (ptr == NULL || allocator.v == NULL || allocator.v->free == NULL) {
        return;
    }
    ta_allocator_free_params params = *layout;
    params.ptr = ptr;
    allocator.v->free(allocator.self, &params);
}

TA_PRIVATE void ta_mem_copy(void *dst, const void *src, size_t size) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = in[index];
    }
}

TA_PRIVATE void ta_mem_zero(void *dst, size_t size) {
    unsigned char *out = dst;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = 0;
    }
}

TA_PRIVATE bool ta_str_equal(ta_str left, ta_str right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

TA_PRIVATE void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout) {
    *vec = (ta_vec){
        .allocator = allocator,
        .element_size = layout->element_size,
        .element_alignment = layout->element_alignment,
    };
}

TA_PRIVATE void ta_vec_destroy(ta_vec *vec) {
    if (vec->items != NULL) {
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

TA_PRIVATE ta_result ta_vec_reserve(ta_vec *vec, uint32_t count) {
    if (count <= vec->capacity) {
        return TA_OK;
    }
    uint32_t capacity = vec->capacity == 0 ? 8u : vec->capacity;
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity *= 2u;
    }
    void *items = ta_mem_alloc(vec->allocator, capacity * vec->element_size, vec->element_alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->items != NULL) {
        ta_mem_copy(items, vec->items, vec->count * vec->element_size);
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = items;
    vec->capacity = capacity;
    return TA_OK;
}

TA_PRIVATE ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out) {
    if (count > UINT32_MAX - vec->count) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(vec, vec->count + count);
    if (result != TA_OK) {
        return result;
    }
    if (out != NULL) {
        *out = vec->count;
    }
    if (count != 0) {
        ta_mem_zero((unsigned char *)vec->items + vec->count * vec->element_size,
                    count * vec->element_size);
    }
    vec->count += count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_vec_push(ta_vec *vec, const void *element) {
    uint32_t index = 0;
    ta_result result = ta_vec_extend(vec, 1, &index);
    if (result == TA_OK) {
        ta_mem_copy(ta_vec_at(vec, index), element, vec->element_size);
    }
    return result;
}

TA_PRIVATE void *ta_vec_at(const ta_vec *vec, uint32_t index) {
    return (unsigned char *)vec->items + (size_t)index * vec->element_size;
}

TA_PRIVATE void ta_text_put(ta_text_writer *writer, ta_str text) {
    for (uint32_t index = 0; index < text.len; index += 1) {
        if (writer->buffer != NULL && writer->length < writer->capacity) {
            writer->buffer[writer->length] = text.ptr[index];
        }
        writer->length += 1;
    }
}

TA_PRIVATE void ta_text_put_u64(ta_text_writer *writer, uint64_t value) {
    char digits[20];
    uint32_t count = 0;
    do {
        digits[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0);
    char ordered[20];
    for (uint32_t index = 0; index < count; index += 1) {
        ordered[index] = digits[count - 1u - index];
    }
    ta_text_put(writer, (ta_str){ .ptr = ordered, .len = count });
}

TA_PRIVATE ta_result ta_text_finish(ta_text_writer *writer, uint32_t *needed) {
    *needed = writer->length + 1u;
    if (writer->buffer == NULL || writer->capacity < writer->length + 1u) {
        return TA_ERR_NOMEM;
    }
    writer->buffer[writer->length] = '\0';
    return TA_OK;
}


static bool ta_arena_valid_alignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

static size_t ta_arena_max_size(size_t left, size_t right) {
    return left > right ? left : right;
}

static void *ta_arena_allocate_from_block(ta_arena_block *block,
                                          size_t size,
                                          size_t alignment) {
    uintptr_t base = (uintptr_t)block->data;
    uintptr_t current = base + block->used;
    uintptr_t aligned = (current + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    size_t offset = (size_t)(aligned - base);
    if (offset > block->capacity || size > block->capacity - offset) {
        return NULL;
    }
    block->used = offset + size;
    return (void *)aligned;
}

TA_API ta_result ta_arena_init(ta_arena *arena, ta_allocator backing, size_t block_size) {
    if (arena == NULL || backing.v == NULL || backing.v->alloc == NULL ||
        backing.v->free == NULL || block_size == 0) {
        return TA_ERR_INVALID;
    }
    *arena = (ta_arena){
        .backing = backing,
        .block_size = block_size,
    };
    return TA_OK;
}

TA_API void ta_arena_reset(ta_arena *arena) {
    if (arena == NULL) {
        return;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        block->used = 0;
    }
}

TA_API void ta_arena_destroy(ta_arena *arena) {
    if (arena == NULL || arena->backing.v == NULL) {
        return;
    }
    ta_arena_block *block = arena->head;
    while (block != NULL) {
        ta_arena_block *next = block->next;
        arena->backing.v->free(
            arena->backing.self,
            &(ta_allocator_free_params){
                .ptr = block,
                .size = block->allocation_size,
                .alignment = block->alignment,
            });
        block = next;
    }
    *arena = (ta_arena){0};
}

TA_API void *ta_arena_alloc(ta_arena *arena, size_t size, size_t alignment) {
    if (arena == NULL || !ta_arena_valid_alignment(alignment) || size == 0) {
        return NULL;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        void *result = ta_arena_allocate_from_block(block, size, alignment);
        if (result != NULL) {
            return result;
        }
    }

    if (size > SIZE_MAX - (alignment - 1u)) {
        return NULL;
    }
    size_t capacity = ta_arena_max_size(arena->block_size, size + alignment - 1u);
    if (capacity > SIZE_MAX - offsetof(ta_arena_block, data)) {
        return NULL;
    }
    size_t allocation_size = offsetof(ta_arena_block, data) + capacity;
    size_t block_alignment = ta_arena_max_size(_Alignof(ta_arena_block), alignment);
    ta_arena_block *block = arena->backing.v->alloc(arena->backing.self,
                                                    allocation_size,
                                                    block_alignment);
    if (block == NULL) {
        return NULL;
    }
    *block = (ta_arena_block){
        .next = arena->head,
        .allocation_size = allocation_size,
        .alignment = block_alignment,
        .capacity = capacity,
    };
    arena->head = block;
    return ta_arena_allocate_from_block(block, size, alignment);
}

TA_PRIVATE size_t ta_arena_allocated_bytes(const ta_arena *arena) {
    size_t total = 0;
    for (const ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        total += block->allocation_size;
    }
    return total;
}

TA_PRIVATE ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (text.len != 0) {
        ta_mem_copy(copy, text.ptr, text.len);
    }
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return TA_OK;
}


/* The arena block every tree starts from, as elsewhere in core. */
#define TA_TREE_BLOCK_SIZE 4096u
/* Elements in a list's first staging chunk; every next chunk doubles. */
#define TA_TREE_LIST_FIRST_CHUNK 8u

static bool ta_tree_power_of_two(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

/* Records the first failure only: an importer reports one result at its
 * outer boundary, and a later, smaller allocation must not clear it. */
static void ta_tree_fail(ta_tree *tree, ta_result result) {
    if (tree != NULL && tree->result == TA_OK) {
        tree->result = result;
    }
}

TA_API void ta_tree_init(ta_tree *tree, ta_allocator allocator) {
    if (tree == NULL) {
        return;
    }
    *tree = (ta_tree){ .allocator = allocator, .result = TA_OK, .impl = NULL };
    if (allocator.v == NULL || allocator.v->alloc == NULL || allocator.v->free == NULL) {
        tree->result = TA_ERR_INVALID;
        return;
    }
    ta_tree_state *state = ta_mem_alloc(allocator, sizeof(ta_tree_state), _Alignof(ta_tree_state));
    if (state == NULL) {
        tree->result = TA_ERR_NOMEM;
        return;
    }
    *state = (ta_tree_state){0};
    ta_result result = ta_arena_init(&state->arena, allocator, TA_TREE_BLOCK_SIZE);
    if (result != TA_OK) {
        ta_mem_free(allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
        tree->result = result;
        return;
    }
    tree->impl = state;
}

TA_API void ta_tree_destroy(ta_tree *tree) {
    if (tree == NULL) {
        return;
    }
    ta_tree_state *state = tree->impl;
    if (state != NULL) {
        ta_arena_destroy(&state->arena);
        ta_mem_free(tree->allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
    }
    *tree = (ta_tree){0};
}

TA_API void *ta_tree_alloc(ta_tree *tree, size_t size, size_t alignment) {
    if (tree == NULL || tree->impl == NULL || tree->result != TA_OK) {
        return NULL;
    }
    if (!ta_tree_power_of_two(alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return NULL;
    }
    ta_tree_state *state = tree->impl;
    /* A zero-sized request still has to yield its own address. */
    void *block = ta_arena_alloc(&state->arena, size == 0 ? 1u : size, alignment);
    if (block == NULL) {
        ta_tree_fail(tree, TA_ERR_NOMEM);
        return NULL;
    }
    return block;
}

TA_API ta_result ta_tree_result(const ta_tree *tree) {
    return tree == NULL ? TA_ERR_INVALID : tree->result;
}

TA_API void ta_tree_list_init(ta_tree_list *list, ta_tree *tree, const ta_tree_list_layout *layout) {
    if (list == NULL) {
        return;
    }
    *list = (ta_tree_list){ .tree = tree };
    if (tree == NULL) {
        return;
    }
    if (layout == NULL || layout->element_size == 0 || !ta_tree_power_of_two(layout->element_alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return;
    }
    list->element_size = layout->element_size;
    list->element_alignment = layout->element_alignment;
    ta_tree_list_state *state =
        ta_tree_alloc(tree, sizeof(ta_tree_list_state), _Alignof(ta_tree_list_state));
    if (state == NULL) {
        return;
    }
    *state = (ta_tree_list_state){ .next_capacity = TA_TREE_LIST_FIRST_CHUNK };
    list->impl = state;
}

/* Appends one more staging chunk, twice the size of the last. */
static ta_tree_chunk *ta_tree_list_grow(ta_tree_list *list, ta_tree_list_state *state) {
    size_t capacity = state->next_capacity == 0 ? TA_TREE_LIST_FIRST_CHUNK : state->next_capacity;
    if (capacity > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    ta_tree_chunk *chunk = ta_tree_alloc(list->tree, sizeof(ta_tree_chunk), _Alignof(ta_tree_chunk));
    if (chunk == NULL) {
        return NULL;
    }
    unsigned char *elements =
        ta_tree_alloc(list->tree, capacity * list->element_size, list->element_alignment);
    if (elements == NULL) {
        return NULL;
    }
    *chunk = (ta_tree_chunk){ .elements = elements, .capacity = capacity };
    if (state->last == NULL) {
        state->first = chunk;
    } else {
        state->last->next = chunk;
    }
    state->last = chunk;
    state->next_capacity = capacity > SIZE_MAX / 2u ? capacity : capacity * 2u;
    return chunk;
}

TA_API void ta_tree_put(ta_tree_list *list, const void *element) {
    if (list == NULL || list->impl == NULL || element == NULL) {
        return;
    }
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return;
    }
    ta_tree_list_state *state = list->impl;
    ta_tree_chunk *chunk = state->last;
    if (chunk == NULL || chunk->count == chunk->capacity) {
        chunk = ta_tree_list_grow(list, state);
        if (chunk == NULL) {
            return;
        }
    }
    ta_mem_copy(chunk->elements + chunk->count * list->element_size, element, list->element_size);
    chunk->count += 1u;
    state->count += 1u;
}

TA_API const void *ta_tree_close(ta_tree_list *list) {
    if (list == NULL || list->impl == NULL) {
        return NULL;
    }
    ta_tree_list_state *state = list->impl;
    /* A closed list is spent: a put after it must not reach the chunks the
     * returned array was copied from. */
    list->impl = NULL;
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return NULL;
    }
    /* Every child list is terminated by a zero-initialized element. */
    size_t total = state->count + 1u;
    if (total > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    unsigned char *array =
        ta_tree_alloc(list->tree, total * list->element_size, list->element_alignment);
    if (array == NULL) {
        return NULL;
    }
    size_t at = 0;
    for (const ta_tree_chunk *chunk = state->first; chunk != NULL; chunk = chunk->next) {
        ta_mem_copy(array + at, chunk->elements, chunk->count * list->element_size);
        at += chunk->count * list->element_size;
    }
    ta_mem_zero(array + at, list->element_size);
    return array;
}


TA_PRIVATE uint32_t ta_hash_bytes(uint32_t seed, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    uint32_t hash = seed ^ 2166136261u;
    for (size_t index = 0; index < size; index += 1) {
        hash ^= in[index];
        hash *= 16777619u;
    }
    return hash;
}

static ta_result ta_atom_rehash(ta_atom_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = TA_ATOM_NONE;
    }
    for (uint32_t atom = 0; atom < table->entries.count; atom += 1) {
        ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        uint32_t bucket = entry->hash & (bucket_count - 1u);
        entry->next = buckets[bucket];
        buckets[bucket] = atom;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator) {
    *table = (ta_atom_table){ .allocator = allocator };
    ta_vec_init(&table->entries, allocator, TA_VEC_LAYOUT(ta_atom_entry));
    ta_result result = ta_arena_init(&table->strings, allocator, 4096);
    if (result == TA_OK) {
        result = ta_atom_rehash(table, 64);
    }
    if (result != TA_OK) {
        ta_atom_table_destroy(table);
    }
    return result;
}

TA_PRIVATE void ta_atom_table_destroy(ta_atom_table *table) {
    ta_vec_destroy(&table->entries);
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    ta_arena_destroy(&table->strings);
    table->buckets = NULL;
    table->bucket_count = 0;
}

TA_PRIVATE ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_hash_bytes(0, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    if (table->entries.count >= TA_ATOM_NONE - 1u) {
        return TA_ERR_NOMEM;
    }
    ta_atom_entry entry = { .hash = hash };
    ta_result result = ta_arena_copy_str(&table->strings, text, &entry.text);
    if (result != TA_OK) {
        return result;
    }
    uint32_t atom = table->entries.count;
    uint32_t bucket = hash & (table->bucket_count - 1u);
    entry.next = table->buckets[bucket];
    result = ta_vec_push(&table->entries, &entry);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[bucket] = atom;
    if (table->entries.count > table->bucket_count) {
        /* A failed rehash leaves the old, still-correct buckets. */
        (void)ta_atom_rehash(table, table->bucket_count * 2u);
    }
    *out = atom;
    return TA_OK;
}

TA_PRIVATE bool ta_atom_valid(const ta_atom_table *table, ta_atom atom) {
    return atom < table->entries.count;
}

TA_PRIVATE ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom) {
    if (!ta_atom_valid(table, atom)) {
        return (ta_str){0};
    }
    const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
    return entry->text;
}

TA_PRIVATE ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out) {
    if (text.ptr == NULL && text.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_hash_bytes(0, text.ptr, text.len);
    for (uint32_t atom = table->buckets[hash & (table->bucket_count - 1u)]; atom != TA_ATOM_NONE;) {
        const ta_atom_entry *entry = ta_vec_at(&table->entries, atom);
        if (entry->hash == hash && ta_str_equal(entry->text, text)) {
            *out = atom;
            return TA_OK;
        }
        atom = entry->next;
    }
    return TA_ERR_NOT_FOUND;
}


static const ta_event_name_node *ta_event_name_node_at(const ta_event_name_table *table,
                                                       ta_event_name_id name) {
    return ta_vec_at(&table->nodes, name);
}

static uint32_t ta_event_name_hash(ta_event_name_id parent, ta_atom segment) {
    uint32_t key[2] = { parent, segment };
    return ta_hash_bytes(0x9e3779b9u, key, sizeof(key));
}

static ta_result ta_event_name_rehash(ta_event_name_table *table, uint32_t bucket_count) {
    uint32_t *buckets = ta_mem_alloc(table->allocator, bucket_count * sizeof(uint32_t),
                                     _Alignof(uint32_t));
    if (buckets == NULL) {
        return TA_ERR_NOMEM;
    }
    for (uint32_t index = 0; index < bucket_count; index += 1) {
        buckets[index] = UINT32_MAX;
    }
    /* Node 0 is the root and is never looked up as a child. */
    for (uint32_t name = 1; name < table->nodes.count; name += 1) {
        ta_event_name_node *node = ta_vec_at(&table->nodes, name);
        uint32_t bucket = ta_event_name_hash(node->parent, node->segment) & (bucket_count - 1u);
        node->hash_next = buckets[bucket];
        buckets[bucket] = name;
    }
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = buckets;
    table->bucket_count = bucket_count;
    return TA_OK;
}

TA_PRIVATE ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out) {
    if (!ta_event_name_valid(table, parent) || !ta_atom_valid(table->atoms, segment)) {
        return TA_ERR_INVALID;
    }
    uint32_t depth = ta_event_name_node_at(table, parent)->depth + 1u;
    if (depth > TA_EVENT_NAME_MAX_DEPTH) {
        return TA_ERR_INVALID;
    }
    uint32_t hash = ta_event_name_hash(parent, segment);
    for (uint32_t name = table->buckets[hash & (table->bucket_count - 1u)]; name != UINT32_MAX;) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        if (node->parent == parent && node->segment == segment) {
            *out = name;
            return TA_OK;
        }
        name = node->hash_next;
    }
    uint32_t name = table->nodes.count;
    ta_event_name_node node = {
        .parent = parent,
        .segment = segment,
        .depth = (uint16_t)depth,
        .hash_next = table->buckets[hash & (table->bucket_count - 1u)],
    };
    ta_result result = ta_vec_push(&table->nodes, &node);
    if (result != TA_OK) {
        return result;
    }
    table->buckets[hash & (table->bucket_count - 1u)] = name;
    if (table->nodes.count > table->bucket_count) {
        (void)ta_event_name_rehash(table, table->bucket_count * 2u);
    }
    *out = name;
    return TA_OK;
}

static ta_result ta_event_name_builtin(ta_event_name_table *table, const char *parent_and_segment) {
    /* "P:segment" where P is the parent's built-in index digit. */
    ta_event_name_id parent = (ta_event_name_id)(parent_and_segment[0] - '0');
    ta_str text = { .ptr = parent_and_segment + 2, .len = 0 };
    while (text.ptr[text.len] != '\0') {
        text.len += 1;
    }
    ta_atom atom = 0;
    ta_event_name_id name = 0;
    ta_result result = ta_atom_intern(table->atoms, text, &atom);
    return result == TA_OK ? ta_event_name_child(table, parent, atom, &name) : result;
}

TA_PRIVATE ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms) {
    *table = (ta_event_name_table){ .allocator = allocator, .atoms = atoms };
    ta_vec_init(&table->nodes, allocator, TA_VEC_LAYOUT(ta_event_name_node));
    ta_event_name_node root = { .parent = TA_EVENT_NAME_ROOT, .segment = TA_ATOM_NONE };
    ta_result result = ta_vec_push(&table->nodes, &root);
    if (result == TA_OK) {
        result = ta_event_name_rehash(table, 64);
    }
    /* Order matches the TA_EVENT_NAME_* constants. */
    static const char *const builtins[] = {
        "0:error", "1:execution", "1:communication", "1:platform",
        "0:done", "5:state", "5:invoke",
    };
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(builtins); index += 1) {
        result = ta_event_name_builtin(table, builtins[index]);
    }
    if (result != TA_OK) {
        ta_event_name_table_destroy(table);
    }
    return result;
}

TA_PRIVATE void ta_event_name_table_destroy(ta_event_name_table *table) {
    ta_vec_destroy(&table->nodes);
    ta_mem_free(table->allocator, table->buckets,
                &(ta_allocator_free_params){ .size = table->bucket_count * sizeof(uint32_t),
                                             .alignment = _Alignof(uint32_t) });
    table->buckets = NULL;
    table->bucket_count = 0;
}

TA_PRIVATE bool ta_event_name_valid(const ta_event_name_table *table, ta_event_name_id name) {
    return name < table->nodes.count;
}

TA_PRIVATE ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out) {
    if (segments == NULL || segments->count == 0 || segments->atoms == NULL ||
        segments->count > TA_EVENT_NAME_MAX_DEPTH) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < segments->count; index += 1) {
        if (!ta_atom_valid(table->atoms, segments->atoms[index])) {
            return TA_ERR_INVALID;
        }
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    for (uint32_t index = 0; index < segments->count; index += 1) {
        ta_result result = ta_event_name_child(table, name, segments->atoms[index], &name);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = name;
    return TA_OK;
}

TA_PRIVATE ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out) {
    if (dotted.ptr == NULL || dotted.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_event_name_id name = TA_EVENT_NAME_ROOT;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= dotted.len; index += 1) {
        if (index < dotted.len && dotted.ptr[index] != '.') {
            continue;
        }
        if (index == start) {
            return TA_ERR_INVALID;
        }
        ta_atom atom = 0;
        ta_result result = ta_atom_intern(table->atoms,
                                          (ta_str){ .ptr = dotted.ptr + start, .len = index - start },
                                          &atom);
        if (result == TA_OK) {
            result = ta_event_name_child(table, name, atom, &name);
        }
        if (result != TA_OK) {
            return result;
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

TA_PRIVATE bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix) {
    if (!ta_event_name_valid(table, event) || !ta_event_name_valid(table, prefix)) {
        return false;
    }
    uint16_t depth = ta_event_name_node_at(table, prefix)->depth;
    while (ta_event_name_node_at(table, event)->depth > depth) {
        event = ta_event_name_node_at(table, event)->parent;
    }
    return event == prefix;
}

TA_PRIVATE uint32_t ta_event_name_segments(const ta_event_name_table *table,
                                ta_event_name_id name,
                                ta_atom out[TA_EVENT_NAME_MAX_DEPTH]) {
    if (!ta_event_name_valid(table, name)) {
        return 0;
    }
    uint32_t depth = ta_event_name_node_at(table, name)->depth;
    for (uint32_t index = depth; index > 0; index -= 1) {
        const ta_event_name_node *node = ta_event_name_node_at(table, name);
        out[index - 1u] = node->segment;
        name = node->parent;
    }
    return depth;
}


TA_API ta_result ta_value_clone(const ta_value *value, ta_value *out) {
    if (value == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (value->v == NULL) {
        *out = (ta_value){0};
        return TA_OK;
    }
    if (value->v->clone == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return value->v->clone(value->self, out);
}


TA_API ta_result ta_event_clone(const ta_event *event, ta_event *out) {
    if (event == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_value data = {0};
    ta_result result = ta_value_clone(&event->data, &data);
    if (result != TA_OK) {
        return result;
    }
    *out = *event;
    out->data = data;
    return TA_OK;
}

TA_API void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
}




/*
 * SCXML 6.2 and C.1 define the target URIs of the SCXML Event I/O
 * Processor; ADR 0044 adds `#_actor_<family>/<name>` for the logical
 * address of a named actor (ADR 0037).
 *
 *   #_internal                 the sender's own internal queue
 *   #_parent                   the invoking parent session
 *   #_scxml_<session>          a session by its runtime ID
 *   #_actor_<family>/<name>    a named actor by its logical address
 *   #_<invokeid>               an invocation of the sending session
 *
 * A session URI whose text is not a session ID names no session, which is
 * inaccessible rather than invalid (SCXML C.1): it parses as runtime actor
 * 0, which never exists, so the send reports error.communication. Anything
 * else is an invalid target and the send reports error.execution.
 */

static const char TA_URI_INTERNAL[] = "#_internal";
static const char TA_URI_PARENT[] = "#_parent";
static const char TA_URI_SESSION[] = "#_scxml_";
static const char TA_URI_ADDRESS[] = "#_actor_";
static const char TA_URI_INVOKE[] = "#_";

static bool ta_uri_is(ta_str text, const char *literal) {
    return ta_str_equal(text, ta_str_from_cstr(literal));
}

/* Consumes `prefix` from `text`; false when it does not start with it. */
static bool ta_uri_prefix(ta_str *text, const char *prefix) {
    ta_str candidate = ta_str_from_cstr(prefix);
    if (text->len < candidate.len ||
        !ta_str_equal((ta_str){ .ptr = text->ptr, .len = candidate.len }, candidate)) {
        return false;
    }
    text->ptr += candidate.len;
    text->len -= candidate.len;
    return true;
}

/* "<family>/<name>": both parts are non-empty and hold no further slash. */
static bool ta_uri_address(ta_str text, ta_actor_address *out) {
    uint32_t slash = 0;
    while (slash < text.len && text.ptr[slash] != '/') {
        slash += 1;
    }
    if (slash == 0 || slash + 1u >= text.len) {
        return false;
    }
    ta_str name = { .ptr = text.ptr + slash + 1u, .len = text.len - slash - 1u };
    for (uint32_t index = 0; index < name.len; index += 1) {
        if (name.ptr[index] == '/') {
            return false;
        }
    }
    *out = (ta_actor_address){ .family = { .name = { .ptr = text.ptr, .len = slash } }, .name = name };
    return true;
}

/* Decimal digits only, at least one, no overflow. */
static bool ta_uri_number(ta_str text, uint64_t *out) {
    if (text.len == 0) {
        return false;
    }
    uint64_t value = 0;
    for (uint32_t index = 0; index < text.len; index += 1) {
        char digit = text.ptr[index];
        if (digit < '0' || digit > '9' || value > (UINT64_MAX - 9u) / 10u) {
            return false;
        }
        value = value * 10u + (uint64_t)(digit - '0');
    }
    *out = value;
    return true;
}

TA_API ta_result ta_scxml_target_parse(ta_system *system, ta_str text, ta_scxml_target *out) {
    if (system == NULL || out == NULL || (text.ptr == NULL && text.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (text.ptr == NULL) {
        text = (ta_str){ .ptr = "", .len = 0 };
    }
    if (ta_uri_is(text, TA_URI_INTERNAL)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INTERNAL };
        return TA_OK;
    }
    if (ta_uri_is(text, TA_URI_PARENT)) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_PARENT };
        return TA_OK;
    }
    ta_str rest = text;
    if (ta_uri_prefix(&rest, TA_URI_ADDRESS)) {
        ta_actor_address address;
        if (!ta_uri_address(rest, &address)) {
            return TA_ERR_INVALID;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = address } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_SESSION)) {
        uint64_t session = 0;
        /* A session URI naming no session is inaccessible, not invalid. */
        if (!ta_uri_number(rest, &session)) {
            session = 0;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = session } };
        return TA_OK;
    }
    rest = text;
    if (ta_uri_prefix(&rest, TA_URI_INVOKE) && rest.len != 0) {
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_INVOKE };
        return ta_invoke_id_from_text(system, rest, &out->as.invoke);
    }
    return TA_ERR_INVALID;
}

TA_PRIVATE bool ta_scxml_uri_write(const ta_system *system, ta_text_writer *writer, const ta_scxml_target *target) {
    switch (target->kind) {
        case TA_SCXML_TARGET_INTERNAL:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INTERNAL));
            return true;
        case TA_SCXML_TARGET_PARENT:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_PARENT));
            return true;
        case TA_SCXML_TARGET_INVOKE:
            ta_text_put(writer, ta_str_from_cstr(TA_URI_INVOKE));
            ta_text_put(writer, ta_invoke_id_text(system, target->as.invoke));
            return true;
        case TA_SCXML_TARGET_ACTOR:
            if (target->as.actor.kind == TA_ACTOR_REF_NAMED) {
                ta_text_put(writer, ta_str_from_cstr(TA_URI_ADDRESS));
                ta_text_put(writer, target->as.actor.as.named.family.name);
                ta_text_put(writer, (ta_str){ .ptr = "/", .len = 1 });
                ta_text_put(writer, target->as.actor.as.named.name);
                return true;
            }
            ta_text_put(writer, ta_str_from_cstr(TA_URI_SESSION));
            ta_text_put_u64(writer, target->as.actor.as.runtime);
            return true;
        default:
            /* SELF is the absent target, which has no URI of its own. */
            return false;
    }
}

TA_API ta_result ta_scxml_target_format(const ta_system *system, const ta_scxml_target_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    if (!ta_scxml_uri_write(system, &writer, &params->target)) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    return ta_text_finish(&writer, params->needed);
}


TA_PRIVATE void ta_address_release(ta_actor_ref *ref, ta_allocator allocator) {
    if (ref->kind != TA_ACTOR_REF_NAMED) {
        return;
    }
    ta_str *texts[2] = { &ref->as.named.family.name, &ref->as.named.name };
    for (uint32_t index = 0; index < 2; index += 1) {
        if (texts[index]->ptr != NULL) {
            ta_mem_free(allocator, (void *)texts[index]->ptr,
                        &(ta_allocator_free_params){ .size = (size_t)texts[index]->len + 1u,
                                                     .alignment = 1 });
        }
        *texts[index] = (ta_str){0};
    }
}

static ta_result ta_address_copy_text(ta_str *text, ta_allocator allocator) {
    if (text->ptr == NULL) {
        return TA_OK;
    }
    char *copy = ta_mem_alloc(allocator, (size_t)text->len + 1u, 1);
    if (copy == NULL) {
        *text = (ta_str){0};
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, text->ptr, text->len);
    copy[text->len] = '\0';
    text->ptr = copy;
    return TA_OK;
}

TA_PRIVATE ta_result ta_address_copy(ta_actor_ref *ref, ta_allocator allocator) {
    if (ref->kind != TA_ACTOR_REF_NAMED) {
        return TA_OK;
    }
    ta_result result = ta_address_copy_text(&ref->as.named.family.name, allocator);
    if (result != TA_OK) {
        /* The family copy left its own text null; the name is still borrowed. */
        ref->as.named.name = (ta_str){0};
        return result;
    }
    result = ta_address_copy_text(&ref->as.named.name, allocator);
    if (result != TA_OK) {
        ta_address_release(ref, allocator);
    }
    return result;
}

TA_API ta_result ta_external_delivery_clone(const ta_external_delivery *delivery,
                                     ta_allocator allocator,
                                     ta_external_delivery *out) {
    if (delivery == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_external_delivery copy = *delivery;
    ta_result result = ta_address_copy(&copy.destination, allocator);
    if (result == TA_OK) {
        result = ta_event_clone(&delivery->event, &copy.event);
    }
    if (result != TA_OK) {
        ta_address_release(&copy.destination, allocator);
        return result;
    }
    *out = copy;
    return TA_OK;
}

TA_API void ta_external_delivery_release(ta_external_delivery *delivery, ta_allocator allocator) {
    if (delivery == NULL) {
        return;
    }
    ta_address_release(&delivery->destination, allocator);
    ta_event_release(&delivery->event);
}



TA_PRIVATE bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state) {
    if (atomic_state >= config->layout->atomic_state_count) {
        return false;
    }
    return ta_bits_get(config->storage, atomic_state);
}

TA_PRIVATE void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active) {
    if (atomic_state < config->layout->atomic_state_count) {
        ta_bits_put(config->storage, atomic_state, active);
    }
}

TA_PRIVATE void ta_configuration_clear(ta_configuration *config) {
    ta_mem_zero(config->storage, config->layout->bytes);
}


TA_PRIVATE uint64_t *ta_history_slot_words(const ta_history_state *history, uint32_t slot) {
    return (uint64_t *)((unsigned char *)history->storage + history->layout->slots[slot].offset);
}



static bool ta_timer_before(const ta_delayed_send *left, const ta_delayed_send *right) {
    return left->due < right->due || (left->due == right->due && left->id < right->id);
}

static void ta_timer_swap(ta_timer_heap *heap, uint32_t left, uint32_t right) {
    ta_delayed_send swap = heap->items[left];
    heap->items[left] = heap->items[right];
    heap->items[right] = swap;
    heap->items[left].heap_index = left;
    heap->items[right].heap_index = right;
}

static void ta_timer_sift_up(ta_timer_heap *heap, uint32_t index) {
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (!ta_timer_before(&heap->items[index], &heap->items[parent])) {
            return;
        }
        ta_timer_swap(heap, index, parent);
        index = parent;
    }
}

static void ta_timer_sift_down(ta_timer_heap *heap, uint32_t index) {
    for (;;) {
        uint32_t smallest = index;
        uint32_t left = index * 2u + 1u;
        uint32_t right = left + 1u;
        if (left < heap->count && ta_timer_before(&heap->items[left], &heap->items[smallest])) {
            smallest = left;
        }
        if (right < heap->count && ta_timer_before(&heap->items[right], &heap->items[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            return;
        }
        ta_timer_swap(heap, index, smallest);
        index = smallest;
    }
}

static void ta_timer_remove_at(ta_timer_heap *heap, uint32_t index) {
    heap->count -= 1u;
    if (index == heap->count) {
        return;
    }
    heap->items[index] = heap->items[heap->count];
    heap->items[index].heap_index = index;
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, index);
}

static void ta_timer_release(ta_delayed_send *send) {
    ta_value_release(&send->target);
    ta_value_release(&send->data);
}

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator) {
    *heap = (ta_timer_heap){ .allocator = allocator, .next_id = 1 };
    return TA_OK;
}

TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap) {
    for (uint32_t index = 0; index < heap->count; index += 1) {
        ta_timer_release(&heap->items[index]);
    }
    ta_mem_free(heap->allocator, heap->items,
                &(ta_allocator_free_params){ .size = heap->capacity * sizeof(ta_delayed_send),
                                             .alignment = _Alignof(ta_delayed_send) });
    *heap = (ta_timer_heap){0};
}

TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send) {
    if (heap->count == heap->capacity) {
        uint32_t capacity = heap->capacity == 0 ? 16u : heap->capacity * 2u;
        ta_delayed_send *items = ta_mem_alloc(heap->allocator, capacity * sizeof(ta_delayed_send),
                                              _Alignof(ta_delayed_send));
        if (items == NULL) {
            return TA_ERR_NOMEM;
        }
        if (heap->items != NULL) {
            ta_mem_copy(items, heap->items, heap->count * sizeof(ta_delayed_send));
            ta_mem_free(heap->allocator, heap->items,
                        &(ta_allocator_free_params){ .size = heap->capacity * sizeof(ta_delayed_send),
                                                     .alignment = _Alignof(ta_delayed_send) });
        }
        heap->items = items;
        heap->capacity = capacity;
    }
    uint32_t index = heap->count;
    heap->items[index] = *send;
    if (heap->items[index].id == 0) {
        heap->items[index].id = heap->next_id;
        heap->next_id += 1u;
    }
    heap->items[index].heap_index = index;
    heap->items[index].active = true;
    heap->count += 1u;
    ta_timer_sift_up(heap, index);
    return TA_OK;
}

typedef struct {
    ta_actor_id owner;
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
} ta_timer_selection;

static bool ta_timer_remove_selected(ta_timer_heap *heap, const ta_timer_selection *selection) {
    bool found = false;
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == selection->owner && (selection->any_send_id || send->send_id == selection->send_id) &&
            (selection->include_submitted || !send->submitted)) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            found = true;
            index = heap->count + 1u;
        }
    }
    return found;
}

TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap,
                            ta_actor_id owner,
                            ta_send_id send_id) {
    return ta_timer_remove_selected(heap, &(ta_timer_selection){ .owner = owner, .send_id = send_id,
                                                                 .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_actor_id owner) {
    (void)ta_timer_remove_selected(heap, &(ta_timer_selection){ .owner = owner, .any_send_id = true });
}

TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id) {
    bool found = false;
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == owner && send->send_id == send_id && !send->submitted) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            found = true;
            /* Removal reorders the tail; restart the scan conservatively. */
            index = heap->count + 1u;
        }
    }
    return found ? TA_OK : TA_ERR_NOT_FOUND;
}

TA_PRIVATE void ta_timer_cancel_owner(ta_timer_heap *heap, ta_actor_id owner) {
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == owner) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            index = heap->count + 1u;
        }
    }
}

TA_PRIVATE uint32_t ta_timer_count_owner(const ta_timer_heap *heap, ta_actor_id owner) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < heap->count; index += 1) {
        count += heap->items[index].owner == owner ? 1u : 0u;
    }
    return count;
}

TA_PRIVATE const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap) {
    return heap->count == 0 ? NULL : &heap->items[0];
}

TA_PRIVATE ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out) {
    if (heap->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = heap->items[0];
    ta_timer_remove_at(heap, 0);
    return TA_OK;
}


TA_API bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return false;
    }
    return ta_actor_state_active(host->impl->actor, state);
}

TA_API const ta_event *ta_dm_host_event(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL ||
        !host->impl->actor->has_current_event) {
        return NULL;
    }
    return &host->impl->actor->current_event;
}

TA_API ta_actor_id ta_dm_host_actor(const ta_dm_host *host) {
    if (host == NULL || host->impl == NULL || host->impl->actor == NULL) {
        return 0;
    }
    return host->impl->actor->record.id;
}

static bool ta_dm_host_valid(const ta_dm_host *host) {
    return host != NULL && host->impl != NULL && host->impl->actor != NULL && host->impl->system != NULL;
}

/*
 * The URI of an actor reference, written with the grammar core parses
 * (ADR 0044): a session URI for a runtime actor, a logical address for a
 * named one. A chart can send to whatever it reads here.
 */
static bool ta_dm_write_actor_uri(const ta_system *system, ta_text_writer *writer, ta_actor_ref actor) {
    ta_scxml_target target = { .kind = TA_SCXML_TARGET_ACTOR, .as.actor = actor };
    return ta_scxml_uri_write(system, writer, &target);
}

/* Writes one event field; false when the field has no value. */
static bool ta_dm_write_event_field(const ta_dm_host *host, ta_dm_text_kind kind, ta_text_writer *writer) {
    const ta_system *system = host->impl->system;
    const ta_event *event = ta_dm_host_event(host);
    if (event == NULL) {
        return false;
    }
    switch (kind) {
        case TA_DM_TEXT_EVENT_NAME: {
            ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
            uint32_t count = ta_event_name_segments(&system->event_names, event->name, segments);
            for (uint32_t index = 0; index < count; index += 1) {
                if (index != 0) {
                    ta_text_put(writer, (ta_str){ .ptr = ".", .len = 1 });
                }
                ta_text_put(writer, ta_atom_text(&system->atoms, segments[index]));
            }
            return count != 0;
        }
        case TA_DM_TEXT_EVENT_TYPE: {
            static const char *const types[] = { "external", "internal", "platform" };
            if ((uint32_t)event->type >= TA_ARRAY_COUNT(types)) {
                return false;
            }
            ta_text_put(writer, ta_str_from_cstr(types[event->type]));
            return true;
        }
        case TA_DM_TEXT_EVENT_SENDID:
            if (event->send_id == 0) {
                return false;
            }
            ta_send_id_write(system, event->send_id, writer);
            return true;
        case TA_DM_TEXT_EVENT_ORIGIN:
            if (event->origin.kind == TA_ORIGIN_ACTOR) {
                return ta_dm_write_actor_uri(system, writer, event->origin.as.actor);
            }
            if (event->origin.kind == TA_ORIGIN_INVOKE) {
                ta_scxml_target target = { .kind = TA_SCXML_TARGET_INVOKE, .as.invoke = event->origin.as.invoke };
                return ta_scxml_uri_write(system, writer, &target);
            }
            return false;
        case TA_DM_TEXT_EVENT_ORIGINTYPE:
            if (event->origin.kind != TA_ORIGIN_ACTOR && event->origin.kind != TA_ORIGIN_INVOKE) {
                return false;
            }
            ta_text_put(writer, ta_atom_text(&system->atoms, system->scxml_io_type));
            return true;
        case TA_DM_TEXT_EVENT_INVOKEID:
            if (event->invoke_id == TA_INVOKE_ID_NONE) {
                return false;
            }
            /* The SCXML ID text, e.g. the author's ID or `<stateid>.<platformid>`. */
            ta_text_put(writer, ta_invoke_id_text(system, event->invoke_id));
            return true;
        default:
            return false;
    }
}

TA_API ta_result ta_dm_host_text(const ta_dm_host *host, const ta_dm_text_params *params) {
    if (!ta_dm_host_valid(host) || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_system *system = host->impl->system;
    const ta_actor *actor = host->impl->actor;
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    bool present = true;
    switch (params->kind) {
        case TA_DM_TEXT_SESSION_ID:
            ta_text_put_u64(&writer, actor->record.id);
            break;
        case TA_DM_TEXT_SESSION_NAME:
            present = actor->definition->document_name != TA_ATOM_NONE;
            if (present) {
                ta_text_put(&writer, ta_atom_text(&system->atoms, actor->definition->document_name));
            }
            break;
        case TA_DM_TEXT_SCXML_IO_LOCATION:
            present = ta_dm_write_actor_uri(system, &writer, actor->record.self);
            break;
        case TA_DM_TEXT_SCXML_IO_TYPE:
            ta_text_put(&writer, ta_atom_text(&system->atoms, system->scxml_io_type));
            break;
        default:
            present = ta_dm_write_event_field(host, params->kind, &writer);
            break;
    }
    if (!present) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    return ta_text_finish(&writer, params->needed);
}

TA_API ta_result ta_dm_host_state_name(const ta_dm_host *host, ta_state_id state, ta_str *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = host->impl->actor->definition;
    if (state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&host->impl->system->atoms, definition->states[state].name);
    return TA_OK;
}

TA_API ta_result ta_dm_host_find_state(const ta_dm_host *host, ta_str id, ta_state_id *out) {
    if (!ta_dm_host_valid(host) || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_system *system = host->impl->system;
    const ta_definition *definition = host->impl->actor->definition;
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}



/*
 * SCXML <invoke>: starting services at the end of a macrostep, cancelling
 * them when their state exits, <finalize>, autoforwarding, done.invoke, and
 * the host's asynchronous reports.
 *
 * Invoke IDs are SCXML text (the author's `id` or a generated
 * `<stateid>.<platformid>`). The runtime ta_invoke_id is the interned text's
 * atom + 1, so the text is always recoverable and `#_<invokeid>` targets
 * resolve by interning.
 */

/* ------------------------------------------------------------------------
 * IDs
 */

TA_PRIVATE ta_result ta_invoke_id_from_text(ta_system *system, ta_str text, ta_invoke_id *out) {
    ta_atom atom = 0;
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_atom_intern(&system->atoms, text, &atom);
    if (result == TA_OK) {
        *out = (ta_invoke_id)atom + 1u;
    }
    return result;
}

TA_PRIVATE ta_str ta_invoke_id_text(const ta_system *system, ta_invoke_id invoke) {
    if (invoke == 0 || invoke == TA_INVOKE_ID_NONE || invoke - 1u >= TA_ATOM_NONE) {
        return (ta_str){0};
    }
    return ta_atom_text(&system->atoms, (ta_atom)(invoke - 1u));
}

TA_API ta_result ta_system_invoke_id_text(const ta_system *system, ta_invoke_id invoke, ta_str *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (invoke == 0 || invoke == TA_INVOKE_ID_NONE || invoke - 1u >= TA_ATOM_NONE ||
        !ta_atom_valid(&system->atoms, (ta_atom)(invoke - 1u))) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_invoke_id_text(system, invoke);
    return TA_OK;
}

/* done.invoke.<id>, one event-name segment per dotted part of the ID text. */
static ta_result ta_invoke_done_name(ta_system *system, ta_invoke_id invoke, ta_event_name_id *out) {
    ta_str text = ta_invoke_id_text(system, invoke);
    ta_event_name_id name = TA_EVENT_NAME_DONE_INVOKE;
    uint32_t start = 0;
    for (uint32_t index = 0; text.ptr != NULL && index <= text.len; index += 1) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index > start) {
            ta_atom atom = 0;
            ta_result result = ta_atom_intern(&system->atoms,
                                              (ta_str){ .ptr = text.ptr + start, .len = index - start }, &atom);
            if (result == TA_OK) {
                result = ta_event_name_child(&system->event_names, name, atom, &name);
            }
            if (result != TA_OK) {
                return result;
            }
        }
        start = index + 1u;
    }
    *out = name;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Records
 */

static int64_t ta_invoke_find(const ta_actor *actor, ta_invoke_id invoke) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].id == invoke) {
            return (int64_t)index;
        }
    }
    return -1;
}

static ta_result ta_invoke_grow(ta_system *system, ta_actor_sparse_state *sparse) {
    if (sparse->invoke_count < sparse->invoke_capacity) {
        return TA_OK;
    }
    uint32_t capacity = sparse->invoke_capacity == 0 ? 4u : sparse->invoke_capacity * 2u;
    ta_active_invoke *invokes = ta_mem_alloc(system->allocator, capacity * sizeof(ta_active_invoke),
                                             _Alignof(ta_active_invoke));
    ta_invoke_meta *meta = ta_mem_alloc(system->allocator, capacity * sizeof(ta_invoke_meta),
                                        _Alignof(ta_invoke_meta));
    if (invokes == NULL || meta == NULL) {
        ta_mem_free(system->allocator, invokes,
                    &(ta_allocator_free_params){ .size = capacity * sizeof(ta_active_invoke),
                                                 .alignment = _Alignof(ta_active_invoke) });
        ta_mem_free(system->allocator, meta,
                    &(ta_allocator_free_params){ .size = capacity * sizeof(ta_invoke_meta),
                                                 .alignment = _Alignof(ta_invoke_meta) });
        return TA_ERR_NOMEM;
    }
    if (sparse->invoke_count != 0) {
        ta_mem_copy(invokes, sparse->invokes, sparse->invoke_count * sizeof(ta_active_invoke));
        ta_mem_copy(meta, sparse->invoke_meta, sparse->invoke_count * sizeof(ta_invoke_meta));
    }
    ta_mem_free(system->allocator, sparse->invokes,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_active_invoke),
                                             .alignment = _Alignof(ta_active_invoke) });
    ta_mem_free(system->allocator, sparse->invoke_meta,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_invoke_meta),
                                             .alignment = _Alignof(ta_invoke_meta) });
    sparse->invokes = invokes;
    sparse->invoke_meta = meta;
    sparse->invoke_capacity = capacity;
    return TA_OK;
}

static void ta_invoke_remove_at(ta_actor_sparse_state *sparse, uint32_t index) {
    for (uint32_t at = index + 1u; at < sparse->invoke_count; at += 1) {
        sparse->invokes[at - 1u] = sparse->invokes[at];
        sparse->invoke_meta[at - 1u] = sparse->invoke_meta[at];
    }
    sparse->invoke_count -= 1u;
}

/* Cancels the service and forgets the invocation. */
static void ta_invoke_cancel_at(ta_actor_sparse_state *sparse, uint32_t index) {
    ta_active_invoke active = sparse->invokes[index];
    ta_invoke_meta meta = sparse->invoke_meta[index];
    /* Remove first: the service may destroy a child that reports back reentrantly. */
    ta_invoke_remove_at(sparse, index);
    if (meta.invoker.v != NULL && meta.invoker.v->cancel != NULL) {
        ta_fault fault = {0};
        (void)meta.invoker.v->cancel(meta.invoker.self, active.service_handle, &fault);
        ta_value_release(&fault.data);
    }
}

static ta_vec *ta_invoke_pending(ta_system *system, ta_actor_sparse_state *sparse) {
    if (sparse->pending_invokes.element_size == 0) {
        ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
    }
    return &sparse->pending_invokes;
}

static const ta_invoker *ta_invoke_find_invoker(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        const ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            return &entry->invoker;
        }
    }
    return NULL;
}

static ta_atom ta_invoke_normalize_type(const ta_system *system, ta_atom type) {
    static const char *const aliases[] = { "scxml", "http://www.w3.org/TR/scxml", "http://www.w3.org/TR/scxml/" };
    ta_str text = ta_atom_text(&system->atoms, type);
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(aliases); index += 1) {
        ta_str alias = { .ptr = aliases[index], .len = 0 };
        while (alias.ptr[alias.len] != '\0') {
            alias.len += 1;
        }
        if (ta_str_equal(text, alias)) {
            return system->scxml_invoke_type;
        }
    }
    return type;
}

/* ------------------------------------------------------------------------
 * Lifecycle hooks
 */

TA_PRIVATE ta_result ta_invoke_state_entered(ta_interpreter *it, ta_state_id state) {
    const ta_definition *definition = it->definition;
    bool has_invokes = false;
    for (uint32_t index = 0; index < definition->invoke_decl_count && !has_invokes; index += 1) {
        has_invokes = definition->invokes[index].owner == state;
    }
    if (!has_invokes) {
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(it->system, it->actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_vec *pending = ta_invoke_pending(it->system, sparse);
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            return TA_OK;
        }
    }
    return ta_vec_push(pending, &state);
}

TA_PRIVATE ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    ta_vec *pending = &sparse->pending_invokes;
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            *(ta_state_id *)ta_vec_at(pending, index) = *(ta_state_id *)ta_vec_at(pending, pending->count - 1u);
            pending->count -= 1u;
            break;
        }
    }
    for (uint32_t index = sparse->invoke_count; index > 0; index -= 1u) {
        if (sparse->invoke_meta[index - 1u].state == state) {
            ta_invoke_cancel_at(sparse, index - 1u);
        }
    }
    return TA_OK;
}

typedef struct {
    ta_value src;
    ta_value data;
    ta_value params;
    ta_fault fault;
} ta_invoke_values;

static void ta_invoke_values_release(ta_invoke_values *values) {
    ta_value_release(&values->src);
    ta_value_release(&values->data);
    ta_value_release(&values->params);
    ta_value_release(&values->fault.data);
}

static ta_op_status ta_invoke_text_value(ta_interpreter *it, ta_datamodel_text_value_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->text_value == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->text_value(definition->datamodel.self, params);
}

static ta_op_status ta_invoke_value_text(ta_interpreter *it, ta_datamodel_value_text_params *params) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    params->shared = definition->datamodel_shared;
    params->instance = it->actor->datamodel_instance;
    params->host = &it->actor->host;
    return definition->datamodel.v->value_text(definition->datamodel.self, params);
}

/* The invoke's ID: the author's, or `<stateid>.<platformid>`, also stored through idlocation. */
static ta_op_status ta_invoke_resolve_id(ta_interpreter *it, const ta_invoke_decl *decl,
                                         ta_invoke_values *values, ta_invoke_id *out) {
    ta_system *system = it->system;
    if (decl->id.kind == TA_ID_SOURCE_LITERAL) {
        *out = (ta_invoke_id)decl->id.as.literal + 1u;
        return TA_OP_OK;
    }
    ta_str state = ta_atom_text(&system->atoms, it->definition->states[decl->owner].name);
    system->next_invoke_serial += 1u;
    char digits[20];
    uint32_t digit_count = 0;
    for (uint64_t serial = system->next_invoke_serial; serial != 0 || digit_count == 0; serial /= 10u) {
        digits[digit_count++] = (char)('0' + serial % 10u);
    }
    uint32_t length = state.len + 1u + digit_count;
    char *text = ta_arena_alloc(&system->scratch, length, 1);
    if (text == NULL) {
        return TA_OP_ERROR;
    }
    ta_mem_copy(text, state.ptr, state.len);
    text[state.len] = '.';
    for (uint32_t index = 0; index < digit_count; index += 1) {
        text[state.len + 1u + index] = digits[digit_count - 1u - index];
    }
    ta_str id = { .ptr = text, .len = length };
    if (ta_invoke_id_from_text(system, id, out) != TA_OK) {
        return TA_OP_ERROR;
    }
    if (decl->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OP_OK;
    }
    const ta_definition *definition = it->definition;
    ta_value value = {0};
    ta_op_status status = ta_invoke_text_value(it, &(ta_datamodel_text_value_params){
                                                       .text = id, .out = &value, .fault = &values->fault });
    if (status == TA_OP_OK) {
        status = definition->datamodel.v->assign == NULL
                     ? TA_OP_ERROR
                     : definition->datamodel.v->assign(definition->datamodel.self,
                                                       &(ta_datamodel_assign_params){
                                                           .shared = definition->datamodel_shared,
                                                           .instance = it->actor->datamodel_instance,
                                                           .location = decl->id.as.location,
                                                           .value = &value,
                                                           .host = &it->actor->host,
                                                           .fault = &values->fault,
                                                       });
        ta_value_release(&value);
    }
    return status;
}

static ta_op_status ta_invoke_resolve_type(ta_interpreter *it, const ta_invoke_decl *decl,
                                           ta_invoke_values *values, ta_atom *out) {
    ta_system *system = it->system;
    if (decl->type.kind == TA_TEXT_SOURCE_NONE) {
        *out = system->scxml_invoke_type;
        return TA_OP_OK;
    }
    if (decl->type.kind == TA_TEXT_SOURCE_ATOM) {
        *out = ta_invoke_normalize_type(system, decl->type.as.atom);
        return TA_OP_OK;
    }
    ta_value value = {0};
    ta_str text = {0};
    ta_op_status status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                                      .source = { .kind = TA_VALUE_SOURCE_EXPR,
                                                                  .as.expr = decl->type.as.expr },
                                                      .out = &value,
                                                      .fault = &values->fault,
                                                  });
    if (status == TA_OP_OK) {
        status = ta_invoke_value_text(it, &(ta_datamodel_value_text_params){
                                              .value = &value, .out = &text, .fault = &values->fault });
    }
    if (status == TA_OP_OK) {
        ta_atom atom = 0;
        status = ta_atom_intern(&system->atoms, text, &atom) == TA_OK ? TA_OP_OK : TA_OP_ERROR;
        *out = ta_invoke_normalize_type(system, atom);
    }
    ta_value_release(&value);
    return status;
}

/* src (a literal becomes a data-model value), inline content, and params. */
static ta_op_status ta_invoke_resolve_values(ta_interpreter *it, const ta_invoke_decl *decl,
                                             ta_invoke_values *values) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = TA_OP_OK;
    if (decl->src.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes bytes = definition->contents[decl->src.as.content].bytes;
        status = ta_invoke_text_value(it, &(ta_datamodel_text_value_params){
                                              .text = { .ptr = (const char *)bytes.ptr, .len = bytes.len },
                                              .out = &values->src,
                                              .fault = &values->fault,
                                          });
    } else {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->src, .out = &values->src, .fault = &values->fault });
    }
    if (status == TA_OP_OK && decl->data.kind == TA_VALUE_SOURCE_CONTENT) {
        status = v->import_content == NULL
                     ? TA_OP_ERROR
                     : v->import_content(definition->datamodel.self,
                                         &(ta_datamodel_import_content_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .content = definition->contents[decl->data.as.content].bytes,
                                             .host = &it->actor->host,
                                             .out = &values->data,
                                             .fault = &values->fault,
                                         });
    } else if (status == TA_OP_OK) {
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = decl->data, .out = &values->data, .fault = &values->fault });
    }
    if (status != TA_OP_OK || decl->params.length == 0) {
        return status;
    }
    if (v->make_params == NULL) {
        return TA_OP_ERROR;
    }
    ta_named_value *named = ta_arena_alloc(&it->system->scratch, decl->params.length * sizeof(ta_named_value),
                                           _Alignof(ta_named_value));
    if (named == NULL) {
        return TA_OP_ERROR;
    }
    uint32_t evaluated = 0;
    for (; evaluated < decl->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[decl->params.offset + evaluated];
        named[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_interpreter_eval(it, &(ta_interpreter_value_params){
                                             .source = param->value, .out = &named[evaluated].value,
                                             .fault = &values->fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = named,
                                    .count = decl->params.length,
                                    .host = &it->actor->host,
                                    .out = &values->params,
                                    .fault = &values->fault,
                                });
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&named[index].value);
    }
    return status;
}

static ta_result ta_invoke_start(ta_interpreter *it, ta_invoke_decl_id decl_id) {
    ta_system *system = it->system;
    const ta_invoke_decl *decl = &it->definition->invokes[decl_id];
    ta_invoke_values values = {0};
    ta_invoke_id id = 0;
    ta_atom type = 0;
    ta_op_status status = ta_invoke_resolve_id(it, decl, &values, &id);
    if (status == TA_OP_OK) {
        status = ta_invoke_resolve_type(it, decl, &values, &type);
    }
    const ta_invoker *invoker = status == TA_OP_OK ? ta_invoke_find_invoker(system, type) : NULL;
    if (invoker == NULL || invoker->v == NULL || invoker->v->start == NULL) {
        status = TA_OP_ERROR;
    }
    if (status == TA_OP_OK) {
        status = ta_invoke_resolve_values(it, decl, &values);
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, it->actor);
    if (status == TA_OP_OK && (sparse == NULL || ta_invoke_grow(system, sparse) != TA_OK)) {
        ta_invoke_values_release(&values);
        return TA_ERR_NOMEM;
    }
    if (status != TA_OP_OK) {
        ta_result result = ta_interpreter_execution_error(it, &values.fault);
        ta_invoke_values_release(&values);
        return result;
    }
    /* Record first, so a child spawned during start() can attach to it. */
    uint32_t index = sparse->invoke_count;
    sparse->invokes[index] = (ta_active_invoke){
        .id = id, .type = type, .autoforward = decl->autoforward, .active = true,
    };
    sparse->invoke_meta[index] = (ta_invoke_meta){
        .decl = decl_id, .state = decl->owner, .invoker = *invoker, .child = 0,
    };
    sparse->invoke_count += 1u;
    ta_invoke_request request = {
        .system = system,
        .parent = it->actor->record.id,
        .invoke_id = id,
        .type = type,
        .src = values.src,
        .data = values.data,
        .params = values.params,
        .autoforward = decl->autoforward,
    };
    uint64_t service = 0;
    status = invoker->v->start(invoker->self, &(ta_invoker_start_params){
                                                  .request = &request, .service_handle = &service,
                                                  .fault = &values.fault });
    int64_t found = ta_invoke_find(it->actor, id);
    ta_result result = TA_OK;
    if (status == TA_OP_OK && found >= 0) {
        sparse->invokes[found].service_handle = service;
    } else if (status != TA_OP_OK) {
        if (found >= 0) {
            ta_invoke_remove_at(sparse, (uint32_t)found);
        }
        result = ta_interpreter_execution_error(it, &values.fault);
    }
    ta_invoke_values_release(&values);
    return result;
}

TA_PRIVATE ta_result ta_invoke_start_pending(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->pending_invokes.count == 0) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    ta_vec *pending = &sparse->pending_invokes;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        bool listed = false;
        for (uint32_t index = 0; index < pending->count && !listed; index += 1) {
            listed = *(ta_state_id *)ta_vec_at(pending, index) == state;
        }
        if (!listed || !ta_actor_state_active(it->actor, state)) {
            continue;
        }
        for (uint32_t decl = 0; decl < definition->invoke_decl_count; decl += 1) {
            if (definition->invokes[decl].owner != state) {
                continue;
            }
            ta_result result = ta_invoke_start(it, decl);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    pending->count = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Communication
 */

typedef struct {
    ta_actor *sender;
    ta_actor_ref destination;
    ta_event event;
} ta_invoke_delivery;

/*
 * Offers an event to another session through the router. When the router
 * accepts nothing, the event waits in the deadline index, due now, owned by
 * the sender, so ta_system_dispatch_due() offers it again.
 */
static ta_result ta_invoke_submit(ta_system *system, const ta_invoke_delivery *delivery) {
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    ta_fault fault = {0};
    if (system->external_events.v != NULL) {
        ta_external_delivery external = { .destination = delivery->destination, .event = delivery->event };
        status = system->external_events.v->submit(system->external_events.self,
                                                   &(ta_external_event_submit_params){
                                                       .delivery = &external, .fault = &fault });
    }
    ta_value_release(&fault.data);
    if (status != TA_EXTERNAL_EVENT_RETRY) {
        return TA_OK;
    }
    ta_delayed_send record = {
        .owner = delivery->sender->record.id,
        .send_id = delivery->event.send_id,
        .due = ta_clock_now(&system->clock),
        .event = delivery->event.name,
        .io_type = system->scxml_io_type,
        .destination = delivery->destination,
        .submitted = true,
        .origin = delivery->event.origin.kind == TA_ORIGIN_ACTOR ? delivery->event.origin.as.actor
                                                                  : delivery->sender->record.self,
        .invoke_id = delivery->event.invoke_id,
    };
    ta_result result = ta_value_clone(&delivery->event.data, &record.data);
    if (result == TA_OK) {
        result = ta_timer_schedule(&system->timers, &record);
        if (result != TA_OK) {
            ta_value_release(&record.data);
        }
    }
    return result;
}

TA_PRIVATE bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out) {
    int64_t index = ta_invoke_find(actor, invoke);
    if (index < 0 || !actor->sparse->invoke_meta[index].has_child) {
        return false;
    }
    *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = actor->sparse->invoke_meta[index].child };
    return true;
}

TA_PRIVATE ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination) {
    if (sender == NULL || sender->sparse == NULL || !sender->sparse->has_parent ||
        destination->kind != TA_ACTOR_REF_RUNTIME || destination->as.runtime != sender->sparse->parent.actor) {
        return TA_INVOKE_ID_NONE;
    }
    return sender->sparse->parent.invoke_id;
}

TA_PRIVATE void ta_invoke_attach_child(ta_system *system, const ta_actor_parent *parent, ta_actor_id child) {
    ta_actor *actor = ta_actor_lookup(system, parent->actor);
    int64_t index = actor == NULL ? -1 : ta_invoke_find(actor, parent->invoke_id);
    if (index >= 0) {
        actor->sparse->invoke_meta[index].child = child;
        actor->sparse->invoke_meta[index].has_child = true;
    }
}

static ta_result ta_invoke_forward(ta_interpreter *it, uint32_t index, const ta_event *event) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_invoke_meta meta = sparse->invoke_meta[index];
    if (meta.has_child) {
        /* The forwarded event keeps its envelope; it is not an event of the child's own invocation. */
        ta_event copy = *event;
        copy.invoke_id = TA_INVOKE_ID_NONE;
        return ta_invoke_submit(it->system, &(ta_invoke_delivery){
                                                .sender = it->actor,
                                                .destination = { .kind = TA_ACTOR_REF_RUNTIME,
                                                                 .as.runtime = meta.child },
                                                .event = copy,
                                            });
    }
    if (meta.invoker.v != NULL && meta.invoker.v->forward != NULL) {
        ta_fault fault = {0};
        (void)meta.invoker.v->forward(meta.invoker.self, &(ta_invoker_forward_params){
                                                              .service_handle = sparse->invokes[index].service_handle,
                                                              .event = event,
                                                              .fault = &fault,
                                                          });
        ta_value_release(&fault.data);
    }
    return TA_OK;
}

TA_PRIVATE ta_result ta_invoke_before_external(ta_interpreter *it) {
    const ta_event *event = &it->actor->current_event;
    if (event->invoke_id != TA_INVOKE_ID_NONE) {
        /*
         * An event of an invocation cancelled since it was queued is still
         * processed, without <finalize>: cancelling destroys the service, so
         * nothing it sends afterwards arrives, and stale host reports are
         * refused by ta_actor_report_invoke().
         */
        int64_t index = ta_invoke_find(it->actor, event->invoke_id);
        if (index < 0) {
            return TA_OK;
        }
        ta_span32 finalize = it->definition->invokes[it->actor->sparse->invoke_meta[index].decl].finalize_actions;
        ta_result result = ta_interpreter_run_actions(it, finalize);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_actor_sparse_state *sparse = it->actor->sparse;
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1) {
        if (sparse->invokes[index].autoforward) {
            ta_result result = ta_invoke_forward(it, index, event);
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* Sends done.invoke.<id> with the completion data to the invoking parent. */
static ta_result ta_invoke_publish(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_done_name(system, sparse->parent.invoke_id, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = {
        .name = name,
        .type = TA_EVENT_EXTERNAL,
        .send_id = TA_SEND_ID_COMPLETION,
        .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
        .invoke_id = sparse->parent.invoke_id,
        .data = sparse->completion.has_data ? sparse->completion.data : (ta_value){0},
    };
    return ta_invoke_submit(system, &(ta_invoke_delivery){
                                        .sender = actor,
                                        .destination = { .kind = TA_ACTOR_REF_RUNTIME,
                                                         .as.runtime = sparse->parent.actor },
                                        .event = event,
                                    });
}

TA_PRIVATE ta_result ta_invoke_finished(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    while (sparse->invoke_count != 0) {
        ta_invoke_cancel_at(sparse, sparse->invoke_count - 1u);
    }
    sparse->pending_invokes.count = 0;
    if (!sparse->has_parent) {
        return TA_OK;
    }
    sparse->completion_pending = true;
    return ta_invoke_publish(it->system, it->actor);
}

TA_PRIVATE void ta_invoke_release(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL) {
        return;
    }
    while (sparse->invoke_count != 0) {
        ta_invoke_cancel_at(sparse, sparse->invoke_count - 1u);
    }
    ta_mem_free(system->allocator, sparse->invokes,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_active_invoke),
                                             .alignment = _Alignof(ta_active_invoke) });
    ta_mem_free(system->allocator, sparse->invoke_meta,
                &(ta_allocator_free_params){ .size = sparse->invoke_capacity * sizeof(ta_invoke_meta),
                                             .alignment = _Alignof(ta_invoke_meta) });
    sparse->invokes = NULL;
    sparse->invoke_meta = NULL;
    sparse->invoke_capacity = 0;
    if (sparse->pending_invokes.element_size != 0) {
        ta_vec_destroy(&sparse->pending_invokes);
    }
}

/* ------------------------------------------------------------------------
 * Host reports
 */

static ta_result ta_invoke_unavailable(ta_system *system, ta_actor *actor, const ta_invoke_report_params *params) {
    /* error.communication.invoke.<id>.unavailable */
    ta_event_name_id name = 0;
    ta_atom invoke_atom = 0;
    ta_atom unavailable = 0;
    ta_result result = ta_atom_intern(&system->atoms, (ta_str){ .ptr = "invoke", .len = 6 }, &invoke_atom);
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, (ta_str){ .ptr = "unavailable", .len = 11 }, &unavailable);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, TA_EVENT_NAME_ERROR_COMMUNICATION, invoke_atom, &name);
    }
    ta_str text = ta_invoke_id_text(system, params->invoke_id);
    ta_atom id_atom = 0;
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, text, &id_atom);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, id_atom, &name);
    }
    if (result == TA_OK) {
        result = ta_event_name_child(&system->event_names, name, unavailable, &name);
    }
    if (result != TA_OK) {
        return result;
    }
    ta_event event = { .name = name, .type = TA_EVENT_PLATFORM, .invoke_id = TA_INVOKE_ID_NONE };
    if (params->as.fault != NULL) {
        result = ta_value_clone(&params->as.fault->data, &event.data);
        if (result != TA_OK) {
            return result;
        }
    }
    result = ta_interpreter_enqueue_internal(system, actor, &event);
    if (result == TA_OK && !actor->macrostep) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return result;
}

TA_API ta_result ta_actor_report_invoke(ta_system *system, const ta_invoke_report_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    int64_t index = ta_invoke_find(actor, params->invoke_id);
    if (index < 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = params->invoke_id };
    switch (params->kind) {
        case TA_INVOKE_REPORT_EVENT:
            if (params->as.event == NULL) {
                return TA_ERR_INVALID;
            }
            event = *params->as.event;
            event.type = TA_EVENT_EXTERNAL;
            event.invoke_id = params->invoke_id;
            return ta_actor_deliver(system, params->actor, &event);
        case TA_INVOKE_REPORT_DONE: {
            ta_result result = ta_invoke_done_name(system, params->invoke_id, &event.name);
            if (result != TA_OK) {
                return result;
            }
            if (params->as.done_data != NULL) {
                event.data = *params->as.done_data;
            }
            return ta_actor_deliver(system, params->actor, &event);
        }
        case TA_INVOKE_REPORT_UNAVAILABLE:
            ta_invoke_remove_at(actor->sparse, (uint32_t)index);
            return ta_invoke_unavailable(system, actor, params);
        default:
            return TA_ERR_INVALID;
    }
}

TA_API ta_result ta_actor_publish_completion(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor_sparse_state *sparse = actor->sparse;
    if (!actor->finished || sparse == NULL || !sparse->has_parent) {
        return TA_ERR_INVALID;
    }
    ta_actor *parent = ta_actor_lookup(system, sparse->parent.actor);
    if (parent == NULL || ta_invoke_find(parent, sparse->parent.invoke_id) < 0) {
        (void)ta_timer_withdraw(&system->timers, id, TA_SEND_ID_COMPLETION);
        return TA_ERR_NOT_FOUND;
    }
    if (ta_timer_withdraw(&system->timers, id, TA_SEND_ID_COMPLETION) != TA_OK) {
        return TA_OK; /* already delivered */
    }
    return ta_invoke_publish(system, actor);
}


TA_API void ta_definition_destroy(ta_definition *definition) {
    if (definition == NULL) {
        return;
    }
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (definition->datamodel_prepared) {
        v->destroy_definition(definition->datamodel.self, definition->datamodel_shared,
                              definition->allocator);
    }
    for (uint32_t index = 0; index < definition->expression_count; index += 1) {
        ta_expression *expression = &definition->expressions[index];
        if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
            expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                           definition->allocator);
        }
    }
    ta_allocator allocator = definition->allocator;
    ta_arena_destroy(&definition->arena);
    ta_mem_free(allocator, definition,
                &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                             .alignment = _Alignof(ta_definition) });
}

TA_PRIVATE const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id) {
    return ta_system_definition(system, id);
}

TA_PRIVATE bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state) {
    ta_state_kind kind = definition->states[state].kind;
    return kind == TA_STATE_ATOMIC || kind == TA_STATE_FINAL;
}

TA_PRIVATE bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return state != TA_STATE_NONE;
    }
    for (ta_state_id at = definition->states[state].parent; at != TA_STATE_NONE;
         at = definition->states[at].parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_atom *event;
    uint32_t event_count;
    const ta_event_pattern_segment *pattern;
    uint32_t pattern_count;
} ta_pattern_match;

/* A wildcard matches zero or more segments; trailing event segments are a prefix match. */
static bool ta_pattern_match_from(const ta_pattern_match *match, uint32_t event_index, uint32_t pattern_index) {
    if (pattern_index == match->pattern_count) {
        return true;
    }
    const ta_event_pattern_segment *segment = &match->pattern[pattern_index];
    if (segment->flags & TA_EVENT_PATTERN_SEGMENT_WILDCARD) {
        for (uint32_t skip = event_index; skip <= match->event_count; skip += 1) {
            if (ta_pattern_match_from(match, skip, pattern_index + 1u)) {
                return true;
            }
        }
        return false;
    }
    return event_index < match->event_count && match->event[event_index] == segment->atom &&
           ta_pattern_match_from(match, event_index + 1u, pattern_index + 1u);
}

TA_PRIVATE bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&definition->system->event_names, event, segments);
    if (count == 0) {
        return false;
    }
    ta_pattern_match match = {
        .event = segments,
        .event_count = count,
        .pattern = definition->event_pattern_segments + pattern->segments.offset,
        .pattern_count = pattern->segments.length,
    };
    return ta_pattern_match_from(&match, 0, 0);
}

TA_API ta_result ta_definition_state(const ta_system *system, const ta_definition_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (params->state >= definition->state_count) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_state_def *state = &definition->states[params->state];
    *params->out = (ta_state_info){
        .id = ta_atom_text(&system->atoms, state->name),
        .parent = state->parent,
        .kind = state->kind,
    };
    return TA_OK;
}

TA_API ta_result ta_definition_find_state(const ta_system *system, const ta_definition_find_state_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_atom name = 0;
    if (ta_atom_find(&system->atoms, params->id, &name) != TA_OK) {
        return TA_ERR_NOT_FOUND;
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (definition->states[state].name == name) {
            *params->out = state;
            return TA_OK;
        }
    }
    return TA_ERR_NOT_FOUND;
}

TA_PRIVATE size_t ta_definition_session_bytes(const ta_definition *definition) {
    /* The configuration, the late-binding bits (sized like it), and history. */
    return definition->configuration.bytes * 2u + definition->history.storage_bytes;
}

TA_PRIVATE size_t ta_definition_instance_bytes(const ta_definition *definition) {
    return definition->datamodel_instance_size == 0 ? 1u : definition->datamodel_instance_size;
}

TA_API ta_result ta_definition_inspect(const ta_system *system,
                                ta_definition_id id,
                                ta_definition_inspection *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, id);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_str document_name = { .ptr = "", .len = 0 };
    if (definition->document_name != TA_ATOM_NONE) {
        document_name = ta_atom_text(&system->atoms, definition->document_name);
    }
    *out = (ta_definition_inspection){
        .id = definition->id,
        .identity = definition->identity,
        .document_name = document_name,
        .datamodel = definition->datamodel,
        .binding = definition->binding_mode == TA_BINDING_LATE ? TA_SCXML_BINDING_LATE : TA_SCXML_BINDING_EARLY,
        .state_count = definition->state_count,
        .retired = definition->retired,
        .actor_count = definition->actor_count,
        .actor_fixed_bytes = sizeof(ta_actor) + ta_definition_session_bytes(definition) +
                             ta_definition_instance_bytes(definition),
        .definition_bytes = ta_arena_allocated_bytes(&definition->arena),
    };
    return TA_OK;
}


/*
 * Compiles a ta_scxml_spec in two passes. Pass one assigns state IDs in
 * document (pre-)order and records child ranges; pass two resolves targets and
 * compiles transitions, initial transitions, executable content, and data.
 * Every table is built in a growable vector and finally copied into the
 * definition's arena, which owns all compiled storage.
 */
typedef struct {
    ta_system *system;
    ta_allocator allocator;
    const ta_scxml_spec *spec;
    ta_definition *definition;
    ta_vec states;       /* ta_state_def */
    ta_vec state_specs;  /* const ta_scxml_state_spec * */
    ta_vec children;     /* ta_state_id */
    ta_vec transitions;  /* ta_transition_def */
    ta_vec initials;     /* ta_initial_transition_def */
    ta_vec targets;      /* ta_state_id */
    ta_vec actions;      /* ta_action_def */
    ta_vec handlers;     /* ta_span32: one action block per handler */
    ta_vec params;       /* ta_param_def */
    ta_vec data_decls;   /* ta_data_decl */
    ta_vec initializers; /* ta_initializer_def */
    ta_vec contents;     /* ta_content_def */
    ta_vec patterns;     /* ta_event_pattern */
    ta_vec segments;     /* ta_event_pattern_segment */
    ta_vec expressions;  /* ta_expression */
    ta_vec history;      /* ta_history_slot */
    ta_vec done_data;    /* ta_done_data_def */
    ta_vec invokes;      /* ta_invoke_decl */
    ta_vec texts;        /* ta_str: authored text kept for export */
    uint64_t hash;
} ta_compiler;

typedef struct {
    ta_vec *vec;
    void **out;
    uint32_t *count;
} ta_compiler_seal_params;

static void ta_compiler_hash(ta_compiler *compiler, const void *bytes, size_t size) {
    const unsigned char *in = bytes;
    for (size_t index = 0; index < size; index += 1) {
        compiler->hash ^= in[index];
        compiler->hash *= 1099511628211ull;
    }
}

static void ta_compiler_hash_str(ta_compiler *compiler, ta_str text) {
    ta_compiler_hash(compiler, &text.len, sizeof(text.len));
    if (text.ptr != NULL) {
        ta_compiler_hash(compiler, text.ptr, text.len);
    }
}

static void ta_compiler_hash_u32(ta_compiler *compiler, uint32_t value) {
    ta_compiler_hash(compiler, &value, sizeof(value));
}

static ta_atom_table *ta_compiler_atoms(ta_compiler *compiler) {
    return &compiler->system->atoms;
}

static ta_result ta_compiler_atom(ta_compiler *compiler, ta_str text, ta_atom *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_compiler_hash_str(compiler, text);
    return ta_atom_intern(ta_compiler_atoms(compiler), text, out);
}

TA_PRIVATE ta_result ta_duration_parse(ta_str text, ta_time *out) {
    if (text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    uint32_t index = 0;
    uint64_t whole = 0;
    uint64_t fraction = 0;
    uint64_t scale = 1;
    bool digits = false;
    while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
        if (whole > (UINT64_MAX - 9u) / 10u) {
            return TA_ERR_INVALID;
        }
        whole = whole * 10u + (uint64_t)(text.ptr[index] - '0');
        digits = true;
        index += 1;
    }
    if (index < text.len && text.ptr[index] == '.') {
        index += 1;
        bool fraction_digits = false;
        while (index < text.len && text.ptr[index] >= '0' && text.ptr[index] <= '9') {
            if (scale < 1000000000ull) {
                fraction = fraction * 10u + (uint64_t)(text.ptr[index] - '0');
                scale *= 10u;
            }
            fraction_digits = true;
            index += 1;
        }
        if (!fraction_digits) {
            return TA_ERR_INVALID;
        }
    }
    if (!digits) {
        return TA_ERR_INVALID;
    }
    ta_str unit = { .ptr = text.ptr + index, .len = text.len - index };
    uint64_t multiplier = 0;
    if (ta_str_equal(unit, (ta_str){ .ptr = "ms", .len = 2 })) {
        multiplier = 1;
    } else if (ta_str_equal(unit, (ta_str){ .ptr = "s", .len = 1 })) {
        multiplier = 1000;
    } else {
        return TA_ERR_INVALID;
    }
    if (whole > UINT64_MAX / multiplier) {
        return TA_ERR_INVALID;
    }
    /* Sub-millisecond remainders are truncated. */
    *out = whole * multiplier + (fraction * multiplier) / scale;
    return TA_OK;
}

static ta_result ta_compiler_expression(ta_compiler *compiler, const ta_expression *expression,
                                        ta_expr_id *out) {
    ta_expression copy = { .kind = expression->kind };
    ta_result result = TA_OK;
    switch (expression->kind) {
        case TA_EXPRESSION_SOURCE:
            ta_compiler_hash_str(compiler, expression->as.source);
            if (expression->as.source.ptr == NULL) {
                return TA_ERR_INVALID;
            }
            result = ta_arena_copy_str(&compiler->definition->arena, expression->as.source,
                                       &copy.as.source);
            break;
        case TA_EXPRESSION_HOST:
            if (expression->as.host.v == NULL || expression->as.host.v->retain == NULL) {
                return TA_ERR_INVALID;
            }
            copy.as.host = expression->as.host;
            result = expression->as.host.v->retain(expression->as.host.self, expression->as.host.value,
                                                   compiler->allocator, &copy.as.host.value);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    *out = compiler->expressions.count;
    result = ta_vec_push(&compiler->expressions, &copy);
    if (result != TA_OK && copy.kind == TA_EXPRESSION_HOST && copy.as.host.v->release != NULL) {
        copy.as.host.v->release(copy.as.host.self, copy.as.host.value, compiler->allocator);
    }
    return result;
}

/*
 * Keeps authored text that the compiler normalizes, so ta_definition_export()
 * can rebuild the exact input (ADR 0042). The text is copied into the
 * definition's arena.
 */
static ta_result ta_compiler_keep_text(ta_compiler *compiler, ta_str text, ta_authored_text_id *out) {
    ta_str copy = {0};
    ta_result result = ta_arena_copy_str(&compiler->definition->arena, text, &copy);
    if (result == TA_OK) {
        *out = compiler->texts.count;
        result = ta_vec_push(&compiler->texts, &copy);
    }
    return result;
}

static ta_result ta_compiler_content(ta_compiler *compiler, ta_bytes bytes, ta_content_id *out) {
    ta_content_def content = {0};
    if (bytes.len != 0) {
        uint8_t *copy = ta_arena_alloc(&compiler->definition->arena, bytes.len, 1);
        if (copy == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(copy, bytes.ptr, bytes.len);
        content.bytes = (ta_bytes){ .ptr = copy, .len = bytes.len };
    }
    ta_compiler_hash(compiler, bytes.ptr == NULL ? "" : (const void *)bytes.ptr, bytes.len);
    *out = compiler->contents.count;
    return ta_vec_push(&compiler->contents, &content);
}

static ta_result ta_compiler_value(ta_compiler *compiler, const ta_scxml_value_spec *spec,
                                   ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_VALUE_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_VALUE_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &spec->as.expr, &out->as.expr);
        case TA_SCXML_VALUE_LOCATION:
            out->kind = TA_VALUE_SOURCE_LOCATION;
            return ta_compiler_expression(compiler, &spec->as.location, &out->as.location);
        case TA_SCXML_VALUE_CONTENT:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler, spec->as.content, &out->as.content);
        default:
            return TA_ERR_INVALID;
    }
}

static ta_result ta_compiler_params(ta_compiler *compiler, const ta_scxml_param_spec *specs,
                                    ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].name.ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->params, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_param_def param = {0};
        result = ta_compiler_atom(compiler, specs[index].name, &param.name);
        if (result == TA_OK) {
            result = ta_compiler_value(compiler, &specs[index].value, &param.value);
        }
        if (result == TA_OK) {
            *(ta_param_def *)ta_vec_at(&compiler->params, base + index) = param;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_block(ta_compiler *compiler, const ta_scxml_action_spec *specs,
                                   ta_span32 *out);

static ta_result ta_compiler_send(ta_compiler *compiler, const ta_scxml_send_spec *spec,
                                  ta_send_action *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom(compiler, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression(compiler, &spec->id.as.location, &out->id.as.location);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom(compiler, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression(compiler, &spec->type.as.expr, &out->type.as.expr);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    out->delay.text = TA_AUTHORED_TEXT_NONE;
    ta_compiler_hash_u32(compiler, spec->event.kind);
    if (spec->event.kind == TA_SCXML_EVENT_EXPR) {
        out->event.kind = TA_EVENT_SOURCE_EXPR;
        result = ta_compiler_expression(compiler, &spec->event.as.expr, &out->event.as.expr);
    } else if (spec->event.kind == TA_SCXML_EVENT_NAME) {
        out->event.kind = TA_EVENT_SOURCE_NAME;
        ta_compiler_hash_str(compiler, spec->event.as.name);
        result = ta_event_name_intern_dotted(&compiler->system->event_names, spec->event.as.name,
                                             &out->event.as.name);
    } else {
        return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->target.kind) {
        case TA_SCXML_STRING_NONE:
            out->target.kind = TA_VALUE_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->target.kind = TA_VALUE_SOURCE_CONTENT;
            result = ta_compiler_content(compiler,
                                         (ta_bytes){ .ptr = (const uint8_t *)spec->target.as.literal.ptr,
                                                     .len = spec->target.as.literal.len },
                                         &out->target.as.content);
            break;
        case TA_SCXML_STRING_EXPR:
            out->target.kind = TA_VALUE_SOURCE_EXPR;
            result = ta_compiler_expression(compiler, &spec->target.as.expr, &out->target.as.expr);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->delay.kind) {
        case TA_SCXML_STRING_NONE:
            out->delay.kind = TA_DELAY_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->delay.kind = TA_DELAY_SOURCE_LITERAL;
            ta_compiler_hash_str(compiler, spec->delay.as.literal);
            result = ta_duration_parse(spec->delay.as.literal, &out->delay.as.literal);
            if (result == TA_OK) {
                result = ta_compiler_keep_text(compiler, spec->delay.as.literal, &out->delay.text);
            }
            break;
        case TA_SCXML_STRING_EXPR:
            out->delay.kind = TA_DELAY_SOURCE_EXPR;
            result = ta_compiler_expression(compiler, &spec->delay.as.expr, &out->delay.as.expr);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK && out->params.length != 0 && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = TA_ERR_INVALID;
    }
    return result;
}

/*
 * <foreach>: the array is an expression; item and index are variable names
 * whose legality the data model judges when it binds them (SCXML 4.6).
 */
static ta_result ta_compiler_foreach(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                     ta_foreach_action *out) {
    if (spec->as.foreach.array.kind == TA_EXPRESSION_NONE) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_compiler_expression(compiler, &spec->as.foreach.array, &out->array);
    if (result == TA_OK) {
        result = ta_compiler_atom(compiler, spec->as.foreach.item, &out->item);
    }
    if (result == TA_OK && spec->as.foreach.index.ptr != NULL) {
        out->has_index = true;
        result = ta_compiler_atom(compiler, spec->as.foreach.index, &out->index);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->as.foreach.body, &out->body_actions);
    }
    return result;
}

static ta_result ta_compiler_action(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                    ta_action_def *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    ta_result result = TA_OK;
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            out->kind = TA_ACTION_RAISE;
            ta_compiler_hash_str(compiler, spec->as.raise.event);
            result = ta_event_name_intern_dotted(&compiler->system->event_names, spec->as.raise.event,
                                                 &out->as.raise.event);
            if (result == TA_OK) {
                result = ta_compiler_value(compiler, &spec->as.raise.data, &out->as.raise.data);
            }
            return result;
        case TA_SCXML_ACTION_SEND:
            out->kind = TA_ACTION_SEND;
            return ta_compiler_send(compiler, &spec->as.send, &out->as.send);
        case TA_SCXML_ACTION_CANCEL:
            out->kind = TA_ACTION_CANCEL;
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_EXPR) {
                out->as.cancel.sendid.kind = TA_TEXT_SOURCE_EXPR;
                return ta_compiler_expression(compiler, &spec->as.cancel.sendid.as.expr,
                                              &out->as.cancel.sendid.as.expr);
            }
            if (spec->as.cancel.sendid.kind != TA_SCXML_STRING_LITERAL) {
                return TA_ERR_INVALID;
            }
            out->as.cancel.sendid.kind = TA_TEXT_SOURCE_ATOM;
            return ta_compiler_atom(compiler, spec->as.cancel.sendid.as.literal,
                                    &out->as.cancel.sendid.as.atom);
        case TA_SCXML_ACTION_ASSIGN:
            out->kind = TA_ACTION_ASSIGN;
            if (spec->as.assign.value.kind == TA_SCXML_VALUE_NONE) {
                return TA_ERR_INVALID;
            }
            result = ta_compiler_expression(compiler, &spec->as.assign.location, &out->as.assign.location);
            if (result == TA_OK) {
                result = ta_compiler_value(compiler, &spec->as.assign.value, &out->as.assign.value);
            }
            return result;
        case TA_SCXML_ACTION_SCRIPT:
            out->kind = TA_ACTION_SCRIPT;
            return ta_compiler_expression(compiler, &spec->as.script.script, &out->as.script);
        case TA_SCXML_ACTION_LOG:
            out->kind = TA_ACTION_LOG;
            if (spec->as.log.label.ptr != NULL) {
                out->as.log.has_label = true;
                result = ta_atom_intern(ta_compiler_atoms(compiler), spec->as.log.label, &out->as.log.label);
            }
            if (result == TA_OK && spec->as.log.expr.kind != TA_EXPRESSION_NONE) {
                out->as.log.has_expr = true;
                result = ta_compiler_expression(compiler, &spec->as.log.expr, &out->as.log.expr);
            }
            return result;
        case TA_SCXML_ACTION_IF:
            out->kind = TA_ACTION_IF;
            result = ta_compiler_expression(compiler, &spec->as.conditional.condition,
                                            &out->as.conditional.condition);
            if (result == TA_OK) {
                result = ta_compiler_block(compiler, spec->as.conditional.then_actions,
                                           &out->as.conditional.then_actions);
            }
            if (result == TA_OK) {
                result = ta_compiler_block(compiler, spec->as.conditional.else_actions,
                                           &out->as.conditional.else_actions);
            }
            return result;
        case TA_SCXML_ACTION_FOREACH:
            out->kind = TA_ACTION_FOREACH;
            return ta_compiler_foreach(compiler, spec, &out->as.foreach);
        case TA_SCXML_ACTION_INVOKE:
            return TA_ERR_UNSUPPORTED;
        default:
            return TA_ERR_INVALID;
    }
}

static ta_result ta_compiler_block(ta_compiler *compiler, const ta_scxml_action_spec *specs,
                                   ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].kind != TA_SCXML_ACTION_NONE) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->actions, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        /* Nested blocks append behind this block, so write by index. */
        ta_action_def action = {0};
        result = ta_compiler_action(compiler, &specs[index], &action);
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base + index) = action;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

/*
 * Compiles a list of <onentry> or <onexit> handlers. Each handler keeps its
 * own action block, so an error in one handler does not skip the next.
 */
static ta_result ta_compiler_handlers(ta_compiler *compiler, const ta_scxml_handler_spec *specs,
                                      ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].actions != NULL) {
        count += 1;
    }
    ta_compiler_hash_u32(compiler, count);
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->handlers, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_span32 block = {0};
        result = ta_compiler_block(compiler, specs[index].actions, &block);
        if (result == TA_OK) {
            ta_compiler_hash_u32(compiler, block.length);
            *(ta_span32 *)ta_vec_at(&compiler->handlers, base + index) = block;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_state_def *ta_compiler_state(ta_compiler *compiler, ta_state_id state) {
    return ta_vec_at(&compiler->states, state);
}

static ta_result ta_compiler_kind(ta_scxml_state_kind kind, ta_state_kind *out) {
    switch (kind) {
        case TA_SCXML_STATE_ATOMIC: *out = TA_STATE_ATOMIC; return TA_OK;
        case TA_SCXML_STATE_COMPOUND: *out = TA_STATE_COMPOUND; return TA_OK;
        case TA_SCXML_STATE_PARALLEL: *out = TA_STATE_PARALLEL; return TA_OK;
        case TA_SCXML_STATE_FINAL: *out = TA_STATE_FINAL; return TA_OK;
        case TA_SCXML_STATE_HISTORY_SHALLOW: *out = TA_STATE_HISTORY_SHALLOW; return TA_OK;
        case TA_SCXML_STATE_HISTORY_DEEP: *out = TA_STATE_HISTORY_DEEP; return TA_OK;
        default: return TA_ERR_INVALID;
    }
}

static bool ta_state_kind_is_history(ta_state_kind kind) {
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

/* Pass one: assigns IDs in document order and records child ranges. */
static ta_result ta_compiler_collect(ta_compiler *compiler, const ta_scxml_state_spec *specs,
                                     ta_state_id parent, ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].kind != TA_SCXML_STATE_NONE) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->children, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        const ta_scxml_state_spec *spec = &specs[index];
        ta_state_def state = {
            .parent = parent,
            .initial = TA_INITIAL_TRANSITION_NONE,
            .done_data = TA_DONE_DATA_NONE,
            .history_slot = UINT32_MAX,
        };
        ta_compiler_hash_u32(compiler, spec->kind);
        result = ta_compiler_kind(spec->kind, &state.kind);
        if (result == TA_OK) {
            result = ta_compiler_atom(compiler, spec->id, &state.name);
        }
        for (uint32_t other = 0; result == TA_OK && other < compiler->states.count; other += 1) {
            if (ta_compiler_state(compiler, other)->name == state.name) {
                result = TA_ERR_INVALID;
            }
        }
        if (result != TA_OK) {
            return result;
        }
        ta_state_id id = compiler->states.count;
        *(ta_state_id *)ta_vec_at(&compiler->children, base + index) = id;
        result = ta_vec_push(&compiler->states, &state);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->state_specs, &spec);
        }
        ta_span32 children = {0};
        if (result == TA_OK) {
            result = ta_compiler_collect(compiler, spec->states, id, &children);
        }
        if (result == TA_OK) {
            ta_compiler_state(compiler, id)->children = children;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_resolve(ta_compiler *compiler, ta_str id, ta_state_id *out) {
    ta_atom atom = 0;
    ta_result result = ta_compiler_atom(compiler, id, &atom);
    if (result != TA_OK) {
        return result;
    }
    for (ta_state_id state = 0; state < compiler->states.count; state += 1) {
        if (ta_compiler_state(compiler, state)->name == atom) {
            *out = state;
            return TA_OK;
        }
    }
    return TA_ERR_INVALID;
}

static bool ta_compiler_descends(ta_compiler *compiler, ta_state_id state, ta_state_id ancestor) {
    if (ancestor == TA_STATE_NONE) {
        return true;
    }
    for (ta_state_id at = ta_compiler_state(compiler, state)->parent; at != TA_STATE_NONE;
         at = ta_compiler_state(compiler, at)->parent) {
        if (at == ancestor) {
            return true;
        }
    }
    return false;
}

typedef struct {
    const ta_str *ids;
    /* Targets must be proper descendants of this state (TA_STATE_NONE: any). */
    ta_state_id within;
    ta_span32 *out;
} ta_compiler_targets_params;

static ta_result ta_compiler_targets(ta_compiler *compiler, const ta_compiler_targets_params *params) {
    uint32_t count = 0;
    while (params->ids != NULL && params->ids[count].ptr != NULL) {
        count += 1;
    }
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->targets, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_state_id target = TA_STATE_NONE;
        result = ta_compiler_resolve(compiler, params->ids[index], &target);
        if (result == TA_OK && !ta_compiler_descends(compiler, target, params->within)) {
            result = TA_ERR_INVALID;
        }
        if (result == TA_OK) {
            *(ta_state_id *)ta_vec_at(&compiler->targets, base + index) = target;
        }
    }
    *params->out = (ta_span32){ .offset = base, .length = count };
    return result;
}

/* Parses space-separated descriptors; a `*` segment is a wildcard. */
static ta_result ta_compiler_descriptors(ta_compiler *compiler, ta_str text, ta_span32 *out) {
    ta_compiler_hash_str(compiler, text);
    uint32_t first = compiler->patterns.count;
    uint32_t index = 0;
    while (text.ptr != NULL && index < text.len) {
        while (index < text.len && (text.ptr[index] == ' ' || text.ptr[index] == '\t' ||
                                    text.ptr[index] == '\n' || text.ptr[index] == '\r')) {
            index += 1;
        }
        uint32_t start = index;
        while (index < text.len && text.ptr[index] != ' ' && text.ptr[index] != '\t' &&
               text.ptr[index] != '\n' && text.ptr[index] != '\r') {
            index += 1;
        }
        if (index == start) {
            break;
        }
        ta_event_pattern pattern = { .segments = { .offset = compiler->segments.count } };
        uint32_t segment_start = start;
        if (index - start == 2 && text.ptr[start] == '.' && text.ptr[start + 1u] == '*') {
            /* ".*" is the empty prefix with the optional ".*" suffix: every event. */
            segment_start = start + 1u;
        }
        for (uint32_t at = segment_start; at <= index; at += 1) {
            if (at < index && text.ptr[at] != '.') {
                continue;
            }
            ta_str segment = { .ptr = text.ptr + segment_start, .len = at - segment_start };
            if (segment.len == 0) {
                return TA_ERR_INVALID;
            }
            ta_event_pattern_segment compiled = {0};
            ta_result result = TA_OK;
            if (segment.len == 1 && segment.ptr[0] == '*') {
                compiled.flags = TA_EVENT_PATTERN_SEGMENT_WILDCARD;
                compiled.atom = TA_ATOM_NONE;
            } else {
                result = ta_atom_intern(ta_compiler_atoms(compiler), segment, &compiled.atom);
            }
            /* A trailing `.*` adds nothing to SCXML prefix matching. */
            bool trailing_wildcard = at == index && pattern.segments.length > 0 &&
                                     compiled.flags == TA_EVENT_PATTERN_SEGMENT_WILDCARD;
            if (result == TA_OK && !trailing_wildcard) {
                result = ta_vec_push(&compiler->segments, &compiled);
                pattern.segments.length += 1;
            }
            if (result != TA_OK) {
                return result;
            }
            segment_start = at + 1u;
        }
        ta_result result = ta_vec_push(&compiler->patterns, &pattern);
        if (result != TA_OK) {
            return result;
        }
    }
    *out = (ta_span32){ .offset = first, .length = compiler->patterns.count - first };
    return TA_OK;
}

static ta_result ta_compiler_transition(ta_compiler *compiler, ta_state_id source,
                                        const ta_scxml_transition_spec *spec) {
    ta_transition_def transition = {
        .source = source,
        .condition = UINT32_MAX,
        .document_order = compiler->transitions.count,
        .event_text = TA_AUTHORED_TEXT_NONE,
    };
    ta_compiler_hash_u32(compiler, spec->kind);
    if (spec->kind == TA_SCXML_TRANSITION_EXTERNAL) {
        transition.type = TA_TRANSITION_EXTERNAL;
    } else if (spec->kind == TA_SCXML_TRANSITION_INTERNAL) {
        transition.type = TA_TRANSITION_INTERNAL;
    } else {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_compiler_descriptors(compiler, spec->event, &transition.event_patterns);
    if (result == TA_OK && spec->event.ptr != NULL) {
        result = ta_compiler_keep_text(compiler, spec->event, &transition.event_text);
    }
    if (result == TA_OK && spec->condition.kind != TA_EXPRESSION_NONE) {
        result = ta_compiler_expression(compiler, &spec->condition, &transition.condition);
    }
    if (result == TA_OK) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = spec->targets,
                                                   .within = TA_STATE_NONE,
                                                   .out = &transition.targets,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->actions, &transition.actions);
    }
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->transitions, &transition);
    }
    return result;
}

typedef struct {
    ta_state_id owner;
    const ta_scxml_initial_spec *spec;
    ta_span32 children;
    ta_initial_transition_id *out;
} ta_compiler_initial_params;

static ta_result ta_compiler_initial(ta_compiler *compiler, const ta_compiler_initial_params *params) {
    ta_initial_transition_def initial = { .owner = params->owner };
    ta_result result = TA_OK;
    if (params->spec != NULL && params->spec->targets != NULL && params->spec->targets[0].ptr != NULL) {
        initial.authored_targets = true;
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = params->spec->targets,
                                                   .within = params->owner,
                                                   .out = &initial.targets,
                                               });
    } else {
        /* SCXML default: the first child state in document order. */
        ta_state_id first = TA_STATE_NONE;
        for (uint32_t index = 0; index < params->children.length && first == TA_STATE_NONE; index += 1) {
            ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, params->children.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind)) {
                first = child;
            }
        }
        if (first == TA_STATE_NONE) {
            return TA_ERR_INVALID;
        }
        initial.targets.offset = compiler->targets.count;
        initial.targets.length = 1;
        result = ta_vec_push(&compiler->targets, &first);
    }
    if (result == TA_OK && params->spec != NULL) {
        result = ta_compiler_block(compiler, params->spec->actions, &initial.actions);
    }
    if (result == TA_OK) {
        *params->out = compiler->initials.count;
        result = ta_vec_push(&compiler->initials, &initial);
    }
    return result;
}

static ta_result ta_compiler_data(ta_compiler *compiler, const ta_scxml_data_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs != NULL && specs[index].name.ptr != NULL; index += 1) {
        ta_data_decl decl = { .owner_state = owner, .initializer = TA_INITIALIZER_NONE };
        ta_result result = ta_compiler_atom(compiler, specs[index].name, &decl.name);
        if (result == TA_OK && specs[index].initializer.kind != TA_SCXML_VALUE_NONE) {
            ta_initializer_def initializer = {0};
            result = ta_compiler_value(compiler, &specs[index].initializer, &initializer.value);
            if (result == TA_OK) {
                decl.initializer = compiler->initializers.count;
                result = ta_vec_push(&compiler->initializers, &initializer);
            }
        }
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->data_decls, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* <donedata>: exactly one of content or params (SCXML 5.5). */
static ta_result ta_compiler_done_data(ta_compiler *compiler, const ta_scxml_done_data_spec *spec,
                                       ta_done_data_id *out) {
    ta_done_data_def done = {0};
    ta_result result = ta_compiler_value(compiler, &spec->content, &done.content);
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &done.params);
    }
    if (result == TA_OK && done.params.length != 0 && done.content.kind != TA_VALUE_SOURCE_NONE) {
        result = TA_ERR_INVALID;
    }
    if (result == TA_OK) {
        *out = compiler->done_data.count;
        result = ta_vec_push(&compiler->done_data, &done);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Invoke
 */

static ta_result ta_compiler_invoke_text(ta_compiler *compiler, const ta_scxml_string_spec *spec,
                                         ta_value_source *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    switch (spec->kind) {
        case TA_SCXML_STRING_NONE:
            *out = (ta_value_source){ .kind = TA_VALUE_SOURCE_NONE };
            return TA_OK;
        case TA_SCXML_STRING_LITERAL:
            out->kind = TA_VALUE_SOURCE_CONTENT;
            return ta_compiler_content(compiler,
                                       (ta_bytes){ .ptr = (const uint8_t *)spec->as.literal.ptr,
                                                   .len = spec->as.literal.len },
                                       &out->as.content);
        case TA_SCXML_STRING_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &spec->as.expr, &out->as.expr);
        default:
            return TA_ERR_INVALID;
    }
}

static ta_result ta_compiler_invoke(ta_compiler *compiler, const ta_scxml_invoke_spec *spec,
                                    ta_invoke_decl *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom(compiler, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression(compiler, &spec->id.as.location, &out->id.as.location);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result != TA_OK) {
        return result;
    }
    switch (spec->type.kind) {
        case TA_SCXML_STRING_NONE:
            out->type.kind = TA_TEXT_SOURCE_NONE;
            break;
        case TA_SCXML_STRING_LITERAL:
            out->type.kind = TA_TEXT_SOURCE_ATOM;
            result = ta_compiler_atom(compiler, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression(compiler, &spec->type.as.expr, &out->type.as.expr);
            break;
        default:
            return TA_ERR_INVALID;
    }
    if (result == TA_OK) {
        result = ta_compiler_invoke_text(compiler, &spec->src, &out->src);
    }
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->finalize, &out->finalize_actions);
    }
    out->autoforward = spec->autoforward;
    ta_compiler_hash_u32(compiler, spec->autoforward ? 1u : 0u);
    /* SCXML: src and inline content are exclusive. */
    if (result == TA_OK && out->src.kind != TA_VALUE_SOURCE_NONE && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = TA_ERR_INVALID;
    }
    return result;
}

static ta_result ta_compiler_invokes(ta_compiler *compiler, const ta_scxml_invoke_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs[index].kind != TA_SCXML_INVOKE_NONE; index += 1) {
        ta_invoke_decl decl = { .owner = owner };
        ta_result result = ta_compiler_invoke(compiler, &specs[index], &decl);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->invokes, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* Pass two for one state. */
static ta_result ta_compiler_compile_state(ta_compiler *compiler, ta_state_id id) {
    const ta_scxml_state_spec *spec = *(const ta_scxml_state_spec **)ta_vec_at(&compiler->state_specs, id);
    ta_state_def state = *ta_compiler_state(compiler, id);
    ta_state_def parent = {0};
    if (state.parent != TA_STATE_NONE) {
        parent = *ta_compiler_state(compiler, state.parent);
    }
    uint32_t real_children = 0;
    for (uint32_t index = 0; index < state.children.length; index += 1) {
        ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, state.children.offset + index);
        real_children += ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind) ? 0u : 1u;
    }
    bool history = ta_state_kind_is_history(state.kind);
    if (spec->done_data != NULL && state.kind != TA_STATE_FINAL) {
        return TA_ERR_INVALID;
    }
    switch (state.kind) {
        case TA_STATE_ATOMIC:
        case TA_STATE_FINAL:
            if (state.children.length != 0 || spec->initial != NULL) {
                return TA_ERR_INVALID;
            }
            break;
        case TA_STATE_COMPOUND:
        case TA_STATE_PARALLEL:
            if (real_children == 0 || (state.kind == TA_STATE_PARALLEL && spec->initial != NULL)) {
                return TA_ERR_INVALID;
            }
            break;
        default:
            if (state.parent == TA_STATE_NONE || state.children.length != 0 ||
                (parent.kind != TA_STATE_COMPOUND && parent.kind != TA_STATE_PARALLEL)) {
                return TA_ERR_INVALID;
            }
            break;
    }

    state.transitions.offset = compiler->transitions.count;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1) {
        result = ta_compiler_transition(compiler, id, &spec->transitions[index]);
    }
    state.transitions.length = compiler->transitions.count - state.transitions.offset;
    if (result != TA_OK) {
        return result;
    }
    if (history) {
        /* A history state has exactly one targetless-event default transition into its parent. */
        if (state.transitions.length != 1) {
            return TA_ERR_INVALID;
        }
        const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state.transitions.offset);
        if (fallback->event_patterns.length != 0 || fallback->condition != UINT32_MAX ||
            fallback->targets.length == 0) {
            return TA_ERR_INVALID;
        }
        for (uint32_t index = 0; index < fallback->targets.length; index += 1) {
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, fallback->targets.offset + index);
            if (!ta_compiler_descends(compiler, target, state.parent)) {
                return TA_ERR_INVALID;
            }
        }
        state.history_slot = compiler->history.count;
        ta_history_slot slot = {0};
        result = ta_vec_push(&compiler->history, &slot);
    }
    if (result == TA_OK && state.kind == TA_STATE_COMPOUND) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = id,
                                                   .spec = spec->initial,
                                                   .children = state.children,
                                                   .out = &state.initial,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, spec->onentry, &state.onentry_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, spec->onexit, &state.onexit_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
    }
    if (result == TA_OK && spec->done_data != NULL) {
        result = ta_compiler_done_data(compiler, spec->done_data, &state.done_data);
    }
    /* Invoke: compile this state's <invoke> children. */
    if (result == TA_OK && spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        result = history ? TA_ERR_INVALID : ta_compiler_invokes(compiler, spec->invokes, id);
    }
    if (result == TA_OK) {
        ta_atom segment = state.name;
        result = ta_event_name_child(&compiler->system->event_names, TA_EVENT_NAME_DONE_STATE, segment,
                                     &state.done_event);
    }
    if (result == TA_OK) {
        *ta_compiler_state(compiler, id) = state;
    }
    return result;
}

static ta_result ta_compiler_seal(ta_compiler *compiler, const ta_compiler_seal_params *params) {
    *params->out = NULL;
    if (params->count != NULL) {
        *params->count = params->vec->count;
    }
    if (params->vec->count == 0) {
        return TA_OK;
    }
    size_t size = params->vec->count * params->vec->element_size;
    void *copy = ta_arena_alloc(&compiler->definition->arena, size, params->vec->element_alignment);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, params->vec->items, size);
    *params->out = copy;
    return TA_OK;
}

static void ta_compiler_destroy(ta_compiler *compiler) {
    ta_vec *vecs[] = {
        &compiler->states, &compiler->state_specs, &compiler->children, &compiler->transitions,
        &compiler->initials, &compiler->targets, &compiler->actions, &compiler->handlers, &compiler->params,
        &compiler->data_decls, &compiler->initializers, &compiler->contents, &compiler->patterns,
        &compiler->segments, &compiler->expressions, &compiler->history, &compiler->done_data,
        &compiler->invokes, &compiler->texts,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_destroy(vecs[index]);
    }
}

static ta_result ta_compiler_identity(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_scxml_spec *spec = compiler->spec;
    ta_str family = spec->name;
    ta_bytes revision = {0};
    static const char hex[] = "0123456789abcdef";
    char fingerprint[16];
    definition->fingerprinted = spec->identity == NULL;
    if (spec->identity != NULL) {
        family = spec->identity->family.name;
        revision = spec->identity->revision;
    } else {
        for (uint32_t index = 0; index < 16; index += 1) {
            fingerprint[index] = hex[(compiler->hash >> (60u - index * 4u)) & 0xfu];
        }
        revision = (ta_bytes){ .ptr = (const uint8_t *)fingerprint, .len = 16 };
    }
    if (family.ptr == NULL || family.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_result result = ta_arena_copy_str(&definition->arena, family, &definition->identity.family.name);
    ta_str revision_copy = {0};
    if (result == TA_OK) {
        result = ta_arena_copy_str(&definition->arena,
                                   (ta_str){ .ptr = (const char *)revision.ptr, .len = revision.len },
                                   &revision_copy);
    }
    definition->identity.revision = (ta_bytes){ .ptr = (const uint8_t *)revision_copy.ptr,
                                                .len = revision_copy.len };
    if (result == TA_OK) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), family, &definition->name);
    }
    definition->document_name = TA_ATOM_NONE;
    if (result == TA_OK && spec->name.ptr != NULL && spec->name.len != 0) {
        result = ta_atom_intern(ta_compiler_atoms(compiler), spec->name, &definition->document_name);
    }
    definition->authored_binding = spec->binding;
    if (result == TA_OK && spec->datamodel.ptr != NULL && spec->datamodel.len != 0) {
        result = ta_arena_copy_str(&definition->arena, spec->datamodel, &definition->authored_datamodel);
    }
    return result;
}

static ta_result ta_compiler_finish(ta_compiler *compiler) {
    ta_definition *definition = compiler->definition;
    const ta_compiler_seal_params seals[] = {
        { &compiler->states, (void **)&definition->states, &definition->state_count },
        { &compiler->children, (void **)&definition->children, &definition->child_count },
        { &compiler->transitions, (void **)&definition->transitions, &definition->transition_count },
        { &compiler->initials, (void **)&definition->initial_transitions, &definition->initial_transition_count },
        { &compiler->targets, (void **)&definition->targets, &definition->target_count },
        { &compiler->actions, (void **)&definition->action_defs, &definition->action_count },
        { &compiler->handlers, (void **)&definition->handlers, &definition->handler_count },
        { &compiler->params, (void **)&definition->params, &definition->param_count },
        { &compiler->data_decls, (void **)&definition->data_decls, &definition->data_decl_count },
        { &compiler->initializers, (void **)&definition->initializers, &definition->initializer_count },
        { &compiler->contents, (void **)&definition->contents, &definition->content_count },
        { &compiler->patterns, (void **)&definition->event_patterns, &definition->event_pattern_count },
        { &compiler->segments, (void **)&definition->event_pattern_segments, &definition->event_pattern_segment_count },
        { &compiler->history, (void **)&definition->history.slots, &definition->history.slot_count },
        { &compiler->done_data, (void **)&definition->done_data, &definition->done_data_count },
        { &compiler->invokes, (void **)&definition->invokes, &definition->invoke_decl_count },
        { &compiler->texts, (void **)&definition->authored_texts, &definition->authored_text_count },
    };
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && index < TA_ARRAY_COUNT(seals); index += 1) {
        result = ta_compiler_seal(compiler, &seals[index]);
    }
    if (result != TA_OK) {
        return result;
    }
    uint32_t words = ta_bits_words(definition->state_count);
    if (words * 8u > UINT16_MAX) {
        return TA_ERR_UNSUPPORTED;
    }
    definition->configuration = (ta_configuration_layout){
        .kind = TA_CONFIG_WORDS,
        .bytes = (uint16_t)(words * 8u),
        .alignment = (uint16_t)_Alignof(uint64_t),
        .atomic_state_count = definition->state_count,
    };
    for (uint32_t slot = 0; slot < definition->history.slot_count; slot += 1) {
        definition->history.slots[slot] = (ta_history_slot){ .offset = slot * words * 8u,
                                                             .bytes = (uint16_t)(words * 8u) };
    }
    definition->history.storage_bytes = definition->history.slot_count * words * 8u;
    return ta_compiler_identity(compiler);
}

static ta_result ta_compiler_run(ta_compiler *compiler) {
    const ta_scxml_spec *spec = compiler->spec;
    ta_definition *definition = compiler->definition;
    ta_compiler_hash_str(compiler, spec->name);
    ta_compiler_hash_u32(compiler, spec->binding);
    ta_span32 roots = {0};
    ta_result result = ta_compiler_collect(compiler, spec->states, TA_STATE_NONE, &roots);
    if (result == TA_OK && compiler->states.count == 0) {
        result = TA_ERR_INVALID;
    }
    /* Data declarations are listed root first, then by state in document order. */
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < compiler->states.count; state += 1) {
        result = ta_compiler_compile_state(compiler, state);
    }
    if (result == TA_OK) {
        result = ta_compiler_initial(compiler, &(ta_compiler_initial_params){
                                                   .owner = TA_STATE_NONE,
                                                   .spec = spec->initial,
                                                   .children = roots,
                                                   .out = &definition->root_initial,
                                               });
    }
    if (result == TA_OK && spec->script.kind != TA_EXPRESSION_NONE) {
        uint32_t base = 0;
        ta_action_def script = { .kind = TA_ACTION_SCRIPT };
        result = ta_compiler_expression(compiler, &spec->script, &script.as.script);
        if (result == TA_OK) {
            result = ta_vec_extend(&compiler->actions, 1, &base);
        }
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base) = script;
            definition->global_script_actions = (ta_span32){ .offset = base, .length = 1 };
        }
    }
    if (result == TA_OK) {
        result = ta_compiler_finish(compiler);
    }
    return result;
}

static bool ta_datamodel_complete(const ta_datamodel *datamodel) {
    const ta_datamodel_vtable *v = datamodel->v;
    return v != NULL && v->prepare_definition != NULL && v->destroy_definition != NULL &&
           v->instance_size != NULL && v->instance_alignment != NULL && v->init_instance != NULL &&
           v->destroy_instance != NULL;
}

TA_API ta_result ta_definition_compile_spec(ta_system *system, const ta_definition_compile_params *params) {
    if (system == NULL || params == NULL || params->spec == NULL || params->out == NULL ||
        params->allocator.v == NULL || !ta_datamodel_complete(&params->datamodel)) {
        return TA_ERR_INVALID;
    }
    const ta_scxml_spec *spec = params->spec;
    if (spec->datamodel.ptr != NULL && spec->datamodel.len != 0 &&
        !ta_str_equal(spec->datamodel, params->datamodel.v->name)) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_definition *definition = ta_mem_alloc(params->allocator, sizeof(ta_definition),
                                             _Alignof(ta_definition));
    if (definition == NULL) {
        return TA_ERR_NOMEM;
    }
    *definition = (ta_definition){
        .allocator = params->allocator,
        .system = system,
        .root_initial = TA_INITIAL_TRANSITION_NONE,
        .binding_mode = spec->binding == TA_SCXML_BINDING_LATE ? TA_BINDING_LATE : TA_BINDING_EARLY,
        .datamodel = params->datamodel,
    };
    ta_result result = ta_arena_init(&definition->arena, params->allocator, 4096);
    if (result != TA_OK) {
        ta_mem_free(params->allocator, definition,
                    &(ta_allocator_free_params){ .size = sizeof(ta_definition),
                                                 .alignment = _Alignof(ta_definition) });
        return result;
    }

    ta_compiler compiler = {
        .system = system,
        .allocator = params->allocator,
        .spec = spec,
        .definition = definition,
        .hash = 14695981039346656037ull,
    };
    const struct { ta_vec *vec; const ta_tree_list_layout *layout; } vecs[] = {
        { &compiler.states, TA_VEC_LAYOUT(ta_state_def) },
        { &compiler.state_specs, TA_VEC_LAYOUT(const ta_scxml_state_spec *) },
        { &compiler.children, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.transitions, TA_VEC_LAYOUT(ta_transition_def) },
        { &compiler.initials, TA_VEC_LAYOUT(ta_initial_transition_def) },
        { &compiler.targets, TA_VEC_LAYOUT(ta_state_id) },
        { &compiler.actions, TA_VEC_LAYOUT(ta_action_def) },
        { &compiler.handlers, TA_VEC_LAYOUT(ta_span32) },
        { &compiler.params, TA_VEC_LAYOUT(ta_param_def) },
        { &compiler.data_decls, TA_VEC_LAYOUT(ta_data_decl) },
        { &compiler.initializers, TA_VEC_LAYOUT(ta_initializer_def) },
        { &compiler.contents, TA_VEC_LAYOUT(ta_content_def) },
        { &compiler.patterns, TA_VEC_LAYOUT(ta_event_pattern) },
        { &compiler.segments, TA_VEC_LAYOUT(ta_event_pattern_segment) },
        { &compiler.expressions, TA_VEC_LAYOUT(ta_expression) },
        { &compiler.history, TA_VEC_LAYOUT(ta_history_slot) },
        { &compiler.done_data, TA_VEC_LAYOUT(ta_done_data_def) },
        { &compiler.invokes, TA_VEC_LAYOUT(ta_invoke_decl) },
        { &compiler.texts, TA_VEC_LAYOUT(ta_str) },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_init(vecs[index].vec, params->allocator, vecs[index].layout);
    }

    result = ta_compiler_run(&compiler);
    /* The expression table moves to the definition even on failure, so it is released once. */
    ta_result sealed = ta_compiler_seal(&compiler, &(ta_compiler_seal_params){
                                                       .vec = &compiler.expressions,
                                                       .out = (void **)&definition->expressions,
                                                       .count = &definition->expression_count,
                                                   });
    if (sealed != TA_OK) {
        /* Could not move host expressions; release them from the vector. */
        for (uint32_t index = 0; index < compiler.expressions.count; index += 1) {
            ta_expression *expression = ta_vec_at(&compiler.expressions, index);
            if (expression->kind == TA_EXPRESSION_HOST && expression->as.host.v->release != NULL) {
                expression->as.host.v->release(expression->as.host.self, expression->as.host.value,
                                               params->allocator);
            }
        }
        definition->expression_count = 0;
        result = result == TA_OK ? sealed : result;
    }
    ta_compiler_destroy(&compiler);

    if (result == TA_OK) {
        result = params->datamodel.v->prepare_definition(
            params->datamodel.self,
            &(ta_datamodel_prepare_definition_params){
                .expressions = definition->expressions,
                .expression_count = definition->expression_count,
                .allocator = params->allocator,
                .shared = &definition->datamodel_shared,
            });
        definition->datamodel_prepared = result == TA_OK;
    }
    if (result == TA_OK) {
        definition->datamodel_instance_size =
            params->datamodel.v->instance_size(params->datamodel.self, definition->datamodel_shared);
        definition->datamodel_instance_alignment =
            params->datamodel.v->instance_alignment(params->datamodel.self, definition->datamodel_shared);
        if (definition->datamodel_instance_alignment == 0) {
            definition->datamodel_instance_alignment = 1;
        }
    }
    if (result != TA_OK) {
        ta_definition_destroy(definition);
        return result;
    }
    *params->out = definition;
    return TA_OK;
}


/*
 * Rebuilds the author-level ta_scxml_spec of a compiled definition. The
 * walk mirrors the compiler (builder.c) so that recompiling the result hashes
 * the same input in the same order: every list is rebuilt in the order the
 * compiler recorded it, and input the compiler normalizes comes from the
 * authored text the definition keeps (delay literals, transition event
 * descriptors, authored initial targets, the <scxml> binding and datamodel).
 */
typedef struct {
    const ta_system *system;
    const ta_definition *definition;
    ta_arena *arena;
    /* The first failure; later allocations are skipped. */
    ta_result result;
} ta_exporter;

/* Non-null text for an empty literal, which the RPC codecs keep apart from absent. */
static const char ta_export_empty[] = "";

/*
 * Allocates `count` zeroed list elements plus the zero terminator. An empty
 * list is null.
 */
static void *ta_export_list(ta_exporter *exporter, const ta_tree_list_layout *layout, uint32_t count) {
    if (count == 0 || exporter->result != TA_OK) {
        return NULL;
    }
    size_t size = layout->element_size * ((size_t)count + 1u);
    void *items = ta_arena_alloc(exporter->arena, size, layout->element_alignment);
    if (items == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    ta_mem_zero(items, size);
    return items;
}

static ta_str ta_export_atom(const ta_exporter *exporter, ta_atom atom) {
    ta_str text = ta_atom_text(&exporter->system->atoms, atom);
    return text.ptr == NULL ? (ta_str){ .ptr = ta_export_empty, .len = 0 } : text;
}

static ta_str ta_export_state_id(const ta_exporter *exporter, ta_state_id state) {
    return ta_export_atom(exporter, exporter->definition->states[state].name);
}

static ta_str ta_export_authored(const ta_exporter *exporter, ta_authored_text_id text) {
    if (text == TA_AUTHORED_TEXT_NONE) {
        return (ta_str){0};
    }
    return exporter->definition->authored_texts[text];
}

/* A dotted event name. The compiler rejects empty segments, so joining them restores the text. */
static ta_str ta_export_event_name(ta_exporter *exporter, ta_event_name_id name) {
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&exporter->system->event_names, name, segments);
    if (count == 1) {
        return ta_export_atom(exporter, segments[0]);
    }
    size_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&exporter->system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    char *text = exporter->result == TA_OK ? ta_arena_alloc(exporter->arena, length + 1u, 1) : NULL;
    if (text == NULL) {
        exporter->result = exporter->result == TA_OK ? TA_ERR_NOMEM : exporter->result;
        return (ta_str){ .ptr = ta_export_empty, .len = 0 };
    }
    size_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        ta_str segment = ta_atom_text(&exporter->system->atoms, segments[index]);
        if (index != 0) {
            text[at++] = '.';
        }
        if (segment.len != 0) {
            ta_mem_copy(text + at, segment.ptr, segment.len);
        }
        at += segment.len;
    }
    text[at] = '\0';
    return (ta_str){ .ptr = text, .len = (uint32_t)length };
}

static ta_expression ta_export_expression(const ta_exporter *exporter, ta_expr_id expr) {
    return exporter->definition->expressions[expr];
}

static ta_bytes ta_export_content(const ta_exporter *exporter, ta_content_id content) {
    ta_bytes bytes = exporter->definition->contents[content].bytes;
    return bytes.ptr == NULL ? (ta_bytes){ .ptr = (const uint8_t *)ta_export_empty, .len = 0 } : bytes;
}

static ta_scxml_value_spec ta_export_value(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_value_spec out = {0};
    switch (source->kind) {
        case TA_VALUE_SOURCE_EXPR:
            out.kind = TA_SCXML_VALUE_EXPR;
            out.as.expr = ta_export_expression(exporter, source->as.expr);
            break;
        case TA_VALUE_SOURCE_LOCATION:
            out.kind = TA_SCXML_VALUE_LOCATION;
            out.as.location = ta_export_expression(exporter, source->as.location);
            break;
        case TA_VALUE_SOURCE_CONTENT:
            out.kind = TA_SCXML_VALUE_CONTENT;
            out.as.content = ta_export_content(exporter, source->as.content);
            break;
        default:
            break;
    }
    return out;
}

/* A `type` / `typeexpr` or `sendid` / `sendidexpr` pair. */
static ta_scxml_string_spec ta_export_text_source(const ta_exporter *exporter, const ta_text_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_TEXT_SOURCE_ATOM) {
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.atom);
    } else if (source->kind == TA_TEXT_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

/* A send `target` or invoke `src`: the compiler stores literal text as content. */
static ta_scxml_string_spec ta_export_literal_source(const ta_exporter *exporter, const ta_value_source *source) {
    ta_scxml_string_spec out = {0};
    if (source->kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes bytes = ta_export_content(exporter, source->as.content);
        out.kind = TA_SCXML_STRING_LITERAL;
        out.as.literal = (ta_str){ .ptr = (const char *)bytes.ptr, .len = bytes.len };
    } else if (source->kind == TA_VALUE_SOURCE_EXPR) {
        out.kind = TA_SCXML_STRING_EXPR;
        out.as.expr = ta_export_expression(exporter, source->as.expr);
    }
    return out;
}

static ta_scxml_id_spec ta_export_id(const ta_exporter *exporter, const ta_id_source *source) {
    ta_scxml_id_spec out = {0};
    if (source->kind == TA_ID_SOURCE_LITERAL) {
        out.kind = TA_SCXML_ID_LITERAL;
        out.as.literal = ta_export_atom(exporter, source->as.literal);
    } else if (source->kind == TA_ID_SOURCE_LOCATION) {
        out.kind = TA_SCXML_ID_LOCATION;
        out.as.location = ta_export_expression(exporter, source->as.location);
    }
    return out;
}

static const ta_scxml_param_spec *ta_export_params(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_param_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_param_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_param_def *param = &exporter->definition->params[span.offset + index];
        items[index] = (ta_scxml_param_spec){
            .name = ta_export_atom(exporter, param->name),
            .value = ta_export_value(exporter, &param->value),
        };
    }
    return items;
}

static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block);

static void ta_export_send(ta_exporter *exporter, const ta_send_action *send, ta_scxml_send_spec *out) {
    out->id = ta_export_id(exporter, &send->id);
    out->type = ta_export_text_source(exporter, &send->type);
    if (send->event.kind == TA_EVENT_SOURCE_EXPR) {
        out->event.kind = TA_SCXML_EVENT_EXPR;
        out->event.as.expr = ta_export_expression(exporter, send->event.as.expr);
    } else {
        out->event.kind = TA_SCXML_EVENT_NAME;
        out->event.as.name = ta_export_event_name(exporter, send->event.as.name);
    }
    out->target = ta_export_literal_source(exporter, &send->target);
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        out->delay.kind = TA_SCXML_STRING_LITERAL;
        out->delay.as.literal = ta_export_authored(exporter, send->delay.text);
    } else if (send->delay.kind == TA_DELAY_SOURCE_EXPR) {
        out->delay.kind = TA_SCXML_STRING_EXPR;
        out->delay.as.expr = ta_export_expression(exporter, send->delay.as.expr);
    }
    out->data = ta_export_value(exporter, &send->data);
    out->params = ta_export_params(exporter, send->params);
}

static void ta_export_action(ta_exporter *exporter, const ta_action_def *action, ta_scxml_action_spec *out) {
    switch (action->kind) {
        case TA_ACTION_RAISE:
            out->kind = TA_SCXML_ACTION_RAISE;
            out->as.raise.event = ta_export_event_name(exporter, action->as.raise.event);
            out->as.raise.data = ta_export_value(exporter, &action->as.raise.data);
            break;
        case TA_ACTION_SEND:
            out->kind = TA_SCXML_ACTION_SEND;
            ta_export_send(exporter, &action->as.send, &out->as.send);
            break;
        case TA_ACTION_CANCEL:
            out->kind = TA_SCXML_ACTION_CANCEL;
            out->as.cancel.sendid = ta_export_text_source(exporter, &action->as.cancel.sendid);
            break;
        case TA_ACTION_ASSIGN:
            out->kind = TA_SCXML_ACTION_ASSIGN;
            out->as.assign.location = ta_export_expression(exporter, action->as.assign.location);
            out->as.assign.value = ta_export_value(exporter, &action->as.assign.value);
            break;
        case TA_ACTION_SCRIPT:
            out->kind = TA_SCXML_ACTION_SCRIPT;
            out->as.script.script = ta_export_expression(exporter, action->as.script);
            break;
        case TA_ACTION_LOG:
            out->kind = TA_SCXML_ACTION_LOG;
            if (action->as.log.has_label) {
                out->as.log.label = ta_export_atom(exporter, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                out->as.log.expr = ta_export_expression(exporter, action->as.log.expr);
            }
            break;
        case TA_ACTION_IF:
            out->kind = TA_SCXML_ACTION_IF;
            out->as.conditional.condition = ta_export_expression(exporter, action->as.conditional.condition);
            out->as.conditional.then_actions = ta_export_actions(exporter, action->as.conditional.then_actions);
            out->as.conditional.else_actions = ta_export_actions(exporter, action->as.conditional.else_actions);
            break;
        case TA_ACTION_FOREACH:
            out->kind = TA_SCXML_ACTION_FOREACH;
            out->as.foreach.array = ta_export_expression(exporter, action->as.foreach.array);
            out->as.foreach.item = ta_export_atom(exporter, action->as.foreach.item);
            if (action->as.foreach.has_index) {
                out->as.foreach.index = ta_export_atom(exporter, action->as.foreach.index);
            }
            out->as.foreach.body = ta_export_actions(exporter, action->as.foreach.body_actions);
            break;
        default:
            /* The compiler accepts neither <invoke> nor extensions as executable content. */
            exporter->result = exporter->result == TA_OK ? TA_ERR_UNSUPPORTED : exporter->result;
            break;
    }
}

static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block) {
    ta_scxml_action_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_action_spec), block.length);
    for (uint32_t index = 0; items != NULL && index < block.length; index += 1) {
        ta_export_action(exporter, &exporter->definition->action_defs[block.offset + index], &items[index]);
    }
    return items;
}

/* One <onentry>/<onexit> handler per element; an empty handler keeps a non-null, empty action list. */
static const ta_scxml_handler_spec *ta_export_handlers(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_handler_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_handler_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        ta_span32 block = exporter->definition->handlers[span.offset + index];
        const ta_scxml_action_spec *actions = ta_export_actions(exporter, block);
        if (actions == NULL && exporter->result == TA_OK) {
            actions = ta_arena_alloc(exporter->arena, sizeof(ta_scxml_action_spec), _Alignof(ta_scxml_action_spec));
            if (actions == NULL) {
                exporter->result = TA_ERR_NOMEM;
            } else {
                ta_mem_zero((void *)actions, sizeof(ta_scxml_action_spec));
            }
        }
        items[index].actions = actions;
    }
    return items;
}

static const ta_str *ta_export_targets(ta_exporter *exporter, ta_span32 span) {
    ta_str *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_str), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        items[index] = ta_export_state_id(exporter, exporter->definition->targets[span.offset + index]);
    }
    return items;
}

/* Null when the author wrote neither targets nor actions: the compiler then takes the first child. */
static const ta_scxml_initial_spec *ta_export_initial(ta_exporter *exporter, ta_initial_transition_id id) {
    if (id == TA_INITIAL_TRANSITION_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_initial_transition_def *initial = &exporter->definition->initial_transitions[id];
    if (!initial->authored_targets && initial->actions.length == 0) {
        return NULL;
    }
    ta_scxml_initial_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_initial_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_initial_spec){
        .targets = initial->authored_targets ? ta_export_targets(exporter, initial->targets) : NULL,
        .actions = ta_export_actions(exporter, initial->actions),
    };
    return out;
}

static const ta_scxml_transition_spec *ta_export_transitions(ta_exporter *exporter, ta_span32 span) {
    ta_scxml_transition_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_transition_spec), span.length);
    for (uint32_t index = 0; items != NULL && index < span.length; index += 1) {
        const ta_transition_def *transition = &exporter->definition->transitions[span.offset + index];
        items[index] = (ta_scxml_transition_spec){
            .kind = transition->type == TA_TRANSITION_INTERNAL ? TA_SCXML_TRANSITION_INTERNAL
                                                                : TA_SCXML_TRANSITION_EXTERNAL,
            .event = ta_export_authored(exporter, transition->event_text),
            .targets = ta_export_targets(exporter, transition->targets),
            .actions = ta_export_actions(exporter, transition->actions),
        };
        if (transition->condition != UINT32_MAX) {
            items[index].condition = ta_export_expression(exporter, transition->condition);
        }
    }
    return items;
}

/* The <data> children of `owner` (TA_STATE_NONE: the <scxml> element), in document order. */
static const ta_scxml_data_spec *ta_export_data(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->data_decl_count; index += 1) {
        count += definition->data_decls[index].owner_state == owner ? 1u : 0u;
    }
    ta_scxml_data_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_data_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->data_decl_count; index += 1) {
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner) {
            continue;
        }
        items[at].name = ta_export_atom(exporter, decl->name);
        if (decl->initializer != TA_INITIALIZER_NONE) {
            items[at].initializer = ta_export_value(exporter, &definition->initializers[decl->initializer].value);
        }
        at += 1;
    }
    return items;
}

static const ta_scxml_invoke_spec *ta_export_invokes(ta_exporter *exporter, ta_state_id owner) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (uint32_t index = 0; index < definition->invoke_decl_count; index += 1) {
        count += definition->invokes[index].owner == owner ? 1u : 0u;
    }
    ta_scxml_invoke_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_invoke_spec), count);
    uint32_t at = 0;
    for (uint32_t index = 0; items != NULL && index < definition->invoke_decl_count; index += 1) {
        const ta_invoke_decl *invoke = &definition->invokes[index];
        if (invoke->owner != owner) {
            continue;
        }
        items[at] = (ta_scxml_invoke_spec){
            .kind = TA_SCXML_INVOKE,
            .id = ta_export_id(exporter, &invoke->id),
            .type = ta_export_text_source(exporter, &invoke->type),
            .src = ta_export_literal_source(exporter, &invoke->src),
            .data = ta_export_value(exporter, &invoke->data),
            .autoforward = invoke->autoforward,
            .params = ta_export_params(exporter, invoke->params),
            .finalize = ta_export_actions(exporter, invoke->finalize_actions),
        };
        at += 1;
    }
    return items;
}

static const ta_scxml_done_data_spec *ta_export_done_data(ta_exporter *exporter, ta_done_data_id id) {
    if (id == TA_DONE_DATA_NONE || exporter->result != TA_OK) {
        return NULL;
    }
    const ta_done_data_def *done = &exporter->definition->done_data[id];
    ta_scxml_done_data_spec *out = ta_arena_alloc(exporter->arena, sizeof(*out), _Alignof(ta_scxml_done_data_spec));
    if (out == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    *out = (ta_scxml_done_data_spec){
        .content = ta_export_value(exporter, &done->content),
        .params = ta_export_params(exporter, done->params),
    };
    return out;
}

static ta_scxml_state_kind ta_export_state_kind(ta_state_kind kind) {
    switch (kind) {
        case TA_STATE_ATOMIC: return TA_SCXML_STATE_ATOMIC;
        case TA_STATE_COMPOUND: return TA_SCXML_STATE_COMPOUND;
        case TA_STATE_PARALLEL: return TA_SCXML_STATE_PARALLEL;
        case TA_STATE_FINAL: return TA_SCXML_STATE_FINAL;
        case TA_STATE_HISTORY_SHALLOW: return TA_SCXML_STATE_HISTORY_SHALLOW;
        case TA_STATE_HISTORY_DEEP: return TA_SCXML_STATE_HISTORY_DEEP;
        default: return TA_SCXML_STATE_NONE;
    }
}

static const ta_scxml_state_spec *ta_export_states(ta_exporter *exporter, const ta_state_id *ids, uint32_t count);

static void ta_export_state(ta_exporter *exporter, ta_state_id id, ta_scxml_state_spec *out) {
    const ta_definition *definition = exporter->definition;
    const ta_state_def *state = &definition->states[id];
    *out = (ta_scxml_state_spec){
        .kind = ta_export_state_kind(state->kind),
        .id = ta_export_state_id(exporter, id),
        .states = ta_export_states(exporter, definition->children + state->children.offset, state->children.length),
        .initial = state->kind == TA_STATE_COMPOUND ? ta_export_initial(exporter, state->initial) : NULL,
        .transitions = ta_export_transitions(exporter, state->transitions),
        .onentry = ta_export_handlers(exporter, state->onentry_handlers),
        .onexit = ta_export_handlers(exporter, state->onexit_handlers),
        .data = ta_export_data(exporter, id),
        .invokes = ta_export_invokes(exporter, id),
        .done_data = ta_export_done_data(exporter, state->done_data),
    };
}

static const ta_scxml_state_spec *ta_export_states(ta_exporter *exporter, const ta_state_id *ids, uint32_t count) {
    ta_scxml_state_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), count);
    for (uint32_t index = 0; items != NULL && index < count; index += 1) {
        ta_export_state(exporter, ids[index], &items[index]);
    }
    return items;
}

/*
 * Top-level states in document order. The compiler numbers states in
 * pre-order and records the roots first in `children`.
 */
static const ta_scxml_state_spec *ta_export_roots(ta_exporter *exporter) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += definition->states[state].parent == TA_STATE_NONE ? 1u : 0u;
    }
    return ta_export_states(exporter, definition->children, count);
}

TA_API ta_result ta_definition_export(const ta_system *system, const ta_definition_export_params *params) {
    if (system == NULL || params == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    ta_exporter exporter = { .system = system, .definition = definition, .arena = params->arena };
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(*spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = (ta_scxml_spec){
        .name = definition->document_name == TA_ATOM_NONE ? (ta_str){0}
                                                          : ta_export_atom(&exporter, definition->document_name),
        .datamodel = definition->authored_datamodel,
        .identity = definition->fingerprinted ? NULL : &definition->identity,
        .binding = definition->authored_binding,
        .states = ta_export_roots(&exporter),
        .initial = ta_export_initial(&exporter, definition->root_initial),
        .data = ta_export_data(&exporter, TA_STATE_NONE),
    };
    if (definition->global_script_actions.length == 1) {
        const ta_action_def *script = &definition->action_defs[definition->global_script_actions.offset];
        spec->script = ta_export_expression(&exporter, script->as.script);
    }
    if (exporter.result != TA_OK) {
        return exporter.result;
    }
    *params->out = spec;
    return TA_OK;
}


TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t generation = (uint32_t)(id >> 32);
    if (slot >= system->actors.count) {
        return NULL;
    }
    const ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
    return entry->generation == generation ? entry->actor : NULL;
}

TA_PRIVATE bool ta_actor_state_active(const ta_actor *actor, ta_state_id state) {
    const ta_definition *definition = actor->definition;
    if (state >= definition->state_count) {
        return false;
    }
    if (actor->live_active != NULL) {
        return ta_bits_get(actor->live_active, state);
    }
    if (ta_definition_is_atomic(definition, state)) {
        return ta_configuration_contains_atomic(&actor->configuration, state);
    }
    for (ta_state_id leaf = 0; leaf < definition->state_count; leaf += 1) {
        if (ta_configuration_contains_atomic(&actor->configuration, leaf) &&
            ta_definition_is_descendant(definition, leaf, state)) {
            return true;
        }
    }
    return false;
}

static ta_result ta_actor_claim_slot(ta_system *system, ta_actor *actor, ta_actor_id *out) {
    uint32_t slot = system->actors.count;
    for (uint32_t index = 0; index < system->actors.count; index += 1) {
        const ta_actor_slot *entry = ta_vec_at(&system->actors, index);
        if (entry->actor == NULL) {
            slot = index;
            break;
        }
    }
    if (slot == system->actors.count) {
        ta_actor_slot fresh = { .generation = 1 };
        ta_result result = ta_vec_push(&system->actors, &fresh);
        if (result != TA_OK) {
            return result;
        }
    }
    ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
    entry->actor = actor;
    *out = ((ta_actor_id)entry->generation << 32) | slot;
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * retired definition frees the definition, so nothing below may touch it.
 */
static void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    if (actor->datamodel_instance != NULL) {
        ta_mem_free(system->allocator, actor->datamodel_instance,
                    &(ta_allocator_free_params){ .size = ta_definition_instance_bytes(definition),
                                                 .alignment = definition->datamodel_instance_alignment });
    }
    ta_mem_free(system->allocator, actor->session_storage,
                &(ta_allocator_free_params){ .size = actor->session_bytes, .alignment = _Alignof(uint64_t) });
    /* A named actor owns the text of its address; a runtime ref owns nothing. */
    ta_address_release(&actor->record.self, system->allocator);
    ta_interpreter_release_actor_state(system, actor);
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor), .alignment = _Alignof(ta_actor) });
    ta_definition *owner = ta_system_definition(system, definition->id);
    owner->actor_count -= 1u;
    if (owner->retired && owner->actor_count == 0) {
        ta_system_free_definition(system, owner);
    }
}

/*
 * Takes the address for the actor: copies its text into actor-owned storage
 * and claims it in the host directory. claim is a compare-and-swap against
 * "no entry", so an address another actor holds — resident or unloaded —
 * answers TA_ERR_STALE and this spawn leaves nothing behind.
 */
static ta_result ta_actor_claim_address(ta_system *system,
                                        ta_actor *actor,
                                        const ta_actor_address *address) {
    if (system->directory.v == NULL || system->directory.v->claim == NULL) {
        /* A logical address is only meaningful against a host directory. */
        return TA_ERR_UNSUPPORTED;
    }
    if (address->family.name.ptr == NULL || address->family.name.len == 0 ||
        address->name.ptr == NULL || address->name.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor_ref self = { .kind = TA_ACTOR_REF_NAMED, .as.named = *address };
    ta_result result = ta_address_copy(&self, system->allocator);
    if (result != TA_OK) {
        return result;
    }
    result = system->directory.v->claim(
        system->directory.self,
        &(ta_actor_directory_claim_params){
            .address = &self.as.named,
            .entry = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
    if (result != TA_OK) {
        ta_address_release(&self, system->allocator);
        return result;
    }
    /* From here the actor answers to its address: every origin core writes
     * for it is the address, so a reply outlives this runtime ID. */
    actor->record.self = self;
    return TA_OK;
}

/* Frees the slot an actor was given, so a failed spawn reuses it. */
static void ta_actor_release_slot(ta_system *system, ta_actor_id id) {
    ta_actor_slot *entry = ta_vec_at(&system->actors, (uint32_t)(id & 0xffffffffu));
    entry->actor = NULL;
    entry->generation += 1u;
}

/*
 * Gives up the route of a named actor that is being destroyed. remove
 * compares against this actor's own entry, so an incumbent whose address has
 * already been taken over by an activated replacement removes nothing: the
 * successor keeps the route. A failed comparison is that case, not an error.
 */
static void ta_actor_release_address(ta_system *system, const ta_actor *actor) {
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED || !actor->published) {
        return;
    }
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return;
    }
    (void)system->directory.v->remove(
        system->directory.self,
        &(ta_actor_directory_remove_params){
            .address = &actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
}

/*
 * Resolves a logical address through the host directory. An unloaded actor
 * exists but has no runtime ID, which is TA_ERR_UNLOADED and leaves *out
 * alone; only a resident entry writes one.
 */
TA_API ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params) {
    if (system == NULL || params == NULL || params->address == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (system->directory.v == NULL || system->directory.v->resolve == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
    ta_result result = system->directory.v->resolve(
        system->directory.self,
        &(ta_actor_directory_resolve_params){ .address = params->address, .out = &entry });
    if (result != TA_OK) {
        return result;
    }
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        *params->out = entry.actor;
        return TA_OK;
    }
    return entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_ERR_UNLOADED : TA_ERR_NOT_FOUND;
}

TA_API ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor *actor = ta_mem_alloc(system->allocator, sizeof(ta_actor), _Alignof(ta_actor));
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = ta_definition_session_bytes(definition);
    actor->session_storage = ta_mem_alloc(system->allocator, actor->session_bytes, _Alignof(uint64_t));
    if (actor->session_storage == NULL) {
        ta_actor_free(system, actor);
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(actor->session_storage, actor->session_bytes);
    unsigned char *storage = actor->session_storage;
    actor->configuration = (ta_configuration){ .storage = storage, .layout = &definition->configuration };
    actor->late_initialized = (uint64_t *)(storage + config_bytes);
    actor->history = (ta_history_state){ .storage = storage + config_bytes * 2u, .layout = &definition->history };

    uint32_t instance_size = definition->datamodel_instance_size;
    actor->datamodel_instance = ta_mem_alloc(system->allocator, ta_definition_instance_bytes(definition),
                                             definition->datamodel_instance_alignment);
    if (actor->datamodel_instance == NULL) {
        ta_actor_free(system, actor);
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(actor->datamodel_instance, instance_size);

    if (params->parent != NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            ta_actor_free(system, actor);
            return TA_ERR_NOMEM;
        }
        *actor->sparse = (ta_actor_sparse_state){ .parent = *params->parent, .has_parent = true };
        ta_vec_init(&actor->sparse->internal_events, system->allocator, TA_VEC_LAYOUT(ta_queued_event));
    }

    ta_actor_id id = 0;
    ta_result result = ta_actor_claim_slot(system, actor, &id);
    if (result != TA_OK) {
        ta_actor_free(system, actor);
        return result;
    }
    actor->record = (ta_actor_record){
        .id = id,
        .definition = params->definition,
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
    result = definition->datamodel.v->init_instance(
        definition->datamodel.self,
        &(ta_datamodel_init_instance_params){
            .shared = definition->datamodel_shared,
            .instance = actor->datamodel_instance,
            .binding = params->datamodel_binding,
            .host = &actor->host,
        });
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    actor->datamodel_initialized = true;
    /* The claim is the last fallible step: bootstrap has not run yet, so a
     * refused address leaves no trace of this spawn anywhere. */
    if (params->address != NULL) {
        result = ta_actor_claim_address(system, actor, params->address);
        if (result != TA_OK) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return result;
        }
    }
    if (params->parent != NULL) {
        /* Invoke: the invoking parent routes #_<invokeid> and autoforwarding to this child. */
        ta_invoke_attach_child(system, params->parent, id);
    }
    *params->out = id;
    return TA_OK;
}

TA_API ta_result ta_actor_deliver(ta_system *system, ta_actor_id id, const ta_event *event) {
    if (event == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!ta_event_name_valid(&system->event_names, event->name) || event->name == TA_EVENT_NAME_ROOT) {
        return TA_ERR_INVALID;
    }
    if (actor->finished) {
        return TA_ERR_INVALID;
    }
    if (actor->macrostep || actor->has_external_event) {
        return TA_ERR_BUSY;
    }
    ta_event copy;
    ta_result result = ta_event_clone(event, &copy);
    if (result != TA_OK) {
        return result;
    }
    actor->external_event = copy;
    actor->has_external_event = true;
    actor->macrostep = true;
    actor->record.residency = TA_ACTOR_RUNNING;
    return TA_OK;
}

TA_API ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params) {
    if (params == NULL || params->status == NULL || params->budget == 0) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    return ta_interpreter_resume(system, actor, params);
}

TA_API ta_result ta_actor_destroy(ta_system *system, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* Sends already submitted stay queued for their receivers. */
    ta_timer_cancel_unsubmitted(&system->timers, id);
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    if (actor->has_external_event) {
        return &actor->external_event;
    }
    if (actor->has_current_event && actor->current_event.type == TA_EVENT_EXTERNAL) {
        return &actor->current_event;
    }
    return NULL;
}

static size_t ta_vec_bytes(const ta_vec *vec) {
    return (size_t)vec->capacity * vec->element_size;
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = sizeof(ta_actor) + actor->session_bytes;
    if (actor->datamodel_instance != NULL) {
        total += ta_definition_instance_bytes(actor->definition);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->internal_events);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += (size_t)sparse->invoke_capacity * (sizeof(ta_active_invoke) + sizeof(ta_invoke_meta));
    }
    return total;
}

TA_API ta_result ta_actor_inspect(ta_system *system, ta_actor_id id, ta_actor_inspection *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t active = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        active += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *out = (ta_actor_inspection){
        .actor = id,
        .self = actor->record.self,
        .definition_id = definition->id,
        .definition = definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .active_state_count = active,
        .macrostep_in_progress = actor->macrostep,
        .current_event = ta_actor_paused_event(actor),
        .delayed_send_count = ta_timer_count_owner(&system->timers, id),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
    };
    if (actor->sparse != NULL) {
        out->internal_events = actor->sparse->internal_events.items;
        out->internal_count = actor->sparse->internal_events.count;
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
        out->has_completion = actor->finished;
        out->completion = actor->finished ? &actor->sparse->completion : NULL;
    }
    return TA_OK;
}

TA_API ta_result ta_actor_datamodel_root(ta_system *system, ta_actor_id id, ta_value *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    *out = (ta_value){0};
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->inspect == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    ta_result result = v->inspect(definition->datamodel.self,
                                  &(ta_datamodel_inspect_params){
                                      .shared = definition->datamodel_shared,
                                      .instance = actor->datamodel_instance,
                                      .host = &actor->host,
                                      .out = out,
                                  });
    if (result != TA_OK) {
        *out = (ta_value){0};
    }
    return result;
}

TA_API ta_result ta_system_visit_actors(ta_system *system, ta_actor_inspector inspector) {
    if (system == NULL || inspector.visit == NULL) {
        return TA_ERR_INVALID;
    }
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        const ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
        if (entry->actor == NULL) {
            continue;
        }
        ta_actor_inspection inspection;
        ta_result result = ta_actor_inspect(system, entry->actor->record.id, &inspection);
        if (result == TA_OK) {
            result = inspector.visit(inspector.self, &inspection);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

TA_API ta_result ta_actor_active_states(ta_system *system, const ta_actor_active_states_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_actor_state_active(actor, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_actor_state_active(actor, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

TA_API ta_result ta_actor_history(ta_system *system, const ta_actor_history_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    const ta_definition *definition = actor->definition;
    if (params->history >= definition->state_count ||
        definition->states[params->history].history_slot == UINT32_MAX) {
        return TA_ERR_INVALID;
    }
    const uint64_t *words = ta_history_slot_words(&actor->history, definition->states[params->history].history_slot);
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += ta_bits_get(words, state) ? 1u : 0u;
    }
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->states == NULL)) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(words, state)) {
            params->states[at++] = state;
        }
    }
    return TA_OK;
}

TA_API ta_result ta_actor_delayed_sends(ta_system *system, const ta_actor_delayed_sends_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    if (ta_actor_lookup(system, params->actor) == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t count = ta_timer_count_owner(&system->timers, params->actor);
    *params->count = count;
    if (count > params->capacity || (count != 0 && params->sends == NULL)) {
        return TA_ERR_NOMEM;
    }
    /* Selection by (due, id) keeps the documented order without sorting the heap. */
    ta_time last_due = 0;
    ta_timer_id last_id = 0;
    for (uint32_t at = 0; at < count; at += 1) {
        const ta_delayed_send *best = NULL;
        for (uint32_t index = 0; index < system->timers.count; index += 1) {
            const ta_delayed_send *send = &system->timers.items[index];
            bool after = at == 0 || send->due > last_due || (send->due == last_due && send->id > last_id);
            bool better = best == NULL || send->due < best->due || (send->due == best->due && send->id < best->id);
            if (send->owner == params->actor && after && better) {
                best = send;
            }
        }
        last_due = best->due;
        last_id = best->id;
        params->sends[at] = (ta_delayed_send_info){
            .send_id = best->send_id,
            .due = best->due,
            .event = best->event,
            .io_type = best->io_type,
            .target = best->target,
            .data = best->data,
        };
    }
    return TA_OK;
}

TA_API ta_result ta_actor_get_completion(ta_system *system, ta_actor_id id, ta_actor_completion *out) {
    if (out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!actor->finished) {
        return TA_ERR_BUSY;
    }
    *out = (ta_actor_completion){ .has_data = false };
    if (actor->sparse == NULL || !actor->sparse->completion.has_data) {
        return TA_OK;
    }
    ta_result result = ta_value_clone(&actor->sparse->completion.data, &out->data);
    out->has_data = result == TA_OK;
    return result;
}


/*
 * Private structural image. Every pointer is image-owned. Values embedded in
 * events, timers, continuation, and completion are retained, never traversed.
 */
struct ta_session_image {
    ta_allocator allocator;
    ta_actor_id source_actor;
    uint64_t source_epoch;
    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;
    void *configuration_storage;
    uint32_t configuration_bytes;
    void *history_storage;
    uint32_t history_bytes;
    ta_continuation continuation;
    ta_queued_event *internal_events;
    uint32_t internal_count;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    ta_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    bool has_parent;
    ta_actor_parent parent;
    ta_actor_completion completion;
};



#define TA_SCXML_IO_URI "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

static ta_str ta_cstr(const char *text) {
    ta_str out = { .ptr = text, .len = 0 };
    while (text[out.len] != '\0') {
        out.len += 1;
    }
    return out;
}

TA_API ta_result ta_system_create(const ta_system_config *config, ta_system **out) {
    if (config == NULL || out == NULL || config->allocator.v == NULL ||
        config->allocator.v->alloc == NULL || config->allocator.v->free == NULL) {
        return TA_ERR_INVALID;
    }
    ta_system *system = ta_mem_alloc(config->allocator, sizeof(ta_system), _Alignof(ta_system));
    if (system == NULL) {
        return TA_ERR_NOMEM;
    }
    *system = (ta_system){ .allocator = config->allocator };
    ta_clock_set(&system->clock, config->initial_time);
    ta_vec_init(&system->actors, config->allocator, TA_VEC_LAYOUT(ta_actor_slot));
    ta_vec_init(&system->definitions, config->allocator, TA_VEC_LAYOUT(ta_definition_slot));
    system->free_definition_slot = UINT32_MAX;
    ta_vec_init(&system->io_processors, config->allocator, TA_VEC_LAYOUT(ta_io_registration));
    ta_vec_init(&system->invokers, config->allocator, TA_VEC_LAYOUT(ta_invoker_registration));
    ta_result result = ta_atom_table_init(&system->atoms, config->allocator);
    if (result != TA_OK) {
        ta_mem_free(config->allocator, system,
                    &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
        return result;
    }
    result = ta_event_name_table_init(&system->event_names, config->allocator, &system->atoms);
    if (result == TA_OK) {
        result = ta_timer_heap_init(&system->timers, config->allocator);
    }
    if (result == TA_OK) {
        result = ta_arena_init(&system->scratch, config->allocator, 4096);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr(TA_SCXML_IO_URI), &system->scxml_io_type);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr("scxml"), &system->scxml_io_alias);
    }
    if (result == TA_OK) {
        result = ta_atom_intern(&system->atoms, ta_cstr("http://www.w3.org/TR/scxml/"),
                                &system->scxml_invoke_type);
    }
    if (result != TA_OK) {
        ta_system_destroy(system);
        return result;
    }
    *out = system;
    return TA_OK;
}

TA_API void ta_system_destroy(ta_system *system) {
    if (system == NULL) {
        return;
    }
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ((ta_actor_id)entry->generation << 32) | slot);
        }
    }
    /* Every actor is gone, so every definition goes, retired or not. */
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        if (entry->definition != NULL) {
            ta_definition_destroy(entry->definition);
            entry->definition = NULL;
        }
    }
    ta_vec_destroy(&system->definitions);
    ta_timer_heap_destroy(&system->timers);
    ta_vec_destroy(&system->actors);
    ta_vec_destroy(&system->io_processors);
    ta_vec_destroy(&system->invokers);
    ta_arena_destroy(&system->scratch);
    ta_event_name_table_destroy(&system->event_names);
    ta_atom_table_destroy(&system->atoms);
    ta_allocator allocator = system->allocator;
    ta_mem_free(allocator, system,
                &(ta_allocator_free_params){ .size = sizeof(ta_system), .alignment = _Alignof(ta_system) });
}

TA_API ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    system->directory = directory;
    return TA_OK;
}

TA_API ta_result ta_system_set_log_sink(ta_system *system, ta_log_sink sink) {
    if (system == NULL || (sink.v != NULL && sink.v->write == NULL)) {
        return TA_ERR_INVALID;
    }
    system->log_sink = sink;
    return TA_OK;
}

TA_API ta_result ta_system_set_external_event_router(ta_system *system, ta_external_event_router router) {
    if (system == NULL || (router.v != NULL && router.v->submit == NULL)) {
        return TA_ERR_INVALID;
    }
    system->external_events = router;
    return TA_OK;
}

TA_API ta_result ta_system_register_definition(ta_system *system, ta_definition *definition, ta_definition_id *out) {
    if (system == NULL || definition == NULL || out == NULL || definition->system != system ||
        definition->registered) {
        return TA_ERR_INVALID;
    }
    uint32_t slot = system->free_definition_slot;
    if (slot == UINT32_MAX) {
        ta_definition_slot fresh = { .generation = 1, .next_free = UINT32_MAX };
        ta_result result = ta_vec_push(&system->definitions, &fresh);
        if (result != TA_OK) {
            return result;
        }
        slot = system->definitions.count - 1u;
    }
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    if (slot == system->free_definition_slot) {
        system->free_definition_slot = entry->next_free;
    }
    entry->definition = definition;
    entry->next_free = UINT32_MAX;
    definition->id = ((ta_definition_id)entry->generation << 32) | slot;
    definition->registered = true;
    definition->registration_serial = system->next_definition_serial++;
    *out = definition->id;
    return TA_OK;
}

TA_PRIVATE ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = (uint32_t)(id & 0xffffffffu);
    uint32_t generation = (uint32_t)(id >> 32);
    if (slot >= system->definitions.count) {
        return NULL;
    }
    const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    return entry->generation == generation ? entry->definition : NULL;
}

TA_PRIVATE void ta_system_free_definition(ta_system *system, ta_definition *definition) {
    uint32_t slot = (uint32_t)(definition->id & 0xffffffffu);
    ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
    /* The slot's next occupant gets a new generation, so this ID goes stale. */
    entry->definition = NULL;
    entry->generation += 1u;
    entry->next_free = system->free_definition_slot;
    system->free_definition_slot = slot;
    ta_definition_destroy(definition);
}

TA_API ta_result ta_system_retire_definition(ta_system *system, ta_definition_id id) {
    if (system == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, id);
    if (definition == NULL) {
        /*
         * A slot's generation advances only when its definition is freed, so
         * an older generation of an existing slot was retired and freed.
         */
        uint32_t slot = (uint32_t)(id & 0xffffffffu);
        uint32_t generation = (uint32_t)(id >> 32);
        bool freed = slot < system->definitions.count && generation != 0 &&
                     generation < ((const ta_definition_slot *)ta_vec_at(&system->definitions, slot))->generation;
        return freed ? TA_OK : TA_ERR_NOT_FOUND;
    }
    definition->retired = true;
    if (definition->actor_count == 0) {
        ta_system_free_definition(system, definition);
    }
    return TA_OK;
}

static bool ta_bytes_same(ta_bytes left, ta_bytes right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

TA_API ta_result ta_system_find_definition(const ta_system *system,
                                    const ta_definition_identity *identity,
                                    ta_definition_id *out) {
    if (system == NULL || identity == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    /* Slots are reused, so registration order decides: the newest match wins. */
    const ta_definition *best = NULL;
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        const ta_definition *definition = entry->definition;
        if (definition == NULL || definition->retired ||
            (best != NULL && best->registration_serial > definition->registration_serial)) {
            continue;
        }
        if (ta_str_equal(definition->identity.family.name, identity->family.name) &&
            ta_bytes_same(definition->identity.revision, identity->revision)) {
            best = definition;
        }
    }
    if (best == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = best->id;
    return TA_OK;
}

TA_PRIVATE bool ta_system_is_scxml_io(const ta_system *system, ta_atom type) {
    return type == system->scxml_io_type || type == system->scxml_io_alias;
}

TA_PRIVATE const ta_io *ta_system_find_io(const ta_system *system, ta_atom type) {
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        const ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            return &entry->io;
        }
    }
    return NULL;
}

TA_API ta_result ta_system_register_io(ta_system *system, ta_atom type, ta_io io) {
    if (system == NULL || io.v == NULL || io.v->send == NULL || !ta_atom_valid(&system->atoms, type) ||
        ta_system_is_scxml_io(system, type)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < system->io_processors.count; index += 1) {
        ta_io_registration *entry = ta_vec_at(&system->io_processors, index);
        if (entry->type == type) {
            entry->io = io;
            return TA_OK;
        }
    }
    ta_io_registration entry = { .type = type, .io = io };
    return ta_vec_push(&system->io_processors, &entry);
}

TA_API ta_result ta_system_register_invoker(ta_system *system, ta_atom type, ta_invoker invoker) {
    if (system == NULL || invoker.v == NULL || !ta_atom_valid(&system->atoms, type)) {
        return TA_ERR_INVALID;
    }
    for (uint32_t index = 0; index < system->invokers.count; index += 1) {
        ta_invoker_registration *entry = ta_vec_at(&system->invokers, index);
        if (entry->type == type) {
            entry->invoker = invoker;
            return TA_OK;
        }
    }
    ta_invoker_registration entry = { .type = type, .invoker = invoker };
    return ta_vec_push(&system->invokers, &entry);
}

TA_API ta_result ta_system_intern_atom(ta_system *system, ta_str text, ta_atom *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    return ta_atom_intern(&system->atoms, text, out);
}

TA_API ta_result ta_system_atom_text(const ta_system *system, ta_atom atom, ta_str *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (!ta_atom_valid(&system->atoms, atom)) {
        return TA_ERR_NOT_FOUND;
    }
    *out = ta_atom_text(&system->atoms, atom);
    return TA_OK;
}

TA_API ta_result ta_event_name_intern(ta_system *system, const ta_atom_list *segments, ta_event_name_id *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    return ta_event_name_intern_atoms_internal(&system->event_names, segments, out);
}

TA_API bool ta_event_name_matches(const ta_system *system, ta_event_name_id event, ta_event_name_id prefix) {
    return system != NULL && ta_event_name_matches_internal(&system->event_names, event, prefix);
}

TA_API ta_result ta_event_name_format(const ta_system *system, const ta_event_name_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL ||
        !ta_event_name_valid(&system->event_names, params->name)) {
        return TA_ERR_INVALID;
    }
    ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
    uint32_t count = ta_event_name_segments(&system->event_names, params->name, segments);
    uint32_t length = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        length += ta_atom_text(&system->atoms, segments[index]).len + (index == 0 ? 0u : 1u);
    }
    *params->needed = length + 1u;
    if (params->buffer == NULL || params->capacity < length + 1u) {
        return TA_ERR_NOMEM;
    }
    uint32_t at = 0;
    for (uint32_t index = 0; index < count; index += 1) {
        if (index != 0) {
            params->buffer[at++] = '.';
        }
        ta_str text = ta_atom_text(&system->atoms, segments[index]);
        ta_mem_copy(params->buffer + at, text.ptr, text.len);
        at += text.len;
    }
    params->buffer[at] = '\0';
    return TA_OK;
}

TA_API ta_result ta_system_set_time(ta_system *system, ta_time now) {
    if (system == NULL || now < ta_clock_now(&system->clock)) {
        return TA_ERR_INVALID;
    }
    ta_clock_set(&system->clock, now);
    return TA_OK;
}

TA_API ta_time ta_system_time(const ta_system *system) {
    return system == NULL ? 0 : ta_clock_now(&system->clock);
}

TA_API ta_atom ta_system_scxml_io_type(const ta_system *system) {
    return system == NULL ? TA_ATOM_NONE : system->scxml_io_type;
}

TA_API ta_result ta_system_next_deadline(const ta_system *system, ta_time *out) {
    if (system == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_delayed_send *next = ta_timer_peek(&system->timers);
    if (next == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    *out = next->due;
    return TA_OK;
}

TA_API ta_result ta_system_dispatch_due(ta_system *system, const ta_due_dispatch_params *params) {
    if (system == NULL || params == NULL || params->budget == 0 || params->dispatched == NULL ||
        params->more_due == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t dispatched = 0;
    bool retry = false;
    ta_time now = ta_clock_now(&system->clock);
    while (dispatched < params->budget) {
        const ta_delayed_send *next = ta_timer_peek(&system->timers);
        if (next == NULL || next->due > now) {
            break;
        }
        ta_delayed_send send;
        (void)ta_timer_pop(&system->timers, &send);
        if (ta_interpreter_dispatch_delayed(system, &send) == TA_EXTERNAL_EVENT_RETRY) {
            /* Rescheduling keeps its ID, and so its place in deadline order. */
            ta_result result = ta_timer_schedule(&system->timers, &send);
            if (result != TA_OK) {
                ta_value_release(&send.target);
                ta_value_release(&send.data);
                return result;
            }
            retry = true;
            break;
        }
        dispatched += 1;
    }
    const ta_delayed_send *next = ta_timer_peek(&system->timers);
    *params->dispatched = dispatched;
    *params->more_due = retry || (next != NULL && next->due <= now);
    return TA_OK;
}


/*
 * SCXML execution following the W3C algorithm (Appendix D). The actor's
 * configuration stores active atomic states; a microstep expands it into the
 * full active set, applies exits and entries to that set, and writes the
 * atomic states back.
 */

/* Executable content failed; error.execution is queued and the block stops (SCXML 4.9). */
#define TA_EXEC_FAILED 1

/* The root initial transition taken at bootstrap. */
#define TA_TRANSITION_BOOTSTRAP (UINT32_MAX - 1u)

typedef struct {
    ta_state_id source;
    ta_span32 targets;
    ta_transition_type type;
    ta_span32 actions;
} ta_transition_view;

static ta_transition_view ta_view(const ta_interpreter *it, ta_transition_id transition) {
    const ta_definition *definition = it->definition;
    if (transition == TA_TRANSITION_BOOTSTRAP) {
        const ta_initial_transition_def *initial = &definition->initial_transitions[definition->root_initial];
        return (ta_transition_view){
            .source = TA_STATE_NONE,
            .targets = initial->targets,
            .type = TA_TRANSITION_EXTERNAL,
            .actions = initial->actions,
        };
    }
    const ta_transition_def *def = &definition->transitions[transition];
    return (ta_transition_view){
        .source = def->source,
        .targets = def->targets,
        .type = def->type,
        .actions = def->actions,
    };
}

static const ta_state_def *ta_state(const ta_interpreter *it, ta_state_id state) {
    return &it->definition->states[state];
}

static bool ta_is_history(const ta_interpreter *it, ta_state_id state) {
    ta_state_kind kind = ta_state(it, state)->kind;
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_descends(const ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return ta_definition_is_descendant(it->definition, state, ancestor);
}

static ta_state_id ta_child_at(const ta_interpreter *it, ta_state_id state, uint32_t index) {
    return it->definition->children[ta_state(it, state)->children.offset + index];
}

static ta_state_id ta_target_at(const ta_interpreter *it, ta_span32 targets, uint32_t index) {
    return it->definition->targets[targets.offset + index];
}

static void ta_bits_clear(const ta_interpreter *it, uint64_t *words) {
    ta_mem_zero(words, it->words * sizeof(uint64_t));
}

static bool ta_bits_any(const ta_interpreter *it, const uint64_t *words) {
    for (uint32_t index = 0; index < it->words; index += 1) {
        if (words[index] != 0) {
            return true;
        }
    }
    return false;
}

/* ------------------------------------------------------------------------
 * Events and errors
 */

static ta_actor_sparse_state *ta_sparse(ta_system *system, ta_actor *actor) {
    if (actor->sparse == NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            return NULL;
        }
        *actor->sparse = (ta_actor_sparse_state){0};
        ta_vec_init(&actor->sparse->internal_events, system->allocator, TA_VEC_LAYOUT(ta_queued_event));
    }
    return actor->sparse;
}

TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_sparse_state *sparse = ta_sparse(system, actor);
    ta_queued_event queued = {
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    ta_result result = sparse == NULL ? TA_ERR_NOMEM : ta_vec_push(&sparse->internal_events, &queued);
    if (result != TA_OK) {
        ta_event_release(event);
    }
    return result;
}

static bool ta_dequeue_internal(ta_actor *actor, ta_event *out) {
    if (actor->sparse == NULL || actor->sparse->internal_events.count == 0) {
        return false;
    }
    ta_vec *queue = &actor->sparse->internal_events;
    *out = ((ta_queued_event *)queue->items)[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        ((ta_queued_event *)queue->items)[index - 1u] = ((ta_queued_event *)queue->items)[index];
    }
    queue->count -= 1u;
    return true;
}

typedef struct {
    ta_event_name_id name;
    ta_send_id send_id;
    ta_fault *fault;
} ta_error_params;

/* Queues a platform error event; the fault's data becomes the event data. */
static ta_result ta_raise_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    ta_event event = {
        .name = params->name,
        .type = TA_EVENT_PLATFORM,
        .send_id = params->send_id,
        .invoke_id = TA_INVOKE_ID_NONE,
    };
    if (params->fault != NULL) {
        event.data = params->fault->data;
        params->fault->data = (ta_value){0};
    }
    return ta_interpreter_enqueue_internal(system, actor, &event);
}

static ta_result ta_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

static void ta_set_current_event(ta_actor *actor, ta_event *event) {
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
    }
    actor->current_event = *event;
    actor->has_current_event = true;
}

/* ------------------------------------------------------------------------
 * Data-model calls
 */

typedef struct {
    ta_value_source source;
    ta_value *out;
    ta_fault *fault;
} ta_value_params;

static ta_op_status ta_eval_source(ta_interpreter *it, const ta_value_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    *params->out = (ta_value){0};
    switch (params->source.kind) {
        case TA_VALUE_SOURCE_NONE:
            return TA_OP_OK;
        case TA_VALUE_SOURCE_EXPR:
            if (v->eval_value == NULL) {
                return TA_OP_ERROR;
            }
            return v->eval_value(definition->datamodel.self,
                                 &(ta_datamodel_eval_value_params){
                                     .shared = definition->datamodel_shared,
                                     .instance = it->actor->datamodel_instance,
                                     .expr = params->source.as.expr,
                                     .host = &it->actor->host,
                                     .out = params->out,
                                     .fault = params->fault,
                                 });
        case TA_VALUE_SOURCE_LOCATION:
            if (v->read_location == NULL) {
                return TA_OP_ERROR;
            }
            return v->read_location(definition->datamodel.self,
                                    &(ta_datamodel_read_location_params){
                                        .shared = definition->datamodel_shared,
                                        .instance = it->actor->datamodel_instance,
                                        .location = params->source.as.location,
                                        .host = &it->actor->host,
                                        .out = params->out,
                                        .fault = params->fault,
                                    });
        case TA_VALUE_SOURCE_CONTENT:
            if (v->import_content == NULL) {
                return TA_OP_ERROR;
            }
            return v->import_content(definition->datamodel.self,
                                     &(ta_datamodel_import_content_params){
                                         .shared = definition->datamodel_shared,
                                         .instance = it->actor->datamodel_instance,
                                         .content = definition->contents[params->source.as.content].bytes,
                                         .host = &it->actor->host,
                                         .out = params->out,
                                         .fault = params->fault,
                                     });
        default:
            return TA_OP_ERROR;
    }
}

/*
 * Evaluates an expression and reads its value as text through value_text.
 * `*text` is borrowed from `*holder`, which the caller releases after use.
 */
typedef struct {
    ta_expr_id expr;
    ta_value *holder;
    ta_str *text;
    ta_fault *fault;
} ta_text_eval_params;

static ta_op_status ta_eval_text(ta_interpreter *it, const ta_text_eval_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = ta_eval_source(it, &(ta_value_params){
                                                 .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = params->expr },
                                                 .out = params->holder,
                                                 .fault = params->fault,
                                             });
    if (status != TA_OP_OK) {
        return status;
    }
    if (v->value_text == NULL) {
        return TA_OP_ERROR;
    }
    *params->text = (ta_str){0};
    status = v->value_text(definition->datamodel.self,
                           &(ta_datamodel_value_text_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .value = params->holder,
                               .host = &it->actor->host,
                               .out = params->text,
                               .fault = params->fault,
                           });
    return status == TA_OP_OK && params->text->ptr == NULL && params->text->len != 0 ? TA_OP_ERROR : status;
}

static ta_op_status ta_eval_condition(ta_interpreter *it, ta_expr_id expr, bool *out, ta_fault *fault) {
    const ta_definition *definition = it->definition;
    if (definition->datamodel.v->eval_condition == NULL) {
        return TA_OP_ERROR;
    }
    return definition->datamodel.v->eval_condition(definition->datamodel.self,
                                                   &(ta_datamodel_eval_condition_params){
                                                       .shared = definition->datamodel_shared,
                                                       .instance = it->actor->datamodel_instance,
                                                       .expr = expr,
                                                       .host = &it->actor->host,
                                                       .out = out,
                                                       .fault = fault,
                                                   });
}

static ta_result ta_initialize_data(ta_interpreter *it, ta_state_id owner) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (uint32_t index = 0; index < definition->data_decl_count; index += 1) {
        const ta_data_decl *decl = &definition->data_decls[index];
        if (decl->owner_state != owner || decl->initializer == TA_INITIALIZER_NONE ||
            (it->bound_decls != NULL && ta_bits_get(it->bound_decls, index))) {
            continue;
        }
        ta_fault fault = {0};
        ta_value value = {0};
        ta_op_status status = ta_eval_source(it, &(ta_value_params){
                                                     .source = definition->initializers[decl->initializer].value,
                                                     .out = &value,
                                                     .fault = &fault,
                                                 });
        if (status == TA_OP_OK) {
            status = v->set_named == NULL
                         ? TA_OP_ERROR
                         : v->set_named(definition->datamodel.self,
                                        &(ta_datamodel_set_named_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .name = ta_atom_text(&it->system->atoms, decl->name),
                                            .value = &value,
                                            .host = &it->actor->host,
                                            .fault = &fault,
                                        });
            ta_value_release(&value);
        }
        if (status != TA_OP_OK) {
            /* The binding stays declared; the error does not stop other initializers. */
            ta_result result = ta_execution_error(it, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
        }
    }
    return TA_OK;
}

static ta_result ta_declare_data(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    for (uint32_t index = 0; index < definition->data_decl_count && v->declare != NULL; index += 1) {
        ta_fault fault = {0};
        bool bound = false;
        ta_op_status status = v->declare(definition->datamodel.self,
                                         &(ta_datamodel_declare_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .name = ta_atom_text(&it->system->atoms,
                                                                  definition->data_decls[index].name),
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                             .bound = &bound,
                                         });
        if (bound && it->bound_decls != NULL) {
            ta_bits_put(it->bound_decls, index, true);
        }
        if (status != TA_OP_OK) {
            ta_result result = ta_execution_error(it, &fault);
            if (result != TA_EXEC_FAILED) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Sends
 */

static ta_send_id ta_literal_send_id(ta_atom atom) {
    return (ta_send_id)atom + 1u;
}

/*
 * Send IDs as text. A literal ID is the author's text (its atom); a generated
 * ID is TA_GENERATED_SEND_ID_PREFIX followed by the actor-local counter. Both
 * forms round-trip, so idlocation, sendidexpr, and _event.sendid agree.
 */
#define TA_GENERATED_SEND_ID_BIT ((ta_send_id)1u << 63)
#define TA_GENERATED_SEND_ID_PREFIX "#_send."

TA_PRIVATE void ta_send_id_write(const ta_system *system, ta_send_id id, ta_text_writer *writer) {
    if (id & TA_GENERATED_SEND_ID_BIT) {
        ta_text_put(writer, ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX));
        ta_text_put_u64(writer, id & ~TA_GENERATED_SEND_ID_BIT);
    } else if (id != 0) {
        ta_text_put(writer, ta_atom_text(&system->atoms, (ta_atom)(id - 1u)));
    }
}

TA_PRIVATE ta_send_id ta_send_id_from_text(const ta_system *system, ta_str text) {
    ta_str prefix = ta_str_from_cstr(TA_GENERATED_SEND_ID_PREFIX);
    if (text.len > prefix.len && ta_str_equal((ta_str){ .ptr = text.ptr, .len = prefix.len }, prefix)) {
        uint64_t number = 0;
        bool digits = true;
        for (uint32_t index = prefix.len; index < text.len && digits; index += 1) {
            char digit = text.ptr[index];
            digits = digit >= '0' && digit <= '9' && number <= (UINT64_MAX >> 1) / 10u;
            number = number * 10u + (uint64_t)(digit - '0');
        }
        if (digits && number != 0 && (number & TA_GENERATED_SEND_ID_BIT) == 0) {
            return TA_GENERATED_SEND_ID_BIT | number;
        }
    }
    ta_atom atom = 0;
    return ta_atom_find(&system->atoms, text, &atom) == TA_OK ? ta_literal_send_id(atom) : 0;
}

TA_API ta_result ta_send_id_parse(const ta_system *system, ta_str text, ta_send_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    ta_send_id id = ta_send_id_from_text(system, text);
    if (id == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = id;
    return TA_OK;
}

TA_API ta_result ta_send_id_format(const ta_system *system, const ta_send_id_format_params *params) {
    if (system == NULL || params == NULL || params->needed == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->send_id == 0) {
        *params->needed = 0;
        return TA_ERR_NOT_FOUND;
    }
    ta_text_writer writer = { .buffer = params->buffer, .capacity = params->capacity };
    ta_send_id_write(system, params->send_id, &writer);
    return ta_text_finish(&writer, params->needed);
}

typedef struct {
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    ta_actor_ref destination;
    ta_event_name_id event;
    ta_send_id send_id;
    const ta_value *data;
    ta_fault *fault;
} ta_submit_params;

static ta_external_event_submit_status ta_submit_external(ta_system *system, const ta_submit_params *params) {
    if (system->external_events.v == NULL) {
        return TA_EXTERNAL_EVENT_ERROR;
    }
    ta_external_delivery delivery = {
        .destination = params->destination,
        .event = {
            .name = params->event,
            .type = TA_EVENT_EXTERNAL,
            .send_id = params->send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->origin },
            .invoke_id = params->invoke_id,
            .data = *params->data,
        },
    };
    return system->external_events.v->submit(system->external_events.self,
                                             &(ta_external_event_submit_params){
                                                 .delivery = &delivery,
                                                 .fault = params->fault,
                                             });
}

static ta_result ta_communication_error(ta_system *system, ta_actor *actor, const ta_error_params *params) {
    if (actor->finished) {
        /* A send submitted before termination failed; the session can no longer observe it. */
        if (params->fault != NULL) {
            ta_value_release(&params->fault->data);
            params->fault->data = (ta_value){0};
        }
        return TA_OK;
    }
    ta_error_params error = *params;
    error.name = TA_EVENT_NAME_ERROR_COMMUNICATION;
    ta_result result = ta_raise_error(system, actor, &error);
    if (result == TA_OK && !actor->macrostep) {
        /* An asynchronous failure makes an idle actor runnable again. */
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return result;
}

TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send) {
    ta_actor *sender = ta_actor_lookup(system, send->owner);
    ta_fault fault = {0};
    ta_external_event_submit_status status = TA_EXTERNAL_EVENT_ERROR;
    /*
     * A submitted send was already sent: it is logically in the receiver's
     * queue and keeps its envelope even when the sending session is gone.
     */
    ta_actor_ref origin = send->submitted || sender == NULL ? send->origin : sender->record.self;
    /* Invoke: events from an invoked child to its parent carry the invoke ID. */
    ta_invoke_id invoke_id = send->submitted || sender == NULL
                                 ? send->invoke_id
                                 : ta_invoke_parent_id(sender, &send->destination);
    if (sender == NULL && !send->submitted) {
        status = TA_EXTERNAL_EVENT_ACCEPTED; /* the sender's session is gone; drop the send */
    } else if (ta_system_is_scxml_io(system, send->io_type)) {
        status = ta_submit_external(system, &(ta_submit_params){
                                                .origin = origin,
                                                .invoke_id = invoke_id,
                                                .destination = send->destination,
                                                .event = send->event,
                                                .send_id = send->send_id,
                                                .data = &send->data,
                                                .fault = &fault,
                                            });
    } else {
        const ta_io *io = ta_system_find_io(system, send->io_type);
        ta_send request = {
            .source = origin,
            .send_id = send->send_id,
            .event = send->event,
            .io_type = send->io_type,
            .target = send->target,
            .data = send->data,
        };
        if (io != NULL && io->v->send(io->self, &request, &fault) == TA_OP_OK) {
            status = TA_EXTERNAL_EVENT_ACCEPTED;
        }
    }
    if (status == TA_EXTERNAL_EVENT_RETRY) {
        ta_value_release(&fault.data);
        return status;
    }
    if (status == TA_EXTERNAL_EVENT_ERROR && sender != NULL) {
        (void)ta_communication_error(system, sender,
                                     &(ta_error_params){ .send_id = send->send_id, .fault = &fault });
    }
    ta_value_release(&fault.data);
    ta_value_release(&send->target);
    ta_value_release(&send->data);
    return status;
}

typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    bool scxml_io;
} ta_send_state;

static ta_op_status ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
    if (send->params.length == 0) {
        return ta_eval_source(it, &(ta_value_params){ .source = send->data, .out = &state->data, .fault = fault });
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->make_params == NULL) {
        return TA_OP_ERROR;
    }
    ta_named_value *values = ta_arena_alloc(&it->system->scratch, send->params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        return TA_OP_ERROR;
    }
    uint32_t evaluated = 0;
    ta_op_status status = TA_OP_OK;
    for (; evaluated < send->params.length && status == TA_OP_OK; evaluated += 1) {
        const ta_param_def *param = &definition->params[send->params.offset + evaluated];
        values[evaluated].name = ta_atom_text(&it->system->atoms, param->name);
        status = ta_eval_source(it, &(ta_value_params){ .source = param->value, .out = &values[evaluated].value,
                                                        .fault = fault });
    }
    if (status == TA_OP_OK) {
        status = v->make_params(definition->datamodel.self,
                                &(ta_datamodel_make_params_params){
                                    .shared = definition->datamodel_shared,
                                    .instance = it->actor->datamodel_instance,
                                    .values = values,
                                    .count = send->params.length,
                                    .host = &it->actor->host,
                                    .out = &state->data,
                                    .fault = fault,
                                });
    }
    for (uint32_t index = 0; index < evaluated; index += 1) {
        ta_value_release(&values[index].value);
    }
    return status;
}

static ta_op_status ta_send_target(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                   ta_fault *fault) {
    if (!state->scxml_io) {
        /* A custom I/O processor receives the evaluated value, not a URI. */
        return send->target.kind == TA_VALUE_SOURCE_NONE
                   ? TA_OP_OK
                   : ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                            .fault = fault });
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OP_OK;
    }
    /*
     * One grammar for both spellings (ADR 0044): a literal target is the
     * definition's text, and an evaluated one is the text of its value. A
     * named address borrows that text, which the send record keeps alive by
     * retaining the value.
     */
    ta_str text;
    if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_bytes content = it->definition->contents[send->target.as.content].bytes;
        text = (ta_str){ .ptr = (const char *)content.ptr, .len = content.len };
    } else {
        const ta_definition *definition = it->definition;
        const ta_datamodel_vtable *v = definition->datamodel.v;
        ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                                     .fault = fault });
        if (status != TA_OP_OK) {
            return status;
        }
        if (v->value_text == NULL) {
            return TA_OP_ERROR;
        }
        status = v->value_text(definition->datamodel.self,
                               &(ta_datamodel_value_text_params){
                                   .shared = definition->datamodel_shared,
                                   .instance = it->actor->datamodel_instance,
                                   .value = &state->target,
                                   .host = &it->actor->host,
                                   .out = &text,
                                   .fault = fault,
                               });
        if (status != TA_OP_OK) {
            return status;
        }
    }
    return ta_scxml_target_parse(it->system, text, &state->scxml) == TA_OK ? TA_OP_OK : TA_OP_ERROR;
}

/* Resolves an SCXML target to an external destination; false means inaccessible. */
static bool ta_send_destination(ta_interpreter *it, const ta_scxml_target *target, ta_actor_ref *out) {
    switch (target->kind) {
        case TA_SCXML_TARGET_SELF:
            *out = it->actor->record.self;
            return true;
        case TA_SCXML_TARGET_PARENT:
            if (it->actor->sparse == NULL || !it->actor->sparse->has_parent) {
                return false;
            }
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = it->actor->sparse->parent.actor };
            return true;
        case TA_SCXML_TARGET_ACTOR:
            *out = target->as.actor;
            return true;
        case TA_SCXML_TARGET_INVOKE:
            return ta_invoke_destination(it->actor, target->as.invoke, out);
        default:
            return false;
    }
}

/* Resolves `type` / `typeexpr`; an unknown processor is an execution error. */
static ta_op_status ta_send_io_type(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
    ta_system *system = it->system;
    ta_atom type = system->scxml_io_type;
    if (send->type.kind == TA_TEXT_SOURCE_ATOM) {
        type = send->type.as.atom;
    } else if (send->type.kind == TA_TEXT_SOURCE_EXPR) {
        ta_value holder = {0};
        ta_str text = {0};
        ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                                   .expr = send->type.as.expr, .holder = &holder,
                                                   .text = &text, .fault = fault,
                                               });
        if (status == TA_OP_OK && (text.len == 0 || ta_atom_intern(&system->atoms, text, &type) != TA_OK)) {
            status = TA_OP_ERROR;
        }
        ta_value_release(&holder);
        if (status != TA_OP_OK) {
            return status;
        }
    }
    state->scxml_io = ta_system_is_scxml_io(system, type);
    state->io_type = state->scxml_io ? system->scxml_io_type : type;
    return state->scxml_io || ta_system_find_io(system, type) != NULL ? TA_OP_OK : TA_OP_ERROR;
}

/* Resolves `event` / `eventexpr`; the expression's text is a dotted event name. */
static ta_op_status ta_send_event_name(ta_interpreter *it, const ta_send_action *send, ta_event_name_id *out,
                                       ta_fault *fault) {
    if (send->event.kind == TA_EVENT_SOURCE_NAME) {
        *out = send->event.as.name;
        return TA_OP_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->event.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    if (status == TA_OP_OK && ta_event_name_intern_dotted(&it->system->event_names, text, out) != TA_OK) {
        status = TA_OP_ERROR;
    }
    ta_value_release(&holder);
    return status;
}

/* Resolves `delay` / `delayexpr`; the expression's text is an SCXML duration. */
static ta_op_status ta_send_delay(ta_interpreter *it, const ta_send_action *send, ta_time *out, ta_fault *fault) {
    *out = 0;
    if (send->delay.kind == TA_DELAY_SOURCE_LITERAL) {
        *out = send->delay.as.literal;
        return TA_OP_OK;
    }
    if (send->delay.kind != TA_DELAY_SOURCE_EXPR) {
        return TA_OP_OK;
    }
    ta_value holder = {0};
    ta_str text = {0};
    ta_op_status status = ta_eval_text(it, &(ta_text_eval_params){
                                               .expr = send->delay.as.expr, .holder = &holder,
                                               .text = &text, .fault = fault,
                                           });
    if (status == TA_OP_OK && ta_duration_parse(text, out) != TA_OK) {
        status = TA_OP_ERROR;
    }
    ta_value_release(&holder);
    return status;
}

/* Stores generated text, such as an ID, through a location (`idlocation`). */
typedef struct {
    ta_expr_id location;
    ta_str text;
    ta_fault *fault;
} ta_store_text_params;

static ta_op_status ta_store_text(ta_interpreter *it, const ta_store_text_params *params) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->text_value == NULL || v->assign == NULL) {
        return TA_OP_ERROR;
    }
    ta_value value = {0};
    ta_op_status status = v->text_value(definition->datamodel.self,
                                        &(ta_datamodel_text_value_params){
                                            .shared = definition->datamodel_shared,
                                            .instance = it->actor->datamodel_instance,
                                            .text = params->text,
                                            .host = &it->actor->host,
                                            .out = &value,
                                            .fault = params->fault,
                                        });
    if (status == TA_OP_OK) {
        status = v->assign(definition->datamodel.self,
                           &(ta_datamodel_assign_params){
                               .shared = definition->datamodel_shared,
                               .instance = it->actor->datamodel_instance,
                               .location = params->location,
                               .value = &value,
                               .host = &it->actor->host,
                               .fault = params->fault,
                           });
    }
    ta_value_release(&value);
    return status;
}

/*
 * Assigns the send ID. Without `id` or `idlocation` the send has no ID, so
 * its events carry a blank sendid (SCXML 5.10.1).
 */
static ta_op_status ta_send_assign_id(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                               ta_fault *fault) {
    state->send_id = 0;
    if (send->id.kind == TA_ID_SOURCE_LITERAL) {
        state->send_id = ta_literal_send_id(send->id.as.literal);
        return TA_OP_OK;
    }
    if (send->id.kind != TA_ID_SOURCE_LOCATION) {
        return TA_OP_OK;
    }
    it->actor->next_send_id += 1u;
    state->send_id = TA_GENERATED_SEND_ID_BIT | it->actor->next_send_id;
    char buffer[48];
    ta_text_writer writer = { .buffer = buffer, .capacity = sizeof(buffer) };
    ta_send_id_write(it->system, state->send_id, &writer);
    return ta_store_text(it, &(ta_store_text_params){
                                 .location = send->id.as.location,
                                 .text = { .ptr = buffer, .len = writer.length },
                                 .fault = fault,
                             });
}

/* An error while evaluating a <send> carries that send's ID (SCXML 5.10.1). */
static ta_result ta_send_execution_error(ta_interpreter *it, ta_send_id send_id, ta_fault *fault) {
    ta_result result = ta_raise_error(it->system, it->actor,
                                      &(ta_error_params){ .name = TA_EVENT_NAME_ERROR_EXECUTION,
                                                          .send_id = send_id, .fault = fault });
    return result == TA_OK ? TA_EXEC_FAILED : result;
}

static ta_result ta_exec_send(ta_interpreter *it, const ta_send_action *send) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    ta_send_state state = { .io_type = system->scxml_io_type, .scxml_io = true };
    ta_fault fault = {0};
    ta_event_name_id event_name = 0;
    ta_time delay = 0;
    ta_op_status status = ta_send_assign_id(it, send, &state, &fault);
    if (status == TA_OP_OK) {
        status = ta_send_event_name(it, send, &event_name, &fault);
    }
    if (status == TA_OP_OK) {
        status = ta_send_io_type(it, send, &state, &fault);
    }
    if (status == TA_OP_OK) {
        status = ta_send_delay(it, send, &delay, &fault);
    }
    if (status != TA_OP_OK) {
        return ta_send_execution_error(it, state.send_id, &fault);
    }
    status = ta_send_payload(it, send, &state, &fault);
    if (status == TA_OP_OK) {
        status = ta_send_target(it, send, &state, &fault);
    }
    if (status == TA_OP_OK && state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL && delay != 0) {
        status = TA_OP_ERROR; /* a delayed send cannot target the internal queue */
    }
    if (status != TA_OP_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        return ta_send_execution_error(it, state.send_id, &fault);
    }

    if (state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL) {
        ta_value_release(&state.target);
        ta_event event = {
            .name = event_name,
            .type = TA_EVENT_INTERNAL,
            .send_id = state.send_id,
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = actor->record.self },
            .invoke_id = TA_INVOKE_ID_NONE,
            .data = state.data,
        };
        return ta_interpreter_enqueue_internal(system, actor, &event);
    }

    ta_delayed_send record = {
        .owner = actor->record.id,
        .send_id = state.send_id,
        .due = ta_clock_now(&system->clock) + delay,
        .event = event_name,
        .io_type = state.io_type,
        .target = state.target,
        .data = state.data,
    };
    if (state.scxml_io && !ta_send_destination(it, &state.scxml, &record.destination)) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state.send_id });
    }
    if (delay == 0) {
        ta_external_event_submit_status submitted = ta_interpreter_dispatch_delayed(system, &record);
        if (submitted != TA_EXTERNAL_EVENT_RETRY) {
            return TA_OK;
        }
        /*
         * The router accepted nothing, typically because the destination is
         * this busy actor. The evaluated send waits in the deadline index,
         * due now, and keeps its order among later sends.
         */
        record.submitted = true;
        record.origin = actor->record.self;
        record.invoke_id = ta_invoke_parent_id(actor, &record.destination);
    }
    ta_result result = ta_timer_schedule(&system->timers, &record);
    if (result != TA_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Executable content
 */

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block);

/*
 * <foreach> iterates a shallow copy made by the data model. A failed
 * snapshot or binding, or an error in the body, stops the foreach and the
 * enclosing block (SCXML 4.6).
 */
static ta_result ta_exec_foreach(ta_interpreter *it, const ta_foreach_action *foreach) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->foreach_snapshot == NULL || v->foreach_bind == NULL) {
        return ta_execution_error(it, NULL);
    }
    ta_fault fault = {0};
    ta_value snapshot = {0};
    uint32_t count = 0;
    if (v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = foreach->array,
                                .host = &it->actor->host,
                                .out = &snapshot,
                                .count = &count,
                                .fault = &fault,
                            }) != TA_OP_OK) {
        return ta_execution_error(it, &fault);
    }
    ta_str item = ta_atom_text(&it->system->atoms, foreach->item);
    ta_str index = foreach->has_index ? ta_atom_text(&it->system->atoms, foreach->index) : (ta_str){0};
    ta_result result = TA_OK;
    for (uint32_t position = 0; position < count && result == TA_OK; position += 1) {
        ta_op_status bound = v->foreach_bind(definition->datamodel.self,
                                             &(ta_datamodel_foreach_bind_params){
                                                 .shared = definition->datamodel_shared,
                                                 .instance = it->actor->datamodel_instance,
                                                 .snapshot = &snapshot,
                                                 .index = position,
                                                 .item = item,
                                                 .index_name = index,
                                                 .host = &it->actor->host,
                                                 .fault = &fault,
                                             });
        result = bound == TA_OP_OK ? ta_run_block(it, foreach->body_actions) : ta_execution_error(it, &fault);
    }
    ta_value_release(&snapshot);
    return result;
}

static ta_result ta_exec_action(ta_interpreter *it, const ta_action_def *action) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    switch (action->kind) {
        case TA_ACTION_RAISE: {
            ta_value data = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.raise.data, .out = &data,
                                                            .fault = &fault });
            if (status != TA_OP_OK) {
                break;
            }
            ta_event event = {
                .name = action->as.raise.event,
                .type = TA_EVENT_INTERNAL,
                .invoke_id = TA_INVOKE_ID_NONE,
                .data = data,
            };
            return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
        }
        case TA_ACTION_SEND:
            return ta_exec_send(it, &action->as.send);
        case TA_ACTION_CANCEL: {
            ta_send_id send_id = 0;
            if (action->as.cancel.sendid.kind == TA_TEXT_SOURCE_ATOM) {
                send_id = ta_literal_send_id(action->as.cancel.sendid.as.atom);
            } else {
                ta_value holder = {0};
                ta_str text = {0};
                status = ta_eval_text(it, &(ta_text_eval_params){
                                              .expr = action->as.cancel.sendid.as.expr, .holder = &holder,
                                              .text = &text, .fault = &fault,
                                          });
                if (status == TA_OP_OK) {
                    send_id = ta_send_id_from_text(it->system, text);
                }
                ta_value_release(&holder);
            }
            if (status == TA_OP_OK && send_id != 0) {
                (void)ta_timer_cancel(&it->system->timers, it->actor->record.id, send_id);
            }
            break;
        }
        case TA_ACTION_ASSIGN: {
            ta_value value = {0};
            status = ta_eval_source(it, &(ta_value_params){ .source = action->as.assign.value, .out = &value,
                                                            .fault = &fault });
            if (status == TA_OP_OK) {
                status = v->assign == NULL
                             ? TA_OP_ERROR
                             : v->assign(definition->datamodel.self,
                                         &(ta_datamodel_assign_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .location = action->as.assign.location,
                                             .value = &value,
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                         });
                ta_value_release(&value);
            }
            break;
        }
        case TA_ACTION_SCRIPT:
            status = v->execute_script == NULL
                         ? TA_OP_ERROR
                         : v->execute_script(definition->datamodel.self,
                                             &(ta_datamodel_execute_script_params){
                                                 .shared = definition->datamodel_shared,
                                                 .instance = it->actor->datamodel_instance,
                                                 .script = action->as.script,
                                                 .host = &it->actor->host,
                                                 .fault = &fault,
                                             });
            break;
        case TA_ACTION_LOG: {
            /*
             * SCXML 4.7: evaluating the expression can fail, which is an
             * ordinary execution error; writing the record cannot, so a
             * <log> never changes interpretation.
             */
            ta_log_record record = { .source = it->actor->record.self };
            if (action->as.log.has_label) {
                record.label = ta_atom_text(&it->system->atoms, action->as.log.label);
            }
            if (action->as.log.has_expr) {
                status = ta_eval_source(it, &(ta_value_params){
                                                .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = action->as.log.expr },
                                                .out = &record.value,
                                                .fault = &fault,
                                            });
                record.has_value = status == TA_OP_OK;
            }
            if (status == TA_OP_OK && it->system->log_sink.v != NULL) {
                it->system->log_sink.v->write(it->system->log_sink.self, &record);
            }
            ta_value_release(&record.value);
            break;
        }
        case TA_ACTION_FOREACH:
            return ta_exec_foreach(it, &action->as.foreach);
        case TA_ACTION_IF: {
            bool condition = false;
            status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            if (status == TA_OP_OK) {
                return ta_run_block(it, condition ? action->as.conditional.then_actions
                                                  : action->as.conditional.else_actions);
            }
            break;
        }
        default:
            status = TA_OP_ERROR;
            break;
    }
    return status == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
}

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    for (uint32_t index = 0; index < block.length; index += 1) {
        ta_result result = ta_exec_action(it, &it->definition->action_defs[block.offset + index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* A failed block has already queued its error; execution continues with the next block. */
static ta_result ta_run_block_isolated(ta_interpreter *it, ta_span32 block) {
    ta_result result = ta_run_block(it, block);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/*
 * Runs a state's <onentry> or <onexit> handlers in document order. Each
 * handler is a separate block (SCXML 3.8, 3.9): an error skips the rest of
 * its own handler only.
 */
static ta_result ta_run_handlers(ta_interpreter *it, ta_span32 handlers) {
    for (uint32_t index = 0; index < handlers.length; index += 1) {
        ta_result result = ta_run_block_isolated(it, it->definition->handlers[handlers.offset + index]);
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Transition selection
 */

static void ta_effective_targets(ta_interpreter *it, ta_span32 targets, uint64_t *out) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_state_id target = ta_target_at(it, targets, index);
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
            continue;
        }
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, ta_state(it, target)->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (uint32_t word = 0; word < it->words; word += 1) {
                out[word] |= recorded[word];
            }
        } else {
            const ta_transition_def *fallback = &it->definition->transitions[ta_state(it, target)->transitions.offset];
            ta_effective_targets(it, fallback->targets, out);
        }
    }
}

/* False when the transition is targetless and so has no domain. */
static bool ta_transition_domain(ta_interpreter *it, ta_transition_id transition, ta_state_id *out) {
    ta_transition_view view = ta_view(it, transition);
    if (view.targets.length == 0) {
        return false;
    }
    if (view.source == TA_STATE_NONE) {
        *out = TA_STATE_NONE;
        return true;
    }
    uint64_t *targets = it->domain_targets;
    ta_bits_clear(it, targets);
    ta_effective_targets(it, view.targets, targets);
    const ta_definition *definition = it->definition;
    if (view.type == TA_TRANSITION_INTERNAL && ta_state(it, view.source)->kind == TA_STATE_COMPOUND) {
        bool inside = true;
        for (ta_state_id state = 0; state < definition->state_count && inside; state += 1) {
            inside = !ta_bits_get(targets, state) || ta_descends(it, state, view.source);
        }
        if (inside) {
            *out = view.source;
            return true;
        }
    }
    /* The least common compound ancestor of the source and every target. */
    for (ta_state_id ancestor = ta_state(it, view.source)->parent; ancestor != TA_STATE_NONE;
         ancestor = ta_state(it, ancestor)->parent) {
        if (ta_state(it, ancestor)->kind != TA_STATE_COMPOUND) {
            continue;
        }
        bool common = true;
        for (ta_state_id state = 0; state < definition->state_count && common; state += 1) {
            common = !ta_bits_get(targets, state) || ta_descends(it, state, ancestor);
        }
        if (common) {
            *out = ancestor;
            return true;
        }
    }
    *out = TA_STATE_NONE;
    return true;
}

static void ta_exit_set(ta_interpreter *it, ta_transition_id transition, uint64_t *out) {
    ta_state_id domain = TA_STATE_NONE;
    if (!ta_transition_domain(it, transition, &domain)) {
        return;
    }
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_descends(it, state, domain)) {
            ta_bits_put(out, state, true);
        }
    }
}

static void ta_remove_conflicts(ta_interpreter *it) {
    uint32_t kept = 0;
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id candidate = it->enabled[index];
        ta_bits_clear(it, it->exit_left);
        ta_exit_set(it, candidate, it->exit_left);
        bool preempted = false;
        for (uint32_t other = 0; other < kept && !preempted; other += 1) {
            it->remove[other] = false;
            ta_bits_clear(it, it->exit_right);
            ta_exit_set(it, it->filtered[other], it->exit_right);
            bool overlap = false;
            for (uint32_t word = 0; word < it->words && !overlap; word += 1) {
                overlap = (it->exit_left[word] & it->exit_right[word]) != 0;
            }
            if (!overlap) {
                continue;
            }
            if (ta_descends(it, ta_view(it, candidate).source, ta_view(it, it->filtered[other]).source)) {
                it->remove[other] = true;
            } else {
                preempted = true;
            }
        }
        if (preempted) {
            continue;
        }
        uint32_t write = 0;
        for (uint32_t other = 0; other < kept; other += 1) {
            if (!it->remove[other]) {
                it->filtered[write++] = it->filtered[other];
            }
        }
        kept = write;
        it->filtered[kept++] = candidate;
    }
    for (uint32_t index = 0; index < kept; index += 1) {
        it->enabled[index] = it->filtered[index];
    }
    it->enabled_count = kept;
}

static bool ta_transition_matches(ta_interpreter *it, const ta_transition_def *transition, const ta_event *event) {
    if (event == NULL) {
        return transition->event_patterns.length == 0;
    }
    for (uint32_t index = 0; index < transition->event_patterns.length; index += 1) {
        if (ta_event_pattern_matches(it->definition,
                                     &it->definition->event_patterns[transition->event_patterns.offset + index],
                                     event->name)) {
            return true;
        }
    }
    return false;
}

/* Selects the optimal enabled transition set; event is null for eventless selection. */
static ta_result ta_select_transitions(ta_interpreter *it, const ta_event *event) {
    const ta_definition *definition = it->definition;
    it->enabled_count = 0;
    for (ta_state_id atomic = 0; atomic < definition->state_count; atomic += 1) {
        if (!ta_bits_get(it->active, atomic) || !ta_definition_is_atomic(definition, atomic)) {
            continue;
        }
        bool found = false;
        for (ta_state_id state = atomic; state != TA_STATE_NONE && !found; state = ta_state(it, state)->parent) {
            ta_span32 transitions = ta_state(it, state)->transitions;
            for (uint32_t index = 0; index < transitions.length && !found; index += 1) {
                ta_transition_id id = transitions.offset + index;
                const ta_transition_def *transition = &definition->transitions[id];
                if (!ta_transition_matches(it, transition, event)) {
                    continue;
                }
                bool enabled = true;
                if (transition->condition != UINT32_MAX) {
                    ta_fault fault = {0};
                    if (ta_eval_condition(it, transition->condition, &enabled, &fault) != TA_OP_OK) {
                        enabled = false;
                        ta_result result = ta_execution_error(it, &fault);
                        if (result != TA_EXEC_FAILED) {
                            return result;
                        }
                    }
                }
                if (!enabled) {
                    continue;
                }
                found = true;
                bool duplicate = false;
                for (uint32_t other = 0; other < it->enabled_count && !duplicate; other += 1) {
                    duplicate = it->enabled[other] == id;
                }
                if (!duplicate) {
                    it->enabled[it->enabled_count++] = id;
                }
            }
        }
    }
    ta_remove_conflicts(it);
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Microsteps
 */

static void ta_add_descendants(ta_interpreter *it, ta_state_id state);

static bool ta_entering_below(ta_interpreter *it, ta_state_id ancestor) {
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->to_enter, state) && ta_descends(it, state, ancestor)) {
            return true;
        }
    }
    return false;
}

static void ta_enter_parallel_regions(ta_interpreter *it, ta_state_id parallel) {
    for (uint32_t index = 0; index < ta_state(it, parallel)->children.length; index += 1) {
        ta_state_id child = ta_child_at(it, parallel, index);
        if (!ta_is_history(it, child) && !ta_entering_below(it, child)) {
            ta_add_descendants(it, child);
        }
    }
}

static void ta_add_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    for (ta_state_id at = ta_state(it, state)->parent; at != ancestor && at != TA_STATE_NONE;
         at = ta_state(it, at)->parent) {
        ta_bits_put(it->to_enter, at, true);
        if (ta_state(it, at)->kind == TA_STATE_PARALLEL) {
            ta_enter_parallel_regions(it, at);
        }
    }
}

static void ta_add_targets(ta_interpreter *it, ta_span32 targets, ta_state_id ancestor) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_add_descendants(it, ta_target_at(it, targets, index));
    }
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_add_ancestors(it, ta_target_at(it, targets, index), ancestor);
    }
}

static void ta_add_descendants(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (ta_is_history(it, state)) {
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (ta_state_id each = 0; each < it->definition->state_count; each += 1) {
                if (ta_bits_get(recorded, each)) {
                    ta_add_descendants(it, each);
                }
            }
            for (ta_state_id each = 0; each < it->definition->state_count; each += 1) {
                if (ta_bits_get(recorded, each)) {
                    ta_add_ancestors(it, each, def->parent);
                }
            }
        } else {
            it->default_history[def->parent] = state;
            ta_add_targets(it, it->definition->transitions[def->transitions.offset].targets, def->parent);
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        ta_add_targets(it, it->definition->initial_transitions[def->initial].targets, state);
    } else if (def->kind == TA_STATE_PARALLEL) {
        ta_enter_parallel_regions(it, state);
    }
}

static bool ta_in_final_state(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    if (def->kind == TA_STATE_COMPOUND) {
        for (uint32_t index = 0; index < def->children.length; index += 1) {
            ta_state_id child = ta_child_at(it, state, index);
            if (ta_state(it, child)->kind == TA_STATE_FINAL && ta_bits_get(it->active, child)) {
                return true;
            }
        }
        return false;
    }
    if (def->kind == TA_STATE_PARALLEL) {
        for (uint32_t index = 0; index < def->children.length; index += 1) {
            ta_state_id child = ta_child_at(it, state, index);
            if (!ta_is_history(it, child) && !ta_in_final_state(it, child)) {
                return false;
            }
        }
        return true;
    }
    return false;
}

/* Queues done.state.<state>; takes ownership of data, which may be empty. */
static ta_result ta_raise_done(ta_interpreter *it, ta_state_id state, ta_value *data) {
    ta_event event = {
        .name = ta_state(it, state)->done_event,
        .type = TA_EVENT_INTERNAL,
        .invoke_id = TA_INVOKE_ID_NONE,
        .data = *data,
    };
    *data = (ta_value){0};
    return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
}

/*
 * <donedata> <param>s: a param whose value fails is ignored after queueing
 * error.execution (SCXML 5.7); the rest still form the payload.
 */
static ta_result ta_done_data_params(ta_interpreter *it, ta_span32 params, ta_value *out) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_named_value *values = ta_arena_alloc(&it->system->scratch, params.length * sizeof(ta_named_value),
                                            _Alignof(ta_named_value));
    if (values == NULL) {
        return TA_ERR_NOMEM;
    }
    uint32_t count = 0;
    ta_result result = TA_OK;
    for (uint32_t index = 0; index < params.length && result == TA_OK; index += 1) {
        const ta_param_def *param = &definition->params[params.offset + index];
        ta_fault fault = {0};
        values[count].name = ta_atom_text(&it->system->atoms, param->name);
        if (ta_eval_source(it, &(ta_value_params){ .source = param->value, .out = &values[count].value,
                                                   .fault = &fault }) == TA_OP_OK) {
            count += 1;
        } else {
            result = ta_execution_error(it, &fault);
            result = result == TA_EXEC_FAILED ? TA_OK : result;
        }
    }
    ta_fault fault = {0};
    if (result == TA_OK && count != 0 &&
        (v->make_params == NULL ||
         v->make_params(definition->datamodel.self,
                        &(ta_datamodel_make_params_params){
                            .shared = definition->datamodel_shared,
                            .instance = it->actor->datamodel_instance,
                            .values = values,
                            .count = count,
                            .host = &it->actor->host,
                            .out = out,
                            .fault = &fault,
                        }) != TA_OP_OK)) {
        *out = (ta_value){0};
        result = ta_execution_error(it, &fault);
        result = result == TA_EXEC_FAILED ? TA_OK : result;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        ta_value_release(&values[index].value);
    }
    return result;
}

/*
 * Evaluates a final state's <donedata> into `out`. A failure queues
 * error.execution and leaves the data empty; the done event still follows.
 */
static ta_result ta_done_data_value(ta_interpreter *it, ta_done_data_id id, ta_value *out) {
    *out = (ta_value){0};
    if (id == TA_DONE_DATA_NONE) {
        return TA_OK;
    }
    const ta_definition *definition = it->definition;
    const ta_done_data_def *done = &definition->done_data[id];
    if (done->params.length != 0) {
        return ta_done_data_params(it, done->params, out);
    }
    ta_fault fault = {0};
    ta_op_status status = TA_OP_OK;
    if (done->content.kind == TA_VALUE_SOURCE_CONTENT) {
        const ta_datamodel_vtable *v = definition->datamodel.v;
        status = v->import_content == NULL
                     ? TA_OP_ERROR
                     : v->import_content(definition->datamodel.self,
                                         &(ta_datamodel_import_content_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .content = definition->contents[done->content.as.content].bytes,
                                             .host = &it->actor->host,
                                             .out = out,
                                             .fault = &fault,
                                         });
    } else {
        status = ta_eval_source(it, &(ta_value_params){ .source = done->content, .out = out, .fault = &fault });
    }
    if (status == TA_OP_OK) {
        return TA_OK;
    }
    *out = (ta_value){0};
    ta_result result = ta_execution_error(it, &fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

/* Keeps a top-level final state's done data as the actor's completion. */
static ta_result ta_store_completion(ta_interpreter *it, ta_value *data) {
    ta_actor_sparse_state *sparse = ta_sparse(it->system, it->actor);
    if (sparse == NULL) {
        ta_value_release(data);
        return TA_ERR_NOMEM;
    }
    ta_value_release(&sparse->completion.data);
    sparse->completion = (ta_actor_completion){ .has_data = data->v != NULL, .data = *data };
    *data = (ta_value){0};
    return TA_OK;
}

static ta_result ta_exit_states(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_exit);
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_exit_set(it, it->enabled[index], it->to_exit);
    }
    /* Record history before any exit changes the configuration. */
    for (ta_state_id history = 0; history < definition->state_count; history += 1) {
        const ta_state_def *def = ta_state(it, history);
        if (!ta_is_history(it, history) || !ta_bits_get(it->to_exit, def->parent)) {
            continue;
        }
        uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        ta_bits_clear(it, recorded);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (!ta_bits_get(it->active, state)) {
                continue;
            }
            bool keep = def->kind == TA_STATE_HISTORY_DEEP
                            ? ta_definition_is_atomic(definition, state) && ta_descends(it, state, def->parent)
                            : ta_state(it, state)->parent == def->parent;
            if (keep) {
                ta_bits_put(recorded, state, true);
            }
        }
    }
    for (ta_state_id index = definition->state_count; index > 0; index -= 1u) {
        ta_state_id state = index - 1u;
        if (!ta_bits_get(it->to_exit, state)) {
            continue;
        }
        ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
        if (result == TA_OK) {
            result = ta_invoke_state_exited(it, state); /* Invoke: cancel this state's invocations. */
        }
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
    }
    return TA_OK;
}

static ta_result ta_enter_states(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->to_enter);
    ta_bits_clear(it, it->default_entry);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        it->default_history[state] = TA_STATE_NONE;
    }
    for (uint32_t index = 0; index < it->enabled_count; index += 1) {
        ta_transition_id transition = it->enabled[index];
        ta_transition_view view = ta_view(it, transition);
        ta_state_id ancestor = TA_STATE_NONE;
        if (!ta_transition_domain(it, transition, &ancestor)) {
            continue;
        }
        for (uint32_t target = 0; target < view.targets.length; target += 1) {
            ta_add_descendants(it, ta_target_at(it, view.targets, target));
        }
        ta_bits_clear(it, it->entry_targets);
        ta_effective_targets(it, view.targets, it->entry_targets);
        for (ta_state_id state = 0; state < definition->state_count; state += 1) {
            if (ta_bits_get(it->entry_targets, state)) {
                ta_add_ancestors(it, state, ancestor);
            }
        }
    }
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (!ta_bits_get(it->to_enter, state)) {
            continue;
        }
        const ta_state_def *def = ta_state(it, state);
        ta_bits_put(it->active, state, true);
        ta_result result = ta_invoke_state_entered(it, state); /* Invoke: start after the macrostep. */
        if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
            ta_bits_put(it->actor->late_initialized, state, true);
            result = ta_initialize_data(it, state);
        }
        if (result == TA_OK) {
            result = ta_run_handlers(it, def->onentry_handlers);
        }
        if (result == TA_OK && ta_bits_get(it->default_entry, state)) {
            result = ta_run_block_isolated(it, definition->initial_transitions[def->initial].actions);
        }
        if (result == TA_OK && it->default_history[state] != TA_STATE_NONE) {
            const ta_state_def *history = ta_state(it, it->default_history[state]);
            result = ta_run_block_isolated(it, definition->transitions[history->transitions.offset].actions);
        }
        if (result != TA_OK) {
            return result;
        }
        if (def->kind != TA_STATE_FINAL) {
            continue;
        }
        ta_value data = {0};
        result = ta_done_data_value(it, def->done_data, &data);
        if (result != TA_OK) {
            return result;
        }
        if (def->parent == TA_STATE_NONE) {
            it->actor->finished = true;
            result = ta_store_completion(it, &data);
            if (result != TA_OK) {
                return result;
            }
            continue;
        }
        result = ta_raise_done(it, def->parent, &data);
        ta_state_id grandparent = ta_state(it, def->parent)->parent;
        if (result == TA_OK && grandparent != TA_STATE_NONE &&
            ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
            ta_value none = {0};
            result = ta_raise_done(it, grandparent, &none);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static void ta_load_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_bits_clear(it, it->active);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (!ta_configuration_contains_atomic(&it->actor->configuration, state)) {
            continue;
        }
        for (ta_state_id at = state; at != TA_STATE_NONE; at = ta_state(it, at)->parent) {
            ta_bits_put(it->active, at, true);
        }
    }
}

static void ta_store_active(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    ta_configuration_clear(&it->actor->configuration);
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        if (ta_bits_get(it->active, state) && ta_definition_is_atomic(definition, state)) {
            ta_configuration_set_atomic(&it->actor->configuration, state, true);
        }
    }
}

static ta_result ta_microstep(ta_interpreter *it) {
    ta_result result = ta_exit_states(it);
    for (uint32_t index = 0; result == TA_OK && index < it->enabled_count; index += 1) {
        result = ta_run_block_isolated(it, ta_view(it, it->enabled[index]).actions);
    }
    if (result == TA_OK) {
        result = ta_enter_states(it);
    }
    ta_store_active(it);
    it->steps += 1u;
    return result;
}

/* Exits every active state in exit order when a top-level final state is reached. */
static ta_result ta_finish(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    for (ta_state_id index = definition->state_count; index > 0; index -= 1u) {
        ta_state_id state = index - 1u;
        if (!ta_bits_get(it->active, state)) {
            continue;
        }
        ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
    }
    ta_store_active(it);
    /* Sends the router already refused once stay queued: they were sent before termination. */
    ta_timer_cancel_unsubmitted(&it->system->timers, it->actor->record.id);
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_event event;
    while (sparse != NULL && ta_dequeue_internal(it->actor, &event)) {
        ta_event_release(&event);
    }
    it->actor->macrostep = false;
    it->actor->record.residency = TA_ACTOR_DONE;
    /* Invoke: cancel this session's invocations and report done.invoke to its parent. */
    return ta_invoke_finished(it);
}

static ta_result ta_bootstrap(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    /* Top-level data is initialized at bootstrap under either binding (it belongs to <scxml>). */
    it->bound_decls = ta_arena_alloc(&it->system->scratch,
                                     (ta_bits_words(definition->data_decl_count) + 1u) * sizeof(uint64_t),
                                     _Alignof(uint64_t));
    if (it->bound_decls == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_zero(it->bound_decls, (ta_bits_words(definition->data_decl_count) + 1u) * sizeof(uint64_t));
    ta_result result = ta_declare_data(it);
    if (result == TA_OK) {
        result = ta_initialize_data(it, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < definition->state_count &&
                                definition->binding_mode == TA_BINDING_EARLY;
         state += 1) {
        result = ta_initialize_data(it, state);
    }
    if (result == TA_OK) {
        result = ta_run_block_isolated(it, definition->global_script_actions);
    }
    if (result == TA_OK) {
        it->enabled[0] = TA_TRANSITION_BOOTSTRAP;
        it->enabled_count = 1;
        result = ta_microstep(it);
    }
    it->bound_decls = NULL;
    it->actor->bootstrapped = true;
    return result;
}

static ta_result ta_interpreter_init(ta_interpreter *it, ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    uint32_t words = ta_bits_words(definition->state_count);
    *it = (ta_interpreter){ .system = system, .actor = actor, .definition = definition, .words = words };
    ta_arena_reset(&system->scratch);
    uint64_t **sets[] = {
        &it->active, &it->to_exit, &it->to_enter, &it->default_entry,
        &it->domain_targets, &it->entry_targets, &it->exit_left, &it->exit_right,
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(sets); index += 1) {
        *sets[index] = ta_arena_alloc(&system->scratch, words * sizeof(uint64_t), _Alignof(uint64_t));
        if (*sets[index] == NULL) {
            return TA_ERR_NOMEM;
        }
    }
    /* At most one transition per atomic state, plus the bootstrap transition. */
    uint32_t capacity = definition->state_count + 1u;
    it->default_history = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_state_id), _Alignof(ta_state_id));
    it->enabled = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->filtered = ta_arena_alloc(&system->scratch, capacity * sizeof(ta_transition_id), _Alignof(ta_transition_id));
    it->remove = ta_arena_alloc(&system->scratch, capacity * sizeof(bool), _Alignof(bool));
    if (it->default_history == NULL || it->enabled == NULL || it->filtered == NULL || it->remove == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_load_active(it);
    actor->live_active = it->active;
    return TA_OK;
}

static ta_result ta_interpreter_run(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
    ta_run_status *status = params->status;
    if (actor->finished) {
        *status = TA_RUN_DONE;
        return TA_OK;
    }
    ta_interpreter interpreter;
    ta_interpreter *it = &interpreter;
    ta_result result = ta_interpreter_init(it, system, actor);
    if (result == TA_OK && !actor->bootstrapped) {
        result = ta_bootstrap(it);
    }
    while (result == TA_OK && !actor->finished) {
        if (it->steps >= params->budget) {
            *status = TA_RUN_PAUSED;
            return TA_OK;
        }
        result = ta_select_transitions(it, NULL);
        if (result != TA_OK) {
            break;
        }
        if (it->enabled_count != 0) {
            result = ta_microstep(it);
            continue;
        }
        ta_event event;
        if (ta_dequeue_internal(actor, &event)) {
            ta_set_current_event(actor, &event);
        } else if ((result = ta_invoke_start_pending(it)) != TA_OK) {
            break; /* Invoke: the macrostep ended, so start its states' invocations. */
        } else if (ta_interpreter_internal_pending(actor)) {
            continue;
        } else if (actor->has_external_event) {
            actor->has_external_event = false;
            ta_set_current_event(actor, &actor->external_event);
            /* Invoke: <finalize> and autoforwarding. */
            result = ta_invoke_before_external(it);
            if (result != TA_OK) {
                break;
            }
        } else {
            actor->macrostep = false;
            actor->record.residency = TA_ACTOR_IDLE;
            *status = TA_RUN_STABLE;
            return TA_OK;
        }
        result = ta_select_transitions(it, &actor->current_event);
        if (result == TA_OK && it->enabled_count != 0) {
            result = ta_microstep(it);
        } else {
            it->steps += 1u;
        }
    }
    if (result != TA_OK) {
        *status = TA_RUN_ERROR;
        return result;
    }
    result = ta_finish(it);
    *status = result == TA_OK ? TA_RUN_DONE : TA_RUN_ERROR;
    return result;
}

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
    ta_result result = ta_interpreter_run(system, actor, params);
    /* The live set lives in scratch memory; between calls In() derives from the configuration. */
    actor->live_active = NULL;
    return result;
}

/* ------------------------------------------------------------------------
 * Services for invoke.c
 */

TA_PRIVATE ta_actor_sparse_state *ta_interpreter_sparse(ta_system *system, ta_actor *actor) {
    return ta_sparse(system, actor);
}

TA_PRIVATE ta_op_status ta_interpreter_eval(ta_interpreter *it, const ta_interpreter_value_params *params) {
    return ta_eval_source(it, &(ta_value_params){ .source = params->source, .out = params->out,
                                                  .fault = params->fault });
}

TA_PRIVATE ta_result ta_interpreter_execution_error(ta_interpreter *it, ta_fault *fault) {
    ta_result result = ta_execution_error(it, fault);
    return result == TA_EXEC_FAILED ? TA_OK : result;
}

TA_PRIVATE ta_result ta_interpreter_run_actions(ta_interpreter *it, ta_span32 block) {
    return ta_run_block_isolated(it, block);
}

TA_PRIVATE bool ta_interpreter_internal_pending(const ta_actor *actor) {
    return actor->sparse != NULL && actor->sparse->internal_events.count != 0;
}

TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    if (actor->has_external_event) {
        ta_event_release(&actor->external_event);
        actor->has_external_event = false;
    }
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
        actor->has_current_event = false;
    }
    if (actor->sparse == NULL) {
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_event event;
    while (ta_dequeue_internal(actor, &event)) {
        ta_event_release(&event);
    }
    ta_vec_destroy(&actor->sparse->internal_events);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}
