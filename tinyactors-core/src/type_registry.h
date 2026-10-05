/**
 * @file type_registry.h
 *
 * # Type registry
 *
 * `ta_type_registry` is a set of type names, each an atom: the types a
 * chart may name in a `<send type>` (the I/O processor types) or an
 * `<invoke type>` (the invoker types). A system has one registry of each
 * kind, made from this one unit with a different built-in set. Core
 * checks a type against its registry when the action runs: an
 * unsupported type raises `error.execution` in the chart, which is why
 * core, not the host, must know the supported types.
 *
 * No behaviour is attached to an entry. The registry answers only whether
 * a type is declared, and which entry a name stands for; what a type does
 * is the binding's, reached through the records core writes for a send or
 * an invocation.
 *
 * ## Entries
 *
 * - **Built-in entries** are the types core implements itself. They come
 *   from the `ta_type_registry_builtins` given to `ta_type_registry_init`,
 *   which also lists their **aliases**: other names for a built-in entry.
 *   Built-ins and aliases are there from init, and core owns them: they
 *   cannot be declared.
 * - **Declared entries** are added by `ta_type_registry_declare`, one per
 *   name, outside a macrostep, when the host declares a type.
 * - Every name in the registry stands for a **canonical** entry: a
 *   declared entry and a built-in entry stand for themselves, an alias for
 *   the built-in entry it names. `ta_type_registry_declared` answers the
 *   canonical atom, so a name and its aliases agree: a check of
 *   `scxml` and of the SCXML URI give the same answer and the same type.
 *
 * Two built-in sets are defined here, from the types core accepts today:
 *
 * | Set | Canonical entry | Aliases |
 * |---|---|---|
 * | `TA_TYPE_REGISTRY_IO_BUILTINS` | `http://www.w3.org/TR/scxml/#SCXMLEventProcessor` (local mail) | `scxml` |
 * | `TA_TYPE_REGISTRY_INVOKER_BUILTINS` | `http://www.w3.org/TR/scxml/` (local child sessions) | `scxml`, `http://www.w3.org/TR/scxml` |
 *
 * ## Contract and invariants
 *
 * - **Declaring is idempotent.** Declaring a name that is declared
 *   already answers `TA_OK` and changes nothing; the host's declaration
 *   record writes nothing back in that case either.
 * - **A name core owns is reserved.** Declaring a built-in name or an
 *   alias answers `TA_ERR_UNSUPPORTED` and changes nothing; the host's
 *   declaration is bounced as `reserved_type`.
 * - **No withdrawal.** An entry, once there, stays until the registry is
 *   destroyed.
 * - **Lookups never insert.** `ta_type_registry_declared` takes an atom
 *   and never interns or adds anything. A run-time type (a `typeexpr`
 *   value) that is not an atom is looked up as `TA_ATOM_NONE`, which no
 *   entry has, and is not declared.
 * - Each name appears once. Entries are numbered in the order they were
 *   added, the built-ins first in their set's order; `entries.count` is
 *   how many there are, aliases included.
 * - Declared names are interned in the atom table the registry borrows,
 *   so they compare as atoms with the types a chart names.
 *
 * ## Memory
 *
 * The registry owns its entries (a `ta_vec`) and its hash table
 * (`table.h`, growing mode, keyed by the name's atom), both from the
 * allocator given to `ta_type_registry_init`, until
 * `ta_type_registry_destroy`. It borrows the atom table, which must
 * outlive it, and keeps a pointer to the table's struct, never into its
 * storage. The built-in set is borrowed only during init; its texts are
 * copied into the atom table.
 *
 * - `ta_type_registry_init` allocates and interns the built-in names: it
 *   runs where the system is made.
 * - `ta_type_registry_declare` may allocate (the entries, the hash
 *   table's slots, the name's atom). It runs only outside a macrostep,
 *   when the host's declaration is applied.
 * - `ta_type_registry_declared` never allocates and may run inside a
 *   macrostep.
 * - `ta_type_registry_destroy` only frees. The atoms stay in the atom
 *   table.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_type_registry_declared` | one hash lookup, expected O(1) |
 * | `ta_type_registry_declare` of n bytes | O(n) to find or intern the atom, then expected O(1); amortized O(1) for the growth of the entries and the hash table |
 * | `ta_type_registry_init` with k built-ins | O(k^2) to check the set for duplicates (k is a handful), then O(k) expected |
 * | `ta_type_registry_destroy` | two frees |
 *
 * ## Failure
 *
 * - `ta_type_registry_init` answers `TA_ERR_INVALID` for a malformed
 *   built-in set (a NULL list with a non-zero count, an empty name, a
 *   name listed twice, an alias whose canonical entry is not an earlier
 *   built-in that is itself no alias) and `TA_ERR_NOMEM` when it cannot
 *   allocate; the registry is then empty and may be destroyed. Built-in
 *   names it interned before failing stay in the atom table.
 * - `ta_type_registry_declare` answers `TA_ERR_INVALID` for an empty name
 *   or a NULL pointer with a non-zero length, `TA_ERR_UNSUPPORTED` for a
 *   built-in name or alias, `TA_ERR_FULL` when the registry holds
 *   `TA_TABLE_MAX_KEYS` entries, and `TA_ERR_NOMEM` when it cannot
 *   allocate. A failed call changes no entry: the registry answers every
 *   lookup as before. It may keep room it grew, and the atom table keeps
 *   whatever it interned, as `ta_atom_intern` documents.
 * - `ta_type_registry_declared` answers `TA_ERR_NOT_FOUND` for a type
 *   that is not in the registry.
 *
 * ## Users
 *
 * The system's two registries: I/O processor types, checked when a
 * `<send>` runs, and invoker types, checked when an invocation starts.
 * Both are filled by the host's `h2c.type_declare` control, whose
 * `registry` field picks one; its replay sees each declaration at the
 * same point among the inputs.
 */
