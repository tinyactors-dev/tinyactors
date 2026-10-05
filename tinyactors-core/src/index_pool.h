/**
 * @file index_pool.h
 *
 * # Index pool
 *
 * `ta_index_pool` hands out fixed-size items addressed by `uint32_t`
 * index, `0` meaning none. Items live in pages of a power-of-two number of
 * items that never move, and the items that are free form an intrusive
 * free list (`list.h`) linked through a `uint32_t` field of the item the
 * caller chooses. Taking and giving an item are O(1) and never allocate;
 * pages are added only by an explicit `ta_index_pool_refill` at a safe
 * point, up to a ceiling, and given back only by an explicit
 * `ta_index_pool_trim`.
 *
 * Pages rather than one array: indices stay stable and nothing is copied
 * when the pool grows, where one reallocated array would move every item
 * and need one contiguous block.
 *
 * ## Indices and positions
 *
 * Index `i` (from 1) is position `i - 1`. With `2^k` items per page,
 * position `p` lives in page `p >> k` at item `p & (2^k - 1)`. Index 0 is
 * never issued, so a link or a field holding 0 names no item. A user that
 * numbers its slots from 0 maps slot `s` to index `s + 1`: the actor table
 * does, so actor IDs keep slot 0 for the first actor.
 *
 * ## Contract and invariants
 *
 * - Every item of an allocated page, up to the ceiling, is either taken
 *   (`used`) or on the free list (`free`): `used + free` is the number of
 *   items the pool holds. An index above the ceiling is never issued, even
 *   when the last page has room for it.
 * - **Order.** `take` answers the head of the free list. `give` puts an
 *   item at the head, so the item given last is taken first. `refill`
 *   appends a new page's items at the tail, lowest index first. So an
 *   item given back is always taken before an item that was never taken,
 *   and items that were never taken go out in ascending order, lowest
 *   page first.
 * - **Items keep their bytes.** The pool writes only an item's link
 *   field, and only while the item is free. `take` leaves the item as it
 *   was when it was given back, its link set to 0; an item of a page that
 *   `refill` just added reads as zero bytes apart from its link. A user
 *   can therefore keep state in a free item, such as a slot's generation.
 * - **Items never move.** The address `ta_index_pool_at` answers for an
 *   index stays valid until `trim` frees its page or the pool is
 *   destroyed; refill does not move it. Users still keep indices, not
 *   addresses, and look an address up again in each call.
 * - `trim` frees a page only when every item on it is free, so an item
 *   that is taken never loses its page, and a burst can leave partly used
 *   pages that cannot be freed. Its indices come back with a later refill,
 *   which fills such a hole before it adds a page at the end. A trimmed
 *   page loses what its free items held: a user that keeps state in free
 *   items (generations) does not trim.
 * - `high_water` is the highest index taken since the pool was set up.
 *   Every index above it has never been taken; a walk over every item that
 *   was ever taken stops there. Because fresh items go out in ascending
 *   order only after every given item, it is also the most items that
 *   were in use at once, as long as no trim has left a hole.
 *
 * ## The directory
 *
 * The pages are found through a directory, a `ta_vec` of one
 * `ta_index_pool_page` (address and count of taken items) per page.
 * Nothing keeps an address into it, so it may move when `refill` grows
 * it; it is one entry per page, a few kilobytes for millions of items,
 * so one array is simpler than a paged array here and costs one
 * indirection less on every access. A page that `trim` freed leaves a
 * null entry, a hole, unless it was at the end, where the directory
 * shrinks.
 *
 * ## Memory
 *
 * The pool owns its pages and its directory, allocated from the
 * `ta_allocator` given to `ta_index_pool_init`, which it borrows and which
 * must outlive it. The items' contents belong to the user. The
 * `ta_index_pool` itself is embedded where its user likes (the actor
 * table).
 *
 * - `ta_index_pool_init`, `take`, `give`, `at`, `item` and `links` never
 *   allocate, and may run inside a macrostep.
 * - `ta_index_pool_refill` allocates pages and, when it is full, a larger
 *   directory. It runs only at a safe point, never inside a macrostep: a
 *   user refills before it takes, at the point where it may allocate.
 * - `ta_index_pool_trim` and `ta_index_pool_destroy` only free. Trim runs
 *   only at a safe point, when the host asks core to give memory back.
 *   Core never trims on its own: when to give memory back is policy, and
 *   a heuristic in core would free and refill in a loop.
 *
 * ## Complexity
 *
 * | Operation | Cost |
 * |---|---|
 * | `ta_index_pool_take`, `_give`, `_at`, `_item` | O(1): a shift, a mask and one indirection |
 * | `ta_index_pool_refill(min_free)` | O(1) when `free >= min_free`; otherwise one allocation and one zeroing pass per page added, plus one for the directory when it is full, and a walk of the directory when trim left holes |
 * | `ta_index_pool_trim(keep)` | walks the directory and the free list once; one free per page freed |
 * | `ta_index_pool_init` | O(1) |
 * | `ta_index_pool_destroy` | one free per page, and the directory |
 *
 * ## Failure
 *
 * - `ta_index_pool_init` answers `TA_ERR_INVALID` for parameters that
 *   break a rule of `ta_index_pool_params`, and leaves `*pool` untouched.
 * - `ta_index_pool_take` answers `TA_ERR_FULL` when no item is free; the
 *   user refills at its next safe point.
 * - `ta_index_pool_refill` answers `TA_ERR_FULL` when `min_free` free
 *   items would pass the ceiling, and `TA_ERR_NOMEM` when an allocation
 *   fails. Either way it changes nothing: it frees what it allocated, and
 *   the pages, the directory, the free list and the counters stay as they
 *   were.
 * - Nothing else fails. Giving an index that is 0, not taken, or not this
 *   pool's is a programmer error, not checked: it corrupts the free list.
 *
 * ## Users
 *
 * The **actor table** keeps its slots here, one item per actor, with each
 * slot's generation kept in the item while it is free (ADR 0063, ADR
 * 0059). The envelope pool, the name area's and the park area's chunks,
 * and the macrostep's slot records are pools of this kind with a ceiling,
 * and the host has their free pages trimmed when it asks core to give
 * memory back.
 */
