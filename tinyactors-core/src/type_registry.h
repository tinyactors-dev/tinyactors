#pragma once

#include "atom.h"

/*
 * Package type_registry is block B7 of the ring protocol's catalog: the set
 * of type names a system has declared, held as atoms.
 *
 * What it is for. An SCXML document names a kind of thing by a string: the
 * `type` of a <send> says which I/O processor carries the event, the `type`
 * of an <invoke> says which invoker runs the child. Core accepts a type only
 * if someone declared it, and today each of the two checks has its own
 * private list and its own spelling of the SCXML forms (system.c, invoke.c).
 * This block is the one set both use; Phase 5.2 creates it twice, once for
 * I/O processors and once for invokers, and fills it from the host's type
 * declarations. The registry records that a name is declared and which
 * built-in it is an alias of. No behaviour is attached to an entry: what an
 * I/O processor or an invoker does stays in the vtable registries, which
 * look their handler up by the canonical atom this block returns.
 *
 * Names and aliases. An entry is a pair (name, canonical), both atoms. A
 * built-in type has name == canonical. The SCXML forms core has always
 * accepted for the same type ("scxml", the URI with and without its trailing
 * slash) are aliases: entries whose canonical is the built-in, so a lookup
 * of an alias resolves to the one atom the rest of core compares against.
 * Only the exact strings of the built-in specs are aliases; "SCXML" or a
 * different slash is not declared. A name a host declares is its own
 * canonical.
 *
 * Lifetime: system. The registry is embedded in its owner (ta_system once
 * Phase 5.2 wires it), is created with the system and destroyed with it.
 * Entries are never removed, so an entry index and the atoms in it are valid
 * for the system's life. The registry owns its entry vector and its index
 * table and nothing else. It borrows the system's atom table (a pointer,
 * not owned), which holds every name's text; the registry stores atom
 * numbers only and never a text pointer, so nothing can dangle. The
 * registry must be destroyed no later than the atom table, and the atom
 * table must not move while the registry is alive (both live inside
 * ta_system, so it does not).
 *
 * When it allocates. Only at safe points, never inside a macrostep:
 *
 *   - ta_type_registry_init (system creation): the vector, the first index
 *     page, and the atoms and entries of the built-ins.
 *   - ta_type_registry_declare (host declaration, before sessions run): the
 *     atom of a new name, one vector slot, and, when the index says it is
 *     due, one more index page (ta_table_grow) and room for the insert
 *     (ta_table_prepare).
 *
 * ta_type_registry_declared, ta_type_registry_declared_atom,
 * ta_type_registry_count and ta_type_registry_destroy never allocate. The
 * lookups never insert and never intern: ta_type_registry_declared finds the
 * name with ta_atom_find, which leaves the atom table untouched, so a
 * document that names a type nobody declared costs a probe and not an atom.
 * That is what lets the check run inside a macrostep or while loading
 * untrusted text.
 *
 * Invariants.
 *
 *   - Every entry's name is a valid atom of the borrowed table, and no two
 *     entries have the same name.
 *   - The canonical of an entry is the name of an entry whose canonical is
 *     itself: aliases do not chain.
 *   - The index holds one pair (hash of the name atom, entry index) per
 *     entry. The hash is a 32 bit mix of the atom number; two atoms may
 *     share it, so a probe compares the atom in the entry.
 *   - Atom numbers are persisted in traces and session images, and the
 *     built-ins are interned in the order of their spec (canonical, then its
 *     aliases, spec by spec). The order of a spec is part of its contract.
 *
 * Complexity. Lookups and declare are O(1) expected (one table probe plus,
 * for a text lookup, ta_atom_find, O(length of the name)). Growing is
 * O(page slots), amortised into the declares.
 *
 * Failure. Init is all or nothing: on failure it destroys what it made and
 * leaves *registry zeroed. Declare answers TA_ERR_NOMEM or TA_ERR_INVALID
 * and leaves the registry unchanged. It may have added the atom of the name
 * to the atom table; atoms are never removed and an unreferenced one is
 * harmless, so that is not a change of the registry. The unit never
 * partially applies: every fallible step (intern, reserve a slot, grow,
 * prepare) is done before the infallible ones (push, insert).
 *
 * Withdrawal is not supported. Nothing needs it yet, and adding it later is
 * safe: withdrawing a type only refuses new uses of the name. It would mark
 * the entry (or remove the pair from the index with ta_table_remove) and
 * keep the entry and its atoms, so every atom, index and canonical already
 * handed out stays valid, and a running session that holds the atom is not
 * affected. The invariants above would only gain "an alias to a withdrawn
 * type is withdrawn with it".
 *
 * Example, an I/O processor registry created with the system and used by
 * the compiler and the sender:
 *
 *     ta_type_registry io;
 *     ta_type_registry_init(&io, allocator, &(ta_type_registry_params){
 *         .atoms = &system->atoms, .builtins = ta_type_registry_io_builtins });
 *
 *     // before sessions run (a safe point): the host declares its own type
 *     ta_atom canonical;
 *     ta_type_registry_declare(&io, ta_str_from_cstr("mqtt"), &canonical);
 *
 *     // later, in a macrostep: a lookup that cannot allocate
 *     if (!ta_type_registry_declared_atom(&io, type_attribute, &canonical)) {
 *         // refuse the send: unknown I/O processor type
 *     }
 *
 *     ta_type_registry_destroy(&io);   // with the system, before the atoms
 */

