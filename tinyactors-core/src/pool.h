#pragma once

#include "types.h"
#include "layout.h"

/*
 * Package pool is the index pool (building block A5 of docs/proposals/
 * ring-proto): a table of fixed-size items that are addressed by a 32-bit
 * index, where 0 means "none", and that never move once they exist.
 *
 * What it is for. Core keeps several tables of records that are handed out
 * and given back at the rate of the workload: actor slots, definition slots,
 * and (in later phases of the plan) envelopes, name and park chunks and slot
 * records. Each one wants the same four things: a small name for a record
 * that fits in a handle (an index), a pointer that stays valid while the
 * record is in use (so a caller may hold one across a spawn, which can grow
 * the table), a take and a give that cost the same at every population
 * (invariant 17), and a growth step that happens only where an out-of-memory
 * answer can be reported cleanly. Before this unit every table had its own
 * copy of the page directory, the free list and the growth code.
 *
 * Layout. Items live in pages of `page_items` items, a power of two, so the
 * address of item `index` is `directory[(index - 1) >> shift].base +
 * ((index - 1) & mask) * item_size`: a shift, a mask and two loads. A page
 * is one allocation and never moves or shrinks, so growing the pool never
 * copies an item. Only the directory (an array of one {base, used} entry per
 * page, 16 bytes) is replaced when it fills, and it is replaced by a bigger
 * one before any item can be reached through the old one: nobody holds a
 * pointer into the directory, only into items.
 *
 * Two sources of items. Given items form a LIFO free list threaded through
 * the items themselves: while an item is free, 4 bytes of it at
 * `link_offset` hold the index of the next free item. The hot item is reused
 * first. Items that were never issued are handed out by a cursor, the
 * extent, in ascending order: indexes 1..extent have been issued (and hold
 * valid bytes), and an item beyond the extent has never been read or written
 * and is not initialised. So take is "the free list's head, else the next
 * index past the extent, else 0", which is the order the hand-rolled tables
 * this unit replaces used, and why replacing them leaves actor IDs
 * bit-identical. A refill touches no item memory because it threads nothing.
 * Every walk over the table is bounded by the extent, never by the capacity.
 *
 * Lifetime: system. The ta_pool control struct is embedded in its owner (a
 * ta_system, or a test's frame) and has the owner's lifetime; the pages and
 * the directory live from the refill that allocated them to ta_pool_destroy
 * or, for a trailing page that holds no taken item, to ta_pool_trim. An item
 * has the lifetime "from take to give": the bytes of a taken item belong to
 * the caller, and a pointer to it (ta_pool_at) is valid until it is given
 * (or the pool is destroyed). After a give the address is still mapped but
 * the bytes are the pool's (the free-list link), except that the caller's
 * own persistent fields (a generation counter) survive, because only the
 * four link bytes are ever written. A table whose records are recycled with
 * a new generation, as the actor and definition slot tables are, never
 * frees an item before destroy and so has no dangling-pointer question.
 * The `fresh` template of the params must outlive the pool (a static const).
 *
 * When it allocates. Only ta_pool_refill allocates, and only its owner's
 * safe points may call it (spawn, load, registration; in later phases, the
 * admission of an envelope). ta_pool_init allocates nothing (it validates and
 * sizes). ta_pool_take, ta_pool_give, ta_pool_at, ta_pool_extent,
 * ta_pool_counts_get and ta_pool_trim never allocate: a take that finds the
 * pool full answers 0 and the owner refills at its next safe point. Pages and
 * the directory are sized with ta_layout and finished where they are
 * allocated (init for the page, refill for the directory), the sizes are
 * stored in the pool, and every free uses the stored sizes, so a strict
 * allocator sees sized frees that match their allocations exactly.
 *
 * Ceiling. `max_items` bounds the capacity. A refill that would pass it
 * fails with TA_ERR_NOMEM and changes nothing. It stands in for a host that
 * runs out of memory, which makes an exhaustion path testable without
 * exhausting the machine; the tables in core pass 0 (as many as a 32-bit
 * index can name, in whole pages) and rely on their own quotas.
 *
 * Failure. Every size is computed with ta_layout or a checked 64-bit
 * product, so a count an input can influence either fits or fails. A refill
 * is all or nothing: it allocates the new directory (if the directory must
 * grow) first and then the pages, and if any allocation fails it returns
 * what it took, in reverse, and the pool is exactly as it was: same pages,
 * same directory, same counters, same free list. The old directory is freed
 * only after every allocation has succeeded, and the copy into the new one
 * cannot fail.
 *
 * Invariants. used <= extent <= capacity <= max_capacity; the free list
 * holds only indexes <= extent, each at most once, and
 * used + (length of the free list) == extent; pages * page_items ==
 * capacity; the `used` of a directory entry counts the taken items of that
 * page and the entries sum to the pool's used; high_water >= used.
 *
 * Complexity. take, give and at are O(1). refill is O(pages added) plus
 * O(pages) when the directory doubles (amortised O(1) per page). trim is
 * O(pages + free items), one pass. Space: capacity * item_size plus 16 bytes
 * per page plus at most twice that for the directory while it grows.
 *
 * Misuse that is not checked, because checking would cost the O(1): giving
 * 0, an index that was never issued, or an item twice; taking a pointer from
 * ta_pool_at and using it after the item was given and its page trimmed.
 *
 * Usage, a table of 16-byte records with a free-list link at offset 8:
 *
 *	typedef struct { void *thing; uint32_t generation; uint32_t next; } record;
 *	static const record fresh = { .generation = 1 };
 *	ta_pool pool;
 *	ta_pool_init(&pool, allocator, &(ta_pool_params){
 *		.item_size = sizeof(record), .item_alignment = _Alignof(record),
 *		.link_offset = offsetof(record, next), .page_items = 256, .fresh = &fresh });
 *	uint32_t index = ta_pool_take(&pool);
 *	if (index == 0) {                              // full: at a safe point,
 *		if (ta_pool_refill(&pool, 1) != TA_OK) {   // make room
 *			return TA_ERR_NOMEM;
 *		}
 *		index = ta_pool_take(&pool);
 *	}
 *	record *item = ta_pool_at(&pool, index);       // stable until given
 *	item->thing = thing;
 *	...
 *	ta_pool_give(&pool, index);
 *	ta_pool_destroy(&pool);
 */

