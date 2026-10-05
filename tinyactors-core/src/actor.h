/**
 * @file actor.h
 *
 * # Resident actors
 *
 * A `ta_actor` is one resident session of a compiled definition: its
 * record (runtime ID, definition, self reference, residency), its session
 * storage (configuration, late-binding bits, history), its data-model
 * instance, and the state it needs only sometimes. The unit also holds the
 * system's actor table, which maps a runtime ID to its actor, the event
 * slots and event block an actor holds its events in, and the operator
 * transition a delivered event may carry (ADR 0082).
 *
 * ## Contract and invariants
 *
 * - **One allocation per actor** (ADR 0063). An actor's block holds, in
 *   order, the `ta_actor` record, its session storage and its data-model
 *   instance, each at its alignment. `ta_actor_layout` lays the block out
 *   once per definition, when it is compiled, with the layout builder
 *   (`layout.h`), and the definition keeps the result in
 *   `actor_block_bytes`, `actor_block_alignment`, `actor_session_offset`
 *   and `actor_instance_offset`. The block ends with the instance; an
 *   instance of size 0 (the null data model) points at the block's end.
 * - **Session storage** is sized from the definition
 *   (`ta_definition_session_bytes`): the configuration's words, then the
 *   late-binding bits (sized like the configuration), then the history
 *   slots. It starts aligned for `uint64_t` and is zeroed when the actor
 *   is made. Uncommon state (invocations, a parent, completion data,
 *   delayed sends, a paused continuation) lives in a sparse state
 *   allocated only for an actor that uses it, so an ordinary actor costs
 *   its block and its slot.
 * - **The actor table** issues slots in fixed pages of
 *   `TA_ACTOR_PAGE_SLOTS`; pages never move, so a pointer to a slot stays
 *   valid for the life of the system. A runtime ID is a slot and a
 *   generation; a freed slot's generation moves on, so an old ID finds
 *   nothing.
 * - **An operator transition** is one allocation: its header, then its
 *   targets (`ta_operator_jump_bytes`). The event block owns it from
 *   delivery until the macrostep that takes the event frees it.
 * - Each semantic mutation advances the session epoch
 *   (`ta_actor_touch`); a session image records the epoch it was taken
 *   at, and unloading refuses an older one (ADR 0014).
 *
 * ## Memory
 *
 * The system owns its actors and the table's pages, from its allocator.
 * `ta_actor_new` allocates the block (and may allocate a table page);
 * `ta_actor_free` releases what the actor holds (its sparse state, its
 * event block, its session entry) and frees the block.
 * The definition outlives each of its actors: it counts them in
 * `actor_count`. The event block is lent to an actor from delivery or its
 * first internal event until its macrostep ends (ADR 0064, ADR 0069).
 * `ta_actor_layout` and `ta_operator_jump_bytes` never allocate. Making an
 * actor, delivering to it and storing its delayed sends allocate, outside
 * a macrostep or into room reserved before it (ADR 0070).
 *
 * ## Complexity
 *
 * `ta_actor_lookup` and `ta_actor_slot_at` are O(1). `ta_actor_layout` and
 * `ta_operator_jump_bytes` are O(1). Issuing a slot is O(1), and
 * allocates a page once per `TA_ACTOR_PAGE_SLOTS` slots.
 *
 * ## Failure
 *
 * Allocation failure and the `maxActors` quota answer `TA_ERR_NOMEM`, and
 * change nothing. A layout or an operator transition whose size would
 * exceed `SIZE_MAX` answers `TA_ERR_NOMEM` too, before anything is
 * allocated.
 *
 * ## Users
 *
 * The system (spawn, delivery, unload and load), the interpreter, session
 * images and invocations. ADRs 0014, 0063, 0064, 0066, 0069, 0070, 0082.
 */
#pragma once

#include "continuation.h"
#include "definition.h"
#include "invoke.h"
#include "timer.h"
#include "session.h"
#include "vec.h"
#include "../include/tinyactors.h"
#include "configuration.h"
#include "history.h"
#include "datamodel.h"

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
    /* A replaying actor (ADR 0089): it crosses no border until published,
     * and `clock` is its own logical time, moved by each input's time (ADR 0094). */
    bool replaying;
    ta_time clock;
    /* QUEUED records of the checkpoint the replay started from (ADR 0091)
     * still to hand back, although their seq is the checkpoint's. */
    uint32_t checkpoint_owed;
} ta_activation_effects;

/* Where an actor's external event is (ADR 0069). */
typedef enum {
    TA_EXTERNAL_NONE = 0,
    TA_EXTERNAL_WAITING, /* delivered, behind any internal events */
    TA_EXTERNAL_TAKEN    /* taken by the running macrostep, kept until it ends */
} ta_external_state;

