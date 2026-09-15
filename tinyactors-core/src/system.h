#pragma once

#include "arena.h"
#include "actor.h"
#include "atom.h"
#include "builder.h"
#include "event_name.h"
#include "io.h"
#include "store.h"
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

    ta_store store;
    ta_arena scratch;
};

ta_result ta_system_register_definition(ta_system *system,
                                        ta_definition *definition,
                                        ta_definition_id *out);
ta_result ta_system_register_io(ta_system *system, ta_atom type, ta_io io);
ta_result ta_system_register_invoker(ta_system *system,
                                     ta_atom type,
                                     ta_invoker invoker);
ta_result ta_system_set_store(ta_system *system, ta_store store);
