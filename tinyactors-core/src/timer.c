#include "timer.h"
#include "routing.h"

/* ------------------------------------------------------------------------
 * Keys and the owner heap
 */

typedef struct {
    ta_time due;
    ta_timer_id id;
} ta_timer_key;

static bool ta_timer_key_before(ta_timer_key left, ta_timer_key right) {
    return left.due < right.due || (left.due == right.due && left.id < right.id);
}

static ta_timer_key ta_timer_key_at(const ta_timer_list *list, uint32_t index) {
    return (ta_timer_key){ .due = list->sends[index].due, .id = list->meta[index].id };
}

static bool ta_timer_list_before(const ta_timer_list *left, const ta_timer_list *right) {
    return ta_timer_key_before(ta_timer_key_at(left, 0), ta_timer_key_at(right, 0));
}

static void ta_timer_place(ta_timer_heap *heap, uint32_t index, ta_timer_list *list) {
    heap->owners[index] = list;
    list->heap_index = index;
}

static void ta_timer_sift_up(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = heap->owners[index];
    while (index > 0) {
        uint32_t parent = (index - 1u) / 2u;
        if (!ta_timer_list_before(list, heap->owners[parent])) {
            break;
        }
        ta_timer_place(heap, index, heap->owners[parent]);
        index = parent;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_sift_down(ta_timer_heap *heap, uint32_t index) {
    ta_timer_list *list = heap->owners[index];
    for (;;) {
        uint32_t smallest = index;
        const ta_timer_list *best = list;
        uint32_t left = index * 2u + 1u;
        uint32_t right = left + 1u;
        if (left < heap->count && ta_timer_list_before(heap->owners[left], best)) {
            smallest = left;
            best = heap->owners[left];
        }
        if (right < heap->count && ta_timer_list_before(heap->owners[right], best)) {
            smallest = right;
        }
        if (smallest == index) {
            break;
        }
        ta_timer_place(heap, index, heap->owners[smallest]);
        index = smallest;
    }
    ta_timer_place(heap, index, list);
}

static void ta_timer_unindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (!list->indexed) {
        return;
    }
    uint32_t index = list->heap_index;
    list->indexed = false;
    heap->count -= 1u;
    if (index == heap->count) {
        return;
    }
    ta_timer_place(heap, index, heap->owners[heap->count]);
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, index);
}

/*
 * Restores the heap after the list's front may have changed. Inserting a
 * list needs a free heap slot; ta_timer_reserve_heap() provides it.
 */
static void ta_timer_reindex(ta_timer_heap *heap, ta_timer_list *list) {
    if (list->count == 0) {
        ta_timer_unindex(heap, list);
        return;
    }
    if (!list->indexed) {
        list->indexed = true;
        ta_timer_place(heap, heap->count, list);
        heap->count += 1u;
        ta_timer_sift_up(heap, list->heap_index);
        return;
    }
    uint32_t index = list->heap_index;
    ta_timer_sift_down(heap, index);
    ta_timer_sift_up(heap, list->heap_index);
}

static uint32_t ta_timer_grow_capacity(uint32_t current, uint32_t needed) {
    uint32_t capacity = current == 0 ? 4u : current;
    while (capacity < needed) {
        if (capacity > UINT32_MAX / 2u) {
            return needed;
        }
        capacity *= 2u;
    }
    return capacity;
}

