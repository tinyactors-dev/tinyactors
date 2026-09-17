#pragma once

#include "event.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TA_ACTOR_DIRECTORY_NONE = 0,
    TA_ACTOR_DIRECTORY_RESIDENT,
    TA_ACTOR_DIRECTORY_UNLOADED
} ta_actor_directory_entry_kind;

typedef struct {
    ta_actor_directory_entry_kind kind;
    /* Meaningful only for TA_ACTOR_DIRECTORY_RESIDENT. */
    ta_actor_id actor;
} ta_actor_directory_entry;
typedef struct { const ta_actor_address *address; ta_actor_directory_entry entry; } ta_actor_directory_claim_params;
typedef struct { const ta_actor_address *address; ta_actor_directory_entry *out; } ta_actor_directory_resolve_params;
typedef struct {
    const ta_actor_address *address;
    ta_actor_directory_entry expected;
    ta_actor_directory_entry replacement;
} ta_actor_directory_replace_params;
typedef struct {
    const ta_actor_address *address;
    ta_actor_directory_entry expected;
} ta_actor_directory_remove_params;
typedef struct ta_actor_directory_vtable {
    /* claim succeeds only when the address has no entry. */
    ta_result (*claim)(void *self, const ta_actor_directory_claim_params *params);
    ta_result (*resolve)(void *self, const ta_actor_directory_resolve_params *params);
    /* replace/remove are atomic comparisons against params->expected. */
    ta_result (*replace)(void *self, const ta_actor_directory_replace_params *params);
    ta_result (*remove)(void *self, const ta_actor_directory_remove_params *params);
} ta_actor_directory_vtable;
typedef struct { void *self; const ta_actor_directory_vtable *v; } ta_actor_directory;

typedef struct {
    ta_actor_id actor;
    ta_invoke_id invoke_id;
} ta_actor_parent;

typedef struct {
    ta_definition_id definition;
    const ta_actor_address *address;
    const ta_value *datamodel_binding;
    const ta_actor_parent *parent;
    ta_actor_id *out;
} ta_actor_spawn_params;
typedef struct { const ta_actor_address *address; ta_actor_id *out; } ta_actor_find_params;
typedef struct { ta_actor_id actor; uint32_t budget; ta_run_status *status; } ta_actor_resume_params;
typedef struct {
    ta_actor_id actor;
    const ta_session_image *image;
} ta_actor_unload_params;
typedef struct { const ta_actor_address *address; } ta_actor_remove_unloaded_params;
typedef struct {
    const ta_session_image *image;
    const ta_value *datamodel_binding;
    ta_actor_id *out;
} ta_actor_load_params;
typedef struct {
    ta_actor_id incumbent;
    ta_definition_id definition;
    const ta_value *datamodel_binding;
    ta_actor_id *out;
} ta_actor_prepare_replacement_params;
typedef struct { ta_actor_id incumbent; ta_actor_id replacement; } ta_actor_activate_replacement_params;
/* Release data with ta_value_release() when has_data is true. */
typedef struct { bool has_data; ta_value data; } ta_actor_completion;

ta_result ta_actor_spawn(ta_system *system, const ta_actor_spawn_params *params);
/* Creates an unpublished actor and holds bootstrap effects until activation. */
ta_result ta_actor_prepare_replacement(ta_system *system,
                                       const ta_actor_prepare_replacement_params *params);
/* Atomically replaces the route, then releases held effects in SCXML order. */
ta_result ta_actor_activate_replacement(ta_system *system,
                                        const ta_actor_activate_replacement_params *params);
ta_result ta_actor_find(ta_system *system, const ta_actor_find_params *params);
ta_result ta_actor_deliver(ta_system *system, ta_actor_id actor, const ta_event *event);
ta_result ta_actor_resume(ta_system *system, const ta_actor_resume_params *params);
/*
 * Evicts a named actor without terminating it. The image must be a fresh
 * capture of actor; TA_ERR_STALE means the actor changed after capture.
 */
ta_result ta_actor_unload(ta_system *system, const ta_actor_unload_params *params);
/* Restores an unloaded named actor and atomically republishes its route. */
ta_result ta_actor_load(ta_system *system, const ta_actor_load_params *params);
/* Releases the name of an unloaded actor; the host then deletes its checkpoint. */
ta_result ta_actor_remove_unloaded(ta_system *system,
                                   const ta_actor_remove_unloaded_params *params);
ta_result ta_actor_get_completion(ta_system *system,
                                  ta_actor_id actor,
                                  ta_actor_completion *out);
/*
 * Retries an invoked child's completion after TA_ERR_BUSY. TA_ERR_NOT_FOUND
 * retires it when the parent invocation was already cancelled.
 */
ta_result ta_actor_publish_completion(ta_system *system, ta_actor_id actor);
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

typedef struct ta_activation_effects ta_activation_effects;

/* Allocated only for actors that use one of these uncommon facilities. */
typedef struct {
    ta_queued_event *internal_events;
    uint32_t internal_count;
    uint32_t internal_capacity;
    ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t invoke_capacity;
    ta_activation_effects *activation_effects;
    ta_actor_parent parent;
    ta_actor_completion completion;
    bool has_parent;
    bool completion_pending;
} ta_actor_sparse_state;

typedef struct {
    ta_actor_record record;
    const ta_definition *definition;
    /* One definition-sized allocation backs configuration and history. */
    void *session_storage;
    ta_configuration configuration;
    ta_history_state history;
    /* Null while stable; allocated only for an in-progress macrostep. */
    ta_continuation *continuation;
    void *datamodel_instance;
    ta_actor_sparse_state *sparse;
    ta_actor_id replacement_of;
    uint64_t session_epoch;
    bool published;
} ta_actor;

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A read-only operational view of one resident interpreter instance.  Core
 * pointers are borrowed and valid only while the actor is externally
 * synchronized against mutation.  When `has_datamodel_root` is true the root
 * is a host value returned by the data-model adapter; release it after use.
 * The completion pointer and its data are borrowed and must not be released.
 */
typedef struct {
    ta_actor_id actor;
    ta_actor_ref self;
    ta_definition_identity definition;
    ta_actor_residency residency;
    bool published;
    const ta_configuration *configuration;
    const ta_history_state *history;
    const ta_continuation *continuation;
    const ta_queued_event *internal_events;
    uint32_t internal_count;
    const ta_active_invoke *invokes;
    uint32_t invoke_count;
    uint32_t delayed_send_count;
    bool has_completion;
    const ta_actor_completion *completion;
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
