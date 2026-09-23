#pragma once

#include "actor.h"
#include "address.h"
#include "arena.h"
#include "atom.h"
#include "builder.h"
#include "event_name.h"
#include "io.h"
#include "invoke.h"
#include "routing.h"
#include "timer.h"

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
ta_definition *ta_system_definition(const ta_system *system, ta_definition_id id);
/* Frees a registered definition and its slot; no actor may use it. */
void ta_system_free_definition(ta_system *system, ta_definition *definition);
const ta_io *ta_system_find_io(const ta_system *system, ta_atom type);
bool ta_system_is_scxml_io(const ta_system *system, ta_atom type);
