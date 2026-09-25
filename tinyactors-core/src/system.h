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
#include "routing.h"
#include "timer.h"
#include "trace.h"

/* How many empty event blocks a new system keeps for reuse (ADR 0064): one
 * per interpreter that can run at once, nested exits included (ADR 0060).
 * ta_system_set_event_block_cache() changes it (ADR 0069). */
#define TA_EVENT_BLOCK_CACHE_DEFAULT 4u

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
    /* Reloaded and unloaded sessions, by session ID (ADR 0066). */
    ta_session_table sessions;
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
    /* The trace-ID generator's state and the sampler's threshold (ADR 0073). */
    uint64_t trace_rng[4];
    uint64_t trace_threshold;
    /* Marks the bytes this system offloads (ADR 0074), drawn from the ID
     * generator at the first offload; 0 until then. */
    uint64_t offload_token;
    /* Every definition's counters, freed ones included (ADR 0072). */
    ta_counters counters;
    /* Microsteps one macrostep may run, for definitions that set none (ADR
     * 0075); 0 for no limit. */
    uint32_t microstep_limit;
    ta_atom scxml_io_type;
    /* The short form "scxml" also selects the built-in processor. */
    ta_atom scxml_io_alias;
    /* Invoke: the canonical SCXML invoker type and the generated-ID counter. */
    ta_atom scxml_invoke_type;
    uint64_t next_invoke_serial;
    ta_arena scratch;
    /* Empty event blocks, lent to the next actor that needs one (ADR 0064),
     * newest first, linked through `next_cached`; at most `block_cache_limit`
     * of them (ADR 0069). */
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
/* Frees a registered definition and its slot; no actor may use it. */
void ta_system_free_definition(ta_system *system, ta_definition *definition);
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
