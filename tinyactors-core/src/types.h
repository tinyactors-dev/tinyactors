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

typedef struct { const char *ptr; uint32_t len; } ta_str;
typedef struct { const ta_atom *atoms; uint32_t count; } ta_atom_list;
typedef struct { const uint8_t *ptr; uint32_t len; } ta_bytes;

typedef struct ta_system ta_system;
typedef struct ta_definition ta_definition;
typedef struct ta_definition_builder ta_definition_builder;
typedef struct ta_dm_host ta_dm_host;
typedef struct ta_value ta_value;
typedef struct ta_snapshot_batch ta_snapshot_batch;

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct { void *ptr; size_t size; size_t alignment; } ta_allocator_free_params;
typedef struct ta_allocator_vtable {
    void *(*alloc)(void *self, size_t size, size_t alignment);
    void (*free)(void *self, const ta_allocator_free_params *params);
} ta_allocator_vtable;
typedef struct { void *self; const ta_allocator_vtable *v; } ta_allocator;

#ifdef __cplusplus
}
#endif

#define TA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define TA_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

typedef struct {
    uint32_t offset;
    uint32_t length;
} ta_span32;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} ta_actor_key;
