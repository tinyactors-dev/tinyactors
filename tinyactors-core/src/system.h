/**
 * @file system.h
 *
 * # The system
 *
 * `ta_system` is the in-process container for the runtime state every
 * actor shares: the injected allocator and the buffers each run needs,
 * the logical clock, atom and event-name interning, the registered
 * immutable definitions, the resident actors and their runtime IDs, the
 * address and session tables, delayed-send state, the pool of pending
 * inputs with its name area and macrostep slot (`run.h`, ADR 0099, ADR
 * 0103), the built-in SCXML Event I/O Processor, the waiting entries of
 * mail for targets that are not resident (`waiting.h`), the registered I/O processors and invokers, the host's actor directory
 * and log sink, and the control block with its `tel` ring (ADR 0098, ADR
 * 0104).
 *
 * ## What it is not
 *
 * The system creates no threads, decides no cross-actor scheduling order,
 * owns no socket or event loop, reads no wall-clock time, needs no
 * filesystem, and hides no host concurrency behind locks. It has no
 * persistence adapter and no concrete index of named addresses: the host
 * installs `ta_actor_directory` operations for claim, resolution,
 * replacement and removal, backed by whatever it likes (a hash table, a
 * database, a shard router). Core keeps only resident runtime state; the
 * host keeps the images of evicted actors for the life of the process and
 * indexes their deadlines. Eviction bounds memory; it is not persistence.
 *
 * ## Contract and invariants
 *
 * - **Directory entries** tell a resident runtime ID from an unloaded
 *   logical actor. The host's replace and remove callbacks receive the
 *   entry the caller expects and behave as atomic compare-and-swaps;
 *   replacement, unloading, loading and destruction rely on that, so a
 *   stale runtime actor never overwrites or removes a newer route.
 * - **Definitions** live in a slot table: a definition ID is a generation
 *   handle (`handle.h`) of its slot, and freed slots form a list through
 *   `next_free`, so registering never scans.
 * - **Actors** live in the actor table (`actor.h`), an index pool whose
 *   free list a spawn takes from, so a spawn costs the same against four
 *   million actors as against four (invariant 17).
 * - **Mail between sessions** never leaves core (`run.h`, ADR 0099): mail
 *   to a resident session goes into the pool, and mail to one that is not
 *   resident waits there for the host's answer to a load request. The
 *   SCXML I/O type atom is interned at creation, and registering an I/O
 *   processor cannot replace it.
 * - **Generated names:** every prefix of a name core generates is an
 *   atom from creation on: the segments of the built-in event names below
 *   and `invoke`. Matching an event compares its segments as atoms
 *   against a definition's descriptors (`definition.h`); a `<send>` or
 *   `<invoke>` `typeexpr` is looked up among the atoms, never interned,
 *   since every processor and invoker type is an atom already.
 * - The parameters (ADR 0092) are fixed at creation and bound every
 *   buffer; `maxActors` and `maxDefinitions` are counted against
 *   `live_actors` and `live_definitions`, and `poolEnvelopes`,
 *   `nameAreaBytes` and `admissionWatermark` bound the pool (`run.h`).
 *
 * ## Memory
 *
 * The system owns everything it holds, from the allocator in its config,
 * until `ta_system_destroy`, which destroys every actor and definition
 * first. Creation allocates the system, the first storage of the tables
 * that start with some (sessions, atoms, event names), the run
 * buffers (ADR 0092 section 3), and the `tel` ring with its scratch area
 * unless `telBytes` is 0; the actor table, the pool and the name area
 * start empty. Afterwards it allocates where a caller adds something (a
 * spawn, a registration, an interned address, an admitted input, which
 * may add a page to the pool or the name area), a nested interpreter's
 * region the first time an invocation is cancelled at that depth, and an
 * actor's internal queue; never in the middle of a macrostep, where room
 * is reserved before the run (ADR 0070).
 *
 * ## Complexity
 *
 * Looking up a definition or an actor by ID is O(1); so are the quota
 * checks. Teardown walks every slot ever issued.
 *
 * ## Failure
 *
 * Creation answers `TA_ERR_INVALID` for a config without an allocator,
 * the parameters' own answers, and `TA_ERR_NOMEM`, freeing what it made.
 * A full quota answers as an allocator that ran out (ADR 0092 section 4).
 *
 * ## Users
 *
 * Every unit that runs: the public API's entry points, the interpreter,
 * actors, session images, timers, invocations and I/O. ADRs 0043, 0063,
 * 0066, 0067, 0068, 0072, 0079, 0092, 0098, 0104.
 */
