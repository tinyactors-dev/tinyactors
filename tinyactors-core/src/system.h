#pragma once

#include "types.h"
#include "actor.h"
#include "io.h"
#include "invoke.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_allocator allocator; ta_time initial_time; } ta_system_config;
typedef struct { ta_event_name_id name; char *buffer; uint32_t capacity; uint32_t *needed; } ta_event_name_format_params;

ta_result ta_system_create(const ta_system_config *config, ta_system **out);
void ta_system_destroy(ta_system *system);
ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory);
ta_result ta_system_intern_atom(ta_system *system, ta_str text, ta_atom *out);
ta_result ta_event_name_intern(ta_system *system, const ta_atom_list *segments, ta_event_name_id *out);
bool ta_event_name_matches(const ta_system *system, ta_event_name_id event, ta_event_name_id prefix);
ta_result ta_event_name_format(const ta_system *system, const ta_event_name_format_params *params);
ta_result ta_system_set_time(ta_system *system, ta_time now);
ta_time ta_system_time(const ta_system *system);

#ifdef __cplusplus
}
#endif

#include "arena.h"
#include "actor.h"
#include "atom.h"
#include "builder.h"
#include "event_name.h"
#include "io.h"
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
    ta_arena scratch;
};

ta_result ta_system_register_definition(ta_system *system,
                                        ta_definition *definition,
                                        ta_definition_id *out);
ta_result ta_system_register_io(ta_system *system, ta_atom type, ta_io io);
ta_result ta_system_register_invoker(ta_system *system,
                                     ta_atom type,
                                     ta_invoker invoker);
ta_result ta_system_set_actor_directory(ta_system *system, ta_actor_directory directory);
