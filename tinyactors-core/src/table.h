#pragma once

#include "types.h"
#include "layout.h"
#include "vec.h"

/*
 * Package table is block B3 of the ring protocol's catalog: an open
 * addressing hash table from a 32-bit hash to a 32-bit value, that grows only
 * when its owner says so.
 *
 * What it is for. Core keeps four tables that map a key to a record: atoms
 * (text to atom), the atom trie (parent and segment to node), addresses
 * (family and name to record) and sessions (session ID to node). Before this
 * block each had its own chained buckets and its own rehash or linear
 * hashing code, two of them near copies of each other. This block is the
 * part they share: the slots, the probing, the removal and the growth. The
 * owner keeps what is specific to it, the key and the record.
 *
 * What it stores. A slot is a pair (hash, value). The table never sees a
 * key: the caller hashes its key to 32 bits (hash.h has the functions),
 * inserts the pair, and on lookup walks the values that carry the same hash
 * and compares the real key in its own record. The value is a u32 that
 * names the record: an index into the owner's vector or pool. That is
 * also the stable name of an entry. The slot itself moves, so no one may keep
 * a pointer to a slot. TA_TABLE_NONE (0xFFFFFFFF) is the one value that
 * cannot be stored, because it marks an empty slot.
 *
 * Two modes.
 *
 *   TA_TABLE_FIXED has capacity set once at init, as max_entries. It is one
 *     page of ceil(max_entries * 4 / 3) + 1 slots, so it is never more than
 *     three quarters full and always has an empty slot. ta_table_insert never
 *     allocates and answers TA_ERR_NOMEM when max_entries are held; there is
 *     nothing to grow. For tables whose bound is a configured number
 *     (waiting lists bounded by poolEnvelopes).
 *
 *   TA_TABLE_GROWING starts with one page of page_slots slots and gains one
 *     page per ta_table_grow. It is linear hashing (Litwin) over fixed pages:
 *     the pages are found through a directory, a grow adds one page at the
 *     end and splits one existing page into itself and the new one, and so
 *     one grow costs O(page_slots) and allocates one page (plus a directory
 *     block now and then). It never copies the whole table. This is
 *     deliberate. A single array that doubles would allocate a block that
 *     grows with the population, and one spawn would pay to copy it: the
 *     largest block must be flat (ADR 0063, checked by scale_test).
 *
 * Growth is the owner's call, never insert's. ta_table_insert does not
 * allocate, so it can run where allocation is not allowed; ta_table_grow
 * allocates, so it runs at a safe point of the owner's choosing.
 * ta_table_wants_grow says when it is due: the table holds more than 3/8 of
 * its slots (on average; a page that has not been split yet can be up to
 * twice as full as the average). An owner that cannot grow right now, or
 * whose grow failed, carries on: the pages simply fill, and insert answers
 * TA_ERR_NOMEM only when the page a hash belongs to has no empty slot, which
 * at the 3/8 mark takes a page more than eight standard deviations above
 * the average. The exception is a crafted set of keys that collide in all
 * 32 bits of the hash: no number of splits separates identical hashes, so
 * enough of them fill one page and insert fails, where a chained table would
 * only slow down. Nothing in core hashes attacker-chosen text to a table
 * whose failure matters more than the failure of the allocation it sits
 * beside.
 *
 * Lifetime. The table is embedded in its owner and lives as long as the
 * owner does: system lifetime for atoms, the trie, addresses and sessions.
 * It owns its pages and its directory and nothing else; the records the
 * values name belong to the owner. ta_table_destroy frees every page. A
 * ta_table_probe is a call-lifetime cursor (below).
 *
 * Allocation. The only functions that allocate are ta_table_init (the first
 * page and the directory) and ta_table_grow (one page, and a directory block
 * when the directory is full). Both are for safe points. ta_table_insert,
 * ta_table_remove, the probe functions, ta_table_wants_grow, ta_table_count
 * and ta_table_bytes never allocate, and a failure in insert or remove
 * changes nothing. A page is freed with the exact size and alignment it was
 * allocated with; every size is computed with ta_checked_mul, so a size that
 * does not fit is TA_ERR_NOMEM before any allocation, not a short block.
 *
 * Invariants.
 *
 *   - A slot is empty when its value is TA_TABLE_NONE; all other slots hold
 *     an entry. There are no tombstones: ta_table_remove shifts the rest of
 *     the probe sequence back (Knuth's algorithm R), so a table that has had
 *     a million inserts and removes probes like a fresh one.
 *   - An entry lives in the page its hash selects and, in it, in the run of
 *     occupied slots that starts at its home slot. Probing is linear and
 *     wraps inside the page, so a page is a ring; a page is never searched
 *     for more than page_slots steps, which makes a full page safe.
 *   - Page selection takes the low bits of the hash (linear hashing); the
 *     home slot in the page comes from the high bits of hash * 0x9E3779B1,
 *     so the two do not correlate even though every entry of a page shares
 *     its low bits.
 *   - Pages = base + split, base a power of two. Entries of the pages before
 *     `split` are selected by one more bit than the others.
 *   - count is the number of entries. Duplicate (hash, value) pairs are the
 *     caller's bug; equal hashes with different values are normal.
 *
 * Complexity. Insert, remove and a probe are O(1) expected: a run of
 * occupied slots in a page that is at most about 3/4 full, and usually
 * around 3/8. ta_table_grow is O(page_slots) (it splits one page).
 * ta_table_destroy is O(pages).
 *
 * Failure. Init and grow answer TA_ERR_NOMEM, or TA_ERR_INVALID for
 * parameters that make no table, and leave the table as it was (init leaves
 * it zeroed). Insert answers TA_ERR_NOMEM when there is no empty slot in the
 * hash's page (or, FIXED, when max_entries are held) and TA_ERR_INVALID for
 * TA_TABLE_NONE; the table is unchanged.
 *
 * Example, a table of records found by name; the vector `records` is the
 * owner's and the index in it is the value:
 *
 *     ta_table index;
 *     ta_table_init(&index, allocator, &(ta_table_params){
 *         .mode = TA_TABLE_GROWING, .page_slots = 256 });
 *
 *     uint32_t hash = ta_fnv1a32(TA_FNV1A32_BASIS, name.ptr, name.len);
 *     ta_table_probe probe;
 *     uint32_t found = TA_TABLE_NONE;
 *     ta_table_probe_start(&index, hash, &probe);
 *     for (uint32_t candidate; ta_table_probe_next(&probe, &candidate);) {
 *         if (ta_str_equal(record_name(candidate), name)) { found = candidate; break; }
 *     }
 *     if (found == TA_TABLE_NONE) {                 // intern a new record
 *         if (ta_table_wants_grow(&index)) {
 *             (void)ta_table_grow(&index);          // safe point; failure only postpones
 *         }
 *         ta_table_insert(&index, hash, new_record_index);
 *     }
 */

