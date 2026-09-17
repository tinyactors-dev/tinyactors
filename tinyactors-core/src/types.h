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
typedef uint32_t ta_action_block_id;
typedef uint32_t ta_initial_transition_id;
typedef uint32_t ta_data_decl_id;
typedef uint32_t ta_initializer_id;
typedef uint32_t ta_content_id;
typedef uint32_t ta_param_id;
typedef uint32_t ta_invoke_decl_id;
typedef uint32_t ta_extension_id;
typedef uint64_t ta_actor_id;
typedef uint64_t ta_invoke_id;
typedef uint64_t ta_send_id;
typedef uint64_t ta_timer_id;
typedef uint64_t ta_time;

#define TA_STATE_NONE UINT32_MAX
#define TA_INITIAL_TRANSITION_NONE UINT32_MAX
#define TA_INITIALIZER_NONE UINT32_MAX

enum {
    TA_OK = 0,
    TA_ERR_INVALID = -1,
    TA_ERR_NOMEM = -2,
    TA_ERR_NOT_FOUND = -3,
    TA_ERR_NOT_RECONSTRUCTIBLE = -4,
    TA_ERR_BUSY = -5,
    TA_ERR_UNSUPPORTED = -6,
    TA_ERR_CORRUPT = -7,
    TA_ERR_DEFINITION_MISMATCH = -8
};

typedef struct { const char *ptr; uint32_t len; } ta_str;
typedef struct { const ta_atom *atoms; uint32_t count; } ta_atom_list;
typedef struct { const uint8_t *ptr; uint32_t len; } ta_bytes;
typedef struct { ta_str name; } ta_definition_family;
typedef struct { ta_definition_family family; ta_str name; } ta_actor_address;
typedef enum { TA_ACTOR_REF_RUNTIME = 0, TA_ACTOR_REF_NAMED = 1 } ta_actor_ref_kind;
typedef struct {
    ta_actor_ref_kind kind;
    union {
        ta_actor_id runtime;
        ta_actor_address named;
    } as;
} ta_actor_ref;

typedef struct ta_system ta_system;
typedef struct ta_definition ta_definition;
typedef struct ta_definition_builder ta_definition_builder;
typedef struct ta_dm_host ta_dm_host;
typedef struct ta_value ta_value;
typedef struct ta_session_image ta_session_image;

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
