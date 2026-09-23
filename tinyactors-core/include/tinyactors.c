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

/* `value` rounded up to a multiple of `alignment`, which is never zero. */
TA_PRIVATE inline size_t ta_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}

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



/*
 * Interned named addresses (ADR 0065). A system keeps one record per
 * distinct (family, name) pair that anyone holds; a record is freed with its
 * last reference.
 *
 * The table is a linear hash table (Litwin): it grows one bucket at a time,
 * each insert splitting at most one bucket, so no single operation's cost
 * grows with the number of addresses. Buckets live in fixed pages found
 * through a small page directory, as the actor table's slots do (ADR 0063),
 * so growing never copies the buckets. A failed page allocation only skips
 * a split; the only allocation that can fail an intern is the record's own.
 */

#define TA_ADDRESS_PAGE_BUCKETS 1024u
#define TA_ADDRESS_INITIAL_BUCKETS 16u

typedef struct ta_address_table ta_address_table;

struct ta_address {
    /* Null for a detached record: a session image's own copy (see below). */
    ta_address_table *table;
    struct ta_address *next;
    uint32_t references;
    uint32_t hash;
    uint32_t family_len;
    uint32_t name_len;
    /* The family, a NUL, the name, a NUL. */
    char text[];
};

struct ta_address_table {
    ta_allocator allocator;
    ta_vec pages; /* ta_address ** pages of TA_ADDRESS_PAGE_BUCKETS buckets */
    /* Linear hashing: `base` buckets hashed with base - 1, and the first
     * `split` of them already split with 2 * base - 1. */
    uint32_t base;
    uint32_t split;
    uint32_t count;
};

TA_PRIVATE void ta_address_table_init(ta_address_table *table, ta_allocator allocator);
/* Frees every record, held or not; references held past this are dangling. */
TA_PRIVATE void ta_address_table_destroy(ta_address_table *table);
TA_PRIVATE ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out);

/*
 * A detached copy in `arena`: the same strings, no table, no counting.
 * Retain and release do nothing on it. Session images carry their addresses
 * this way, so an image does not depend on the system that captured it
 * (ADR 0028); loading interns them again.
 */
TA_PRIVATE ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out);
TA_PRIVATE bool ta_address_in(const ta_address *address, const ta_address_table *table);



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



/* Events, faults, and queued events are fully public. */

/* ta_event_retain() and ta_event_release() are public (ADR 0050, ADR 0065). */



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
 * A named ta_actor_ref is a reference to an interned address (ADR 0065).
 * An owner retains it and releases it; both ignore a runtime ref.
 */
TA_PRIVATE void ta_actor_ref_retain(const ta_actor_ref *ref);
TA_PRIVATE void ta_actor_ref_release(ta_actor_ref *ref);

/*
 * Makes a named ref one of this system's handles: a detached address (from a
 * session image) is interned and the ref then holds a new reference. A ref
 * that is already this system's, or a runtime ref, is left alone.
 */
TA_PRIVATE ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref);
/* Detaches a named ref into `arena`, for an image; a runtime ref is left alone. */
TA_PRIVATE ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref);
/* The origin of an event, when it is an actor. */
TA_PRIVATE ta_actor_ref *ta_event_origin_ref(ta_event *event);



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
    TA_EXEC_FRAME_ACTIONS = 0,
    TA_EXEC_FRAME_FOREACH
} ta_exec_frame_kind;

/*
 * One open block of executable content, or one running <foreach> (ADR 0060).
 * ACTIONS: `actions` is the block and `cursor` the next action in it.
 * FOREACH: `actions` is the body, `cursor` the <foreach> action itself, and
 * the snapshot, position and count are the loop's (ADR 0033).
 */
typedef struct {
    ta_exec_frame_kind kind;
    ta_span32 actions;
    uint32_t cursor;
    uint32_t iteration;
    uint32_t iteration_count;
    ta_value iteration_snapshot;
} ta_exec_frame;

/* Where a microstep stands (ADR 0061 section 2). */
typedef enum {
    TA_MICRO_NONE = 0,
    TA_MICRO_EXIT,
    TA_MICRO_TRANSITIONS,
    TA_MICRO_ENTER
} ta_micro_phase;

/* The stages of one state inside a phase. */
enum {
    TA_EXIT_HANDLERS = 0,
    TA_EXIT_HANDLERS_DONE = 1
};
enum {
    TA_ENTER_ACTIVATE = 0,
    TA_ENTER_HANDLERS = 1,
    TA_ENTER_INITIAL = 2,
    TA_ENTER_HISTORY = 3,
    TA_ENTER_DONE = 4
};

typedef struct {
    ta_micro_phase phase;
    /*
     * EXIT: the states still to exit are the exit set below this ID, highest
     * first. TRANSITIONS: the index of the enabled transition whose content
     * runs. ENTER: the next state to consider, lowest first.
     */
    uint32_t state;
    uint32_t stage;
    /* The <onentry>/<onexit> handler being run. */
    uint32_t handler;
    /* The executor's frames hold an open block that the current step resumes. */
    bool block_open;
    /* Where the microstep's event came from, for the TA_ERR_NOMEM path. */
    bool restore_internal;
    bool restore_external;
} ta_micro_position;

/*
 * A microstep paused inside executable content, moved out of the
 * interpreter's scratch into one allocation the actor owns (ADR 0061). The
 * header is followed, at the offsets the accessors compute, by the four
 * state sets (active, to exit, to enter, default entry; `words` each), the
 * default-history map (`state_count`), the enabled transitions, and the
 * executor's frames. It holds no pointers, so copying `bytes` copies it;
 * the frames' snapshots are values a copy must retain.
 */
typedef struct ta_continuation {
    size_t bytes;
    ta_micro_position at;
    uint32_t words;
    uint32_t state_count;
    uint32_t enabled_count;
    uint32_t frame_count;
} ta_continuation;

/* The bytes a continuation of this shape takes; `shape` has the counts set. */
TA_PRIVATE size_t ta_continuation_bytes(const ta_continuation *shape);
TA_PRIVATE uint64_t *ta_continuation_sets(ta_continuation *continuation);
TA_PRIVATE ta_state_id *ta_continuation_history(ta_continuation *continuation);
TA_PRIVATE ta_transition_id *ta_continuation_enabled(ta_continuation *continuation);
TA_PRIVATE ta_exec_frame *ta_continuation_frames(ta_continuation *continuation);
/* The alignment every continuation allocation needs. */
#define TA_CONTINUATION_ALIGN _Alignof(ta_exec_frame)

/* Frees a continuation and releases its frames' snapshots; null is fine. */
TA_PRIVATE void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation);
/* Releases the snapshots of a continuation's frames, and nothing else. */
TA_PRIVATE void ta_continuation_release(ta_continuation *continuation);
/* Retains the snapshots of a continuation's frames, for a copy of it. */
TA_PRIVATE void ta_continuation_retain(ta_continuation *continuation);



/*
 * Delayed sends (ADR 0026, ADR 0054).
 *
 * A send is stored with its owner: each owner has a ta_timer_list, one array
 * of public ta_delayed_send_info sorted by (due, timer id) plus a parallel
 * array of private fields. The system keeps a min-heap of the lists that hold
 * at least one send, keyed by each list's front. The heap's top list's front
 * is the globally earliest send, so dispatch order is exactly (due, id).
 *
 * A list's address must stay stable while it holds sends: the heap points at
 * it. Actors keep theirs in their sparse state; the sends whose session is
 * gone live in the heap's own `orphans` list.
 */

/*
 * An evaluated delayed send, the flat form used to schedule a send, to hand a
 * due send to dispatch, and to carry sends in a session image. For the
 * built-in SCXML processor `destination` is the resolved external target;
 * custom processors receive `target`.
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
    /*
     * A send to a host I/O processor with literal <content>: the content's ID
     * in the owner's definition, not its bytes, so a session image restores
     * it against its exact revision (ADR 0053, ADR 0038). The processor gets
     * the bytes when the send is dispatched.
     */
    bool has_content;
    ta_content_id content;
    /*
     * Already sent: the router accepted nothing and the send waits here to be
     * offered again. <cancel> and session termination leave it in place.
     */
    bool submitted;
    /* Submitted sends only: the envelope, which outlives the sending session. */
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
} ta_delayed_send;

/* The private fields of one stored send, parallel to ta_timer_list.sends. */
typedef struct {
    ta_timer_id id;
    ta_actor_id owner;
    ta_actor_ref origin;
    ta_invoke_id invoke_id;
    /* See ta_delayed_send.content. */
    bool has_content;
    ta_content_id content;
} ta_delayed_send_meta;

/* One owner's pending sends, sorted by (due, id). Zero-initialized is empty. */
typedef struct {
    ta_delayed_send_info *sends;
    ta_delayed_send_meta *meta;
    uint32_t count;
    uint32_t capacity;
    /* Position in the owner heap; meaningful only while `indexed`. */
    uint32_t heap_index;
    /* In the owner heap: true exactly when count != 0. */
    bool indexed;
} ta_timer_list;

typedef struct {
    ta_allocator allocator;
    /* A binary min-heap of non-empty lists, keyed by each list's front. */
    ta_timer_list **owners;
    uint32_t count;
    uint32_t capacity;
    ta_timer_id next_id;
    /* Submitted sends whose sending session is gone (ADR 0054). */
    ta_timer_list orphans;
    /* Set while the system is destroyed: orphaning releases instead of keeping. */
    bool closing;
} ta_timer_heap;

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator);
/* Releases every send still indexed, the orphans included. */
TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap);
/* Makes the next `additional` schedules into list allocation-free. */
TA_PRIVATE ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional);
/* Takes ownership of the send's values on TA_OK; assigns send->id when it is zero. */
TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send);
/* Removes and releases every not yet submitted send in list with send_id. */
TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Like ta_timer_cancel(), but also removes submitted sends. */
TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id);
/* Removes and releases every not yet submitted send in list (session termination). */
TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list);
/*
 * Makes ta_timer_orphan(list) allocation-free: reserves room in the orphans
 * for the list's submitted sends.
 */
TA_PRIVATE ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list);
/*
 * The sending session is gone: releases the list's unsubmitted sends and
 * moves its submitted ones to the orphans. Call ta_timer_reserve_orphans()
 * first. Leaves the list empty and unindexed; its storage stays allocated.
 */
TA_PRIVATE void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list);
/* Releases every send in list, unindexes it, and frees its storage. */
TA_PRIVATE void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list);
/* Bytes the list's storage occupies. */
TA_PRIVATE size_t ta_timer_list_bytes(const ta_timer_list *list);
/*
 * Copies the list's not yet submitted sends in (due, id) order, and returns
 * how many there are; a null `out` only counts. These are the sends a
 * session belongs to — a submitted send outlives it and is the system's to
 * re-offer. The copies borrow their values; the caller takes its own
 * references.
 */
TA_PRIVATE uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out);
/* The earliest due time of any indexed send; false when there is none. */
TA_PRIVATE bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out);
/* Removes the earliest send and transfers ownership of its values to *out. */
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



/*
 * The SCXML Null data model (SCXML B.1, ADR 0057), declared publicly as
 * ta_null_datamodel(). Its whole expression language is the `In(state)`
 * predicate in a condition; everything that needs data is an error. See
 * docs/internal/datamodel-null-profile.md.
 */



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

/*
 * Retires an invocation whose service did not survive a session image, and
 * queues its error.communication.invoke.<id>.unavailable event.
 */
TA_PRIVATE ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke);



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

/*
 * Literal content, and the text of a literal <send target> or <invoke src>.
 * `media_type` is what the front end said the bytes are (ADR 0053); it is
 * empty for literal text, and hashed into the revision either way.
 */
typedef struct {
    ta_str media_type;
    ta_bytes bytes;
} ta_content_def;

#define TA_CONTENT_NONE ((ta_content_id)UINT32_MAX)

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
    ta_system *system;
    bool registered;
    /* Intrusive list of compiled definitions not yet registered. */
    ta_definition *unregistered_previous;
    ta_definition *unregistered_next;
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
    /* The deepest nesting of states: 1 for a chart of root states only.
     * Sizes the interpreter's entry-set frames (ADR 0060). */
    uint32_t max_state_depth;
    /* The deepest nesting of blocks of executable content: 1 for blocks with
     * no <if> or <foreach> in them, 0 for a chart with no blocks at all.
     * Sizes the executor's frames (ADR 0060). */
    uint32_t max_action_depth;
    /*
     * Whether a microstep of this chart can need the actor's sparse state: it
     * has a <send> (a refused or delayed send is kept there), an <invoke>, or
     * a top-level final state (its completion is kept there). Such a chart's
     * sparse state is made before the microstep is visible (ADR 0047), with
     * its internal queue (ADR 0064); any other chart's actors need none.
     */
    bool microstep_needs_sparse;
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
    /* One actor of this definition is one block (ADR 0063; ta_actor_layout()). */
    size_t actor_block_bytes;
    size_t actor_block_alignment;
    size_t actor_session_offset;
    size_t actor_instance_offset;
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



/*
 * ta_definition_import() and ta_definition_encode() (ADR 0053 section 4):
 * the two functions core expresses over a chart codec. Core links no parser;
 * the codec is the embedder's.
 */



typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_actor_ref self;
    ta_actor_residency residency;
} ta_actor_record;

typedef struct {
    ta_delayed_send send;
    bool immediate;
} ta_activation_send;

typedef enum {
    TA_ACTIVATION_SEND,
    TA_ACTIVATION_LOG
} ta_activation_effect_kind;

typedef struct {
    ta_activation_effect_kind kind;
    union {
        ta_activation_send send;
        ta_log_record log;
    } as;
} ta_activation_effect;

typedef struct ta_activation_effects {
    ta_vec entries; /* ta_activation_effect, in authored order */
} ta_activation_effects;

/*
 * An actor's internal event queue (ADR 0064): its events, oldest first, and
 * its capacity, in one allocation. It is lent to an actor while it runs, and
 * the actor keeps it only while events are queued or a microstep is paused.
 */
typedef struct ta_internal_queue {
    uint32_t count;
    uint32_t capacity;
    ta_queued_event events[];
} ta_internal_queue;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
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
    /* The top-level <final> whose entry ended the session (ADR 0056); set
     * with `completion`, and meaningful only while the actor is finished. */
    ta_state_id final_state;
    /* This actor's pending delayed sends, indexed by the system's owner heap. */
    ta_timer_list timers;
    bool has_parent;
    bool completion_pending;
    /* A microstep paused inside executable content (ADR 0061), or null. */
    ta_continuation *paused;
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
    /* Its internal event queue (ADR 0064), or null. */
    ta_internal_queue *internal_queue;
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
    bool has_macrostep_event;
    ta_event macrostep_event;
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
    /* While free: the next free slot, or UINT32_MAX. Fits the padding the
     * generation already left, so the table costs no more per actor. */
    uint32_t next_free;
} ta_actor_slot;

/*
 * The actor table (ADR 0063): slots live in fixed pages of
 * TA_ACTOR_PAGE_SLOTS, found through a page directory. Slot `s` is in page
 * `s / TA_ACTOR_PAGE_SLOTS`. A page is allocated only when the last one is
 * full, and pages never move, so no spawn copies the table; only the
 * directory of page pointers doubles. Pages live as long as the system.
 */
#define TA_ACTOR_PAGE_SLOTS 4096u
typedef struct {
    ta_vec pages;   /* ta_actor_slot *, one per page */
    uint32_t count; /* slots issued so far */
} ta_actor_table;

/* The entry for `slot`, or NULL when that slot was never issued. */
TA_PRIVATE ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot);
TA_PRIVATE void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator);
/*
 * Lays out one actor of `definition` as a single allocation (ADR 0063): the
 * ta_actor record, then the session storage, then the data-model instance,
 * each at its alignment. Called once, when the definition is compiled.
 */
TA_PRIVATE void ta_actor_layout(ta_definition *definition);

TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id);
TA_PRIVATE bool ta_actor_state_active(const ta_actor *actor, ta_state_id state);
/* Runs an invoked child's exitInterpreter path with isolated scratch storage. */
TA_PRIVATE void ta_actor_exit_invoked(ta_system *system, ta_actor_id id);

/*
 * A blank, unbootstrapped actor of `definition` with its slot claimed; the
 * data-model instance is allocated but not initialized. ta_actor_spawn()
 * bootstraps it, ta_actor_reconstruct() fills it from a session image.
 */
TA_PRIVATE ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out);
/* Tears the actor down completely and uncounts it against its definition. */
TA_PRIVATE void ta_actor_free(ta_system *system, ta_actor *actor);
/* Frees the slot an actor was given, so a failed construction reuses it. */
TA_PRIVATE void ta_actor_release_slot(ta_system *system, ta_actor_id id);

/* The host directory's compare-and-swaps, which every residency change uses. */
TA_PRIVATE ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params);
TA_PRIVATE ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params);

/* The actor's delayed sends, or null when it has no sparse state (so none). */
TA_PRIVATE ta_timer_list *ta_actor_timers(const ta_actor *actor);
/* Stores a delayed send with its owner; takes the send's values on TA_OK. */
TA_PRIVATE ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send);
/* The list a send owned by `owner` belongs in: the actor's, or the orphans when it is gone. */
TA_PRIVATE ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out);

/* Materialized effects of an unpublished replacement's bootstrap. */
TA_PRIVATE ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send);
TA_PRIVATE ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record);
TA_PRIVATE bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id);

/*
 * Advances the session epoch: the actor has changed in a way a session image
 * captured before the change no longer describes. ta_actor_unload() compares
 * epochs to refuse a stale image (ADR 0014), so this marks every semantic
 * mutation and nothing else — a read, an inspection, or a capture must not
 * move it.
 */
TA_PRIVATE inline void ta_actor_touch(ta_actor *actor) { actor->session_epoch += 1u; }



/* Session images are opaque; struct ta_session_image is private to session_image.c. */



/*
 * Tinyactors core deliberately defines no persistence-store protocol.  This
 * compatibility module remains empty while durable checkpointing, command
 * sourcing, and storage adapters live in optional higher-level components.
 */



/* How many empty internal queues a system keeps for reuse (ADR 0064): one
 * per interpreter that can run at once, nested exits included (ADR 0060). */
#define TA_INTERNAL_QUEUE_CACHE 4u

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
    /* Interned named addresses (ADR 0065). */
    ta_address_table addresses;
    ta_event_name_table event_names;

    /*
     * ta_definition_slot; a definition ID is (generation << 32) | slot, as
     * for actors. Freed slots form a list through `next_free`.
     */
    ta_vec definitions;
    uint32_t free_definition_slot;
    /* Compiled definitions still owned by callers, reclaimed at teardown. */
    ta_definition *unregistered_definitions;
    /* Orders registrations, so the newest identity match wins after slot reuse. */
    uint64_t next_definition_serial;

    /*
     * ta_actor_slot; an actor ID is ta_actor_id_make(slot, generation). Freed slots
     * form a list through `next_free`, as definitions' do: a spawn takes the
     * head instead of scanning the table, so it costs the same against four
     * million actors as against four (invariant 17).
     */
    ta_actor_table actors;
    uint32_t free_actor_slot;

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
    /* Empty internal queues, lent to the next run that needs one (ADR 0064). */
    ta_internal_queue *queue_cache[TA_INTERNAL_QUEUE_CACHE];
    uint32_t queue_cache_count;
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
 * Per-call interpreter scratch. Between microsteps the actor holds all
 * state: configuration, history, internal queue, and pending event. A pause
 * inside a microstep (ADR 0061) moves the rest, the position, the sets and
 * the executor's frames, into the actor's continuation.
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
    /*
     * Scratch for the state-tree queries (ADR 0060 section 4), sized from
     * the definition when the interpreter starts, so no query allocates.
     */
    struct ta_entry_frame *entry_frames;
    uint32_t entry_capacity;
    ta_state_id *history_stack;
    uint64_t *history_seen;
    uint64_t *history_busy;
    uint64_t *final_states;
    /*
     * The executor's frames (ADR 0060 section 2): one per open block and one
     * per running <foreach>, sized from the definition's deepest nesting of
     * blocks when the interpreter starts, so running actions never allocates
     * frames.
     */
    ta_exec_frame *exec_frames;
    uint32_t exec_count;
    uint32_t exec_capacity;
    /*
     * Pausing inside a microstep (ADR 0061): where the microstep stands, the
     * units of executable content this call may run and has run, and
     * whether the block running now is one a pause may stop.
     */
    ta_micro_position at;
    uint32_t action_budget;
    uint32_t actions;
    bool pausable;
    bool in_microstep;
    /* Bootstrap only: data declarations the instance's binding already supplies. */
    uint64_t *bound_decls;
} ta_interpreter;

TA_PRIVATE ta_result ta_interpreter_resume(ta_system *system, ta_actor *actor, const ta_actor_resume_params *params);
/*
 * Runs SCXML exitInterpreter for a cancelled invoked session, without
 * done.invoke, and for every session it invoked in turn, without recursion
 * (ADR 0060). A session that is gone or finished is skipped.
 */
TA_PRIVATE void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id);
/* Takes ownership of the event's data. */
TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event);
/* Dispatches a due delayed send; RETRY leaves the send owned by the caller. */
TA_PRIVATE ta_external_event_submit_status ta_interpreter_dispatch_delayed(ta_system *system, ta_delayed_send *send);
/*
 * A literal content as a host adapter receives it (ADR 0053): its media type
 * and its bytes. The bytes' pointer is non-null even for empty content, so an
 * adapter can tell empty content from none.
 */
TA_PRIVATE void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes);
TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor);
/* Frees the system's cached internal queues (ADR 0064), at teardown. */
TA_PRIVATE void ta_interpreter_queue_cache_destroy(ta_system *system);
/* The bytes an internal queue of this capacity takes. */
TA_PRIVATE size_t ta_internal_queue_bytes(uint32_t capacity);

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
/* An invocation taken out of an actor's table, to be cancelled. */
typedef struct {
    ta_active_invoke active;
    ta_invoke_meta meta;
} ta_invoke_taken;
/* Forgets the not-yet-started invocations of `state`. */
TA_PRIVATE void ta_invoke_forget_pending(ta_actor *actor, ta_state_id state);
/* Takes the last invocation of `state` below index `*cursor` out of the
 * actor's table; false when there is none. */
TA_PRIVATE bool ta_invoke_take(ta_actor *actor, ta_state_id state, uint32_t *cursor, ta_invoke_taken *out);
/* Tells the invoker to cancel the service of a taken invocation. */
TA_PRIVATE void ta_invoke_cancel_service(const ta_invoke_taken *taken);
/* Starts the invokes of states entered during the finished macrostep. */
TA_PRIVATE ta_result ta_invoke_start_pending(ta_interpreter *it);
/* Applies <finalize> and autoforwarding to the actor's current (external) event. */
TA_PRIVATE ta_result ta_invoke_before_external(ta_interpreter *it);
/* Cancels every invocation and publishes done.invoke to the parent, if any. */
TA_PRIVATE ta_result ta_invoke_finished(ta_interpreter *it);
TA_PRIVATE bool ta_invoke_destination(const ta_actor *actor, ta_invoke_id invoke, ta_actor_ref *out);
/* The invoke ID an event from `sender` to `destination` carries: set for child-to-parent events. */
TA_PRIVATE ta_invoke_id ta_invoke_parent_id(const ta_actor *sender, const ta_actor_ref *destination);
/* False after a parent has retired the invocation that identifies this child. */
TA_PRIVATE bool ta_invoke_parent_accepts(const ta_system *system,
                              const ta_actor *sender,
                              const ta_actor_ref *destination);
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

static void *ta_arena_allocator_alloc(void *self, size_t size, size_t alignment) {
    return ta_arena_alloc(self, size, alignment);
}

static void ta_arena_allocator_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const ta_allocator_vtable ta_arena_allocator_vtable = {
    .alloc = ta_arena_allocator_alloc,
    .free = ta_arena_allocator_free,
};

TA_API ta_allocator ta_arena_allocator(ta_arena *arena) {
    return (ta_allocator){ .self = arena, .v = &ta_arena_allocator_vtable };
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


static uint32_t ta_address_hash(const ta_actor_address *address) {
    uint32_t hash = ta_hash_bytes(0, address->family.name.ptr, address->family.name.len);
    const char separator = 0;
    hash = ta_hash_bytes(hash, &separator, 1);
    return ta_hash_bytes(hash, address->name.ptr, address->name.len);
}

static uint32_t ta_address_bucket_count(const ta_address_table *table) {
    return table->base + table->split;
}

static ta_address **ta_address_bucket(const ta_address_table *table, uint32_t index) {
    ta_address **page = *(ta_address ***)ta_vec_at(&table->pages, index / TA_ADDRESS_PAGE_BUCKETS);
    return &page[index % TA_ADDRESS_PAGE_BUCKETS];
}

static uint32_t ta_address_index(const ta_address_table *table, uint32_t hash) {
    uint32_t index = hash & (table->base - 1u);
    return index < table->split ? hash & (2u * table->base - 1u) : index;
}

static size_t ta_address_bytes(uint32_t family_len, uint32_t name_len) {
    return sizeof(ta_address) + (size_t)family_len + (size_t)name_len + 2u;
}

/* Makes sure the page holding bucket `index` exists; false when it cannot. */
static bool ta_address_page_for(ta_address_table *table, uint32_t index) {
    uint32_t page_index = index / TA_ADDRESS_PAGE_BUCKETS;
    if (page_index < table->pages.count) {
        return true;
    }
    ta_address **page = ta_mem_alloc(table->allocator, TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                     _Alignof(ta_address *));
    if (page == NULL) {
        return false;
    }
    for (uint32_t bucket = 0; bucket < TA_ADDRESS_PAGE_BUCKETS; bucket += 1u) {
        page[bucket] = NULL;
    }
    if (ta_vec_push(&table->pages, &page) != TA_OK) {
        ta_mem_free(table->allocator, page,
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
        return false;
    }
    return true;
}

/* Splits one bucket when the table is fuller than one record per bucket. */
static void ta_address_grow(ta_address_table *table) {
    uint32_t added = ta_address_bucket_count(table);
    if (table->count <= added || added == UINT32_MAX || !ta_address_page_for(table, added)) {
        return; /* a failed page only postpones the split */
    }
    uint32_t wide = 2u * table->base - 1u;
    ta_address **from = ta_address_bucket(table, table->split);
    ta_address **to = ta_address_bucket(table, added);
    ta_address *chain = *from;
    *from = NULL;
    while (chain != NULL) {
        ta_address *next = chain->next;
        ta_address **home = (chain->hash & wide) == table->split ? from : to;
        chain->next = *home;
        *home = chain;
        chain = next;
    }
    table->split += 1u;
    if (table->split == table->base) {
        table->base *= 2u;
        table->split = 0;
    }
}

TA_PRIVATE void ta_address_table_init(ta_address_table *table, ta_allocator allocator) {
    *table = (ta_address_table){ .allocator = allocator, .base = TA_ADDRESS_INITIAL_BUCKETS };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_address **));
}

static void ta_address_free(ta_address_table *table, ta_address *address) {
    ta_mem_free(table->allocator, address,
                &(ta_allocator_free_params){ .size = ta_address_bytes(address->family_len, address->name_len),
                                             .alignment = _Alignof(ta_address) });
}

TA_PRIVATE void ta_address_table_destroy(ta_address_table *table) {
    uint32_t buckets = table->pages.count == 0 ? 0u : ta_address_bucket_count(table);
    for (uint32_t index = 0; index < buckets; index += 1u) {
        ta_address *chain = *ta_address_bucket(table, index);
        while (chain != NULL) {
            ta_address *next = chain->next;
            ta_address_free(table, chain);
            chain = next;
        }
    }
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(table->allocator, *(ta_address ***)ta_vec_at(&table->pages, page),
                    &(ta_allocator_free_params){ .size = TA_ADDRESS_PAGE_BUCKETS * sizeof(ta_address *),
                                                 .alignment = _Alignof(ta_address *) });
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_address_table){0};
}

static bool ta_address_matches(const ta_address *record, const ta_actor_address *address, uint32_t hash) {
    ta_str family = { .ptr = record->text, .len = record->family_len };
    ta_str name = { .ptr = record->text + record->family_len + 1u, .len = record->name_len };
    return record->hash == hash && ta_str_equal(family, address->family.name) && ta_str_equal(name, address->name);
}

static ta_address *ta_address_record(ta_allocator allocator, const ta_actor_address *address, uint32_t hash) {
    uint32_t family_len = address->family.name.len;
    uint32_t name_len = address->name.len;
    ta_address *record = ta_mem_alloc(allocator, ta_address_bytes(family_len, name_len), _Alignof(ta_address));
    if (record == NULL) {
        return NULL;
    }
    record->table = NULL;
    record->next = NULL;
    record->references = 0;
    record->hash = hash;
    record->family_len = family_len;
    record->name_len = name_len;
    if (family_len != 0) {
        ta_mem_copy(record->text, address->family.name.ptr, family_len);
    }
    record->text[family_len] = 0;
    if (name_len != 0) {
        ta_mem_copy(record->text + family_len + 1u, address->name.ptr, name_len);
    }
    record->text[family_len + 1u + name_len] = 0;
    return record;
}

TA_PRIVATE ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address,
                                  ta_address **out) {
    uint32_t hash = ta_address_hash(address);
    if (table->pages.count != 0) {
        for (ta_address *record = *ta_address_bucket(table, ta_address_index(table, hash)); record != NULL;
             record = record->next) {
            if (ta_address_matches(record, address, hash)) {
                record->references += 1u;
                *out = record;
                return TA_OK;
            }
        }
    } else if (!ta_address_page_for(table, 0)) {
        return TA_ERR_NOMEM;
    }
    ta_address *record = ta_address_record(table->allocator, address, hash);
    if (record == NULL) {
        return TA_ERR_NOMEM;
    }
    record->table = table;
    record->references = 1;
    ta_address **bucket = ta_address_bucket(table, ta_address_index(table, hash));
    record->next = *bucket;
    *bucket = record;
    table->count += 1u;
    ta_address_grow(table);
    *out = record;
    return TA_OK;
}

TA_PRIVATE ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out) {
    size_t bytes = ta_address_bytes(address->family_len, address->name_len);
    ta_address *copy = ta_arena_alloc(arena, bytes, _Alignof(ta_address));
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, address, bytes);
    copy->table = NULL;
    copy->next = NULL;
    copy->references = 0;
    *out = copy;
    return TA_OK;
}

TA_PRIVATE bool ta_address_in(const ta_address *address, const ta_address_table *table) {
    return address != NULL && address->table == table;
}

/* ------------------------------------------------------------------------
 * Public
 */

TA_API ta_result ta_address_intern(ta_system *system, const ta_actor_address *address, ta_address **out) {
    if (system == NULL || address == NULL || out == NULL ||
        (address->family.name.ptr == NULL && address->family.name.len != 0) ||
        (address->name.ptr == NULL && address->name.len != 0)) {
        return TA_ERR_INVALID;
    }
    return ta_address_table_intern(&system->addresses, address, out);
}

TA_API void ta_address_retain(ta_address *address) {
    if (address != NULL && address->table != NULL) {
        address->references += 1u;
    }
}

TA_API void ta_address_release(ta_address *address) {
    if (address == NULL || address->table == NULL) {
        return;
    }
    ta_address *record = address;
    record->references -= 1u;
    if (record->references != 0) {
        return;
    }
    ta_address_table *table = record->table;
    for (ta_address **link = ta_address_bucket(table, ta_address_index(table, record->hash)); *link != NULL;
         link = &(*link)->next) {
        if (*link == record) {
            *link = record->next;
            break;
        }
    }
    table->count -= 1u;
    ta_address_free(table, record);
}

TA_API ta_actor_address ta_address_view(const ta_address *address) {
    if (address == NULL) {
        return (ta_actor_address){0};
    }
    return (ta_actor_address){
        .family = { .name = { .ptr = address->text, .len = address->family_len } },
        .name = { .ptr = address->text + address->family_len + 1u, .len = address->name_len },
    };
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


TA_API void ta_event_retain(const ta_event *event, ta_event *out) {
    *out = *event;
    ta_value_retain(&out->data);
    if (out->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_retain(&out->origin.as.actor);
    }
}

TA_API void ta_event_release(ta_event *event) {
    if (event == NULL) {
        return;
    }
    ta_value_release(&event->data);
    event->data = (ta_value){0};
    if (event->origin.kind == TA_ORIGIN_ACTOR) {
        ta_actor_ref_release(&event->origin.as.actor);
    }
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
        /* The target holds a reference to the interned address (ADR 0065). */
        ta_address *named = NULL;
        ta_result result = ta_address_intern(system, &address, &named);
        if (result != TA_OK) {
            return result;
        }
        *out = (ta_scxml_target){ .kind = TA_SCXML_TARGET_ACTOR,
                                  .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = named } };
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

TA_API void ta_scxml_target_release(ta_scxml_target *target) {
    if (target != NULL && target->kind == TA_SCXML_TARGET_ACTOR) {
        ta_actor_ref_release(&target->as.actor);
    }
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
                ta_actor_address view = ta_address_view(target->as.actor.as.named);
                ta_text_put(writer, ta_str_from_cstr(TA_URI_ADDRESS));
                ta_text_put(writer, view.family.name);
                ta_text_put(writer, (ta_str){ .ptr = "/", .len = 1 });
                ta_text_put(writer, view.name);
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


TA_PRIVATE void ta_actor_ref_retain(const ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_retain(ref->as.named);
    }
}

TA_PRIVATE void ta_actor_ref_release(ta_actor_ref *ref) {
    if (ref->kind == TA_ACTOR_REF_NAMED) {
        ta_address_release(ref->as.named);
        ref->as.named = NULL;
    }
}

TA_PRIVATE ta_result ta_actor_ref_localize(ta_system *system, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL || ta_address_in(ref->as.named, &system->addresses)) {
        return TA_OK;
    }
    ta_actor_address view = ta_address_view(ref->as.named);
    return ta_address_table_intern(&system->addresses, &view, &ref->as.named);
}

