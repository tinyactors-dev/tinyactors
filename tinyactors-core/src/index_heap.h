#pragma once

#include "types.h"
#include "layout.h"
#include "pool.h"

/*
 * Package index_heap is the indexed min-heap (building block B2 of docs/
 * proposals/ring-proto): a priority queue of 32-bit item indices, ordered by
 * a key each item carries, in which any member can be removed in O(log n)
 * because every member knows its own position.
 *
 * What it is for. Delayed sends and timers fire in time order, and a
 * <cancel> takes one out of the middle. A plain heap cannot find the item to
 * remove without a search, so the position is kept in the item itself, in a
 * field `heap_index`. The items are the records of an index pool (block A5,
 * pool.h); the heap adds no node of its own. Its only memory is one array of
 * 4 bytes per item, which is what the byte budget of the proposal assumes.
 *
 * Keys. An item has a `due` (a uint64_t time, the primary key, smaller
 * first) and an `order` (a uint64_t, the tie-breaker, smaller first), so
 * items with the same deadline come out in the order they were sent. The
 * owner assigns `order` (a send counter of the system): the heap never
 * invents it, which lets a session that is reloaded rejoin the heap with its
 * original order. `order` must be unique among the items in the heap; two
 * equal (due, order) pairs do not corrupt the heap, they only leave the
 * order between those two unspecified.
 *
 * How the heap reads an item. It holds the pool and three byte offsets (of
 * `due`, `order` and `heap_index` inside an item) and reaches a field as
 * (unsigned char *)ta_pool_at(pool, index) + offset. There is no callback,
 * no wrapper struct and no per-item cost beyond those three fields, which
 * the item's owner declares anyway. The heap only ever writes `heap_index`.
 * `heap_index` is 0 when the item is not in the heap and otherwise its
 * 1-based position: array[heap_index - 1] == index.
 *
 * Lifetime. The ta_index_heap control struct and its array have the
 * lifetime system: one deadline index per system, embedded in its owner (a
 * test's frame today). The array lives from the ta_index_heap_reserve that
 * allocated it to ta_index_heap_destroy, or until a later reserve replaces
 * it. The items have the lifetime of their own block (the envelope, once the
 * deadline index is wired in): the heap holds an item's u32 index only while
 * its heap_index is non-zero, and never holds a pointer between calls, so it
 * is safe across pool refills. An item must leave the heap (remove or
 * pop_min, which set heap_index to 0) before its owner gives it back to the
 * pool. Items in the heap are taken, and a taken item's page is never
 * trimmed, so every ta_pool_at the heap does is valid.
 *
 * When it allocates. Only ta_index_heap_reserve allocates, and only its
 * owner's safe points may call it (admission of a delayed send, spawn, load,
 * or right after the pool's refill). ta_index_heap_init allocates nothing.
 * insert, peek_min, pop_min, remove, count and capacity never allocate, so
 * nothing in a macrostep allocates for the heap: once capacity covers the
 * pool's capacity, insert cannot fail. Insert answers TA_ERR_NOMEM only when
 * the owner forgot to reserve; that is a checked answer, not an out-of-bounds
 * write. The array is sized with ta_layout, finished where it is allocated,
 * and its byte size is stored and reused for the sized free, so a strict
 * allocator sees frees that match their allocations.
 *
 * Growth. Reserve grows the array geometrically (16 entries first, then
 * doubling, clamped to the ceiling `max_items`). The pool adds one page at a
 * time; an exact-fit array would be copied at every page, which is quadratic
 * over a pool's life. At the ceiling the array is exactly 4 bytes per item;
 * below it, up to twice the used capacity may be reserved for a while.
 *
 * Ceiling. `max_items` bounds the capacity, as ta_pool_params.max_items does
 * for the pool; 0 means as many as a u32 position can name. A reserve past
 * it answers TA_ERR_NOMEM.
 *
 * Failure. Reserve is all or nothing: size the new array, allocate it, copy
 * the live entries (cannot fail), free the old array, commit. On any failure
 * (size overflow, request over the ceiling, refused allocation) it returns
 * TA_ERR_NOMEM and the heap is exactly as it was. Insert changes the heap
 * only on TA_OK.
 *
 * Invariants. count <= capacity <= max_capacity; array[p - 1] == i implies
 * item(i).heap_index == p; each of the first `count` entries of the array has
 * heap_index != 0 and appears once; no child is before its parent by (due,
 * order).
 *
 * Complexity. insert, pop_min and remove are O(log n) with no recursion (the
 * sifts are loops); peek_min, count and capacity are O(1); reserve is O(n)
 * when it grows, amortised O(1) per item across a pool's growth.
 *
 * Why no update. A delayed send never changes its due, and <cancel> is a
 * remove. A reloaded session inserts again with the same (due, order).
 *
 * Misuse that is not checked, because checking would cost more than the
 * operation: changing `due` or `order` of an item while it is in the heap;
 * giving an item to the pool while it is in the heap; offsets that are not
 * those of real fields (the pool does not tell the heap its item size).
 *
 * Usage, with a 24-byte item:
 *
 *	typedef struct { uint64_t due; uint64_t order; uint32_t heap_index; uint32_t next; } item;
 *	ta_pool pool;       // an A5 pool of `item`, link at offsetof(item, next)
 *	ta_index_heap heap;
 *	ta_index_heap_init(&heap, allocator, &(ta_index_heap_params){
 *		.pool = &pool, .due_offset = offsetof(item, due), .order_offset = offsetof(item, order),
 *		.heap_index_offset = offsetof(item, heap_index) });
 *	// at a safe point, after the pool has room:
 *	if (ta_index_heap_reserve(&heap, capacity_of_pool) != TA_OK) { return TA_ERR_NOMEM; }
 *	// anywhere:
 *	uint32_t index = ta_pool_take(&pool);
 *	item *it = ta_pool_at(&pool, index);
 *	it->due = 100; it->order = sequence++;
 *	ta_index_heap_insert(&heap, index);
 *	for (uint32_t head = ta_index_heap_peek_min(&heap);
 *	     head != 0 && ((item *)ta_pool_at(&pool, head))->due <= now;
 *	     head = ta_index_heap_peek_min(&heap)) {
 *		uint32_t fired = ta_index_heap_pop_min(&heap);
 *		... deliver it ...
 *		ta_pool_give(&pool, fired);
 *	}
 *	ta_index_heap_destroy(&heap);
 */

