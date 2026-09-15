#pragma once

#include "event.h"
#include "types.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct { const ta_definition *definition; ta_allocator allocator; void **shared; } ta_datamodel_prepare_definition_params;
typedef struct { void *shared; void *instance; const ta_dm_host *host; } ta_datamodel_init_instance_params;
typedef struct { void *shared; void *instance; ta_expr_id expr; const ta_dm_host *host; bool *out; ta_fault *fault; } ta_datamodel_eval_condition_params;
typedef struct { void *shared; void *instance; ta_expr_id expr; const ta_dm_host *host; ta_value *out; ta_fault *fault; } ta_datamodel_eval_value_params;
typedef struct { void *shared; void *instance; ta_expr_id location; const ta_dm_host *host; ta_value *out; ta_fault *fault; } ta_datamodel_read_location_params;
typedef struct { void *shared; void *instance; ta_expr_id location; const ta_value *value; const ta_dm_host *host; ta_fault *fault; } ta_datamodel_assign_params;
typedef struct { void *shared; void *instance; ta_expr_id script; const ta_dm_host *host; ta_fault *fault; } ta_datamodel_execute_script_params;
typedef struct { void *shared; void *instance; ta_snapshot_writer *writer; } ta_datamodel_snapshot_params;
typedef struct { void *shared; void *instance; ta_snapshot_reader *reader; } ta_datamodel_restore_params;
typedef struct ta_datamodel_vtable {
    ta_result (*prepare_definition)(void *self, const ta_datamodel_prepare_definition_params *params);
    void (*destroy_definition)(void *self, void *shared, ta_allocator allocator);
    uint32_t (*instance_size)(void *self, void *shared);
    uint32_t (*instance_alignment)(void *self, void *shared);
    ta_result (*init_instance)(void *self, const ta_datamodel_init_instance_params *params);
    void (*destroy_instance)(void *self, void *shared, void *instance);
    ta_op_status (*eval_condition)(void *self, const ta_datamodel_eval_condition_params *params);
    ta_op_status (*eval_value)(void *self, const ta_datamodel_eval_value_params *params);
    ta_op_status (*read_location)(void *self, const ta_datamodel_read_location_params *params);
    ta_op_status (*assign)(void *self, const ta_datamodel_assign_params *params);
    ta_op_status (*execute_script)(void *self, const ta_datamodel_execute_script_params *params);
    ta_result (*snapshot)(void *self, const ta_datamodel_snapshot_params *params);
    ta_result (*restore)(void *self, const ta_datamodel_restore_params *params);
} ta_datamodel_vtable;
typedef struct { void *self; const ta_datamodel_vtable *v; } ta_datamodel;

#ifdef __cplusplus
}
#endif

#include "configuration.h"
#include "fault.h"

typedef struct {
    ta_system *system;
    ta_actor_id actor;
    const ta_event *current_event;
    const ta_configuration *configuration;
} ta_dm_host_impl;

struct ta_dm_host {
    const ta_dm_host_impl *impl;
};

bool ta_dm_host_in_state(const ta_dm_host *host, ta_state_id state);
const ta_event *ta_dm_host_event(const ta_dm_host *host);
ta_actor_id ta_dm_host_actor(const ta_dm_host *host);