TA_PRIVATE ta_result ta_actor_ref_detach(ta_arena *arena, ta_actor_ref *ref) {
    if (ref->kind != TA_ACTOR_REF_NAMED || ref->as.named == NULL) {
        return TA_OK;
    }
    return ta_address_detach(arena, ref->as.named, &ref->as.named);
}

TA_PRIVATE ta_actor_ref *ta_event_origin_ref(ta_event *event) {
    return event->origin.kind == TA_ORIGIN_ACTOR ? &event->origin.as.actor : NULL;
}

TA_API void ta_external_delivery_retain(const ta_external_delivery *delivery, ta_external_delivery *out) {
    if (delivery == NULL || out == NULL) {
        return;
    }
    ta_external_delivery copy = *delivery;
    ta_actor_ref_retain(&copy.destination);
    ta_event_retain(&delivery->event, &copy.event);
    *out = copy;
}

TA_API void ta_external_delivery_release(ta_external_delivery *delivery) {
    if (delivery == NULL) {
        return;
    }
    ta_actor_ref_release(&delivery->destination);
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


/* Offsets of the arrays that follow a continuation's header, in order. */
typedef struct {
    size_t sets;
    size_t history;
    size_t enabled;
    size_t frames;
    size_t end;
} ta_continuation_layout;

static ta_continuation_layout ta_continuation_offsets(const ta_continuation *shape) {
    ta_continuation_layout layout;
    layout.sets = ta_align_up(sizeof(ta_continuation), _Alignof(uint64_t));
    layout.history = ta_align_up(layout.sets + (size_t)shape->words * 4u * sizeof(uint64_t), _Alignof(ta_state_id));
    layout.enabled = ta_align_up(layout.history + (size_t)shape->state_count * sizeof(ta_state_id),
                                 _Alignof(ta_transition_id));
    layout.frames = ta_align_up(layout.enabled + (size_t)shape->enabled_count * sizeof(ta_transition_id),
                                _Alignof(ta_exec_frame));
    layout.end = layout.frames + (size_t)shape->frame_count * sizeof(ta_exec_frame);
    return layout;
}

TA_PRIVATE size_t ta_continuation_bytes(const ta_continuation *shape) {
    return ta_continuation_offsets(shape).end;
}

TA_PRIVATE uint64_t *ta_continuation_sets(ta_continuation *continuation) {
    return (uint64_t *)((unsigned char *)continuation + ta_continuation_offsets(continuation).sets);
}

TA_PRIVATE ta_state_id *ta_continuation_history(ta_continuation *continuation) {
    return (ta_state_id *)((unsigned char *)continuation + ta_continuation_offsets(continuation).history);
}

TA_PRIVATE ta_transition_id *ta_continuation_enabled(ta_continuation *continuation) {
    return (ta_transition_id *)((unsigned char *)continuation + ta_continuation_offsets(continuation).enabled);
}

TA_PRIVATE ta_exec_frame *ta_continuation_frames(ta_continuation *continuation) {
    return (ta_exec_frame *)((unsigned char *)continuation + ta_continuation_offsets(continuation).frames);
}

TA_PRIVATE void ta_continuation_retain(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_retain(&frames[index].iteration_snapshot);
        }
    }
}

TA_PRIVATE void ta_continuation_release(ta_continuation *continuation) {
    ta_exec_frame *frames = ta_continuation_frames(continuation);
    for (uint32_t index = 0; index < continuation->frame_count; index += 1u) {
        if (frames[index].kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frames[index].iteration_snapshot);
        }
    }
}

TA_PRIVATE void ta_continuation_free(ta_allocator allocator, ta_continuation **continuation) {
    ta_continuation *held = *continuation;
    if (held == NULL) {
        return;
    }
    ta_continuation_release(held);
    ta_mem_free(allocator, held, &(ta_allocator_free_params){ .size = held->bytes, .alignment = TA_CONTINUATION_ALIGN });
    *continuation = NULL;
}


/* ------------------------------------------------------------------------
 * Keys and the owner heap
 */

typedef struct {
    ta_time due;
    ta_timer_id id;
} ta_timer_key;

static bool ta_timer_key_before(ta_timer_key left, ta_timer_key right) {
    return left.due < right.due || (left.due == right.due && left.id < right.id);
}

static ta_timer_key ta_timer_key_at(const ta_timer_list *list, uint32_t index) {
    return (ta_timer_key){ .due = list->sends[index].due, .id = list->meta[index].id };
}

static bool ta_timer_list_before(const ta_timer_list *left, const ta_timer_list *right) {
    return ta_timer_key_before(ta_timer_key_at(left, 0), ta_timer_key_at(right, 0));
}

static void ta_timer_place(ta_timer_heap *heap, uint32_t index, ta_timer_list *list) {
    heap->owners[index] = list;
    list->heap_index = index;
}

static void ta_timer_sift_up(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = heap->owners[index];
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (!ta_timer_list_before(list, heap->owners[parent])) {
            break;
        }
        ta_timer_place(heap, index, heap->owners[parent]);
        index = parent;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_sift_down(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = heap->owners[index];
    for (;;) {
        uint32_t smallest = index;
        const ta_timer_list *best = list;
        uint32_t left = index * 2u + 1u;
        uint32_t right = left + 1u;
        if (left < heap->count && ta_timer_list_before(heap->owners[left], best)) {
            smallest = left;
            best = heap->owners[left];
        }
        if (right < heap->count && ta_timer_list_before(heap->owners[right], best)) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        ta_timer_place(heap, index, heap->owners[smallest]);
        index = smallest;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_unindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (!list->indexed) {
        return;
    }
    uint32_t index = list->heap_index;
    list->indexed = false;
    heap->count -= 1u;
    if (index == heap->count) {
        return;
    }
    ta_timer_place(heap, index, heap->owners[heap->count]);
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, index);
}

/*
 * Restores the heap after the list's front may have changed. Inserting a
 * list needs a free heap slot; ta_timer_reserve_heap() provides it.
 */
static void ta_timer_reindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (list->count == 0) {
        ta_timer_unindex(heap, list);
        return;
    }
    if (!list->indexed) {
        list->indexed = true;
        ta_timer_place(heap, heap->count, list);
        heap->count += 1u;
        ta_timer_sift_up(heap, list->heap_index);
        return;
    }
    uint32_t index = list->heap_index;
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, list->heap_index);
}

static uint32_t ta_timer_grow_capacity(uint32_t current, uint32_t needed) {
    uint32_t capacity = current == 0 ? 4u : current;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return needed;
        }
        capacity *= 2u;
    }
    return capacity;
}

static ta_result ta_timer_reserve_heap(ta_timer_heap *heap, uint32_t additional) {
    if (additional > UINT32_MAX - heap->count) {
        return TA_ERR_NOMEM;
    }
    if (additional <= heap->capacity - heap->count) {
        return TA_OK;
    }
    uint32_t capacity = ta_timer_grow_capacity(heap->capacity, heap->count + additional);
    ta_timer_list **owners = ta_mem_alloc(heap->allocator, (size_t)capacity * sizeof(ta_timer_list *),
                                          _Alignof(ta_timer_list *));
    if (owners == NULL) {
        return TA_ERR_NOMEM;
    }
    if (heap->owners != NULL) {
        ta_mem_copy(owners, heap->owners, (size_t)heap->count * sizeof(ta_timer_list *));
        ta_mem_free(heap->allocator, heap->owners,
                    &(ta_allocator_free_params){ .size = (size_t)heap->capacity * sizeof(ta_timer_list *),
                                                 .alignment = _Alignof(ta_timer_list *) });
    }
    heap->owners = owners;
    heap->capacity = capacity;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * One owner's list
 */

static void ta_timer_free_storage(ta_allocator allocator, ta_timer_list *list) {
    if (list->capacity == 0) {
        return;
    }
    ta_mem_free(allocator, list->sends,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_info),
                                             .alignment = _Alignof(ta_delayed_send_info) });
    ta_mem_free(allocator, list->meta,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_meta),
                                             .alignment = _Alignof(ta_delayed_send_meta) });
    list->sends = NULL;
    list->meta = NULL;
    list->capacity = 0;
}

static ta_result ta_timer_reserve_list(ta_allocator allocator, ta_timer_list *list, uint32_t additional) {
    if (additional > UINT32_MAX - list->count) {
        return TA_ERR_NOMEM;
    }
    if (additional <= list->capacity - list->count) {
        return TA_OK;
    }
    uint32_t capacity = ta_timer_grow_capacity(list->capacity, list->count + additional);
    ta_delayed_send_info *sends = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_info),
                                               _Alignof(ta_delayed_send_info));
    ta_delayed_send_meta *meta = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_meta),
                                              _Alignof(ta_delayed_send_meta));
    if (sends == NULL || meta == NULL) {
        if (sends != NULL) {
            ta_mem_free(allocator, sends,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_info),
                                                     .alignment = _Alignof(ta_delayed_send_info) });
        }
        if (meta != NULL) {
            ta_mem_free(allocator, meta,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_meta),
                                                     .alignment = _Alignof(ta_delayed_send_meta) });
        }
        return TA_ERR_NOMEM;
    }
    if (list->count != 0) {
        ta_mem_copy(sends, list->sends, (size_t)list->count * sizeof(ta_delayed_send_info));
        ta_mem_copy(meta, list->meta, (size_t)list->count * sizeof(ta_delayed_send_meta));
    }
    ta_timer_free_storage(allocator, list);
    list->sends = sends;
    list->meta = meta;
    list->capacity = capacity;
    return TA_OK;
}

static void ta_timer_split(const ta_delayed_send *send, ta_delayed_send_info *info, ta_delayed_send_meta *meta) {
    *info = (ta_delayed_send_info){
        .send_id = send->send_id,
        .due = send->due,
        .event = send->event,
        .io_type = send->io_type,
        .destination = send->destination,
        .target = send->target,
        .data = send->data,
        .submitted = send->submitted,
    };
    *meta = (ta_delayed_send_meta){
        .id = send->id,
        .owner = send->owner,
        .origin = send->origin,
        .invoke_id = send->invoke_id,
        .has_content = send->has_content,
        .content = send->content,
    };
}

static ta_delayed_send ta_timer_join(const ta_timer_list *list, uint32_t index) {
    const ta_delayed_send_info *info = &list->sends[index];
    const ta_delayed_send_meta *meta = &list->meta[index];
    return (ta_delayed_send){
        .id = meta->id,
        .owner = meta->owner,
        .send_id = info->send_id,
        .due = info->due,
        .event = info->event,
        .io_type = info->io_type,
        .destination = info->destination,
        .target = info->target,
        .data = info->data,
        .submitted = info->submitted,
        .origin = meta->origin,
        .invoke_id = meta->invoke_id,
        .has_content = meta->has_content,
        .content = meta->content,
    };
}

/* The index of the first stored send that sorts after key. */
static uint32_t ta_timer_upper_bound(const ta_timer_list *list, ta_timer_key key) {
    uint32_t low = 0;
    uint32_t high = list->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (ta_timer_key_before(key, ta_timer_key_at(list, middle))) {
            high = middle;
        } else {
            low = middle + 1u;
        }
    }
    return low;
}

/* Inserts into reserved capacity; the caller reindexes. */
static void ta_timer_insert(ta_timer_list *list, const ta_delayed_send *send) {
    uint32_t at = ta_timer_upper_bound(list, (ta_timer_key){ .due = send->due, .id = send->id });
    for (uint32_t index = list->count; index > at; index -= 1u) {
        list->sends[index] = list->sends[index - 1u];
        list->meta[index] = list->meta[index - 1u];
    }
    ta_timer_split(send, &list->sends[at], &list->meta[at]);
    list->count += 1u;
}

typedef struct {
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
} ta_timer_selection;

static bool ta_timer_selected(const ta_delayed_send_info *send, const ta_timer_selection *selection) {
    return (selection->any_send_id || send->send_id == selection->send_id) &&
           (selection->include_submitted || !send->submitted);
}

/* Releases and removes the selected sends, keeping order; reindexes the list. */
static bool ta_timer_remove_selected(ta_timer_heap *heap, ta_timer_list *list, const ta_timer_selection *selection) {
    if (list == NULL) {
        return false;
    }
    uint32_t kept = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (ta_timer_selected(&list->sends[index], selection)) {
            ta_value_release(&list->sends[index].target);
            ta_value_release(&list->sends[index].data);
            /* A stored send owns its destination and origin references (ADR 0065). */
            ta_actor_ref_release(&list->sends[index].destination);
            ta_actor_ref_release(&list->meta[index].origin);
            continue;
        }
        list->sends[kept] = list->sends[index];
        list->meta[kept] = list->meta[index];
        kept += 1u;
    }
    bool found = kept != list->count;
    list->count = kept;
    if (found) {
        ta_timer_reindex(heap, list);
    }
    return found;
}

/* ------------------------------------------------------------------------
 * Public within core
 */

TA_PRIVATE ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator) {
    *heap = (ta_timer_heap){ .allocator = allocator, .next_id = 1 };
    return TA_OK;
}

TA_PRIVATE void ta_timer_heap_destroy(ta_timer_heap *heap) {
    /* Lists still indexed belong to actors that outlived the system's teardown. */
    while (heap->count != 0) {
        ta_timer_list *list = heap->owners[0];
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    }
    ta_timer_free_storage(heap->allocator, &heap->orphans);
    if (heap->owners != NULL) {
        ta_mem_free(heap->allocator, heap->owners,
                    &(ta_allocator_free_params){ .size = (size_t)heap->capacity * sizeof(ta_timer_list *),
                                                 .alignment = _Alignof(ta_timer_list *) });
    }
    *heap = (ta_timer_heap){0};
}

TA_PRIVATE ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional) {
    if (additional == 0) {
        return TA_OK;
    }
    ta_result result = ta_timer_reserve_heap(heap, list->indexed ? 0u : 1u);
    return result == TA_OK ? ta_timer_reserve_list(heap->allocator, list, additional) : result;
}

TA_PRIVATE ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send) {
    ta_result result = ta_timer_reserve(heap, list, 1u);
    if (result != TA_OK) {
        return result;
    }
    ta_delayed_send record = *send;
    if (record.id == 0) {
        record.id = heap->next_id;
        heap->next_id += 1u;
    }
    ta_timer_insert(list, &record);
    ta_timer_reindex(heap, list);
    return TA_OK;
}

TA_PRIVATE ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .send_id = send_id }) ? TA_OK
                                                                                            : TA_ERR_NOT_FOUND;
}

TA_PRIVATE ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list,
                                    &(ta_timer_selection){ .send_id = send_id, .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

TA_PRIVATE void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .any_send_id = true });
}

static uint32_t ta_timer_submitted_count(const ta_timer_list *list) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        count += list->sends[index].submitted ? 1u : 0u;
    }
    return count;
}

TA_PRIVATE ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list) {
    if (list == NULL || heap->closing) {
        return TA_OK;
    }
    return ta_timer_reserve(heap, &heap->orphans, ta_timer_submitted_count(list));
}

TA_PRIVATE void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list) {
    if (list == NULL) {
        return;
    }
    ta_timer_cancel_unsubmitted(heap, list);
    if (heap->closing) {
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
        return;
    }
    if (list->count == 0) {
        return;
    }
    /* Merge the two sorted lists from the back into the reserved room. */
    ta_timer_list *orphans = &heap->orphans;
    uint32_t left = orphans->count;
    uint32_t right = list->count;
    uint32_t out = left + right;
    while (right > 0) {
        out -= 1u;
        if (left > 0 && ta_timer_key_before(ta_timer_key_at(list, right - 1u), ta_timer_key_at(orphans, left - 1u))) {
            left -= 1u;
            orphans->sends[out] = orphans->sends[left];
            orphans->meta[out] = orphans->meta[left];
        } else {
            right -= 1u;
            orphans->sends[out] = list->sends[right];
            orphans->meta[out] = list->meta[right];
        }
    }
    orphans->count += list->count;
    list->count = 0;
    ta_timer_unindex(heap, list);
    ta_timer_reindex(heap, orphans);
}

TA_PRIVATE void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list,
                                   &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    ta_timer_unindex(heap, list);
    ta_timer_free_storage(heap->allocator, list);
}

TA_PRIVATE size_t ta_timer_list_bytes(const ta_timer_list *list) {
    return list == NULL ? 0u
                        : (size_t)list->capacity * (sizeof(ta_delayed_send_info) + sizeof(ta_delayed_send_meta));
}

TA_PRIVATE uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out) {
    if (list == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (list->sends[index].submitted) {
            continue;
        }
        if (out != NULL) {
            out[count] = ta_timer_join(list, index);
        }
        count += 1u;
    }
    return count;
}

TA_PRIVATE bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out) {
    if (heap->count == 0) {
        return false;
    }
    *out = heap->owners[0]->sends[0].due;
    return true;
}

TA_PRIVATE ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out) {
    if (heap->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_timer_list *list = heap->owners[0];
    *out = ta_timer_join(list, 0);
    for (uint32_t index = 1; index < list->count; index += 1u) {
        list->sends[index - 1u] = list->sends[index];
        list->meta[index - 1u] = list->meta[index];
    }
    list->count -= 1u;
    ta_timer_reindex(heap, list);
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
 * The Null data model (SCXML B.1, ADR 0057). Its whole expression language
 * is the `In(state)` predicate used in a `cond`. `prepare_definition` refuses
 * a chart with any other expression, naming the expression (ADR 0058), so
 * such a chart fails when it is defined rather than on first use. The
 * remaining operations need data the profile does not have; each fails with
 * a TA_ERR_UNSUPPORTED fault, which the interpreter turns into
 * error.execution.
 *
 * Nothing is stored per actor: instance size 0. A definition with
 * expressions keeps one small record, the expression table core already
 * owns, because an expression reaches the adapter as a ta_expr_id. A
 * definition without expressions keeps nothing.
 */

typedef struct {
    const ta_expression *expressions;
    uint32_t expression_count;
} ta_null_shared;

#define TA_NULL_TEXT(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* Every operation the profile does not support answers this way. */
static ta_op_status ta_null_unsupported(ta_fault *fault, ta_str message) {
    if (fault != NULL) {
        *fault = (ta_fault){ .code = TA_ERR_UNSUPPORTED, .message = message };
    }
    return TA_OP_ERROR;
}

static bool ta_null_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static uint32_t ta_null_skip_space(ta_str source, uint32_t at) {
    while (at < source.len && ta_null_space(source.ptr[at])) {
        at += 1u;
    }
    return at;
}

/*
 * The only expression the profile has: `In(id)`, with the ID optionally
 * quoted in ' or ". Returns false when `source` is anything else.
 */
static bool ta_null_parse_in(ta_str source, ta_str *out) {
    uint32_t at = ta_null_skip_space(source, 0);
    if (source.len - at < 4u || source.ptr[at] != 'I' || source.ptr[at + 1u] != 'n') {
        return false;
    }
    at = ta_null_skip_space(source, at + 2u);
    if (at >= source.len || source.ptr[at] != '(') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    char quote = 0;
    if (at < source.len && (source.ptr[at] == '\'' || source.ptr[at] == '"')) {
        quote = source.ptr[at];
        at += 1u;
    }
    uint32_t start = at;
    while (at < source.len && source.ptr[at] != ')' && source.ptr[at] != quote &&
           (quote != 0 || !ta_null_space(source.ptr[at]))) {
        at += 1u;
    }
    uint32_t end = at;
    if (quote != 0) {
        if (at >= source.len || source.ptr[at] != quote) {
            return false;
        }
        at += 1u;
    }
    at = ta_null_skip_space(source, at);
    if (at >= source.len || source.ptr[at] != ')') {
        return false;
    }
    at = ta_null_skip_space(source, at + 1u);
    if (at != source.len || end == start) {
        return false;
    }
    *out = (ta_str){ .ptr = source.ptr + start, .len = end - start };
    return true;
}

static ta_result ta_null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    if (params == NULL || params->shared == NULL || params->allocator.v == NULL ||
        (params->expressions == NULL && params->expression_count != 0)) {
        return TA_ERR_INVALID;
    }
    *params->shared = NULL;
    for (uint32_t index = 0; index < params->expression_count; index += 1u) {
        const ta_expression *expression = &params->expressions[index];
        ta_str id = {0};
        if (expression->kind == TA_EXPRESSION_NONE) {
            continue;
        }
        if (expression->kind == TA_EXPRESSION_SOURCE && ta_null_parse_in(expression->as.source, &id)) {
            continue;
        }
        if (params->rejected != NULL) {
            *params->rejected = (ta_expression_rejection){
                .expression = index,
                .offset = 0,
                .length = expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source.len : 0u,
                .message = expression->kind == TA_EXPRESSION_SOURCE
                               ? TA_NULL_TEXT("the null data model has only the In(state) predicate")
                               : TA_NULL_TEXT("the null data model takes expressions as source text"),
            };
        }
        return expression->kind == TA_EXPRESSION_SOURCE ? TA_ERR_INVALID : TA_ERR_UNSUPPORTED;
    }
    if (params->expression_count == 0) {
        return TA_OK;
    }
    ta_null_shared *shared = ta_mem_alloc(params->allocator, sizeof(ta_null_shared), _Alignof(ta_null_shared));
    if (shared == NULL) {
        return TA_ERR_NOMEM;
    }
    *shared = (ta_null_shared){ .expressions = params->expressions, .expression_count = params->expression_count };
    *params->shared = shared;
    return TA_OK;
}

static void ta_null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    if (shared == NULL || allocator.v == NULL) {
        return;
    }
    ta_mem_free(allocator, shared,
                &(ta_allocator_free_params){ .size = sizeof(ta_null_shared), .alignment = _Alignof(ta_null_shared) });
}

/* No data: an actor of a null-model chart allocates nothing for it. */
static uint32_t ta_null_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t ta_null_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result ta_null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    /* A binding is host data, which this profile cannot hold. */
    return params != NULL && params->binding != NULL && params->binding->v != NULL ? TA_ERR_UNSUPPORTED : TA_OK;
}

static void ta_null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static ta_str ta_null_source(const ta_null_shared *shared, ta_expr_id expr) {
    if (shared == NULL || shared->expressions == NULL || expr >= shared->expression_count) {
        return (ta_str){0};
    }
    const ta_expression *expression = &shared->expressions[expr];
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_op_status ta_null_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    ta_str id = {0};
    if (!ta_null_parse_in(ta_null_source(params->shared, params->expr), &id)) {
        return ta_null_unsupported(params->fault,
                                   TA_NULL_TEXT("the null data model evaluates only the In(state) predicate"));
    }
    /* SCXML B.1: In() of a state the chart does not have is simply false;
     * core answers TA_ERR_NOT_FOUND for it (ADR 0049). */
    ta_state_id state = 0;
    *params->out = ta_dm_host_find_state(params->host, id, &state) == TA_OK && ta_dm_host_in_state(params->host, state);
    return TA_OP_OK;
}

static ta_op_status ta_null_declare(void *self, const ta_datamodel_declare_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no data bindings"));
}

static ta_op_status ta_null_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no value expressions"));
}

static ta_op_status ta_null_read_location(void *self, const ta_datamodel_read_location_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no locations"));
}

static ta_op_status ta_null_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no assignment"));
}

static ta_op_status ta_null_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no scripts"));
}

static ta_op_status ta_null_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values to make params from"));
}

static ta_op_status ta_null_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_text_value(void *self, const ta_datamodel_text_value_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no values"));
}

static ta_op_status ta_null_import_content(void *self, const ta_datamodel_import_content_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model cannot import content"));
}

static ta_op_status ta_null_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

static ta_op_status ta_null_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    return ta_null_unsupported(params->fault, TA_NULL_TEXT("the null data model has no collections to iterate"));
}

/*
 * `inspect` stays absent: the profile gives the application no data to read,
 * so ta_actor_datamodel_root() reports TA_ERR_UNSUPPORTED for these actors.
 */
static const ta_datamodel_vtable ta_null_datamodel_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = ta_null_prepare,
    .destroy_definition = ta_null_destroy_definition,
    .instance_size = ta_null_instance_size,
    .instance_alignment = ta_null_instance_alignment,
    .init_instance = ta_null_init_instance,
    .destroy_instance = ta_null_destroy_instance,
    .declare = ta_null_declare,
    .set_named = ta_null_set_named,
    .eval_condition = ta_null_eval_condition,
    .eval_value = ta_null_eval_value,
    .read_location = ta_null_read_location,
    .assign = ta_null_assign,
    .execute_script = ta_null_execute_script,
    .make_params = ta_null_make_params,
    .value_text = ta_null_value_text,
    .text_value = ta_null_text_value,
    .import_content = ta_null_import_content,
    .foreach_snapshot = ta_null_foreach_snapshot,
    .foreach_bind = ta_null_foreach_bind,
};

TA_API ta_datamodel ta_null_datamodel(void) {
    return (ta_datamodel){ .self = NULL, .v = &ta_null_datamodel_vtable };
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

static ta_invoke_taken ta_invoke_take_index(ta_actor_sparse_state *sparse, uint32_t index) {
    ta_invoke_taken taken = { .active = sparse->invokes[index], .meta = sparse->invoke_meta[index] };
    /* Remove first: the service may destroy a child that reports back reentrantly. */
    ta_invoke_remove_at(sparse, index);
    return taken;
}

TA_PRIVATE void ta_invoke_cancel_service(const ta_invoke_taken *taken) {
    if (taken->meta.invoker.v != NULL && taken->meta.invoker.v->cancel != NULL) {
        ta_fault fault = {0};
        (void)taken->meta.invoker.v->cancel(taken->meta.invoker.self, taken->active.service_handle, &fault);
        ta_value_release(&fault.data);
    }
}

/* Cancels the service and forgets the invocation. */
static void ta_invoke_cancel_at(ta_system *system, ta_actor_sparse_state *sparse, uint32_t index) {
    ta_invoke_taken taken = ta_invoke_take_index(sparse, index);
    if (taken.meta.has_child) {
        ta_actor_exit_invoked(system, taken.meta.child);
    }
    ta_invoke_cancel_service(&taken);
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

TA_PRIVATE void ta_invoke_forget_pending(ta_actor *actor, ta_state_id state) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL) {
        return;
    }
    ta_vec *pending = &sparse->pending_invokes;
    for (uint32_t index = 0; index < pending->count; index += 1) {
        if (*(ta_state_id *)ta_vec_at(pending, index) == state) {
            *(ta_state_id *)ta_vec_at(pending, index) = *(ta_state_id *)ta_vec_at(pending, pending->count - 1u);
            pending->count -= 1u;
            break;
        }
    }
}

TA_PRIVATE bool ta_invoke_take(ta_actor *actor, ta_state_id state, uint32_t *cursor, ta_invoke_taken *out) {
    ta_actor_sparse_state *sparse = actor->sparse;
    uint32_t index = sparse == NULL ? 0 : *cursor;
    if (sparse != NULL && index > sparse->invoke_count) {
        index = sparse->invoke_count;
    }
    for (; index > 0; index -= 1u) {
        if (sparse->invoke_meta[index - 1u].state == state) {
            *out = ta_invoke_take_index(sparse, index - 1u);
            *cursor = index - 1u;
            return true;
        }
    }
    *cursor = 0;
    return false;
}

