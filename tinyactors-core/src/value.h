#pragma once

#include "types.h"

static inline void ta_value_retain(ta_value *value) {
    if (value && value->v && value->v->retain) value->v->retain(value->self);
}

static inline void ta_value_release(ta_value *value) {
    if (value && value->v && value->v->release) value->v->release(value->self);
}

ta_result ta_value_clone(const ta_value *value, ta_value *out);