#pragma once

#include "atom.h"
#include "table.h"
#include "vec.h"
#include "../include/tinyactors.h"

/** One built-in name of a set: a canonical entry, or an alias of one. */
typedef struct {
    /** The name; not empty. Copied into the atom table at init. */
    ta_str name;
    /**
     * 0 for a canonical entry. For an alias, the position plus one of its
     * canonical entry in the same set, which comes earlier and is not an
     * alias.
     */
    uint32_t canonical;
} ta_type_registry_builtin;

/** The built-in entries and aliases a registry is made with. */
typedef struct {
    /** `count` names; NULL only when `count` is 0. */
    const ta_type_registry_builtin *names;
    uint32_t count;
} ta_type_registry_builtins;

/** The I/O processor types core owns: the SCXML Event I/O Processor. */
extern const ta_type_registry_builtins TA_TYPE_REGISTRY_IO_BUILTINS;
/** The invoker types core owns: SCXML child sessions. */
extern const ta_type_registry_builtins TA_TYPE_REGISTRY_INVOKER_BUILTINS;

/** One name in a registry and the canonical entry it stands for. */
typedef struct {
    ta_atom name;
    /** `name` itself, unless the entry is an alias. */
    ta_atom canonical;
} ta_type_registry_entry;

/**
 * A type registry; see the file comment. `entries.count` (the names held,
 * aliases included) and `builtin_count` are for reading only.
 */
typedef struct {
    /** Where names are interned; borrowed, must outlive the registry. */
    ta_atom_table *atoms;
    /** One `ta_type_registry_entry` per name, the built-ins first. */
    ta_vec entries;
    /** The hash of a name's atom to its entry's position plus one. */
    ta_table index;
    /** The first `builtin_count` entries are the built-ins and aliases. */
    uint32_t builtin_count;
} ta_type_registry;

/** What `ta_type_registry_init` needs. */
typedef struct {
    /** Where the entries and hash table come from; must outlive the registry. */
    ta_allocator allocator;
    /** The atom table names are interned in; borrowed, must outlive the registry. */
    ta_atom_table *atoms;
    /** The built-ins and aliases; borrowed for the call. */
    const ta_type_registry_builtins *builtins;
} ta_type_registry_params;

/**
 * Makes `*registry` a registry holding the built-ins and aliases of
 * `params->builtins` and nothing else, interning their names. Allocates;
 * runs where the system is made, never inside a macrostep. Answers
 * `TA_ERR_INVALID` for a malformed built-in set and `TA_ERR_NOMEM` when it
 * cannot allocate; on failure the registry is empty and may be destroyed.
 */
ta_result ta_type_registry_init(ta_type_registry *registry, const ta_type_registry_params *params);

/**
 * Frees the entries and the hash table, leaving an empty registry that
 * may be destroyed again. Only frees; never fails. The atoms stay.
 */
void ta_type_registry_destroy(ta_type_registry *registry);

/**
 * Declares the type `name`, interning it. Borrows `name` for the call.
 * Declaring a name declared already answers `TA_OK` and changes nothing.
 * May allocate: runs only outside a macrostep, when the host's
 * declaration is applied. Answers `TA_ERR_INVALID` for an empty name or a
 * NULL pointer with a non-zero length, `TA_ERR_UNSUPPORTED` for a name
 * core owns (a built-in or an alias), `TA_ERR_FULL` when the registry
 * holds `TA_TABLE_MAX_KEYS` names, and `TA_ERR_NOMEM` when it cannot
 * allocate; a failed call adds no entry.
 */
ta_result ta_type_registry_declare(ta_type_registry *registry, ta_str name);

/**
 * Looks the type `type` up and, unless `canonical` is NULL, stores the
 * canonical entry it stands for in `*canonical`: the type itself, or for
 * an alias its built-in entry. Never inserts and never allocates; may run
 * inside a macrostep. Answers `TA_ERR_NOT_FOUND`, leaving `*canonical`
 * alone, for a type the registry does not hold, `TA_ATOM_NONE` included.
 */
ta_result ta_type_registry_declared(const ta_type_registry *registry, ta_atom type, ta_atom *canonical);
