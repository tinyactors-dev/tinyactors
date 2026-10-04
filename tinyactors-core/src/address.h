#pragma once

#include "arena.h"
#include "atom.h"
#include "pool.h"
#include "table.h"

/*
 * Package address is the table of interned named addresses (ADR 0065),
 * built on the index pool (pool.h, block A5) and the hash table (table.h,
 * block B3).
 *
 * What it is for. A named actor is found by an address, a (family, name)
 * pair. A system keeps one record per distinct pair that anyone holds, so
 * two equal addresses are the same pointer, and the pointer is the public
 * handle (ta_address): hosts compare it, hash it and keep it in their own
 * directory. A record is reference counted and is freed with its last
 * reference.
 *
 * How it is built. A record is one individually allocated block, variable in
 * size: a 28-byte header and the text. It cannot be an item of a pool, since
 * its address is the handle and the text is variable, so the pool holds
 * only the directory: the item at index i is a pointer to the record with
 * `index` i. The hash table maps hash(family, NUL, name) to that index. There
 * are no pointer chains. The record keeps its own index, so release finds
 * its slot without searching. Before this unit the table was paged linear
 * hashing with a chain through the records, a near copy of the one in
 * session.c.
 *
 * Lifetimes. Each thing here has exactly one.
 *
 *   - The control block ta_address_table (with its pool and its index) is
 *     system lifetime: embedded in the system, initialised by
 *     ta_address_table_init (which allocates nothing) and destroyed by
 *     ta_address_table_destroy, last, after the session table gave back the
 *     addresses unloaded sessions hold.
 *   - The pool pages, the index pages and their directories are system
 *     lifetime: allocated at the first intern that needs them and freed only
 *     by destroy. They never shrink, and the records never move: do not call
 *     ta_pool_trim on this pool, since a trimmed page would take indexes the
 *     records still hold.
 *   - A record is system lifetime ended by its reference count: it is
 *     allocated by the first intern of its (family, name) and freed by the
 *     release of its last reference, or by destroy for records still held
 *     (references held past destroy are dangling). The holders are actors
 *     (their self), events, delayed sends, the host's directory, and the
 *     unloaded session node of the session table. The pointer is valid for
 *     as long as the holder's reference.
 *   - A pool item (the directory entry) and the record's u32 `index` live
 *     exactly as long as the record: taken at intern, given at the last
 *     release. The index is held only by the record itself and by the index
 *     slot.
 *   - A detached copy (ta_address_detach, and an offload reader's) is parked:
 *     it belongs to a session image or an offload arena. It has no table
 *     and index 0, is never counted, retain and release ignore it, and it
 *     is freed with its arena, never by itself.
 *
 * When it allocates. ta_address_table_intern is the only allocating function,
 * and only on a miss (a name not interned yet): the record, a refill of the
 * pool, the first page of the index, a grow of the index, a prepare of
 * the page the name hashes to. A hit never allocates. Release, retain, view,
 * find and destroy never allocate. Intern is called at the safe points that
 * spawn, find and load are, and, today, from the SCXML URI target parse in a
 * macrostep (scxml_uri.c), exactly as the atom table interns there; phase 4
 * of the plan moves that parse out of the macrostep.
 *
 * Crafted names. An address name can come from outside (an HTTP endpoint
 * that maps a request field to a name), and the hash of it is FNV-1a over 32
 * bits, which is easy to collide on purpose: its step is invertible, so a
 * meet-in-the-middle search finds as many names with one hash as an
 * attacker likes. Linear hashing never separates identical hashes, so no
 * number of splits helps. This table therefore grows instead of refusing:
 * intern calls ta_table_prepare, which doubles the one page those names fill,
 * at the safe point the intern is at, so the insert that follows always has
 * a slot. The names cost their sender probe length (a lookup of a name in
 * such a set is linear in the set) and the memory of their own entries, and
 * never a refused spawn or load while memory allows. A per-system seed was
 * rejected: core has no entropy source and must stay deterministic and
 * replayable, and a seed only makes a collision set improbable. The largest
 * block is flat for honest names (scale_test checks it); under attack it
 * grows with the set, by doubled pages.
 *
 * Invariants.
 *
 *   - Every interned record, and only those, has an item in the pool and an
 *     entry (hash of its text, record->index) in the index; the item holds
 *     the record's pointer, and record->index is its index (never 0).
 *   - references of an interned record is at least 1; at 0 it is freed.
 *   - Equal (family, name) pairs have one record.
 *   - `unloaded` is 0, or the session pool index of the node in the session
 *     table that holds this address (that node holds one reference, so the
 *     record outlives its `unloaded`).
 *
 * Failure. ta_address_table_intern answers TA_ERR_NOMEM, with nothing
 * interned and no record leaked (capacity it added to the pool or index
 * may stay), also for a text length whose record size does not fit a
 * size_t. Everything else is infallible.
 *
 * Complexity. Intern, find and release are O(1) expected plus the
 * comparison of the name; a grow or a doubled page is O(page) in a rare
 * call. Space: a named actor's address costs the 28-byte header and text,
 * 8 bytes for its pool item and about 18 bytes of index slots (7/16 load).
 * Destroy is O(records).
 *
 * Example, a host that spawns and finds named actors:
 *
 *     ta_address *address;
 *     if (ta_address_intern(system, &(ta_actor_address){ .family = family, .name = name }, &address) != TA_OK) {
 *         return TA_ERR_NOMEM;                  // nothing interned
 *     }
 *     ... use the pointer as the handle; equal addresses are equal pointers ...
 *     ta_address_release(address);              // the last release frees the record
 */

