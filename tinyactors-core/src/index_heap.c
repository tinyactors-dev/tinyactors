#include "index_heap.h"

/*
 * Implementation of block B2 (indexed min-heap). The contract, lifetime
 * (system), allocation rules and usage are in index_heap.h; this comment
 * records how the code meets them.
 *
 * The heap is a binary heap of u32 item indices in one array, positions
 * 1-based so that 0 can mean "not in the heap" in an item's heap_index field.
 * Keys and heap_index are read and written by byte offset through
 * ta_pool_at, with ta_mem_copy so no alignment or aliasing is assumed beyond
 * what init checked. Every heap-wide write of an item goes through
 * ta_index_heap_place, which keeps the array and the item's heap_index in
 * step. Sifting is iterative (no recursion, bounded stack). The array is the
 * only allocation; it is made in ta_index_heap_reserve (allocate, copy, free
 * the old array, commit: nothing after the allocation can fail) and freed in
 * destroy with the size stored at allocation. insert, remove and pop never
 * allocate.
 */

/* The first array holds this many entries and doubles from there. */
#define TA_INDEX_HEAP_FIRST_CAPACITY 16u

/* ta_index_heap_field returns the address of the field at `offset` of item `index`, or NULL beyond the extent. */
static unsigned char *ta_index_heap_field(const ta_index_heap *heap, uint32_t index, size_t offset) {
    unsigned char *item = ta_pool_at(heap->pool, index);
    return item == NULL ? NULL : item + offset;
}

/* ta_index_heap_due reads the primary key of an item. The caller has checked the index is in the extent. */
static uint64_t ta_index_heap_due(const ta_index_heap *heap, uint32_t index) {
    uint64_t value;
    ta_mem_copy(&value, ta_index_heap_field(heap, index, heap->due_offset), sizeof value);
    return value;
}

/* ta_index_heap_order reads the tie-breaker of an item. The caller has checked the index is in the extent. */
static uint64_t ta_index_heap_order(const ta_index_heap *heap, uint32_t index) {
    uint64_t value;
    ta_mem_copy(&value, ta_index_heap_field(heap, index, heap->order_offset), sizeof value);
    return value;
}

/* ta_index_heap_back reads the position field of an item. The caller has checked the index is in the extent. */
static uint32_t ta_index_heap_back(const ta_index_heap *heap, uint32_t index) {
    uint32_t value;
    ta_mem_copy(&value, ta_index_heap_field(heap, index, heap->heap_index_offset), sizeof value);
    return value;
}

/* ta_index_heap_set_back writes the position of an item: the only field of an item the heap writes. */
static void ta_index_heap_set_back(const ta_index_heap *heap, uint32_t index, uint32_t position) {
    ta_mem_copy(ta_index_heap_field(heap, index, heap->heap_index_offset), &position, sizeof position);
}

/* ta_index_heap_before says whether item a is due strictly before item b by (due, order). */
static bool ta_index_heap_before(const ta_index_heap *heap, uint32_t a, uint32_t b) {
    uint64_t due_a = ta_index_heap_due(heap, a);
    uint64_t due_b = ta_index_heap_due(heap, b);
    if (due_a != due_b) {
        return due_a < due_b;
    }
    return ta_index_heap_order(heap, a) < ta_index_heap_order(heap, b);
}

/* ta_index_heap_place stores `index` at 1-based `position` and records the position in the item. */
static void ta_index_heap_place(ta_index_heap *heap, uint32_t position, uint32_t index) {
    heap->array[position - 1u] = index;
    ta_index_heap_set_back(heap, index, position);
}

/* ta_index_heap_sift_up moves the item at `position` toward the root while it is before its parent. Iterative. */
static void ta_index_heap_sift_up(ta_index_heap *heap, uint32_t position) {
    uint32_t index = heap->array[position - 1u];
    while (position > 1u) {
        uint32_t parent = position / 2u;
        uint32_t parent_index = heap->array[parent - 1u];
        if (!ta_index_heap_before(heap, index, parent_index)) {
            break;
        }
        ta_index_heap_place(heap, position, parent_index);
        position = parent;
    }
    ta_index_heap_place(heap, position, index);
}

