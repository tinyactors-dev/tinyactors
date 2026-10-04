/**
 * @file heap.h
 *
 * # Indexed min-heap
 *
 * `ta_heap` is a binary min-heap of items addressed by `uint32_t` index,
 * `0` meaning none. Items are ordered by the key `(due, order)`: the
 * smaller `due` first and, for the same `due`, the smaller `order`. Each
 * item records its own position in the heap, so removing an item from
 * anywhere in the heap costs O(log n), not a search.
 *
 * ## Reaching an item's fields: `ta_heap_items`
 *
 * Items live in an index pool's pages, which never move but are not one
 * array. A `ta_heap_items` names how to reach the three fields the heap
 * uses:
 *
 * - `item(items, index)` answers the address of item `index` (never 0) in
 *   the storage `items`, as for an intrusive index list;
 * - `due_offset` and `order_offset` are the byte offsets of the item's
 *   two `uint64_t` key fields (an envelope's `due` and its send order);
 * - `position_offset` is the byte offset of the item's `uint32_t`
 *   position field (an envelope's `heap_index`).
 *
 * Each field is aligned for its type. The heap keeps no pointer to an
 * item: it asks `item` again at every step and uses the address only
 * within that call. The item storage may therefore grow between calls;
 * within a call it must not move, which holds because the functions that
 * take a `ta_heap_items` never allocate. Using different `ta_heap_items`
 * for one heap is a programmer error.
 *
 * ## Contract and invariants
 *
 * - The heap holds `length` items at positions `0` to `length - 1`. The
 *   item at position `p > 0` is not smaller than the one at `(p - 1) / 2`,
 *   so position 0 holds a smallest item.
 * - For every item in the heap, its position field holds its position.
 *   The heap writes the field when it places the item and never reads or
 *   writes it once the item has left. While an item is out of the heap,
 *   the field belongs to the item's owner: an envelope in the run queue
 *   keeps its arrival number there, since an envelope in the deadline
 *   index is not runnable and a runnable one is not in the index.
 * - Whether an item is in the heap is exact, whatever its position field
 *   holds while it is out: it is in the heap when the field is below
 *   `length` and the entry at that position is the item itself.
 * - The heap reads `due` and `order` and never writes them. An item's key
 *   must not change while it is in the heap. Items with equal keys leave
 *   in an unspecified order between them, so a user that needs a total
 *   order gives each item a distinct `order` (the send order).
 * - An item is in at most one heap at a time; inserting an item that is
 *   already in a heap is a programmer error, not checked.
 *
 * ## Memory
 *
 * The heap keeps one `uint32_t` item index per position in a paged
 * array, pages of `1 << page_shift` positions allocated from the
 * `ta_allocator` given to `ta_heap_init`, which it borrows and which must
 * outlive it. It owns those pages. The items and their storage belong to
 * the caller (the envelope pool), and the heap borrows them only for the
 * length of a call. A `ta_heap` itself is embedded where its user likes.
 *
 * - `ta_heap_init` allocates nothing.
 * - `ta_heap_grow` allocates pages, and so runs only at a safe point: the
 *   envelope pool's refill grows the heap together with the pool, so that
 *   the heap's capacity is never below the number of envelopes that can be
 *   delayed sends.
 * - `ta_heap_shrink` and `ta_heap_destroy` only free. Shrink runs at a
 *   safe point too, when the host calls `ta_system_trim`, never on core's
 *   own initiative.
 * - `ta_heap_insert`, `ta_heap_pop`, `ta_heap_remove`, `ta_heap_head`,
 *   `ta_heap_length` and `ta_heap_capacity` never allocate, and may run
 *   inside a macrostep. Insert does not grow the heap: its capacity was
 *   reserved at the last safe point.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_heap_head`, `ta_heap_length`, `ta_heap_capacity` | O(1), and touches no item |
 * | `ta_heap_insert` | O(log n) |
 * | `ta_heap_pop` | O(log n) |
 * | `ta_heap_remove(i)` | O(log n): the item's position field gives where it is |
 * | `ta_heap_grow(n)` | one allocation per page added, as the paged array's grow |
 * | `ta_heap_shrink(n)` | one free per page freed |
 * | `ta_heap_init` | O(1) |
 * | `ta_heap_destroy` | one free per page, and the page directory |
 *
 * Each step that moves an item costs one call to `items->item` and one
 * read of its key; each position costs a shift, a mask and one
 * indirection through the paged array.
 *
 * ## Failure
 *
 * - `ta_heap_init` answers `TA_ERR_INVALID` for a `page_shift` above 31,
 *   or a page whose size in bytes does not fit in `size_t`, and leaves
 *   `*heap` untouched.
 * - `ta_heap_grow` answers `TA_ERR_NOMEM` when the allocator refuses, and
 *   the heap is then exactly as before: its capacity, its items and their
 *   positions.
 * - `ta_heap_insert` answers `TA_ERR_FULL`, and changes nothing, when the
 *   heap has no room: its length equals its capacity. The check is one
 *   comparison; a full heap is a state the caller must handle, not a
 *   programmer error.
 * - `ta_heap_remove` answers `TA_ERR_NOT_FOUND`, and changes nothing, when
 *   the item is not in the heap. The check costs one item lookup.
 * - Everything else is a documented precondition, not checked at run
 *   time: an item index of 0, an item already in a heap.
 *
 * ## Users
 *
 * The **deadline index** is this heap over envelope indices, keyed by an
 * envelope's `due` time and its send order. A delayed `<send>` inserts
 * its envelope; `<cancel>` and unloading remove envelopes from anywhere
 * in it; `h2c.time` pops every envelope due by the new time, in due order
 * and then send order. Core writes `c2h.wake` from the head's `due` when
 * the head changes.
 */
