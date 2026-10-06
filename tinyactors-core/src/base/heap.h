/* heap.h — 4-ary min-heap keyed by (due, seq) with O(log n) removal.
 *
 * Items are u32 indices < capacity (timer pool indices). `positions[item]` is the item's
 * heap position or TA_NIL, enabling <cancel> without a search. Ties on `due`
 * break by `seq` (monotonic), which makes timer firing order deterministic.
 * 4-ary: shallower than binary, children share a cache line.
 */
#ifndef TINYACTORS_BASE_HEAP_H
#define TINYACTORS_BASE_HEAP_H

#include "base/config.h"

typedef struct ta_heap_node {
    uint64_t due;
    uint32_t sequence;
    uint32_t item;
} ta_heap_node;

typedef struct ta_heap {
    int64_t nodes_relative; /* self-relative, see pool.h */
    int64_t positions_relative;
    uint32_t capacity;
    uint32_t count;
} ta_heap;

/* The heap's order: earlier due first, ties by seq (wrap-around aware). */
TA_INLINE int ta_heap_before(const ta_heap_node *a, const ta_heap_node *b) {
    return a->due < b->due || (a->due == b->due && (int32_t)(a->sequence - b->sequence) < 0);
}

TA_INLINE size_t ta_heap_nodes_bytes(uint32_t capacity) {
    return sizeof(ta_heap_node) * capacity;
}
TA_INLINE size_t ta_heap_position_bytes(uint32_t capacity) {
    return sizeof(uint32_t) * capacity;
}

void ta_heap_init(ta_heap *h, ta_heap_node *nodes, uint32_t *position, uint32_t capacity);
/* Copy into a heap with capacity >= src->capacity (items keep their numbers). */
void ta_heap_copy(ta_heap *dst, ta_heap_node *nodes, uint32_t *position, uint32_t capacity,
                  const ta_heap *source);
ta_status ta_heap_push(ta_heap *h, uint64_t due, uint32_t seq, uint32_t item);
const ta_heap_node *ta_heap_top(const ta_heap *h); /* NULL if empty */
uint32_t ta_heap_pop(ta_heap *h);                  /* item, or TA_NIL */
int ta_heap_remove(ta_heap *h, uint32_t item);     /* 1 if it was queued */
/* The node of a queued item (due, seq), or NULL. */
const ta_heap_node *ta_heap_find(const ta_heap *h, uint32_t item);
TA_INLINE int ta_heap_contains(const ta_heap *heap, uint32_t item) {
    return item < heap->capacity &&
           ((const uint32_t *)ta_pointer_add(heap, heap->positions_relative))[item] != TA_NIL;
}

#endif /* TINYACTORS_BASE_HEAP_H */
