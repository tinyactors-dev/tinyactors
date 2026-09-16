#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { ta_actor_id actor; } ta_actor_directory_entry;
typedef struct { const ta_actor_address *address; ta_actor_directory_entry entry; } ta_actor_directory_claim_params;
typedef struct { const ta_actor_address *address; ta_actor_directory_entry *out; } ta_actor_directory_resolve_params;
typedef struct { const ta_actor_address *address; ta_actor_directory_entry replacement; } ta_actor_directory_replace_params;
typedef struct { const ta_actor_address *address; } ta_actor_directory_remove_params;
typedef struct ta_actor_directory_vtable {
    ta_result (*claim)(void *self, const ta_actor_directory_claim_params *params);
    ta_result (*resolve)(void *self, const ta_actor_directory_resolve_params *params);
    ta_result (*replace)(void *self, const ta_actor_directory_replace_params *params);
    ta_result (*remove)(void *self, const ta_actor_directory_remove_params *params);
} ta_actor_directory_vtable;
typedef struct { void *self; const ta_actor_directory_vtable *v; } ta_actor_directory;

typedef struct { ta_definition_id definition; const ta_actor_address *address; const ta_value *initial_data; ta_actor_id *out; } ta_actor_spawn_params;
typedef struct { const ta_actor_address *address; ta_actor_id *out; } ta_actor_find_params;
typedef struct { ta_actor_id actor; uint32_t budget; ta_run_status *status; } ta_actor_resume_params;
typedef struct { ta_snapshot_reader reader; const ta_actor_address *address; ta_actor_id *out; } ta_actor_recover_params;

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params);
ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params);
ta_result ta_actor_deliver(ta_system *system, ta_actor_id actor, const ta_event *event);
ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params);
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
    ta_actor_ref self;
    ta_actor_residency residency;
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