#pragma once

#include "../include/tinyactors.h"
#include "list.h"
#include "vec.h"

/** What `ta_index_pool_init` needs. */
typedef struct {
    /** Where pages and the directory come from; borrowed, and must outlive
     * the pool. */
    ta_allocator allocator;
    /** Bytes per item: not 0, a multiple of `item_alignment`, as `sizeof`
     * of a type always is. */
    size_t item_size;
    /** Alignment of each item: a power of two. */
    size_t item_alignment;
    /** Byte offset of the item's `uint32_t` link field, which the pool uses
     * while the item is free (`offsetof(T, link)`): a multiple of 4, and
     * the field lies inside the item. */
    size_t link_offset;
    /** Each page holds `1 << page_shift` items; at most 31, and a page is
     * at least as large as a pointer (refill chains the pages it is about
     * to add through their first bytes). */
    uint32_t page_shift;
    /** The most items the pool may hold, and so its highest index: from 1
     * to `UINT32_MAX`. */
    uint32_t ceiling;
} ta_index_pool_params;

/** One directory entry: a page and how many of its items are taken. */
typedef struct {
    /** The page's items, or NULL for a page that trim gave back. */
    unsigned char *items;
    uint32_t used;
} ta_index_pool_page;

/**
 * A pool of fixed-size items in pages; see the file comment. `used`,
 * `free` and `high_water` are the pool's counters, for reading only.
 */
typedef struct {
    ta_allocator allocator;
    /** The directory: one ta_index_pool_page per page. */
    ta_vec pages;
    /** The free items, linked through `link_offset`. */
    ta_list free_list;
    size_t item_size;
    size_t item_alignment;
    size_t link_offset;
    uint32_t page_shift;
    /** `(1 << page_shift) - 1`. */
    uint32_t page_mask;
    uint32_t ceiling;
    /** Directory entries that trim left NULL below the last page. */
    uint32_t holes;
    /** Items taken and not given back. */
    uint32_t used;
    /** Items on the free list. */
    uint32_t free;
    /** The highest index taken so far, 0 before the first take. */
    uint32_t high_water;
} ta_index_pool;

