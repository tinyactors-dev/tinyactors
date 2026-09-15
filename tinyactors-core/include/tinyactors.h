#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t ta_result;
typedef uint32_t ta_atom;
typedef uint32_t ta_event_name_id;
typedef uint32_t ta_definition_id;
typedef uint32_t ta_state_id;
typedef uint32_t ta_transition_id;
typedef uint32_t ta_expr_id;
typedef uint32_t ta_action_id;
typedef uint64_t ta_actor_id;
typedef uint64_t ta_invoke_id;
typedef uint64_t ta_send_id;
typedef uint64_t ta_timer_id;
typedef uint64_t ta_time;

enum {
    TA_OK = 0,
    TA_ERR_INVALID = -1,
    TA_ERR_NOMEM = -2,
    TA_ERR_NOT_FOUND = -3,
    TA_ERR_NOT_SERIALIZABLE = -4,
    TA_ERR_BUSY = -5,
    TA_ERR_UNSUPPORTED = -6,
    TA_ERR_CORRUPT = -7
};

typedef struct {
    const char *ptr;
    uint32_t len;
} ta_str;

typedef struct {
    const uint8_t *ptr;
    uint32_t len;
} ta_bytes;

typedef struct ta_system ta_system;
typedef struct ta_definition ta_definition;
typedef struct ta_definition_builder ta_definition_builder;
typedef struct ta_snapshot_writer ta_snapshot_writer;
typedef struct ta_snapshot_reader ta_snapshot_reader;

typedef struct {
    void *ptr;
    size_t size;
    size_t alignment;
} ta_allocator_free_params;

typedef struct ta_allocator_vtable {
    void *(*alloc)(void *self, size_t size, size_t alignment);
    void (*free)(void *self, const ta_allocator_free_params *params);
} ta_allocator_vtable;

typedef struct {
    void *self;
    const ta_allocator_vtable *v;
} ta_allocator;

typedef struct ta_value ta_value;

typedef struct ta_value_vtable {
    void (*retain)(void *self);
    void (*release)(void *self);
    ta_result (*clone)(void *self, ta_value *out);
    ta_result (*serialize)(void *self, ta_snapshot_writer *writer);
} ta_value_vtable;

struct ta_value {
    void *self;
    const ta_value_vtable *v;
};

typedef enum {
    TA_EVENT_EXTERNAL = 0,
    TA_EVENT_INTERNAL = 1,
    TA_EVENT_PLATFORM = 2
} ta_event_type;

typedef enum {
    TA_ORIGIN_NONE = 0,
    TA_ORIGIN_ACTOR = 1,
    TA_ORIGIN_IO = 2,
    TA_ORIGIN_INVOKE = 3
} ta_origin_kind;

typedef struct {
    ta_origin_kind kind;
    union {
        ta_actor_id actor;
        ta_invoke_id invoke;
        uint64_t opaque;
    } as;
} ta_origin;

typedef struct {
    ta_event_name_id name;
    ta_event_type type;
    ta_send_id send_id;
    ta_origin origin;
    ta_invoke_id invoke_id;
    ta_value data;
} ta_event;

typedef struct {
    ta_event_name_id detail;
    int32_t code;
    ta_str message;
    ta_value data;
} ta_fault;

typedef enum {
    TA_OP_OK = 0,
    TA_OP_ERROR = 1
} ta_op_status;

typedef enum {
    TA_RUN_STABLE = 0,
    TA_RUN_PAUSED = 1,
    TA_RUN_DONE = 2,
    TA_RUN_ERROR = 3
} ta_run_status;

typedef struct ta_dm_host ta_dm_host;

typedef struct {
    const ta_definition *definition;
    ta_allocator allocator;
    void **shared;
} ta_datamodel_prepare_definition_params;

typedef struct {
    void *shared;
    void *instance;
    const ta_dm_host *host;
} ta_datamodel_init_instance_params;

typedef struct {
    void *shared;
    void *instance;
    ta_expr_id expr;
    const ta_dm_host *host;
    bool *out;
    ta_fault *fault;
} ta_datamodel_eval_condition_params;

typedef struct {
    void *shared;
    void *instance;
    ta_expr_id expr;
    const ta_dm_host *host;
    ta_value *out;
    ta_fault *fault;
} ta_datamodel_eval_value_params;

typedef struct {
    void *shared;
    void *instance;
    ta_expr_id location;
    const ta_dm_host *host;
    ta_value *out;
    ta_fault *fault;
} ta_datamodel_read_location_params;

typedef struct {
    void *shared;
    void *instance;
    ta_expr_id location;
    const ta_value *value;
    const ta_dm_host *host;
    ta_fault *fault;
} ta_datamodel_assign_params;

typedef struct {
    void *shared;
    void *instance;
    ta_expr_id script;
    const ta_dm_host *host;
    ta_fault *fault;
} ta_datamodel_execute_script_params;