static ta_result ta_timer_reserve_heap(ta_timer_heap *heap, uint32_t additional) {
    if (additional > UINT32_MAX - heap->count) {
        return TA_ERR_NOMEM;
    }
    if (additional <= heap->capacity - heap->count) {
        return TA_OK;
    }
    uint32_t capacity = ta_timer_grow_capacity(heap->capacity, heap->count + additional);
    ta_timer_list **owners = ta_mem_alloc(heap->allocator, (size_t)capacity * sizeof(ta_timer_list *),
                                          _Alignof(ta_timer_list *));
    if (owners == NULL) {
        return TA_ERR_NOMEM;
    }
    if (heap->owners != NULL) {
        ta_mem_copy(owners, heap->owners, (size_t)heap->count * sizeof(ta_timer_list *));
        ta_mem_free(heap->allocator, heap->owners,
                    &(ta_allocator_free_params){ .size = (size_t)heap->capacity * sizeof(ta_timer_list *),
                                                 .alignment = _Alignof(ta_timer_list *) });
    }
    heap->owners = owners;
    heap->capacity = capacity;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * One owner's list
 */

static void ta_timer_free_storage(ta_allocator allocator, ta_timer_list *list) {
    if (list->capacity == 0) {
        return;
    }
    ta_mem_free(allocator, list->sends,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_info),
                                             .alignment = _Alignof(ta_delayed_send_info) });
    ta_mem_free(allocator, list->meta,
                &(ta_allocator_free_params){ .size = (size_t)list->capacity * sizeof(ta_delayed_send_meta),
                                             .alignment = _Alignof(ta_delayed_send_meta) });
    list->sends = NULL;
    list->meta = NULL;
    list->capacity = 0;
}

static ta_result ta_timer_reserve_list(ta_allocator allocator, ta_timer_list *list, uint32_t additional) {
    if (additional > UINT32_MAX - list->count) {
        return TA_ERR_NOMEM;
    }
    if (additional <= list->capacity - list->count) {
        return TA_OK;
    }
    uint32_t capacity = ta_timer_grow_capacity(list->capacity, list->count + additional);
    ta_delayed_send_info *sends = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_info),
                                               _Alignof(ta_delayed_send_info));
    ta_delayed_send_meta *meta = ta_mem_alloc(allocator, (size_t)capacity * sizeof(ta_delayed_send_meta),
                                              _Alignof(ta_delayed_send_meta));
    if (sends == NULL || meta == NULL) {
        if (sends != NULL) {
            ta_mem_free(allocator, sends,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_info),
                                                     .alignment = _Alignof(ta_delayed_send_info) });
        }
        if (meta != NULL) {
            ta_mem_free(allocator, meta,
                        &(ta_allocator_free_params){ .size = (size_t)capacity * sizeof(ta_delayed_send_meta),
                                                     .alignment = _Alignof(ta_delayed_send_meta) });
        }
        return TA_ERR_NOMEM;
    }
    if (list->count != 0) {
        ta_mem_copy(sends, list->sends, (size_t)list->count * sizeof(ta_delayed_send_info));
        ta_mem_copy(meta, list->meta, (size_t)list->count * sizeof(ta_delayed_send_meta));
    }
    ta_timer_free_storage(allocator, list);
    list->sends = sends;
    list->meta = meta;
    list->capacity = capacity;
    return TA_OK;
}

static void ta_timer_split(const ta_delayed_send *send, ta_delayed_send_info *info, ta_delayed_send_meta *meta) {
    *info = (ta_delayed_send_info){
        .send_id = send->send_id,
        .due = send->due,
        .event = send->event,
        .io_type = send->io_type,
        .destination = send->destination,
        .target = send->target,
        .data = send->data,
        .submitted = send->submitted,
        .has_target_text = send->has_target_text,
        .target_text = send->target_text,
    };
    *meta = (ta_delayed_send_meta){
        .id = send->id,
        .owner = send->owner,
        .origin = send->origin,
        .invoke_id = send->invoke_id,
        .has_content = send->has_content,
        .content = send->content,
        .trace = send->trace,
    };
}

static ta_delayed_send ta_timer_join(const ta_timer_list *list, uint32_t index) {
    const ta_delayed_send_info *info = &list->sends[index];
    const ta_delayed_send_meta *meta = &list->meta[index];
    return (ta_delayed_send){
        .id = meta->id,
        .owner = meta->owner,
        .send_id = info->send_id,
        .due = info->due,
        .event = info->event,
        .io_type = info->io_type,
        .destination = info->destination,
        .target = info->target,
        .data = info->data,
        .submitted = info->submitted,
        .origin = meta->origin,
        .invoke_id = meta->invoke_id,
        .has_content = meta->has_content,
        .content = meta->content,
        .has_target_text = info->has_target_text,
        .target_text = info->target_text,
        .trace = meta->trace,
    };
}