TA_PRIVATE ta_result ta_invoke_state_exited(ta_interpreter *it, ta_state_id state) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL) {
        return TA_OK;
    }
    ta_invoke_forget_pending(it->actor, state);
    for (uint32_t index = sparse->invoke_count; index > 0; index -= 1u) {
        if (sparse->invoke_meta[index - 1u].state == state) {
            ta_invoke_cancel_at(it->system, sparse, index - 1u);
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
        /*
         * Literal content is not imported: core owns no invoker, so it has no
         * semantics to impose, and the invoker receives the bytes and their
         * media type (ADR 0053, SCXML 6.4.2). ta_invoke_start reads them from
         * the definition.
         */
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
    if (decl->data.kind == TA_VALUE_SOURCE_CONTENT) {
        ta_interpreter_literal_content(it->definition, decl->data.as.content, &request.content_type,
                                       &request.content);
    }
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
    /* A prepared replacement materializes its bootstrap off-route. Starting
     * an invocation is externally visible, so activation releases it. */
    if (!it->actor->published) {
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
    const ta_actor_sparse_state *sender_sparse = delivery->sender->sparse;
    if (sender_sparse != NULL && sender_sparse->has_parent &&
        delivery->destination.kind == TA_ACTOR_REF_RUNTIME &&
        delivery->destination.as.runtime == sender_sparse->parent.actor &&
        delivery->event.invoke_id == sender_sparse->parent.invoke_id) {
        ta_actor *parent = ta_actor_lookup(system, sender_sparse->parent.actor);
        if (parent == NULL || ta_invoke_find(parent, sender_sparse->parent.invoke_id) < 0) {
            /* SCXML 6.4: once the parent cancels an invocation, events the
             * child emits while running onexit are discarded. */
            return TA_OK;
        }
    }
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
    record.data = delivery->event.data;
    ta_value_retain(&record.data);
    /* The stored send owns its destination and origin references (ADR 0065). */
    ta_actor_ref_retain(&record.destination);
    ta_actor_ref_retain(&record.origin);
    ta_result result = ta_actor_schedule_send(system, delivery->sender, &record);
    if (result != TA_OK) {
        ta_value_release(&record.data);
        ta_actor_ref_release(&record.destination);
        ta_actor_ref_release(&record.origin);
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

TA_PRIVATE bool ta_invoke_parent_accepts(const ta_system *system,
                              const ta_actor *sender,
                              const ta_actor_ref *destination) {
    if (sender == NULL || sender->sparse == NULL || !sender->sparse->has_parent ||
        destination->kind != TA_ACTOR_REF_RUNTIME ||
        destination->as.runtime != sender->sparse->parent.actor) {
        return true;
    }
    ta_actor *parent = ta_actor_lookup(system, sender->sparse->parent.actor);
    return parent != NULL && ta_invoke_find(parent, sender->sparse->parent.invoke_id) >= 0;
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
        /* Autoforwarding is an exact envelope copy, including the invoke ID
         * that identifies the child which originally sent the event. */
        ta_event copy = *event;
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
        ta_invoke_cancel_at(it->system, sparse, sparse->invoke_count - 1u);
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
        ta_invoke_cancel_at(system, sparse, sparse->invoke_count - 1u);
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

/* error.communication.invoke.<id>.unavailable */
static ta_result ta_invoke_unavailable_name(ta_system *system, ta_invoke_id invoke, ta_event_name_id *out) {
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
    ta_str text = ta_invoke_id_text(system, invoke);
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
    *out = name;
    return result;
}

/*
 * Reconstruction lost the service behind an invocation: it is retired and
 * reported once, with no fault data, because nothing failed — the invocation
 * simply did not survive the image (ADR 0038).
 */
TA_PRIVATE ta_result ta_invoke_report_lost(ta_system *system, ta_actor *actor, ta_invoke_id invoke) {
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_unavailable_name(system, invoke, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = { .name = name, .type = TA_EVENT_PLATFORM, .invoke_id = TA_INVOKE_ID_NONE };
    return ta_interpreter_enqueue_internal(system, actor, &event);
}

static ta_result ta_invoke_unavailable(ta_system *system, ta_actor *actor, const ta_invoke_report_params *params) {
    ta_event_name_id name = 0;
    ta_result result = ta_invoke_unavailable_name(system, params->invoke_id, &name);
    if (result != TA_OK) {
        return result;
    }
    ta_event event = { .name = name, .type = TA_EVENT_PLATFORM, .invoke_id = TA_INVOKE_ID_NONE };
    if (params->as.fault != NULL) {
        event.data = params->as.fault->data;
        ta_value_retain(&event.data);
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
        (void)ta_timer_withdraw(&system->timers, ta_actor_timers(actor), TA_SEND_ID_COMPLETION);
        return TA_ERR_NOT_FOUND;
    }
    if (ta_timer_withdraw(&system->timers, ta_actor_timers(actor), TA_SEND_ID_COMPLETION) != TA_OK) {
        return TA_OK; /* already delivered */
    }
    return ta_invoke_publish(system, actor);
}


TA_API void ta_definition_destroy(ta_definition *definition) {
    if (definition == NULL) {
        return;
    }
    if (!definition->registered && definition->system != NULL) {
        ta_system *system = definition->system;
        if (definition->unregistered_previous != NULL) {
            definition->unregistered_previous->unregistered_next = definition->unregistered_next;
        } else if (system->unregistered_definitions == definition) {
            system->unregistered_definitions = definition->unregistered_next;
        }
        if (definition->unregistered_next != NULL) {
            definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
        }
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

/*
 * Matches an event against one descriptor (ADR 0060 section 5). A `*`
 * segment matches zero or more event segments, anywhere in the descriptor;
 * a descriptor used up before the event is a prefix match. Two pointers,
 * no recursion: on a mismatch the last `*` passed takes one more event
 * segment and matching resumes after it. O(event x descriptor) time, O(1)
 * space.
 */
typedef struct {
    const ta_atom *event;
    uint32_t event_count;
    const ta_event_pattern_segment *pattern;
    uint32_t pattern_count;
} ta_pattern_match;

static bool ta_pattern_match_run(const ta_pattern_match *match) {
    uint32_t event = 0;
    uint32_t pattern = 0;
    bool starred = false;
    uint32_t star_pattern = 0;
    uint32_t star_event = 0;
    while (pattern < match->pattern_count) {
        const ta_event_pattern_segment *segment = &match->pattern[pattern];
        if (segment->flags & TA_EVENT_PATTERN_SEGMENT_WILDCARD) {
            starred = true;
            star_pattern = pattern;
            star_event = event;
            pattern += 1u;
        } else if (event < match->event_count && match->event[event] == segment->atom) {
            event += 1u;
            pattern += 1u;
        } else if (starred && star_event < match->event_count) {
            star_event += 1u;
            event = star_event;
            pattern = star_pattern + 1u;
        } else {
            return false;
        }
    }
    return true;
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
    return ta_pattern_match_run(&match);
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
        .actor_fixed_bytes = definition->actor_block_bytes,
        .definition_bytes = ta_arena_allocated_bytes(&definition->arena),
    };
    return TA_OK;
}


/* A state by its interned name, for the name index built after pass one. */
typedef struct {
    ta_atom name;
    ta_state_id state;
} ta_state_key;

/*
 * The compiler's explicit stacks (ADR 0060: no recursion in core).
 *
 * A level of pass one: one list of sibling states, the parent they belong
 * to, and the next one to collect. `current` is the state whose children
 * the level above is collecting.
 */
typedef struct {
    const ta_scxml_state_spec *specs;
    ta_state_id parent;
    ta_state_id current;
    uint32_t count;
    uint32_t base;
    uint32_t index;
} ta_collect_frame;

/*
 * A block of executable content being compiled. `action` is the action at
 * `index` while its nested blocks are compiled; `stage` says which one:
 * an <if>'s then block, then its else block, or a <foreach>'s body.
 */
typedef enum {
    TA_BLOCK_NEXT = 0,
    TA_BLOCK_THEN,
    TA_BLOCK_ELSE,
    TA_BLOCK_BODY
} ta_block_stage;

typedef struct {
    const ta_scxml_action_spec *specs;
    ta_spec_member member;
    ta_block_stage stage;
    uint32_t count;
    uint32_t base;
    uint32_t index;
    ta_action_def action;
} ta_block_frame;

/*
 * At or below this many states the name index is scanned; above it, it is
 * binary-searched. The index is sorted either way, so both give the same
 * answer.
 */
#define TA_COMPILER_LINEAR_NAMES 16u

/* Longer unknown targets get no nearest-name suggestion. */
#define TA_COMPILER_NEAREST_MAX 128u

/*
 * Compiles a ta_scxml_spec in two passes. Pass one assigns state IDs in
 * document (pre-)order and records child ranges; pass two resolves targets and
 * compiles transitions, initial transitions, executable content, and data.
 * Every table is built in a growable vector and finally copied into the
 * definition's arena, which owns all compiled storage.
 *
 * Errors (ADR 0058). The compiler keeps the path to the element it is working
 * on: the state it is in (`path_state`, whose own path from the root follows
 * from the state table) and a stack of steps below it. Descending pushes a
 * step and returning pops it; nothing else happens on success. The first
 * fault stops the compile and is recorded once, with the path copied into the
 * caller's error arena.
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
    ta_vec path;         /* ta_spec_step: the steps below path_state */
    ta_vec names;        /* ta_state_key: every state, sorted by (name, state) after pass one */
    ta_vec levels;       /* ta_collect_frame: pass one's stack */
    ta_vec blocks;       /* ta_block_frame: the stack of nested blocks being compiled */
    uint32_t action_depth; /* the deepest stack of blocks so far */
    ta_state_id path_state;
    ta_compile_error *error;
    ta_arena *error_arena;
    /* Replay (ADR 0058 section 3): stop at this expression ID. */
    uint32_t locate;
    bool failed;
    uint64_t hash;
} ta_compiler;

typedef struct {
    ta_vec *vec;
    void **out;
    uint32_t *count;
} ta_compiler_seal_params;

#define TA_PHRASE(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * Faults and the path
 */

typedef struct {
    ta_compile_error_code code;
    ta_str subject;
    uint32_t subject_offset;
    uint32_t subject_length;
    ta_str related;
    ta_str message;
    /* The result to return; zero picks the code's usual one. */
    ta_result result;
} ta_compiler_fault;

static ta_state_def *ta_compiler_state(ta_compiler *compiler, ta_state_id state) {
    return ta_vec_at(&compiler->states, state);
}

static const ta_scxml_state_spec *ta_compiler_state_spec(ta_compiler *compiler, ta_state_id state) {
    return *(const ta_scxml_state_spec **)ta_vec_at(&compiler->state_specs, state);
}

/* The state's index in its parent's `states` list: specs are arrays. */
static uint32_t ta_compiler_state_index(ta_compiler *compiler, ta_state_id state) {
    ta_state_id parent = ta_compiler_state(compiler, state)->parent;
    const ta_scxml_state_spec *list =
        parent == TA_STATE_NONE ? compiler->spec->states : ta_compiler_state_spec(compiler, parent)->states;
    return (uint32_t)(ta_compiler_state_spec(compiler, state) - list);
}

static void ta_compiler_copy_path(ta_compiler *compiler) {
    uint32_t depth = 0;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        depth += 1u;
    }
    uint32_t length = depth + compiler->path.count;
    if (length == 0) {
        return;
    }
    ta_spec_step *steps = ta_arena_alloc(compiler->error_arena, (size_t)length * sizeof(ta_spec_step),
                                         _Alignof(ta_spec_step));
    if (steps == NULL) {
        return; /* the error keeps its code and subject without a path */
    }
    uint32_t index = depth;
    for (ta_state_id at = compiler->path_state; at != TA_STATE_NONE; at = ta_compiler_state(compiler, at)->parent) {
        index -= 1u;
        steps[index] = (ta_spec_step){ .member = TA_SPEC_MEMBER_STATES,
                                       .index = ta_compiler_state_index(compiler, at) };
    }
    for (uint32_t step = 0; step < compiler->path.count; step += 1u) {
        steps[depth + step] = *(const ta_spec_step *)ta_vec_at(&compiler->path, step);
    }
    compiler->error->path = steps;
    compiler->error->path_length = length;
}

static ta_result ta_compiler_record(ta_compiler *compiler, const ta_compiler_fault *fault) {
    ta_result result = fault->result;
    if (result == TA_OK) {
        switch (fault->code) {
            case TA_COMPILE_ERROR_DATAMODEL_MISMATCH:
            case TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT:
            case TA_COMPILE_ERROR_LIMIT:
                result = TA_ERR_UNSUPPORTED;
                break;
            default:
                result = TA_ERR_INVALID;
                break;
        }
    }
    if (compiler->failed) {
        return result;
    }
    compiler->failed = true;
    if (compiler->error == NULL) {
        return result;
    }
    *compiler->error = (ta_compile_error){
        .code = fault->code,
        .subject = fault->subject,
        .subject_offset = fault->subject_offset,
        .subject_length = fault->subject_length,
        .related = fault->related,
        .message = fault->message,
    };
    if (compiler->error_arena != NULL) {
        ta_compiler_copy_path(compiler);
    }
    return result;
}

static ta_result ta_compiler_fail(ta_compiler *compiler, ta_compile_error_code code, ta_str subject,
                                  ta_str message) {
    return ta_compiler_record(compiler, &(ta_compiler_fault){ .code = code, .subject = subject, .message = message });
}

static ta_result ta_compiler_enter(ta_compiler *compiler, ta_spec_member member, uint32_t index) {
    ta_spec_step step = { .member = member, .index = index };
    return ta_vec_push(&compiler->path, &step);
}

/* Steps are popped only on success: a fault ends the compile. */
static void ta_compiler_leave(ta_compiler *compiler) {
    compiler->path.count -= 1u;
}

/* Starts the path over at `state` (TA_STATE_NONE: the root). */
static void ta_compiler_at(ta_compiler *compiler, ta_state_id state) {
    compiler->path_state = state;
    compiler->path.count = 0;
}

static ta_result ta_compiler_fail_at(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                     const ta_compiler_fault *fault) {
    ta_result entered = ta_compiler_enter(compiler, member, index);
    return entered != TA_OK ? entered : ta_compiler_record(compiler, fault);
}

/*
 * Optimal string alignment distance, a transposition counting one edit, so
 * "idel" is one edit from "idle". Error path only. UINT32_MAX when the rows
 * cannot be allocated.
 */
static uint32_t ta_compiler_distance(ta_allocator allocator, ta_str left, ta_str right) {
    uint32_t width = right.len + 1u;
    size_t size = (size_t)width * 3u * sizeof(uint32_t);
    uint32_t *rows = ta_mem_alloc(allocator, size, _Alignof(uint32_t));
    if (rows == NULL) {
        return UINT32_MAX;
    }
    uint32_t *before = rows;
    uint32_t *previous = rows + width;
    uint32_t *current = rows + 2u * width;
    for (uint32_t column = 0; column < width; column += 1u) {
        previous[column] = column;
    }
    for (uint32_t row = 1; row <= left.len; row += 1u) {
        current[0] = row;
        for (uint32_t column = 1; column < width; column += 1u) {
            uint32_t cost = left.ptr[row - 1u] == right.ptr[column - 1u] ? 0u : 1u;
            uint32_t best = previous[column] + 1u;
            if (current[column - 1u] + 1u < best) {
                best = current[column - 1u] + 1u;
            }
            if (previous[column - 1u] + cost < best) {
                best = previous[column - 1u] + cost;
            }
            if (row > 1u && column > 1u && left.ptr[row - 1u] == right.ptr[column - 2u] &&
                left.ptr[row - 2u] == right.ptr[column - 1u] && before[column - 2u] + 1u < best) {
                best = before[column - 2u] + 1u;
            }
            current[column] = best;
        }
        uint32_t *spare = before;
        before = previous;
        previous = current;
        current = spare;
    }
    uint32_t distance = previous[right.len];
    ta_mem_free(allocator, rows, &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    return distance;
}

/* The state ID nearest to `id`, within a third of its length; the first
 * such in document order on a tie. Empty when none is that near. */
static ta_str ta_compiler_nearest(ta_compiler *compiler, ta_str id) {
    ta_str nearest = {0};
    uint32_t best = UINT32_MAX;
    if (id.len > TA_COMPILER_NEAREST_MAX) {
        return nearest; /* nobody misspells a name this long by a few letters */
    }
    for (ta_state_id state = 0; state < compiler->states.count; state += 1u) {
        ta_str candidate = ta_compiler_state_spec(compiler, state)->id;
        uint32_t gap = candidate.len > id.len ? candidate.len - id.len : id.len - candidate.len;
        if (3u * (uint64_t)gap > id.len) {
            continue; /* the distance is at least the gap in length */
        }
        uint32_t distance = ta_compiler_distance(compiler->allocator, id, candidate);
        if (distance != 0 && distance < best && 3u * (uint64_t)distance <= id.len) {
            best = distance;
            nearest = candidate;
        }
    }
    return nearest;
}

/* ------------------------------------------------------------------------
 * Leaves
 */

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
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("required text is empty"));
    }
    ta_compiler_hash_str(compiler, text);
    return ta_atom_intern(ta_compiler_atoms(compiler), text, out);
}

static ta_result ta_compiler_atom_at(ta_compiler *compiler, ta_spec_member member, ta_str text, ta_atom *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_atom(compiler, text, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
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
        digits = true; /* ".5s": SCXML durations are CSS2 times, whose whole part is optional */
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
    if (compiler->locate == compiler->expressions.count &&
        (expression->kind == TA_EXPRESSION_SOURCE || expression->kind == TA_EXPRESSION_HOST)) {
        /* Replay: this is the expression the data model refused. */
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXPRESSION_REJECTED,
                                expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0},
                                TA_PHRASE("the data model refused this expression"));
    }
    ta_expression copy = { .kind = expression->kind };
    ta_result result = TA_OK;
    switch (expression->kind) {
        case TA_EXPRESSION_SOURCE:
            ta_compiler_hash_str(compiler, expression->as.source);
            if (expression->as.source.ptr == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                        TA_PHRASE("an expression has no source text"));
            }
            result = ta_arena_copy_str(&compiler->definition->arena, expression->as.source,
                                       &copy.as.source);
            break;
        case TA_EXPRESSION_HOST:
            if (expression->as.host.v == NULL || expression->as.host.v->retain == NULL) {
                return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                        TA_PHRASE("a host expression has no retain operation"));
            }
            copy.as.host = expression->as.host;
            result = expression->as.host.v->retain(expression->as.host.self, expression->as.host.value,
                                                   compiler->allocator, &copy.as.host.value);
            if (result != TA_OK && result != TA_ERR_NOMEM) {
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_HOST_EXPRESSION,
                                                        .message = TA_PHRASE("a host expression could not be retained"),
                                                        .result = result,
                                                    });
            }
            break;
        case TA_EXPRESSION_NONE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, (ta_str){0},
                                    TA_PHRASE("an expression is required"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown expression kind"));
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

static ta_result ta_compiler_expression_at(ta_compiler *compiler, ta_spec_member member,
                                           const ta_expression *expression, ta_expr_id *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_expression(compiler, expression, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
}

/* A dotted event name, as <raise event> and <send event> write one. */
static ta_result ta_compiler_event_name(ta_compiler *compiler, ta_str text, ta_event_name_id *out) {
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    if (result != TA_OK) {
        return result;
    }
    ta_compiler_hash_str(compiler, text);
    result = ta_event_name_intern_dotted(&compiler->system->event_names, text, out);
    if (result == TA_ERR_INVALID) {
        return text.len == 0
                   ? ta_compiler_fail(compiler, TA_COMPILE_ERROR_MISSING_VALUE, text, TA_PHRASE("an event name is required"))
                   : ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR, text,
                                      TA_PHRASE("an event name has an empty segment"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
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

/* `media_type` is empty for literal text (a send target, an invoke src). */
static ta_result ta_compiler_content(ta_compiler *compiler, ta_str media_type, ta_bytes bytes,
                                     ta_content_id *out) {
    ta_content_def content = {0};
    if (media_type.len != 0) {
        ta_result copied = ta_arena_copy_str(&compiler->definition->arena, media_type, &content.media_type);
        if (copied != TA_OK) {
            return copied;
        }
    }
    /* A media type is part of what the chart says, so it is part of its
     * revision (ADR 0053): two charts differing only in one must not share a
     * fingerprint. */
    ta_compiler_hash_str(compiler, media_type);
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
            return ta_compiler_content(compiler, spec->as.content.media_type, spec->as.content.bytes,
                                       &out->as.content);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown value kind"));
    }
}

static ta_result ta_compiler_value_at(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_value_spec *spec, ta_value_source *out) {
    ta_result result = ta_compiler_enter(compiler, member, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value(compiler, spec, out);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    return result;
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
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_PARAMS, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &param.name);
        }
        if (result == TA_OK) {
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].value, &param.value);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            *(ta_param_def *)ta_vec_at(&compiler->params, base + index) = param;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out);

static ta_result ta_compiler_malformed(ta_compiler *compiler, ta_spec_member member) {
    return ta_compiler_fail_at(compiler, member, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MALFORMED_SPEC,
                                                     .message = TA_PHRASE("unknown kind") });
}

static ta_result ta_compiler_send(ta_compiler *compiler, const ta_scxml_send_spec *spec,
                                  ta_send_action *out) {
    ta_result result = TA_OK;
    switch (spec->id.kind) {
        case TA_SCXML_ID_NONE:
            out->id.kind = TA_ID_SOURCE_GENERATED;
            break;
        case TA_SCXML_ID_LITERAL:
            out->id.kind = TA_ID_SOURCE_LITERAL;
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_ID, &spec->id.as.location,
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
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
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_TYPE, &spec->type.as.expr,
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result != TA_OK) {
        return result;
    }
    out->delay.text = TA_AUTHORED_TEXT_NONE;
    ta_compiler_hash_u32(compiler, spec->event.kind);
    if (spec->event.kind == TA_SCXML_EVENT_EXPR) {
        out->event.kind = TA_EVENT_SOURCE_EXPR;
        result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_EVENT, &spec->event.as.expr,
                                           &out->event.as.expr);
    } else if (spec->event.kind == TA_SCXML_EVENT_NAME) {
        out->event.kind = TA_EVENT_SOURCE_NAME;
        result = ta_compiler_event_name(compiler, spec->event.as.name, &out->event.as.name);
    } else if (spec->event.kind == TA_SCXML_EVENT_NONE) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                         .message = TA_PHRASE("a send needs an event") });
    } else {
        return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_EVENT);
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
            result = ta_compiler_content(compiler, (ta_str){0},
                                         (ta_bytes){ .ptr = (const uint8_t *)spec->target.as.literal.ptr,
                                                     .len = spec->target.as.literal.len },
                                         &out->target.as.content);
            break;
        case TA_SCXML_STRING_EXPR:
            out->target.kind = TA_VALUE_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_TARGET, &spec->target.as.expr,
                                               &out->target.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TARGET);
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
            if (result == TA_ERR_INVALID) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DELAY, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){
                                               .code = TA_COMPILE_ERROR_BAD_DURATION,
                                               .subject = spec->delay.as.literal,
                                               .message = TA_PHRASE("not an SCXML duration such as \"500ms\" or \"2.5s\""),
                                           });
            }
            if (result == TA_OK) {
                result = ta_compiler_keep_text(compiler, spec->delay.as.literal, &out->delay.text);
            }
            break;
        case TA_SCXML_STRING_EXPR:
            out->delay.kind = TA_DELAY_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_DELAY, &spec->delay.as.expr,
                                               &out->delay.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_DELAY);
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK && out->params.length != 0 && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a send has both params and content"));
    }
    return result;
}

/*
 * <foreach>: the array is an expression; item and index are variable names
 * whose legality the data model judges when it binds them (SCXML 4.6). The
 * body is compiled by ta_compiler_block after this.
 */
static ta_result ta_compiler_foreach(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                     ta_foreach_action *out) {
    ta_result result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_EXPR, &spec->as.foreach.array, &out->array);
    if (result == TA_OK) {
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ITEM, spec->as.foreach.item, &out->item);
    }
    if (result == TA_OK && spec->as.foreach.index.ptr != NULL) {
        out->has_index = true;
        result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_INDEX, spec->as.foreach.index, &out->index);
    }
    return result;
}

/*
 * One action, up to its nested blocks: everything an <if> or <foreach> has
 * before them. ta_compiler_block compiles the nested blocks next, in the
 * order the recursive compiler did.
 */
static ta_result ta_compiler_action(ta_compiler *compiler, const ta_scxml_action_spec *spec,
                                    ta_action_def *out) {
    ta_compiler_hash_u32(compiler, spec->kind);
    ta_result result = TA_OK;
    switch (spec->kind) {
        case TA_SCXML_ACTION_RAISE:
            out->kind = TA_ACTION_RAISE;
            result = ta_compiler_event_name(compiler, spec->as.raise.event, &out->as.raise.event);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.raise.data,
                                              &out->as.raise.data);
            }
            return result;
        case TA_SCXML_ACTION_SEND:
            out->kind = TA_ACTION_SEND;
            return ta_compiler_send(compiler, &spec->as.send, &out->as.send);
        case TA_SCXML_ACTION_CANCEL:
            out->kind = TA_ACTION_CANCEL;
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_EXPR) {
                out->as.cancel.sendid.kind = TA_TEXT_SOURCE_EXPR;
                return ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_SENDID, &spec->as.cancel.sendid.as.expr,
                                                 &out->as.cancel.sendid.as.expr);
            }
            if (spec->as.cancel.sendid.kind == TA_SCXML_STRING_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_SENDID, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("a cancel needs a sendid") });
            }
            if (spec->as.cancel.sendid.kind != TA_SCXML_STRING_LITERAL) {
                return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_SENDID);
            }
            out->as.cancel.sendid.kind = TA_TEXT_SOURCE_ATOM;
            return ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_SENDID, spec->as.cancel.sendid.as.literal,
                                       &out->as.cancel.sendid.as.atom);
        case TA_SCXML_ACTION_ASSIGN:
            out->kind = TA_ACTION_ASSIGN;
            if (spec->as.assign.value.kind == TA_SCXML_VALUE_NONE) {
                return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, TA_SPEC_INDEX_NONE,
                                           &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_MISSING_VALUE,
                                                                 .message = TA_PHRASE("an assign needs a value") });
            }
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_LOCATION, &spec->as.assign.location,
                                               &out->as.assign.location);
            if (result == TA_OK) {
                result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->as.assign.value,
                                              &out->as.assign.value);
            }
            return result;
        case TA_SCXML_ACTION_SCRIPT:
            out->kind = TA_ACTION_SCRIPT;
            return ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_EXPR, &spec->as.script.script, &out->as.script);
        case TA_SCXML_ACTION_LOG:
            out->kind = TA_ACTION_LOG;
            if (spec->as.log.label.ptr != NULL) {
                out->as.log.has_label = true;
                result = ta_atom_intern(ta_compiler_atoms(compiler), spec->as.log.label, &out->as.log.label);
            }
            if (result == TA_OK && spec->as.log.expr.kind != TA_EXPRESSION_NONE) {
                out->as.log.has_expr = true;
                result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_EXPR, &spec->as.log.expr,
                                                   &out->as.log.expr);
            }
            return result;
        case TA_SCXML_ACTION_IF:
            out->kind = TA_ACTION_IF;
            return ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_CONDITION, &spec->as.conditional.condition,
                                             &out->as.conditional.condition);
        case TA_SCXML_ACTION_FOREACH:
            out->kind = TA_ACTION_FOREACH;
            return ta_compiler_foreach(compiler, spec, &out->as.foreach);
        case TA_SCXML_ACTION_INVOKE:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, (ta_str){0},
                                    TA_PHRASE("<invoke> is not executable content"));
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown action kind"));
    }
}

/* Starts a block: reserves its actions, so nested blocks append behind it. */
static ta_result ta_compiler_block_open(ta_compiler *compiler, ta_spec_member member,
                                        const ta_scxml_action_spec *specs) {
    ta_block_frame frame = { .specs = specs, .member = member };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_ACTION_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->actions, frame.count, &frame.base);
    if (result == TA_OK) {
        result = ta_vec_push(&compiler->blocks, &frame);
    }
    if (result == TA_OK && compiler->blocks.count > compiler->action_depth) {
        compiler->action_depth = compiler->blocks.count;
    }
    return result;
}

static ta_block_frame *ta_compiler_block_top(ta_compiler *compiler) {
    return ta_vec_at(&compiler->blocks, compiler->blocks.count - 1u);
}

/* Stores the finished action of the top block and moves to the next one. */
static void ta_compiler_block_store(ta_compiler *compiler, ta_block_frame *frame) {
    ta_compiler_leave(compiler);
    *(ta_action_def *)ta_vec_at(&compiler->actions, frame->base + frame->index) = frame->action;
    frame->index += 1u;
    frame->stage = TA_BLOCK_NEXT;
}

/*
 * Gives a finished nested block to the action that owns it, in the top
 * frame, and opens the owner's next nested block if it has one.
 */
static ta_result ta_compiler_block_deliver(ta_compiler *compiler, ta_span32 span) {
    ta_block_frame *frame = ta_compiler_block_top(compiler);
    switch (frame->stage) {
        case TA_BLOCK_THEN:
            frame->action.as.conditional.then_actions = span;
            frame->stage = TA_BLOCK_ELSE;
            return ta_compiler_block_open(compiler, TA_SPEC_MEMBER_ELSE,
                                          frame->specs[frame->index].as.conditional.else_actions);
        case TA_BLOCK_ELSE:
            frame->action.as.conditional.else_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        case TA_BLOCK_BODY:
            frame->action.as.foreach.body_actions = span;
            ta_compiler_block_store(compiler, frame);
            return TA_OK;
        default:
            return TA_ERR_INVALID; /* unreachable: only an owner waits for a block */
    }
}

/*
 * Compiles a block of executable content and every block nested in it,
 * without recursion (ADR 0060). Each open block is a frame; an <if> or
 * <foreach> in a block compiles up to its nested blocks, opens the first,
 * and resumes when it is delivered. Actions, expressions, nested block
 * reservations and path steps come in exactly the order the recursive
 * compiler produced them, so revisions and numbering do not change.
 */
static ta_result ta_compiler_block(ta_compiler *compiler, ta_spec_member member,
                                   const ta_scxml_action_spec *specs, ta_span32 *out) {
    uint32_t floor = compiler->blocks.count;
    ta_result result = ta_compiler_block_open(compiler, member, specs);
    while (result == TA_OK) {
        ta_block_frame *frame = ta_compiler_block_top(compiler);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->blocks.count -= 1u;
            if (compiler->blocks.count == floor) {
                *out = span;
                return TA_OK;
            }
            result = ta_compiler_block_deliver(compiler, span);
            continue;
        }
        const ta_scxml_action_spec *spec = &frame->specs[frame->index];
        ta_action_def action = {0};
        result = ta_compiler_enter(compiler, frame->member, frame->index);
        if (result == TA_OK) {
            result = ta_compiler_action(compiler, spec, &action);
        }
        if (result != TA_OK) {
            break;
        }
        frame = ta_compiler_block_top(compiler);
        frame->action = action;
        if (action.kind == TA_ACTION_IF) {
            frame->stage = TA_BLOCK_THEN;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_THEN, spec->as.conditional.then_actions);
        } else if (action.kind == TA_ACTION_FOREACH) {
            frame->stage = TA_BLOCK_BODY;
            result = ta_compiler_block_open(compiler, TA_SPEC_MEMBER_BODY, spec->as.foreach.body);
        } else {
            ta_compiler_block_store(compiler, frame);
        }
    }
    compiler->blocks.count = floor;
    return result;
}

/*
 * Compiles a list of <onentry> or <onexit> handlers. Each handler keeps its
 * own action block, so an error in one handler does not skip the next.
 */
static ta_result ta_compiler_handlers(ta_compiler *compiler, ta_spec_member member,
                                      const ta_scxml_handler_spec *specs, ta_span32 *out) {
    uint32_t count = 0;
    while (specs != NULL && specs[count].actions != NULL) {
        count += 1;
    }
    ta_compiler_hash_u32(compiler, count);
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->handlers, count, &base);
    for (uint32_t index = 0; result == TA_OK && index < count; index += 1) {
        ta_span32 block = {0};
        result = ta_compiler_enter(compiler, member, index);
        if (result == TA_OK) {
            result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, specs[index].actions, &block);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            ta_compiler_hash_u32(compiler, block.length);
            *(ta_span32 *)ta_vec_at(&compiler->handlers, base + index) = block;
        }
    }
    *out = (ta_span32){ .offset = base, .length = count };
    return result;
}

static ta_result ta_compiler_kind(ta_compiler *compiler, ta_scxml_state_kind kind, ta_state_kind *out) {
    switch (kind) {
        case TA_SCXML_STATE_ATOMIC: *out = TA_STATE_ATOMIC; return TA_OK;
        case TA_SCXML_STATE_COMPOUND: *out = TA_STATE_COMPOUND; return TA_OK;
        case TA_SCXML_STATE_PARALLEL: *out = TA_STATE_PARALLEL; return TA_OK;
        case TA_SCXML_STATE_FINAL: *out = TA_STATE_FINAL; return TA_OK;
        case TA_SCXML_STATE_HISTORY_SHALLOW: *out = TA_STATE_HISTORY_SHALLOW; return TA_OK;
        case TA_SCXML_STATE_HISTORY_DEEP: *out = TA_STATE_HISTORY_DEEP; return TA_OK;
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown state kind"));
    }
}

static bool ta_state_kind_is_history(ta_state_kind kind) {
    return kind == TA_STATE_HISTORY_SHALLOW || kind == TA_STATE_HISTORY_DEEP;
}

static bool ta_state_key_before(ta_state_key left, ta_state_key right) {
    return left.name < right.name || (left.name == right.name && left.state < right.state);
}

static void ta_state_keys_sift(ta_state_key *keys, uint32_t root, uint32_t count) {
    for (;;) {
        uint32_t largest = root;
        uint32_t left = 2u * root + 1u;
        uint32_t right = left + 1u;
        if (left < count && ta_state_key_before(keys[largest], keys[left])) {
            largest = left;
        }
        if (right < count && ta_state_key_before(keys[largest], keys[right])) {
            largest = right;
        }
        if (largest == root) {
            return;
        }
        ta_state_key swap = keys[root];
        keys[root] = keys[largest];
        keys[largest] = swap;
        root = largest;
    }
}

/*
 * Sorts by (name, state): insertion sort for a few keys, heapsort otherwise,
 * so the sort is O(n log n), in place, and allocates nothing. The order is
 * total, so stability does not matter.
 */
static void ta_state_keys_sort(ta_state_key *keys, uint32_t count) {
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 1; index < count; index += 1u) {
            ta_state_key key = keys[index];
            uint32_t at = index;
            while (at > 0 && ta_state_key_before(key, keys[at - 1u])) {
                keys[at] = keys[at - 1u];
                at -= 1u;
            }
            keys[at] = key;
        }
        return;
    }
    for (uint32_t root = count / 2u; root > 0; root -= 1u) {
        ta_state_keys_sift(keys, root - 1u, count);
    }
    for (uint32_t end = count - 1u; end > 0; end -= 1u) {
        ta_state_key swap = keys[0];
        keys[0] = keys[end];
        keys[end] = swap;
        ta_state_keys_sift(keys, 0, end);
    }
}

/* The state named `name`. Pass two only: the index is complete and has no
 * duplicates. */
static bool ta_compiler_find(ta_compiler *compiler, ta_atom name, ta_state_id *out) {
    const ta_state_key *keys = compiler->names.items;
    uint32_t count = compiler->names.count;
    if (count <= TA_COMPILER_LINEAR_NAMES) {
        for (uint32_t index = 0; index < count; index += 1u) {
            if (keys[index].name == name) {
                *out = keys[index].state;
                return true;
            }
        }
        return false;
    }
    uint32_t low = 0;
    uint32_t high = count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (keys[middle].name < name) {
            low = middle + 1u;
        } else {
            high = middle;
        }
    }
    if (low < count && keys[low].name == name) {
        *out = keys[low].state;
        return true;
    }
    return false;
}

/*
 * Builds the name index over the states pass one collected, and reports a
 * duplicate ID. Sorted by (name, state), each run of equal names lists its
 * states in document order. So the second key of a run is the duplicate the
 * walk meets first, and the first key is the definition it repeats. Across
 * runs the smallest such second state wins: that is the duplicate a
 * per-state check during pass one would report.
 *
 * `collected` is the result of pass one. A pass-one fault stops collection
 * at a state after every collected one, so a duplicate among the collected
 * states comes earlier in document order and replaces that fault. That keeps
 * the first error in compile order (ADR 0058 section 2).
 */
static ta_result ta_compiler_index_names(ta_compiler *compiler, ta_result collected) {
    uint32_t count = compiler->states.count;
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&compiler->names, count, &base);
    if (result != TA_OK) {
        return collected != TA_OK ? collected : result;
    }
    ta_state_key *keys = compiler->names.items;
    for (ta_state_id state = 0; state < count; state += 1u) {
        keys[state] = (ta_state_key){ .name = ta_compiler_state(compiler, state)->name, .state = state };
    }
    ta_state_keys_sort(keys, count);
    ta_state_id duplicate = TA_STATE_NONE;
    ta_state_id first = TA_STATE_NONE;
    for (uint32_t index = 1, run = 0; index < count; index += 1u) {
        if (keys[index].name != keys[index - 1u].name) {
            run = index;
        } else if (index == run + 1u && (duplicate == TA_STATE_NONE || keys[index].state < duplicate)) {
            duplicate = keys[index].state;
            first = keys[run].state;
        }
    }
    if (duplicate == TA_STATE_NONE) {
        return collected;
    }
    if (collected != TA_OK && !compiler->failed) {
        return collected; /* an allocation failure, not a fault in the chart */
    }
    compiler->failed = false;
    ta_compiler_at(compiler, ta_compiler_state(compiler, duplicate)->parent);
    result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, ta_compiler_state_index(compiler, duplicate));
    if (result != TA_OK) {
        return result;
    }
    return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_ID, TA_SPEC_INDEX_NONE,
                               &(ta_compiler_fault){
                                   .code = TA_COMPILE_ERROR_DUPLICATE_STATE,
                                   .subject = ta_compiler_state_spec(compiler, duplicate)->id,
                                   .related = ta_compiler_state_spec(compiler, first)->id,
                                   .message = TA_PHRASE("duplicate state ID"),
                               });
}

