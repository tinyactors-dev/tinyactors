/* handles.c — slot tables (tinyactors-core/docs/bindings.md, section 8):
 * every Python reference the datamodel holds for core is in a slot, and
 * core holds a handle, a slot and its generation, never a pointer. A handle
 * whose slot was freed, and maybe reused, is refused. */
#include "datamodel/datamodel.h"

static uint64_t ta_python_handle_of(const ta_python_handles *table, uint32_t slot) {
    return (uint64_t)table->slots[slot].generation << 32 | (uint64_t)(slot + 1);
}

uint64_t ta_python_handle_add(ta_python_handles *table, void *item) {
    uint32_t slot;
    if (table->free) {
        slot = table->free - 1;
        table->free = table->slots[slot].count;
    } else {
        if (table->count == table->capacity) {
            uint32_t capacity = table->capacity ? table->capacity * 2 : 16;
            ta_python_slot *grown = PyMem_RawMalloc(capacity * sizeof *grown);
            if (!grown) return 0;
            if (table->count) memcpy(grown, table->slots, table->count * sizeof *grown);
            PyMem_RawFree(table->slots);
            table->slots = grown;
            table->capacity = capacity;
        }
        slot = table->count++;
        table->slots[slot].generation = 1;
    }
    table->slots[slot].item = item;
    table->slots[slot].count = 1;
    return ta_python_handle_of(table, slot);
}

ta_python_slot *ta_python_handle_slot(const ta_python_handles *table, uint64_t handle) {
    uint32_t slot = (uint32_t)(handle & 0xFFFFFFFFu);
    if (!slot || slot > table->count) return NULL;
    ta_python_slot *entry = &table->slots[slot - 1];
    if (!entry->item || entry->generation != (uint32_t)(handle >> 32)) return NULL;
    return entry;
}

void *ta_python_handle_remove(ta_python_handles *table, uint64_t handle) {
    ta_python_slot *entry = ta_python_handle_slot(table, handle);
    if (!entry) return NULL;
    void *item = entry->item;
    entry->item = NULL;
    entry->generation++;
    entry->count = table->free;
    table->free = (uint32_t)(handle & 0xFFFFFFFFu);
    return item;
}

void ta_python_handles_free(ta_python_handles *table) {
    PyMem_RawFree(table->slots);
    memset(table, 0, sizeof *table);
}
