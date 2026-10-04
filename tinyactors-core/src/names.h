/**
 * @file names.h
 *
 * # The name resolver
 *
 * Run-time names (the value of an `eventexpr`, the name of an external
 * event, a generated `done.invoke.<id>` or
 * `error.communication.invoke.<id>.<reason>`, a `typeexpr`, a
 * `sendidexpr`) stay bytes: core looks them up in the atom table and
 * never adds them to it. `ta_names` is the one object that does the
 * looking up. It turns name bytes into atoms that core's event matcher
 * (`ta_pattern_match_run`, `src/definition.c`) compares against a
 * descriptor's segments, without interning anything.
 *
 * - `ta_names_resolve` splits a dotted name at its dots, looks each
 *   segment up with `ta_atom_find` and writes one atom per segment into
 *   the resolver's scratch, in order. A segment that is not an atom
 *   becomes `TA_NAMES_UNKNOWN`.
 * - `ta_names_lookup` looks a whole text up as one atom, dots and all,
 *   for names that are not dotted paths: a `typeexpr` value or a
 *   `<cancel sendidexpr>` literal.
 *
 * ## UNKNOWN
 *
 * `TA_NAMES_UNKNOWN` is `TA_ATOM_NONE` (`UINT32_MAX`), the atom table's
 * "no atom". No real atom has that value: `ta_atom_intern` refuses to
 * create an atom once the table holds `TA_ATOM_NONE - 1` of them, so
 * every atom is below `TA_ATOM_NONE - 1`, and `ta_atom_valid` answers
 * false for it. The matcher compares an event segment with equality
 * only against a descriptor segment that is not a wildcard, and such a
 * segment always holds a real atom interned at registration; so UNKNOWN
 * equals none of them and is taken only by a `*` segment. (A wildcard
 * segment also stores `TA_ATOM_NONE` in its atom field, but the matcher
 * tests the wildcard flag first and never compares that field.) Because
 * every segment a descriptor compares with equality is a known atom, and
 * gets the same atom from the lookup, matching resolved atoms gives the
 * same answers as matching interned text.
 *
 * Every run-time name with the same bytes resolves to the same UNKNOWN;
 * UNKNOWN carries no identity. Atom 0 is the empty text, which names
 * nothing; no successful resolution yields it, since an empty segment is
 * refused, and `ta_names_lookup` answers UNKNOWN for empty text.
 *
 * ## Contract and invariants
 *
 * - A name is one or more segments separated by single dots. A segment
 *   is any run of bytes without a dot, compared byte for byte (case
 *   matters, no NUL terminator is needed or looked for).
 * - The scratch holds `capacity` atoms: the system's `eventNameDepth`.
 *   After a successful `ta_names_resolve`, `count` is the number of
 *   segments, `1 <= count <= capacity`, and `scratch[0 .. count)` are
 *   the segments' atoms, first segment first. They stay there until the
 *   next successful `ta_names_resolve`.
 * - Resolution is current: it reads the atom table as it is at the call,
 *   so a segment that was UNKNOWN resolves to its atom once a definition
 *   registered later has interned it. That is why a name is resolved
 *   when its envelope is taken, not when it is admitted.
 * - The resolver never inserts into the atom table, and never writes to
 *   it at all.
 *
 * ## Memory
 *
 * - `ta_names` owns no memory and never allocates. It borrows the atom
 *   table and the scratch array given to `ta_names_init`; both must
 *   outlive it, and the scratch must not move while the resolver holds
 *   it. The resolver keeps a pointer to the table's struct, never into
 *   its storage, so the table may grow (at registration) between calls.
 * - The name bytes are borrowed for the call only; nothing keeps them.
 * - One resolver, with one scratch of `eventNameDepth` atoms, serves the
 *   whole system: only one macrostep resolves at a time.
 * - Every function may run inside a macrostep.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_names_init` | O(1) |
 * | `ta_names_resolve` of n bytes in k segments | O(n) to check the dots, then k hash lookups over the n bytes: O(n) expected, the bucket chains being one atom long on average |
 * | `ta_names_lookup` of n bytes | one hash lookup, O(n) expected |
 *
 * ## Failure
 *
 * `ta_names_resolve` answers `TA_ERR_INVALID` for a name that is not a
 * valid event name, the same answer the atom and event-name tables give
 * where names are interned (ADR 0092 for the depth):
 *
 * - an empty name;
 * - an empty segment: a leading, trailing or doubled dot (`.a`, `a.`,
 *   `a..b`, `.`);
 * - more segments than the scratch holds (deeper than `eventNameDepth`);
 * - a NULL pointer with a nonzero length.
 *
 * Each of these can arrive at run time, from a host or a chart, so each
 * is checked on every call. The name is checked whole before anything is
 * written, so a failed call changes nothing: the scratch and `count` keep
 * the previous result. A caller answers the failure as for any invalid
 * name: an external event is bounced as `invalid`, and a name produced in
 * a macrostep fails its `<send>` as `error.execution`.
 *
 * A segment that is not an atom is not a failure: it is UNKNOWN.
 * `ta_names_lookup` never fails.
 *
 * ## Users
 *
 * Resolving a taken envelope's event name for the event matcher, which
 * then runs unchanged over the scratch; and the `typeexpr` and
 * `<cancel sendidexpr>` lookups, which use `ta_names_lookup`. ADR 0060
 * defines the matcher it feeds; ADR 0092 bounds names by `eventNameDepth`.
 */