/* Opens a level of pass one: reserves its slots in `children`. */
static ta_result ta_compiler_level_open(ta_compiler *compiler, const ta_scxml_state_spec *specs, ta_state_id parent) {
    ta_collect_frame frame = { .specs = specs, .parent = parent, .current = TA_STATE_NONE };
    while (specs != NULL && specs[frame.count].kind != TA_SCXML_STATE_NONE) {
        frame.count += 1;
    }
    ta_result result = ta_vec_extend(&compiler->children, frame.count, &frame.base);
    return result == TA_OK ? ta_vec_push(&compiler->levels, &frame) : result;
}

/*
 * Pass one: assigns IDs in document pre-order and records child ranges,
 * without recursion (ADR 0060). A state's children are collected right after
 * it, before its next sibling, as the recursive compiler did, so IDs, the
 * layout of `children` and the hash do not change.
 */
static ta_result ta_compiler_collect(ta_compiler *compiler, const ta_scxml_state_spec *specs,
                                     ta_state_id parent, ta_span32 *out) {
    ta_result result = ta_compiler_level_open(compiler, specs, parent);
    while (result == TA_OK) {
        ta_collect_frame *frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
        if (frame->index == frame->count) {
            ta_span32 span = { .offset = frame->base, .length = frame->count };
            compiler->levels.count -= 1u;
            if (compiler->levels.count == 0) {
                *out = span;
                return TA_OK;
            }
            frame = ta_vec_at(&compiler->levels, compiler->levels.count - 1u);
            ta_compiler_state(compiler, frame->current)->children = span;
            frame->index += 1u;
            continue;
        }
        const ta_scxml_state_spec *spec = &frame->specs[frame->index];
        ta_state_def state = {
            .parent = frame->parent,
            .initial = TA_INITIAL_TRANSITION_NONE,
            .done_data = TA_DONE_DATA_NONE,
            .history_slot = UINT32_MAX,
        };
        ta_compiler_at(compiler, frame->parent);
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_STATES, frame->index);
        if (result == TA_OK) {
            ta_compiler_hash_u32(compiler, spec->kind);
            result = ta_compiler_kind(compiler, spec->kind, &state.kind);
        }
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id, &state.name);
        }
        if (result != TA_OK) {
            break;
        }
        ta_state_id id = compiler->states.count;
        *(ta_state_id *)ta_vec_at(&compiler->children, frame->base + frame->index) = id;
        frame->current = id;
        result = ta_vec_push(&compiler->states, &state);
        if (result == TA_OK) {
            result = ta_vec_push(&compiler->state_specs, &spec);
        }
        if (result == TA_OK) {
            result = ta_compiler_level_open(compiler, spec->states, id);
        }
    }
    compiler->levels.count = 0;
    return result;
}

static ta_result ta_compiler_resolve(ta_compiler *compiler, ta_str id, ta_state_id *out) {
    ta_atom atom = 0;
    ta_result result = ta_compiler_atom(compiler, id, &atom);
    if (result != TA_OK) {
        return result;
    }
    if (ta_compiler_find(compiler, atom, out)) {
        return TA_OK;
    }
    return ta_compiler_record(compiler, &(ta_compiler_fault){
                                            .code = TA_COMPILE_ERROR_UNKNOWN_STATE,
                                            .subject = id,
                                            .related = ta_compiler_nearest(compiler, id),
                                            .message = TA_PHRASE("unknown state"),
                                        });
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
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TARGETS, index);
        if (result == TA_OK) {
            result = ta_compiler_resolve(compiler, params->ids[index], &target);
        }
        if (result == TA_OK && !ta_compiler_descends(compiler, target, params->within)) {
            result = ta_compiler_record(compiler, &(ta_compiler_fault){
                                                      .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                      .subject = params->ids[index],
                                                      .related = ta_compiler_state_spec(compiler, params->within)->id,
                                                      .message = TA_PHRASE("target is not inside its state"),
                                                  });
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
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
                return ta_compiler_record(compiler, &(ta_compiler_fault){
                                                        .code = TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR,
                                                        .subject = text,
                                                        .subject_offset = start,
                                                        .subject_length = index - start,
                                                        .message = TA_PHRASE("an event descriptor has an empty segment"),
                                                    });
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
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                TA_PHRASE("unknown transition kind"));
    }
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_EVENT, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_descriptors(compiler, spec->event, &transition.event_patterns);
    }
    if (result == TA_OK && spec->event.ptr != NULL) {
        result = ta_compiler_keep_text(compiler, spec->event, &transition.event_text);
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
    }
    if (result == TA_OK && spec->condition.kind != TA_EXPRESSION_NONE) {
        result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_CONDITION, &spec->condition,
                                           &transition.condition);
    }
    if (result == TA_OK) {
        result = ta_compiler_targets(compiler, &(ta_compiler_targets_params){
                                                   .ids = spec->targets,
                                                   .within = TA_STATE_NONE,
                                                   .out = &transition.targets,
                                               });
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, spec->actions, &transition.actions);
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
    if (params->spec != NULL) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE);
        if (result != TA_OK) {
            return result;
        }
    }
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
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_BAD_STRUCTURE, (ta_str){0},
                                    TA_PHRASE("no child state to enter"));
        }
        initial.targets.offset = compiler->targets.count;
        initial.targets.length = 1;
        result = ta_vec_push(&compiler->targets, &first);
    }
    if (result == TA_OK && params->spec != NULL) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_ACTIONS, params->spec->actions, &initial.actions);
    }
    if (result == TA_OK) {
        if (params->spec != NULL) {
            ta_compiler_leave(compiler);
        }
        *params->out = compiler->initials.count;
        result = ta_vec_push(&compiler->initials, &initial);
    }
    return result;
}

static ta_result ta_compiler_data(ta_compiler *compiler, const ta_scxml_data_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs != NULL && specs[index].name.ptr != NULL; index += 1) {
        ta_data_decl decl = { .owner_state = owner, .initializer = TA_INITIALIZER_NONE };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DATA, index);
        if (result == TA_OK) {
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_NAME, specs[index].name, &decl.name);
        }
        if (result == TA_OK && specs[index].initializer.kind != TA_SCXML_VALUE_NONE) {
            ta_initializer_def initializer = {0};
            result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &specs[index].initializer,
                                          &initializer.value);
            if (result == TA_OK) {
                decl.initializer = compiler->initializers.count;
                result = ta_vec_push(&compiler->initializers, &initializer);
            }
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
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
    ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE);
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->content, &done.content);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &done.params);
    }
    if (result == TA_OK && done.params.length != 0 && done.content.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("a donedata has both params and content"));
    }
    if (result == TA_OK) {
        ta_compiler_leave(compiler);
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
            return ta_compiler_content(compiler, (ta_str){0},
                                       (ta_bytes){ .ptr = (const uint8_t *)spec->as.literal.ptr,
                                                   .len = spec->as.literal.len },
                                       &out->as.content);
        case TA_SCXML_STRING_EXPR:
            out->kind = TA_VALUE_SOURCE_EXPR;
            return ta_compiler_expression(compiler, &spec->as.expr, &out->as.expr);
        default:
            return ta_compiler_fail(compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                                    TA_PHRASE("unknown kind"));
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
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_ID, spec->id.as.literal, &out->id.as.literal);
            break;
        case TA_SCXML_ID_LOCATION:
            out->id.kind = TA_ID_SOURCE_LOCATION;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_ID, &spec->id.as.location,
                                               &out->id.as.location);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_ID);
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
            result = ta_compiler_atom_at(compiler, TA_SPEC_MEMBER_TYPE, spec->type.as.literal, &out->type.as.atom);
            break;
        case TA_SCXML_STRING_EXPR:
            out->type.kind = TA_TEXT_SOURCE_EXPR;
            result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_TYPE, &spec->type.as.expr,
                                               &out->type.as.expr);
            break;
        default:
            return ta_compiler_malformed(compiler, TA_SPEC_MEMBER_TYPE);
    }
    if (result == TA_OK) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_SRC, TA_SPEC_INDEX_NONE);
        if (result == TA_OK) {
            result = ta_compiler_invoke_text(compiler, &spec->src, &out->src);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    if (result == TA_OK) {
        result = ta_compiler_value_at(compiler, TA_SPEC_MEMBER_DATA_VALUE, &spec->data, &out->data);
    }
    if (result == TA_OK) {
        result = ta_compiler_params(compiler, spec->params, &out->params);
    }
    if (result == TA_OK) {
        result = ta_compiler_block(compiler, TA_SPEC_MEMBER_FINALIZE, spec->finalize, &out->finalize_actions);
    }
    out->autoforward = spec->autoforward;
    ta_compiler_hash_u32(compiler, spec->autoforward ? 1u : 0u);
    /* SCXML: src and inline content are exclusive. */
    if (result == TA_OK && out->src.kind != TA_VALUE_SOURCE_NONE && out->data.kind != TA_VALUE_SOURCE_NONE) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, (ta_str){0},
                                  TA_PHRASE("an invoke has both src and content"));
    }
    return result;
}

static ta_result ta_compiler_invokes(ta_compiler *compiler, const ta_scxml_invoke_spec *specs, ta_state_id owner) {
    for (uint32_t index = 0; specs[index].kind != TA_SCXML_INVOKE_NONE; index += 1) {
        ta_invoke_decl decl = { .owner = owner };
        ta_result result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_INVOKES, index);
        if (result == TA_OK) {
            result = ta_compiler_invoke(compiler, &specs[index], &decl);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
            result = ta_vec_push(&compiler->invokes, &decl);
        }
        if (result != TA_OK) {
            return result;
        }
    }
    return TA_OK;
}

static ta_result ta_compiler_structure(ta_compiler *compiler, ta_spec_member member, uint32_t index,
                                       ta_str message) {
    ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_STRUCTURE, .message = message };
    return member == TA_SPEC_MEMBER_NONE ? ta_compiler_record(compiler, &fault)
                                         : ta_compiler_fail_at(compiler, member, index, &fault);
}

/* Pass two for one state. */
static ta_result ta_compiler_compile_state(ta_compiler *compiler, ta_state_id id) {
    const ta_scxml_state_spec *spec = ta_compiler_state_spec(compiler, id);
    ta_state_def state = *ta_compiler_state(compiler, id);
    ta_state_def parent = {0};
    if (state.parent != TA_STATE_NONE) {
        parent = *ta_compiler_state(compiler, state.parent);
    }
    ta_compiler_at(compiler, id);
    uint32_t real_children = 0;
    for (uint32_t index = 0; index < state.children.length; index += 1) {
        ta_state_id child = *(ta_state_id *)ta_vec_at(&compiler->children, state.children.offset + index);
        real_children += ta_state_kind_is_history(ta_compiler_state(compiler, child)->kind) ? 0u : 1u;
    }
    bool history = ta_state_kind_is_history(state.kind);
    if (spec->done_data != NULL && state.kind != TA_STATE_FINAL) {
        return ta_compiler_structure(compiler, TA_SPEC_MEMBER_DONE_DATA, TA_SPEC_INDEX_NONE,
                                     TA_PHRASE("only a final state has <donedata>"));
    }
    switch (state.kind) {
        case TA_STATE_ATOMIC:
        case TA_STATE_FINAL:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("an atomic or final state has no child states"));
            }
            if (spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("an atomic or final state has no <initial>"));
            }
            break;
        case TA_STATE_COMPOUND:
        case TA_STATE_PARALLEL:
            if (real_children == 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a compound or parallel state needs a child state"));
            }
            if (state.kind == TA_STATE_PARALLEL && spec->initial != NULL) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_INITIAL, TA_SPEC_INDEX_NONE,
                                             TA_PHRASE("a parallel state has no <initial>"));
            }
            break;
        default:
            if (state.children.length != 0) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_STATES, 0,
                                             TA_PHRASE("a history state has no child states"));
            }
            if (state.parent == TA_STATE_NONE ||
                (parent.kind != TA_STATE_COMPOUND && parent.kind != TA_STATE_PARALLEL)) {
                return ta_compiler_structure(compiler, TA_SPEC_MEMBER_NONE, 0,
                                             TA_PHRASE("a history state must be inside a compound or parallel state"));
            }
            break;
    }

    state.transitions.offset = compiler->transitions.count;
    ta_result result = TA_OK;
    for (uint32_t index = 0; result == TA_OK && spec->transitions != NULL &&
                             spec->transitions[index].kind != TA_SCXML_TRANSITION_NONE;
         index += 1) {
        result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, index);
        if (result == TA_OK) {
            result = ta_compiler_transition(compiler, id, &spec->transitions[index]);
        }
        if (result == TA_OK) {
            ta_compiler_leave(compiler);
        }
    }
    state.transitions.length = compiler->transitions.count - state.transitions.offset;
    if (result != TA_OK) {
        return result;
    }
    if (history) {
        /* A history state has exactly one targetless-event default transition into its parent. */
        ta_compiler_fault fault = { .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                    .message = TA_PHRASE("a history state needs exactly one default transition") };
        if (state.transitions.length != 1) {
            return state.transitions.length == 0
                       ? ta_compiler_record(compiler, &fault)
                       : ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 1, &fault);
        }
        const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state.transitions.offset);
        if (fallback->event_patterns.length != 0 || fallback->condition != UINT32_MAX ||
            fallback->targets.length == 0) {
            fault.message = TA_PHRASE("a history default transition has targets and no event or condition");
            return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0, &fault);
        }
        for (uint32_t index = 0; index < fallback->targets.length; index += 1) {
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, fallback->targets.offset + index);
            if (!ta_compiler_descends(compiler, target, state.parent)) {
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_TARGET_OUTSIDE,
                                                     .subject = spec->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, state.parent)->id,
                                                     .message = TA_PHRASE("target is not inside its state"),
                                                 });
                }
                return result;
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
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONENTRY, spec->onentry, &state.onentry_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_handlers(compiler, TA_SPEC_MEMBER_ONEXIT, spec->onexit, &state.onexit_handlers);
    }
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, id);
    }
    if (result == TA_OK && spec->done_data != NULL) {
        result = ta_compiler_done_data(compiler, spec->done_data, &state.done_data);
    }
    /* Invoke: compile this state's <invoke> children. */
    if (result == TA_OK && spec->invokes != NULL && spec->invokes[0].kind != TA_SCXML_INVOKE_NONE) {
        result = history ? ta_compiler_structure(compiler, TA_SPEC_MEMBER_INVOKES, 0,
                                                 TA_PHRASE("a history state has no <invoke>"))
                         : ta_compiler_invokes(compiler, spec->invokes, id);
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
        &compiler->invokes, &compiler->texts, &compiler->path, &compiler->names, &compiler->levels,
        &compiler->blocks,
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
        return ta_compiler_fail_at(compiler, spec->identity != NULL ? TA_SPEC_MEMBER_IDENTITY : TA_SPEC_MEMBER_NAME,
                                   TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_NO_FAMILY,
                                                         .message = TA_PHRASE("the chart has no name and no identity family") });
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
    definition->max_action_depth = compiler->action_depth;
    definition->microstep_needs_sparse = definition->invoke_decl_count != 0;
    for (uint32_t action = 0; action < definition->action_count && !definition->microstep_needs_sparse; action += 1u) {
        definition->microstep_needs_sparse = definition->action_defs[action].kind == TA_ACTION_SEND;
    }
    for (ta_state_id state = 0; state < definition->state_count && !definition->microstep_needs_sparse; state += 1u) {
        definition->microstep_needs_sparse = definition->states[state].parent == TA_STATE_NONE &&
                                             definition->states[state].kind == TA_STATE_FINAL;
    }
    if (definition->global_script_actions.length != 0 && definition->max_action_depth == 0) {
        definition->max_action_depth = 1;
    }
    /* Depths in one pass: every parent has a lower ID than its children. */
    if (definition->state_count != 0) {
        size_t size = (size_t)definition->state_count * sizeof(uint32_t);
        uint32_t *depths = ta_mem_alloc(compiler->allocator, size, _Alignof(uint32_t));
        if (depths == NULL) {
            return TA_ERR_NOMEM;
        }
        for (ta_state_id state = 0; state < definition->state_count; state += 1u) {
            ta_state_id parent = definition->states[state].parent;
            depths[state] = parent == TA_STATE_NONE ? 1u : depths[parent] + 1u;
            if (depths[state] > definition->max_state_depth) {
                definition->max_state_depth = depths[state];
            }
        }
        ta_mem_free(compiler->allocator, depths,
                    &(ta_allocator_free_params){ .size = size, .alignment = _Alignof(uint32_t) });
    }
    uint32_t words = ta_bits_words(definition->state_count);
    if (words * 8u > UINT16_MAX) {
        return ta_compiler_fail(compiler, TA_COMPILE_ERROR_LIMIT, (ta_str){0},
                                TA_PHRASE("the chart has more states than a configuration can hold"));
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

/*
 * The compile order, which decides which fault a chart with several reports
 * (ADR 0058 section 2): the data-model requirement; pass one over every state
 * in document order (kind, ID, duplicates); the root's <data>; pass two for
 * each state in document order; the root <initial>; the root <script>; the
 * configuration limit and the family; then the data model's preparation.
 */
/* One history state on the walk below, and the next default target to follow. */
typedef struct {
    ta_state_id state;
    uint32_t next;
} ta_history_walk;

/* The default targets of a compiled history state. */
static ta_span32 ta_compiler_history_targets(ta_compiler *compiler, ta_state_id history) {
    const ta_state_def *state = ta_compiler_state(compiler, history);
    const ta_transition_def *fallback = ta_vec_at(&compiler->transitions, state->transitions.offset);
    return fallback->targets;
}

/*
 * A history state's default may target another history state, which is
 * then entered through its own default. That chain must end in real
 * states: SCXML 3.11 requires a target to become a legal configuration
 * once ancestors and defaults are added, and a loop never reaches an
 * atomic state. So a loop, `h` targeting `h` or `h1` and `h2` targeting
 * each other, is BAD_HISTORY_DEFAULT.
 *
 * A depth-first walk over the edges "this history's default targets that
 * history state", with an explicit stack (ADR 0060). Walks start from the
 * history states in document order and each state is finished once, so
 * the loop reported is the same every time. The error is at the target
 * that closes the loop, with the history it returns to as `related`.
 */
static ta_result ta_compiler_history_loops(ta_compiler *compiler) {
    uint32_t count = compiler->states.count;
    uint32_t histories = 0;
    for (ta_state_id state = 0; state < count; state += 1u) {
        histories += ta_state_kind_is_history(ta_compiler_state(compiler, state)->kind) ? 1u : 0u;
    }
    if (histories < 1u) {
        return TA_OK;
    }
    enum { UNSEEN = 0, ON_WALK = 1, FINISHED = 2 };
    ta_vec marks;
    ta_vec walk;
    ta_vec_init(&marks, compiler->allocator, TA_VEC_LAYOUT(uint8_t));
    ta_vec_init(&walk, compiler->allocator, TA_VEC_LAYOUT(ta_history_walk));
    uint32_t base = 0;
    ta_result result = ta_vec_extend(&marks, count, &base);
    for (ta_state_id start = 0; result == TA_OK && start < count; start += 1u) {
        uint8_t *mark = marks.items;
        if (!ta_state_kind_is_history(ta_compiler_state(compiler, start)->kind) || mark[start] != UNSEEN) {
            continue;
        }
        mark[start] = ON_WALK;
        result = ta_vec_push(&walk, &(ta_history_walk){ .state = start });
        while (result == TA_OK && walk.count != 0) {
            ta_history_walk *top = ta_vec_at(&walk, walk.count - 1u);
            ta_span32 targets = ta_compiler_history_targets(compiler, top->state);
            if (top->next == targets.length) {
                ((uint8_t *)marks.items)[top->state] = FINISHED;
                walk.count -= 1u;
                continue;
            }
            uint32_t index = top->next;
            top->next += 1u;
            ta_state_id history = top->state;
            ta_state_id target = *(ta_state_id *)ta_vec_at(&compiler->targets, targets.offset + index);
            if (!ta_state_kind_is_history(ta_compiler_state(compiler, target)->kind)) {
                continue;
            }
            uint8_t seen = ((uint8_t *)marks.items)[target];
            if (seen == ON_WALK) {
                ta_compiler_at(compiler, history);
                result = ta_compiler_enter(compiler, TA_SPEC_MEMBER_TRANSITIONS, 0);
                if (result == TA_OK) {
                    result = ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_TARGETS, index,
                                                 &(ta_compiler_fault){
                                                     .code = TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT,
                                                     .subject = ta_compiler_state_spec(compiler, history)
                                                                    ->transitions[0].targets[index],
                                                     .related = ta_compiler_state_spec(compiler, target)->id,
                                                     .message = TA_PHRASE("history defaults loop and never reach a state"),
                                                 });
                }
            } else if (seen == UNSEEN) {
                ((uint8_t *)marks.items)[target] = ON_WALK;
                result = ta_vec_push(&walk, &(ta_history_walk){ .state = target });
            }
        }
    }
    ta_vec_destroy(&walk);
    ta_vec_destroy(&marks);
    return result;
}

static ta_result ta_compiler_run(ta_compiler *compiler, ta_datamodel datamodel) {
    const ta_scxml_spec *spec = compiler->spec;
    ta_definition *definition = compiler->definition;
    if (spec->datamodel.ptr != NULL && spec->datamodel.len != 0 && !ta_str_equal(spec->datamodel, datamodel.v->name)) {
        return ta_compiler_fail_at(compiler, TA_SPEC_MEMBER_DATAMODEL, TA_SPEC_INDEX_NONE,
                                   &(ta_compiler_fault){ .code = TA_COMPILE_ERROR_DATAMODEL_MISMATCH,
                                                         .subject = spec->datamodel,
                                                         .message = TA_PHRASE("the chart requires another data model") });
    }
    ta_compiler_hash_str(compiler, spec->name);
    ta_compiler_hash_u32(compiler, spec->binding);
    ta_span32 roots = {0};
    ta_result result = ta_compiler_collect(compiler, spec->states, TA_STATE_NONE, &roots);
    result = ta_compiler_index_names(compiler, result);
    ta_compiler_at(compiler, TA_STATE_NONE);
    if (result == TA_OK && compiler->states.count == 0) {
        result = ta_compiler_fail(compiler, TA_COMPILE_ERROR_NO_STATES, (ta_str){0},
                                  TA_PHRASE("the chart has no states"));
    }
    /* Data declarations are listed root first, then by state in document order. */
    if (result == TA_OK) {
        result = ta_compiler_data(compiler, spec->data, TA_STATE_NONE);
    }
    for (ta_state_id state = 0; result == TA_OK && state < compiler->states.count; state += 1) {
        result = ta_compiler_compile_state(compiler, state);
    }
    if (result == TA_OK) {
        result = ta_compiler_history_loops(compiler);
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
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
        result = ta_compiler_expression_at(compiler, TA_SPEC_MEMBER_SCRIPT, &spec->script, &script.as.script);
        if (result == TA_OK) {
            result = ta_vec_extend(&compiler->actions, 1, &base);
        }
        if (result == TA_OK) {
            *(ta_action_def *)ta_vec_at(&compiler->actions, base) = script;
            definition->global_script_actions = (ta_span32){ .offset = base, .length = 1 };
        }
    }
    if (result == TA_OK) {
        ta_compiler_at(compiler, TA_STATE_NONE);
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

typedef struct {
    ta_compile_error *error;
    ta_arena *error_arena;
    /* UINT32_MAX for a compile; otherwise a replay that stops at this
     * expression ID and never prepares or keeps a definition. */
    uint32_t locate;
    ta_expression_rejection *rejected;
    ta_definition **out;
} ta_compile_run_params;

static ta_result ta_compile(ta_system *system, const ta_definition_compile_params *params,
                            const ta_compile_run_params *run) {
    const ta_scxml_spec *spec = params->spec;
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
        .path_state = TA_STATE_NONE,
        .error = run->error,
        .error_arena = run->error_arena,
        .locate = run->locate,
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
        { &compiler.path, TA_VEC_LAYOUT(ta_spec_step) },
        { &compiler.names, TA_VEC_LAYOUT(ta_state_key) },
        { &compiler.levels, TA_VEC_LAYOUT(ta_collect_frame) },
        { &compiler.blocks, TA_VEC_LAYOUT(ta_block_frame) },
    };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(vecs); index += 1) {
        ta_vec_init(vecs[index].vec, params->allocator, vecs[index].layout);
    }

    result = ta_compiler_run(&compiler, params->datamodel);
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && !compiler.failed) {
        /* A refusal from below the compiler's own checks. */
        ta_compiler_at(&compiler, TA_STATE_NONE);
        (void)ta_compiler_fail(&compiler, TA_COMPILE_ERROR_MALFORMED_SPEC, (ta_str){0},
                               TA_PHRASE("the spec was refused"));
    }
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

    if (run->locate != UINT32_MAX) {
        ta_definition_destroy(definition);
        return result;
    }
    if (result == TA_OK) {
        result = params->datamodel.v->prepare_definition(
            params->datamodel.self,
            &(ta_datamodel_prepare_definition_params){
                .expressions = definition->expressions,
                .expression_count = definition->expression_count,
                .allocator = params->allocator,
                .shared = &definition->datamodel_shared,
                .rejected = run->rejected,
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
        ta_actor_layout(definition);
    }
    if (result != TA_OK) {
        ta_definition_destroy(definition);
        return result;
    }
    definition->unregistered_next = system->unregistered_definitions;
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition;
    }
    system->unregistered_definitions = definition;
    *run->out = definition;
    return TA_OK;
}

/*
 * The data model refused the prepared expression table. Name the expression
 * it pointed at by replaying the walk to it: expression IDs are assigned in
 * walk order, so nothing is stored per expression on the success path.
 */
/* The longest prefix of `text` of at most `limit` bytes that does not end
 * inside a UTF-8 sequence. */
static uint32_t ta_utf8_prefix(ta_str text, uint32_t limit) {
    if (text.len <= limit) {
        return text.len;
    }
    uint32_t end = limit;
    while (end > 0 && ((unsigned char)text.ptr[end] & 0xC0u) == 0x80u) {
        end -= 1u;
    }
    return end;
}

static void ta_compile_rejected(ta_system *system, const ta_definition_compile_params *params,
                                const ta_expression_rejection *rejection) {
    ta_compile_error *error = params->error;
    /* Copied first: the message is the adapter's until core calls it again. */
    ta_str message = TA_PHRASE("the data model refused this expression");
    if (params->error_arena != NULL && rejection->message.ptr != NULL && rejection->message.len != 0) {
        ta_str copy = {0};
        ta_str kept = { .ptr = rejection->message.ptr,
                        .len = ta_utf8_prefix(rejection->message, TA_COMPILE_ERROR_MESSAGE_MAX) };
        if (ta_arena_copy_str(params->error_arena, kept, &copy) == TA_OK) {
            message = copy;
        }
    }
    if (rejection->expression != TA_EXPR_ID_NONE) {
        ta_result replayed = ta_compile(system, params, &(ta_compile_run_params){
                                                            .error = error,
                                                            .error_arena = params->error_arena,
                                                            .locate = rejection->expression,
                                                        });
        if (replayed == TA_OK) {
            *error = (ta_compile_error){0}; /* no such expression: about none */
        }
    }
    error->code = TA_COMPILE_ERROR_EXPRESSION_REJECTED;
    if (rejection->expression != TA_EXPR_ID_NONE && rejection->offset <= error->subject.len &&
        rejection->length <= error->subject.len - rejection->offset) {
        error->subject_offset = rejection->offset;
        error->subject_length = rejection->length;
    }
    error->message = message;
}

TA_API ta_result ta_definition_compile_spec(ta_system *system, const ta_definition_compile_params *params) {
    if (params != NULL && params->error != NULL) {
        *params->error = (ta_compile_error){0};
    }
    if (system == NULL || params == NULL || params->spec == NULL || params->out == NULL ||
        params->allocator.v == NULL || !ta_datamodel_complete(&params->datamodel)) {
        return TA_ERR_INVALID;
    }
    ta_expression_rejection rejection = { .expression = TA_EXPR_ID_NONE };
    ta_compile_error error = {0};
    ta_result result = ta_compile(system, params, &(ta_compile_run_params){
                                                      .error = params->error != NULL ? &error : NULL,
                                                      .error_arena = params->error_arena,
                                                      .locate = UINT32_MAX,
                                                      .rejected = &rejection,
                                                      .out = params->out,
                                                  });
    if (params->error == NULL || (result != TA_ERR_INVALID && result != TA_ERR_UNSUPPORTED)) {
        return result;
    }
    if (error.code == TA_COMPILE_ERROR_NONE) {
        /* The compiler passed the spec and the data model refused it. */
        *params->error = (ta_compile_error){0};
        ta_compile_rejected(system, params, &rejection);
        return result;
    }
    *params->error = error;
    return result;
}

/* ------------------------------------------------------------------------
 * Formatting
 */

/*
 * The formatter's output: one buffer of TA_COMPILE_ERROR_TEXT_MAX bytes,
 * allocated once. Appending past the end is cut off, which the field limits
 * below keep from happening; it is the backstop, not the rule.
 */
typedef struct {
    char *bytes;
    uint32_t length;
} ta_text_builder;

static void ta_text_append(ta_text_builder *text, ta_str piece) {
    uint32_t room = TA_COMPILE_ERROR_TEXT_MAX - text->length;
    uint32_t count = ta_utf8_prefix(piece, room);
    if (count != 0) {
        ta_mem_copy(text->bytes + text->length, piece.ptr, count);
        text->length += count;
    }
}

static void ta_text_append_u32(ta_text_builder *text, uint32_t value) {
    char digits[10];
    uint32_t count = 0;
    do {
        digits[sizeof(digits) - 1u - count] = (char)('0' + value % 10u);
        value /= 10u;
        count += 1u;
    } while (value != 0);
    ta_text_append(text, (ta_str){ .ptr = digits + sizeof(digits) - count, .len = count });
}

/* Output limits for one field, and how much of a deep path is shown. */
enum {
    TA_FORMAT_ID_MAX = 64u,
    TA_FORMAT_TEXT_MAX = 96u,
    TA_FORMAT_HEAD_STEPS = 2u,
    TA_FORMAT_TAIL_STEPS = 6u
};

/*
 * The length in bytes of the unit of `text` starting at `at`: a well-formed
 * UTF-8 sequence, or one byte that is not part of one.
 */
static uint32_t ta_utf8_unit(ta_str text, uint32_t at) {
    unsigned char lead = (unsigned char)text.ptr[at];
    uint32_t length = lead < 0xC0u ? 1u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : lead < 0xF8u ? 4u : 1u;
    if (length > text.len - at) {
        return 1u;
    }
    for (uint32_t index = 1; index < length; index += 1u) {
        if (((unsigned char)text.ptr[at + index] & 0xC0u) != 0x80u) {
            return 1u;
        }
    }
    return length;
}

/* How a unit is written: escaped when it would break the line, or, in a
 * quoted field, the quotes. */
static uint32_t ta_text_escape(ta_str text, uint32_t at, uint32_t unit, bool quoted, char out[4]) {
    unsigned char byte = (unsigned char)text.ptr[at];
    static const char hex[] = "0123456789abcdef";
    if (unit == 1u && (byte < 0x20u || byte == 0x7Fu || byte >= 0x80u)) {
        if (byte == '\n' || byte == '\t' || byte == '\r') {
            out[0] = '\\';
            out[1] = byte == '\n' ? 'n' : byte == '\t' ? 't' : 'r';
            return 2u;
        }
        out[0] = '\\';
        out[1] = 'x';
        out[2] = hex[byte >> 4];
        out[3] = hex[byte & 0xFu];
        return 4u;
    }
    if (quoted && unit == 1u && (byte == '"' || byte == '\\')) {
        out[0] = '\\';
        out[1] = (char)byte;
        return 2u;
    }
    ta_mem_copy(out, text.ptr + at, unit);
    return unit;
}

/*
 * Appends `value` escaped, in at most `limit` bytes of output. When it does
 * not fit, it is cut at a unit boundary and ends in "...". A `quoted` field
 * also escapes quotes and backslashes.
 */