/* The index of the first stored send that sorts after key. */
static uint32_t ta_timer_upper_bound(const ta_timer_list *list, ta_timer_key key) {
    uint32_t low = 0;
    uint32_t high = list->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2u;
        if (ta_timer_key_before(key, ta_timer_key_at(list, middle))) {
            high = middle;
        } else {
            low = middle + 1u;
        }
    }
    return low;
}

/* Inserts into reserved capacity; the caller reindexes. */
static void ta_timer_insert(ta_timer_list *list, const ta_delayed_send *send) {
    uint32_t at = ta_timer_upper_bound(list, (ta_timer_key){ .due = send->due, .id = send->id });
    for (uint32_t index = list->count; index > at; index -= 1u) {
        list->sends[index] = list->sends[index - 1u];
        list->meta[index] = list->meta[index - 1u];
    }
    ta_timer_split(send, &list->sends[at], &list->meta[at]);
    list->count += 1u;
}

typedef struct {
    ta_send_id send_id;
    bool any_send_id;
    bool include_submitted;
} ta_timer_selection;

static bool ta_timer_selected(const ta_delayed_send_info *send, const ta_timer_selection *selection) {
    return (selection->any_send_id || send->send_id == selection->send_id) &&
           (selection->include_submitted || !send->submitted);
}

/* Releases and removes the selected sends, keeping order; reindexes the list. */
static bool ta_timer_remove_selected(ta_timer_heap *heap, ta_timer_list *list, const ta_timer_selection *selection) {
    if (list == NULL) {
        return false;
    }
    uint32_t kept = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (ta_timer_selected(&list->sends[index], selection)) {
            ta_value_release(&list->sends[index].target);
            ta_value_release(&list->sends[index].data);
            /* A stored send owns its destination and origin references (ADR 0065). */
            ta_actor_ref_release(&list->sends[index].destination);
            ta_actor_ref_release(&list->meta[index].origin);
            continue;
        }
        list->sends[kept] = list->sends[index];
        list->meta[kept] = list->meta[index];
        kept += 1u;
    }
    bool found = kept != list->count;
    list->count = kept;
    if (found) {
        ta_timer_reindex(heap, list);
    }
    return found;
}

/* ------------------------------------------------------------------------
 * Public within core
 */

ta_result ta_timer_heap_init(ta_timer_heap *heap, ta_allocator allocator) {
    *heap = (ta_timer_heap){ .allocator = allocator, .next_id = 1 };
    return TA_OK;
}

void ta_timer_heap_destroy(ta_timer_heap *heap) {
    /* Lists still indexed belong to actors that outlived the system's teardown. */
    while (heap->count != 0) {
        ta_timer_list *list = heap->owners[0];
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    }
    ta_timer_free_storage(heap->allocator, &heap->orphans);
    if (heap->owners != NULL) {
        ta_mem_free(heap->allocator, heap->owners,
                    &(ta_allocator_free_params){ .size = (size_t)heap->capacity * sizeof(ta_timer_list *),
                                                 .alignment = _Alignof(ta_timer_list *) });
    }
    *heap = (ta_timer_heap){0};
}

ta_result ta_timer_reserve(ta_timer_heap *heap, ta_timer_list *list, uint32_t additional) {
    if (additional == 0) {
        return TA_OK;
    }
    ta_result result = ta_timer_reserve_heap(heap, list->indexed ? 0u : 1u);
    return result == TA_OK ? ta_timer_reserve_list(heap->allocator, list, additional) : result;
}

ta_result ta_timer_schedule(ta_timer_heap *heap, ta_timer_list *list, const ta_delayed_send *send) {
    ta_result result = ta_timer_reserve(heap, list, 1u);
    if (result != TA_OK) {
        return result;
    }
    ta_delayed_send record = *send;
    if (record.id == 0) {
        record.id = heap->next_id;
        heap->next_id += 1u;
    }
    ta_timer_insert(list, &record);
    ta_timer_reindex(heap, list);
    return TA_OK;
}

