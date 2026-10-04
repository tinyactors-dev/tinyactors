#pragma once

#include "address.h"
#include "pool.h"
#include "table.h"

/*
 * Package session is the session table (ADR 0066), built on the index pool
 * (pool.h, block A5) and the hash table (table.h, block B3).
 *
 * What it is for. A session's ID is the runtime ID of the actor it was
 * spawned as, so a never-paged session is found through the actor table and
 * has no entry here. This table holds the other two kinds, the sessions whose
 * ID is not a resident actor's runtime ID:
 *
 *   - a reloaded resident session: its image was loaded into a new actor, which
 *     has a new runtime ID but keeps the session's ID (`actor` is set); and
 *   - an unloaded named session: its actor is gone, an image holds it, and
 *     the entry keeps it findable by ID and routable by address (`actor` is
 *     NULL, `address` is held).
 *
 * A lookup by ID prefers a resident entry to an unloaded one, so a session
 * that was unloaded and loaded again is never seen as two.
 *
 * How it is built. A node is a 24-byte item of an index pool, found by a u32
 * index that never changes while the node lives and that 0 never names. The
 * hash table maps hash(ID) to that index (the table stores only the pair of
 * 32-bit numbers; the ID is compared in the node). There are no pointer
 * chains. Before this unit the table was a second copy of the address table's
 * paged linear hashing; both now share the table block.
 *
 * Lifetimes. There are four things, and each has exactly one lifetime.
 *
 *   - The control block ta_session_table (with its pool and its index) is
 *     system lifetime: embedded in the system, initialised by
 *     ta_session_table_init (which allocates nothing) and destroyed by
 *     ta_session_table_destroy in ta_system_destroy, before the address
 *     table, because unloaded nodes hold addresses.
 *   - The pool pages and the index pages are system lifetime too: they are
 *     allocated by the first node (the pool by the first refill, the index
 *     by the first ta_session_node_new) and freed only by destroy. They never
 *     shrink. A system that never reloads or unloads a session pays nothing.
 *   - A node lives from ta_session_node_new to ta_session_drop or destroy,
 *     in one of two lifetimes. While it holds a reloaded resident session it
 *     is session lifetime: from the load to the unload or to the actor's
 *     free. While it holds an unloaded session it is parked: from the unload
 *     to the next load at its address, the removal of the name, the
 *     replacement by another unload, or destroy. The holder of a parked node's
 *     index is the address (ta_address.unloaded, until the node is dropped);
 *     a resident node's index is held by nobody, since the actor finds its
 *     node by (session ID, actor) when it is freed, which costs nothing per
 *     actor. Nobody else may keep an index.
 *   - A pointer returned by ta_session_find or ta_session_node_at is
 *     call lifetime: valid until the node is dropped. Nodes do not move.
 *
 * When it allocates. Only ta_session_node_new allocates (an index pool
 * refill, the index's first page, a grow of the index, a prepare of the page
 * the ID hashes to), and it is called at safe points: reconstruct (load) and
 * unload, both before they change any other state. ta_session_insert,
 * ta_session_park, ta_session_drop, ta_session_drop_unloaded,
 * ta_session_node_free, ta_session_node_at, ta_session_find,
 * ta_session_resident, ta_session_id_of and ta_session_route never
 * allocate (an address release can free, never allocate).
 *
 * Crafted IDs. A host can supply the session ID of an image it loads, and
 * the table hashes it to 32 bits, so a host could choose IDs that all share
 * a hash. Linear hashing never separates equal hashes. The table therefore
 * answers growth, not refusal: ta_session_node_new calls ta_table_prepare,
 * which doubles the one page those hashes fill, at this safe point, so
 * ta_session_insert always has a slot. Such a set costs the sender probe
 * length (lookups of those IDs are linear in their number) and memory, and
 * never a refused load while memory allows. The same holds for
 * ta_session_drop_resident, which ta_actor_free calls for a reloaded session:
 * it is O(probe length), so freeing the actors of n crafted equal-hash IDs
 * costs O(n) each. It is probe length only, never a failure.
 *
 * Invariants.
 *
 *   - Every node that is in the index has a non-zero index, and a node
 *     that has been taken but not yet inserted is in no table.
 *   - A node holds a reference to its address if and only if its `address`
 *     is set (it is then an unloaded node, `actor` NULL); that reference
 *     outlives address->unloaded, which names this node or is 0.
 *   - Between ta_session_node_new and the ta_session_insert or
 *     ta_session_park of the same node there is no other insert into this
 *     table (only removals): the room prepare made is that one slot's.
 *
 * Failure. ta_session_node_new answers TA_ERR_NOMEM and takes no node. The
 * pool or the index may be larger than before (a refill or a first page
 * that succeeded stays); nothing else has changed. Everything else is
 * infallible.
 *
 * Complexity. All O(1) expected; ta_session_node_new is O(page) in the
 * rare call that grows the index or doubles a page. Space: 24 bytes per
 * node plus 8 bytes per slot at a load of 7/16 (about 18 bytes), and
 * no per-node allocation.
 *
 * Example, a load that continues a session, its actor's free, and an unload
 * that parks another session at its address:
 *
 *     uint32_t index;                                     // safe point: may allocate
 *     if (ta_session_node_new(&system->sessions,
 *             &(ta_session_node_params){ .id = id, .actor = actor }, &index) != TA_OK) {
 *         return TA_ERR_NOMEM;                            // nothing changed
 *     }
 *     ta_session_insert(&system->sessions, index);        // resident
 *     ...
 *     ta_session_drop_resident(&system->sessions, id, actor);    // actor freed
 *
 *     // unload of a named actor: the same safe point, before other changes
 *     if (ta_session_node_new(&system->sessions,
 *             &(ta_session_node_params){ .id = id }, &index) != TA_OK) {
 *         return TA_ERR_NOMEM;                            // nothing changed
 *     }
 *     // `address` is the ta_address * the actor holds as its self (interned earlier)
 *     ta_session_park(&system->sessions, index, address); // parked at address
 */

