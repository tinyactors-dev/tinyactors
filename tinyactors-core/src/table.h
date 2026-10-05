/**
 * @file table.h
 *
 * # Hash table
 *
 * `ta_table` maps keys to non-zero `uint32_t` values by open addressing.
 * The table never sees a key: the caller hashes it and keeps it in its own
 * storage, and the table keeps, per slot, the key's 32-bit hash and the
 * value. A value is usually an index into the owner's records (0 meaning
 * none), from which the owner reaches the key again.
 *
 * It has two modes, chosen once:
 *
 * - **fixed**: the most keys it holds is set when it is made, which is the
 *   only time it allocates; insert never allocates and answers
 *   `TA_ERR_FULL` once the table holds that many keys.
 * - **growing**: insert never allocates either and answers `TA_ERR_FULL`
 *   when the table is full; the owner makes room first with an explicit
 *   `ta_table_reserve` at a safe point, which grows the slots when they
 *   are needed.
 *
 * ## Keys and comparison
 *
 * A lookup is given a `ta_table_key`: the key's hash, and an `equal`
 * callback with a context that the owner fills in, typically with the key
 * it looks for and its own key storage. The table calls `equal(context,
 * value)` only for slots whose stored hash equals the key's hash, and the
 * callback answers whether the key stored under `value` is the key looked
 * for. The callback must not allocate, change the table or call back into
 * it; it is how the table compares keys that live with the owner, whatever
 * their form (text, a pair of IDs, address bytes in pages).
 *
 * `ta_table_insert` takes only a hash and a value, no key: it does not
 * look for the key first. The owner finds before it inserts, as it must
 * anyway to answer "already there"; inserting a key the table holds
 * already is a programmer error, not checked, and leaves two slots for the
 * key, of which a lookup answers the one nearer the key's home.
 *
 * ## Layout and invariants
 *
 * - The slots are one array of `slot_count` `ta_table_slot`s: 0 before a
 *   growing table first reserves, otherwise a power of two of at least
 *   `TA_TABLE_MIN_SLOTS`. A slot whose value is 0 is empty; that is why a
 *   value is never 0.
 * - **Probe sequence: linear.** A key's home is `hash & (slot_count - 1)`;
 *   a key lives in the first slot from its home, wrapping at the end,
 *   that was empty when it was inserted. Every slot between a key's home
 *   and its slot holds a key, so a lookup walks from the home and stops at
 *   the first empty slot.
 * - **Load factor: at most three quarters.** A table holds at most
 *   `slot_count - slot_count / 4` keys, so at least a quarter of the slots
 *   stay empty and every probe ends. `limit` is the most keys the table
 *   holds now: that load for a growing table, and the capacity it was made
 *   with for a fixed one. `count` is the keys it holds; insert answers
 *   `TA_ERR_FULL` when `count == limit`.
 * - **Delete by backward shift, no tombstones.** Removing a key empties
 *   its slot and moves later keys of the same run back into the hole when
 *   the hole lies between their home and their slot, so the invariant
 *   above holds again and every remaining key stays reachable. A table's
 *   probes never get longer from removals alone.
 * - **Deterministic.** There is no seed and no randomness: the same
 *   sequence of inserts, removals and reserves from the same empty table
 *   gives the same slots, byte for byte, on every host and every run.
 *   Growth reinserts the keys in the order of their old slots.
 * - The table keeps no pointer into the owner's storage and the owner
 *   keeps none into the table: the slots move when a growing table grows.
 *
 * ## Memory
 *
 * The table owns its slots, allocated from the `ta_allocator` given to
 * `ta_table_init`, which it borrows and which must outlive it, until
 * `ta_table_destroy`. The keys, and whatever the callback's context points
 * at, belong to the owner. The `ta_table` itself is embedded where its
 * owner likes.
 *
 * - `ta_table_init` allocates the slots for its capacity, if any; it runs
 *   where the owner is made, at a safe point.
 * - `ta_table_reserve` allocates only when a growing table needs more
 *   slots. It runs only at a safe point, never inside a macrostep.
 * - `ta_table_insert`, `ta_table_find` and `ta_table_remove` never
 *   allocate and may run inside a macrostep.
 * - `ta_table_destroy` only frees.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_table_find`, `_insert`, `_remove` | expected O(1) at a load of at most three quarters: a walk of one run of occupied slots, calling `equal` only on equal hashes |
 * | `ta_table_reserve` | O(1) when there is room; otherwise one allocation and one pass over the old slots, amortized O(1) per key as the slots at least double |
 * | `ta_table_init` | O(1) for an empty growing table; otherwise one allocation and one zeroing pass |
 * | `ta_table_destroy` | one free |
 *
 * ## Failure
 *
 * - `ta_table_init` answers `TA_ERR_INVALID` for an unknown mode or a
 *   fixed table of capacity 0, `TA_ERR_FULL` for a capacity above
 *   `TA_TABLE_MAX_KEYS`, and `TA_ERR_NOMEM` when the slots cannot be
 *   allocated; it then leaves an empty table that holds nothing.
 * - `ta_table_insert` answers `TA_ERR_FULL` when the table holds `limit`
 *   keys, and `TA_ERR_INVALID` for a value of 0.
 * - `ta_table_find` and `ta_table_remove` answer `TA_ERR_NOT_FOUND` for a
 *   key the table does not hold.
 * - `ta_table_reserve` answers `TA_ERR_FULL` when the room asked for would
 *   pass a fixed table's capacity or `TA_TABLE_MAX_KEYS`, and
 *   `TA_ERR_NOMEM` when the larger slots cannot be allocated.
 * - A failed call changes nothing: the same slots, count and limit.
 *
 * ## Users
 *
 * The atom table, whose values are atoms plus one, and the trie of atoms
 * behind event names, keyed by (parent, segment). The address and session
 * tables do not use it: they grow by linear hashing, one bucket split per
 * insert, so that no single insert's cost grows with the number of named
 * actors (ADR 0065), where this table rehashes all its keys at once when
 * it grows.
 */
