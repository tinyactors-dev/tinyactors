#pragma once

#include "atom.h"

/*
 * Package name_resolver is block D4 of the ring protocol: it turns the
 * bytes of a run-time event name into the atoms the matcher compares,
 * without interning anything (docs/proposals/ring-proto/reference/names.md,
 * "Matching without interning").
 *
 * # What it is for
 *
 * Authored names are interned once, when a definition is compiled. A name
 * that arrives while the system runs (an envelope's event name) stays bytes
 * in the name area, and interning it would grow tables that never shrink.
 * So when an envelope is taken, ta_name_resolve splits its bytes at the dots
 * and looks each segment up with ta_atom_find, which never inserts. The
 * atoms go into caller scratch and ta_pattern_match_run (definition.c) runs
 * on them unchanged.
 *
 * # UNKNOWN
 *
 * A segment that is not an atom becomes TA_NAME_UNKNOWN, which is
 * TA_ATOM_NONE. That value is never a valid atom, and the compiler gives
 * every non-wildcard descriptor segment a real atom. So an UNKNOWN event
 * segment equals no literal descriptor segment, and only a '*' descriptor
 * segment (which the matcher tests first and never compares by atom) can
 * consume it. A descriptor that ends before an UNKNOWN segment is a prefix
 * match, as before. This reproduces every row of the worked-example table in
 * names.md, so the matcher needs no change. The text '*' in an event name is
 * ordinary text here, as it is for the interner.
 *
 * # Which names are valid
 *
 * The same ones ta_event_name_intern_dotted accepts, so Phase 3 changes no
 * answer about validity: an empty name, an empty segment (a leading,
 * trailing or doubled dot) and more segments than `capacity` are
 * TA_ERR_INVALID. The empty segment is rejected before any lookup, because
 * the empty text is atom 0 and would otherwise resolve instead of failing.
 *
 * # Lifetimes
 *
 *   - The name bytes: envelope (or call, for a literal). Read during the
 *     call, never copied, retained or pointed into afterwards.
 *   - The atom table: system. Borrowed through a const pointer for the call.
 *   - `out`: macrostep/unit scratch, owned by the caller (Phase 3.1: one
 *     buffer per interpreter, it->event_atoms, eventNameDepth entries). The atoms in it are indexes into the
 *     atom table, which never moves or removes one, so they stay meaningful
 *     for as long as the system lives; UNKNOWN entries refer to nothing.
 *   - The parameter struct: call, on the caller's stack, not stored.
 *
 * # When it allocates
 *
 * Never. It takes no allocator, calls no allocating function and writes only
 * into `out` and `*count`, so it may run inside a macrostep.
 *
 * # Failure
 *
 * *count is set to 0 first and to the segment count only on success, so a
 * failure leaves nothing that could be taken for a result. The contents of
 * `out` are unspecified after a failure, but never written past `capacity`.
 * The atom table is the same before and after in every case.
 *
 * # Complexity
 *
 * O(length of the name): one pass, and one expected O(1)-probe hash lookup
 * per segment. No size arithmetic beyond indexes bounded by the length.
 *
 * Example:
 *
 *     ta_atom segments[TA_EVENT_NAME_MAX_DEPTH];
 *     uint32_t count = 0;
 *     ta_result result = ta_name_resolve(&(ta_name_resolve_params){
 *         .atoms = &system->atoms, .name = envelope_name,
 *         .out = segments, .capacity = system->event_names.trie.max_depth, .count = &count });
 *     // then ta_pattern_match_run over segments[0..count)
 */

/* The atom written for a segment that is not an atom; see the package comment. */
#define TA_NAME_UNKNOWN TA_ATOM_NONE

typedef struct {
    /* System lifetime; looked up, never changed, not retained. */
    const ta_atom_table *atoms;
    /* Dotted bytes; read during the call only. */
    ta_str name;
    /* Macrostep/unit scratch with room for `capacity` atoms. */
    ta_atom *out;
    /* Segments allowed: the system's eventNameDepth, at most TA_EVENT_NAME_MAX_DEPTH. */
    uint32_t capacity;
    /* Receives the number of segments written; 0 on failure. */
    uint32_t *count;
} ta_name_resolve_params;

/*
 * ta_name_resolve writes one atom per dot-separated segment of params->name
 * into params->out (TA_NAME_UNKNOWN for a segment that is not an atom) and
 * the number of segments into *params->count. It never allocates and never
 * interns. TA_ERR_INVALID, with *count 0, for a null atoms, out or count,
 * capacity 0, an empty name, an empty segment, more segments than capacity,
 * or a null ptr with a nonzero length. Needs nothing but the arguments.
 */
ta_result ta_name_resolve(const ta_name_resolve_params *params);
