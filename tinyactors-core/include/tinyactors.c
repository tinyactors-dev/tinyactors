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



/* External delivery and the router boundary are fully public. */



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
/* Removes and releases every pending send of owner with send_id. */
TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id);
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

/* LITERAL is a duration parsed at compile time, in ta_time milliseconds. */
typedef enum {
    TA_DELAY_SOURCE_NONE,
    TA_DELAY_SOURCE_LITERAL,
    TA_DELAY_SOURCE_EXPR
} ta_delay_source_kind;

typedef struct {
    ta_delay_source_kind kind;
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
    TA_ACTION_INVOKE,
    TA_ACTION_EXTENSION
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
        ta_extension_id extension;
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
    ta_span32 onentry_actions;
    ta_span32 onexit_actions;
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
} ta_transition_def;

typedef struct {
    ta_state_id owner;
    ta_span32 targets;
    ta_span32 actions;
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
    ta_datamodel_binding_mode binding_mode;
    ta_datamodel datamodel;
    void *datamodel_shared;
    bool datamodel_prepared;
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
};

TA_PRIVATE const ta_definition *ta_definition_lookup(const ta_system *system, ta_definition_id id);
TA_PRIVATE bool ta_definition_is_atomic(const ta_definition *definition, ta_state_id state);
TA_PRIVATE bool ta_definition_is_descendant(const ta_definition *definition, ta_state_id state, ta_state_id ancestor);
TA_PRIVATE bool ta_event_pattern_matches(const ta_definition *definition,
                              const ta_event_pattern *pattern,
                              ta_event_name_id event);



/* The declarative specification tree and ta_tree are fully public. */

/* Parses an SCXML duration ("500ms", "2.5s") into logical milliseconds. */
TA_PRIVATE ta_result ta_duration_parse(ta_str text, ta_time *out);



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



struct ta_system {
    ta_allocator allocator;
    ta_clock clock;
    ta_atom_table atoms;
    ta_event_name_table event_names;

    /* Registered definitions; ta_definition_id indexes this array. */
    ta_definition **definitions;
    uint32_t definition_count;
    uint32_t definition_capacity;

    /* ta_actor_slot; an actor ID is (generation << 32) | slot. */
    ta_vec actors;

    ta_timer_heap timers;

    ta_vec io_processors; /* ta_io_registration */
    ta_vec invokers;      /* ta_invoker_registration */

    ta_actor_directory directory;
    ta_external_event_router external_events;
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    ta_arena scratch;
};

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
} ta_interpreter;

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/* Takes ownership of the event's data. */
TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send);
TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);


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



