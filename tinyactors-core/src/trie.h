/**
 * @file trie.h
 *
 * # Trie of atoms
 *
 * `ta_trie` interns paths of atoms. Each path, such as the segments
 * `error`, `communication`, `invoke` of the event name
 * `error.communication.invoke`, becomes one node, keyed by its parent node
 * and its last segment, and the node's ID names the path for as long as
 * the trie lives. Two paths with the same segments in the same order have
 * the same ID, so comparing IDs compares paths, and a path's prefixes are
 * its ancestors, so token-prefix matching walks parents and never reparses
 * text (ADR 0007).
 *
 * ## Contract and invariants
 *
 * - **Node 0 is the root**, `TA_TRIE_ROOT`: the empty path, made by
 *   `ta_trie_init`. Every other node has a parent, one segment (an atom
 *   of the trie's atom table) and a depth, its parent's plus one.
 * - **IDs are assigned in order:** the n-th node added has ID n, and an ID
 *   never changes or names another path while the trie lives. Nothing is
 *   ever removed.
 * - **One node per (parent, segment).** A node is found through a hash
 *   table (`table.h`) in growing mode, keyed by the 32-bit FNV-1a hash
 *   (`hash.h`) of the pair's two `uint32_t`s in memory order, from
 *   `TA_HASH_FNV1A32_BASIS ^ 0x9e3779b9`. Its values are node IDs, never 0
 *   because the root is never a child; it compares keys by reading the
 *   node's parent and segment. The hash table grows only in a call that
 *   adds nodes, before it adds them.
 * - **Depth is bounded:** no node is deeper than the trie's `max_depth`,
 *   set at init, at most `TA_TRIE_MAX_DEPTH`, which sizes the arrays
 *   `ta_trie_path` writes.
 * - Paths are written root first. The trie keeps no text: a segment's text
 *   is its atom's, in the atom table.
 * - A node ID is process-local. A portable durability encoding records the
 *   path's segments as text (or another stable dictionary key) and interns
 *   them again after a restart; an in-process session image may keep IDs.
 *
 * ## Memory
 *
 * The trie owns its nodes (a `ta_vec`) and its hash table, both from the
 * allocator given to `ta_trie_init`, which it borrows and which must
 * outlive it, until `ta_trie_destroy`. It borrows the atom table, which
 * must outlive it too; `ta_trie_intern_dotted` interns atoms there.
 *
 * - `ta_trie_init`, `ta_trie_child`, `ta_trie_intern_path` and
 *   `ta_trie_intern_dotted` may allocate: they run where the caller may
 *   allocate (system creation, registration), never in the middle of a
 *   macrostep that must not allocate.
 * - `ta_trie_find`, `ta_trie_valid`, `ta_trie_has_prefix` and
 *   `ta_trie_path` never allocate and may run inside a macrostep. Nor do
 *   the calls that add nodes, for a path the trie holds already: they
 *   grow only for nodes they add.
 * - `ta_trie_destroy` only frees.
 *
 * Nothing outside the trie keeps a pointer into its nodes or slots: both
 * move when they grow.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_trie_find`, `ta_trie_child` | expected O(1): one hash and a probe of the hash table; a child that grows the nodes or the table moves each once, amortized O(1) |
 * | `ta_trie_intern_path` | O(segments) |
 * | `ta_trie_intern_dotted` | O(length of the text) plus O(segments) |
 * | `ta_trie_valid` | O(1) |
 * | `ta_trie_has_prefix`, `ta_trie_path` | O(depth): walks the parents |
 * | `ta_trie_init` | O(1) |
 *
 * ## Failure
 *
 * - `TA_ERR_INVALID`: a parent that is not a node, a segment that is not
 *   an atom, a path that is empty or deeper than `max_depth`, a dotted
 *   text that is empty or has an empty segment, a `max_depth` of 0 or above
 *   `TA_TRIE_MAX_DEPTH`.
 * - `TA_ERR_NOT_FOUND`: `ta_trie_find` for a path that is not a node.
 * - `TA_ERR_NOMEM`: the nodes or the hash table cannot grow;
 *   `TA_ERR_FULL`: the hash table holds `TA_TABLE_MAX_KEYS` nodes.
 * - A failed call adds no node. The room it grew may stay, and
 *   `ta_trie_intern_dotted` may leave atoms it interned in the atom table.
 *
 * ## Users
 *
 * **Event names** (ADR 0007): the system's trie, `event_names` in
 * `ta_system`, whose node IDs are `ta_event_name_id`s. Its `max_depth` is
 * the system's `eventNameDepth` (ADR 0092), and system creation adds the
 * built-in names first (`TA_EVENT_NAME_ERROR` and the others in
 * `system.h`). A descriptor `P` matches an event `E` when `P` is `E` or an
 * ancestor of it (`ta_trie_has_prefix`), case-sensitively, as atoms are.
 * Compiled transitions keep `ta_event_pattern` records of segments, whose
 * wildcard segment matches zero or more segments anywhere in the
 * descriptor; that matcher lives with the definition and compares atoms.
 * Core builds the names it generates from segments, `done.state.<id>`,
 * `done.invoke.<id>`, `error.communication.invoke.<id>.<reason>`, with
 * `ta_trie_child`, rather than formatting and reparsing dotted text. Event
 * names are authored names: those of a definition, interned when it is
 * registered, and those core names at system creation.
 *
 * Anything else named by a dotted path of authored segments may have a
 * trie of its own.
 */
