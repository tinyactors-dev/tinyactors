/* handles.c — tables of rows: the native datamodel's payloads (with
 * reference counts), sessions, charts and <foreach> iterations, and a
 * host's images. A handle is a row's generation and slot, never a pointer,
 * so every JavaScript value the binding holds is in a row (a napi_ref),
 * and a stale handle is refused (bindings.md, section 8). The tables grow
 * by doubling, reuse freed slots and are freed once, with the host: with
 * host.c's create, free, buffers and allocator, the only places that call
 * the allocator (AGENTS.md, "The binding's memory"). A payload's count
 * lives in its row, not in its napi_ref: a finalizer may call only
 * basic-env functions, and napi_reference_unref is not one,
 * napi_delete_reference is (Node.js 22's include/node/js_native_api.h:339,
 * 355). */
#include "datamodel.h"

#include <stdlib.h>
#include <string.h>

/* handle = generation << 32 | (slot + 1) */
static uint64_t ta_napi_handle(uint32_t slot, uint32_t generation) {
    return (uint64_t)generation << 32 | (uint64_t)(slot + 1);
}

static ta_napi_row *ta_napi_slot(const ta_napi_table *table, uint32_t slot) {
    return (ta_napi_row *)(table->rows.pointer + (size_t)slot * table->row_size);
}

uint64_t ta_napi_table_add(ta_napi_table *table) {
    uint32_t slot;
    if (table->count && table->free != TA_NIL) {
        slot = table->free;
        table->free = ta_napi_slot(table, slot)->next_free;
    } else {
        size_t need = ((size_t)table->count + 1) * table->row_size;
        if (need > table->rows.capacity) {
            size_t capacity = table->rows.capacity ? table->rows.capacity : 64 * table->row_size;
            while (capacity < need)
                capacity *= 2;
            char *rows = (char *)realloc(table->rows.pointer, capacity);
            if (!rows) return 0;
            table->rows.pointer = rows;
            table->rows.capacity = capacity;
        }
        if (!table->count) table->free = TA_NIL;
        slot = table->count++;
        ta_napi_slot(table, slot)->generation = 1;
    }
    ta_napi_row *row = ta_napi_slot(table, slot);
    uint32_t generation = row->generation;
    memset(row, 0, table->row_size);
    row->generation = generation;
    row->references = 1;
    row->next_free = TA_NIL;
    return ta_napi_handle(slot, generation);
}

void *ta_napi_table_get(const ta_napi_table *table, uint64_t handle) {
    uint32_t slot = (uint32_t)(handle & 0xFFFFFFFFu);
    if (!slot || slot > table->count) return NULL;
    ta_napi_row *row = ta_napi_slot(table, slot - 1);
    return row->references && row->generation == (uint32_t)(handle >> 32) ? row : NULL;
}

void ta_napi_table_remove(ta_napi_table *table, uint64_t handle) {
    ta_napi_row *row = (ta_napi_row *)ta_napi_table_get(table, handle);
    if (!row) return;
    row->references = 0;
    row->generation++;
    row->next_free = table->free;
    table->free = (uint32_t)((handle & 0xFFFFFFFFu) - 1);
}

void *ta_napi_table_row(const ta_napi_table *table, uint32_t index) {
    if (index >= table->count) return NULL;
    ta_napi_row *row = ta_napi_slot(table, index);
    return row->references ? row : NULL;
}

void ta_napi_table_free(ta_napi_table *table) {
    free(table->rows.pointer);
    table->rows.pointer = NULL;
    table->rows.capacity = 0;
    table->count = 0;
    table->free = TA_NIL;
}

/* ---- the deferred drops -------------------------------------------------------------- */

void ta_napi_defer_drop(ta_napi_drops *drops, napi_ref reference) {
    size_t need = ((size_t)drops->count + 1) * sizeof(napi_ref);
    if (need > drops->items.capacity) {
        size_t capacity = drops->items.capacity ? drops->items.capacity * 2 : 64 * sizeof(napi_ref);
        char *items = (char *)realloc(drops->items.pointer, capacity);
        if (!items) { /* no room to defer it: drop it now */
            napi_delete_reference(drops->env, reference);
            return;
        }
        drops->items.pointer = items;
        drops->items.capacity = capacity;
    }
    ((napi_ref *)drops->items.pointer)[drops->count++] = reference;
}

uint32_t ta_napi_drops_drain(ta_napi_drops *drops) {
    uint32_t dropped = 0;
    while (drops->count) {
        napi_delete_reference(drops->env, ((napi_ref *)drops->items.pointer)[--drops->count]);
        dropped++;
    }
    return dropped;
}

/* With the host: what is still deferred is dropped, and the list goes. */
void ta_napi_drops_free(ta_napi_drops *drops) {
    ta_napi_drops_drain(drops);
    free(drops->items.pointer);
    drops->items.pointer = NULL;
    drops->items.capacity = 0;
}