/* TA_TABLE_NONE is the one value a table cannot store: it marks an empty slot. */
#define TA_TABLE_NONE UINT32_MAX

/* ta_table_mode is TA_TABLE_FIXED (capacity set at init, never grows) or TA_TABLE_GROWING (grows by pages when told). */
typedef enum ta_table_mode { TA_TABLE_FIXED, TA_TABLE_GROWING } ta_table_mode;

/*
 * ta_table_params says what table to make. For TA_TABLE_FIXED, max_entries
 * (at least 1) is the most entries the table will ever hold and page_slots is
 * ignored. For TA_TABLE_GROWING, page_slots (at least 16, at most 2^20) is the
 * number of slots in each page and max_entries is ignored. Choose a page of
 * about 2 to 8 KiB: small tables waste little, and the largest block a grow
 * allocates stays small and flat.
 */
typedef struct ta_table_params {
    ta_table_mode mode;
    uint32_t max_entries;
    uint32_t page_slots;
} ta_table_params;

/* ta_table_slot is one slot: the caller's hash and its value, TA_TABLE_NONE when empty. Private to table.c and the tests. */
typedef struct ta_table_slot {
    uint32_t hash;
    uint32_t value;
} ta_table_slot;

/*
 * ta_table is the control block, embedded in the owner (system lifetime).
 * Its fields are private to table.c. `pages` is a directory of
 * ta_table_slot * (one entry in FIXED mode).
 */