/*
 * ta_index_heap_params says where an item keeps its key and its position. It
 * is read by ta_index_heap_init and not kept.
 */
typedef struct ta_index_heap_params {
    ta_pool *pool;            /* items live here; must outlive the heap; items in the heap must be taken */
    size_t due_offset;        /* offsetof(item, due): a uint64_t, the primary key, smaller first */
    size_t order_offset;      /* offsetof(item, order): a uint64_t, breaks ties, unique among items in the heap */
    size_t heap_index_offset; /* offsetof(item, heap_index): a uint32_t, 0 = not in the heap, else the 1-based position */
    uint32_t max_items;       /* ceiling on the capacity, in items; 0 means as many as a u32 position can name */
} ta_index_heap_params;

/*
 * ta_index_heap is the control struct of one heap (lifetime: system, owned by
 * whatever embeds it). Its fields are for index_heap.c: use the functions. A
 * zeroed ta_index_heap is a destroyed heap.
 */
typedef struct ta_index_heap {
    ta_allocator allocator;
    ta_pool *pool;
    size_t due_offset;
    size_t order_offset;
    size_t heap_index_offset;
    uint32_t *array;     /* capacity entries; array[p - 1] is the item at position p */
    size_t array_bytes;  /* finished once per allocation, reused for the sized free; 0 with no array */
    uint32_t count;
    uint32_t capacity;
    uint32_t max_capacity;
} ta_index_heap;

/*
 * ta_index_heap_init validates `params` and stores them, and allocates
 * nothing: no safe point is needed and nothing is left to free. It answers
 * TA_ERR_INVALID for a NULL pool or params, an offset that is not aligned
 * for its type, or two fields that overlap. On failure the heap is left
 * zeroed. Whether an offset lies inside the item is the caller's contract.
 */
ta_result ta_index_heap_init(ta_index_heap *heap, ta_allocator allocator, const ta_index_heap_params *params);

/*
 * ta_index_heap_destroy frees the array with the size it was allocated with
 * and zeroes the heap. Items still in the heap keep a stale heap_index: their
 * owner (the pool) is dropped with them. It is idempotent and safe on a heap
 * that never reserved.
 */
void ta_index_heap_destroy(ta_index_heap *heap);

/*
 * ta_index_heap_reserve makes the capacity at least `items`, growing
 * geometrically and clamped to the ceiling. It is the only function of the
 * heap that allocates: call it only at a safe point. It allocates nothing
 * when `items` is 0 or already fits. It is all or nothing: TA_ERR_NOMEM
 * (size overflow, `items` over the ceiling, or a refused allocation) leaves
 * the heap exactly as it was.
 */
ta_result ta_index_heap_reserve(ta_index_heap *heap, uint32_t items);

/*
 * ta_index_heap_insert adds item `index`, which the caller took from the
 * pool and whose heap_index is 0, at the (due, order) it holds now. It never
 * allocates. O(log n). TA_ERR_NOMEM when count == capacity (the owner did not
 * reserve), TA_ERR_INVALID for 0, an index beyond the pool's extent or an
 * item that is already in the heap. The heap changes only on TA_OK.
 */
ta_result ta_index_heap_insert(ta_index_heap *heap, uint32_t index);

/*
 * ta_index_heap_peek_min returns the item with the smallest (due, order), or
 * 0 when the heap is empty. It does not change the heap. O(1).
 */
uint32_t ta_index_heap_peek_min(const ta_index_heap *heap);

/*
 * ta_index_heap_pop_min removes and returns the item with the smallest (due,
 * order) and sets its heap_index to 0, or returns 0 when the heap is empty.
 * It never allocates. O(log n).
 */
uint32_t ta_index_heap_pop_min(ta_index_heap *heap);

/*
 * ta_index_heap_remove takes item `index` out of the heap wherever it is (a
 * <cancel>) and sets its heap_index to 0. It returns false and changes
 * nothing when the item is not in the heap (or is 0 or beyond the extent).
 * It never allocates. O(log n).
 */
bool ta_index_heap_remove(ta_index_heap *heap, uint32_t index);

/* ta_index_heap_count returns the number of items in the heap. O(1). */
uint32_t ta_index_heap_count(const ta_index_heap *heap);

/* ta_index_heap_capacity returns the number of items the array can hold now. O(1). */
uint32_t ta_index_heap_capacity(const ta_index_heap *heap);
