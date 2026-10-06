/**
 * @file actor.h
 *
 * # Resident actors
 *
 * A `ta_actor` is one resident session of a compiled definition: its
 * record (runtime ID, definition, self reference, residency), its session
 * storage (configuration, late-binding bits, history), its data-model
 * instance, and the state it needs only sometimes. The unit also holds the
 * system's actor table, which maps a runtime ID to its actor, the internal
 * queue an actor holds its internal and kept events in, and the operator
 * transition a delivered event may carry (ADR 0082). An actor's external
 * event is not here: it stays in the pool envelope of the input the actor
 * took (`run.h`), which the actor names by index.
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
 * - **The actor table** is an index pool of slots (`index_pool.h`) in
 *   fixed pages of 4,096 (`TA_ACTOR_PAGE_SHIFT`); pages never move, so a
 *   pointer to a slot stays valid for the life of the system. A runtime
 *   ID is a slot and a generation (`handle.h`), and slot `s` is pool index
 *   `s + 1`, so the first actor of a system is slot 0, generation 1,
 *   `0x100000000`. A freed slot's generation moves on, so an old ID finds
 *   nothing; the slot keeps its generation while free, which is why the
 *   table never trims its pool.
 * - **Slot order.** A spawn takes the slot freed last, else the lowest
 *   slot never issued, never scanning for a hole (invariant 17): the
 *   pool's free list gives exactly that order. `ta_actor_table_issued`
 *   bounds a walk over the table.
 * - **An operator transition** is one allocation: its header, then its
 *   targets (`ta_operator_jump_bytes`). The input's envelope owns it from
 *   delivery until the input completes.
 * - **The input.** While a macrostep runs, waits or is stopped, `input`
 *   names its taken envelope, by index, never by address; before the
 *   macrostep begins, the wake queued to start it. `external`,
 *   `bound`, `bound_before` and `input_flags` describe that input's events
 *   and fill padding, so an actor costs no more for them.
 * - Each semantic mutation advances the session epoch
 *   (`ta_actor_touch`); a session image records the epoch it was taken
 *   at, and unloading refuses an older one (ADR 0014).
 *
 * ## Memory
 *
 * The system owns its actors and the table's pages, from its allocator.
 * `ta_actor_new` allocates the block, and refills the table's pool by one
 * page when no slot is free, at the point where it allocates the block;
 * `ta_actor_free` releases what the actor holds (its sparse state, its
 * internal queue, its session entry, the input it took and the inputs
 * deferred for it, which go back to the run queue) and frees the block.
 * An actor's deferred list and its input are pool envelopes
 * (`envelope.h`): the actor owns neither, only the indices. The definition
 * outlives each of its actors: it counts them in `actor_count`. The
 * internal queue is the actor's from its first internal event until its
 * macrostep ends with nothing queued or kept (ADR 0069); then it becomes
 * the system's spare, which the next macrostep takes.
 * `ta_actor_layout`, `ta_operator_jump_bytes` and the event accessors
 * never allocate. Making an actor, delivering to it and storing its
 * delayed sends allocate, outside a macrostep or into room reserved before
 * it (ADR 0070).
 *
 * ## Complexity
 *
 * `ta_actor_lookup` and `ta_actor_slot_at` are O(1). `ta_actor_layout` and
 * `ta_operator_jump_bytes` are O(1). Issuing and freeing a slot are O(1);
 * issuing allocates a 64 KiB page once per 4,096 slots never issued
 * before, and zeroes it.
 *
 * ## Failure
 *
 * Allocation failure, the `maxActors` quota and a table that has issued
 * all `UINT32_MAX` slots answer `TA_ERR_NOMEM`, and change nothing. A
 * layout or an operator transition whose size would exceed `SIZE_MAX`
 * answers `TA_ERR_NOMEM` too, before anything is allocated.
 *
 * ## Users
 *
 * The system (spawn, delivery, unload and load), the run, the interpreter,
 * session images and invocations. ADRs 0014, 0063, 0066, 0069, 0070, 0082,
 * 0099.
 */
#pragma once

#include "continuation.h"
#include "definition.h"
#include "index_pool.h"
#include "list.h"
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
    /* The latest time an input admitted for it carries. Its clock moves
     * only when ta_run() takes an input, so admission checks the next
     * input's time against this as well (ADR 0094 section 1). */
    ta_time admitted;
    /* QUEUED records of the checkpoint the replay started from (ADR 0091)
     * still to hand back, although their seq is the checkpoint's. */
    uint32_t checkpoint_owed;
} ta_activation_effects;