#pragma once

#include "actor.h"
#include "address.h"
#include "session.h"
#include "arena.h"
#include "atom.h"
#include "builder.h"
#include "trie.h"
#include "names.h"
#include "text_writer.h"
#include "io.h"
#include "io_origin.h"
#include "invoke.h"
#include "routing.h"
#include "deadline.h"
#include "trace.h"
#include "ring.h"
#include "run.h"
#include "vec.h"
#include "../include/tinyactors.h"
#include "clock.h"
#include "continuation.h"

/**
 * The event names system creation adds to `event_names`, in this order, so
 * their IDs are fixed: `error`, `error.execution`, `error.communication`,
 * `error.platform`, `done`, `done.state`, `done.invoke`,
 * `error.execution.capacity`, and below it `pool` and `names`, the
 * resources a `<send>` to a resident session can find without room
 * (ADR 0099, ADR 0102). Core names the events it raises below them. Creation also interns `invoke`, the third
 * segment of `error.communication.invoke.<id>.<reason>`, as
 * `invoke_segment`.
 */
enum {
    TA_EVENT_NAME_ERROR = 1u,
    TA_EVENT_NAME_ERROR_EXECUTION = 2u,
    TA_EVENT_NAME_ERROR_COMMUNICATION = 3u,
    TA_EVENT_NAME_ERROR_PLATFORM = 4u,
    TA_EVENT_NAME_DONE = 5u,
    TA_EVENT_NAME_DONE_STATE = 6u,
    TA_EVENT_NAME_DONE_INVOKE = 7u,
    TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY = 8u,
    TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_POOL = 9u,
    TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_NAMES = 10u,
    TA_EVENT_NAME_ERROR_EXECUTION_CAPACITY_INTERNAL = 11u
};

/** The ceiling of `eventNameDepth` (ADR 0092): the deepest a trie goes. */
#define TA_EVENT_NAME_MAX_DEPTH TA_TRIE_MAX_DEPTH

/*
 * A nested interpreter's memory (ADR 0092 section 3): its structural region
 * and its render area, one allocation made the first time an invocation is
 * cancelled at this depth, then kept for reuse until the system goes.
 */
typedef struct {
    void *memory;
    size_t bytes;
} ta_nested_region;

/**
 * The control block (ADR 0104): the rings core shares with its host, with
 * their positions (`head`, `tail`, the next seq) and counters. Today it
 * holds `tel` alone, a lossy ring of `telBytes` (ADR 0098), whose `dropped`
 * counts the trace records dropped since creation; its capacity is 0 when
 * `telBytes` is 0. The host reads it through `ta_system_tel_peek`,
 * `ta_system_tel_release` and `ta_system_tel_status`.
 */
typedef struct {
    ta_ring tel;
} ta_control_block;

typedef struct {
    ta_definition *definition;
    uint32_t generation;
    /* While free: the next free slot, or UINT32_MAX. */
    uint32_t next_free;
} ta_definition_slot;