static void ta_text_append_field(ta_text_builder *text, ta_str value, uint32_t limit, bool quoted) {
    uint32_t total = 0;
    char unit_text[4];
    for (uint32_t at = 0; at < value.len && total <= limit;) {
        uint32_t unit = ta_utf8_unit(value, at);
        total += ta_text_escape(value, at, unit, quoted, unit_text);
        at += unit;
    }
    uint32_t budget = total <= limit ? limit : limit - 3u;
    uint32_t written = 0;
    for (uint32_t at = 0; at < value.len;) {
        uint32_t unit = ta_utf8_unit(value, at);
        uint32_t size = ta_text_escape(value, at, unit, quoted, unit_text);
        if (written + size > budget) {
            break;
        }
        ta_text_append(text, (ta_str){ .ptr = unit_text, .len = size });
        written += size;
        at += unit;
    }
    if (total > limit) {
        ta_text_append(text, TA_PHRASE("..."));
    }
}

static ta_str ta_spec_member_name(ta_spec_member member) {
    switch (member) {
        case TA_SPEC_MEMBER_STATES: return TA_PHRASE("states");
        case TA_SPEC_MEMBER_TRANSITIONS: return TA_PHRASE("transitions");
        case TA_SPEC_MEMBER_TARGETS: return TA_PHRASE("targets");
        case TA_SPEC_MEMBER_ONENTRY: return TA_PHRASE("onentry");
        case TA_SPEC_MEMBER_ONEXIT: return TA_PHRASE("onexit");
        case TA_SPEC_MEMBER_ACTIONS: return TA_PHRASE("actions");
        case TA_SPEC_MEMBER_THEN: return TA_PHRASE("then");
        case TA_SPEC_MEMBER_ELSE: return TA_PHRASE("else");
        case TA_SPEC_MEMBER_BODY: return TA_PHRASE("body");
        case TA_SPEC_MEMBER_FINALIZE: return TA_PHRASE("finalize");
        case TA_SPEC_MEMBER_INVOKES: return TA_PHRASE("invokes");
        case TA_SPEC_MEMBER_DATA: return TA_PHRASE("data");
        case TA_SPEC_MEMBER_PARAMS: return TA_PHRASE("params");
        case TA_SPEC_MEMBER_INITIAL: return TA_PHRASE("initial");
        case TA_SPEC_MEMBER_DONE_DATA: return TA_PHRASE("done_data");
        case TA_SPEC_MEMBER_SCRIPT: return TA_PHRASE("script");
        case TA_SPEC_MEMBER_ID: return TA_PHRASE("id");
        case TA_SPEC_MEMBER_NAME: return TA_PHRASE("name");
        case TA_SPEC_MEMBER_EVENT: return TA_PHRASE("event");
        case TA_SPEC_MEMBER_CONDITION: return TA_PHRASE("condition");
        case TA_SPEC_MEMBER_TYPE: return TA_PHRASE("type");
        case TA_SPEC_MEMBER_SRC: return TA_PHRASE("src");
        case TA_SPEC_MEMBER_TARGET: return TA_PHRASE("target");
        case TA_SPEC_MEMBER_DELAY: return TA_PHRASE("delay");
        case TA_SPEC_MEMBER_DATA_VALUE: return TA_PHRASE("value");
        case TA_SPEC_MEMBER_LOCATION: return TA_PHRASE("location");
        case TA_SPEC_MEMBER_EXPR: return TA_PHRASE("expr");
        case TA_SPEC_MEMBER_ITEM: return TA_PHRASE("item");
        case TA_SPEC_MEMBER_INDEX: return TA_PHRASE("index");
        case TA_SPEC_MEMBER_SENDID: return TA_PHRASE("sendid");
        case TA_SPEC_MEMBER_DATAMODEL: return TA_PHRASE("datamodel");
        case TA_SPEC_MEMBER_IDENTITY: return TA_PHRASE("identity");
        default: return TA_PHRASE("?");
    }
}

TA_API ta_result ta_compile_error_format(const ta_compile_error_format_params *params) {
    if (params == NULL || params->error == NULL || params->arena == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_compile_error *error = params->error;
    ta_text_builder text = { .bytes = ta_arena_alloc(params->arena, TA_COMPILE_ERROR_TEXT_MAX, 1) };
    if (text.bytes == NULL) {
        return TA_ERR_NOMEM;
    }
    /*
     * A deep path shows its first and last steps. States are resolved while
     * the path goes through states only, so the walk follows every step,
     * shown or not.
     */
    const ta_scxml_state_spec *list = params->spec != NULL ? params->spec->states : NULL;
    uint32_t length = error->path_length;
    bool elide = length > TA_FORMAT_HEAD_STEPS + TA_FORMAT_TAIL_STEPS;
    for (uint32_t index = 0; index < length; index += 1u) {
        ta_spec_step step = error->path[index];
        const ta_scxml_state_spec *state = NULL;
        if (step.member == TA_SPEC_MEMBER_STATES && list != NULL) {
            state = &list[step.index];
            list = state->states;
        } else {
            list = NULL;
        }
        bool shown = !elide || index < TA_FORMAT_HEAD_STEPS || index >= length - TA_FORMAT_TAIL_STEPS;
        if (elide && index == TA_FORMAT_HEAD_STEPS) {
            ta_text_append(&text, TA_PHRASE(".["));
            ta_text_append_u32(&text, length - TA_FORMAT_HEAD_STEPS - TA_FORMAT_TAIL_STEPS);
            ta_text_append(&text, TA_PHRASE(" steps]"));
        }
        if (!shown) {
            continue;
        }
        if (index != 0) {
            ta_text_append(&text, TA_PHRASE("."));
        }
        ta_text_append(&text, ta_spec_member_name(step.member));
        if (step.index != TA_SPEC_INDEX_NONE) {
            ta_text_append(&text, TA_PHRASE("["));
            ta_text_append_u32(&text, step.index);
            ta_text_append(&text, TA_PHRASE("]"));
        }
        if (state != NULL) {
            ta_text_append(&text, TA_PHRASE("("));
            ta_text_append_field(&text, state->id, TA_FORMAT_ID_MAX, false);
            ta_text_append(&text, TA_PHRASE(")"));
        }
    }
    if (length != 0) {
        ta_text_append(&text, TA_PHRASE(": "));
    }
    ta_text_append_field(&text, error->message.len != 0 ? error->message : TA_PHRASE("the spec was refused"),
                         TA_FORMAT_TEXT_MAX, false);
    if (error->subject.len != 0) {
        ta_text_append(&text, TA_PHRASE(" \""));
        ta_text_append_field(&text, error->subject, TA_FORMAT_ID_MAX, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    if (error->related.len != 0) {
        switch (error->code) {
            case TA_COMPILE_ERROR_UNKNOWN_STATE:
                ta_text_append(&text, TA_PHRASE("; nearest is \""));
                break;
            case TA_COMPILE_ERROR_TARGET_OUTSIDE:
                ta_text_append(&text, TA_PHRASE("; it must be inside \""));
                break;
            default:
                ta_text_append(&text, TA_PHRASE("; see also \""));
                break;
        }
        ta_text_append_field(&text, error->related, TA_FORMAT_ID_MAX, true);
        ta_text_append(&text, TA_PHRASE("\""));
    }
    *params->out = (ta_str){ .ptr = text.bytes, .len = text.length };
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
    /* Action blocks still to export, each with the member that receives it
     * (ADR 0060: no recursion). At most one per block, so the capacity is
     * bounded by the definition. */
    struct ta_export_block *pending;
    uint32_t pending_count;
    uint32_t pending_capacity;
} ta_exporter;

struct ta_export_block {
    ta_span32 block;
    const ta_scxml_action_spec **out;
};

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
            /* The media type round-trips, so a decoded chart recompiles to
             * the same revision (ADR 0053). */
            out.as.content.media_type = exporter->definition->contents[source->as.content].media_type;
            out.as.content.bytes = ta_export_content(exporter, source->as.content);
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

/* Queues a nested block for ta_export_actions to fill into `*out`. */
static void ta_export_later(ta_exporter *exporter, ta_span32 block, const ta_scxml_action_spec **out) {
    *out = NULL;
    if (block.length == 0 || exporter->result != TA_OK) {
        return;
    }
    if (exporter->pending_count == exporter->pending_capacity) {
        exporter->result = TA_ERR_NOMEM; /* unreachable: the capacity counts every block */
        return;
    }
    exporter->pending[exporter->pending_count++] = (struct ta_export_block){ .block = block, .out = out };
}

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
            ta_export_later(exporter, action->as.conditional.then_actions, &out->as.conditional.then_actions);
            ta_export_later(exporter, action->as.conditional.else_actions, &out->as.conditional.else_actions);
            break;
        case TA_ACTION_FOREACH:
            out->kind = TA_SCXML_ACTION_FOREACH;
            out->as.foreach.array = ta_export_expression(exporter, action->as.foreach.array);
            out->as.foreach.item = ta_export_atom(exporter, action->as.foreach.item);
            if (action->as.foreach.has_index) {
                out->as.foreach.index = ta_export_atom(exporter, action->as.foreach.index);
            }
            ta_export_later(exporter, action->as.foreach.body_actions, &out->as.foreach.body);
            break;
        default:
            /* The compiler accepts neither <invoke> nor extensions as executable content. */
            exporter->result = exporter->result == TA_OK ? TA_ERR_UNSUPPORTED : exporter->result;
            break;
    }
}

/*
 * Exports a block and every block nested in it. A nested block is queued by
 * the action that owns it and filled in from the queue, so nothing recurses;
 * each block's output is written through the member that receives it, so the
 * order the queue is drained in does not matter.
 */
static const ta_scxml_action_spec *ta_export_actions(ta_exporter *exporter, ta_span32 block) {
    const ta_scxml_action_spec *result = NULL;
    if (exporter->pending == NULL && exporter->result == TA_OK && exporter->definition->action_count != 0) {
        /* Every block but a root block is nested in one action, and an
         * action owns at most two: capacity for all of them at once. */
        exporter->pending_capacity = 2u * exporter->definition->action_count + 1u;
        exporter->pending = ta_arena_alloc(exporter->arena,
                                           (size_t)exporter->pending_capacity * sizeof(struct ta_export_block),
                                           _Alignof(struct ta_export_block));
        if (exporter->pending == NULL) {
            exporter->result = TA_ERR_NOMEM;
        }
    }
    ta_export_later(exporter, block, &result);
    while (exporter->pending_count != 0 && exporter->result == TA_OK) {
        struct ta_export_block next = exporter->pending[--exporter->pending_count];
        ta_scxml_action_spec *items = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_action_spec), next.block.length);
        *next.out = items;
        for (uint32_t index = 0; items != NULL && index < next.block.length; index += 1) {
            ta_export_action(exporter, &exporter->definition->action_defs[next.block.offset + index], &items[index]);
        }
    }
    exporter->pending_count = 0;
    return result;
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

/* Every member of a state's spec except its child states. */
static void ta_export_state(ta_exporter *exporter, ta_state_id id, ta_scxml_state_spec *out) {
    const ta_definition *definition = exporter->definition;
    const ta_state_def *state = &definition->states[id];
    *out = (ta_scxml_state_spec){
        .kind = ta_export_state_kind(state->kind),
        .id = ta_export_state_id(exporter, id),
        .initial = state->kind == TA_STATE_COMPOUND ? ta_export_initial(exporter, state->initial) : NULL,
        .transitions = ta_export_transitions(exporter, state->transitions),
        .onentry = ta_export_handlers(exporter, state->onentry_handlers),
        .onexit = ta_export_handlers(exporter, state->onexit_handlers),
        .data = ta_export_data(exporter, id),
        .invokes = ta_export_invokes(exporter, id),
        .done_data = ta_export_done_data(exporter, state->done_data),
    };
}

/*
 * The state tree in one pass, without recursion (ADR 0060). The compiler
 * numbers states in document pre-order and records the roots first in
 * `children`, so every state is visited after its parent: the parent
 * allocates its list of child states and hands each child its slot, and the
 * child fills it when its turn comes.
 */
static const ta_scxml_state_spec *ta_export_roots(ta_exporter *exporter) {
    const ta_definition *definition = exporter->definition;
    uint32_t count = 0;
    for (ta_state_id state = 0; state < definition->state_count; state += 1) {
        count += definition->states[state].parent == TA_STATE_NONE ? 1u : 0u;
    }
    ta_scxml_state_spec *roots = ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), count);
    if (roots == NULL) {
        return NULL;
    }
    ta_scxml_state_spec **slots = ta_arena_alloc(exporter->arena,
                                                 (size_t)definition->state_count * sizeof(ta_scxml_state_spec *),
                                                 _Alignof(ta_scxml_state_spec *));
    if (slots == NULL) {
        exporter->result = TA_ERR_NOMEM;
        return NULL;
    }
    for (uint32_t index = 0; index < count; index += 1) {
        slots[definition->children[index]] = &roots[index];
    }
    for (ta_state_id id = 0; id < definition->state_count && exporter->result == TA_OK; id += 1) {
        const ta_state_def *state = &definition->states[id];
        ta_scxml_state_spec *out = slots[id];
        ta_export_state(exporter, id, out);
        ta_scxml_state_spec *children =
            ta_export_list(exporter, TA_VEC_LAYOUT(ta_scxml_state_spec), state->children.length);
        out->states = children;
        for (uint32_t index = 0; children != NULL && index < state->children.length; index += 1) {
            slots[definition->children[state->children.offset + index]] = &children[index];
        }
    }
    return roots;
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


/*
 * Import: decode, compile, register. The decode's arena holds the spec,
 * which may point into the input; compile copies what it keeps, so neither
 * outlives this call as far as the definition is concerned.
 */

static void ta_import_fail(const ta_definition_import_params *params, const ta_import_error *error) {
    if (params->error != NULL) {
        *params->error = *error;
    }
}

/* ------------------------------------------------------------------------
 * Locating a compile error in the input (ADR 0058 section 5)
 */

static bool ta_import_within(ta_bytes input, const char *ptr, uint32_t len) {
    const char *begin = (const char *)input.ptr;
    return ptr != NULL && input.ptr != NULL && ptr >= begin && (size_t)(ptr - begin) <= input.len &&
           len <= input.len - (size_t)(ptr - begin);
}

/* Lines and columns count from 1; a column counts bytes, as txml's do. */
static ta_chart_position ta_import_position(ta_bytes input, uint64_t offset) {
    ta_chart_position at = { .offset = offset, .line = 1, .column = 1 };
    for (uint64_t index = 0; index < offset && index < input.len; index += 1u) {
        if (input.ptr[index] == '\n') {
            at.line += 1u;
            at.column = 1;
        } else {
            at.column += 1u;
        }
    }
    return at;
}

typedef enum {
    TA_NODE_NONE = 0,
    TA_NODE_SPEC,
    TA_NODE_STATE,
    TA_NODE_TRANSITION,
    TA_NODE_HANDLER,
    TA_NODE_INITIAL,
    TA_NODE_ACTION,
    TA_NODE_DATA,
    TA_NODE_PARAM,
    TA_NODE_INVOKE,
    TA_NODE_DONE_DATA,
    TA_NODE_TEXT
} ta_node_kind;

/* One element of a spec reached by a path; TA_NODE_TEXT is a text member. */
typedef struct {
    ta_node_kind kind;
    const void *ptr;
    ta_str text;
} ta_node;

static ta_str ta_expression_text(const ta_expression *expression) {
    return expression->kind == TA_EXPRESSION_SOURCE ? expression->as.source : (ta_str){0};
}

static ta_str ta_value_text(const ta_scxml_value_spec *value) {
    switch (value->kind) {
        case TA_SCXML_VALUE_EXPR: return ta_expression_text(&value->as.expr);
        case TA_SCXML_VALUE_LOCATION: return ta_expression_text(&value->as.location);
        case TA_SCXML_VALUE_CONTENT:
            return (ta_str){ .ptr = (const char *)value->as.content.bytes.ptr, .len = value->as.content.bytes.len };
        default: return (ta_str){0};
    }
}

static ta_str ta_string_text(const ta_scxml_string_spec *string) {
    switch (string->kind) {
        case TA_SCXML_STRING_LITERAL: return string->as.literal;
        case TA_SCXML_STRING_EXPR: return ta_expression_text(&string->as.expr);
        default: return (ta_str){0};
    }
}

static ta_str ta_id_text(const ta_scxml_id_spec *id) {
    switch (id->kind) {
        case TA_SCXML_ID_LITERAL: return id->as.literal;
        case TA_SCXML_ID_LOCATION: return ta_expression_text(&id->as.location);
        default: return (ta_str){0};
    }
}

static ta_node ta_text_node(ta_str text) {
    return (ta_node){ .kind = TA_NODE_TEXT, .text = text };
}

static ta_node ta_list_node(ta_node_kind kind, const void *element) {
    return (ta_node){ .kind = kind, .ptr = element };
}

static ta_node ta_action_step(const ta_scxml_action_spec *action, ta_spec_step step) {
    uint32_t i = step.index;
    switch (action->kind) {
        case TA_SCXML_ACTION_RAISE:
            if (step.member == TA_SPEC_MEMBER_EVENT) return ta_text_node(action->as.raise.event);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.raise.data));
            return (ta_node){0};
        case TA_SCXML_ACTION_SEND: {
            const ta_scxml_send_spec *send = &action->as.send;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &send->params[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&send->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&send->type));
                case TA_SPEC_MEMBER_EVENT:
                    if (send->event.kind == TA_SCXML_EVENT_NAME) return ta_text_node(send->event.as.name);
                    if (send->event.kind == TA_SCXML_EVENT_EXPR) return ta_text_node(ta_expression_text(&send->event.as.expr));
                    return (ta_node){0};
                case TA_SPEC_MEMBER_TARGET: return ta_text_node(ta_string_text(&send->target));
                case TA_SPEC_MEMBER_DELAY: return ta_text_node(ta_string_text(&send->delay));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&send->data));
                default: return (ta_node){0};
            }
        }
        case TA_SCXML_ACTION_CANCEL:
            if (step.member == TA_SPEC_MEMBER_SENDID) return ta_text_node(ta_string_text(&action->as.cancel.sendid));
            return (ta_node){0};
        case TA_SCXML_ACTION_ASSIGN:
            if (step.member == TA_SPEC_MEMBER_LOCATION) return ta_text_node(ta_expression_text(&action->as.assign.location));
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&action->as.assign.value));
            return (ta_node){0};
        case TA_SCXML_ACTION_SCRIPT:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.script.script));
            return (ta_node){0};
        case TA_SCXML_ACTION_LOG:
            if (step.member == TA_SPEC_MEMBER_EXPR) return ta_text_node(ta_expression_text(&action->as.log.expr));
            return (ta_node){0};
        case TA_SCXML_ACTION_IF:
            switch (step.member) {
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&action->as.conditional.condition));
                case TA_SPEC_MEMBER_THEN: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.then_actions[i]);
                case TA_SPEC_MEMBER_ELSE: return ta_list_node(TA_NODE_ACTION, &action->as.conditional.else_actions[i]);
                default: return (ta_node){0};
            }
        case TA_SCXML_ACTION_FOREACH:
            switch (step.member) {
                case TA_SPEC_MEMBER_EXPR: return ta_text_node(ta_expression_text(&action->as.foreach.array));
                case TA_SPEC_MEMBER_ITEM: return ta_text_node(action->as.foreach.item);
                case TA_SPEC_MEMBER_INDEX: return ta_text_node(action->as.foreach.index);
                case TA_SPEC_MEMBER_BODY: return ta_list_node(TA_NODE_ACTION, &action->as.foreach.body[i]);
                default: return (ta_node){0};
            }
        default:
            return (ta_node){0};
    }
}

/* Follows one step of a compile error's path. */
static ta_node ta_node_step(ta_node node, ta_spec_step step) {
    uint32_t i = step.index;
    switch (node.kind) {
        case TA_NODE_SPEC: {
            const ta_scxml_spec *spec = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &spec->states[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &spec->data[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, spec->initial);
                case TA_SPEC_MEMBER_SCRIPT: return ta_text_node(ta_expression_text(&spec->script));
                case TA_SPEC_MEMBER_NAME: return ta_text_node(spec->name);
                case TA_SPEC_MEMBER_DATAMODEL: return ta_text_node(spec->datamodel);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_STATE: {
            const ta_scxml_state_spec *state = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_STATES: return ta_list_node(TA_NODE_STATE, &state->states[i]);
                case TA_SPEC_MEMBER_TRANSITIONS: return ta_list_node(TA_NODE_TRANSITION, &state->transitions[i]);
                case TA_SPEC_MEMBER_ONENTRY: return ta_list_node(TA_NODE_HANDLER, &state->onentry[i]);
                case TA_SPEC_MEMBER_ONEXIT: return ta_list_node(TA_NODE_HANDLER, &state->onexit[i]);
                case TA_SPEC_MEMBER_DATA: return ta_list_node(TA_NODE_DATA, &state->data[i]);
                case TA_SPEC_MEMBER_INVOKES: return ta_list_node(TA_NODE_INVOKE, &state->invokes[i]);
                case TA_SPEC_MEMBER_INITIAL: return ta_list_node(TA_NODE_INITIAL, state->initial);
                case TA_SPEC_MEMBER_DONE_DATA: return ta_list_node(TA_NODE_DONE_DATA, state->done_data);
                case TA_SPEC_MEMBER_ID: return ta_text_node(state->id);
                default: return (ta_node){0};
            }
        }
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_TARGETS: return ta_text_node(transition->targets[i]);
                case TA_SPEC_MEMBER_ACTIONS: return ta_list_node(TA_NODE_ACTION, &transition->actions[i]);
                case TA_SPEC_MEMBER_EVENT: return ta_text_node(transition->event);
                case TA_SPEC_MEMBER_CONDITION: return ta_text_node(ta_expression_text(&transition->condition));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_HANDLER: {
            const ta_scxml_handler_spec *handler = node.ptr;
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &handler->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            if (step.member == TA_SPEC_MEMBER_TARGETS) return ta_text_node(initial->targets[i]);
            if (step.member == TA_SPEC_MEMBER_ACTIONS) return ta_list_node(TA_NODE_ACTION, &initial->actions[i]);
            return (ta_node){0};
        }
        case TA_NODE_DATA: {
            const ta_scxml_data_spec *data = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(data->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&data->initializer));
            return (ta_node){0};
        }
        case TA_NODE_PARAM: {
            const ta_scxml_param_spec *param = node.ptr;
            if (step.member == TA_SPEC_MEMBER_NAME) return ta_text_node(param->name);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&param->value));
            return (ta_node){0};
        }
        case TA_NODE_DONE_DATA: {
            const ta_scxml_done_data_spec *done = node.ptr;
            if (step.member == TA_SPEC_MEMBER_PARAMS) return ta_list_node(TA_NODE_PARAM, &done->params[i]);
            if (step.member == TA_SPEC_MEMBER_DATA_VALUE) return ta_text_node(ta_value_text(&done->content));
            return (ta_node){0};
        }
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            switch (step.member) {
                case TA_SPEC_MEMBER_PARAMS: return ta_list_node(TA_NODE_PARAM, &invoke->params[i]);
                case TA_SPEC_MEMBER_FINALIZE: return ta_list_node(TA_NODE_ACTION, &invoke->finalize[i]);
                case TA_SPEC_MEMBER_ID: return ta_text_node(ta_id_text(&invoke->id));
                case TA_SPEC_MEMBER_TYPE: return ta_text_node(ta_string_text(&invoke->type));
                case TA_SPEC_MEMBER_SRC: return ta_text_node(ta_string_text(&invoke->src));
                case TA_SPEC_MEMBER_DATA_VALUE: return ta_text_node(ta_value_text(&invoke->data));
                default: return (ta_node){0};
            }
        }
        case TA_NODE_ACTION:
            return ta_action_step(node.ptr, step);
        default:
            return (ta_node){0};
    }
}

/* The text that stands for a node, if any of it lies within `input`. */
static ta_str ta_node_anchor(ta_node node, ta_bytes input) {
    ta_str candidates[3] = { {0}, {0}, {0} };
    switch (node.kind) {
        case TA_NODE_TEXT: candidates[0] = node.text; break;
        case TA_NODE_SPEC: candidates[0] = ((const ta_scxml_spec *)node.ptr)->name; break;
        case TA_NODE_STATE: candidates[0] = ((const ta_scxml_state_spec *)node.ptr)->id; break;
        case TA_NODE_TRANSITION: {
            const ta_scxml_transition_spec *transition = node.ptr;
            candidates[0] = transition->event;
            candidates[1] = ta_expression_text(&transition->condition);
            candidates[2] = transition->targets != NULL ? transition->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_INITIAL: {
            const ta_scxml_initial_spec *initial = node.ptr;
            candidates[0] = initial->targets != NULL ? initial->targets[0] : (ta_str){0};
            break;
        }
        case TA_NODE_DATA: candidates[0] = ((const ta_scxml_data_spec *)node.ptr)->name; break;
        case TA_NODE_PARAM: candidates[0] = ((const ta_scxml_param_spec *)node.ptr)->name; break;
        case TA_NODE_INVOKE: {
            const ta_scxml_invoke_spec *invoke = node.ptr;
            candidates[0] = ta_id_text(&invoke->id);
            candidates[1] = ta_string_text(&invoke->type);
            candidates[2] = ta_string_text(&invoke->src);
            break;
        }
        case TA_NODE_ACTION: {
            /* The first text member the action has. */
            static const ta_spec_member members[] = {
                TA_SPEC_MEMBER_EVENT, TA_SPEC_MEMBER_LOCATION, TA_SPEC_MEMBER_EXPR, TA_SPEC_MEMBER_CONDITION,
                TA_SPEC_MEMBER_SENDID, TA_SPEC_MEMBER_TARGET, TA_SPEC_MEMBER_TYPE, TA_SPEC_MEMBER_ID,
            };
            for (uint32_t index = 0; index < sizeof(members) / sizeof(members[0]); index += 1u) {
                ta_node text = ta_action_step(node.ptr, (ta_spec_step){ .member = members[index],
                                                                        .index = TA_SPEC_INDEX_NONE });
                if (text.kind == TA_NODE_TEXT && ta_import_within(input, text.text.ptr, text.text.len)) {
                    return text.text;
                }
            }
            break;
        }
        default: break;
    }
    for (uint32_t index = 0; index < 3u; index += 1u) {
        if (ta_import_within(input, candidates[index].ptr, candidates[index].len)) {
            return candidates[index];
        }
    }
    return (ta_str){0};
}

/*
 * Where in `input` a compile error is: its subject, if the subject is a
 * slice of the input; otherwise the deepest element on its path whose text
 * is. False when neither. Error path only.
 */
static bool ta_import_locate(const ta_scxml_spec *spec, const ta_compile_error *error, ta_bytes input,
                             uint64_t *offset) {
    const char *begin = (const char *)input.ptr;
    if (error->subject.len != 0 && ta_import_within(input, error->subject.ptr, error->subject.len)) {
        *offset = (uint64_t)(error->subject.ptr - begin) + error->subject_offset;
        return true;
    }
    for (uint32_t depth = error->path_length + 1u; depth > 0; depth -= 1u) {
        ta_node node = { .kind = TA_NODE_SPEC, .ptr = spec };
        for (uint32_t step = 0; step + 1u < depth && node.kind != TA_NODE_NONE; step += 1u) {
            node = ta_node_step(node, error->path[step]);
            if (node.kind != TA_NODE_TEXT && node.ptr == NULL) {
                node.kind = TA_NODE_NONE;
            }
        }
        if (node.kind == TA_NODE_NONE) {
            continue;
        }
        ta_str anchor = ta_node_anchor(node, input);
        if (anchor.ptr != NULL) {
            *offset = (uint64_t)(anchor.ptr - begin);
            return true;
        }
    }
    return false;
}

TA_API ta_result ta_definition_import(ta_system *system, const ta_definition_import_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL || (params->input.ptr == NULL && params->input.len != 0)) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_import_error){0};
    }
    if (params->codec.v->decode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }

    const ta_scxml_spec *decoded = NULL;
    ta_chart_error decode_error = {0};
    ta_result result = params->codec.v->decode(params->codec.self,
                                               &(ta_chart_decode_params){
                                                   .input = params->input,
                                                   .arena = params->arena,
                                                   .resolver = params->resolver,
                                                   .lowering = params->lowering,
                                                   .identity = params->identity,
                                                   .out = &decoded,
                                                   .error = &decode_error,
                                               });
    if (result == TA_ERR_INVALID) {
        ta_import_fail(params, &(ta_import_error){ .stage = TA_IMPORT_STAGE_DECODE,
                                                   .as.decode = decode_error,
                                                   .located = true,
                                                   .at = decode_error.at });
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    if (decoded == NULL) {
        return TA_ERR_INVALID; /* a codec that reports success must produce a spec */
    }

    /* No chart syntax carries an identity, so the caller's is the chart's,
     * whether or not the codec copied it across. */
    ta_scxml_spec *spec = ta_arena_alloc(params->arena, sizeof(ta_scxml_spec), _Alignof(ta_scxml_spec));
    if (spec == NULL) {
        return TA_ERR_NOMEM;
    }
    *spec = *decoded;
    spec->identity = params->identity;

    ta_definition *definition = NULL;
    ta_compile_error compile_error = {0};
    result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = params->datamodel,
                                                    .allocator = params->allocator,
                                                    .out = &definition,
                                                    .error = params->error != NULL ? &compile_error : NULL,
                                                    .error_arena = params->arena,
                                                });
    if ((result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) && params->error != NULL) {
        ta_import_error error = { .stage = TA_IMPORT_STAGE_COMPILE, .as.compile = compile_error };
        uint64_t offset = 0;
        if (ta_import_locate(spec, &compile_error, params->input, &offset)) {
            error.located = true;
            error.at = ta_import_position(params->input, offset);
        }
        ta_import_fail(params, &error);
        return result;
    }
    if (result == TA_ERR_INVALID || result == TA_ERR_UNSUPPORTED) {
        return result;
    }
    if (result != TA_OK) {
        return result;
    }
    result = ta_system_register_definition(system, definition, params->out);
    if (result != TA_OK) {
        /* Registration takes ownership only on success. */
        ta_definition_destroy(definition);
    }
    return result;
}

/*
 * Encode: export, then the codec's encode. The identity no syntax carries is
 * handed back beside the bytes.
 */
TA_API ta_result ta_definition_encode(const ta_system *system, const ta_definition_encode_params *params) {
    if (system == NULL || params == NULL || params->codec.v == NULL || params->arena == NULL ||
        params->out == NULL) {
        return TA_ERR_INVALID;
    }
    if (params->error != NULL) {
        *params->error = (ta_chart_error){0};
    }
    if (params->codec.v->encode == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_definition_export(system, &(ta_definition_export_params){
                                                        .definition = params->definition,
                                                        .arena = params->arena,
                                                        .out = &spec,
                                                    });
    if (result != TA_OK) {
        return result;
    }
    ta_chart_error encode_error = {0};
    result = params->codec.v->encode(params->codec.self, &(ta_chart_encode_params){
                                                             .spec = spec,
                                                             .arena = params->arena,
                                                             .expressions = params->expressions,
                                                             .out = params->out,
                                                             .error = &encode_error,
                                                         });
    if (result == TA_ERR_INVALID && params->error != NULL) {
        *params->error = encode_error;
    }
    if (result == TA_OK && params->identity != NULL) {
        *params->identity = spec->identity;
    }
    return result;
}


TA_PRIVATE ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id) {
    if (system == NULL) {
        return NULL;
    }
    uint32_t slot = ta_actor_id_slot(id);
    uint32_t generation = ta_actor_id_generation(id);
    const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    return entry != NULL && entry->generation == generation ? entry->actor : NULL;
}

TA_PRIVATE ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot) {
    if (slot >= system->actors.count) {
        return NULL;
    }
    ta_actor_slot *const *pages = system->actors.pages.items;
    return &pages[slot / TA_ACTOR_PAGE_SLOTS][slot % TA_ACTOR_PAGE_SLOTS];
}

TA_PRIVATE void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator) {
    ta_actor_slot **pages = table->pages.items;
    for (uint32_t page = 0; page < table->pages.count; page += 1u) {
        ta_mem_free(allocator, pages[page],
                    &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                 .alignment = _Alignof(ta_actor_slot) });
    }
    ta_vec_destroy(&table->pages);
    table->count = 0;
}

TA_PRIVATE void ta_actor_layout(ta_definition *definition) {
    size_t instance_alignment = definition->datamodel_instance_alignment;
    size_t alignment = _Alignof(ta_actor);
    if (alignment < _Alignof(uint64_t)) {
        alignment = _Alignof(uint64_t);
    }
    if (alignment < instance_alignment) {
        alignment = instance_alignment;
    }
    definition->actor_session_offset = ta_align_up(sizeof(ta_actor), _Alignof(uint64_t));
    definition->actor_instance_offset =
        ta_align_up(definition->actor_session_offset + ta_definition_session_bytes(definition), instance_alignment);
    definition->actor_block_bytes = definition->actor_instance_offset + definition->datamodel_instance_size;
    definition->actor_block_alignment = alignment;
}