typedef struct ta_table {
    ta_allocator allocator;
    ta_vec pages;
    ta_table_mode mode;
    uint32_t page_slots;
    /* Bytes of one page, computed once with ta_layout at init, so every free matches its allocation. */
    size_t page_bytes;
    uint32_t base;
    uint32_t split;
    uint32_t count;
    uint32_t max_entries;
} ta_table;

/*
 * ta_table_probe is a cursor over the entries with one hash, a call-lifetime
 * stack value. Any insert, remove or grow on the table invalidates it.
 */
typedef struct ta_table_probe {
    const ta_table_slot *page;
    uint32_t slots;
    uint32_t hash;
    uint32_t position;
    uint32_t left;
} ta_table_probe;

/*
 * ta_table_init makes an empty table and allocates its first page and its
 * directory; this is the only allocation of a FIXED table. A safe point.
 * TA_ERR_INVALID for parameters that make no table, TA_ERR_NOMEM when a size
 * does not fit or an allocation fails; *table is then zeroed and owns
 * nothing, and ta_table_destroy on it is a no-op.
 */
ta_result ta_table_init(ta_table *table, ta_allocator allocator, const ta_table_params *params);

/*
 * ta_table_destroy frees every page and the directory with the sizes they
 * were allocated with and zeroes *table. Idempotent on a zeroed table. The
 * values' records are not touched: they are the owner's.
 */
void ta_table_destroy(ta_table *table);

/*
 * ta_table_insert adds the pair (hash, value). It never allocates. A pair
 * already present must not be added again; the same hash with another value
 * may be. TA_ERR_INVALID when value is TA_TABLE_NONE; TA_ERR_NOMEM when
 * the table (FIXED) or the page of `hash` (GROWING) has no empty slot. In
 * both failures nothing changed. Call ta_table_grow first when
 * ta_table_wants_grow, at a safe point.
 */
ta_result ta_table_insert(ta_table *table, uint32_t hash, uint32_t value);

/*
 * ta_table_remove removes the entry that holds exactly (hash, value) and
 * answers true, or answers false when there is none. It shifts later entries
 * of the same run back, so there are no tombstones. It never allocates and
 * never shrinks the table: a page stays allocated until destroy.
 */
bool ta_table_remove(ta_table *table, uint32_t hash, uint32_t value);

/*
 * ta_table_probe_start begins a walk over the values stored with `hash`.
 * ta_table_probe_next stores the next one in *value and answers true, or
 * answers false at the end. The order is not promised and the values are
 * candidates only: the caller compares its real key. Neither allocates.
 * The cursor is valid until the next insert, remove or grow of the table.
 */
void ta_table_probe_start(const ta_table *table, uint32_t hash, ta_table_probe *probe);
bool ta_table_probe_next(ta_table_probe *probe, uint32_t *value);

/*
 * ta_table_wants_grow answers true when a GROWING table holds more than 3/8
 * of its slots, which is when its owner should call ta_table_grow at its
 * next safe point. Always false for FIXED. Never allocates.
 */
bool ta_table_wants_grow(const ta_table *table);

/*
 * ta_table_grow adds one page and splits one page between itself and the new
 * one: O(page_slots), one page-sized allocation, plus a directory block
 * when the directory is full. Safe points only. TA_ERR_NOMEM (a size that
 * does not fit, or an allocation that failed) leaves the table's pages and
 * entries as they were, and is only a postponement (a directory block that
 * was already enlarged is kept; the next try needs it). TA_ERR_INVALID for a
 * FIXED table. It is not called from insert; the owner decides when.
 */
ta_result ta_table_grow(ta_table *table);

/* ta_table_count is the number of entries held. */
uint32_t ta_table_count(const ta_table *table);

/*
 * ta_table_bytes is the number of bytes the table has asked its allocator for
 * and not returned: every page plus the directory block. For the scale
 * figures and the sized-free tests.
 */
size_t ta_table_bytes(const ta_table *table);