struct ta_system {
    ta_allocator allocator;
    ta_clock clock;
    /* The first part of every session ID's text (ADR 0088). */
    uint64_t incarnation;
    ta_atom_table atoms;
    /* Interned named addresses (ADR 0065). */
    ta_address_table addresses;
    /* Reloaded and unloaded sessions, by session ID (ADR 0066). */
    ta_session_table sessions;
    /* Event names: a trie of atoms, holding the built-in names below. */
    ta_trie event_names;
    /* Resolves an event name kept as text into atoms for the matcher, with
     * `name_atoms` as its scratch (names.h); one serves the system, since
     * one macrostep runs at a time. */
    ta_names names;
    ta_atom name_atoms[TA_TRIE_MAX_DEPTH];

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
     * The actor table (actor.h); an actor ID is ta_actor_id_make(slot,
     * generation). A spawn takes the head of its pool's free list instead
     * of scanning the table, so it costs the same against four million
     * actors as against four (invariant 17).
     */
    ta_actor_table actors;

    /* Pending delayed sends (ADR 0101). */
    ta_deadline deadline;

    /* The I/O processor registry (ADR 0068): registry index i is element i - 1; 0 is SCXML. */
    ta_vec io_processors; /* ta_io_registration */
    /* Invokers, whose element index is their registry index. */
    ta_vec invokers;      /* ta_invoker_registration */
    /* Stored URIs of SCOPED and MESSAGE I/O origins (ADR 0068). */
    ta_io_origin_table io_scopes;
    ta_io_origin_table io_messages;

    ta_actor_directory directory;
    ta_log_sink log_sink;
    /* What is traced and how records are built (trace.h, ADR 0098). */
    ta_tracer trace;
    /* The rings and their counters (ADR 0104). */
    ta_control_block control;
    /* The pool of pending inputs, its name area and the macrostep slot
     * (run.h, ADRs 0099, 0103). */
    ta_run_state run;
    /* Never null: ta_null_definition_sink() when the host installed none (ADR 0079). */
    ta_definition_sink definition_sink;
    /* The trace-ID generator's state and the sampler's threshold (ADR 0073). */
    uint64_t trace_rng[4];
    uint64_t trace_threshold;
    /* Marks the bytes this system offloads (ADR 0074), drawn from the ID
     * generator at the first offload; 0 until then. */
    uint64_t offload_token;
    /* Every definition's counters, freed ones included (ADR 0072). */
    ta_counters counters;
    /* The system's parameters (ADR 0092), fixed at creation. */
    uint32_t params[TA_PARAM_COUNT];
    /* Live actors and definitions, for their quotas (ADR 0092 section 4). */
    uint32_t live_actors;
    uint32_t live_definitions;
    /*
     * The memory allocated at creation for the buffers every run needs (ADR
     * 0092 section 3): interpreter region 0 and its render area, and the
     * table of nested regions. `scratch` and `render` are arenas over it
     * that never grow.
     */
    void *arena_memory;
    size_t arena_bytes;
    ta_arena render;
    ta_nested_region *nested; /* interpreterNesting - 1 entries */
    /* The frames exiting invoked sessions use, interpreterNesting - 1 of
     * them, allocated with the nested regions; null until then. */
    void *exit_frames;
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    /* Invoke: the canonical SCXML invoker type. */
    ta_atom scxml_invoke_type;
    /* The `invoke` segment of error.communication.invoke.<id>.<reason>,
     * interned at creation with the built-in event names. */
    ta_atom invoke_segment;
    ta_arena scratch;
    /*
     * The internal queue the next macrostep that needs one takes (ADR
     * 0069): one an actor left empty, keeping no event, when its macrostep
     * ended, at its first capacity. One macrostep runs at a time, so one
     * spare serves them all, and a macrostep that runs executable content
     * allocates no queue of its own; an actor whose macrostep is stopped or
     * paused, or that keeps an event, keeps its own. Null when taken.
     */
    ta_internal_queue *spare_queue;
    /** How many queues `spare_queue` lists, at most TA_INTERNAL_QUEUE_SPARES. */
    uint32_t spare_queue_count;
};

