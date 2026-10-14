#include <tinyactors/table.h>
#include "base/memory.h"

typedef struct ta_table_slot {
    uint32_t generation;
    uint32_t next; /* slot + 1 in the free list */
} ta_table_slot;

/* Rows and slot records share one allocation. row_size is rounded to 8,
 * so the metadata is aligned too on 32- and 64-bit targets. */
static ta_table_slot *table_slots(const ta_table *table) {
    return (ta_table_slot *)(void *)(table->rows + (size_t)table->row_size * table->capacity);
}

void ta_table_init(ta_table *table, const ta_table_config *config) {
    ta_memset(table, 0, sizeof *table);
    if (!config || config->size < sizeof *config || !config->row_size ||
        config->row_size > UINT32_MAX - 7u || !config->allocator ||
        !config->allocator->alloc || !config->allocator->free) return;
    table->row_size = (config->row_size + 7u) & ~7u;
    table->allocator = config->allocator;
}

/* Build a replacement completely before publishing it. Refusal preserves
 * the old allocation, free list, rows and handle generations. */
static int table_grow(ta_table *table) {
    if (table->capacity > UINT32_MAX / 2u) return 0;
    uint32_t capacity = table->capacity ? table->capacity * 2u : 16u;
    size_t stride = (size_t)table->row_size + sizeof(ta_table_slot);
    if (stride < table->row_size || capacity > SIZE_MAX / stride) return 0;
    size_t bytes = stride * capacity;
    unsigned char *rows = table->allocator->alloc(table->allocator->context, bytes, 8);
    if (!rows) return 0;
    ta_memset(rows, 0, bytes);
    ta_table_slot *slots = (ta_table_slot *)(void *)(rows + (size_t)table->row_size * capacity);
    if (table->capacity) {
        ta_memcpy(rows, table->rows, (size_t)table->row_size * table->capacity);
        ta_memcpy(slots, table_slots(table), sizeof *slots * table->capacity);
    }
    uint32_t first = table->free;
    for (uint32_t slot = capacity; slot > table->capacity;) {
        --slot;
        slots[slot].next = first;
        first = slot + 1u;
    }
    if (table->rows)
        table->allocator->free(table->allocator->context, table->rows, stride * table->capacity);
    table->rows = rows;
    table->capacity = capacity;
    table->free = first;
    return 1;
}

void *ta_table_add(ta_table *table, uint64_t *handle) {
    if (!handle) return NULL;
    *handle = 0;
    if (!table->allocator || !table->row_size) return NULL;
    if (!table->free && !table_grow(table)) return NULL;
    uint32_t slot = table->free - 1u;
    ta_table_slot *record = &table_slots(table)[slot];
    table->free = record->next;
    ++record->generation; /* even -> odd; retired slots never enter this list */
    record->next = 0;
    ++table->count;
    *handle = (uint64_t)record->generation << 32 | (uint64_t)(slot + 1u);
    void *row = table->rows + (size_t)slot * table->row_size;
    ta_memset(row, 0, table->row_size);
    return row;
}

uint64_t ta_table_handle(const ta_table *table, uint32_t slot) {
    if (slot >= table->capacity) return 0;
    uint32_t generation = table_slots(table)[slot].generation;
    return generation & 1u ? (uint64_t)generation << 32 | (uint64_t)(slot + 1u) : 0;
}

void *ta_table_at(const ta_table *table, uint32_t slot) {
    return ta_table_handle(table, slot) ? table->rows + (size_t)slot * table->row_size : NULL;
}

void *ta_table_get(const ta_table *table, uint64_t handle) {
    uint32_t encoded = (uint32_t)handle;
    if (!encoded || !(handle >> 32 & 1u)) return NULL;
    uint32_t slot = encoded - 1u;
    return ta_table_handle(table, slot) == handle ? ta_table_at(table, slot) : NULL;
}

