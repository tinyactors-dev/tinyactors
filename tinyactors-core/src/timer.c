#include "timer.h"

static bool ta_timer_before(const ta_delayed_send *left, const ta_delayed_send *right) {
    return left->due < right->due || (left->due == right->due && left->id < right->id);
}

static void ta_timer_swap(ta_timer_heap *heap, uint32_t left, uint32_t right) {
    ta_delayed_send swap = heap->items[left];
    heap->items[left] = heap->items[right];
    heap->items[right] = swap;
    heap->items[left].heap_index = left;
    heap->items[right].heap_index = right;
}

static void ta_timer_sift_up(ta_timer_heap *heap, uint32_t index) {
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (!ta_timer_before(&heap->items[index], &heap->items[parent])) {
            return;
        }
        ta_timer_swap(heap, index, parent);
        index = parent;
    }
}

static void ta_timer_sift_down(ta_timer_heap *heap, uint32_t index) {
    for (;;) {
        uint32_t smallest = index;
        uint32_t left = index * 2u + 1u;
        uint32_t right = left + 1u;
        if (left < heap->count && ta_timer_before(&heap->items[left], &heap->items[smallest])) {
            smallest = left;
        }
        if (right < heap->count && ta_timer_before(&heap->items[right], &heap->items[smallest])) {
            smallest = right;
        }
        if (smallest == index) {
            return;
        }
        ta_timer_swap(heap, index, smallest);
        index = smallest;
    }
}

static void ta_timer_remove_at(ta_timer_heap *heap, uint32_t index) {
    heap->count -= 1u;
    if (index == heap->count) {
        return;
    }
    heap->items[index] = heap->items[heap->count];
    heap->items[index].heap_index = index;
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, index);
}

static void ta_timer_release(ta_delayed_send *send) {
    ta_value_release(&send->target);
    ta_value_release(&send->data);
}

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator) {
    *heap = (ta_timer_heap){ .allocator = allocator, .next_id = 1 };
    return TA_OK;
}

void ta_timer_heap_destroy(ta_timer_heap *heap) {
    for (uint32_t index = 0; index < heap->count; index += 1) {
        ta_timer_release(&heap->items[index]);
    }
    ta_mem_free(heap->allocator, heap->items,
                &(ta_allocator_free_params){ .size = heap->capacity * sizeof(ta_delayed_send),
                                             .alignment = _Alignof(ta_delayed_send) });
    *heap = (ta_timer_heap){0};
}

ta_result ta_timer_schedule(ta_timer_heap *heap, const ta_delayed_send *send) {
    if (heap->count == heap->capacity) {
        uint32_t capacity = heap->capacity == 0 ? 16u : heap->capacity * 2u;
        ta_delayed_send *items = ta_mem_alloc(heap->allocator, capacity * sizeof(ta_delayed_send),
                                              _Alignof(ta_delayed_send));
        if (items == NULL) {
            return TA_ERR_NOMEM;
        }
        if (heap->items != NULL) {
            ta_mem_copy(items, heap->items, heap->count * sizeof(ta_delayed_send));
            ta_mem_free(heap->allocator, heap->items,
                        &(ta_allocator_free_params){ .size = heap->capacity * sizeof(ta_delayed_send),
                                                     .alignment = _Alignof(ta_delayed_send) });
        }
        heap->items = items;
        heap->capacity = capacity;
    }
    uint32_t index = heap->count;
    heap->items[index] = *send;
    if (heap->items[index].id == 0) {
        heap->items[index].id = heap->next_id;
        heap->next_id += 1u;
    }
    heap->items[index].heap_index = index;
    heap->items[index].active = true;
    heap->count += 1u;
    ta_timer_sift_up(heap, index);
    return TA_OK;
}

typedef struct {
    ta_actor_id owner;
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
} ta_timer_selection;

static bool ta_timer_remove_selected(ta_timer_heap *heap, const ta_timer_selection *selection) {
    bool found = false;
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == selection->owner && (selection->any_send_id || send->send_id == selection->send_id) &&
            (selection->include_submitted || !send->submitted)) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            found = true;
            index = heap->count + 1u;
        }
    }
    return found;
}

ta_result ta_timer_withdraw(ta_timer_heap *heap,
                            ta_actor_id owner,
                            ta_send_id send_id) {
    return ta_timer_remove_selected(heap, &(ta_timer_selection){ .owner = owner, .send_id = send_id,
                                                                 .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_actor_id owner) {
    (void)ta_timer_remove_selected(heap, &(ta_timer_selection){ .owner = owner, .any_send_id = true });
}

ta_result ta_timer_cancel(ta_timer_heap *heap,
                          ta_actor_id owner,
                          ta_send_id send_id) {
    bool found = false;
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == owner && send->send_id == send_id && !send->submitted) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            found = true;
            /* Removal reorders the tail; restart the scan conservatively. */
            index = heap->count + 1u;
        }
    }
    return found ? TA_OK : TA_ERR_NOT_FOUND;
}

void ta_timer_cancel_owner(ta_timer_heap *heap, ta_actor_id owner) {
    for (uint32_t index = heap->count; index > 0; index -= 1u) {
        ta_delayed_send *send = &heap->items[index - 1u];
        if (send->owner == owner) {
            ta_timer_release(send);
            ta_timer_remove_at(heap, index - 1u);
            index = heap->count + 1u;
        }
    }
}

uint32_t ta_timer_count_owner(const ta_timer_heap *heap, ta_actor_id owner) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < heap->count; index += 1) {
        count += heap->items[index].owner == owner ? 1u : 0u;
    }
    return count;
}

uint32_t ta_timer_copy_unsubmitted(const ta_timer_heap *heap, ta_actor_id owner, ta_delayed_send *out) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < heap->count; index += 1) {
        const ta_delayed_send *send = &heap->items[index];
        if (send->owner != owner || send->submitted) {
            continue;
        }
        if (out != NULL) {
            out[count] = *send;
        }
        count += 1u;
    }
    return count;
}

const ta_delayed_send *ta_timer_peek(const ta_timer_heap *heap) {
    return heap->count == 0 ? NULL : &heap->items[0];
}

ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out) {
    if (heap->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    *out = heap->items[0];
    ta_timer_remove_at(heap, 0);
    return TA_OK;
}
