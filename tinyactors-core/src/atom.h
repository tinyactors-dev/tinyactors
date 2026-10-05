/**
 * @file atom.h
 *
 * # Atom table
 *
 * `ta_atom_table` interns ordinary strings, such as definition, state and
 * type names, and gives each distinct text one `ta_atom`: a compact,
 * process-local index into the table. Atoms and structured event names
 * are distinct: an event name is a path of atom segments linked through
 * the event-name table.
 *
 * ## Contract and invariants
 *
 * - Interning the same bytes twice answers the same atom; atoms are
 *   numbered from 0 in the order their texts were first interned.
 * - Atom 0 is the empty text, interned by `ta_atom_table_init`, so a
 *   zeroed family atom names none (ADR 0088).
 * - An atom's text is a copy the table keeps in arena blocks, so the
 *   `ta_str` that `ta_atom_text` answers never moves while the table
 *   lives.
 * - Buckets are found with 32-bit FNV-1a over the text (`hash.h`), and
 *   the table doubles its buckets when it holds more atoms than buckets.
 * - An atom is process-local. A portable durability encoding records the
 *   text (or another stable dictionary representation), never an atom's
 *   index; session images in-process may hold atoms.
 *
 * ## Memory
 *
 * The table owns its entries (a `ta_vec`), its buckets and the arena of
 * texts, all from the allocator given to `ta_atom_table_init`, until
 * `ta_atom_table_destroy`. Callers borrow the texts it answers for as long
 * as the table lives. `ta_atom_table_init` and `ta_atom_intern` allocate;
 * `ta_atom_find`, `ta_atom_text` and `ta_atom_valid` never allocate and
 * may run inside a macrostep.
 *
 * ## Complexity
 *
 * `ta_atom_intern` and `ta_atom_find` hash the text, O(length), then walk
 * one bucket's chain, expected O(1); an intern that doubles the buckets
 * rehashes every atom once, amortized O(1). `ta_atom_text` and
 * `ta_atom_valid` are O(1).
 *
 * ## Failure
 *
 * `ta_atom_intern` and `ta_atom_find` answer `TA_ERR_INVALID` for a text
 * with a NULL `ptr` and a non-zero `len`. `ta_atom_find` answers
 * `TA_ERR_NOT_FOUND` for a text that is not an atom. `ta_atom_intern`
 * answers `TA_ERR_NOMEM` when the copy or the entry cannot be allocated,
 * or the table is full; the table then holds no new atom (the arena may
 * keep the copied bytes until the table is destroyed). A failed doubling
 * of the buckets is not a failure: the old buckets stay correct.
 * `ta_atom_table_init` answers `TA_ERR_NOMEM` and holds nothing.
 * `ta_atom_text` answers an empty text for an atom the table does not
 * hold.
 *
 * ## Users
 *
 * The system's names and types, the event-name table and the name
 * resolver, the builder and compiled definitions, the interpreter and
 * invocations, the data model, SCXML URIs and the session image.
 */
#pragma once

#include "arena.h"
#include "vec.h"
#include "../include/tinyactors.h"

/** One atom: its text's hash, the next atom in its bucket, and its text in arena blocks, which never moves. */
typedef struct {
    uint32_t hash;
    uint32_t next;
    ta_str text;
} ta_atom_entry;

/** The atom table; see the file comment. */
typedef struct {
    ta_allocator allocator;
    ta_vec entries;
    uint32_t *buckets;
    uint32_t bucket_count;
    ta_arena strings;
} ta_atom_table;

/** No atom; it also ends a bucket's chain. */
#define TA_ATOM_NONE UINT32_MAX

/**
 * Makes `table` an atom table on `allocator`, holding the empty text as
 * atom 0. Allocates. Answers `TA_ERR_NOMEM`, holding nothing, when an
 * allocation fails.
 */
ta_result ta_atom_table_init(ta_atom_table *table, ta_allocator allocator);
/** Frees everything `table` holds; texts it answered are gone. Never allocates. */
void ta_atom_table_destroy(ta_atom_table *table);
/**
 * Stores in `*out` the atom of `text`, interning a copy when it is new.
 * Borrows `text`. Allocates. Answers `TA_ERR_INVALID` for a NULL `ptr`
 * with a non-zero `len`, and `TA_ERR_NOMEM` when it cannot allocate or the
 * table is full, holding no new atom.
 */
ta_result ta_atom_intern(ta_atom_table *table, ta_str text, ta_atom *out);
/** The text of `atom`, borrowed from the table; empty for an atom it does not hold. Never allocates. */
ta_str ta_atom_text(const ta_atom_table *table, ta_atom atom);
/**
 * Looks `text` up without interning it and stores its atom in `*out`.
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_NOT_FOUND`
 * when it is not an atom and `TA_ERR_INVALID` for a NULL `ptr` with a
 * non-zero `len`.
 */
ta_result ta_atom_find(const ta_atom_table *table, ta_str text, ta_atom *out);
/** Whether the table holds `atom`. Never allocates. */
bool ta_atom_valid(const ta_atom_table *table, ta_atom atom);
