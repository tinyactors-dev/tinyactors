#include "internal.h"

/* Looks one segment up into *out; UNKNOWN when it is not an atom. Never inserts. */
static ta_result ta_name_resolve_segment(const ta_atom_table *atoms, ta_str text, ta_atom *out) {
    ta_result result = ta_atom_find(atoms, text, out);
    if (result == TA_ERR_NOT_FOUND) {
        *out = TA_NAME_UNKNOWN;
        return TA_OK;
    }
    return result == TA_OK ? TA_OK : TA_ERR_INVALID;
}

ta_result ta_name_resolve(const ta_name_resolve_params *params) {
    if (params == NULL || params->count == NULL) {
        return TA_ERR_INVALID;
    }
    *params->count = 0;
    ta_str name = params->name;
    if (params->atoms == NULL || params->out == NULL || params->capacity == 0 ||
        name.len == 0 || name.ptr == NULL) {
        return TA_ERR_INVALID;
    }
    uint32_t segments = 0;
    uint32_t start = 0;
    /* index < len only: len may be UINT32_MAX, so the last segment is
     * flushed after the loop instead of at index == len. */
    for (uint32_t index = 0; index < name.len; index += 1u) {
        if (name.ptr[index] != '.') {
            continue;
        }
        if (index == start || segments == params->capacity) {
            return TA_ERR_INVALID;
        }
        ta_str text = { .ptr = name.ptr + start, .len = index - start };
        if (ta_name_resolve_segment(params->atoms, text, &params->out[segments]) != TA_OK) {
            return TA_ERR_INVALID;
        }
        segments += 1u;
        start = index + 1u;
    }
    if (start == name.len || segments == params->capacity) {
        return TA_ERR_INVALID;
    }
    ta_str last = { .ptr = name.ptr + start, .len = name.len - start };
    if (ta_name_resolve_segment(params->atoms, last, &params->out[segments]) != TA_OK) {
        return TA_ERR_INVALID;
    }
    *params->count = segments + 1u;
    return TA_OK;
}
