#include "events.h"

ta_result tah_event_name_intern(ta_system *system, ta_str text, ta_event_name_id *out) {
    if (system == NULL || out == NULL || text.ptr == NULL || text.len == 0) {
        return TA_ERR_INVALID;
    }
    /* eventNameDepth can only be lowered from its default (core ADR 0092),
     * which therefore sizes the array; the system's own value is the limit. */
    ta_atom atoms[TA_PARAM_DEFAULT_EVENT_NAME_DEPTH];
    uint64_t depth = TA_PARAM_DEFAULT_EVENT_NAME_DEPTH;
    (void)ta_system_param(system, TA_PARAM_EVENT_NAME_DEPTH, &depth);
    uint32_t count = 0;
    uint32_t start = 0;
    for (uint32_t index = 0; index <= text.len; index += 1) {
        if (index < text.len && text.ptr[index] != '.') {
            continue;
        }
        if (index == start || count == depth || count == TA_PARAM_DEFAULT_EVENT_NAME_DEPTH) {
            return TA_ERR_INVALID; /* an empty segment, or too many */
        }
        ta_result result = ta_system_intern_atom(system, (ta_str){ .ptr = text.ptr + start, .len = index - start },
                                                 &atoms[count]);
        if (result != TA_OK) {
            return result;
        }
        count += 1u;
        start = index + 1u;
    }
    return ta_event_name_intern(system, &(ta_atom_list){ .atoms = atoms, .count = count }, out);
}
