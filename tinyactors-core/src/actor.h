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

typedef struct { ta_definition_id definition; const ta_actor_address *address; const ta_value *datamodel_binding; ta_actor_id *out; } ta_actor_spawn_params;
typedef struct { const ta_actor_address *address; ta_actor_id *out; } ta_actor_find_params;
typedef struct { ta_actor_id actor; uint32_t budget; ta_run_status *status; } ta_actor_resume_params;

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params);
ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params);
ta_result ta_actor_deliver(ta_system *system, ta_actor_id actor, const ta_event *event);
ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params);
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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A read-only operational view of one resident interpreter instance.  Core
 * pointers are borrowed and valid only while the actor is externally
 * synchronized against mutation.  When `has_datamodel_root` is true the root
 * is a host value returned by the data-model adapter; release it after use.
 */
typedef struct {
    ta_actor_id actor;
    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    const ta_configuration *configuration;
    const ta_history_state *history;
    const ta_continuation *continuation;
    const ta_queued_event *internal_events;
    uint32_t internal_count;
    const ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t delayed_send_count;
    bool has_datamodel_root;
    ta_value datamodel_root;
} ta_actor_inspection;

typedef ta_result (*ta_actor_inspection_visitor)(void *self,
                                                 const ta_actor_inspection *inspection);
typedef struct { void *self; ta_actor_inspection_visitor visit; } ta_actor_inspector;

ta_result ta_actor_inspect(ta_system *system,
                           ta_actor_id actor,
                           ta_actor_inspection *out);
ta_result ta_system_visit_actors(ta_system *system, ta_actor_inspector inspector);

#ifdef __cplusplus
}
#endif