static void ta_address_release(ta_actor_ref *ref, ta_allocator allocator) {
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

TA_API ta_result ta_external_delivery_clone(const ta_external_delivery *delivery,
                                     ta_allocator allocator,
                                     ta_external_delivery *out) {
    if (delivery == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_external_delivery copy = *delivery;
    ta_result result = TA_OK;
    if (copy.destination.kind == TA_ACTOR_REF_NAMED) {
        result = ta_address_copy_text(&copy.destination.as.named.family.name, allocator);
        if (result == TA_OK) {
            result = ta_address_copy_text(&copy.destination.as.named.name, allocator);
        } else {
            copy.destination.as.named.name = (ta_str){0};
        }
    }
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

TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id) {
    bool found = false;
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == owner && send->send_id == send_id) {
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
    if (system == NULL || id >= system->definition_count) {
        return NULL;
    }
    return system->definitions[id];
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
        return TA_ERR_INVALID;
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
    ta_vec params;       /* ta_param_def */
    ta_vec data_decls;   /* ta_data_decl */
    ta_vec initializers; /* ta_initializer_def */
    ta_vec contents;     /* ta_content_def */
    ta_vec patterns;     /* ta_event_pattern */
    ta_vec segments;     /* ta_event_pattern_segment */
    ta_vec expressions;  /* ta_expression */
    ta_vec history;      /* ta_history_slot */
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
        default:
            /* idlocation needs a data-model value made from a generated ID. */
            return TA_ERR_UNSUPPORTED;
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
        default:
            return TA_ERR_UNSUPPORTED;
    }
    if (result != TA_OK) {
        return result;
    }
    if (spec->event.kind != TA_SCXML_EVENT_NAME) {
        return spec->event.kind == TA_SCXML_EVENT_EXPR ? TA_ERR_UNSUPPORTED : TA_ERR_INVALID;
    }
    out->event.kind = TA_EVENT_SOURCE_NAME;
    ta_compiler_hash_str(compiler, spec->event.as.name);
    result = ta_event_name_intern_dotted(&compiler->system->event_names, spec->event.as.name,
                                         &out->event.as.name);
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
            break;
        default:
            /* delayexpr needs a data-model value converted to a duration. */
            return TA_ERR_UNSUPPORTED;
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
                /* sendidexpr needs a data-model value converted to a send ID. */
                return TA_ERR_UNSUPPORTED;
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
        case TA_SCXML_ACTION_INVOKE:
        case TA_SCXML_ACTION_EXTENSION:
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
        for (uint32_t at = start; at <= index; at += 1) {
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
    if (spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        return TA_ERR_UNSUPPORTED;
    }
    if (spec->done_data != NULL) {
        return TA_ERR_UNSUPPORTED;
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
        result = ta_compiler_block(compiler, spec->onentry, &state.onentry_actions);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, spec->onexit, &state.onexit_actions);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
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
        &compiler->initials, &compiler->targets, &compiler->actions, &compiler->params,
        &compiler->data_decls, &compiler->initializers, &compiler->contents, &compiler->patterns,
        &compiler->segments, &compiler->expressions, &compiler->history,
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
        { &compiler->params, (void **)&definition->params, &definition->param_count },
        { &compiler->data_decls, (void **)&definition->data_decls, &definition->data_decl_count },
        { &compiler->initializers, (void **)&definition->initializers, &definition->initializer_count },
        { &compiler->contents, (void **)&definition->contents, &definition->content_count },
        { &compiler->patterns, (void **)&definition->event_patterns, &definition->event_pattern_count },
        { &compiler->segments, (void **)&definition->event_pattern_segments, &definition->event_pattern_segment_count },
        { &compiler->history, (void **)&definition->history.slots, &definition->history.slot_count },
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
        { &compiler.params, TA_VEC_LAYOUT(ta_param_def) },
        { &compiler.data_decls, TA_VEC_LAYOUT(ta_data_decl) },
        { &compiler.initializers, TA_VEC_LAYOUT(ta_initializer_def) },
        { &compiler.contents, TA_VEC_LAYOUT(ta_content_def) },
        { &compiler.patterns, TA_VEC_LAYOUT(ta_event_pattern) },
        { &compiler.segments, TA_VEC_LAYOUT(ta_event_pattern_segment) },
        { &compiler.expressions, TA_VEC_LAYOUT(ta_expression) },
        { &compiler.history, TA_VEC_LAYOUT(ta_history_slot) },
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

static void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    if (actor->datamodel_instance != NULL) {
        ta_mem_free(system->allocator, actor->datamodel_instance,
                    &(ta_allocator_free_params){ .size = definition->datamodel_instance_size,
                                                 .alignment = definition->datamodel_instance_alignment });
    }
    ta_mem_free(system->allocator, actor->session_storage,
                &(ta_allocator_free_params){ .size = actor->session_bytes, .alignment = _Alignof(uint64_t) });
    ta_interpreter_release_actor_state(system, actor);
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor), .alignment = _Alignof(ta_actor) });
}

