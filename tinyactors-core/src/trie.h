#pragma once

#include "atom.h"

/*
 * Package trie is block D7 of the ring protocol's catalog: a trie of atoms.
 * It stores a set of paths, each path a sequence of atoms, so that every
 * path has one small number, its node, and so that "is this path under that
 * one" is a walk up a parent chain instead of a comparison of strings.
 *
 * What it is for. Event names are the one user today (ADR 0007): `done.invoke.x`
 * is the path of atoms (done, invoke, x), interned once, and an event carries
 * the node ID. A descriptor matches an event when its node is an ancestor of
 * the event's, which ta_trie_under answers by walking up from the deeper
 * node to the shallower node's depth. Nothing about events is in this unit.
 * The built-in names (error.execution, done.state, ...), the check that a
 * segment is an atom of the atom table and the dotted text form stay in
 * event_name.c. This unit knows only atoms as numbers.
 *
 * Shape. Node 0 is the root, the empty path; its parent is itself and its
 * segment TA_ATOM_NONE. Every other node has a parent with a smaller ID
 * (a node is made after its parent), a segment and a depth, the number
 * of segments from the root. Node IDs are dense and follow the order the
 * paths were first interned. They appear in traces and images, so they
 * never change once handed out. The nodes are a vector; the (parent,
 * segment) to node index is a ta_table (block B3) holding
 * (hash of the pair, node), where the pair is compared in the node itself.
 * The root is not in the index: it is never a child.
 *
 * Lifetime: its owner's, system for event names. Nodes are never removed, so
 * a node ID is good until ta_trie_destroy; the table has no pointers out, and
 * a pointer from ta_vec_at (used inside this unit only) is good until the
 * next ta_trie_child.
 *
 * When it allocates. Only ta_trie_init (the root, the first index page) and
 * ta_trie_child when it makes a new node: a vector slot and, when the index
 * is due for growth, one page (ta_table_grow). ta_trie_find, valid,
 * segments and under never allocate. Making nodes belongs at registration and
 * compile time, but invoke IDs still make event names in the middle of a
 * macrostep, so ta_trie_child asks for growth itself, just before it inserts;
 * Phase 4 (4.2) of the ring protocol removes run-time interning, after which
 * growth happens only at the safe points that make names. A grow that fails
 * only postpones.
 *
 * Invariants. A node's depth is its parent's plus one, and at most max_depth,
 * which the owner sets at init (the system's eventNameDepth, ADR 0092) and
 * which is at most TA_TRIE_MAX_DEPTH, so a caller can size an array of
 * segments with that constant. Two nodes never share (parent, segment).
 *
 * Failure. TA_ERR_INVALID for a parent that is no node, TA_ATOM_NONE as a
 * segment, a path deeper than max_depth. TA_ERR_NOMEM when memory is out. A
 * failed ta_trie_child leaves no node: the fallible steps (reserve, insert)
 * come before the record of the node.
 *
 * Complexity. ta_trie_child and ta_trie_find are O(1) expected; segments is
 * O(depth); under is O(depth of the deeper node).
 *
 * Example:
 *
 *     ta_atom_trie trie;
 *     ta_trie_init(&trie, allocator, 16);
 *     uint32_t done, invoke;                         // atoms made by the owner
 *     ta_trie_child(&trie, &(ta_trie_key){ TA_TRIE_ROOT, done_atom }, &done);
 *     ta_trie_child(&trie, &(ta_trie_key){ done, invoke_atom }, &invoke);
 *     ta_trie_under(&trie, invoke, done);            // true: done.invoke is under done
 */

/* TA_TRIE_ROOT is the node of the empty path. */
#define TA_TRIE_ROOT 0u
/* TA_TRIE_MAX_DEPTH is the ceiling of max_depth; it sizes the arrays callers pass to ta_trie_segments. */
#define TA_TRIE_MAX_DEPTH 64u
/* Slots per full index page: 8 KiB, so the index grows in small, flat steps. */
#define TA_TRIE_PAGE_SLOTS 1024u
/* Slots of the first page: 512 bytes; ta_table_grow doubles it to TA_TRIE_PAGE_SLOTS before it adds a page. */
#define TA_TRIE_FIRST_SLOTS 64u

typedef struct ta_trie_node {
    uint32_t parent;
    ta_atom segment;
    uint32_t depth;
} ta_trie_node;

typedef struct ta_atom_trie {
    ta_vec nodes;
    ta_table index;
    /* Segments a path may have: at most TA_TRIE_MAX_DEPTH. */
    uint32_t max_depth;
} ta_atom_trie;

/*
 * ta_trie_init makes a trie holding the root. max_depth (1 to
 * TA_TRIE_MAX_DEPTH) bounds every path; TA_ERR_INVALID otherwise. Safe
 * point: it allocates. On failure nothing is left allocated.
 */
ta_result ta_trie_init(ta_atom_trie *trie, ta_allocator allocator, uint32_t max_depth);
/* ta_trie_destroy frees the nodes and the index; every node ID is dead. */
void ta_trie_destroy(ta_atom_trie *trie);

/* ta_trie_key names a child: the node it hangs from and its segment atom. A call-lifetime value, copied by the callee. */
typedef struct ta_trie_key {
    uint32_t parent;
    ta_atom segment;
} ta_trie_key;

/*
 * ta_trie_child stores in *out the node of key->segment below key->parent, making
 * it if it is new. Allocates only for a new node (see the package comment).
 * TA_ERR_INVALID for an unknown parent, TA_ATOM_NONE or a path longer than
 * max_depth; TA_ERR_NOMEM when memory is out. Failure leaves no node.
 */
ta_result ta_trie_child(ta_atom_trie *trie, const ta_trie_key *key, uint32_t *out);
/* ta_trie_find is ta_trie_child without the making: TA_ERR_NOT_FOUND when the node does not exist. Never allocates. */
ta_result ta_trie_find(const ta_atom_trie *trie, const ta_trie_key *key, uint32_t *out);
/* ta_trie_valid says whether `node` is a node of this trie (the root is). */
bool ta_trie_valid(const ta_atom_trie *trie, uint32_t node);
/*
 * ta_trie_segments writes the atoms of the path of `node`, root-first, into
 * out and returns how many (the node's depth); 0 for the root and for an
 * invalid node. `out` has TA_TRIE_MAX_DEPTH places, which is enough for any
 * node. Never allocates.
 */
uint32_t ta_trie_segments(const ta_atom_trie *trie, uint32_t node, ta_atom out[TA_TRIE_MAX_DEPTH]);
/* ta_trie_under says whether `node` is `prefix` or below it; false if either is invalid. Never allocates. */
bool ta_trie_under(const ta_atom_trie *trie, uint32_t node, uint32_t prefix);
