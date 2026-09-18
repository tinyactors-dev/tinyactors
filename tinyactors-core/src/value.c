#include "value.h"

ta_result ta_value_clone(const ta_value *value, ta_value *out) {
    if (value == NULL || out == NULL) {
        return TA_ERR_INVALID;
    }
    if (value->v == NULL) {
        *out = (ta_value){0};
        return TA_OK;
    }
    if (value->v->clone == NULL) {
        return TA_ERR_UNSUPPORTED;
    }
    return value->v->clone(value->self, out);
}