TA_API ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_definition *definition = ta_definition_lookup(system, params->definition);
    if (definition == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->address != NULL) {
        /* Named actors require the directory protocol, which is not implemented yet. */
        return TA_ERR_UNSUPPORTED;
    }
    ta_actor *actor = ta_mem_alloc(system->allocator, sizeof(ta_actor), _Alignof(ta_actor));
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    size_t history_bytes = definition->history.storage_bytes;
    actor->session_bytes = config_bytes * 2u + history_bytes;
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
    actor->datamodel_instance = ta_mem_alloc(system->allocator, instance_size == 0 ? 1u : instance_size,
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
        ta_actor_slot *entry = ta_vec_at(&system->actors, (uint32_t)(id & 0xffffffffu));
        entry->actor = NULL;
        entry->generation += 1u;
        ta_actor_free(system, actor);
        return result;
    }
    actor->datamodel_initialized = true;
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
    ta_timer_cancel_owner(&system->timers, id);
    ta_actor_slot *entry = ta_vec_at(&system->actors, (uint32_t)(id & 0xffffffffu));
    entry->actor = NULL;
    entry->generation += 1u;
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
    };
    if (actor->sparse != NULL) {
        out->internal_events = actor->sparse->internal_events.items;
        out->internal_count = actor->sparse->internal_events.count;
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
    }
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->inspect != NULL &&
        v->inspect(definition->datamodel.self,
                   &(ta_datamodel_inspect_params){
                       .shared = definition->datamodel_shared,
                       .instance = actor->datamodel_instance,
                       .host = &actor->host,
                       .out = &out->datamodel_root,
                   }) == TA_OK) {
        out->has_datamodel_root = true;
    } else {
        out->datamodel_root = (ta_value){0};
    }
    return TA_OK;
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
            if (inspection.has_datamodel_root) {
                ta_value_release(&inspection.datamodel_root);
            }
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
    /* <donedata> is not compiled yet, so completion never carries data. */
    *out = (ta_actor_completion){ .has_data = false };
    return TA_OK;
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
    for (uint32_t index = 0; index < system->definition_count; index += 1) {
        ta_definition_destroy(system->definitions[index]);
    }
    ta_mem_free(system->allocator, system->definitions,
                &(ta_allocator_free_params){ .size = system->definition_capacity * sizeof(ta_definition *),
                                             .alignment = _Alignof(ta_definition *) });
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
    if (system->definition_count == system->definition_capacity) {
        uint32_t capacity = system->definition_capacity == 0 ? 8u : system->definition_capacity * 2u;
        ta_definition **grown = ta_mem_alloc(system->allocator, capacity * sizeof(ta_definition *),
                                             _Alignof(ta_definition *));
        if (grown == NULL) {
            return TA_ERR_NOMEM;
        }
        if (system->definitions != NULL) {
            ta_mem_copy(grown, system->definitions, system->definition_count * sizeof(ta_definition *));
            ta_mem_free(system->allocator, system->definitions,
                        &(ta_allocator_free_params){ .size = system->definition_capacity * sizeof(ta_definition *),
                                                     .alignment = _Alignof(ta_definition *) });
        }
        system->definitions = grown;
        system->definition_capacity = capacity;
    }
    definition->id = system->definition_count;
    definition->registered = true;
    system->definitions[system->definition_count] = definition;
    system->definition_count += 1;
    *out = definition->id;
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
        default:
            /* Literal content needs a data-model import operation, which the vtable lacks. */
            return TA_OP_ERROR;
    }
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
        if (decl->owner_state != owner || decl->initializer == TA_INITIALIZER_NONE) {
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
        ta_op_status status = v->declare(definition->datamodel.self,
                                         &(ta_datamodel_declare_params){
                                             .shared = definition->datamodel_shared,
                                             .instance = it->actor->datamodel_instance,
                                             .name = ta_atom_text(&it->system->atoms,
                                                                  definition->data_decls[index].name),
                                             .host = &it->actor->host,
                                             .fault = &fault,
                                         });
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

/* Parses the standard lexical SCXML Event I/O Processor targets. */
static bool ta_parse_scxml_target(ta_bytes text, ta_scxml_target *out) {
    ta_str target = { .ptr = (const char *)text.ptr, .len = text.len };
    static const struct { const char *text; ta_scxml_target_kind kind; } fixed[] = {
        { "#_internal", TA_SCXML_TARGET_INTERNAL },
        { "#_parent", TA_SCXML_TARGET_PARENT },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(fixed); index += 1) {
        ta_str candidate = { .ptr = fixed[index].text, .len = 0 };
        while (candidate.ptr[candidate.len] != '\0') {
            candidate.len += 1;
        }
        if (ta_str_equal(target, candidate)) {
            *out = (ta_scxml_target){ .kind = fixed[index].kind };
            return true;
        }
    }
    uint32_t prefix = 0;
    ta_scxml_target_kind kind = TA_SCXML_TARGET_INVOKE;
    if (target.len > 8 && ta_str_equal((ta_str){ .ptr = target.ptr, .len = 8 }, (ta_str){ .ptr = "#_scxml_", .len = 8 })) {
        prefix = 8;
        kind = TA_SCXML_TARGET_ACTOR;
    } else if (target.len > 2 && target.ptr[0] == '#' && target.ptr[1] == '_') {
        prefix = 2;
    } else {
        return false;
    }
    uint64_t number = 0;
    for (uint32_t index = prefix; index < target.len; index += 1) {
        char digit = target.ptr[index];
        if (digit < '0' || digit > '9' || number > (UINT64_MAX - 9u) / 10u) {
            return false;
        }
        number = number * 10u + (uint64_t)(digit - '0');
    }
    *out = (ta_scxml_target){ .kind = kind };
    if (kind == TA_SCXML_TARGET_ACTOR) {
        out->as.actor = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = number };
    } else {
        out->as.invoke = number;
    }
    return true;
}

