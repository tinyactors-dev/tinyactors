#pragma once

#include "actor.h"
#include "address.h"
#include "session.h"
#include "arena.h"
#include "atom.h"
#include "builder.h"
#include "event_name.h"
#include "io.h"
#include "io_origin.h"
#include "invoke.h"
#include "ring.h"
#include "routing.h"
#include "timer.h"
#include "trace.h"

/*
 * A nested interpreter's memory (ADR 0092 section 3): its structural region
 * and its render area, one allocation made the first time an invocation is
 * cancelled at this depth, then kept for reuse until the system goes.
 */
typedef struct {
    void *memory;
    size_t bytes;
} ta_nested_region;

typedef struct {
    ta_definition *definition;
    uint32_t generation;
    /* The pool's free-list link: the next free slot's pool index (0 for none) while the
     * slot is free, zero while it is in use. */
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
    ta_event_name_table event_names;

    /*
     * ta_definition_slot; a definition ID is a handle (handle.h), as for
     * actors. An index pool (pool.h; lifetime: system): freed slots form a
     * free list through `next_free`, and a slot is recycled with a new
     * generation, never freed before the system's destroy.
     */
    ta_pool definitions;
    /* Compiled definitions still owned by callers, reclaimed at teardown. */
    ta_definition *unregistered_definitions;
    /* Orders registrations, so the newest identity match wins after slot reuse. */
    uint64_t next_definition_serial;

    /*
     * ta_actor_slot; an actor ID is a handle (handle.h). An index pool
     * (pool.h; lifetime: system): freed slots form a free list, so a spawn
     * takes the head instead of scanning the table and costs the same against
     * four million actors as against four (invariant 17).
     */
    ta_pool actors;

    ta_timer_heap timers;

    /* The I/O processor registry (ADR 0068): registry index i is element i - 1; 0 is SCXML. */
    ta_vec io_processors; /* ta_io_registration */
    /* Invokers, whose element index is their registry index. */
    ta_vec invokers;      /* ta_invoker_registration */
    /* Stored URIs of SCOPED and MESSAGE I/O origins (ADR 0068). */
    ta_io_origin_table io_scopes;
    ta_io_origin_table io_messages;

    ta_actor_directory directory;
    ta_external_event_router external_events;
    ta_log_sink log_sink;
    /* Never null: ta_null_trace_sink() when the host installed none (ADR 0067). */
    ta_trace_sink trace;
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
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    /* Invoke: the canonical SCXML invoker type. */
    ta_atom scxml_invoke_type;
    ta_arena scratch;
    /*
     * The rings (ADR 0098): the control block, the three ring areas (h2c and
     * c2h absent until ring-proto 5.1, tel of telBytes) and the text staging
     * area, one allocation made by ta_system_create (lifetime: system) and
     * freed, with the size and alignment kept here, by ta_system_destroy.
     * `rings` holds the handles over it and owns nothing; it is zeroed (and
     * `rings_memory` NULL) while no region exists. Nothing grows or moves.
     */
    ta_rings rings;
    void *rings_memory;
    size_t rings_bytes;
    size_t rings_alignment;
    /* maxRecordBytes of scratch for the texts of one tel record (lifetime of the contents: one write); inside rings_memory. */
    unsigned char *tel_staging;
    uint32_t tel_staging_bytes;
    /* Empty event blocks, lent to the next actor that needs one (ADR 0064),
     * newest first, linked through `next_cached`; at most `block_cache_limit`
     * of them (ADR 0069). Empty at creation: a block is made when a delivery
     * finds none, and kept when released while there is room (ADR 0093). */
    ta_event_block *block_cache;
    uint32_t block_cache_count;
    uint32_t block_cache_limit;
    /*
     * What a run that stops inside a phase takes (ADR 0070): a continuation
     * at least as large as the running definition needs, and a sparse state
     * for an actor that has none. ta_interpreter_resume reserves both before
     * it runs anything, so stopping never allocates. Null when taken.
     */
    ta_continuation *spare_continuation;
    ta_actor_sparse_state *spare_sparse;
};

/*
 * The registered definition behind a live ID, retired or not; NULL for an
 * unknown or stale ID.
 */
ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id);
/* ta_system_definition_slot returns the definition slot numbered `slot` (0-based), or NULL when it was never issued. Never allocates. */
ta_definition_slot *ta_system_definition_slot(const ta_system *system, uint32_t slot);
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
 * Counts one occurrence for `definition` and for the system (ADR 0072). A
 * definition's counters are bookkeeping, not part of what it compiled to,
 * so they change through a definition core otherwise treats as read-only.
 */
#define TA_COUNT(system, definition, field)                                                                   \
    do {                                                                                                      \
        ((ta_definition *)(definition))->counters.field += 1u;                                               \
        (system)->counters.field += 1u;                                                                       \
    } while (0)
