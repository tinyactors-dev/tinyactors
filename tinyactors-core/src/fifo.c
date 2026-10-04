#include "fifo.h"
#include "memory.h"
#include "../include/tinyactors.h"

static void ta_fifo_free_entries(ta_fifo *fifo) {
    if (fifo->entries == NULL) {
        return;
    }
    ta_mem_free(fifo->allocator, fifo->entries,
                &(ta_allocator_free_params){
                    .size = (size_t)fifo->capacity * fifo->entry_size,
                    .alignment = fifo->entry_alignment,
                });
}

ta_result ta_fifo_init(ta_fifo *fifo, const ta_fifo_params *params) {
    size_t alignment = params->entry_alignment;
    if (params->entry_size == 0 || alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        params->entry_size % alignment != 0) {
        return TA_ERR_INVALID;
    }
    *fifo = (ta_fifo){
        .allocator = params->allocator,
        .entry_size = params->entry_size,
        .entry_alignment = alignment,
    };
    return TA_OK;
}

void ta_fifo_destroy(ta_fifo *fifo) {
    ta_fifo_free_entries(fifo);
    fifo->entries = NULL;
    fifo->capacity = 0;
    fifo->head = 0;
    fifo->count = 0;
}

ta_result ta_fifo_reserve(ta_fifo *fifo, uint32_t capacity) {
    if (capacity <= fifo->capacity) {
        return TA_OK;
    }
    if (capacity > SIZE_MAX / fifo->entry_size) {
        return TA_ERR_NOMEM;
    }
    unsigned char *entries = ta_mem_alloc(fifo->allocator, (size_t)capacity * fifo->entry_size,
                                          fifo->entry_alignment);
    if (entries == NULL) {
        return TA_ERR_NOMEM;
    }
    /* The entries are at most two runs of the old array: from the head to
     * its end, then from its start. */
    if (fifo->count != 0) {
        uint32_t to_end = fifo->capacity - fifo->head;
        uint32_t first = fifo->count < to_end ? fifo->count : to_end;
        ta_mem_copy(entries, fifo->entries + (size_t)fifo->head * fifo->entry_size,
                    (size_t)first * fifo->entry_size);
        ta_mem_copy(entries + (size_t)first * fifo->entry_size, fifo->entries,
                    (size_t)(fifo->count - first) * fifo->entry_size);
    }
    ta_fifo_free_entries(fifo);
    fifo->entries = entries;
    fifo->capacity = capacity;
    fifo->head = 0;
    return TA_OK;
}

ta_result ta_fifo_push(ta_fifo *fifo, const void *entry) {
    if (fifo->count == fifo->capacity) {
        return TA_ERR_FULL;
    }
    fifo->count += 1u;
    ta_mem_copy(ta_fifo_at(fifo, fifo->count - 1u), entry, fifo->entry_size);
    return TA_OK;
}

ta_result ta_fifo_pop(ta_fifo *fifo, void *out) {
    if (fifo->count == 0) {
        return TA_ERR_NOT_FOUND;
    }
    if (out != NULL) {
        ta_mem_copy(out, ta_fifo_peek(fifo), fifo->entry_size);
    }
    fifo->count -= 1u;
    fifo->head = fifo->count == 0 ? 0u : (fifo->head + 1u == fifo->capacity ? 0u : fifo->head + 1u);
    return TA_OK;
}
