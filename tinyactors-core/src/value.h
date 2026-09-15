#pragma once

#include "store.h"


#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_value_vtable {
    void (*retain)(void *self);
    void (*release)(void *self);
    ta_result (*clone)(void *self, ta_value *out);
    ta_result (*serialize)(void *self, ta_snapshot_writer *writer);
} ta_value_vtable;
struct ta_value { void *self; const ta_value_vtable *v; };

#ifdef __cplusplus
}
#endif

#include "types.h"

static inline void ta_value_retain(ta_value *value) {
    if (value && value->v && value->v->retain) value->v->retain(value->self);
}

static inline void ta_value_release(ta_value *value) {
    if (value && value->v && value->v->release) value->v->release(value->self);
}

ta_result ta_value_clone(const ta_value *value, ta_value *out);