#pragma once

#include "atom.h"
#include "../include/tinyactors.h"

/** The atom a segment resolves to when it is not an atom; see the file
 * comment for why it equals no real atom. */
#define TA_NAMES_UNKNOWN TA_ATOM_NONE

/** The name resolver: the atom table it reads and the scratch it writes;
 * see the file comment. Read `scratch` and `count` after a successful
 * `ta_names_resolve`. */
typedef struct {
    /** The table names are looked up in; borrowed, never written. */
    const ta_atom_table *atoms;
    /** Room for `capacity` atoms; borrowed. */
    ta_atom *scratch;
    /** Segments the scratch holds: the system's `eventNameDepth`. */
    uint32_t capacity;
    /** Segments of the last name resolved; 0 before the first. */
    uint32_t count;
} ta_names;

/** What `ta_names_init` binds the resolver to. */
typedef struct {
    /** The atom table; must outlive the resolver. */
    const ta_atom_table *atoms;
    /** An array of `capacity` atoms; must outlive the resolver and stay
     * where it is. NULL only with a capacity of 0. */
    ta_atom *scratch;
    /** The deepest name the resolver accepts, in segments. */
    uint32_t capacity;
} ta_names_init_params;

/**
 * Binds `names` to an atom table and a scratch array, with nothing
 * resolved yet (`count` 0). Borrows both; takes over nothing. Allocates
 * nothing, never fails, and may run inside a macrostep.
 */
void ta_names_init(ta_names *names, const ta_names_init_params *params);

/**
 * Resolves the dotted `name` into the scratch: one atom per segment, in
 * order, `TA_NAMES_UNKNOWN` for a segment that is not an atom, and sets
 * `count`. Borrows `name` for the call. Never inserts into the atom
 * table and never allocates, so it may run inside a macrostep.
 *
 * Answers `TA_ERR_INVALID`, changing nothing, for an empty name, an empty
 * segment (a leading, trailing or doubled dot), more segments than
 * `capacity`, or a NULL pointer with a nonzero length.
 */
ta_result ta_names_resolve(ta_names *names, ta_str name);

/**
 * Looks the whole `text` up as one atom, without splitting it at dots:
 * the atom when the text is one, `TA_NAMES_UNKNOWN` when it is not, when
 * it is empty, or when it is a NULL pointer with a nonzero length.
 * Borrows `text` for the call. Leaves the scratch alone, never inserts,
 * never allocates, never fails, and may run inside a macrostep.
 */
ta_atom ta_names_lookup(const ta_names *names, ta_str text);
