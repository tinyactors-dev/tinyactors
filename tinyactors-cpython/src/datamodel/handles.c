/* handles.c — the handle tables (tinyactors-core/docs/bindings.md,
 * section 6, "The binding allocates almost nothing"; section 8): what core
 * cannot hold, a session's globals, a chart's compiled expressions, a
 * payload's value, a <foreach>'s items, is a row of a table, and core
 * holds a handle, a slot and its generation, never a pointer. A handle
 * whose slot was freed, and maybe reused, is refused.
 *
 * Rows are not allocations of their own: a table is one block of rows of
 * one size, grown by doubling, its free slots reused, freed once with the
 * host. This file and the host's create, free and allocator are the only
 * places that allocate (AGENTS.md, "The binding's memory"; the lint rule
 * allocation-in-tables-and-host). A row moves when its table grows: hold
 * its handle, not a pointer to it, across anything that can add a row. */
#include "datamodel/datamodel.h"

static ta_python_row *ta_python_row_at(const ta_python_table *table, uint32_t slot) {
    return (ta_python_row *)((char *)table->rows + (size_t)slot * table->size);
}

void *ta_python_table_add(ta_python_table *table, uint64_t *handle) {
    uint32_t slot;
    if (table->free) {
        slot = table->free - 1;
        table->free = ta_python_row_at(table, slot)->count;
    } else {
        if (table->count == table->capacity) {
            uint32_t capacity = table->capacity ? table->capacity * 2 : 16;
            void *grown = PyMem_RawMalloc((size_t)capacity * table->size);
            if (!grown) return NULL;
            if (table->count) memcpy(grown, table->rows, (size_t)table->count * table->size);
            PyMem_RawFree(table->rows);
            table->rows = grown;
            table->capacity = capacity;
        }
        slot = table->count++;
        ta_python_row_at(table, slot)->generation = 1;
    }
    ta_python_row *row = ta_python_row_at(table, slot);
    uint32_t generation = row->generation;
    memset(row, 0, table->size);
    row->generation = generation;
    row->used = 1;
    row->count = 1;
    *handle = (uint64_t)generation << 32 | (uint64_t)(slot + 1);
    return row;
}

void *ta_python_table_row(const ta_python_table *table, uint64_t handle) {
    uint32_t slot = (uint32_t)(handle & 0xFFFFFFFFu);
    if (!slot || slot > table->count) return NULL;
    ta_python_row *row = ta_python_row_at(table, slot - 1);
    if (!row->used || row->generation != (uint32_t)(handle >> 32)) return NULL;
    return row;
}

void *ta_python_table_slot(const ta_python_table *table, uint32_t slot) {
    if (slot >= table->count) return NULL;
    ta_python_row *row = ta_python_row_at(table, slot);
    return row->used ? row : NULL;
}

void ta_python_table_remove(ta_python_table *table, uint64_t handle) {
    ta_python_row *row = ta_python_table_row(table, handle);
    if (!row) return;
    row->used = 0;
    row->generation++;
    row->count = table->free;
    table->free = (uint32_t)(handle & 0xFFFFFFFFu);
}

void ta_python_table_free(ta_python_table *table) {
    PyMem_RawFree(table->rows);
    table->rows = NULL;
    table->count = table->capacity = table->free = 0;
}