/* Where an actor's external event is (ADR 0069). */
typedef enum {
    TA_EXTERNAL_NONE = 0,
    TA_EXTERNAL_WAITING, /* in its taken input, behind any internal events */
    TA_EXTERNAL_TAKEN    /* taken by the running macrostep, until the macrostep ends */
} ta_external_state;

/* What `_event` names (ADR 0069). */
typedef enum {
    TA_BOUND_NONE = 0,
    TA_BOUND_EXTERNAL, /* the external event, in the taken input's envelope */
    TA_BOUND_INTERNAL, /* the head of the internal queue, which stays queued while bound */
    TA_BOUND_KEPT      /* the kept event */
} ta_event_binding;

/* `ta_actor.input_flags`: what happened to the input its macrostep runs. */
/* Its external event enabled no transition (ADR 0089 section 8); read when
 * the macrostep ends, cleared with the event. */
#define TA_ACTOR_INPUT_IGNORED ((uint8_t)0x01u)
/* A fault stopped the macrostep: it waits for ta_actor_release(). */
#define TA_ACTOR_INPUT_STOPPED ((uint8_t)0x02u)
/* The fault was the microstep limit (ADR 0075). */
#define TA_ACTOR_INPUT_RUNAWAY ((uint8_t)0x04u)
/* Released after a runaway: the rest of the macrostep runs without the
 * microstep limit. */
#define TA_ACTOR_INPUT_UNLIMITED ((uint8_t)0x08u)
/* `input` is the wake queued to start the macrostep, not taken yet. */
#define TA_ACTOR_INPUT_WAITING ((uint8_t)0x10u)

/*
 * An operator transition delivered with the external event (ADR 0082), in
 * one allocation the input's envelope owns until the input completes. Its
 * targets are checked when the input is taken, against the configuration
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
 * An actor's internal queue (ADR 0069): its internal events, oldest first,
 * with the queue's capacity, and its kept event, in one allocation the
 * actor owns. An event leaves the queue only when `_event` moves on, and
 * it moves into `kept_event` when something may still read it:
 * - a taken queue head, when the next event is taken, so that a retry can
 *   bind it again (ADR 0047);
 * - the last event of an invoked session, when its macrostep ends, so that
 *   the session's <onexit> sees it if the parent cancels it (SCXML 6.4).
 * The actor takes one with its first internal event, or, for an invoked
 * session, when it takes an input: the system's spare when there is one,
 * else a new one. It gives it up when its macrostep ends with the queue
 * empty and no event kept, to be the system's spare or freed. An idle actor
 * that nobody invoked holds none.
 */