/*
 * ta_pool_params describes the items of one pool. It is read by ta_pool_init
 * and not kept, except for `fresh`, which is kept as a pointer and so must
 * outlive the pool.
 */
typedef struct ta_pool_params {
    size_t item_size;      /* bytes of one item; at least 4, a multiple of item_alignment */
    size_t item_alignment; /* a power of two, at least 4: pages are allocated with it, so every item is aligned */
    size_t link_offset;    /* where the 4-byte free-list link lives while an item is free; a multiple of 4 with
                              link_offset + 4 <= item_size. Pick a field whose value means nothing while the
                              item is free, so that the others (a generation) survive give and take. */
    uint32_t page_items;   /* items per page; a power of two, so an index to an address is a shift and a mask */
    uint32_t max_items;    /* ceiling on the capacity, in items; 0 means as many as a 32-bit index can name. A
                              ceiling that is not a multiple of page_items is rounded down to whole pages, and
                              one under a page is invalid. */
    const void *fresh;     /* item_size bytes copied over an item the first time it is issued; NULL means all
                              zero. Not copied: it must outlive the pool. */
} ta_pool_params;

/* ta_pool_page is one entry of the page directory; its fields are for pool.c. */
typedef struct ta_pool_page {
    void *base;     /* the page: page_bytes at page_alignment */
    uint32_t used;  /* taken items in this page: what lets trim find whole free pages in O(pages) */
} ta_pool_page;

/*
 * ta_pool is the control struct of one pool (lifetime: system, owned by whatever embeds it). Its
 * fields are for pool.c: use the functions. A zeroed ta_pool is a destroyed pool.
 */
typedef struct ta_pool {
    ta_allocator allocator;
    ta_pool_page *directory;
    size_t page_bytes;       /* finished once in init and reused for every allocation and every sized free */
    size_t page_alignment;
    size_t directory_bytes;  /* bytes of the current directory, for its sized free; 0 with no directory */
    size_t item_size;
    size_t link_offset;
    const void *fresh;
    uint32_t page_shift;
    uint32_t page_mask;
    uint32_t pages;          /* pages allocated */
    uint32_t directory_capacity;
    uint32_t capacity;       /* pages * page_items */
    uint32_t max_capacity;   /* the ceiling, in whole pages' worth of items */
    uint32_t extent;         /* indexes 1..extent have been issued and hold valid bytes */
    uint32_t free_head;      /* LIFO list of given items; 0 when empty */
    uint32_t used;
    uint32_t high_water;
} ta_pool;

