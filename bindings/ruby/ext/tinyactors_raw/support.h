#ifndef TINYACTORS_RAW_SUPPORT_H
#define TINYACTORS_RAW_SUPPORT_H

#include "ruby.h"
#include "tinyactors.h"

ta_value_t tar_value_to_native(VALUE object);
const ta_value_ops_t *tar_value_ops(void);

/* Shared native state.  These records are deliberately private: later
 * wrappers receive them through the factories below and never see an address
 * as a Ruby Integer. */
typedef struct tar_lifetime tar_lifetime_t;
typedef struct tar_root tar_root_t;
typedef struct tar_handle tar_handle_t;
typedef struct tar_system_state tar_system_state_t;
typedef void (*tar_cleanup_fn)(void *native);
typedef VALUE (*tar_protected_fn)(VALUE argument);

struct tar_lifetime {
    unsigned long generation;
    unsigned int alive;
    tar_lifetime_t *parent;
    void *system_identity;
    unsigned int callback;
    unsigned int references;
    tar_system_state_t *system_state;
};

struct tar_system_state { unsigned int callback_depth; };

struct tar_root {
    VALUE value;
    tar_root_t *next;
    unsigned int active;
};

struct tar_handle {
    void *native;
    tar_lifetime_t *token;
    VALUE owner;
    tar_root_t *roots;
    unsigned int kind;
    tar_cleanup_fn cleanup;
    uint64_t id;
    void *system_identity;
};

extern VALUE tar_mTinyActors;
extern VALUE tar_mRaw;

extern const rb_data_type_t tar_system_type;
extern const rb_data_type_t tar_definition_type;
extern const rb_data_type_t tar_arena_type;
extern const rb_data_type_t tar_context_type;
extern const rb_data_type_t tar_actor_ref_type;
extern const rb_data_type_t tar_request_ref_type;
extern const rb_data_type_t tar_invocation_ref_type;
extern const rb_data_type_t tar_cursor_type;
extern const rb_data_type_t tar_memory_type;

void tar_init_support(VALUE raw);
tar_lifetime_t *tar_lifetime_new(tar_lifetime_t *parent);
void tar_lifetime_invalidate(tar_lifetime_t *token);
void tar_lifetime_retain(tar_lifetime_t *token);
void tar_lifetime_release(tar_lifetime_t *token);
int tar_lifetime_live(const tar_lifetime_t *token);
void tar_root_add(tar_handle_t *handle, VALUE object);
void tar_root_remove_all(tar_handle_t *handle);
void tar_handle_init(tar_handle_t *handle, void *native, tar_lifetime_t *token);
void tar_handle_release(tar_handle_t *handle);
void tar_check_handle(const tar_handle_t *handle);
void tar_handle_set_cleanup(tar_handle_t *handle, tar_cleanup_fn cleanup);
void tar_deferred_cleanup_drain(void);
void tar_deferred_cleanup_boundary(void);
void tar_callback_enter(tar_lifetime_t *token);
void tar_callback_exit(tar_lifetime_t *token);
int tar_callback_reentry(tar_lifetime_t *token);
int tar_same_system(const tar_handle_t *left, const tar_handle_t *right);
VALUE tar_protected_call(tar_protected_fn function, VALUE argument, int *raised);
int tar_size_to_size_t(VALUE value, size_t *out);

tar_handle_t *tar_handle_from(VALUE object, const rb_data_type_t *type);
VALUE tar_system_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_definition_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_arena_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_context_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_memory_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_cursor_wrap(void *native, tar_lifetime_t *token, VALUE owner);
VALUE tar_actor_ref_wrap(uint64_t id, void *system_identity, tar_lifetime_t *token, VALUE owner);
VALUE tar_request_ref_wrap(uint64_t id, void *system_identity, tar_lifetime_t *token, VALUE owner);
VALUE tar_invocation_ref_wrap(uint64_t id, void *system_identity, tar_lifetime_t *token, VALUE owner);

VALUE tar_make_handle_class(const char *name, const rb_data_type_t *type);
typedef struct tar_wrap_spec {
    VALUE klass;
    const rb_data_type_t *type;
    VALUE owner;
    unsigned int kind;
} tar_wrap_spec_t;
VALUE tar_handle_wrap(const tar_wrap_spec_t *spec, void *native, tar_lifetime_t *token);
void *tar_handle_get(VALUE object, const rb_data_type_t *type);

#endif