typedef struct ta_address_table ta_address_table;

struct ta_address {
    /* NULL for a detached record: a session image's own copy. */
    ta_address_table *table;
    /* The session pool index of the unloaded session at this address (ADR 0066), or 0. */
    uint32_t unloaded;
    uint32_t references;
    /* This record's index in the address pool: its name in the table. 0 when detached. */
    uint32_t index;
    uint32_t family_len;
    uint32_t name_len;
    /*
     * The family, a NUL, the name, a NUL. The record keeps no copy of its
     * hash: release recomputes it from the text (a few bytes of FNV-1a),
     * which saves 4 bytes on every named actor.
     */
    char text[];
};

/* ta_address_table is the control block (system lifetime), embedded in the system. Its fields are private to address.c. */
struct ta_address_table {
    ta_allocator allocator;
    ta_pool records; /* items are ta_address *: the directory from a u32 index to a record */
    ta_table index;  /* hash(family, name) to the record's pool index */
};

/*
 * ta_address_table_init makes an empty table. It allocates nothing (the
 * pool and the index get their first pages with the first intern), so it
 * cannot fail and needs no safe point.
 */
void ta_address_table_init(ta_address_table *table, ta_allocator allocator);

/*
 * ta_address_table_destroy frees every record, held or not, then the index
 * and the pool, with sized frees, and zeroes *table; references held past
 * this are dangling. Idempotent on a zeroed table.
 */
void ta_address_table_destroy(ta_address_table *table);

/*
 * ta_address_table_intern stores in *out the record of `address`, with one
 * more reference than before (a new record starts with one): the caller
 * releases it with ta_address_release. A hit allocates nothing. A miss
 * allocates (see the package comment) and is for safe points only.
 * TA_ERR_NOMEM leaves nothing interned.
 */
ta_result ta_address_table_intern(ta_address_table *table, const ta_actor_address *address, ta_address **out);

/*
 * ta_address_record_bytes stores in *bytes the size of an address record
 * with these text lengths: the header (up to `text`), the family, a NUL, the name, a NUL.
 * It is the one formula for the interned record, a detached copy and the
 * offload bytes, computed with ta_layout, so a size that does not fit a
 * size_t (possible on a 32-bit host, or with lengths read from an image) is
 * TA_ERR_NOMEM and *bytes is not to be used. Never allocates.
 */
ta_result ta_address_record_bytes(uint32_t family_len, uint32_t name_len, size_t *bytes);

/*
 * ta_address_bytes is the size of a record that exists (interned or
 * detached): the formula above for its own lengths, which fitted when it was
 * made. It is what a sized free and an offload write use.
 */
size_t ta_address_bytes(const ta_address *address);

/*
 * ta_address_detached_init makes `copy`, a bitwise copy of a record, a
 * detached one: no table, no index, no session, not counted.
 */
void ta_address_detached_init(ta_address *copy);

/*
 * ta_address_detach copies `address` into `arena` as a detached record: the
 * same strings, no table, no counting. Retain and release do nothing on it.
 * Session images carry their addresses this way, so an image does not
 * depend on the system that captured it (ADR 0028); loading interns them
 * again. The copy is parked: it lives as long as the arena. TA_ERR_NOMEM
 * when the arena cannot hold it.
 */
ta_result ta_address_detach(ta_arena *arena, const ta_address *address, ta_address **out);

/* ta_address_in answers whether `address` is an interned record of `table` (not NULL, not a detached copy). */
bool ta_address_in(const ta_address *address, const ta_address_table *table);