/* ta_pool_counts is a snapshot of the pool's figures. */
typedef struct ta_pool_counts {
    uint32_t used;       /* taken and not given */
    uint32_t free;       /* capacity - used: given items plus never-issued ones, what take serves before a refill */
    uint32_t high_water; /* the most items ever used at once */
    uint32_t capacity;   /* items in all pages */
    uint32_t extent;     /* the highest index ever issued (and not trimmed away) */
    uint32_t pages;
} ta_pool_counts;

/*
 * ta_pool_init validates `params`, finishes the page's ta_layout and stores
 * the result, and leaves the pool empty (no page, no directory). It never
 * allocates, so it needs no safe point and cannot leave anything to free.
 * It answers TA_ERR_INVALID for params that break the rules documented on
 * ta_pool_params, and TA_ERR_NOMEM when item_size * page_items does not fit
 * a size_t. On failure the pool is left zeroed.
 */
ta_result ta_pool_init(ta_pool *pool, ta_allocator allocator, const ta_pool_params *params);

/*
 * ta_pool_destroy returns every page and the directory to the allocator,
 * with sized frees that match their allocations, and zeroes the pool. Items
 * that are still taken are dropped: their owner finishes them first (the
 * pool cannot know what they hold). It is idempotent and safe on a pool
 * that never grew, so a partially built owner can call it.
 */
void ta_pool_destroy(ta_pool *pool);

/*
 * ta_pool_refill makes counts.free at least `min_free`, by allocating pages.
 * Call it only at a safe point, where an out-of-memory answer can be
 * reported: it is the only function of the pool that allocates. It
 * allocates nothing when the pool already has that many free items
 * (including min_free 0). It is all or nothing: on any failure everything
 * taken in this call is returned and the pool is unchanged. It answers
 * TA_ERR_NOMEM when a size overflows, when the capacity would pass the
 * ceiling, or when the allocator refuses.
 */
ta_result ta_pool_refill(ta_pool *pool, uint32_t min_free);

/*
 * ta_pool_take returns the index of an item for the caller to use: the head
 * of the free list, else the next never-issued index (initialised from
 * `fresh`), else 0 when the pool is full, in which case the caller refills
 * at its next safe point. A taken item's link bytes are zero; every other
 * byte is what the previous owner left (for a recycled item) or the template
 * (for a fresh one). It never allocates. O(1).
 */
uint32_t ta_pool_take(ta_pool *pool);

/*
 * ta_pool_give returns a taken item to the head of the free list, so the
 * most recently given item is the next one taken. Only the link bytes are
 * written. Giving 0, an index that was never issued, or an item that is
 * already free is a caller bug and is not detected. It never allocates.
 * O(1).
 */
void ta_pool_give(ta_pool *pool, uint32_t index);

/*
 * ta_pool_at returns the address of item `index`, or NULL for 0 or an index
 * beyond the extent. The address is stable until the item's page is trimmed
 * or the pool is destroyed; a taken item's page is never trimmed. It never
 * allocates. O(1).
 */
void *ta_pool_at(const ta_pool *pool, uint32_t index);

/*
 * ta_pool_extent returns the highest index ever issued: the bound of every
 * walk over a table built on the pool (a walk to the capacity would read
 * items that were never initialised). 0 for an empty pool.
 */
uint32_t ta_pool_extent(const ta_pool *pool);

/* ta_pool_counts_get fills `*out` with the pool's figures. It never allocates. O(1). */
void ta_pool_counts_get(const ta_pool *pool, ta_pool_counts *out);

/*
 * ta_pool_trim gives back every whole trailing page that has no taken item
 * and returns how many pages it freed. Items of those pages that sit on the
 * free list are unlinked in one pass over the list, and the extent is
 * lowered to the kept capacity if it was higher. It never allocates and
 * never shrinks the directory (so it can run anywhere a free can). Pointers
 * to taken items stay valid; the address of a given item in a freed page
 * does not. O(pages + free items).
 */
uint32_t ta_pool_trim(ta_pool *pool);
