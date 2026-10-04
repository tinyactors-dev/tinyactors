#include "fifo.h"

/* ta_fifo_slot: the storage index of the index-th oldest entry. head < capacity and index <= count <= capacity, so the sum is below 2^33 in uint64_t and one subtraction reduces it. */
static uint32_t ta_fifo_slot(const ta_fifo *fifo, uint32_t index) {
    uint64_t slot = (uint64_t)fifo->head + index;
    if (slot >= fifo->capacity) {
        slot -= fifo->capacity;
    }
    return (uint32_t)slot;
}

/* ta_fifo_init: see fifo.h. */
ta_result ta_fifo_init(ta_fifo *fifo, const ta_fifo_params *params) {
    if (fifo == NULL) {
        return TA_ERR_INVALID;
    }
    ta_mem_zero(fifo, sizeof *fifo);
    if (params == NULL || params->entry_size == 0 || params->entry_alignment == 0 ||
        (params->entry_alignment & (params->entry_alignment - 1u)) != 0 ||
        params->entry_size % params->entry_alignment != 0) {
        return TA_ERR_INVALID;
    }
    if (params->storage != NULL && (uintptr_t)params->storage % params->entry_alignment != 0) {
        return TA_ERR_INVALID;
    }
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = params->entry_size,
                                               .alignment = params->entry_alignment,
                                               .count = params->capacity });
    size_t bytes = 0;
    size_t alignment = 0;
    ta_result result = ta_layout_finish(&layout, &bytes, &alignment);
    if (result != TA_OK) {
        return result;
    }
    unsigned char *entries = params->storage;
    bool owned = false;
    if (entries == NULL && params->capacity > 0) {
        entries = ta_mem_alloc(params->allocator, bytes, alignment);
        if (entries == NULL) {
            return TA_ERR_NOMEM;
        }
        owned = true;
    }
    *fifo = (ta_fifo){ .allocator = params->allocator,
                       .entries = entries,
                       .entry_size = params->entry_size,
                       .entry_alignment = alignment,
                       .bytes = bytes,
                       .capacity = params->capacity,
                       .head = 0,
                       .count = 0,
                       .owned = owned };
    return TA_OK;
}

/* ta_fifo_destroy: see fifo.h. The sized free uses the bytes and alignment stored at init. */
void ta_fifo_destroy(ta_fifo *fifo) {
    if (fifo->owned) {
        ta_mem_free(fifo->allocator, fifo->entries,
                    &(ta_allocator_free_params){ .size = fifo->bytes, .alignment = fifo->entry_alignment });
    }
    ta_mem_zero(fifo, sizeof *fifo);
}

/* ta_fifo_push: see fifo.h. The byte offset cannot overflow: init proved capacity * entry_size fits. */
bool ta_fifo_push(ta_fifo *fifo, const void *entry) {
    if (fifo->count == fifo->capacity) {
        return false;
    }
    ta_mem_copy(fifo->entries + (size_t)ta_fifo_slot(fifo, fifo->count) * fifo->entry_size, entry, fifo->entry_size);
    fifo->count += 1u;
    return true;
}

/* ta_fifo_pop: see fifo.h. */
bool ta_fifo_pop(ta_fifo *fifo, void *out) {
    if (fifo->count == 0) {
        return false;
    }
    if (out != NULL) {
        ta_mem_copy(out, fifo->entries + (size_t)fifo->head * fifo->entry_size, fifo->entry_size);
    }
    fifo->head = ta_fifo_slot(fifo, 1u);
    fifo->count -= 1u;
    return true;
}

/* ta_fifo_peek: see fifo.h. */
void *ta_fifo_peek(const ta_fifo *fifo, uint32_t index) {
    if (index >= fifo->count) {
        return NULL;
    }
    return fifo->entries + (size_t)ta_fifo_slot(fifo, index) * fifo->entry_size;
}

/* ta_fifo_clear: see fifo.h. */
void ta_fifo_clear(ta_fifo *fifo) {
    fifo->head = 0;
    fifo->count = 0;
}

/* ta_fifo_count: see fifo.h. */
uint32_t ta_fifo_count(const ta_fifo *fifo) { return fifo->count; }

/* ta_fifo_capacity: see fifo.h. */
uint32_t ta_fifo_capacity(const ta_fifo *fifo) { return fifo->capacity; }