/* What `_event` names (ADR 0069). */
typedef enum {
    TA_BOUND_NONE = 0,
    TA_BOUND_EXTERNAL, /* the external event */
    TA_BOUND_INTERNAL, /* the head of the internal queue, which stays queued while bound */
    TA_BOUND_KEPT      /* the kept event */
} ta_event_binding;

/*
 * An operator transition delivered with the external event (ADR 0082), in
 * one allocation the event block owns until the macrostep takes the event.
 * Its targets are checked when it is delivered, against the configuration
 * it will run from: the event it came with is the first step taken.
 */
typedef struct ta_operator_jump {
    ta_state_id source; /* TA_STATE_NONE: the root */
    uint32_t target_count;
    ta_state_id targets[];
} ta_operator_jump;

/**
 * The bytes an operator transition with `count` targets takes, its header
 * then its targets, in `*out`. Never allocates; may run anywhere. Answers
 * TA_ERR_NOMEM, leaving `*out` alone, when they would exceed SIZE_MAX.
 * The count of a transition that was allocated was sized before, so for
 * one that exists it cannot fail.
 */
ta_result ta_operator_jump_bytes(uint32_t count, size_t *out);

/*
 * An actor's events other than its queued internal ones (ADR 0069). Taking
 * an event does not move it: `bound` names where it is. An event leaves
 * only when `_event` moves on, and it moves into `kept_event` when
 * something may still read it:
 * - a taken queue head, when the next event is taken, so that a retry can
 *   bind it again;
 * - the last event of an invoked session, when its macrostep ends, so that
 *   the session's <onexit> sees it if the parent cancels it (SCXML 6.4).
 */
typedef struct {
    uint8_t external;     /* ta_external_state */
    uint8_t bound;        /* ta_event_binding */
    uint8_t bound_before; /* what `bound` was before the last take, for a retry (ADR 0047) */
    bool has_kept;
    /* The external event enabled no transition (ADR 0089 section 8); read
     * when its macrostep ends, cleared with the event. Fills padding. */
    bool external_ignored;
    /* `time` holds the running macrostep's logical time (ADR 0089 section
     * 9), taken by the first ta_actor_resume() of the macrostep. */
    bool timed;
    ta_time time;
    /* The external event of the running macrostep. Inspection reports it
     * while the macrostep runs (ADR 0047). */
    ta_event external_event;
    ta_event kept_event;
    /* The operator transition that takes the waiting external event, or
     * null (ADR 0082). */
    ta_operator_jump *jump;
    /*
     * What the running macrostep runs under (ADR 0067): its delivery's, its
     * spawn's, or the failed send's that queued its first event. Set when a
     * macrostep starts, and kept while it runs, so a block that holds only a
     * parent stays with its actor until the macrostep ends.
     */
    ta_trace_parent trace;
} ta_event_slots;

/*
 * An actor's event block (ADR 0064, ADR 0069): its events and its internal
 * queue, oldest first, with the queue's capacity, in one allocation. It is
 * lent to an actor from ta_actor_deliver(), or the first internal event,
 * until the macrostep ends; an actor keeps it longer only while events are
 * queued, a microstep is paused, or it keeps an event. An idle actor that
 * nobody invoked holds none, so it carries no event.
 */
typedef struct ta_event_block {
    uint32_t count;
    uint32_t capacity; /* 0 for a header only, which grows with the first internal event */
    /* While the block is in the system's cache, the next cached block. */
    struct ta_event_block *next_cached;
    ta_event_slots slots;
    ta_queued_event events[];
} ta_event_block;

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
    /* Its event block (ADR 0064), or null. */
    ta_event_block *event_block;
    ta_actor_id replacement_of;
    /* The session this actor holds (ADR 0066), and its entry in the session
     * table when that ID is not this actor's runtime ID. */
    uint64_t session_id;
    ta_session_node *session_node;
    uint64_t session_epoch;
    bool published;
    /* Lifecycle records written for this session, from 1 (ADR 0067; napi
     * ADR 0016): a lifecycle record's key is (session, 0, this count), so
     * each has its own span ID. Sits in the padding after `published`. */
    uint32_t trace_lifecycle;
    ta_dm_host_impl host_impl;
    ta_dm_host host;

    /* Execution state between microsteps; a macrostep resumes from here. */
    bool bootstrapped;
    bool macrostep;
    bool finished;
    /* The running macrostep has written MACROSTEP_BEGIN (ADR 0067): taking
     * its event can no longer be undone, and its key is `trace_macrostep`. */
    bool trace_begun;
    /* The step counters (ADR 0067 section 3): the current or last macrostep,
     * from 1, and the current or last microstep within it, from 1. They run
     * whether or not anything traces, and move only when a step commits. */
    uint32_t trace_microstep;
    uint64_t trace_macrostep;
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
ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot);
void ta_actor_table_destroy(ta_actor_table *table, ta_allocator allocator);
/**
 * Lays out one actor of `definition` as a single allocation (ADR 0063): the
 * ta_actor record, then the session storage, then the data-model instance,
 * each at its alignment, into the definition's `actor_*` fields. Called
 * once, when the definition is compiled, after its configuration, history
 * and data-model instance are sized; never allocates. Answers
 * TA_ERR_NOMEM when the block would exceed SIZE_MAX and TA_ERR_INVALID for
 * an instance alignment of 0, and then leaves the fields alone.
 */