/* ta_index_heap_sift_down moves the item at `position` toward the leaves while a child is before it. Iterative. */
static void ta_index_heap_sift_down(ta_index_heap *heap, uint32_t position) {
    uint32_t index = heap->array[position - 1u];
    uint32_t count = heap->count;
    for (;;) {
        uint64_t left = (uint64_t)position * 2u;
        if (left > count) {
            break;
        }
        uint32_t child = (uint32_t)left;
        if (child < count && ta_index_heap_before(heap, heap->array[child], heap->array[child - 1u])) {
            child += 1u;
        }
        uint32_t child_index = heap->array[child - 1u];
        if (!ta_index_heap_before(heap, child_index, index)) {
            break;
        }
        ta_index_heap_place(heap, position, child_index);
        position = child;
    }
    ta_index_heap_place(heap, position, index);
}

/* ta_index_heap_span is a field of an item: `size` bytes at `offset`. */
typedef struct ta_index_heap_span {
    size_t offset;
    size_t size;
} ta_index_heap_span;

/* ta_index_heap_overlap says whether two fields of an item share a byte. It compares the distance between the
 * offsets with the size of the lower field instead of adding, so no offset can wrap the arithmetic. */
static bool ta_index_heap_overlap(ta_index_heap_span a, ta_index_heap_span b) {
    return a.offset < b.offset ? b.offset - a.offset < a.size : a.offset - b.offset < b.size;
}

/* ta_index_heap_init: see index_heap.h. */
ta_result ta_index_heap_init(ta_index_heap *heap, ta_allocator allocator, const ta_index_heap_params *params) {
    if (heap == NULL) {
        return TA_ERR_INVALID;
    }
    ta_mem_zero(heap, sizeof *heap);
    if (params == NULL) {
        return TA_ERR_INVALID;
    }
    ta_index_heap_span due = { params->due_offset, sizeof(uint64_t) };
    ta_index_heap_span order = { params->order_offset, sizeof(uint64_t) };
    ta_index_heap_span back = { params->heap_index_offset, sizeof(uint32_t) };
    if (params->pool == NULL || params->due_offset % _Alignof(uint64_t) != 0 ||
        params->order_offset % _Alignof(uint64_t) != 0 || params->heap_index_offset % _Alignof(uint32_t) != 0 ||
        ta_index_heap_overlap(due, order) || ta_index_heap_overlap(due, back) || ta_index_heap_overlap(order, back)) {
        return TA_ERR_INVALID;
    }
    heap->allocator = allocator;
    heap->pool = params->pool;
    heap->due_offset = params->due_offset;
    heap->order_offset = params->order_offset;
    heap->heap_index_offset = params->heap_index_offset;
    heap->max_capacity = params->max_items == 0 ? UINT32_MAX : params->max_items;
    return TA_OK;
}

/* ta_index_heap_destroy: see index_heap.h. The sized free uses the size the allocation stored. */
void ta_index_heap_destroy(ta_index_heap *heap) {
    if (heap == NULL) {
        return;
    }
    if (heap->array != NULL) {
        ta_mem_free(heap->allocator, heap->array,
                    &(ta_allocator_free_params){ .size = heap->array_bytes, .alignment = _Alignof(uint32_t) });
    }
    ta_mem_zero(heap, sizeof *heap);
}

