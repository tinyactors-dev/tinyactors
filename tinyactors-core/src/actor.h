#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_definition_id definition; ta_str name; const ta_value *initial_data; ta_actor_id *out; } ta_actor_spawn_params;
typedef struct { ta_definition_id definition; ta_str name; ta_actor_id *out; } ta_actor_find_params;
typedef struct { ta_actor_id actor; uint32_t budget; ta_run_status *status; } ta_actor_resume_params;
typedef struct { ta_snapshot_reader reader; ta_actor_id *out; } ta_actor_recover_params;

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params);
ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params);
ta_result ta_actor_deliver(ta_system *system, ta_actor_id actor, const ta_event *event);
ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params);
ta_result ta_actor_unload(ta_system *system, ta_actor_id actor);
ta_result ta_actor_load(ta_system *system, ta_actor_id actor);
ta_result ta_actor_recover(ta_system *system, const ta_actor_recover_params *params);
ta_result ta_actor_destroy(ta_system *system, ta_actor_id actor);

#ifdef __cplusplus
}
#endif

#include "continuation.h"
#include "definition.h"
#include "invoke.h"

typedef enum {
    TA_ACTOR_UNLOADED,
    TA_ACTOR_IDLE,
    TA_ACTOR_RUNNING,
    TA_ACTOR_DONE
} ta_actor_residency;

typedef struct {
    ta_actor_id id;
    ta_definition_id definition;
    ta_atom name;
    ta_actor_residency residency;
    uint32_t generation;
} ta_actor_record;

typedef struct {
    ta_actor_record record;
    const ta_definition *definition;
    ta_configuration configuration;
    ta_history_state history;
    ta_continuation continuation;
    void *datamodel_instance;
    ta_queued_event *internal_events;
    uint32_t internal_count;
    uint32_t internal_capacity;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
} ta_actor;