#pragma once

#include "../include/tinyactors.h"

/** The fewest slots a table that has slots has. */
#define TA_TABLE_MIN_SLOTS 8u
/** The most slots a table has: 2^31. */
#define TA_TABLE_MAX_SLOTS 0x80000000u
/** The most keys a table holds: three quarters of `TA_TABLE_MAX_SLOTS`. */
#define TA_TABLE_MAX_KEYS (TA_TABLE_MAX_SLOTS - TA_TABLE_MAX_SLOTS / 4u)

/** How a table finds room for more keys. */
typedef enum {
    /** Its capacity is set by `ta_table_init`, which is all it allocates. */
    TA_TABLE_FIXED = 1,
    /** It grows its slots, but only in `ta_table_reserve`. */
    TA_TABLE_GROWING = 2
} ta_table_mode;

/** What `ta_table_init` needs. */
typedef struct {
    /** Where the slots come from; borrowed, and must outlive the table. */
    ta_allocator allocator;
    ta_table_mode mode;
    /**
     * The keys the table holds without growing: for a fixed table, the
     * most it ever holds (at least 1); for a growing table, the room made
     * at once (0 allocates nothing). At most `TA_TABLE_MAX_KEYS`.
     */
    uint32_t capacity;
} ta_table_params;

/**
 * Answers whether the key the owner stores under `value` is the key in
 * `context`. Called only on slots whose hash equals the key's. Must not
 * allocate, change the table or call into it.
 */
typedef bool (*ta_table_equal_fn)(const void *context, uint32_t value);

/** A key to look up: its hash, and how to compare it with stored keys. */
typedef struct {
    uint32_t hash;
    ta_table_equal_fn equal;
    /** Passed to `equal`; borrowed for the call. */
    const void *context;
} ta_table_key;

/** One slot: a key's hash and its value, 0 when the slot is empty. */
typedef struct {
    uint32_t hash;
    uint32_t value;
} ta_table_slot;

/**
 * A hash table; see the file comment. `count`, `limit` and `slot_count`
 * are for reading only.
 */
typedef struct {
    ta_allocator allocator;
    ta_table_slot *slots;
    /** 0, or a power of two from `TA_TABLE_MIN_SLOTS` to `TA_TABLE_MAX_SLOTS`. */
    uint32_t slot_count;
    /** Keys held. */
    uint32_t count;
    /** The most keys the table holds before it grows; insert answers
     * `TA_ERR_FULL` at it. */
    uint32_t limit;
    ta_table_mode mode;
} ta_table;

/**
 * Makes `*table` an empty table as `params` describes, allocating slots
 * for `params->capacity` keys when it is not 0. Runs at a safe point.
 * Answers `TA_ERR_INVALID` for an unknown mode or a fixed capacity of 0,
 * `TA_ERR_FULL` for a capacity above `TA_TABLE_MAX_KEYS`, and
 * `TA_ERR_NOMEM` when the slots cannot be allocated; on any failure
 * `*table` is an empty table that holds nothing and may be destroyed.
 */
ta_result ta_table_init(ta_table *table, const ta_table_params *params);

/**
 * Frees the slots and leaves an empty table with no room, which may be
 * destroyed again. Only frees; never fails.
 */
void ta_table_destroy(ta_table *table);

/**
 * Makes room for `more` keys beyond those held, so the next `more`
 * inserts succeed without allocating. A growing table that lacks the room
 * allocates the smallest power-of-two number of slots that has it, moves
 * every key there and frees the old slots. May allocate: runs only at a
 * safe point, never inside a macrostep. Answers `TA_ERR_FULL` when
 * `count + more` passes a fixed table's capacity or `TA_TABLE_MAX_KEYS`,
 * and `TA_ERR_NOMEM` when the slots cannot be allocated; either way it
 * changes nothing.
 */
ta_result ta_table_reserve(ta_table *table, uint32_t more);

/**
 * Stores `value` under a key whose hash is `hash`, in the first empty
 * slot from the key's home. The key must not be held already (see the
 * file comment). Never allocates; may run inside a macrostep. Answers
 * `TA_ERR_FULL` when the table holds `limit` keys, and `TA_ERR_INVALID`
 * for a value of 0, changing nothing.
 */
ta_result ta_table_insert(ta_table *table, uint32_t hash, uint32_t value);

/**
 * Looks `key` up and stores its value in `*value`. Borrows `key` for the
 * call. Never allocates; may run inside a macrostep. Answers
 * `TA_ERR_NOT_FOUND`, leaving `*value` alone, when the table does not
 * hold the key.
 */
ta_result ta_table_find(const ta_table *table, const ta_table_key *key, uint32_t *value);

/**
 * Removes `key` by backward shift and stores the value it had in `*value`
 * unless `value` is NULL. Borrows `key` for the call. Never allocates; may
 * run inside a macrostep. Answers `TA_ERR_NOT_FOUND` when the table does
 * not hold the key, changing nothing.
 */
ta_result ta_table_remove(ta_table *table, const ta_table_key *key, uint32_t *value);