typedef struct {
    void *shared;
    void *instance;
    ta_snapshot_writer *writer;
} ta_datamodel_snapshot_params;

typedef struct {
    void *shared;
    void *instance;
    ta_snapshot_reader *reader;
} ta_datamodel_restore_params;

typedef struct ta_datamodel_vtable {
    ta_result (*prepare_definition)(
        void *self, const ta_datamodel_prepare_definition_params *params);
    void (*destroy_definition)(void *self, void *shared, ta_allocator allocator);
    uint32_t (*instance_size)(void *self, void *shared);
    uint32_t (*instance_alignment)(void *self, void *shared);
    ta_result (*init_instance)(
        void *self, const ta_datamodel_init_instance_params *params);
    void (*destroy_instance)(void *self, void *shared, void *instance);

    ta_op_status (*eval_condition)(
        void *self, const ta_datamodel_eval_condition_params *params);
    ta_op_status (*eval_value)(
        void *self, const ta_datamodel_eval_value_params *params);
    ta_op_status (*read_location)(
        void *self, const ta_datamodel_read_location_params *params);
    ta_op_status (*assign)(void *self,
                           const ta_datamodel_assign_params *params);
    ta_op_status (*execute_script)(
        void *self, const ta_datamodel_execute_script_params *params);

    ta_result (*snapshot)(void *self,
                          const ta_datamodel_snapshot_params *params);
    ta_result (*restore)(void *self,
                         const ta_datamodel_restore_params *params);
} ta_datamodel_vtable;

typedef struct {
    void *self;
    const ta_datamodel_vtable *v;
} ta_datamodel;

typedef struct {
    ta_actor_id source;
    ta_send_id send_id;
    ta_event_name_id event;
    ta_value target;
    ta_value data;
} ta_send;

typedef struct ta_io_vtable {
    ta_op_status (*send)(void *self, const ta_send *send, ta_fault *fault);
} ta_io_vtable;

typedef struct {
    void *self;
    const ta_io_vtable *v;
} ta_io;

typedef struct {
    ta_actor_id parent;
    ta_invoke_id invoke_id;
    ta_atom type;
    ta_value src;
    ta_value data;
    bool autoforward;
} ta_invoke_request;

typedef struct {
    const ta_invoke_request *request;
    uint64_t *service_handle;
    ta_fault *fault;
} ta_invoker_start_params;

typedef struct {
    uint64_t service_handle;
    const ta_event *event;
    ta_fault *fault;
} ta_invoker_forward_params;

typedef struct ta_invoker_vtable {
    ta_op_status (*start)(void *self,
                          const ta_invoker_start_params *params);
    ta_op_status (*cancel)(void *self, uint64_t service_handle, ta_fault *fault);
    ta_op_status (*forward)(void *self,
                            const ta_invoker_forward_params *params);
} ta_invoker_vtable;

typedef struct {
    void *self;
    const ta_invoker_vtable *v;
} ta_invoker;

typedef struct {
    ta_allocator allocator;
    ta_time initial_time;
} ta_system_config;

typedef struct {
    ta_event_name_id name;
    char *buffer;
    uint32_t capacity;
    uint32_t *needed;
} ta_event_name_format_params;

typedef struct {
    ta_definition_id definition;
    ta_str name;
    const ta_value *initial_data;
    ta_actor_id *out;
} ta_actor_spawn_params;

typedef struct {
    ta_definition_id definition;
    ta_str name;
    ta_actor_id *out;
} ta_actor_find_params;

typedef struct {
    ta_actor_id actor;
    uint32_t budget;
    ta_run_status *status;
} ta_actor_resume_params;

ta_result ta_system_create(const ta_system_config *config, ta_system **out);
void ta_system_destroy(ta_system *system);

ta_event_name_id ta_event_name_parse(ta_system *system, ta_str name);
ta_event_name_id ta_event_name_append(ta_system *system,
                                      ta_event_name_id parent,
                                      ta_str segment);
bool ta_event_name_matches(const ta_system *system,
                           ta_event_name_id event,
                           ta_event_name_id prefix);
ta_result ta_event_name_format(const ta_system *system,
                               const ta_event_name_format_params *params);

ta_result ta_system_set_time(ta_system *system, ta_time now);
ta_time ta_system_time(const ta_system *system);

ta_result ta_actor_spawn(ta_system *system,
                         const ta_actor_spawn_params *params);
ta_result ta_actor_find(ta_system *system,
                        const ta_actor_find_params *params);
ta_result ta_actor_deliver(ta_system *system,
                           ta_actor_id actor,
                           const ta_event *event);
ta_result ta_actor_resume(ta_system *system,
                          const ta_actor_resume_params *params);
ta_result ta_actor_unload(ta_system *system, ta_actor_id actor);
ta_result ta_actor_load(ta_system *system, ta_actor_id actor);
ta_result ta_actor_destroy(ta_system *system, ta_actor_id actor);

#ifdef __cplusplus
}
#endif
