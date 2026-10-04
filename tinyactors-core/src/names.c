#include "names.h"
#include "../include/tinyactors.h"
#include "atom.h"

void ta_names_init(ta_names *names, const ta_names_init_params *params) {
    *names = (ta_names){
        .atoms = params->atoms,
        .scratch = params->scratch,
        .capacity = params->capacity,
        .count = 0,
    };
}

/* Counts the segments of `name`, or answers 0 when it has an empty one
 * (which an empty name is). */
static uint32_t ta_names_segment_count(ta_str name) {
    uint32_t count = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= name.len; index += 1u) {
        if (index < name.len && name.ptr[index] != '.') {
            continue;
        }
        if (index == start) {
            return 0;
        }
        count += 1u;
        start = index + 1u;
    }
    return count;
}

ta_result ta_names_resolve(ta_names *names, ta_str name) {
    if (name.ptr == NULL && name.len != 0) {
        return TA_ERR_INVALID;
    }
    uint32_t count = ta_names_segment_count(name);
    if (count == 0 || count > names->capacity) {
        return TA_ERR_INVALID;
    }
    uint32_t segment = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= name.len; index += 1u) {
        if (index < name.len && name.ptr[index] != '.') {
            continue;
        }
        ta_atom atom = TA_NAMES_UNKNOWN;
        if (ta_atom_find(names->atoms, (ta_str){ .ptr = name.ptr + start, .len = index - start }, &atom) != TA_OK) {
            atom = TA_NAMES_UNKNOWN;
        }
        names->scratch[segment] = atom;
        segment += 1u;
        start = index + 1u;
    }
    names->count = count;
    return TA_OK;
}

ta_atom ta_names_lookup(const ta_names *names, ta_str text) {
    ta_atom atom = TA_NAMES_UNKNOWN;
    if (text.len == 0 || ta_atom_find(names->atoms, text, &atom) != TA_OK) {
        return TA_NAMES_UNKNOWN;
    }
    return atom;
}
