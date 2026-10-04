#pragma once

#include "arena.h"
#include "table.h"

/*
 * Package atom interns text: every distinct string that core compares often
 * (event name segments, family names, state and invoke IDs, URIs) gets one
 * small number, an atom, and from then on two strings are equal when their
 * atoms are. Atoms are dense, in the order the texts were first interned
 * (the empty text is atom 0), and are persisted in traces and session
 * images, so the numbering never changes.
 *
 * Lifetime: system. The table is created with the system and destroyed with
 * it; an atom is never removed, so an atom and the text a ta_str returned by
 * ta_atom_text points at live until the system is destroyed, and neither moves.
 * Text lives in an arena (blocks that never move), the entries in a vector
 * (so a pointer from ta_vec_at is good only until the next intern).
 *
 * The index from text to atom is a ta_table (block B3) holding (FNV-1a of
 * the text, atom). The table only stores and moves slots; this unit
 * compares the text, which it finds in its own entries.
 *
 * When it allocates. Looking an atom up (ta_atom_find, ta_atom_text,
 * ta_atom_valid) and interning a text that is already an atom never
 * allocate. Interning a new text allocates: a slot in the entry vector, the
 * text in the arena and, when the index says it is due, one more index page
 * (ta_table_grow). The growth is requested here, just before the insert,
 * because today interning also happens in the middle of a macrostep (a
 * computed send ID, an event expression, an invoke ID). Phase 4 (4.2) of the
 * ring protocol removes run-time interning, after which new atoms are made
 * only at registration, compile and load, and growth happens at those safe
 * points alone. A grow that fails is only a postponement: the pages fill a
 * little more, and the intern still succeeds.
 *
 * Failure. TA_ERR_NOMEM, or TA_ERR_INVALID for a null pointer with a
 * length. The fallible steps (reserve the entry, copy the text, insert into
 * the index) come before the infallible one (record the entry), so a failed
 * intern leaves no atom; text already copied into the arena stays there
 * until destroy, as an arena does not take single allocations back.
 *
 * Complexity: intern and find are O(length of the text) expected; text and
 * valid are O(1).
 *
 * Example:
 *
 *     ta_atom atom;
 *     ta_atom_intern(&system->atoms, (ta_str){ "error", 5 }, &atom);
 *     ta_str text = ta_atom_text(&system->atoms, atom);   // "error", stable
 */

/* Atom text lives in arena blocks, so a returned ta_str never moves. */
typedef struct {
    ta_str text;
} ta_atom_entry;

typedef struct {
    ta_allocator allocator;
    ta_vec entries;
    ta_table index;
    ta_arena strings;
} ta_atom_table;

#define TA_ATOM_NONE UINT32_MAX

/*
 * ta_atom_table_init makes the table with the empty text as atom 0 (no
 * family is empty, so a zeroed family atom names none, ADR 0088). Safe
 * point: it allocates. On failure nothing is left allocated.
 */
ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
/* ta_atom_table_destroy frees the index, the entries and the text; every ta_str it gave out is dead. */
void ta_atom_table_destroy(ta_atom_table *table);
/*
 * ta_atom_intern stores the atom of `text` in *out, making it if there is
 * none yet. It allocates only for a new text (see the package comment), and
 * a failure leaves the table without that atom.
 */
ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out);
/* ta_atom_text is the text of `atom`, or an empty string for an invalid atom. It never allocates. */
ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
/* Looks text up without interning it; TA_ERR_NOT_FOUND when it is not an atom. Never allocates. */
ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out);
/* ta_atom_valid says whether `atom` names an interned text. */
bool ta_atom_valid(const ta_atom_table *table, ta_atom atom);
