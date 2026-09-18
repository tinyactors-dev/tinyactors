#pragma once

#include "actor.h"
#include "arena.h"
#include "atom.h"
#include "builder.h"
#include "event_name.h"
#include "io.h"
#include "invoke.h"
#include "routing.h"
#include "timer.h"

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

const ta_io *ta_system_find_io(const ta_system *system, ta_atom type);
bool ta_system_is_scxml_io(const ta_system *system, ta_atom type);
