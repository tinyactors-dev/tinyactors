#pragma once

#include "atom.h"
#include "trie.h"

/*
 * Package event_name is the event-name vocabulary of core (ADR 0007): the
 * one user of the atom trie (block D7, trie.h) that knows what the paths
 * mean. An event name is the path of the atoms of its dot-separated
 * segments; `done.invoke.x` is (done, invoke, x). The name's ID is its node
 * in the trie, so the nodes, their IDs, validity, segments and the prefix
 * test are the trie's: ta_trie_valid, ta_trie_segments and ta_trie_under on
 * `table->trie`. What stays here is what is specific to events:
 *
 *   - the built-in names every system has, with fixed IDs (the
 *     TA_EVENT_NAME_* constants), made at init in that order;
 *   - the check that a segment is an atom of this system's atom table, which
 *     the trie, knowing atoms only as numbers, cannot make;
 *   - the dotted text form, used by the compiler for authored names.
 *
 * Generated prefixes. Every segment core itself puts in a generated name is
 * an atom from ta_system_create on: error, execution, communication,
 * platform, done, state and invoke (the `invoke` of both done.invoke.<id> and
 * error.communication.invoke.<id>.<reason>). They are interned by
 * ta_event_name_table_init, the one safe point, and never added later, so
 * code that builds or matches a generated name finds them with
 * ta_atom_find or reads `invoke_segment`, and never needs to intern them.
 *
 * Lifetime: system. The table does not own the atom table it points at (the
 * system does, and destroys the trie first); the trie it embeds owns its
 * nodes and index. Names are never removed.
 *
 * When it allocates. ta_event_name_table_init, and the three functions that
 * intern (ta_event_name_child, ta_event_name_intern_atoms_internal,
 * ta_event_name_intern_dotted) when a name or atom is new. The latter two
 * run at registration and compile time, and today also for invoke IDs
 * inside a macrostep; Phase 4 (4.2) of the ring protocol removes run-time
 * interning (see trie.h and atom.h for how growth is requested meanwhile).
 * Matching and lookup (the trie functions) never allocate.
 *
 * Example:
 *
 *     ta_event_name_id name;
 *     ta_event_name_intern_dotted(&system->event_names, (ta_str){ "done.invoke.x", 13 }, &name);
 *     ta_trie_under(&system->event_names.trie, name, TA_EVENT_NAME_DONE_INVOKE);   // true
 */

typedef struct {
    ta_atom_table *atoms;
    /* Event names are the trie's nodes. Its max_depth is the system's eventNameDepth (ADR 0092). */
    ta_atom_trie trie;
    /* The atom of `invoke`, the segment of error.communication.invoke.<id>.<reason>.
     * System lifetime; an index into `atoms`, set once by ta_event_name_table_init. */
    ta_atom invoke_segment;
} ta_event_name_table;

enum {
    TA_EVENT_NAME_ROOT = TA_TRIE_ROOT,
    TA_EVENT_NAME_ERROR = 1u,
    TA_EVENT_NAME_ERROR_EXECUTION = 2u,
    TA_EVENT_NAME_ERROR_COMMUNICATION = 3u,
    TA_EVENT_NAME_ERROR_PLATFORM = 4u,
    TA_EVENT_NAME_DONE = 5u,
    TA_EVENT_NAME_DONE_STATE = 6u,
    TA_EVENT_NAME_DONE_INVOKE = 7u
};

/* The ceiling of eventNameDepth (ADR 0092): it sizes segment arrays. */
#define TA_EVENT_NAME_MAX_DEPTH TA_TRIE_MAX_DEPTH

/*
 * ta_event_name_table_init makes the table over `atoms` with the built-in
 * names, the depth limit at its ceiling (the system lowers
 * table->trie.max_depth afterwards). Safe point: it allocates. On failure
 * nothing is left allocated.
 */
ta_result ta_event_name_table_init(ta_event_name_table *table,
                                   ta_allocator allocator,
                                   ta_atom_table *atoms);
/* ta_event_name_table_destroy frees the trie; every name ID is dead. */
void ta_event_name_table_destroy(ta_event_name_table *table);
/*
 * ta_event_name_intern_atoms_internal interns the name made of `segments`,
 * each of which must be an atom of the atom table, at most max_depth of them.
 * TA_ERR_INVALID otherwise. Allocates for a new name.
 */
ta_result ta_event_name_intern_atoms_internal(ta_event_name_table *table,
                                              const ta_atom_list *segments,
                                              ta_event_name_id *out);
/*
 * ta_event_name_child interns one more segment below `parent`;
 * TA_ERR_INVALID if parent is no name, the segment is no atom of the atom
 * table, or the name would be deeper than max_depth. Allocates for a new name.
 */
ta_result ta_event_name_child(ta_event_name_table *table,
                              ta_event_name_id parent,
                              ta_atom segment,
                              ta_event_name_id *out);
/* Interns a dotted name; used by the compiler for authored names only. Allocates. */
ta_result ta_event_name_intern_dotted(ta_event_name_table *table, ta_str dotted, ta_event_name_id *out);
