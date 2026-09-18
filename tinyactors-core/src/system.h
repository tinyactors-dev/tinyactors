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