/* ta_type_entry is one declared name and the built-in it resolves to (itself, unless it is an alias). 8 bytes, system lifetime. */
typedef struct ta_type_entry {
    ta_atom name;
    ta_atom canonical;
} ta_type_entry;

/*
 * ta_type_registry is the control block, embedded in its owner (system
 * lifetime). Its fields are private to type_registry.c. A zeroed registry
 * is empty and may be destroyed.
 */
typedef struct ta_type_registry {
    ta_atom_table *atoms; /* borrowed, system lifetime, not owned */
    ta_vec entries;       /* ta_type_entry; the index is the table's value; never shrinks */
    ta_table index;       /* GROWING: key = 32 bit mix of the name atom, value = entry index */
} ta_type_registry;

/* ta_type_builtin is one built-in type core owns: its canonical text and the other names it answers to. Static const data. */
typedef struct ta_type_builtin {
    ta_str name;
    const ta_str *aliases; /* may be NULL when alias_count is 0 */
    uint32_t alias_count;
} ta_type_builtin;

/* ta_type_builtin_set is an array of built-ins and its length. */
typedef struct ta_type_builtin_set {
    const ta_type_builtin *items;
    uint32_t count;
} ta_type_builtin_set;

/*
 * ta_type_registry_params says what registry to make: the atom table it
 * borrows (required, it must outlive the registry) and the built-ins to
 * start with (may be empty). The set is read during init and not kept; it is
 * call lifetime.
 */
typedef struct ta_type_registry_params {
    ta_atom_table *atoms;
    ta_type_builtin_set builtins;
} ta_type_registry_params;

/*
 * ta_type_registry_io_builtins is the I/O processor type core owns: the
 * SCXML event processor URI, with "scxml" as its alias (system.c accepts
 * both today). The URI forms live here, in one place.
 */
extern const ta_type_builtin_set ta_type_registry_io_builtins;

/*
 * ta_type_registry_invoker_builtins is the invoker type core owns:
 * "http://www.w3.org/TR/scxml/" with the aliases "scxml" and
 * "http://www.w3.org/TR/scxml" (the forms ta_invoke_normalize_type accepts
 * today). The I/O and invoker alias sets differ, so they are separate specs.
 */
extern const ta_type_builtin_set ta_type_registry_invoker_builtins;

/*
 * ta_type_registry_init makes a registry over params->atoms with the given
 * built-ins declared, interning each canonical name and then its aliases in
 * array order. A name that is already declared (a spec that repeats one) is
 * kept as it was: the first declaration wins. A safe point: it allocates.
 * TA_ERR_INVALID for a missing atom table or a built-in with an empty or
 * NULL name or alias, or an alias list that is NULL but counted;
 * TA_ERR_NOMEM when an allocation fails. On failure *registry is zeroed and
 * owns nothing (atoms interned stay in the atom table, which is harmless).
 */
ta_result ta_type_registry_init(ta_type_registry *registry, ta_allocator allocator, const ta_type_registry_params *params);

/*
 * ta_type_registry_destroy frees the entry vector and the index table with
 * the sizes they were allocated with and zeroes *registry. It never
 * allocates and touches no atom: the text belongs to the atom table.
 * Idempotent on a zeroed registry.
 */
void ta_type_registry_destroy(ta_type_registry *registry);

/*
 * ta_type_registry_declare declares `name` as a type of its own, interning
 * it. Safe points only, never inside a macrostep: it may allocate (the atom,
 * a vector slot, an index page). It is idempotent: a name already declared,
 * as a built-in, an alias or by an earlier call, returns TA_OK and changes
 * nothing. If `out` is not NULL it receives the canonical atom (the name's
 * own atom for a new type, the built-in for an alias); on failure it is
 * left alone. TA_ERR_INVALID for an empty name or a NULL pointer with a
 * length; TA_ERR_NOMEM when memory runs out or the entry count would reach
 * TA_ATOM_NONE. A failure leaves the registry unchanged (the name's atom may
 * remain in the atom table). A failed ta_table_grow is only a postponement
 * and does not fail the declare.
 */
ta_result ta_type_registry_declare(ta_type_registry *registry, ta_str name, ta_atom *out);

/*
 * ta_type_registry_declared says whether `name` is a declared type. It never
 * allocates, never inserts and never interns: an unknown name leaves the
 * atom table as it was. If `out` is not NULL it receives the canonical atom
 * (an alias gives its built-in), or TA_ATOM_NONE when the answer is false.
 * Safe at any point, including inside a macrostep. O(length of name)
 * expected.
 */
bool ta_type_registry_declared(const ta_type_registry *registry, ta_str name, ta_atom *out);

/*
 * ta_type_registry_declared_atom is ta_type_registry_declared for a caller
 * that already holds the atom, such as a parsed `type` attribute. An atom
 * that is not valid (TA_ATOM_NONE, or past the atom table) is not declared.
 * Never allocates; O(1) expected. `out` is as for ta_type_registry_declared.
 */
bool ta_type_registry_declared_atom(const ta_type_registry *registry, ta_atom name, ta_atom *out);

/* ta_type_registry_count is the number of entries, aliases included. Never allocates. For tests and inspection. */
uint32_t ta_type_registry_count(const ta_type_registry *registry);