void ta_table_remove(ta_table *table, uint64_t handle) {
    void *row = ta_table_get(table, handle);
    if (!row) return;
    uint32_t slot = (uint32_t)handle - 1u;
    ta_table_slot *record = &table_slots(table)[slot];
    ta_memset(row, 0, table->row_size);
    --table->count;
    if (record->generation == UINT32_MAX) {
        record->generation = 0; /* exhausted: retire, never wrap to a live handle */
        record->next = 0;
        return;
    }
    ++record->generation;
    record->next = table->free;
    table->free = slot + 1u;
}

/* magic, row size, capacity, free; then capacity slot records */
#define TABLE_SNAPSHOT_MAGIC 0x54415442u /* "TATB" */

ta_status ta_table_snapshot(const ta_table *table, ta_buffer *out) {
    size_t need = 4 * sizeof(uint32_t) + sizeof(ta_table_slot) * (size_t)table->capacity;
    if (out->capacity < need || !out->pointer) {
        out->length = need;
        return TA_ERROR_NO_MEMORY;
    }
    uint32_t header[4] = {TABLE_SNAPSHOT_MAGIC, table->row_size, table->capacity, table->free};
    ta_memcpy(out->pointer, header, sizeof header);
    if (table->capacity)
        ta_memcpy((unsigned char *)out->pointer + sizeof header, table_slots(table),
                  sizeof(ta_table_slot) * table->capacity);
    out->length = need;
    return TA_OK;
}

ta_status ta_table_restore(ta_table *table, ta_bytes snapshot) {
    if (!table->allocator || !table->row_size || table->capacity || table->count) return TA_ERROR_STATE;
    uint32_t header[4];
    if (snapshot.length < sizeof header) return TA_ERROR_INVALID;
    ta_memcpy(header, snapshot.pointer, sizeof header);
    uint32_t capacity = header[2], free = header[3];
    if (header[0] != TABLE_SNAPSHOT_MAGIC || header[1] != table->row_size || free > capacity ||
        snapshot.length != sizeof header + sizeof(ta_table_slot) * (size_t)capacity)
        return TA_ERROR_INVALID;
    const unsigned char *records = (const unsigned char *)snapshot.pointer + sizeof header;
    uint32_t count = 0;
    for (uint32_t slot = 0; slot < capacity; slot++) {
        ta_table_slot record;
        ta_memcpy(&record, records + sizeof record * slot, sizeof record);
        if (record.next > capacity || ((record.generation & 1u) && record.next)) return TA_ERROR_INVALID;
        count += record.generation & 1u;
    }
    /* the free list: free slots only, ending within capacity steps */
    uint32_t at = free, steps = 0;
    while (at) {
        ta_table_slot record;
        ta_memcpy(&record, records + sizeof record * (at - 1u), sizeof record);
        if ((record.generation & 1u) || ++steps > capacity) return TA_ERROR_INVALID;
        at = record.next;
    }
    if (!capacity) return TA_OK;
    size_t stride = (size_t)table->row_size + sizeof(ta_table_slot);
    if (capacity > SIZE_MAX / stride) return TA_ERROR_INVALID;
    unsigned char *rows = table->allocator->alloc(table->allocator->context, stride * capacity, 8);
    if (!rows) return TA_ERROR_NO_MEMORY;
    ta_memset(rows, 0, (size_t)table->row_size * capacity);
    ta_memcpy(rows + (size_t)table->row_size * capacity, records, sizeof(ta_table_slot) * capacity);
    table->rows = rows;
    table->capacity = capacity;
    table->free = free;
    table->count = count;
    return TA_OK;
}

void ta_table_free(ta_table *table) {
    if (table->rows) {
        size_t bytes = ((size_t)table->row_size + sizeof(ta_table_slot)) * table->capacity;
        table->allocator->free(table->allocator->context, table->rows, bytes);
    }
    ta_table_init(table, NULL);
}