/*
 * The registered definition behind a live ID, retired or not; NULL for an
 * unknown or stale ID.
 */
ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id);
/**
 * Allocates every nested interpreter region interpreterNesting allows, at
 * a safe point, so exiting invoked sessions inside a macrostep allocates
 * nothing (ADR 0102). Keeps what it had; TA_ERR_NOMEM leaves the regions
 * made so far.
 */
ta_result ta_system_nested_reserve(ta_system *system);

/*
 * A nested interpreter's two arenas (ADR 0092 section 3): region `depth`,
 * from 1, allocated the first time it is needed and kept. TA_ERR_UNSUPPORTED
 * past interpreterNesting, TA_ERR_NOMEM when it cannot be allocated.
 */
ta_result ta_system_nested_region(ta_system *system, uint32_t depth, ta_arena *scratch, ta_arena *render);
/* False, counting the refusal, when a new actor would pass maxActors. */
bool ta_system_quota_actor(ta_system *system);
/* Frees a registered definition and its slot if nothing holds or uses it,
 * and what that frees in turn (ADR 0077). */
void ta_system_settle_definition(ta_system *system, ta_definition *definition);
/* Tells the definition sink of one lifecycle event (ADR 0079). */
void ta_system_tell_definition(ta_system *system, const ta_definition *definition, ta_definition_event_kind kind);
/* The adapter registered for `type`, or null (not for the SCXML processor). */
const ta_io *ta_system_io_of(const ta_system *system, ta_atom type);
/* The registration at registry index `index`, or null; index 0 (SCXML) has none. */
const ta_io_registration *ta_system_io_at(const ta_system *system, uint32_t index);
/* The registry index of the invoker for `type`; UINT32_MAX when none. */
uint32_t ta_system_invoker_index(const ta_system *system, ta_atom type);
/* The type of the invoker at registry index `index`; TA_ATOM_NONE when none. */
ta_atom ta_system_invoker_type(const ta_system *system, uint32_t index);
bool ta_system_is_scxml_io(const ta_system *system, ta_atom type);

/*
 * The segments of `event`'s name as atoms, at most TA_TRIE_MAX_DEPTH of
 * them in `out`, and how many: its trie path, or, for a name its input
 * keeps as text (`TA_RUN_NAME_INPUT`, run.h), the text resolved by the
 * system's resolver, a segment
 * that is not an atom being TA_NAMES_UNKNOWN (names.h). 0 for no name, or
 * a text that is not a valid event name. Never allocates; may run inside a
 * macrostep.
 */
uint32_t ta_event_name_atoms(ta_system *system, const ta_event *event, ta_atom out[TA_TRIE_MAX_DEPTH]);

/* Puts `event`'s dotted name; false when it has none. Allocates nothing of
 * its own; may run inside a macrostep. */
bool ta_event_name_put(const ta_system *system, const ta_event *event, ta_text_writer *writer);

/*
 * Gives `event` a name ID for a record that holds names by ID and keeps the
 * event after its input completes: a delayed send (deadline.h). A name its
 * input keeps as text is interned into the event-name trie, and `name`
 * set. Nothing a host is handed goes through here (ADR 0100). May
 * allocate; TA_ERR_INVALID for a text that is not a valid event name,
 * TA_ERR_NOMEM or TA_ERR_FULL when the tables cannot grow, changing
 * nothing. An event that has an ID is left as it is.
 */
ta_result ta_event_name_store(ta_system *system, ta_event *event);

/*
 * Counts one occurrence for `definition` and for the system (ADR 0072). A
 * definition's counters are bookkeeping, not part of what it compiled to,
 * so they change through a definition core otherwise treats as read-only.
 */
#define TA_COUNT(system, definition, field)                                                                   \
    do {                                                                                                      \
        ((ta_definition *)(definition))->counters.field += 1u;                                               \
        (system)->counters.field += 1u;                                                                       \
    } while (0)