typedef struct ta_internal_queue {
    uint32_t count;
    uint32_t capacity;
    bool has_kept;
    /* The kept envelope that holds the kept event's name as text
     * (`ta_run_keep_input`), 0 when its name is interned. */
    uint32_t kept_input;
    ta_event kept_event;
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
    /* Where its input's external event is (ta_external_state), what
     * `_event` names (ta_event_binding), and what it named before the last
     * take, for a retry (ADR 0047). They fill padding. */
    uint8_t external;
    uint8_t bound;
    uint8_t bound_before;
    /* The pool envelope of the input its macrostep runs (run.h): taken from
     * the run queue when the macrostep began, and given back when its input
     * completes; with TA_ACTOR_INPUT_WAITING, the wake queued to start a
     * macrostep that has not begun; 0 for none. The external event, its
     * operator transition, the macrostep's trace parent and its logical
     * time are read from it. */
    uint32_t input;
    ta_actor_sparse_state *sparse;
    /* Its internal queue and kept event, or null. */
    ta_internal_queue *internal;
    /* Its inputs that ta_run() reached while it was in a macrostep, in
     * arrival order, linked through the pool's envelopes (run.h). */
    ta_list deferred;
    ta_actor_id replacement_of;
    /* The session this actor holds (ADR 0066), and its entry in the session
     * table when that ID is not this actor's runtime ID. */
    uint64_t session_id;
    ta_session_node *session_node;
    uint64_t session_epoch;
    bool published;
    /* TA_ACTOR_INPUT_*; fills padding. */
    uint8_t input_flags;
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

/*
 * One slot of the actor table, an item of its index pool. Slot `s` is pool
 * index `s + 1`, so the first actor's slot is 0 and its ID is
 * `ta_handle_make(0, 1)`. The slot keeps its generation while it is free:
 * the pool writes only `link`, and only while the slot is free.
 */
typedef struct {
    ta_actor *actor;
    /* 0 for a slot never taken, whose page refill just zeroed; taking it
     * makes it 1, so no actor ID has generation 0. */
    uint32_t generation;
    /* The pool's free-list link while the slot is free. Fits the padding
     * the generation already left, so the table costs no more per actor. */
    uint32_t link;
} ta_actor_slot;

/* Slots per page of the actor table, as a power of two: 4,096 slots, 64 KiB. */
#define TA_ACTOR_PAGE_SHIFT 12u

/*
 * The actor table (ADR 0063): an index pool of ta_actor_slot (index_pool.h)
 * in pages of 2^TA_ACTOR_PAGE_SHIFT, up to UINT32_MAX slots. It never
 * trims: a free slot keeps the generation of the IDs once issued there.
 */
typedef struct {
    ta_index_pool slots;
} ta_actor_table;

/**
 * Sets up an empty table over `allocator`. Allocates nothing; cannot fail
 * for the table's fixed parameters, and answers what `ta_index_pool_init`
 * answers.
 */
ta_result ta_actor_table_init(ta_actor_table *table, ta_allocator allocator);
/** Frees the table's pages and directory. The actors must be gone. Only frees. */
void ta_actor_table_destroy(ta_actor_table *table);
/**
 * Slots issued so far: every slot below it has been taken at least once,
 * and none at or above it has. Walks over the table stop here. O(1).
 */
static inline uint32_t ta_actor_table_issued(const ta_actor_table *table) { return table->slots.high_water; }
/**
 * The entry for `slot`, or NULL when that slot was never issued. The
 * entry never moves while the system lives. O(1); never allocates; may
 * run inside a macrostep.
 */
ta_actor_slot *ta_actor_slot_at(const ta_system *system, uint32_t slot);
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
/* Where mail for a destination goes; see ta_actor_mail_route. */
typedef struct {
    /* The actor that holds the target session: resident and not finished,
     * published or not (mail for one that is not published waits until it
     * is, run.h). Null when the mail waits for the host instead. */
    ta_actor *actor;
    /* With no actor: the destination to wait for, as a waiting entry keys
     * it (a session that is not resident resolved to its address). */
    ta_actor_ref destination;
    /* `destination` is a reference the route holds: release it with
     * ta_actor_ref_release() when done. */
    bool held;
    /* With no actor: what core knows, TA_ERR_UNLOADED, TA_ERR_DONE or
     * TA_ERR_NOT_FOUND (waiting.h). */
    ta_result hint;
    /* With no actor: no host can bring the target back, since it names no
     * address and no family (a runtime ID no session holds, from a
     * reference that names no family, ADR 0088). The send fails at once, in
     * the sending macrostep (SCXML 6.2.4), instead of waiting. */
    bool unreachable;
} ta_mail_route;
/*
 * Where mail to `destination` goes (ADR 0099): to the actor that holds the
 * session, when one is resident and has not finished; nowhere, when no host
 * could bring it back (`unreachable`); or to a waiting entry for the
 * destination, which asks the host. A runtime or session ID
 * is resolved as ta_session_route() does, a named address through the
 * host's directory. Never allocates; may run inside a macrostep.
 */
void ta_actor_mail_route(ta_system *system, const ta_actor_ref *destination, ta_mail_route *out);
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

/* The external event of the actor's input, waiting or taken, or null: read
 * from the input's envelope (run.h). The pointer is valid until the next
 * call that reads another input's event, or the input completes. */
const ta_event *ta_actor_external_event(ta_system *system, const ta_actor *actor);
/* The event `_event` names, or null (ADR 0069). The pointer is valid until
 * the internal queue grows, the event is released, or, for the external
 * event, as ta_actor_external_event says. */
const ta_event *ta_actor_bound_event(ta_system *system, const ta_actor *actor);
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
/* Whether `time` may be an input's time for `actor` (ADR 0094 section 1):
 * for a replaying actor, no earlier than its clock or than the last input
 * admitted for it, and no later than the system's. */
bool ta_actor_input_time_valid(const ta_system *system, const ta_actor *actor, ta_time time);
/* An input with a checked `time` was admitted for `actor`: the next one may
 * not be earlier. Nothing for an actor that is not replaying. */
void ta_actor_note_admitted(ta_actor *actor, ta_time time);
/* Moves a replaying actor's clock to `time`, dropping held delayed sends due
 * by then (ADR 0089 section 2); a time before its clock leaves it as it is,
 * and nothing happens for any other actor. */
void ta_actor_move_clock(ta_actor *actor, ta_time time);

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