ta_result ta_actor_layout(ta_definition *definition);

ta_actor *ta_actor_lookup(const ta_system *system, ta_actor_id id);
/* The family atom of the resident actor `id`, or 0 when there is none (ADR 0088). */
ta_atom ta_actor_family_of(const ta_system *system, ta_actor_id id);
/* A runtime reference to `id` that names its family (ADR 0088). */
ta_actor_ref ta_actor_runtime_ref(const ta_system *system, ta_actor_id id);
bool ta_actor_state_active(const ta_actor *actor, ta_state_id state);
/* Runs an invoked child's exitInterpreter path with isolated scratch storage. */
void ta_actor_exit_invoked(ta_system *system, ta_actor_id id);

/*
 * A blank, unbootstrapped actor of `definition` with its slot claimed; the
 * data-model instance is allocated but not initialized. ta_actor_spawn()
 * bootstraps it, ta_actor_reconstruct() fills it from a session image.
 */
ta_result ta_actor_new(ta_system *system, ta_definition *definition, ta_actor **out);
/* Tears the actor down completely and uncounts it against its definition. */
void ta_actor_free(ta_system *system, ta_actor *actor);
/* Frees the slot an actor was given, so a failed construction reuses it. */
void ta_actor_release_slot(ta_system *system, ta_actor_id id);

/* The host directory's compare-and-swaps, which every residency change uses. */
ta_result ta_actor_directory_swap(ta_system *system, const ta_actor_directory_replace_params *params);
ta_result ta_actor_directory_drop(ta_system *system, const ta_actor_directory_remove_params *params);

/* The external event the actor holds, waiting or taken, or null. */
const ta_event *ta_actor_external_event(const ta_actor *actor);
/* The event `_event` names, or null (ADR 0069). The pointer is valid until
 * the actor's event block grows or the event is released. */
const ta_event *ta_actor_bound_event(const ta_actor *actor);
/* Internal events waiting to be taken: the queue without a taken head. */
uint32_t ta_actor_waiting_internal(const ta_actor *actor);

/* The actor's delayed sends, or null when it has no sparse state (so none). */
ta_timer_list *ta_actor_timers(const ta_actor *actor);
/* Stores a delayed send with its owner; takes the send's values on TA_OK. */
ta_result ta_actor_schedule_send(ta_system *system, ta_actor *actor, const ta_delayed_send *send);
/* Makes room to store one more delayed send, so that storing it cannot fail
 * (ADR 0070). */
ta_result ta_actor_reserve_send(ta_system *system, ta_actor *actor);
/* The list a send owned by `owner` belongs in: the actor's, or the orphans when it is gone. */
ta_result ta_actor_timer_home(ta_system *system, ta_actor_id owner, ta_timer_list **out);

/* True for a replaying actor (ADR 0089): prepared with `at`, not yet published. */
bool ta_actor_replaying(const ta_actor *actor);
/* The time the actor lives at: a replaying actor's own clock, otherwise the
 * system's (ADR 0089 section 2). */
ta_time ta_actor_now(const ta_system *system, const ta_actor *actor);
/* Whether `time` may be an input's time for `actor` (ADR 0094 section 1). */
bool ta_actor_input_time_valid(const ta_system *system, const ta_actor *actor, ta_time time);
/* Moves a replaying actor's clock to a checked `time`, dropping held delayed
 * sends due by then (ADR 0089 section 2); nothing for any other actor. */
void ta_actor_move_clock(ta_actor *actor, ta_time time);
/* A replaying actor's clock moved by a record that is not an input (ADR 0090
 * section 7's reducer): TA_ERR_INVALID, TA_ERR_BUSY or TA_ERR_NOT_FOUND as
 * an input's time would be refused. */

/* Materialized effects of an unpublished replacement's bootstrap. */
ta_result ta_actor_hold_send(ta_system *system, ta_actor *actor, const ta_activation_send *send);
ta_result ta_actor_hold_log(ta_system *system, ta_actor *actor, const ta_log_record *record);
bool ta_actor_cancel_held_send(ta_actor *actor, ta_send_id send_id);

/*
 * Advances the session epoch: the actor has changed in a way a session image
 * captured before the change no longer describes. ta_actor_unload() compares
 * epochs to refuse a stale image (ADR 0014), so this marks every semantic
 * mutation and nothing else — a read, an inspection, or a capture must not
 * move it.
 */
static inline void ta_actor_touch(ta_actor *actor) { actor->session_epoch += 1u; }