typedef struct {
    ta_actor *sender;
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
            .origin = { .kind = TA_ORIGIN_ACTOR, .as.actor = params->sender->record.self },
            .invoke_id = TA_INVOKE_ID_NONE,
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
    if (sender == NULL) {
        status = TA_EXTERNAL_EVENT_ACCEPTED; /* the sender's session is gone; drop the send */
    } else if (ta_system_is_scxml_io(system, send->io_type)) {
        status = ta_submit_external(system, &(ta_submit_params){
                                                .sender = sender,
                                                .destination = send->destination,
                                                .event = send->event,
                                                .send_id = send->send_id,
                                                .data = &send->data,
                                                .fault = &fault,
                                            });
    } else {
        const ta_io *io = ta_system_find_io(system, send->io_type);
        ta_send request = {
            .source = sender->record.self,
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
    if (status == TA_EXTERNAL_EVENT_ERROR) {
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
        values[evaluated].name = param->name;
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
        if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
            return TA_OP_ERROR; /* a literal target would need a data-model import */
        }
        return ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target, .fault = fault });
    }
    if (send->target.kind == TA_VALUE_SOURCE_NONE) {
        state->scxml = (ta_scxml_target){ .kind = TA_SCXML_TARGET_SELF };
        return TA_OP_OK;
    }
    if (send->target.kind == TA_VALUE_SOURCE_CONTENT) {
        return ta_parse_scxml_target(it->definition->contents[send->target.as.content].bytes, &state->scxml)
                   ? TA_OP_OK
                   : TA_OP_ERROR;
    }
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    ta_op_status status = ta_eval_source(it, &(ta_value_params){ .source = send->target, .out = &state->target,
                                                                 .fault = fault });
    if (status == TA_OP_OK) {
        status = v->decode_scxml_target == NULL
                     ? TA_OP_ERROR
                     : v->decode_scxml_target(definition->datamodel.self,
                                              &(ta_datamodel_decode_scxml_target_params){
                                                  .shared = definition->datamodel_shared,
                                                  .instance = it->actor->datamodel_instance,
                                                  .value = &state->target,
                                                  .host = &it->actor->host,
                                                  .out = &state->scxml,
                                                  .fault = fault,
                                              });
    }
    if (status == TA_OP_OK && state->scxml.kind == TA_SCXML_TARGET_ACTOR &&
        state->scxml.as.actor.kind == TA_ACTOR_REF_NAMED) {
        /* Named-address text is borrowed from the target value; copying it is not implemented yet. */
        status = TA_OP_ERROR;
    }
    return status;
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
        default:
            return false; /* invocations are not implemented yet */
    }
}