/*
 * Issues the next slot, allocating a page when the last one is full. Pages
 * never move, so this never copies the table (ADR 0063).
 */
static ta_result ta_actor_table_grow(ta_system *system, uint32_t *out) {
    ta_actor_table *table = &system->actors;
    if (table->count == UINT32_MAX) {
        return TA_ERR_NOMEM; /* UINT32_MAX is no slot: it ends the free list and a walk */
    }
    if ((uint64_t)table->count == (uint64_t)table->pages.count * TA_ACTOR_PAGE_SLOTS) {
        ta_actor_slot *page = ta_mem_alloc(system->allocator, TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                           _Alignof(ta_actor_slot));
        if (page == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_result result = ta_vec_push(&table->pages, &page);
        if (result != TA_OK) {
            ta_mem_free(system->allocator, page,
                        &(ta_allocator_free_params){ .size = TA_ACTOR_PAGE_SLOTS * sizeof(ta_actor_slot),
                                                     .alignment = _Alignof(ta_actor_slot) });
            return result;
        }
    }
    *out = table->count;
    table->count += 1u;
    *ta_actor_slot_at(system, *out) = (ta_actor_slot){ .generation = 1, .next_free = UINT32_MAX };
    return TA_OK;
}

TA_API uint32_t ta_actor_id_slot(ta_actor_id id) {
    return (uint32_t)(id & 0xffffffffu);
}

TA_API uint32_t ta_actor_id_generation(ta_actor_id id) {
    return (uint32_t)(id >> 32);
}

TA_API ta_actor_id ta_actor_id_make(uint32_t slot, uint32_t generation) {
    return ((ta_actor_id)generation << 32) | slot;
}

TA_PRIVATE ta_timer_list *ta_actor_timers(const ta_actor *actor) {
    return actor->sparse == NULL ? NULL : &actor->sparse->timers;
}

TA_PRIVATE ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    return ta_timer_schedule(&system->timers, &sparse->timers, send);
}

TA_PRIVATE ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out) {
    ta_actor *actor = ta_actor_lookup(system, owner);
    if (actor == NULL) {
        *out = &system->timers.orphans;
        return TA_OK;
    }
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    *out = &sparse->timers;
    return TA_OK;
}

static void ta_activation_effect_release(ta_activation_effect *effect) {
    if (effect->kind == TA_ACTIVATION_SEND) {
        ta_value_release(&effect->as.send.send.target);
        ta_value_release(&effect->as.send.send.data);
        ta_actor_ref_release(&effect->as.send.send.destination);
        ta_actor_ref_release(&effect->as.send.send.origin);
    } else if (effect->kind == TA_ACTIVATION_LOG) {
        ta_value_release(&effect->as.log.value);
    }
}

static void ta_actor_discard_activation_effects(ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect_release(ta_vec_at(&effects->entries, index));
    }
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

static ta_result ta_actor_hold_effect(ta_system *system, ta_actor *actor, const ta_activation_effect *effect) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    if (sparse->activation_effects == NULL) {
        sparse->activation_effects = ta_mem_alloc(system->allocator, sizeof(ta_activation_effects),
                                                  _Alignof(ta_activation_effects));
        if (sparse->activation_effects == NULL) {
            return TA_ERR_NOMEM;
        }
        *sparse->activation_effects = (ta_activation_effects){0};
        ta_vec_init(&sparse->activation_effects->entries, system->allocator, TA_VEC_LAYOUT(ta_activation_effect));
    }
    return ta_vec_push(&sparse->activation_effects->entries, effect);
}

TA_PRIVATE ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send) {
    return ta_actor_hold_effect(system, actor,
                                &(ta_activation_effect){ .kind = TA_ACTIVATION_SEND, .as.send = *send });
}

TA_PRIVATE ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record) {
    ta_activation_effect effect = { .kind = TA_ACTIVATION_LOG, .as.log = *record };
    ta_value_retain(&effect.as.log.value);
    ta_result result = ta_actor_hold_effect(system, actor, &effect);
    if (result != TA_OK) {
        ta_value_release(&effect.as.log.value);
    }
    return result;
}

TA_PRIVATE bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return false;
    }
    ta_vec *entries = &sparse->activation_effects->entries;
    bool found = false;
    for (uint32_t index = entries->count; index > 0; index -= 1u) {
        ta_activation_effect *effect = ta_vec_at(entries, index - 1u);
        if (effect->kind != TA_ACTIVATION_SEND || effect->as.send.send.send_id != send_id) {
            continue;
        }
        ta_activation_effect_release(effect);
        for (uint32_t at = index; at < entries->count; at += 1u) {
            *(ta_activation_effect *)ta_vec_at(entries, at - 1u) = *(ta_activation_effect *)ta_vec_at(entries, at);
        }
        entries->count -= 1u;
        found = true;
    }
    return found;
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

TA_PRIVATE void ta_actor_exit_invoked(ta_system *system, ta_actor_id id) {
    ta_interpreter_exit_invoked(system, id);
}

/*
 * Takes the head of the free list, or grows the table when it is empty.
 * Scanning the table for a hole instead would make every spawn cost a walk
 * over the live population, which is what invariant 17 forbids: an ephemeral
 * child of a four-million-actor system must not pay for its siblings.
 */
static ta_result ta_actor_claim_slot(ta_system *system, ta_actor *actor, ta_actor_id *out) {
    uint32_t slot = system->free_actor_slot;
    if (slot == UINT32_MAX) {
        ta_result result = ta_actor_table_grow(system, &slot);
        if (result != TA_OK) {
            return result;
        }
    } else {
        system->free_actor_slot = ta_actor_slot_at(system, slot)->next_free;
    }
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = actor;
    entry->next_free = UINT32_MAX;
    *out = ta_actor_id_make(slot, entry->generation);
    return TA_OK;
}

/*
 * Tears the actor down completely, then uncounts it. The last actor of a
 * retired definition frees the definition, so nothing below may touch it.
 */
TA_PRIVATE void ta_actor_free(ta_system *system, ta_actor *actor) {
    const ta_definition *definition = actor->definition;
    ta_actor_discard_activation_effects(actor);
    if (actor->datamodel_initialized) {
        definition->datamodel.v->destroy_instance(definition->datamodel.self, definition->datamodel_shared,
                                                  actor->datamodel_instance);
    }
    /* A named actor holds a reference to its address; a runtime ref holds nothing. */
    ta_actor_ref_release(&actor->record.self);
    ta_interpreter_release_actor_state(system, actor);
    /* The record, the session storage and the instance are one block (ADR 0063). */
    ta_mem_free(system->allocator, actor,
                &(ta_allocator_free_params){ .size = definition->actor_block_bytes,
                                             .alignment = definition->actor_block_alignment });
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
    ta_actor_ref self = { .kind = TA_ACTOR_REF_NAMED };
    ta_result result = ta_address_intern(system, address, &self.as.named);
    if (result != TA_OK) {
        return result;
    }
    result = system->directory.v->claim(
        system->directory.self,
        &(ta_actor_directory_claim_params){
            .address = self.as.named,
            .entry = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
    if (result != TA_OK) {
        ta_actor_ref_release(&self);
        return result;
    }
    /* From here the actor answers to its address: every origin core writes
     * for it is the address, so a reply outlives this runtime ID. */
    actor->record.self = self;
    return TA_OK;
}

/*
 * Frees the slot an actor was given, so a failed spawn reuses it. The slot's
 * next occupant gets a new generation, so this ID goes stale. Generation 0
 * is never issued (ADR 0059): a wrap from UINT32_MAX goes to 1.
 */
TA_PRIVATE void ta_actor_release_slot(ta_system *system, ta_actor_id id) {
    uint32_t slot = ta_actor_id_slot(id);
    ta_actor_slot *entry = ta_actor_slot_at(system, slot);
    entry->actor = NULL;
    entry->generation = entry->generation == UINT32_MAX ? 1u : entry->generation + 1u;
    entry->next_free = system->free_actor_slot;
    system->free_actor_slot = slot;
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
    if (system->directory.v->resolve != NULL) {
        ta_actor_directory_entry current = {0};
        ta_result result = system->directory.v->resolve(
            system->directory.self,
            &(ta_actor_directory_resolve_params){ .address = actor->record.self.as.named, .out = &current });
        if (result == TA_OK &&
            (current.kind != TA_ACTOR_DIRECTORY_RESIDENT || current.actor != actor->record.id)) {
            return;
        }
    }
    (void)system->directory.v->remove(
        system->directory.self,
        &(ta_actor_directory_remove_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
        });
}

/*
 * The host directory's compare-and-swap. Every residency change is one of
 * these: the caller states the entry it believes is there, and a directory
 * that disagrees answers TA_ERR_STALE and changes nothing. That is what
 * keeps a residency change retryable — a refused swap has done nothing, so
 * the caller may simply try again (ADR 0014).
 */
TA_PRIVATE ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params) {
    if (system->directory.v == NULL || system->directory.v->replace == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->replace(system->directory.self, params);
}

/* The same comparison, for an entry that should end up gone. */
TA_PRIVATE ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params) {
    if (system->directory.v == NULL || system->directory.v->remove == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return system->directory.v->remove(system->directory.self, params);
}

/*
 * Releases the name of an actor that is not resident. The comparison is
 * against UNLOADED, so this cannot remove a name some live actor answers to:
 * a resident entry, or none at all, is TA_ERR_STALE. Core owns nothing of
 * the actor at this point — the host deletes its checkpoint afterwards.
 */
TA_API ta_result ta_actor_remove_unloaded(ta_system *system, const ta_actor_remove_unloaded_params *params) {
    if (system == NULL || params == NULL || params->address == NULL) {
        return TA_ERR_INVALID;
    }
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    result = ta_actor_directory_drop(
        system,
        &(ta_actor_directory_remove_params){
            .address = address,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    ta_address_release(address);
    return result;
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
    ta_address *address = NULL;
    ta_result result = ta_address_intern(system, params->address, &address);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_directory_entry entry = { .kind = TA_ACTOR_DIRECTORY_NONE };
    result = system->directory.v->resolve(
        system->directory.self,
        &(ta_actor_directory_resolve_params){ .address = address, .out = &entry });
    ta_address_release(address);
    if (result != TA_OK) {
        return result;
    }
    if (entry.kind == TA_ACTOR_DIRECTORY_RESIDENT) {
        *params->out = entry.actor;
        return TA_OK;
    }
    return entry.kind == TA_ACTOR_DIRECTORY_UNLOADED ? TA_ERR_UNLOADED : TA_ERR_NOT_FOUND;
}

/*
 * A blank actor of `definition`: session storage zeroed, a data-model
 * instance allocated but not initialized, a slot claimed, and a record that
 * answers to its runtime ID. Spawn bootstraps it; reconstruction fills it
 * from an image instead, so both start here.
 */
TA_PRIVATE ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out) {
    /* One allocation: the record, the session storage, the instance (ADR 0063). */
    ta_actor *actor = ta_mem_alloc(system->allocator, definition->actor_block_bytes, definition->actor_block_alignment);
    if (actor == NULL) {
        return TA_ERR_NOMEM;
    }
    unsigned char *block = (unsigned char *)actor;
    *actor = (ta_actor){ .definition = definition, .published = true, .macrostep = true };
    /* Counted from here so every ta_actor_free() path can uncount it. */
    definition->actor_count += 1u;
    actor->host_impl = (ta_dm_host_impl){ .system = system, .actor = actor };
    actor->host = (ta_dm_host){ .impl = &actor->host_impl };

    size_t config_bytes = definition->configuration.bytes;
    actor->session_bytes = ta_definition_session_bytes(definition);
    actor->session_storage = block + definition->actor_session_offset;
    ta_mem_zero(actor->session_storage, actor->session_bytes);
    unsigned char *storage = actor->session_storage;
    actor->configuration = (ta_configuration){ .storage = storage, .layout = &definition->configuration };
    actor->late_initialized = (uint64_t *)(storage + config_bytes);
    actor->history = (ta_history_state){ .storage = storage + config_bytes * 2u, .layout = &definition->history };

    /* An instance of size 0 (the null data model) points at the block's end. */
    actor->datamodel_instance = block + definition->actor_instance_offset;
    ta_mem_zero(actor->datamodel_instance, definition->datamodel_instance_size);

    ta_actor_id id = 0;
    ta_result result = ta_actor_claim_slot(system, actor, &id);
    if (result != TA_OK) {
        ta_actor_free(system, actor);
        return result;
    }
    actor->record = (ta_actor_record){
        .id = id,
        .definition = definition->id,
        .self = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = id },
        .residency = TA_ACTOR_RUNNING,
    };
    *out = actor;
    return TA_OK;
}

TA_API ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    ta_actor *actor = NULL;
    ta_result result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;

    if (params->parent != NULL) {
        actor->sparse = ta_mem_alloc(system->allocator, sizeof(ta_actor_sparse_state),
                                     _Alignof(ta_actor_sparse_state));
        if (actor->sparse == NULL) {
            ta_actor_release_slot(system, id);
            ta_actor_free(system, actor);
            return TA_ERR_NOMEM;
        }
        *actor->sparse = (ta_actor_sparse_state){ .parent = *params->parent, .has_parent = true };
    }

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

TA_API ta_result ta_actor_prepare_replacement(ta_system *system, const ta_actor_prepare_replacement_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
    ta_definition *definition = ta_system_definition(system, params->definition);
    if (incumbent == NULL || definition == NULL || definition->retired) {
        return TA_ERR_NOT_FOUND;
    }
    if (!incumbent->published || incumbent->record.self.kind != TA_ACTOR_REF_NAMED ||
        !ta_str_equal(ta_address_view(incumbent->record.self.as.named).family.name,
                      definition->identity.family.name)) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = NULL;
    ta_result result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;
    actor->published = false;
    actor->replacement_of = incumbent->record.id;
    actor->record.self = incumbent->record.self;
    ta_actor_ref_retain(&actor->record.self);
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = params->datamodel_binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result != TA_OK) {
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor);
        return result;
    }
    *params->out = id;
    return TA_OK;
}

static uint32_t ta_actor_activation_send_count(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return 0;
    }
    uint32_t count = 0;
    const ta_vec *entries = &sparse->activation_effects->entries;
    for (uint32_t index = 0; index < entries->count; index += 1u) {
        const ta_activation_effect *effect = ta_vec_at(entries, index);
        count += effect->kind == TA_ACTIVATION_SEND ? 1u : 0u;
    }
    return count;
}

static void ta_actor_release_activation_effects(ta_system *system, ta_actor *actor) {
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse == NULL || sparse->activation_effects == NULL) {
        return;
    }
    ta_activation_effects *effects = sparse->activation_effects;
    for (uint32_t index = 0; index < effects->entries.count; index += 1u) {
        ta_activation_effect *effect = ta_vec_at(&effects->entries, index);
        if (effect->kind == TA_ACTIVATION_LOG) {
            if (system->log_sink.v != NULL) {
                system->log_sink.v->write(system->log_sink.self, &effect->as.log);
            }
            ta_value_release(&effect->as.log.value);
            continue;
        }
        ta_delayed_send *send = &effect->as.send.send;
        if (effect->as.send.immediate) {
            ta_external_event_submit_status status = ta_interpreter_dispatch_delayed(system, send);
            if (status != TA_EXTERNAL_EVENT_RETRY) {
                continue;
            }
            /* Dispatch marked it submitted and took its origin and invoke ID. */
        }
        /* Reserved by ta_actor_activate_replacement(), so this cannot fail. */
        (void)ta_timer_schedule(&system->timers, &sparse->timers, send);
    }
    ta_allocator allocator = effects->entries.allocator;
    ta_vec_destroy(&effects->entries);
    ta_mem_free(allocator, effects,
                &(ta_allocator_free_params){ .size = sizeof(ta_activation_effects),
                                             .alignment = _Alignof(ta_activation_effects) });
    sparse->activation_effects = NULL;
}

TA_API ta_result ta_actor_activate_replacement(ta_system *system, const ta_actor_activate_replacement_params *params) {
    if (system == NULL || params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *incumbent = ta_actor_lookup(system, params->incumbent);
    ta_actor *replacement = ta_actor_lookup(system, params->replacement);
    if (incumbent == NULL || replacement == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    if (!incumbent->published || replacement->published || replacement->replacement_of != incumbent->record.id ||
        incumbent->record.self.kind != TA_ACTOR_REF_NAMED || replacement->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    if (replacement->macrostep || replacement->record.residency != TA_ACTOR_IDLE) {
        return TA_ERR_BUSY;
    }
    uint32_t sends = ta_actor_activation_send_count(replacement);
    ta_result result = sends == 0 ? TA_OK : ta_timer_reserve(&system->timers, &replacement->sparse->timers, sends);
    if (result != TA_OK) {
        return result;
    }
    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = replacement->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = incumbent->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = replacement->record.id },
        });
    if (result != TA_OK) {
        return result;
    }
    replacement->published = true;
    replacement->replacement_of = 0;
    ta_actor_touch(replacement);
    ta_actor_release_activation_effects(system, replacement);
    if (replacement->sparse != NULL && replacement->sparse->pending_invokes.count != 0) {
        replacement->macrostep = true;
        replacement->record.residency = TA_ACTOR_RUNNING;
    }
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
    /* Before busy: a finished actor never becomes free again (ADR 0055). */
    if (actor->finished) {
        return TA_ERR_DONE;
    }
    if (actor->macrostep || actor->has_external_event) {
        return TA_ERR_BUSY;
    }
    ta_event copy;
    ta_event_retain(event, &copy);
    actor->external_event = copy;
    actor->has_external_event = true;
    ta_event_retain(event, &actor->macrostep_event);
    actor->has_macrostep_event = true;
    actor->macrostep = true;
    ta_actor_touch(actor);
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
    ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL && sparse->completion_pending && sparse->has_parent) {
        ta_actor_ref parent = { .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = sparse->parent.actor };
        if (ta_invoke_parent_accepts(system, actor, &parent)) {
            return TA_ERR_BUSY;
        }
    }
    /* Sends already submitted stay queued for their receivers (ADR 0054). */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_actor_release_address(system, actor);
    ta_actor_release_slot(system, id);
    ta_actor_free(system, actor);
    return TA_OK;
}

static const ta_event *ta_actor_paused_event(const ta_actor *actor) {
    if (!actor->macrostep) {
        return NULL;
    }
    if (actor->has_macrostep_event) {
        return &actor->macrostep_event;
    }
    return NULL;
}

static size_t ta_vec_bytes(const ta_vec *vec) {
    return (size_t)vec->capacity * vec->element_size;
}

