#include "heap.h"

/* An item's key, read once per item that moves. */
typedef struct {
    uint64_t due;
    uint64_t order;
} ta_heap_key;

/* An item and the position it goes to. */
typedef struct {
    uint32_t position;
    uint32_t item;
} ta_heap_slot;

static unsigned char *ta_heap_item(const ta_heap_items *items, uint32_t item) {
    return items->item(items->items, item);
}

static ta_heap_key ta_heap_key_of(const ta_heap_items *items, uint32_t item) {
    const unsigned char *address = ta_heap_item(items, item);
    return (ta_heap_key){
        .due = *(const uint64_t *)(const void *)(address + items->due_offset),
        .order = *(const uint64_t *)(const void *)(address + items->order_offset),
    };
}

static bool ta_heap_key_less(ta_heap_key left, ta_heap_key right) {
    return left.due < right.due || (left.due == right.due && left.order < right.order);
}

static uint32_t ta_heap_at(const ta_heap *heap, uint32_t position) {
    uint32_t item;
    ta_paged_array_get(&heap->positions, position, &item);
    return item;
}

/* Puts the item at its position and records the position in the item. */
static void ta_heap_place(ta_heap *heap, const ta_heap_items *items, ta_heap_slot slot) {
    ta_paged_array_set(&heap->positions, slot.position, &slot.item);
    *(uint32_t *)(void *)(ta_heap_item(items, slot.item) + items->position_offset) = slot.position;
}

/* Moves the hole at `slot.position` towards the root until `slot.item`
 * fits there, then places it. */
static void ta_heap_sift_up(ta_heap *heap, const ta_heap_items *items, ta_heap_slot slot) {
    ta_heap_key key = ta_heap_key_of(items, slot.item);
    while (slot.position > 0u) {
        uint32_t parent = (slot.position - 1u) / 2u;
        uint32_t above = ta_heap_at(heap, parent);
        if (!ta_heap_key_less(key, ta_heap_key_of(items, above))) {
            break;
        }
        ta_heap_place(heap, items, (ta_heap_slot){ .position = slot.position, .item = above });
        slot.position = parent;
    }
    ta_heap_place(heap, items, slot);
}

/* Moves the hole at `slot.position` towards the leaves until `slot.item`
 * fits there, then places it. */
static void ta_heap_sift_down(ta_heap *heap, const ta_heap_items *items, ta_heap_slot slot) {
    ta_heap_key key = ta_heap_key_of(items, slot.item);
    for (;;) {
        uint64_t left = (uint64_t)slot.position * 2u + 1u;
        if (left >= heap->length) {
            break;
        }
        uint32_t child = (uint32_t)left;
        uint32_t below = ta_heap_at(heap, child);
        ta_heap_key below_key = ta_heap_key_of(items, below);
        if (left + 1u < heap->length) {
            uint32_t right = ta_heap_at(heap, child + 1u);
            ta_heap_key right_key = ta_heap_key_of(items, right);
            if (ta_heap_key_less(right_key, below_key)) {
                child += 1u;
                below = right;
                below_key = right_key;
            }
        }
        if (!ta_heap_key_less(below_key, key)) {
            break;
        }
        ta_heap_place(heap, items, (ta_heap_slot){ .position = slot.position, .item = below });
        slot.position = child;
    }
    ta_heap_place(heap, items, slot);
}

/* Takes the item at `position` out, filling its place with the last item. */
static void ta_heap_remove_at(ta_heap *heap, const ta_heap_items *items, uint32_t position) {
    heap->length -= 1u;
    if (position == heap->length) {
        return;
    }
    ta_heap_slot slot = { .position = position, .item = ta_heap_at(heap, heap->length) };
    if (position > 0u &&
        ta_heap_key_less(ta_heap_key_of(items, slot.item), ta_heap_key_of(items, ta_heap_at(heap, (position - 1u) / 2u)))) {
        ta_heap_sift_up(heap, items, slot);
    } else {
        ta_heap_sift_down(heap, items, slot);
    }
}

ta_result ta_heap_init(ta_heap *heap, const ta_heap_params *params) {
    ta_paged_array positions;
    ta_result result = ta_paged_array_init(&positions, &(ta_paged_array_params){
                                                           .allocator = params->allocator,
                                                           .entry_size = sizeof(uint32_t),
                                                           .entry_alignment = _Alignof(uint32_t),
                                                           .page_shift = params->page_shift,
                                                       });
    if (result != TA_OK) {
        return result;
    }
    *heap = (ta_heap){ .positions = positions, .length = 0u };
    return TA_OK;
}

void ta_heap_destroy(ta_heap *heap) {
    ta_paged_array_destroy(&heap->positions);
    heap->length = 0u;
}

ta_result ta_heap_grow(ta_heap *heap, uint32_t count) {
    return ta_paged_array_grow(&heap->positions, count);
}

void ta_heap_shrink(ta_heap *heap, uint32_t keep) {
    ta_paged_array_shrink(&heap->positions, keep > heap->length ? keep : heap->length);
}

ta_result ta_heap_insert(ta_heap *heap, const ta_heap_items *items, uint32_t item) {
    if ((uint64_t)heap->length >= ta_heap_capacity(heap)) {
        return TA_ERR_FULL;
    }
    ta_heap_slot slot = { .position = heap->length, .item = item };
    heap->length += 1u;
    ta_heap_sift_up(heap, items, slot);
    return TA_OK;
}

uint32_t ta_heap_pop(ta_heap *heap, const ta_heap_items *items) {
    uint32_t item = ta_heap_head(heap);
    if (item != 0u) {
        ta_heap_remove_at(heap, items, 0u);
    }
    return item;
}

ta_result ta_heap_remove(ta_heap *heap, const ta_heap_items *items, uint32_t item) {
    if (item == 0u) {
        return TA_ERR_NOT_FOUND;
    }
    uint32_t position = *(const uint32_t *)(const void *)(ta_heap_item(items, item) + items->position_offset);
    if (position >= heap->length || ta_heap_at(heap, position) != item) {
        return TA_ERR_NOT_FOUND;
    }
    ta_heap_remove_at(heap, items, position);
    return TA_OK;
}