/**
 * Sets up an empty pool, with no page, for items described by `params`.
 * Allocates nothing; may run anywhere. Answers `TA_ERR_INVALID`, leaving
 * `*pool` untouched, when `params` breaks a rule of
 * `ta_index_pool_params` or a page's size in bytes does not fit in
 * `size_t`.
 */
ta_result ta_index_pool_init(ta_index_pool *pool, const ta_index_pool_params *params);

/**
 * Frees every page and the directory and leaves an empty pool that
 * allocates nothing until it is set up again. Every index the pool issued
 * is gone, taken or not. Only frees; never fails.
 */
void ta_index_pool_destroy(ta_index_pool *pool);

/**
 * Makes at least `min_free` items free. When fewer are, it allocates
 * zeroed pages, filling the holes trim left before adding pages at the
 * end, and appends their items to the tail of the free list in ascending
 * order; a page that the ceiling cuts short contributes only the items up
 * to the ceiling. When `min_free` items are already free it changes
 * nothing and allocates nothing.
 *
 * Allocates, so it runs only at a safe point, never inside a macrostep.
 * Answers `TA_ERR_FULL` when `used + min_free` would pass the ceiling,
 * and `TA_ERR_NOMEM` when an allocation fails; either way the pool is
 * exactly as before.
 */
ta_result ta_index_pool_refill(ta_index_pool *pool, uint32_t min_free);

/**
 * Takes the head of the free list and answers its index in `*out`. The
 * item keeps its bytes (see the file comment), its link set to 0. O(1).
 * Never allocates; may run inside a macrostep. Answers `TA_ERR_FULL`,
 * changing nothing and leaving `*out` alone, when no item is free.
 */
ta_result ta_index_pool_take(ta_index_pool *pool, uint32_t *out);

/**
 * Gives back `index`, which is taken, to the head of the free list, so it
 * is the next item taken. Writes only the item's link field. O(1). Never
 * allocates; may run inside a macrostep. Cannot fail; an index that is 0
 * or not taken is a programmer error.
 */
void ta_index_pool_give(ta_index_pool *pool, uint32_t index);

/**
 * Frees every page whose items are all free, from the last page down, as
 * long as at least `keep` items stay free, and takes their items off the
 * free list. Pages partly in use stay. A freed page at the end shrinks the
 * directory; one below the last page kept leaves a hole that refill fills
 * first. With no page left the directory is freed too. `high_water` stays.
 *
 * Only frees, and never fails. It runs at a safe point, only when the host
 * asks core to give memory back; never inside a
 * macrostep and never on core's own initiative.
 */
void ta_index_pool_trim(ta_index_pool *pool, uint32_t keep);

/**
 * The address of item `index`, or NULL for 0, an index above the ceiling,
 * or one whose page is not allocated (never added, or freed by trim). The
 * address stays valid until trim frees the page or the pool is destroyed.
 * O(1). Never allocates; may run inside a macrostep.
 */
static inline void *ta_index_pool_at(const ta_index_pool *pool, uint32_t index) {
    if (index == 0u || index > pool->ceiling) {
        return NULL;
    }
    uint32_t position = index - 1u;
    uint32_t page = position >> pool->page_shift;
    if (page >= pool->pages.count) {
        return NULL;
    }
    const ta_index_pool_page *entry = (const ta_index_pool_page *)pool->pages.items + page;
    if (entry->items == NULL) {
        return NULL;
    }
    return entry->items + (size_t)(position & pool->page_mask) * pool->item_size;
}

/**
 * `ta_index_pool_at` in the shape a `ta_list_links` asks for: `pool` is
 * the `ta_index_pool`. Never allocates; may run inside a macrostep.
 */
void *ta_index_pool_item(void *pool, uint32_t index);

/**
 * The links of a list over this pool's items, linked through the
 * `uint32_t` field at byte `offset` of each item. The pool's own free list
 * uses its `link_offset`; a user's lists use other fields (or the same
 * one, for items that are taken). Never allocates; may run inside a
 * macrostep.
 */
static inline ta_list_links ta_index_pool_links(ta_index_pool *pool, size_t offset) {
    return (ta_list_links){ .item = ta_index_pool_item, .items = pool, .offset = offset };
}
