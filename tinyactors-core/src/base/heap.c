#include "base/heap.h"
#include "base/memory.h"

#define NODES(h)     ((ta_heap_node *)ta_pointer_add((h), (h)->nodes_relative))
#define POSITIONS(h) ((uint32_t *)ta_pointer_add((h), (h)->positions_relative))

static int less(const ta_heap_node *a, const ta_heap_node *b) {
    return ta_heap_before(a, b);
}

static void h4_place(ta_heap *heap, uint32_t i, ta_heap_node v) {
    NODES(heap)[i] = v;
    POSITIONS(heap)[v.item] = i;
}

static void sift_up(ta_heap *heap, uint32_t i, ta_heap_node v) {
    ta_heap_node *node = NODES(heap);
    while (i > 0) {
        uint32_t parent = (i - 1) >> 2;
        if (!less(&v, &node[parent])) break;
        h4_place(heap, i, node[parent]);
        i = parent;
    }
    h4_place(heap, i, v);
}

static void sift_down(ta_heap *heap, uint32_t i, ta_heap_node v) {
    ta_heap_node *node = NODES(heap);
    for (;;) {
        uint32_t c = (i << 2) + 1, best = TA_NIL;
        for (uint32_t k = c; k < c + 4 && k < heap->count; k++)
            if (best == TA_NIL || less(&node[k], &node[best])) best = k;
        if (best == TA_NIL || !less(&node[best], &v)) break;
        h4_place(heap, i, node[best]);
        i = best;
    }
    h4_place(heap, i, v);
}

void ta_heap_init(ta_heap *heap, ta_heap_node *nodes, uint32_t *position, uint32_t capacity) {
    heap->nodes_relative = (int64_t)((intptr_t)nodes - (intptr_t)heap);
    heap->positions_relative = (int64_t)((intptr_t)position - (intptr_t)heap);
    heap->capacity = capacity;
    heap->count = 0;
    for (uint32_t i = 0; i < capacity; i++)
        position[i] = TA_NIL;
}

ta_status ta_heap_push(ta_heap *heap, uint64_t due, uint32_t seq, uint32_t item) {
    if (item >= heap->capacity || heap->count >= heap->capacity || POSITIONS(heap)[item] != TA_NIL)
        return TA_ERROR_INVALID;
    ta_heap_node v = {due, seq, item};
    sift_up(heap, heap->count++, v);
    return TA_OK;
}

const ta_heap_node *ta_heap_top(const ta_heap *heap) {
    return heap->count ? (const ta_heap_node *)ta_pointer_add(heap, heap->nodes_relative)
                       : (const ta_heap_node *)0;
}

static void remove_at(ta_heap *heap, uint32_t i) {
    ta_heap_node *node = NODES(heap);
    POSITIONS(heap)[node[i].item] = TA_NIL;
    ta_heap_node last = node[--heap->count];
    ta_memset(&node[heap->count], 0,
              sizeof node[heap->count]); /* the vacated slot: never residue */
    if (i == heap->count) return;
    if (i > 0 && less(&last, &node[(i - 1) >> 2]))
        sift_up(heap, i, last);
    else
        sift_down(heap, i, last);
}

uint32_t ta_heap_pop(ta_heap *heap) {
    if (!heap->count) return TA_NIL;
    uint32_t item = NODES(heap)[0].item;
    remove_at(heap, 0);
    return item;
}

int ta_heap_remove(ta_heap *heap, uint32_t item) {
    if (item >= heap->capacity) return 0;
    uint32_t i = POSITIONS(heap)[item];
    if (i == TA_NIL) return 0;
    remove_at(heap, i);
    return 1;
}

void ta_heap_copy(ta_heap *dst, ta_heap_node *nodes, uint32_t *position, uint32_t capacity,
                  const ta_heap *source) {
    TA_ASSERT(capacity >= source->capacity);
    ta_heap_init(dst, nodes, position, capacity);
    for (uint32_t i = 0; i < source->count; i++)
        nodes[i] = NODES(source)[i];
    for (uint32_t i = 0; i < source->capacity; i++)
        position[i] = POSITIONS(source)[i];
    dst->count = source->count;
}

const ta_heap_node *ta_heap_find(const ta_heap *heap, uint32_t item) {
    if (item >= heap->capacity) return (const ta_heap_node *)0;
    uint32_t i = ((const uint32_t *)ta_pointer_add(heap, heap->positions_relative))[item];
    return i == TA_NIL ? (const ta_heap_node *)0
                       : (const ta_heap_node *)ta_pointer_add(heap, heap->nodes_relative) + i;
}