/* Everything the core allocates for the actor now; see ta_actor_inspection. */
static size_t ta_actor_allocated_bytes(const ta_actor *actor) {
    size_t total = actor->definition->actor_block_bytes;
    if (actor->internal_queue != NULL) {
        total += ta_internal_queue_bytes(actor->internal_queue->capacity);
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    if (sparse != NULL) {
        total += sizeof(ta_actor_sparse_state);
        total += ta_vec_bytes(&sparse->pending_invokes);
        total += (size_t)sparse->invoke_capacity * (sizeof(ta_active_invoke) + sizeof(ta_invoke_meta));
        if (sparse->activation_effects != NULL) {
            total += sizeof(ta_activation_effects);
            total += ta_vec_bytes(&sparse->activation_effects->entries);
        }
        total += ta_timer_list_bytes(&sparse->timers);
        if (sparse->paused != NULL) {
            total += sparse->paused->bytes;
        }
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
        .microstep_in_progress = actor->sparse != NULL && actor->sparse->paused != NULL,
        .current_event = ta_actor_paused_event(actor),
        .allocated_bytes = ta_actor_allocated_bytes(actor),
        .final_state = TA_STATE_NONE,
    };
    if (actor->internal_queue != NULL && actor->internal_queue->count != 0) {
        out->internal_events = actor->internal_queue->events;
        out->internal_count = actor->internal_queue->count;
    }
    if (actor->sparse != NULL) {
        out->invokes = actor->sparse->invokes;
        out->invoke_count = actor->sparse->invoke_count;
        out->delayed_sends = actor->sparse->timers.count == 0 ? NULL : actor->sparse->timers.sends;
        out->delayed_send_count = actor->sparse->timers.count;
        out->has_completion = actor->finished;
        out->completion = actor->finished ? &actor->sparse->completion : NULL;
        out->final_state = actor->finished ? actor->sparse->final_state : TA_STATE_NONE;
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

TA_API ta_result ta_system_visit_actors(ta_system *system, const ta_actor_visit_params *params) {
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    /* A walk of everything may leave `next` null. */
    uint32_t unused = 0;
    uint32_t *next = params->next != NULL ? params->next : &unused;
    *next = params->from;
    if (system == NULL || params->inspector.visit == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t visited = 0;
    uint32_t slot = params->from;
    for (; slot < system->actors.count; slot += 1u) {
        if (params->limit != 0 && visited == params->limit) {
            break;
        }
        /* Read the entry each time: a visit may spawn and grow the table. */
        const ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor == NULL) {
            continue;
        }
        ta_actor_inspection inspection;
        ta_result result = ta_actor_inspect(system, entry->actor->record.id, &inspection);
        if (result == TA_OK) {
            result = params->inspector.visit(params->inspector.self, &inspection);
        }
        if (result != TA_OK) {
            *next = slot; /* not taken: the next call offers it again */
            return result;
        }
        visited += 1u;
    }
    *next = slot >= system->actors.count ? TA_ACTOR_VISIT_END : slot;
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
    out->data = actor->sparse->completion.data;
    ta_value_retain(&out->data);
    out->has_data = true;
    return TA_OK;
}


/*
 * A session image owns one arena, and every core-owned byte it keeps lives
 * there: the session storage, the queued events, the invocation records, the
 * delayed sends, and the text of the definition identity and of a named
 * address. That text would otherwise dangle — the identity's family name and
 * revision live in the *definition's* arena, and a named actor's address text
 * dies with the actor — and an image is meant to outlive both.
 *
 * Host values are not copied. They are retained, because a ta_value is a
 * handle and retain/release is its whole contract (ADR 0050). That is also
 * what makes capture all-or-nothing: every arena allocation happens first,
 * and only once all of them have succeeded does the image retain anything.
 * There is no half-retained image to unwind.
 */
struct ta_session_image {
    ta_allocator allocator;
    ta_arena arena;

    /* Local freshness: which actor this came from, and at which epoch. */
    ta_actor_id source_actor;
    uint64_t source_epoch;

    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;

    void *session_storage;
    size_t session_bytes;

    /* Execution state; the actor holds all of it between microsteps. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    bool has_external_event;
    ta_event external_event;
    bool has_macrostep_event;
    ta_event macrostep_event;
    bool has_current_event;
    ta_event current_event;
    uint64_t next_send_id;

    ta_queued_event *internal_events;
    uint32_t internal_count;
    /* Logical records only: a service handle is runtime-only and is dropped. */
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    ta_delayed_send *delayed_sends;
    uint32_t delayed_send_count;
    ta_state_id *pending_invokes;
    uint32_t pending_invoke_count;

    bool has_parent;
    ta_actor_parent parent;
    bool completion_pending;
    ta_actor_completion completion;
    /* The final state that ended the session, with `finished` (ADR 0056). */
    ta_state_id final_state;
    /* A microstep paused inside executable content (ADR 0061), or null. */
    ta_continuation *paused;
};

/* ------------------------------------------------------------------------
 * Arena-backed copies of the text an image must not borrow
 */

static ta_result ta_image_copy_str(ta_arena *arena, ta_str *text) {
    if (text->ptr == NULL) {
        return TA_OK;
    }
    char *copy = ta_arena_alloc(arena, (size_t)text->len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, text->ptr, text->len);
    copy[text->len] = '\0';
    text->ptr = copy;
    return TA_OK;
}

/*
 * An image carries its addresses as detached copies in its own arena (ADR
 * 0065): the strings, not the capturing system's handles, so the image does
 * not depend on that system (ADR 0028). A runtime ref owns nothing.
 */
static ta_result ta_image_copy_address(ta_arena *arena, ta_actor_ref *ref) {
    return ta_actor_ref_detach(arena, ref);
}

static ta_result ta_image_copy_origin(ta_arena *arena, ta_event *event) {
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_detach(arena, origin);
}

/* Makes an event restored from an image hold this system's origin handle. */
static ta_result ta_image_localize_origin(ta_system *system, ta_event *event) {
    ta_actor_ref *origin = ta_event_origin_ref(event);
    return origin == NULL ? TA_OK : ta_actor_ref_localize(system, origin);
}

static ta_result ta_image_copy_identity(ta_arena *arena, ta_definition_identity *identity) {
    ta_result result = ta_image_copy_str(arena, &identity->family.name);
    if (result != TA_OK) {
        return result;
    }
    ta_str revision = { .ptr = (const char *)identity->revision.ptr, .len = identity->revision.len };
    result = ta_image_copy_str(arena, &revision);
    identity->revision = (ta_bytes){ .ptr = (const uint8_t *)revision.ptr, .len = revision.len };
    return result;
}

/* ------------------------------------------------------------------------
 * Capture
 */

/* What the arena has to hold, so one block usually serves the whole image. */
static size_t ta_image_measure(const ta_actor *actor) {
    const ta_actor_sparse_state *sparse = actor->sparse;
    size_t total = actor->session_bytes + 64u;
    total += (size_t)actor->definition->identity.family.name.len + 1u;
    total += (size_t)actor->definition->identity.revision.len + 1u;
    if (actor->record.self.kind == TA_ACTOR_REF_NAMED) {
        ta_actor_address view = ta_address_view(actor->record.self.as.named);
        total += sizeof(ta_address) + (size_t)view.family.name.len + (size_t)view.name.len + 2u;
    }
    if (actor->internal_queue != NULL) {
        total += (size_t)actor->internal_queue->count * sizeof(ta_queued_event);
    }
    if (sparse != NULL) {
        total += (size_t)sparse->invoke_count * sizeof(ta_active_invoke);
        total += (size_t)sparse->pending_invokes.count * sizeof(ta_state_id);
    }
    total += (size_t)ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL) * sizeof(ta_delayed_send);
    if (sparse != NULL && sparse->paused != NULL) {
        total += sparse->paused->bytes + TA_CONTINUATION_ALIGN;
    }
    return total;
}

/* Every arena allocation of a capture; nothing here retains anything. */
static ta_result ta_image_fill(ta_session_image *image, const ta_actor *actor) {
    ta_arena *arena = &image->arena;
    const ta_actor_sparse_state *sparse = actor->sparse;

    image->session_storage = ta_arena_alloc(arena, actor->session_bytes, _Alignof(uint64_t));
    if (image->session_storage == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(image->session_storage, actor->session_storage, actor->session_bytes);
    image->session_bytes = actor->session_bytes;

    ta_result result = ta_image_copy_identity(arena, &image->definition);
    if (result == TA_OK) {
        result = ta_image_copy_address(arena, &image->self);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(arena, &image->external_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(arena, &image->macrostep_event);
    }
    if (result == TA_OK) {
        result = ta_image_copy_origin(arena, &image->current_event);
    }
    if (result != TA_OK) {
        return result;
    }

    if (actor->internal_queue != NULL && actor->internal_queue->count != 0) {
        uint32_t count = actor->internal_queue->count;
        image->internal_events = ta_arena_alloc(arena, (size_t)count * sizeof(ta_queued_event),
                                                _Alignof(ta_queued_event));
        if (image->internal_events == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->internal_events[index] = actor->internal_queue->events[index];
            result = ta_image_copy_origin(arena, &image->internal_events[index].event);
            if (result != TA_OK) {
                return result;
            }
        }
        image->internal_count = count;
    }

    if (sparse != NULL && sparse->invoke_count != 0) {
        image->invokes = ta_arena_alloc(arena, (size_t)sparse->invoke_count * sizeof(ta_active_invoke),
                                        _Alignof(ta_active_invoke));
        if (image->invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < sparse->invoke_count; index += 1) {
            image->invokes[index] = sparse->invokes[index];
            /* Runtime-only: the service is gone by the time this is restored. */
            image->invokes[index].service_handle = 0;
        }
        image->invoke_count = sparse->invoke_count;
    }

    if (sparse != NULL && sparse->pending_invokes.count != 0) {
        uint32_t count = sparse->pending_invokes.count;
        image->pending_invokes = ta_arena_alloc(arena, (size_t)count * sizeof(ta_state_id),
                                                _Alignof(ta_state_id));
        if (image->pending_invokes == NULL) {
            return TA_ERR_NOMEM;
        }
        for (uint32_t index = 0; index < count; index += 1) {
            image->pending_invokes[index] = *(const ta_state_id *)ta_vec_at(&sparse->pending_invokes, index);
        }
        image->pending_invoke_count = count;
    }

    if (sparse != NULL && sparse->paused != NULL) {
        image->paused = ta_arena_alloc(arena, sparse->paused->bytes, TA_CONTINUATION_ALIGN);
        if (image->paused == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(image->paused, sparse->paused, sparse->paused->bytes);
    }

    uint32_t sends = ta_timer_copy_unsubmitted(ta_actor_timers(actor), NULL);
    if (sends != 0) {
        image->delayed_sends = ta_arena_alloc(arena, (size_t)sends * sizeof(ta_delayed_send),
                                              _Alignof(ta_delayed_send));
        if (image->delayed_sends == NULL) {
            return TA_ERR_NOMEM;
        }
        (void)ta_timer_copy_unsubmitted(ta_actor_timers(actor), image->delayed_sends);
        image->delayed_send_count = sends;
        for (uint32_t index = 0; index < sends; index += 1) {
            ta_delayed_send *send = &image->delayed_sends[index];
            result = ta_image_copy_address(arena, &send->destination);
            if (result == TA_OK) {
                result = ta_image_copy_address(arena, &send->origin);
            }
            if (result != TA_OK) {
                return result;
            }
        }
    }
    return TA_OK;
}

/* The second half of a capture: infallible, so it cannot leave a half-image. */
static void ta_image_retain(ta_session_image *image) {
    ta_value_retain(&image->external_event.data);
    ta_value_retain(&image->macrostep_event.data);
    ta_value_retain(&image->current_event.data);
    ta_value_retain(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_retain(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_retain(&image->delayed_sends[index].target);
        ta_value_retain(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_retain(image->paused);
    }
}

TA_API ta_result ta_actor_capture(ta_system *system, const ta_actor_capture_params *params) {
    if (system == NULL || params == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    *params->out = NULL;
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* An unpublished prepared replacement is not a routable session yet. */
    if (!actor->published) {
        return TA_ERR_BUSY;
    }

    ta_session_image *image = ta_mem_alloc(system->allocator, sizeof(ta_session_image),
                                           _Alignof(ta_session_image));
    if (image == NULL) {
        return TA_ERR_NOMEM;
    }
    const ta_actor_sparse_state *sparse = actor->sparse;
    *image = (ta_session_image){
        .allocator = system->allocator,
        .source_actor = actor->record.id,
        .source_epoch = actor->session_epoch,
        .self = actor->record.self,
        .definition = actor->definition->identity,
        .residency = actor->record.residency,
        .published = actor->published,
        .bootstrapped = actor->bootstrapped,
        .macrostep = actor->macrostep,
        .finished = actor->finished,
        /*
         * An event is the actor's only while its flag is set. A cleared flag
         * can leave stale bytes behind: taking the external event moves it
         * into current_event and keeps the copy so a failed microstep can put
         * it back, and releasing an event leaves its origin in place. The
         * image takes, and later retains, detaches and releases, only what
         * the flags say the actor holds.
         */
        .has_external_event = actor->has_external_event,
        .external_event = actor->has_external_event ? actor->external_event : (ta_event){0},
        .has_macrostep_event = actor->has_macrostep_event,
        .macrostep_event = actor->has_macrostep_event ? actor->macrostep_event : (ta_event){0},
        .has_current_event = actor->has_current_event,
        .current_event = actor->has_current_event ? actor->current_event : (ta_event){0},
        .next_send_id = actor->next_send_id,
        .has_parent = sparse != NULL && sparse->has_parent,
        .parent = sparse != NULL ? sparse->parent : (ta_actor_parent){0},
        .completion_pending = sparse != NULL && sparse->completion_pending,
        .completion = sparse != NULL ? sparse->completion : (ta_actor_completion){0},
        .final_state = sparse != NULL && actor->finished ? sparse->final_state : TA_STATE_NONE,
    };
    ta_result result = ta_arena_init(&image->arena, system->allocator, ta_image_measure(actor));
    if (result == TA_OK) {
        result = ta_image_fill(image, actor);
    }
    if (result != TA_OK) {
        /* Nothing was retained yet, so the arena is the whole of it. */
        ta_arena_destroy(&image->arena);
        ta_mem_free(system->allocator, image,
                    &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                                 .alignment = _Alignof(ta_session_image) });
        return result;
    }
    ta_image_retain(image);
    *params->out = image;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Reading and releasing an image
 */

TA_API ta_result ta_session_image_next_deadline(const ta_session_image *image, ta_time *out) {
    if (image == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (image->delayed_send_count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_time earliest = image->delayed_sends[0].due;
    for (uint32_t index = 1; index < image->delayed_send_count; index += 1) {
        if (image->delayed_sends[index].due < earliest) {
            earliest = image->delayed_sends[index].due;
        }
    }
    *out = earliest;
    return TA_OK;
}

TA_API void ta_session_image_destroy(ta_session_image *image) {
    if (image == NULL) {
        return;
    }
    /* The mirror of ta_image_retain(), in the same order. */
    ta_value_release(&image->external_event.data);
    ta_value_release(&image->macrostep_event.data);
    ta_value_release(&image->current_event.data);
    ta_value_release(&image->completion.data);
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_value_release(&image->internal_events[index].event.data);
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_value_release(&image->delayed_sends[index].target);
        ta_value_release(&image->delayed_sends[index].data);
    }
    if (image->paused != NULL) {
        ta_continuation_release(image->paused);
    }
    ta_allocator allocator = image->allocator;
    ta_arena_destroy(&image->arena);
    ta_mem_free(allocator, image,
                &(ta_allocator_free_params){ .size = sizeof(ta_session_image),
                                             .alignment = _Alignof(ta_session_image) });
}

/* ------------------------------------------------------------------------
 * Reconstruction
 */

/*
 * The definition the image names, which must be the exact revision. A
 * different revision of the same family would reinterpret every state ID, so
 * it is a mismatch rather than a near miss; no definition of the family at
 * all is simply not found.
 */
static ta_result ta_image_definition(ta_system *system, const ta_session_image *image, ta_definition_id *out) {
    ta_result result = ta_system_find_definition(system, &image->definition, out);
    if (result != TA_ERR_NOT_FOUND) {
        return result;
    }
    for (uint32_t slot = 0; slot < system->definitions.count; slot += 1) {
        const ta_definition_slot *entry = ta_vec_at(&system->definitions, slot);
        const ta_definition *definition = entry->definition;
        if (definition != NULL && !definition->retired &&
            ta_str_equal(definition->identity.family.name, image->definition.family.name)) {
            return TA_ERR_DEFINITION_MISMATCH;
        }
    }
    return TA_ERR_NOT_FOUND;
}

/*
 * A runtime reference to the actor that was captured means "itself", and the
 * reconstructed session has a new runtime ID. Left alone, a self-addressed
 * delayed send would fire into the id of a session that no longer exists.
 */
static void ta_image_rebind_self(ta_actor_ref *ref, ta_actor_id was, ta_actor_id now) {
    if (ref->kind == TA_ACTOR_REF_RUNTIME && ref->as.runtime == was) {
        ref->as.runtime = now;
    }
}

/* Restores the queued events, invocation losses, and delayed sends. */
static ta_result ta_image_restore_session(ta_session_image *image, ta_actor *actor, ta_system *system) {
    for (uint32_t index = 0; index < image->internal_count; index += 1) {
        ta_event event;
        ta_event_retain(&image->internal_events[index].event, &event);
        ta_result result = ta_image_localize_origin(system, &event);
        if (result != TA_OK) {
            ta_event_release(&event);
            return result;
        }
        result = ta_interpreter_enqueue_internal(system, actor, &event);
        if (result != TA_OK) {
            return result;
        }
    }
    /*
     * Invoker service handles do not survive an image, so every invocation
     * that was active is retired and reported once, in the order it was
     * captured, after the events the image already carried.
     */
    for (uint32_t index = 0; index < image->invoke_count; index += 1) {
        if (!image->invokes[index].active) {
            continue;
        }
        ta_result result = ta_invoke_report_lost(system, actor, image->invokes[index].id);
        if (result != TA_OK) {
            return result;
        }
    }
    for (uint32_t index = 0; index < image->delayed_send_count; index += 1) {
        ta_delayed_send send = image->delayed_sends[index];
        /* A fresh timer ID; the SCXML send ID that <cancel> names is kept. */
        send.id = 0;
        send.owner = actor->record.id;
        ta_image_rebind_self(&send.destination, image->source_actor, actor->record.id);
        ta_image_rebind_self(&send.origin, image->source_actor, actor->record.id);
        ta_value_retain(&send.target);
        ta_value_retain(&send.data);
        /* The image's detached addresses become this system's handles. */
        ta_result result = ta_actor_ref_localize(system, &send.destination);
        if (result == TA_OK) {
            result = ta_actor_ref_localize(system, &send.origin);
        }
        if (result == TA_OK) {
            result = ta_actor_schedule_send(system, actor, &send);
        }
        if (result != TA_OK) {
            ta_value_release(&send.target);
            ta_value_release(&send.data);
            ta_actor_ref_release(&send.destination);
            ta_actor_ref_release(&send.origin);
            return result;
        }
    }
    /*
     * Reporting the lost invocations put work in the queue of an actor the
     * image recorded as idle, so it has a macrostep to run again.
     */
    if (!actor->macrostep && ta_interpreter_internal_pending(actor)) {
        actor->macrostep = true;
        actor->record.residency = TA_ACTOR_RUNNING;
    }
    return TA_OK;
}

/*
 * Gives the actor its own copy of a microstep the image captured paused
 * inside executable content (ADR 0061 section 4); its snapshots are
 * retained again, so the image keeps its own.
 */
static ta_result ta_image_restore_pause(const ta_session_image *image, ta_actor *actor, ta_system *system) {
    ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation *copy = ta_mem_alloc(system->allocator, image->paused->bytes, TA_CONTINUATION_ALIGN);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_mem_copy(copy, image->paused, image->paused->bytes);
    ta_continuation_retain(copy);
    sparse->paused = copy;
    return TA_OK;
}

TA_API ta_result ta_actor_reconstruct(ta_system *system, const ta_actor_reconstruct_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    ta_session_image *image = (ta_session_image *)params->image;
    ta_definition_id definition_id = 0;
    ta_result result = ta_image_definition(system, image, &definition_id);
    if (result != TA_OK) {
        return result;
    }
    ta_definition *definition = ta_system_definition(system, definition_id);
    ta_actor *actor = NULL;
    result = ta_actor_new(system, definition, &actor);
    if (result != TA_OK) {
        return result;
    }
    ta_actor_id id = actor->record.id;

    /* The interpreter state the image describes, restored before anything
     * fallible runs against it. */
    ta_mem_copy(actor->session_storage, image->session_storage, image->session_bytes);
    /*
     * A named actor's logical self is its address and survives verbatim; an
     * anonymous one's self is its runtime ID, and this is a new session, so
     * it answers to the new one. Restoring the captured ID would leave the
     * actor addressing a session that no longer exists — every self-send,
     * every reply address, every delayed timer.
     */
    actor->record.self = image->self;
    if (image->self.kind == TA_ACTOR_REF_RUNTIME) {
        actor->record.self.as.runtime = id;
    }
    actor->record.residency = image->residency;
    actor->published = image->published;
    actor->bootstrapped = image->bootstrapped;
    actor->macrostep = image->macrostep;
    actor->finished = image->finished;
    actor->next_send_id = image->next_send_id;
    actor->session_epoch = image->source_epoch;
    if (image->has_external_event) {
        ta_event_retain(&image->external_event, &actor->external_event);
        actor->has_external_event = true;
    }
    if (image->has_macrostep_event) {
        ta_event_retain(&image->macrostep_event, &actor->macrostep_event);
        actor->has_macrostep_event = true;
    }
    if (image->has_current_event) {
        ta_event_retain(&image->current_event, &actor->current_event);
        actor->has_current_event = true;
    }
    /*
     * The image carries detached addresses; the actor holds this system's
     * handles (ADR 0065). A failure leaves a detached one behind, which
     * releasing ignores.
     */
    result = ta_actor_ref_localize(system, &actor->record.self);
    if (result == TA_OK && actor->has_external_event) {
        result = ta_image_localize_origin(system, &actor->external_event);
    }
    if (result == TA_OK && actor->has_macrostep_event) {
        result = ta_image_localize_origin(system, &actor->macrostep_event);
    }
    if (result == TA_OK && actor->has_current_event) {
        result = ta_image_localize_origin(system, &actor->current_event);
    }
    if (result == TA_OK && image->paused != NULL) {
        result = ta_image_restore_pause(image, actor, system);
    }

    if (result == TA_OK && (image->finished || image->has_parent || image->completion.has_data ||
                            image->internal_count != 0 || image->invoke_count != 0 ||
                            image->pending_invoke_count != 0)) {
        ta_actor_sparse_state *sparse = ta_interpreter_sparse(system, actor);
        if (sparse == NULL) {
            result = TA_ERR_NOMEM;
        } else {
            sparse->has_parent = image->has_parent;
            sparse->parent = image->parent;
            sparse->completion_pending = image->completion_pending;
            sparse->completion = image->completion;
            ta_value_retain(&sparse->completion.data);
            sparse->final_state = image->final_state;
            if (image->pending_invoke_count != 0 && sparse->pending_invokes.element_size == 0) {
                ta_vec_init(&sparse->pending_invokes, system->allocator, TA_VEC_LAYOUT(ta_state_id));
            }
            for (uint32_t index = 0; result == TA_OK && index < image->pending_invoke_count; index += 1) {
                result = ta_vec_push(&sparse->pending_invokes, &image->pending_invokes[index]);
            }
        }
    }
    if (result == TA_OK) {
        result = definition->datamodel.v->init_instance(
            definition->datamodel.self,
            &(ta_datamodel_init_instance_params){
                .shared = definition->datamodel_shared,
                .instance = actor->datamodel_instance,
                .binding = params->datamodel_binding,
                .host = &actor->host,
            });
        actor->datamodel_initialized = result == TA_OK;
    }
    if (result == TA_OK) {
        result = ta_image_restore_session(image, actor, system);
    }
    if (result != TA_OK) {
        /* Nothing of this actor is published, so the image is untouched and
         * the caller may try again. */
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    *params->out = id;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Residency: unload and load
 *
 * A residency change is not a lifecycle change. The actor stays logically
 * alive throughout, its name stays reserved, and the only thing that moves
 * is where its session lives. The two operations are mirror images, and
 * each orders its directory swap against its fallible work so that a
 * failure anywhere leaves the actor exactly where it started (ADR 0014):
 *
 *   unload  swaps first, then discards  — nothing is thrown away until the
 *                                         route is gone, so a refused swap
 *                                         leaves a resident actor intact
 *   load    builds first, then swaps    — nothing is routable until the
 *                                         session is whole, so a refused
 *                                         swap leaves an unloaded actor
 */

TA_API ta_result ta_actor_unload(ta_system *system, const ta_actor_unload_params *params) {
    if (system == NULL || params == NULL || params->image == NULL) {
        return TA_ERR_INVALID;
    }
    ta_actor *actor = ta_actor_lookup(system, params->actor);
    if (actor == NULL) {
        return TA_ERR_NOT_FOUND;
    }
    /* Only a named actor can be unloaded: an anonymous session has no name
     * to hold its place, so evicting it would simply lose it. */
    if (actor->record.self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }
    const ta_session_image *image = params->image;
    /*
     * The image must describe this actor as it is now. An image captured
     * before the actor processed another event would check-point a session
     * the actor has already moved past, so the host is sent back to capture
     * again rather than silently persisting the older one.
     */
    if (image->source_actor != actor->record.id || image->source_epoch != actor->session_epoch) {
        return TA_ERR_STALE;
    }
    /* Room for the submitted sends to outlive the session, before anything changes. */
    ta_result result = ta_timer_reserve_orphans(&system->timers, ta_actor_timers(actor));
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = actor->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = actor->record.id },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
        });
    if (result != TA_OK) {
        /* Still resident, still routable, and the same image will do. */
        return result;
    }

    /*
     * The route is gone, so the session may go. Its not-yet-submitted sends
     * live in the image now and must not also sit in the resident heap,
     * where they would fire for an actor that is not there; a send already
     * handed to the router is an envelope of its own and stays, exactly as
     * it does when a session ends.
     */
    ta_timer_orphan(&system->timers, ta_actor_timers(actor));
    ta_actor_release_slot(system, actor->record.id);
    ta_actor_free(system, actor);
    return TA_OK;
}

TA_API ta_result ta_actor_load(ta_system *system, const ta_actor_load_params *params) {
    if (system == NULL || params == NULL || params->image == NULL || params->out == NULL) {
        return TA_ERR_INVALID;
    }
    const ta_session_image *image = params->image;
    if (image->self.kind != TA_ACTOR_REF_NAMED) {
        return TA_ERR_INVALID;
    }

    /*
     * Built off-route: the directory still reads UNLOADED, so nothing can
     * reach this session until it is whole. Everything that can fail —
     * finding the definition, allocating, binding the data model, restoring
     * the queue and the timers — happens here.
     */
    ta_actor_id id = 0;
    ta_result result = ta_actor_reconstruct(
        system,
        &(ta_actor_reconstruct_params){ .image = image,
                                        .datamodel_binding = params->datamodel_binding,
                                        .out = &id });
    if (result != TA_OK) {
        return result;
    }

    result = ta_actor_directory_swap(
        system,
        &(ta_actor_directory_replace_params){
            .address = ta_actor_lookup(system, id)->record.self.as.named,
            .expected = { .kind = TA_ACTOR_DIRECTORY_UNLOADED },
            .replacement = { .kind = TA_ACTOR_DIRECTORY_RESIDENT, .actor = id },
        });
    if (result != TA_OK) {
        /*
         * Nothing ever routed to this session, so taking it back down leaves
         * no trace: the actor is still unloaded, the image is untouched, and
         * the host may load it again.
         */
        ta_actor *actor = ta_actor_lookup(system, id);
        ta_actor_release_slot(system, id);
        ta_actor_free(system, actor); /* frees its delayed sends too */
        return result;
    }
    *params->out = id;
    return TA_OK;
}



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
    ta_vec_init(&system->actors.pages, config->allocator, TA_VEC_LAYOUT(ta_actor_slot *));
    system->free_actor_slot = UINT32_MAX;
    ta_vec_init(&system->definitions, config->allocator, TA_VEC_LAYOUT(ta_definition_slot));
    system->free_definition_slot = UINT32_MAX;
    ta_vec_init(&system->io_processors, config->allocator, TA_VEC_LAYOUT(ta_io_registration));
    ta_vec_init(&system->invokers, config->allocator, TA_VEC_LAYOUT(ta_invoker_registration));
    ta_address_table_init(&system->addresses, config->allocator);
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
    /* Teardown releases submitted sends instead of keeping them as orphans. */
    system->timers.closing = true;
    for (uint32_t slot = 0; slot < system->actors.count; slot += 1) {
        ta_actor_slot *entry = ta_actor_slot_at(system, slot);
        if (entry->actor != NULL) {
            (void)ta_actor_destroy(system, ta_actor_id_make(slot, entry->generation));
        }
    }
    while (system->unregistered_definitions != NULL) {
        ta_definition_destroy(system->unregistered_definitions);
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
    ta_actor_table_destroy(&system->actors, system->allocator);
    ta_vec_destroy(&system->io_processors);
    ta_vec_destroy(&system->invokers);
    ta_arena_destroy(&system->scratch);
    ta_interpreter_queue_cache_destroy(system);
    /* Last: everything above gave its addresses back. */
    ta_address_table_destroy(&system->addresses);
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
    if (definition->unregistered_previous != NULL) {
        definition->unregistered_previous->unregistered_next = definition->unregistered_next;
    } else {
        system->unregistered_definitions = definition->unregistered_next;
    }
    if (definition->unregistered_next != NULL) {
        definition->unregistered_next->unregistered_previous = definition->unregistered_previous;
    }
    definition->unregistered_previous = NULL;
    definition->unregistered_next = NULL;
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
    return ta_timer_next_due(&system->timers, out) ? TA_OK : TA_ERR_NOT_FOUND;
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
        ta_time due = 0;
        if (!ta_timer_next_due(&system->timers, &due) || due > now) {
            break;
        }
        ta_delayed_send send;
        (void)ta_timer_pop(&system->timers, &send);
        if (ta_interpreter_dispatch_delayed(system, &send) == TA_EXTERNAL_EVENT_RETRY) {
            /*
             * Rescheduling keeps its ID, and so its place in deadline order.
             * The owner may be gone by now; its submitted sends are orphans.
             */
            ta_timer_list *home = NULL;
            ta_result result = ta_actor_timer_home(system, send.owner, &home);
            if (result == TA_OK) {
                result = ta_timer_schedule(&system->timers, home, &send);
            }
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
    ta_time next = 0;
    *params->dispatched = dispatched;
    *params->more_due = retry || (ta_timer_next_due(&system->timers, &next) && next <= now);
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

/* A block stopped at a pause point with its frames left open (ADR 0061). */
#define TA_EXEC_PAUSED 2

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
        *actor->sparse = (ta_actor_sparse_state){ .final_state = TA_STATE_NONE };
    }
    return actor->sparse;
}

/* ------------------------------------------------------------------------
 * Internal queues (ADR 0064)
 */

/* A queue's first capacity, and the largest one the system caches. */
#define TA_INTERNAL_QUEUE_INITIAL 8u
#define TA_INTERNAL_QUEUE_CACHED_MAX 64u

TA_PRIVATE size_t ta_internal_queue_bytes(uint32_t capacity) {
    return offsetof(ta_internal_queue, events) + (size_t)capacity * sizeof(ta_queued_event);
}

static void ta_queue_free(ta_system *system, ta_internal_queue *queue) {
    ta_mem_free(system->allocator, queue,
                &(ta_allocator_free_params){ .size = ta_internal_queue_bytes(queue->capacity),
                                             .alignment = _Alignof(ta_internal_queue) });
}

/*
 * Makes sure the actor holds a queue with room for one more event: its own,
 * else one from the system's cache, else a new one; a full queue doubles.
 * TA_ERR_NOMEM leaves the actor as it was, apart from a queue taken from
 * the cache, which it may keep.
 */
static ta_result ta_queue_room(ta_system *system, ta_actor *actor) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL) {
        if (system->queue_cache_count != 0) {
            system->queue_cache_count -= 1u;
            queue = system->queue_cache[system->queue_cache_count];
        } else {
            queue = ta_mem_alloc(system->allocator, ta_internal_queue_bytes(TA_INTERNAL_QUEUE_INITIAL),
                                 _Alignof(ta_internal_queue));
            if (queue == NULL) {
                return TA_ERR_NOMEM;
            }
            queue->capacity = TA_INTERNAL_QUEUE_INITIAL;
        }
        queue->count = 0;
        actor->internal_queue = queue;
    }
    if (queue->count < queue->capacity) {
        return TA_OK;
    }
    if (queue->capacity > UINT32_MAX / 2u) {
        return TA_ERR_NOMEM;
    }
    uint32_t capacity = queue->capacity * 2u;
    ta_internal_queue *grown = ta_mem_alloc(system->allocator, ta_internal_queue_bytes(capacity),
                                            _Alignof(ta_internal_queue));
    if (grown == NULL) {
        return TA_ERR_NOMEM;
    }
    grown->count = queue->count;
    grown->capacity = capacity;
    ta_mem_copy(grown->events, queue->events, (size_t)queue->count * sizeof(ta_queued_event));
    ta_queue_free(system, queue);
    actor->internal_queue = grown;
    return TA_OK;
}

/*
 * Gives an actor's queue back when the actor no longer needs it: it is
 * empty and no microstep is paused. It goes to the system's cache, or is
 * freed when the cache is full or the queue grew large. Cannot fail.
 */
static void ta_queue_settle(ta_system *system, ta_actor *actor) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL || queue->count != 0 || (actor->sparse != NULL && actor->sparse->paused != NULL)) {
        return;
    }
    actor->internal_queue = NULL;
    if (system->queue_cache_count < TA_INTERNAL_QUEUE_CACHE && queue->capacity <= TA_INTERNAL_QUEUE_CACHED_MAX) {
        system->queue_cache[system->queue_cache_count] = queue;
        system->queue_cache_count += 1u;
        return;
    }
    ta_queue_free(system, queue);
}

TA_PRIVATE void ta_interpreter_queue_cache_destroy(ta_system *system) {
    while (system->queue_cache_count != 0) {
        system->queue_cache_count -= 1u;
        ta_queue_free(system, system->queue_cache[system->queue_cache_count]);
    }
}

TA_PRIVATE ta_result ta_interpreter_enqueue_internal(ta_system *system, ta_actor *actor, ta_event *event) {
    ta_actor_touch(actor);
    ta_result result = ta_queue_room(system, actor);
    if (result != TA_OK) {
        ta_queue_settle(system, actor);
        ta_event_release(event);
        return result;
    }
    ta_internal_queue *queue = actor->internal_queue;
    queue->events[queue->count] = (ta_queued_event){
        .event = *event,
        .flags = TA_EVENT_FLAG_OWNS_DATA | (event->type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
    };
    queue->count += 1u;
    return TA_OK;
}

static bool ta_dequeue_internal(ta_actor *actor, ta_event *out) {
    ta_internal_queue *queue = actor->internal_queue;
    if (queue == NULL || queue->count == 0) {
        return false;
    }
    *out = queue->events[0].event;
    for (uint32_t index = 1; index < queue->count; index += 1) {
        queue->events[index - 1u] = queue->events[index];
    }
    queue->count -= 1u;
    return true;
}

/* Puts the event selected for a failed microstep back where resume finds it. */
static void ta_restore_current_event(ta_actor *actor, bool external) {
    if (external) {
        /* ta_set_current_event moved the retained external_event into current_event. */
        actor->has_external_event = true;
    } else {
        /* The event came out of this queue, so it has room for it again. */
        ta_internal_queue *queue = actor->internal_queue;
        ta_queued_event *events = queue->events;
        for (uint32_t index = queue->count; index > 0; index -= 1u) {
            events[index] = events[index - 1u];
        }
        events[0] = (ta_queued_event){
            .event = actor->current_event,
            .flags = TA_EVENT_FLAG_OWNS_DATA |
                     (actor->current_event.type == TA_EVENT_PLATFORM ? TA_EVENT_FLAG_GENERATED : 0u),
        };
        queue->count += 1u;
    }
    actor->current_event = (ta_event){0};
    actor->has_current_event = false;
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

TA_PRIVATE void ta_interpreter_literal_content(const ta_definition *definition, ta_content_id content, ta_str *media_type,
                                    ta_bytes *bytes) {
    static const uint8_t empty[1] = { 0 };
    const ta_content_def *def = &definition->contents[content];
    *media_type = def->media_type;
    *bytes = def->bytes.ptr != NULL ? def->bytes : (ta_bytes){ .ptr = empty, .len = 0 };
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
    } else if (!send->submitted && !ta_invoke_parent_accepts(system, sender, &send->destination)) {
        /* The parent already cancelled this invoked child. */
        status = TA_EXTERNAL_EVENT_ACCEPTED;
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
        /* A send to a host processor is dispatched while its sender lives,
         * so the content resolves against the sender's definition. */
        if (send->has_content && sender != NULL) {
            ta_interpreter_literal_content(sender->definition, send->content, &request.content_type,
                                           &request.content);
        }
        if (io != NULL && io->v->send(io->self, &request, &fault) == TA_OP_OK) {
            status = TA_EXTERNAL_EVENT_ACCEPTED;
        }
    }
    if (status == TA_EXTERNAL_EVENT_RETRY) {
        /*
         * The router accepted nothing (ADR 0040). The send is due, so it is
         * sent: it waits to be offered again with the envelope it has now,
         * <cancel> no longer reaches it, and it outlives its sender
         * (ADR 0054 D3, D5). The caller keeps it in the deadline index.
         */
        if (!send->submitted) {
            /* The stored send now owns its envelope's origin (ADR 0065). */
            ta_actor_ref_retain(&origin);
            ta_actor_ref_release(&send->origin);
            send->origin = origin;
            send->submitted = true;
            send->invoke_id = invoke_id;
        }
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
    ta_actor_ref_release(&send->destination);
    ta_actor_ref_release(&send->origin);
    return status;
}

typedef struct {
    ta_value data;
    ta_value target;
    ta_scxml_target scxml;
    ta_atom io_type;
    ta_send_id send_id;
    bool scxml_io;
    bool has_content;
    ta_content_id content;
} ta_send_state;

static ta_op_status ta_send_payload(ta_interpreter *it, const ta_send_action *send, ta_send_state *state,
                                    ta_fault *fault) {
    if (!state->scxml_io && send->data.kind == TA_VALUE_SOURCE_CONTENT) {
        /*
         * A host I/O processor receives literal content as bytes: its
         * interpretation is platform-specific (SCXML 6.4.2), and only the
         * built-in SCXML processor, whose receiver reads _event.data, imports
         * it (ADR 0053).
         */
        state->has_content = true;
        state->content = send->data.as.content;
        return TA_OP_OK;
    }
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
            /*
             * A send to self is a delivery to *this* session, so it names the
             * runtime ID and needs no directory to resolve — a named actor's
             * own timers must not depend on one. Reconstruction rebinds such
             * a send to the new session (ta_image_rebind_self); what a
             * receiver replies to is the origin, which is the address.
             */
            *out = (ta_actor_ref){ .kind = TA_ACTOR_REF_RUNTIME, .as.runtime = it->actor->record.id };
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
        ta_scxml_target_release(&state.scxml);
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
        /* The queued event owns its origin reference (ADR 0065). */
        ta_actor_ref_retain(&event.origin.as.actor);
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
        .has_content = state.has_content,
        .content = state.content,
    };
    /* A named target's reference passes to the send record (ADR 0065). */
    if (state.scxml_io && !ta_send_destination(it, &state.scxml, &record.destination)) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        ta_scxml_target_release(&state.scxml);
        return ta_communication_error(system, actor, &(ta_error_params){ .send_id = state.send_id });
    }
    if (!actor->published) {
        ta_result result = ta_actor_hold_send(
            system, actor, &(ta_activation_send){ .send = record, .immediate = delay == 0 });
        if (result != TA_OK) {
            ta_value_release(&state.data);
            ta_value_release(&state.target);
            ta_actor_ref_release(&record.destination);
        }
        return result;
    }
    if (delay == 0) {
        ta_external_event_submit_status submitted = ta_interpreter_dispatch_delayed(system, &record);
        if (submitted != TA_EXTERNAL_EVENT_RETRY) {
            return TA_OK;
        }
        /*
         * The router accepted nothing, typically because the destination is
         * this busy actor. The send, now marked submitted, waits in the
         * deadline index, due now, and keeps its order among later sends.
         */
    }
    ta_result result = ta_actor_schedule_send(system, actor, &record);
    if (result != TA_OK) {
        ta_value_release(&state.data);
        ta_value_release(&state.target);
        ta_actor_ref_release(&record.destination);
        ta_actor_ref_release(&record.origin);
    }
    return result;
}

/* ------------------------------------------------------------------------
 * Executable content
 */

/*
 * <foreach> iterates a shallow copy made by the data model. A failed
 * snapshot or binding, or an error in the body, stops the foreach and the
 * enclosing block (SCXML 4.6). The loop is a frame of the executor (see
 * ta_run_block): `actions` is the body, `cursor` the <foreach> action
 * itself, and the snapshot and position are the frame's.
 */
static ta_result ta_foreach_begin(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_datamodel_vtable *v = definition->datamodel.v;
    if (v->foreach_snapshot == NULL || v->foreach_bind == NULL) {
        return ta_execution_error(it, NULL);
    }
    ta_fault fault = {0};
    if (v->foreach_snapshot(definition->datamodel.self,
                            &(ta_datamodel_foreach_snapshot_params){
                                .shared = definition->datamodel_shared,
                                .instance = it->actor->datamodel_instance,
                                .array = definition->action_defs[loop->cursor].as.foreach.array,
                                .host = &it->actor->host,
                                .out = &loop->iteration_snapshot,
                                .count = &loop->iteration_count,
                                .fault = &fault,
                            }) != TA_OP_OK) {
        return ta_execution_error(it, &fault);
    }
    return TA_OK;
}

/* Binds the item and index for the loop's next position. */
static ta_result ta_foreach_bind(ta_interpreter *it, ta_exec_frame *loop) {
    const ta_definition *definition = it->definition;
    const ta_foreach_action *foreach = &definition->action_defs[loop->cursor].as.foreach;
    ta_str item = ta_atom_text(&it->system->atoms, foreach->item);
    ta_str index = foreach->has_index ? ta_atom_text(&it->system->atoms, foreach->index) : (ta_str){0};
    ta_fault fault = {0};
    ta_op_status bound = definition->datamodel.v->foreach_bind(definition->datamodel.self,
                                                               &(ta_datamodel_foreach_bind_params){
                                                                   .shared = definition->datamodel_shared,
                                                                   .instance = it->actor->datamodel_instance,
                                                                   .snapshot = &loop->iteration_snapshot,
                                                                   .index = loop->iteration,
                                                                   .item = item,
                                                                   .index_name = index,
                                                                   .host = &it->actor->host,
                                                                   .fault = &fault,
                                                               });
    loop->iteration += 1u;
    return bound == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
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
                (void)ta_actor_cancel_held_send(it->actor, send_id);
                (void)ta_timer_cancel(&it->system->timers, ta_actor_timers(it->actor), send_id);
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
                if (it->actor->published) {
                    it->system->log_sink.v->write(it->system->log_sink.self, &record);
                } else {
                    ta_result result = ta_actor_hold_log(it->system, it->actor, &record);
                    if (result != TA_OK) {
                        ta_value_release(&record.value);
                        return result;
                    }
                }
            }
            ta_value_release(&record.value);
            break;
        }
        /* <if> and <foreach> open blocks, which ta_run_block runs. */
        default:
            status = TA_OP_ERROR;
            break;
    }
    return status == TA_OP_OK ? TA_OK : ta_execution_error(it, &fault);
}

static ta_result ta_exec_push(ta_interpreter *it, ta_exec_frame frame) {
    if (it->exec_count == it->exec_capacity) {
        return TA_ERR_INVALID; /* unreachable: the definition's depth sizes the frames */
    }
    it->exec_frames[it->exec_count++] = frame;
    return TA_OK;
}

static ta_result ta_exec_block(ta_interpreter *it, ta_span32 block) {
    return ta_exec_push(it, (ta_exec_frame){ .kind = TA_EXEC_FRAME_ACTIONS, .actions = block });
}

/*
 * Runs a block of executable content and every block nested in it, without
 * recursion (ADR 0060). Each open block is an ACTIONS frame; an <if> opens
 * the block its condition picks, and a <foreach> is a FOREACH frame that
 * opens its body once per position. The frames come from the interpreter's
 * scratch, so nothing here allocates them. An error anywhere stops the whole
 * block, as SCXML 4.9 requires, and every snapshot still held is released.
 */
/*
 * Whether the executor stops before its next unit (ADR 0061): only in a
 * microstep's own block, never in <finalize> or at termination, and only
 * once this call has run its action budget.
 */
static bool ta_exec_should_pause(const ta_interpreter *it, uint32_t floor) {
    return it->pausable && it->in_microstep && floor == 0 && it->action_budget != 0 &&
           it->actions >= it->action_budget;
}

static ta_result ta_run_block(ta_interpreter *it, ta_span32 block) {
    const ta_definition *definition = it->definition;
    uint32_t floor = it->exec_count;
    ta_result result = TA_OK;
    if (it->at.block_open) {
        /* Resuming: the frames the pause left, from the bottom, are this
         * block's, which a pause only ever leaves at the microstep's level. */
        floor = 0;
        it->at.block_open = false;
    } else {
        it->at.block_open = false;
        result = ta_exec_block(it, block);
    }
    while (result == TA_OK && it->exec_count > floor) {
        ta_exec_frame *frame = &it->exec_frames[it->exec_count - 1u];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            if (frame->iteration == frame->iteration_count) {
                ta_value_release(&frame->iteration_snapshot);
                it->exec_count -= 1u;
                continue;
            }
            if (ta_exec_should_pause(it, floor)) {
                it->at.block_open = true;
                return TA_EXEC_PAUSED;
            }
            it->actions += 1u;
            result = ta_foreach_bind(it, frame);
            if (result == TA_OK) {
                result = ta_exec_block(it, frame->actions);
            }
            continue;
        }
        if (frame->cursor == frame->actions.length) {
            it->exec_count -= 1u;
            continue;
        }
        if (ta_exec_should_pause(it, floor)) {
            it->at.block_open = true;
            return TA_EXEC_PAUSED;
        }
        it->actions += 1u;
        uint32_t index = frame->actions.offset + frame->cursor;
        frame->cursor += 1u;
        const ta_action_def *action = &definition->action_defs[index];
        if (action->kind == TA_ACTION_IF) {
            bool condition = false;
            ta_fault fault = {0};
            ta_op_status status = ta_eval_condition(it, action->as.conditional.condition, &condition, &fault);
            result = status != TA_OP_OK ? ta_execution_error(it, &fault)
                                        : ta_exec_block(it, condition ? action->as.conditional.then_actions
                                                                      : action->as.conditional.else_actions);
        } else if (action->kind == TA_ACTION_FOREACH) {
            ta_exec_frame loop = { .kind = TA_EXEC_FRAME_FOREACH, .actions = action->as.foreach.body_actions,
                                   .cursor = index };
            result = ta_foreach_begin(it, &loop);
            if (result == TA_OK) {
                result = ta_exec_push(it, loop);
                if (result != TA_OK) {
                    ta_value_release(&loop.iteration_snapshot);
                }
            }
        } else {
            result = ta_exec_action(it, action);
        }
    }
    while (it->exec_count > floor) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    return result;
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
    /* `at.handler` is where a paused microstep resumes; it is 0 otherwise. */
    for (; it->at.handler < handlers.length; it->at.handler += 1u) {
        ta_result result = ta_run_block_isolated(it, it->definition->handlers[handlers.offset + it->at.handler]);
        if (result != TA_OK) {
            if (result != TA_EXEC_PAUSED) {
                it->at.handler = 0;
            }
            return result;
        }
    }
    it->at.handler = 0;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Transition selection
 */

/*
 * Adds a span of targets to `out`: a target itself, or for a history state
 * its recorded configuration, or else the targets of its default
 * transition, which may name history states again. Those are expanded from
 * a worklist, each at most once, so a history default that leads back to
 * itself ends instead of looping.
 */
static void ta_effective_span(ta_interpreter *it, ta_span32 targets, uint64_t *out, uint32_t *pending) {
    for (uint32_t index = 0; index < targets.length; index += 1) {
        ta_state_id target = ta_target_at(it, targets, index);
        if (!ta_is_history(it, target)) {
            ta_bits_put(out, target, true);
        } else if (!ta_bits_get(it->history_seen, target)) {
            ta_bits_put(it->history_seen, target, true);
            it->history_stack[(*pending)++] = target;
        }
    }
}

static void ta_effective_targets(ta_interpreter *it, ta_span32 targets, uint64_t *out) {
    ta_bits_clear(it, it->history_seen);
    uint32_t pending = 0;
    ta_effective_span(it, targets, out, &pending);
    while (pending != 0) {
        ta_state_id history = it->history_stack[--pending];
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, ta_state(it, history)->history_slot);
        if (ta_bits_any(it, recorded)) {
            for (uint32_t word = 0; word < it->words; word += 1) {
                out[word] |= recorded[word];
            }
        } else {
            const ta_transition_def *fallback = &it->definition->transitions[ta_state(it, history)->transitions.offset];
            ta_effective_span(it, fallback->targets, out, &pending);
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

static bool ta_entering_below(ta_interpreter *it, ta_state_id ancestor) {
    for (ta_state_id state = 0; state < it->definition->state_count; state += 1) {
        if (ta_bits_get(it->to_enter, state) && ta_descends(it, state, ancestor)) {
            return true;
        }
    }
    return false;
}

/*
 * The entry set (SCXML addDescendantStatesToEnter and
 * addAncestorStatesToEnter), without recursion (ADR 0060). Each frame is one
 * activation of the recursive algorithm, resumed where it left off, so the
 * order of every step, which ta_entering_below and default_history depend
 * on, is the recursion's:
 *
 *   DESCENDANTS  addDescendantStatesToEnter(state), not yet started
 *   TARGETS      for each target: descendants; then for each: ancestors
 *   RECORDED     the same over a history state's recorded configuration
 *   ANCESTORS    addAncestorStatesToEnter, `state` the next ancestor
 *   REGIONS      a parallel state's regions, `cursor` the next child
 *
 * A history frame marks its history state busy until it ends. A history
 * default that leads back to a busy history state would recurse forever;
 * it is skipped. Every other chain of descendant frames goes to strictly
 * deeper states. So a chain holds at most one descendants frame per level
 * of nesting and one per history state, and each of those has at most two
 * frames above it (a loop, and an ancestors walk with its regions).
 */
typedef enum {
    TA_ENTRY_DESCENDANTS = 0,
    TA_ENTRY_TARGETS,
    TA_ENTRY_RECORDED,
    TA_ENTRY_ANCESTORS,
    TA_ENTRY_REGIONS
} ta_entry_kind;

struct ta_entry_frame {
    ta_entry_kind kind;
    bool ancestors;         /* TARGETS, RECORDED: the second loop */
    ta_state_id state;
    ta_state_id ancestor;
    ta_state_id history;    /* the history state that owns this frame, or TA_STATE_NONE */
    ta_span32 targets;
    uint32_t cursor;
};

/* The frame capacity ta_interpreter_init reserves: three per level of
 * nesting and per history state, and room for the first frame. */
static uint32_t ta_entry_capacity(const ta_definition *definition) {
    return 3u * (definition->max_state_depth + definition->history.slot_count + 1u) + 1u;
}

static bool ta_entry_push(ta_interpreter *it, uint32_t *top, struct ta_entry_frame frame) {
    if (*top == it->entry_capacity) {
        return false; /* unreachable: the bound above holds */
    }
    it->entry_frames[(*top)++] = frame;
    return true;
}

static struct ta_entry_frame ta_entry_descendants(ta_state_id state) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_DESCENDANTS, .state = state, .history = TA_STATE_NONE };
}

static struct ta_entry_frame ta_entry_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    return (struct ta_entry_frame){ .kind = TA_ENTRY_ANCESTORS, .state = ta_state(it, state)->parent,
                                    .ancestor = ancestor, .history = TA_STATE_NONE };
}

