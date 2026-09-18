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



struct ta_arena_block {
    struct ta_arena_block *next;
    size_t allocation_size;
    size_t alignment;
    size_t capacity;
    size_t used;
    unsigned char data[];
};



typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_span32 text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_atom_entry *entries;
    uint32_t entry_count;
    uint32_t entry_capacity;
    char *strings;
    uint32_t strings_len;
    uint32_t strings_capacity;
} ta_atom_table;

ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
void ta_atom_table_destroy(ta_atom_table *table);
ta_atom ta_atom_intern(ta_atom_table *table, ta_str text);
ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);



typedef struct {
    ta_event_name_id parent;
    ta_atom segment;
    uint16_t depth;
    uint16_t flags;
} ta_event_name_node;

typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    ta_event_name_node *nodes;
    uint32_t node_count;
    uint32_t node_capacity;
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

ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
void ta_event_name_table_destroy(ta_event_name_table *table);
ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
bool ta_event_name_matches_internal(const ta_event_name_table *table,
                                    ta_event_name_id event,
                                    ta_event_name_id prefix);



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

bool ta_configuration_contains_atomic(const ta_configuration *config,
                                      ta_state_id atomic_state);
void ta_configuration_set_atomic(ta_configuration *config,
                                 ta_state_id atomic_state,
                                 bool active);
void ta_configuration_clear(ta_configuration *config);



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



typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_send_id send_id;
    ta_time due;
    ta_event_name_id event;
    ta_atom io_type;
    ta_value target;
    ta_value data;
    uint32_t heap_index;
    bool active;
} ta_delayed_send;

typedef struct {
    ta_allocator allocator;
    ta_delayed_send *items;
    uint32_t count;
    uint32_t capacity;
} ta_timer_heap;

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
void ta_timer_heap_destroy(ta_timer_heap *heap);
ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send);
ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id);
const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap);
ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out);



typedef struct {
    ta_system *system;
    ta_actor_id actor;
    const ta_event *current_event;
    const ta_configuration *configuration;
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
    ta_action_id *actions;
    uint32_t action_count;
    ta_action_def *action_defs;
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
    uint32_t datamodel_instance_size;
    uint32_t datamodel_instance_alignment;
};



/* The declarative specification tree and ta_tree are fully public. */



typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_actor_ref self;
    ta_actor_residency residency;
} ta_actor_record;

typedef struct ta_activation_effects ta_activation_effects;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
    ta_queued_event *internal_events;
    uint32_t internal_count;
    uint32_t internal_capacity;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    bool has_parent;
    bool completion_pending;
} ta_actor_sparse_state;

typedef struct {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration and history. */
    void *session_storage;
    ta_configuration configuration;
    ta_history_state history;
    /* Null while stable; allocated only for an in-progress macrostep. */
    ta_continuation *continuation;
    void *datamodel_instance;
    ta_actor_sparse_state *sparse;
    ta_actor_id replacement_of;
    uint64_t session_epoch;
    bool published;
} ta_actor;




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

    ta_definition **definitions;
    uint32_t definition_count;
    uint32_t definition_capacity;

    ta_actor_record *actors;
    uint32_t actor_count;
    uint32_t actor_capacity;

    ta_actor **resident;
    uint32_t resident_count;
    uint32_t resident_capacity;

    ta_timer_heap timers;

    ta_io_registration *io_processors;
    uint32_t io_count;
    uint32_t io_capacity;

    ta_invoker_registration *invokers;
    uint32_t invoker_count;
    uint32_t invoker_capacity;

    ta_actor_directory directory;
    ta_external_event_router external_events;
    ta_atom scxml_io_type;
    ta_arena scratch;
};



typedef struct {
    ta_system *system;
    ta_actor *actor;
    const ta_definition *definition;
    ta_dm_host dm_host;
    uint32_t budget;
    uint32_t steps;
} ta_interpreter;

ta_result ta_interpreter_deliver(ta_interpreter *it, const ta_event *event);
ta_result ta_interpreter_resume(ta_interpreter *it, ta_run_status *status);
ta_result ta_interpreter_microstep(ta_interpreter *it);
ta_result ta_interpreter_select_transitions(ta_interpreter *it);
ta_result ta_interpreter_exit_states(ta_interpreter *it);
ta_result ta_interpreter_execute_transitions(ta_interpreter *it);
ta_result ta_interpreter_enter_states(ta_interpreter *it);
ta_result ta_interpreter_process_internal(ta_interpreter *it);



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