/* ta_index_heap_reserve: see index_heap.h. Allocate, then copy, free the old array and commit; nothing after the allocation fails. */
ta_result ta_index_heap_reserve(ta_index_heap *heap, uint32_t items) {
    if (items <= heap->capacity) {
        return TA_OK;
    }
    if (items > heap->max_capacity) {
        return TA_ERR_NOMEM;
    }
    uint64_t grown = heap->capacity == 0 ? TA_INDEX_HEAP_FIRST_CAPACITY : (uint64_t)heap->capacity * 2u;
    while (grown < items) {
        grown *= 2u;
    }
    if (grown > heap->max_capacity) {
        grown = heap->max_capacity; /* items <= max_capacity, so the clamp still covers the request */
    }
    if (grown > SIZE_MAX) {
        return TA_ERR_NOMEM; /* a 32-bit size_t cannot count them */
    }
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(uint32_t),
                                               .alignment = _Alignof(uint32_t),
                                               .count = (size_t)grown });
    size_t bytes;
    ta_result result = ta_layout_finish(&layout, &bytes, NULL);
    if (result != TA_OK) {
        return result;
    }
    uint32_t *array = ta_mem_alloc(heap->allocator, bytes, _Alignof(uint32_t));
    if (array == NULL) {
        return TA_ERR_NOMEM;
    }
    /* Commit: nothing below allocates or fails. */
    if (heap->array != NULL) {
        ta_mem_copy(array, heap->array, (size_t)heap->count * sizeof(uint32_t));
        ta_mem_free(heap->allocator, heap->array,
                    &(ta_allocator_free_params){ .size = heap->array_bytes, .alignment = _Alignof(uint32_t) });
    }
    heap->array = array;
    heap->array_bytes = bytes;
    heap->capacity = (uint32_t)grown;
    return TA_OK;
}

/* ta_index_heap_insert: see index_heap.h. Validate first, then append at the end and sift up; nothing changes on failure. */
ta_result ta_index_heap_insert(ta_index_heap *heap, uint32_t index) {
    if (index == 0 || ta_pool_at(heap->pool, index) == NULL || ta_index_heap_back(heap, index) != 0) {
        return TA_ERR_INVALID;
    }
    if (heap->count == heap->capacity) {
        return TA_ERR_NOMEM;
    }
    heap->count += 1u;
    ta_index_heap_place(heap, heap->count, index);
    ta_index_heap_sift_up(heap, heap->count);
    return TA_OK;
}

/* ta_index_heap_peek_min: see index_heap.h. */
uint32_t ta_index_heap_peek_min(const ta_index_heap *heap) {
    return heap->count == 0 ? 0 : heap->array[0];
}

/*
 * ta_index_heap_remove_at takes the entry at 1-based `position` out of a
 * non-empty heap. The last entry fills the hole and then moves up or down,
 * whichever restores the order: up is needed when the last entry belongs to
 * another subtree and is before the hole's parent, down otherwise. Only one
 * of the two sifts moves it.
 */
static void ta_index_heap_remove_at(ta_index_heap *heap, uint32_t position) {
    uint32_t removed = heap->array[position - 1u];
    uint32_t last = heap->array[heap->count - 1u];
    heap->count -= 1u;
    ta_index_heap_set_back(heap, removed, 0);
    if (position == heap->count + 1u) {
        return; /* the removed entry was the last one */
    }
    ta_index_heap_place(heap, position, last);
    ta_index_heap_sift_up(heap, position);
    ta_index_heap_sift_down(heap, ta_index_heap_back(heap, last));
}

/* ta_index_heap_pop_min: see index_heap.h. */
uint32_t ta_index_heap_pop_min(ta_index_heap *heap) {
    if (heap->count == 0) {
        return 0;
    }
    uint32_t index = heap->array[0];
    ta_index_heap_remove_at(heap, 1u);
    return index;
}

/* ta_index_heap_remove: see index_heap.h. The item's own position makes this O(log n). */
bool ta_index_heap_remove(ta_index_heap *heap, uint32_t index) {
    if (index == 0 || ta_pool_at(heap->pool, index) == NULL) {
        return false;
    }
    uint32_t position = ta_index_heap_back(heap, index);
    if (position == 0 || position > heap->count || heap->array[position - 1u] != index) {
        return false;
    }
    ta_index_heap_remove_at(heap, position);
    return true;
}

/* ta_index_heap_count: see index_heap.h. */
uint32_t ta_index_heap_count(const ta_index_heap *heap) {
    return heap->count;
}

/* ta_index_heap_capacity: see index_heap.h. */
uint32_t ta_index_heap_capacity(const ta_index_heap *heap) {
    return heap->capacity;
}