ta_result ta_timer_cancel(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .send_id = send_id }) ? TA_OK
                                                                                            : TA_ERR_NOT_FOUND;
}

ta_result ta_timer_withdraw(ta_timer_heap *heap, ta_timer_list *list, ta_send_id send_id) {
    return ta_timer_remove_selected(heap, list,
                                    &(ta_timer_selection){ .send_id = send_id, .include_submitted = true })
               ? TA_OK
               : TA_ERR_NOT_FOUND;
}

void ta_timer_cancel_unsubmitted(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list, &(ta_timer_selection){ .any_send_id = true });
}

static uint32_t ta_timer_submitted_count(const ta_timer_list *list) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        count += list->sends[index].submitted ? 1u : 0u;
    }
    return count;
}

ta_result ta_timer_reserve_orphans(ta_timer_heap *heap, const ta_timer_list *list) {
    if (list == NULL || heap->closing) {
        return TA_OK;
    }
    return ta_timer_reserve(heap, &heap->orphans, ta_timer_submitted_count(list));
}

void ta_timer_orphan(ta_timer_heap *heap, ta_timer_list *list) {
    if (list == NULL) {
        return;
    }
    ta_timer_cancel_unsubmitted(heap, list);
    if (heap->closing) {
        (void)ta_timer_remove_selected(heap, list,
                                       &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
        return;
    }
    if (list->count == 0) {
        return;
    }
    /* Merge the two sorted lists from the back into the reserved room. */
    ta_timer_list *orphans = &heap->orphans;
    uint32_t left = orphans->count;
    uint32_t right = list->count;
    uint32_t out = left + right;
    while (right > 0) {
        out -= 1u;
        if (left > 0 && ta_timer_key_before(ta_timer_key_at(list, right - 1u), ta_timer_key_at(orphans, left - 1u))) {
            left -= 1u;
            orphans->sends[out] = orphans->sends[left];
            orphans->meta[out] = orphans->meta[left];
        } else {
            right -= 1u;
            orphans->sends[out] = list->sends[right];
            orphans->meta[out] = list->meta[right];
        }
    }
    orphans->count += list->count;
    list->count = 0;
    ta_timer_unindex(heap, list);
    ta_timer_reindex(heap, orphans);
}

void ta_timer_list_destroy(ta_timer_heap *heap, ta_timer_list *list) {
    (void)ta_timer_remove_selected(heap, list,
                                   &(ta_timer_selection){ .any_send_id = true, .include_submitted = true });
    ta_timer_unindex(heap, list);
    ta_timer_free_storage(heap->allocator, list);
}

size_t ta_timer_list_bytes(const ta_timer_list *list) {
    return list == NULL ? 0u
                        : (size_t)list->capacity * (sizeof(ta_delayed_send_info) + sizeof(ta_delayed_send_meta));
}

uint32_t ta_timer_copy_unsubmitted(const ta_timer_list *list, ta_delayed_send *out) {
    if (list == NULL) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t index = 0; index < list->count; index += 1u) {
        if (list->sends[index].submitted) {
            continue;
        }
        if (out != NULL) {
            out[count] = ta_timer_join(list, index);
        }
        count += 1u;
    }
    return count;
}

bool ta_timer_next_due(const ta_timer_heap *heap, ta_time *out) {
    if (heap->count == 0) {
        return false;
    }
    *out = heap->owners[0]->sends[0].due;
    return true;
}

ta_result ta_timer_pop(ta_timer_heap *heap, ta_delayed_send *out) {
    if (heap->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    ta_timer_list *list = heap->owners[0];
    *out = ta_timer_join(list, 0);
    for (uint32_t index = 1; index < list->count; index += 1u) {
        list->sends[index - 1u] = list->sends[index];
        list->meta[index - 1u] = list->meta[index];
    }
    list->count -= 1u;
    ta_timer_reindex(heap, list);
    return TA_OK;
}