#pragma once

#include "../include/tinyactors.h"
#include "paged_array.h"

/** What `ta_heap_init` needs. */
typedef struct {
    /** Where the position pages come from; borrowed, and must outlive the
     * heap. */
    ta_allocator allocator;
    /** Each page holds `1 << page_shift` positions; at most 31. */
    uint32_t page_shift;
} ta_heap_params;

/** How to reach an item's key and position fields; see the file comment. */
typedef struct {
    /** The address of item `index` (never 0) in `items`, valid until the
     * storage next grows. */
    void *(*item)(void *items, uint32_t index);
    /** The storage `item` reads, borrowed for each call. */
    void *items;
    /** Byte offset of the item's `uint64_t` due time. */
    size_t due_offset;
    /** Byte offset of the item's `uint64_t` order, which breaks ties of
     * `due`. */
    size_t order_offset;
    /** Byte offset of the item's `uint32_t` position in the heap. */
    size_t position_offset;
} ta_heap_items;

/** An indexed min-heap of item indices; see the file comment. */
typedef struct {
    /** One `uint32_t` item index per position; positions `0` to
     * `length - 1` hold the heap. */
    ta_paged_array positions;
    uint32_t length;
} ta_heap;

/**
 * Sets up an empty heap with capacity 0. Allocates nothing. Answers
 * `TA_ERR_INVALID`, leaving `*heap` untouched, for a `page_shift` above
 * 31 or a page too large for `size_t`.
 */
ta_result ta_heap_init(ta_heap *heap, const ta_heap_params *params);

/**
 * Frees every page, and leaves an empty heap with capacity 0 that
 * allocates nothing until it is set up again. The items it held are not
 * touched; their position fields are theirs again. Never fails.
 */
void ta_heap_destroy(ta_heap *heap);

/**
 * Makes room for at least `count` items, rounded up to whole pages, so
 * that inserts up to that length never allocate. Moves no item and
 * touches no item. A `count` within the capacity changes nothing and
 * allocates nothing.
 *
 * Allocates, so it runs only at a safe point (the envelope pool's
 * refill), never inside a macrostep. Answers `TA_ERR_NOMEM` when an
 * allocation fails; the heap is then exactly as before.
 */
ta_result ta_heap_grow(ta_heap *heap, uint32_t count);

/**
 * Frees every page wholly above the larger of `keep` and the heap's
 * length, so the items in the heap and room for `keep` items stay. The
 * deadline index keeps room for every envelope its pool still holds, so
 * that inserting stays free of allocation. A capacity already within
 * that changes nothing.
 *
 * Only frees, and never fails. It runs at a safe point, when the host
 * asks core to give memory back (`ta_system_trim`).
 */
void ta_heap_shrink(ta_heap *heap, uint32_t keep);

/** How many items the heap holds. Never allocates. */
static inline uint32_t ta_heap_length(const ta_heap *heap) { return heap->length; }

/** How many items the heap has room for without growing: whole pages, at
 * most 2^32. Never allocates. */
static inline uint64_t ta_heap_capacity(const ta_heap *heap) {
    return ta_paged_array_capacity(&heap->positions);
}

/**
 * A smallest item by `(due, order)`, which stays in the heap, or 0 when
 * the heap is empty. Touches no item. O(1). Never allocates; may run
 * inside a macrostep.
 */
static inline uint32_t ta_heap_head(const ta_heap *heap) {
    if (heap->length == 0u) {
        return 0u;
    }
    uint32_t item;
    ta_paged_array_get(&heap->positions, 0u, &item);
    return item;
}

/**
 * Adds `item` (not 0, in no heap) at the place its key gives it, and sets
 * its position field. Room comes from the last `ta_heap_grow` at a safe
 * point; insert never grows the heap. O(log n). Never allocates; may run
 * inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_FULL` and changes nothing when the length
 * already equals the capacity.
 */
ta_result ta_heap_insert(ta_heap *heap, const ta_heap_items *items, uint32_t item);

/**
 * Removes a smallest item and answers it, or answers 0 and changes
 * nothing when the heap is empty. The removed item's position field is
 * left as it was, for its owner to reuse. O(log n). Never allocates; may
 * run inside a macrostep.
 */
uint32_t ta_heap_pop(ta_heap *heap, const ta_heap_items *items);

/**
 * Removes `item` from wherever it is in the heap, found through its
 * position field, and leaves that field as it was, for its owner to
 * reuse. O(log n). Never allocates; may run inside a macrostep.
 *
 * Answers `TA_OK`, or `TA_ERR_NOT_FOUND` and changes nothing when `item`
 * is not in this heap (0 never is).
 */
ta_result ta_heap_remove(ta_heap *heap, const ta_heap_items *items, uint32_t item);