static ta_result ta_exec_send(ta_interpreter *it, const ta_send_action *send) {
    ta_system *system = it->system;
    ta_actor *actor = it->actor;
    ta_send_state state = { .io_type = system->scxml_io_type, .scxml_io = true };
    ta_fault fault = {0};
    if (send->type.kind == TA_TEXT_SOURCE_ATOM) {
        state.io_type = send->type.as.atom;
        state.scxml_io = ta_system_is_scxml_io(system, state.io_type);
        if (state.scxml_io) {
            state.io_type = system->scxml_io_type;
        } else if (ta_system_find_io(system, state.io_type) == NULL) {
            return ta_execution_error(it, NULL);
        }
    }
    if (send->id.kind == TA_ID_SOURCE_LITERAL) {
        state.send_id = ta_literal_send_id(send->id.as.literal);
    } else {
        actor->next_send_id += 1u;
        state.send_id = ((ta_send_id)1u << 63) | actor->next_send_id;
    }
    ta_op_status status = ta_send_payload(it, send, &state, &fault);
    if (status == TA_OP_OK) {
        status = ta_send_target(it, send, &state, &fault);
    }
    ta_time delay = send->delay.kind == TA_DELAY_SOURCE_LITERAL ? send->delay.as.literal : 0;
    if (status == TA_OP_OK && state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL && delay != 0) {
        status = TA_OP_ERROR; /* a delayed send cannot target the internal queue */
    }
    if (status != TA_OP_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        return ta_execution_error(it, &fault);
    }

    if (state.scxml_io && state.scxml.kind == TA_SCXML_TARGET_INTERNAL) {
        ta_value_release(&state.target);
        ta_event event = {
            .name = send->event.as.name,
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
        .event = send->event.as.name,
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
        case TA_ACTION_CANCEL:
            (void)ta_timer_cancel(&it->system->timers, it->actor->record.id,
                                  ta_literal_send_id(action->as.cancel.sendid.as.atom));
            return TA_OK;
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
        case TA_ACTION_LOG:
            /* There is no logging sink yet; the expression still runs for its errors. */
            if (action->as.log.has_expr) {
                ta_value value = {0};
                status = ta_eval_source(it, &(ta_value_params){
                                                .source = { .kind = TA_VALUE_SOURCE_EXPR, .as.expr = action->as.log.expr },
                                                .out = &value,
                                                .fault = &fault,
                                            });
                ta_value_release(&value);
            }
            break;
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

static ta_result ta_raise_done(ta_interpreter *it, ta_state_id state) {
    ta_event event = {
        .name = ta_state(it, state)->done_event,
        .type = TA_EVENT_INTERNAL,
        .invoke_id = TA_INVOKE_ID_NONE,
    };
    return ta_interpreter_enqueue_internal(it->system, it->actor, &event);
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
        ta_result result = ta_run_block_isolated(it, ta_state(it, state)->onexit_actions);
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
        ta_result result = TA_OK;
        if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
            ta_bits_put(it->actor->late_initialized, state, true);
            result = ta_initialize_data(it, state);
        }
        if (result == TA_OK) {
            result = ta_run_block_isolated(it, def->onentry_actions);
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
        if (def->parent == TA_STATE_NONE) {
            it->actor->finished = true;
            continue;
        }
        result = ta_raise_done(it, def->parent);
        ta_state_id grandparent = ta_state(it, def->parent)->parent;
        if (result == TA_OK && grandparent != TA_STATE_NONE &&
            ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
            result = ta_raise_done(it, grandparent);
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
        ta_result result = ta_run_block_isolated(it, ta_state(it, state)->onexit_actions);
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
    }
    ta_store_active(it);
    ta_timer_cancel_owner(&it->system->timers, it->actor->record.id);
    ta_actor_sparse_state *sparse = it->actor->sparse;
    ta_event event;
    while (sparse != NULL && ta_dequeue_internal(it->actor, &event)) {
        ta_event_release(&event);
    }
    it->actor->macrostep = false;
    it->actor->record.residency = TA_ACTOR_DONE;
    return TA_OK;
}

static ta_result ta_bootstrap(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    /* Top-level data is initialized at bootstrap under either binding (it belongs to <scxml>). */
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
    return TA_OK;
}

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params) {
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
        } else if (actor->has_external_event) {
            actor->has_external_event = false;
            ta_set_current_event(actor, &actor->external_event);
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
    ta_event event;
    while (ta_dequeue_internal(actor, &event)) {
        ta_event_release(&event);
    }
    ta_vec_destroy(&actor->sparse->internal_events);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}