#pragma once

#include "atom.h"
#include "table.h"
#include "vec.h"
#include "../include/tinyactors.h"

/** The ID of a node, and of the path it names. */
typedef uint32_t ta_trie_id;

/** The root: the empty path, node 0. */
#define TA_TRIE_ROOT 0u

/** The deepest any trie goes; it sizes the arrays `ta_trie_path` writes. */
#define TA_TRIE_MAX_DEPTH 64u

/** One node: its parent, its last segment and its depth (the root's is 0). */
typedef struct {
    ta_trie_id parent;
    ta_atom segment;
    uint32_t depth;
} ta_trie_node;

/** A key of the trie: a segment below a parent. */
typedef struct {
    ta_trie_id parent;
    ta_atom segment;
} ta_trie_edge;

/** What `ta_trie_init` needs. */
typedef struct {
    /** Where the nodes and the hash table come from; borrowed, must outlive the trie. */
    ta_allocator allocator;
    /** The atoms segments are; borrowed, must outlive the trie. */
    ta_atom_table *atoms;
    /** The most segments a path has, from 1 to `TA_TRIE_MAX_DEPTH`. */
    uint32_t max_depth;
} ta_trie_params;

/** A trie of atoms; see the file comment. The fields are for reading only. */
typedef struct {
    ta_allocator allocator;
    ta_atom_table *atoms;
    /** One ta_trie_node per node, indexed by ID. */
    ta_vec nodes;
    /** (parent, segment) hash to node ID. */
    ta_table index;
    uint32_t max_depth;
} ta_trie;

/**
 * Makes `*trie` a trie holding only the root, as `params` describes.
 * Allocates; runs at a safe point. Answers `TA_ERR_INVALID` for a
 * `max_depth` of 0 or above `TA_TRIE_MAX_DEPTH`, and `TA_ERR_NOMEM` when it
 * cannot allocate; on failure `*trie` holds nothing and may be destroyed.
 */
ta_result ta_trie_init(ta_trie *trie, const ta_trie_params *params);

/** Frees the nodes and the hash table; IDs it answered name nothing. Only frees. */
void ta_trie_destroy(ta_trie *trie);

/**
 * Looks up the node of `edge` without adding it and stores its ID in
 * `*out`. Never allocates; may run inside a macrostep. Answers
 * `TA_ERR_INVALID` when the parent is not a node or the segment not an
 * atom, and `TA_ERR_NOT_FOUND` when the trie has no such node, leaving
 * `*out` alone.
 */
ta_result ta_trie_find(const ta_trie *trie, ta_trie_edge edge, ta_trie_id *out);

/**
 * Stores in `*out` the node of `edge`, adding it, with the next ID, when
 * it is new. May allocate: grows the nodes and the hash table before it
 * adds. Answers `TA_ERR_INVALID` when the parent is not a node, the
 * segment not an atom, or the child would be deeper than `max_depth`;
 * `TA_ERR_NOMEM` or `TA_ERR_FULL` when it cannot grow. A failure adds no
 * node.
 */
ta_result ta_trie_child(ta_trie *trie, ta_trie_edge edge, ta_trie_id *out);

/**
 * Stores in `*out` the node of the path `segments`, root first, adding the
 * nodes it lacks. Borrows `segments`. May allocate. Answers
 * `TA_ERR_INVALID` for a NULL or empty list, a path deeper than
 * `max_depth` or a segment that is not an atom; `TA_ERR_NOMEM` or
 * `TA_ERR_FULL` when it cannot grow. It makes room for exactly the nodes
 * it lacks first, so a failure adds no node, and a path the trie holds
 * never allocates.
 */
ta_result ta_trie_intern_path(ta_trie *trie, const ta_atom_list *segments, ta_trie_id *out);

/**
 * Stores in `*out` the node of the path the text `dotted` names, its
 * segments separated by `.`, interning each segment as an atom and adding
 * the nodes it lacks. Borrows `dotted`. May allocate. Answers
 * `TA_ERR_INVALID` for an empty text, an empty segment or more segments
 * than `max_depth`, before it interns anything; `TA_ERR_NOMEM` or
 * `TA_ERR_FULL` when it cannot grow. A failure adds no node, but atoms it
 * interned stay in the atom table.
 */
ta_result ta_trie_intern_dotted(ta_trie *trie, ta_str dotted, ta_trie_id *out);

/** Whether `id` is a node of the trie. Never allocates. */
bool ta_trie_valid(const ta_trie *trie, ta_trie_id id);

/**
 * Whether `prefix` is `id` or one of its ancestors: token-prefix matching.
 * False when either is not a node. Never allocates; may run inside a
 * macrostep.
 */
bool ta_trie_has_prefix(const ta_trie *trie, ta_trie_id id, ta_trie_id prefix);

/**
 * Writes the segments of `id`'s path to `out`, root first, and answers how
 * many: its depth, 0 for the root or an ID that is not a node. Never
 * allocates; may run inside a macrostep.
 */
uint32_t ta_trie_path(const ta_trie *trie, ta_trie_id id, ta_atom out[TA_TRIE_MAX_DEPTH]);