/* Starts addDescendantStatesToEnter(frame->state): rewrites the frame into
 * the loop the recursion would run, or pops it. */
static void ta_entry_start(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    ta_state_id state = frame->state;
    const ta_state_def *def = ta_state(it, state);
    if (ta_is_history(it, state)) {
        if (ta_bits_get(it->history_busy, state)) {
            *top -= 1u;
            return;
        }
        ta_bits_put(it->history_busy, state, true);
        const uint64_t *recorded = ta_history_slot_words(&it->actor->history, def->history_slot);
        if (ta_bits_any(it, recorded)) {
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_RECORDED, .state = state, .ancestor = def->parent,
                                              .history = state };
        } else {
            it->default_history[def->parent] = state;
            *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = def->parent, .history = state,
                                              .targets = it->definition->transitions[def->transitions.offset].targets };
        }
        return;
    }
    ta_bits_put(it->to_enter, state, true);
    if (def->kind == TA_STATE_COMPOUND) {
        ta_bits_put(it->default_entry, state, true);
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_TARGETS, .ancestor = state, .history = TA_STATE_NONE,
                                          .targets = it->definition->initial_transitions[def->initial].targets };
    } else if (def->kind == TA_STATE_PARALLEL) {
        *frame = (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = state, .history = TA_STATE_NONE };
    } else {
        *top -= 1u;
    }
}

static void ta_entry_pop(ta_interpreter *it, uint32_t *top) {
    ta_state_id history = it->entry_frames[*top - 1u].history;
    if (history != TA_STATE_NONE) {
        ta_bits_put(it->history_busy, history, false);
    }
    *top -= 1u;
}

/* The next recorded state at or after `from`, or state_count. */
static ta_state_id ta_entry_next_recorded(ta_interpreter *it, const struct ta_entry_frame *frame, ta_state_id from) {
    const uint64_t *recorded =
        ta_history_slot_words(&it->actor->history, ta_state(it, frame->state)->history_slot);
    while (from < it->definition->state_count && !ta_bits_get(recorded, from)) {
        from += 1u;
    }
    return from;
}

/* One step of the frame on top of the stack. */
static bool ta_entry_step(ta_interpreter *it, uint32_t *top) {
    struct ta_entry_frame *frame = &it->entry_frames[*top - 1u];
    switch (frame->kind) {
        case TA_ENTRY_DESCENDANTS:
            ta_entry_start(it, top);
            return true;
        case TA_ENTRY_TARGETS: {
            if (frame->cursor == frame->targets.length) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            ta_state_id target = ta_target_at(it, frame->targets, frame->cursor++);
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, target, frame->ancestor)
                                                           : ta_entry_descendants(target));
        }
        case TA_ENTRY_RECORDED: {
            ta_state_id next = ta_entry_next_recorded(it, frame, frame->cursor);
            if (next == it->definition->state_count) {
                if (frame->ancestors) {
                    ta_entry_pop(it, top);
                    return true;
                }
                frame->ancestors = true;
                frame->cursor = 0;
                return true;
            }
            frame->cursor = next + 1u;
            return ta_entry_push(it, top, frame->ancestors ? ta_entry_ancestors(it, next, frame->ancestor)
                                                           : ta_entry_descendants(next));
        }
        case TA_ENTRY_ANCESTORS: {
            ta_state_id at = frame->state;
            if (at == frame->ancestor || at == TA_STATE_NONE) {
                ta_entry_pop(it, top);
                return true;
            }
            ta_bits_put(it->to_enter, at, true);
            frame->state = ta_state(it, at)->parent;
            if (ta_state(it, at)->kind == TA_STATE_PARALLEL) {
                return ta_entry_push(it, top, (struct ta_entry_frame){ .kind = TA_ENTRY_REGIONS, .state = at,
                                                                       .history = TA_STATE_NONE });
            }
            return true;
        }
        case TA_ENTRY_REGIONS: {
            ta_state_id parallel = frame->state;
            while (frame->cursor < ta_state(it, parallel)->children.length) {
                ta_state_id child = ta_child_at(it, parallel, frame->cursor++);
                if (!ta_is_history(it, child) && !ta_entering_below(it, child)) {
                    return ta_entry_push(it, top, ta_entry_descendants(child));
                }
            }
            ta_entry_pop(it, top);
            return true;
        }
    }
    return false;
}

static void ta_entry_run(ta_interpreter *it, struct ta_entry_frame first) {
    uint32_t top = 0;
    if (!ta_entry_push(it, &top, first)) {
        return;
    }
    while (top != 0 && ta_entry_step(it, &top)) {
    }
}

static void ta_add_descendants(ta_interpreter *it, ta_state_id state) {
    ta_entry_run(it, ta_entry_descendants(state));
}

static void ta_add_ancestors(ta_interpreter *it, ta_state_id state, ta_state_id ancestor) {
    ta_entry_run(it, ta_entry_ancestors(it, state, ancestor));
}

/* The first state after `state`'s subtree: states are numbered in document
 * pre-order, so a subtree is a contiguous range of IDs. */
static ta_state_id ta_subtree_end(ta_interpreter *it, ta_state_id state) {
    ta_state_id last = state;
    while (ta_state(it, last)->children.length != 0) {
        last = ta_child_at(it, last, ta_state(it, last)->children.length - 1u);
    }
    return last + 1u;
}

/*
 * SCXML isInFinalState, bottom up in one pass (ADR 0060 section 4): every
 * child has a higher ID than its parent, so visiting the subtree in reverse
 * ID order answers each child before its parent.
 */
static bool ta_in_final_state(ta_interpreter *it, ta_state_id state) {
    for (ta_state_id at = ta_subtree_end(it, state); at > state;) {
        at -= 1u;
        const ta_state_def *def = ta_state(it, at);
        bool final = false;
        if (def->kind == TA_STATE_COMPOUND) {
            for (uint32_t index = 0; index < def->children.length && !final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_state(it, child)->kind == TA_STATE_FINAL && ta_bits_get(it->active, child);
            }
        } else if (def->kind == TA_STATE_PARALLEL) {
            final = true;
            for (uint32_t index = 0; index < def->children.length && final; index += 1) {
                ta_state_id child = ta_child_at(it, at, index);
                final = ta_is_history(it, child) || ta_bits_get(it->final_states, child);
            }
        }
        ta_bits_put(it->final_states, at, final);
    }
    return ta_bits_get(it->final_states, state);
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

/* Computes the exit set and records history, once, as a microstep begins. */
static void ta_exit_begin(ta_interpreter *it) {
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
    it->at.state = definition->state_count;
    it->at.stage = TA_EXIT_HANDLERS;
}

/* Exits the exit set, highest first, from where `at` stands. */
static ta_result ta_exit_states(ta_interpreter *it) {
    while (it->at.state > 0) {
        ta_state_id state = it->at.state - 1u;
        if (!ta_bits_get(it->to_exit, state)) {
            it->at.state -= 1u;
            continue;
        }
        ta_result result = TA_OK;
        if (it->at.stage == TA_EXIT_HANDLERS) {
            result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_EXIT_HANDLERS_DONE;
        }
        result = ta_invoke_state_exited(it, state); /* Invoke: cancel this state's invocations. */
        if (result != TA_OK) {
            return result;
        }
        ta_bits_put(it->active, state, false);
        it->at.state -= 1u;
        it->at.stage = TA_EXIT_HANDLERS;
    }
    return TA_OK;
}

/* Computes the entry set, once, after the transitions' content has run. */
static void ta_enter_begin(ta_interpreter *it) {
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
    it->at.state = 0;
    it->at.stage = TA_ENTER_ACTIVATE;
}

/* Done data and done.state events for a final state just entered. */
static ta_result ta_enter_final(ta_interpreter *it, ta_state_id state) {
    const ta_state_def *def = ta_state(it, state);
    ta_value data = {0};
    ta_result result = ta_done_data_value(it, def->done_data, &data);
    if (result != TA_OK) {
        return result;
    }
    if (def->parent == TA_STATE_NONE) {
        it->actor->finished = true;
        result = ta_store_completion(it, &data);
        if (result == TA_OK) {
            it->actor->sparse->final_state = state; /* ADR 0056 */
        }
        return result;
    }
    result = ta_raise_done(it, def->parent, &data);
    ta_state_id grandparent = ta_state(it, def->parent)->parent;
    if (result == TA_OK && grandparent != TA_STATE_NONE &&
        ta_state(it, grandparent)->kind == TA_STATE_PARALLEL && ta_in_final_state(it, grandparent)) {
        ta_value none = {0};
        result = ta_raise_done(it, grandparent, &none);
    }
    return result;
}

/*
 * Enters the entry set, lowest first, from where `at` stands. Each stage
 * of a state happens once; `at.stage` records the ones done, so a paused
 * microstep never activates a state or starts its data twice (ADR 0061).
 */
static ta_result ta_enter_states(ta_interpreter *it) {
    const ta_definition *definition = it->definition;
    while (it->at.state < definition->state_count) {
        ta_state_id state = it->at.state;
        if (!ta_bits_get(it->to_enter, state)) {
            it->at.state += 1u;
            continue;
        }
        const ta_state_def *def = ta_state(it, state);
        ta_result result = TA_OK;
        if (it->at.stage == TA_ENTER_ACTIVATE) {
            ta_bits_put(it->active, state, true);
            result = ta_invoke_state_entered(it, state); /* Invoke: start after the macrostep. */
            if (definition->binding_mode == TA_BINDING_LATE && !ta_bits_get(it->actor->late_initialized, state)) {
                ta_bits_put(it->actor->late_initialized, state, true);
                result = ta_initialize_data(it, state);
            }
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_ENTER_HANDLERS;
        }
        if (it->at.stage == TA_ENTER_HANDLERS) {
            result = ta_run_handlers(it, def->onentry_handlers);
            if (result != TA_OK) {
                return result;
            }
            it->at.stage = TA_ENTER_INITIAL;
        }
        if (it->at.stage == TA_ENTER_INITIAL) {
            if (ta_bits_get(it->default_entry, state)) {
                result = ta_run_block_isolated(it, definition->initial_transitions[def->initial].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_HISTORY;
        }
        if (it->at.stage == TA_ENTER_HISTORY) {
            if (it->default_history[state] != TA_STATE_NONE) {
                const ta_state_def *history = ta_state(it, it->default_history[state]);
                result = ta_run_block_isolated(it, definition->transitions[history->transitions.offset].actions);
                if (result != TA_OK) {
                    return result;
                }
            }
            it->at.stage = TA_ENTER_DONE;
        }
        if (def->kind == TA_STATE_FINAL) {
            result = ta_enter_final(it, state);
            if (result != TA_OK) {
                return result;
            }
        }
        it->at.state += 1u;
        it->at.stage = TA_ENTER_ACTIVATE;
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

/*
 * Before exit, actions and entry make the microstep visible, make sure the
 * actor holds an internal queue with room for one event (lent, ADR 0064) and,
 * when the microstep may need it, its sparse state. So a full allocator
 * pauses at the boundary, and a retry starts the same microstep again
 * (ADR 0047).
 */
static ta_result ta_prepare_microstep_queue(ta_interpreter *it) {
    ta_result result = ta_queue_room(it->system, it->actor);
    /* A microstep that may keep a send, a completion, an invocation or a
     * held effect gets its sparse state now, while nothing is visible. */
    if (result == TA_OK && (it->definition->microstep_needs_sparse || !it->actor->published) &&
        ta_sparse(it->system, it->actor) == NULL) {
        result = TA_ERR_NOMEM;
    }
    return result;
}

static void ta_compact_sparse(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = it->actor->sparse;
    if (sparse == NULL || sparse->paused != NULL || sparse->invoke_count != 0 ||
        sparse->pending_invokes.count != 0 || sparse->activation_effects != NULL || sparse->has_parent ||
        sparse->completion_pending || sparse->completion.has_data) {
        return;
    }
    ta_invoke_release(it->system, it->actor);
    if (sparse->timers.count != 0) {
        /* Pending delayed sends keep the sparse state (ADR 0054). */
        return;
    }
    ta_timer_list_destroy(&it->system->timers, &sparse->timers);
    ta_mem_free(it->system->allocator, sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    it->actor->sparse = NULL;
}

/*
 * Runs a microstep, or continues the one `at` describes. TA_EXEC_PAUSED
 * leaves it open: the position, the sets and the frames stay in the
 * interpreter for ta_pause_save (ADR 0061), and the actor's configuration
 * is still the one before the microstep.
 */
static ta_result ta_microstep(ta_interpreter *it) {
    ta_result result = TA_OK;
    if (it->at.phase == TA_MICRO_NONE) {
        /* A microstep changes the configuration, history, or the queue: an
         * image taken before it no longer describes this actor. */
        if (it->actor->bootstrapped) {
            result = ta_prepare_microstep_queue(it);
        }
        if (result != TA_OK) {
            return result;
        }
        ta_actor_touch(it->actor);
        ta_exit_begin(it);
        it->at.phase = TA_MICRO_EXIT;
    }
    it->in_microstep = true;
    if (it->at.phase == TA_MICRO_EXIT) {
        result = ta_exit_states(it);
        if (result == TA_OK) {
            it->at.phase = TA_MICRO_TRANSITIONS;
            it->at.state = 0;
        }
    }
    if (result == TA_OK && it->at.phase == TA_MICRO_TRANSITIONS) {
        for (; result == TA_OK && it->at.state < it->enabled_count; it->at.state += 1u) {
            result = ta_run_block_isolated(it, ta_view(it, it->enabled[it->at.state]).actions);
            if (result != TA_OK) {
                break;
            }
        }
        if (result == TA_OK) {
            ta_enter_begin(it);
            it->at.phase = TA_MICRO_ENTER;
        }
    }
    if (result == TA_OK && it->at.phase == TA_MICRO_ENTER) {
        result = ta_enter_states(it);
    }
    it->in_microstep = false;
    if (result == TA_EXEC_PAUSED) {
        return result;
    }
    it->at = (ta_micro_position){0};
    ta_store_active(it);
    it->steps += 1u;
    return result;
}

/*
 * Moves a paused microstep out of scratch into the actor (ADR 0061
 * section 3). The frames' snapshots move with them. Fails without
 * changing anything when there is no memory.
 */
static ta_result ta_pause_save(ta_interpreter *it) {
    ta_actor_sparse_state *sparse = ta_sparse(it->system, it->actor);
    if (sparse == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_continuation shape = { .at = it->at, .words = it->words, .state_count = it->definition->state_count,
                              .enabled_count = it->enabled_count, .frame_count = it->exec_count };
    shape.bytes = ta_continuation_bytes(&shape);
    ta_continuation *saved = ta_mem_alloc(it->system->allocator, shape.bytes, TA_CONTINUATION_ALIGN);
    if (saved == NULL) {
        return TA_ERR_NOMEM;
    }
    *saved = shape;
    uint64_t *sets = ta_continuation_sets(saved);
    const uint64_t *from[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(from); index += 1u) {
        ta_mem_copy(sets + (size_t)index * it->words, from[index], (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(ta_continuation_history(saved), it->default_history,
                (size_t)shape.state_count * sizeof(ta_state_id));
    ta_mem_copy(ta_continuation_enabled(saved), it->enabled, (size_t)shape.enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(ta_continuation_frames(saved), it->exec_frames, (size_t)shape.frame_count * sizeof(ta_exec_frame));
    sparse->paused = saved;
    it->exec_count = 0;
    it->at = (ta_micro_position){0};
    return TA_OK;
}

/* Moves a paused microstep back into scratch; the actor no longer holds it. */
static void ta_pause_restore(ta_interpreter *it) {
    ta_continuation *saved = it->actor->sparse->paused;
    const uint64_t *sets = ta_continuation_sets(saved);
    uint64_t *to[] = { it->active, it->to_exit, it->to_enter, it->default_entry };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(to); index += 1u) {
        ta_mem_copy(to[index], sets + (size_t)index * it->words, (size_t)it->words * sizeof(uint64_t));
    }
    ta_mem_copy(it->default_history, ta_continuation_history(saved), (size_t)saved->state_count * sizeof(ta_state_id));
    ta_mem_copy(it->enabled, ta_continuation_enabled(saved), (size_t)saved->enabled_count * sizeof(ta_transition_id));
    ta_mem_copy(it->exec_frames, ta_continuation_frames(saved), (size_t)saved->frame_count * sizeof(ta_exec_frame));
    it->enabled_count = saved->enabled_count;
    it->exec_count = saved->frame_count;
    it->at = saved->at;
    it->actor->sparse->paused = NULL;
    ta_mem_free(it->system->allocator, saved,
                &(ta_allocator_free_params){ .size = saved->bytes, .alignment = TA_CONTINUATION_ALIGN });
}

/* Drops a restored pause without finishing it: its snapshots are released. */
static void ta_pause_drop(ta_interpreter *it) {
    while (it->exec_count > 0) {
        ta_exec_frame *frame = &it->exec_frames[--it->exec_count];
        if (frame->kind == TA_EXEC_FRAME_FOREACH) {
            ta_value_release(&frame->iteration_snapshot);
        }
    }
    it->at = (ta_micro_position){0};
}

/*
 * Runs or continues a microstep. A pause that cannot be saved does not
 * pause: the microstep runs one more unit and tries again (ADR 0061
 * section 3), so the budget never turns into an error.
 */
static ta_result ta_run_microstep(ta_interpreter *it) {
    for (;;) {
        ta_result result = ta_microstep(it);
        if (result != TA_EXEC_PAUSED) {
            return result;
        }
        if (ta_pause_save(it) == TA_OK) {
            return TA_EXEC_PAUSED;
        }
        it->action_budget = it->actions + 1u;
    }
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
    ta_timer_cancel_unsubmitted(&it->system->timers, ta_actor_timers(it->actor));
    ta_event event;
    while (ta_dequeue_internal(it->actor, &event)) {
        ta_event_release(&event);
    }
    if (it->actor->has_macrostep_event) {
        ta_event_release(&it->actor->macrostep_event);
        it->actor->has_macrostep_event = false;
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
        &it->history_seen, &it->history_busy, &it->final_states,
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
    it->entry_capacity = ta_entry_capacity(definition);
    it->entry_frames = ta_arena_alloc(&system->scratch, it->entry_capacity * sizeof(struct ta_entry_frame),
                                      _Alignof(struct ta_entry_frame));
    /* Each history state is expanded at most once. */
    it->history_stack = ta_arena_alloc(&system->scratch, (definition->history.slot_count + 1u) * sizeof(ta_state_id),
                                       _Alignof(ta_state_id));
    /* An open block per level, and a <foreach> frame under each body. */
    it->exec_capacity = 2u * definition->max_action_depth + 1u;
    it->exec_frames = ta_arena_alloc(&system->scratch, it->exec_capacity * sizeof(ta_exec_frame), _Alignof(ta_exec_frame));
    if (it->entry_frames == NULL || it->history_stack == NULL || it->exec_frames == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_bits_clear(it, it->history_busy);
    ta_load_active(it);
    if (actor->sparse != NULL && actor->sparse->paused != NULL) {
        ta_pause_restore(it); /* the live set, too, is the paused microstep's */
    }
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
    it->pausable = true;
    it->action_budget = params->action_budget;
    if (result == TA_OK && it->at.phase != TA_MICRO_NONE) {
        /* Continue the microstep a pause inside executable content left. The
         * actor changes again, so an image taken while paused is stale. */
        ta_actor_touch(actor);
        bool restore_internal = it->at.restore_internal;
        bool restore_external = it->at.restore_external;
        result = ta_run_microstep(it);
        if (result == TA_EXEC_PAUSED) {
            *status = TA_RUN_PAUSED;
            return TA_OK;
        }
        if (result == TA_ERR_NOMEM && (restore_internal || restore_external)) {
            ta_restore_current_event(actor, restore_external);
        }
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
            result = ta_run_microstep(it);
            if (result == TA_EXEC_PAUSED) {
                *status = TA_RUN_PAUSED;
                return TA_OK;
            }
            continue;
        }
        ta_event event;
        bool restore_internal = false;
        bool restore_external = false;
        if (ta_dequeue_internal(actor, &event)) {
            ta_set_current_event(actor, &event);
            restore_internal = true;
        } else if ((result = ta_invoke_start_pending(it)) != TA_OK) {
            break; /* Invoke: the macrostep ended, so start its states' invocations. */
        } else if (ta_interpreter_internal_pending(actor)) {
            continue;
        } else if (actor->has_external_event) {
            actor->has_external_event = false;
            ta_set_current_event(actor, &actor->external_event);
            restore_external = true;
            /* Invoke: <finalize> and autoforwarding. */
            result = ta_invoke_before_external(it);
            if (result != TA_OK) {
                break;
            }
        } else {
            ta_compact_sparse(it);
            if (actor->has_macrostep_event) {
                ta_event_release(&actor->macrostep_event);
                actor->has_macrostep_event = false;
            }
            actor->macrostep = false;
            actor->record.residency = TA_ACTOR_IDLE;
            *status = TA_RUN_STABLE;
            return TA_OK;
        }
        result = ta_select_transitions(it, &actor->current_event);
        if (result == TA_OK && it->enabled_count != 0) {
            it->at.restore_internal = restore_internal;
            it->at.restore_external = restore_external;
            result = ta_run_microstep(it);
            if (result == TA_EXEC_PAUSED) {
                *status = TA_RUN_PAUSED;
                return TA_OK;
            }
            if (result == TA_ERR_NOMEM && (restore_internal || restore_external)) {
                ta_restore_current_event(actor, restore_external);
            }
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
    /* An empty queue goes back; queued events or a paused microstep keep it. */
    ta_queue_settle(system, actor);
    return result;
}

/*
 * One session being exited by ta_interpreter_exit_invoked: its interpreter,
 * the scratch arena that interpreter lives in, the states it still has to
 * exit, and, while a child it invoked is being exited above it, the
 * invocation to cancel once that child is done.
 */
typedef struct {
    ta_actor *actor;
    ta_interpreter it;
    ta_arena arena;
    ta_state_id next;     /* states left to exit: those below `next`, highest first */
    ta_state_id exiting;  /* the state whose invocations are being cancelled */
    uint32_t cursor;      /* its invocations left: those below `cursor` */
    bool waiting;         /* `taken`'s child is being exited above this frame */
    ta_invoke_taken taken;
} ta_exit_frame;

/*
 * Starts exiting a session: a scratch arena of its own, so the interpreter
 * that cancelled it keeps its scratch, and an interpreter over it. Nothing
 * happens when the session is gone or finished, or when there is no memory
 * to exit it with.
 */
static void ta_exit_open(ta_system *system, ta_vec *frames, ta_actor_id id) {
    ta_actor *actor = ta_actor_lookup(system, id);
    if (actor == NULL || actor->finished) {
        return;
    }
    ta_exit_frame frame = { .actor = actor, .exiting = TA_STATE_NONE };
    if (ta_arena_init(&frame.arena, system->allocator, system->scratch.block_size) != TA_OK) {
        return;
    }
    ta_arena previous = system->scratch;
    system->scratch = frame.arena;
    ta_result result = ta_interpreter_init(&frame.it, system, actor);
    /* A session paused inside a microstep exits its live set; the rest of
     * the pause is dropped (ADR 0061 section 6). */
    ta_pause_drop(&frame.it);
    frame.arena = system->scratch;
    frame.next = actor->definition->state_count;
    if (result == TA_OK) {
        result = ta_vec_push(frames, &frame);
    }
    if (result != TA_OK) {
        actor->live_active = NULL;
        ta_arena_destroy(&frame.arena);
        system->scratch = previous;
    }
}

/*
 * One step of the session on top: exit its next state, cancel that state's
 * next invocation, or finish. `system->scratch` is the session's arena while
 * it runs; a step that keeps the session saves the arena back to its frame.
 */
static void ta_exit_step(ta_system *system, ta_vec *frames) {
    ta_exit_frame *frame = ta_vec_at(frames, frames->count - 1u);
    ta_interpreter *it = &frame->it;
    if (frame->waiting) {
        frame->waiting = false;
        ta_invoke_cancel_service(&frame->taken);
        frame->arena = system->scratch;
        return;
    }
    if (frame->exiting != TA_STATE_NONE) {
        if (!ta_invoke_take(frame->actor, frame->exiting, &frame->cursor, &frame->taken)) {
            ta_bits_put(it->active, frame->exiting, false);
            frame->exiting = TA_STATE_NONE;
        } else if (frame->taken.meta.has_child) {
            frame->waiting = true;
            frame->arena = system->scratch;
            ta_exit_open(system, frames, frame->taken.meta.child); /* may move `frame` */
            return;
        } else {
            ta_invoke_cancel_service(&frame->taken);
        }
        frame->arena = system->scratch;
        return;
    }
    while (frame->next > 0 && !ta_bits_get(it->active, frame->next - 1u)) {
        frame->next -= 1u;
    }
    if (frame->next == 0) {
        ta_store_active(it);
        frame->actor->live_active = NULL;
        ta_arena_destroy(&system->scratch);
        frames->count -= 1u;
        return;
    }
    ta_state_id state = --frame->next;
    ta_result result = ta_run_handlers(it, ta_state(it, state)->onexit_handlers);
    frame->arena = system->scratch;
    if (result != TA_OK) {
        ta_bits_put(it->active, state, false);
        frame->next = 0; /* exitInterpreter stops at a failure, as it did */
        return;
    }
    ta_invoke_forget_pending(frame->actor, state);
    frame->exiting = state;
    frame->cursor = UINT32_MAX;
}

TA_PRIVATE void ta_interpreter_exit_invoked(ta_system *system, ta_actor_id id) {
    /* Cancelling an invocation happens inside the parent's interpreter:
     * every exited session gets its own scratch, and the parent's comes back. */
    ta_arena outer = system->scratch;
    ta_vec frames;
    ta_vec_init(&frames, system->allocator, TA_VEC_LAYOUT(ta_exit_frame));
    ta_exit_open(system, &frames, id);
    while (frames.count != 0) {
        ta_exit_frame *frame = ta_vec_at(&frames, frames.count - 1u);
        system->scratch = frame->arena;
        ta_exit_step(system, &frames);
    }
    ta_vec_destroy(&frames);
    system->scratch = outer;
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
    return actor->internal_queue != NULL && actor->internal_queue->count != 0;
}

TA_PRIVATE void ta_interpreter_release_actor_state(ta_system *system, ta_actor *actor) {
    if (actor->has_external_event) {
        ta_event_release(&actor->external_event);
        actor->has_external_event = false;
    }
    if (actor->has_macrostep_event) {
        ta_event_release(&actor->macrostep_event);
        actor->has_macrostep_event = false;
    }
    if (actor->has_current_event) {
        ta_event_release(&actor->current_event);
        actor->has_current_event = false;
    }
    ta_event event;
    while (ta_dequeue_internal(actor, &event)) {
        ta_event_release(&event);
    }
    if (actor->sparse == NULL) {
        ta_queue_settle(system, actor);
        return;
    }
    ta_invoke_release(system, actor); /* Invoke: cancel services, free bookkeeping. */
    ta_continuation_free(system->allocator, &actor->sparse->paused);
    ta_queue_settle(system, actor); /* empty, and no longer paused */
    /* The owner heap must not keep pointing into the freed sparse state. */
    ta_timer_list_destroy(&system->timers, &actor->sparse->timers);
    ta_value_release(&actor->sparse->completion.data);
    ta_mem_free(system->allocator, actor->sparse,
                &(ta_allocator_free_params){ .size = sizeof(ta_actor_sparse_state),
                                             .alignment = _Alignof(ta_actor_sparse_state) });
    actor->sparse = NULL;
}