typedef struct ta_session_node {
    uint64_t id;
    /*
     * The resident actor holding the session, or NULL when it is unloaded.
     * While the node is free its pool link overlays the low half of this field.
     */
    struct ta_actor *actor;
    /* An unloaded session's address, one reference held; NULL otherwise. */
    ta_address *address;
} ta_session_node;

/*
 * ta_session_table is the control block, embedded in the system (system
 * lifetime). Its fields are private to session.c.
 */
typedef struct ta_session_table {
    ta_allocator allocator;
    ta_pool nodes;
    ta_table index;
} ta_session_table;

/*
 * ta_session_table_init makes an empty table. It allocates nothing (the pool
 * and the index get their first pages with the first node), so it cannot
 * fail and needs no safe point.
 */
void ta_session_table_init(ta_session_table *table, ta_allocator allocator);

/*
 * ta_session_table_destroy releases the address every unloaded node holds,
 * then frees the index and the pool with sized frees, and zeroes *table.
 * It must run before ta_address_table_destroy. Idempotent on a zeroed table.
 */
void ta_session_table_destroy(ta_session_table *table);

/* ta_session_node_params is what a node starts with: the session's ID and its resident actor, if any. */
typedef struct ta_session_node_params {
    uint64_t id;
    struct ta_actor *actor;
} ta_session_node_params;

/*
 * ta_session_node_new takes a node for `params`, stores its index in
 * *out, and makes room in the index for exactly one insert of its ID's hash.
 * The node is not in the index yet: call ta_session_insert (a resident
 * session) or ta_session_park (an unloaded one) before the next safe point,
 * or ta_session_node_free to give it up. It is the only allocating function,
 * for safe points only. TA_ERR_NOMEM takes no node.
 */
ta_result ta_session_node_new(ta_session_table *table, const ta_session_node_params *params, uint32_t *out);

/*
 * ta_session_insert puts a node made by ta_session_node_new into the index,
 * so that ta_session_find sees it. It never fails and never allocates (see
 * the invariant on inserts in between).
 */
void ta_session_insert(ta_session_table *table, uint32_t index);

/*
 * ta_session_park turns a node made by ta_session_node_new into the unloaded
 * session at `address`: it drops the session already parked there, if any,
 * retains `address` for the node, points address->unloaded at the node and
 * inserts it. Never allocates.
 */
void ta_session_park(ta_session_table *table, uint32_t index, ta_address *address);

/*
 * ta_session_drop_resident drops the node of the reloaded session `id` that
 * `actor` holds, found by probing the hash of `id` for the node whose actor
 * it is; nothing if there is none. It is how an actor leaves the table, so
 * the actor does not carry the node's index (8 bytes of every actor, named
 * or not, and only a reloaded session has a node). Never allocates. O(1)
 * expected, linear in the sessions that share the 32-bit hash.
 */
void ta_session_drop_resident(ta_session_table *table, uint64_t id, const struct ta_actor *actor);

/*
 * ta_session_drop removes a node that is in the index and frees it: it
 * leaves the index, clears address->unloaded if that names it, releases its
 * address (which may free the address) and gives the item back. Never
 * allocates. The index is dead afterwards.
 */
void ta_session_drop(ta_session_table *table, uint32_t index);

/*
 * ta_session_drop_unloaded drops the session parked at `address`, if there
 * is one. It replaces the four places that did this by hand.
 */
void ta_session_drop_unloaded(ta_session_table *table, ta_address *address);

/*
 * ta_session_node_free gives up a node that is not in the index (taken by
 * ta_session_node_new and never inserted). Never allocates.
 */
void ta_session_node_free(ta_session_table *table, uint32_t index);

/* ta_session_node_at is the node of an index, or NULL for 0. Valid until the node is dropped. */
ta_session_node *ta_session_node_at(const ta_session_table *table, uint32_t index);

/*
 * ta_session_find answers the entry for `id`: a resident one if there is
 * one, else an unloaded one, else NULL. The pointer is valid until the node is
 * dropped. Never allocates.
 */
const ta_session_node *ta_session_find(const ta_session_table *table, uint64_t id);

/* Whether a resident session holds `id`: a never-paged actor or a reloaded one. */
bool ta_session_resident(const ta_system *system, uint64_t id);
/* The session ID of the resident actor `runtime`, or `runtime` itself. */
uint64_t ta_session_id_of(const ta_system *system, ta_actor_id runtime);
/*
 * Where an SCXML send to `ref` goes (ADR 0066 section 3): a runtime ID that
 * is not a resident actor's is looked up as a session ID. A reloaded session
 * gives its current runtime ID; an unloaded one gives its address, retained,
 * in which case this returns true and the caller releases *out. Otherwise
 * *out is `ref` and this returns false.
 */
bool ta_session_route(const ta_system *system, const ta_actor_ref *ref, ta_actor_ref *out);
