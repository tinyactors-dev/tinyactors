/*
 * Package name_resolver (implementation): block D4 of the ring protocol. The
 * contract is in name_resolver.h; this is what the implementation relies on.
 *
 * Purpose: turn the bytes of a run-time event name into the atoms
 * ta_pattern_match_run compares, without interning, so the atom table and the
 * trie never grow because of a name that arrives while the system runs.
 *
 * Lifetimes: the name bytes (envelope or call) are read and never retained;
 * the atom table (system) is borrowed const, so a write to it cannot compile;
 * `out` is the caller's macrostep/unit scratch and holds only atom indexes
 * (or TA_NAME_UNKNOWN, which refers to nothing); the params struct is call
 * lifetime.
 *
 * Allocation: never. There is no allocator parameter and no size arithmetic:
 * every offset is an index below name.len, so it cannot overflow uint32_t.
 * The loop stops at index < len and flushes the last segment afterwards,
 * because len may be UINT32_MAX and `index <= len` would never end.
 *
 * Invariants: a segment is looked up with ta_atom_find only (never inserted);
 * an empty segment is rejected before the lookup, since the empty text is a
 * real atom and would otherwise resolve; at most `capacity` entries of `out`
 * are written; *count is 0 until the whole name has resolved.
 *
 * Complexity: one pass over the name, one hash lookup per segment.
 *
 * Failure: any error returns TA_ERR_INVALID with *count == 0 and the atom
 * table untouched; `out` is unspecified but never written past `capacity`.
 * An atom lookup error other than NOT_FOUND is also reported as TA_ERR_INVALID.
 *
 * Example (parameters are described in name_resolver.h):
 *
 *     ta_atom segments[4];
 *     uint32_t count = 0;
 *     ta_str name = { .ptr = (const uint8_t *)"a.zzz", .len = 5 };
 *     ta_result r = ta_name_resolve(&(ta_name_resolve_params){
 *         .atoms = atoms, .name = name,
 *         .out = segments, .capacity = 4, .count = &count });
 *     // r == TA_OK, count == 2; segments[1] == TA_NAME_UNKNOWN if "zzz" is no atom
 */

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
