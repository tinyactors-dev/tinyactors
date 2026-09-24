#include "io_origin.h"

#define TA_IO_ORIGIN_FREE UINT32_MAX

static ta_allocator_free_params ta_io_origin_page_layout(void) {
    return (ta_allocator_free_params){ .size = (size_t)TA_IO_ORIGIN_PAGE * sizeof(ta_io_origin_entry),
                                       .alignment = _Alignof(ta_io_origin_entry) };
}

void ta_io_origin_table_init(ta_io_origin_table *table, ta_allocator allocator, bool scoped) {
    *table = (ta_io_origin_table){ .allocator = allocator, .scoped = scoped };
    ta_vec_init(&table->pages, allocator, TA_VEC_LAYOUT(ta_io_origin_entry *));
}

static void ta_io_origin_free_spill(ta_io_origin_entry *entry) {
    if (entry->length > TA_IO_ORIGIN_INLINE && entry->spill != NULL) {
        ta_mem_free(entry->table->allocator, entry->spill,
                    &(ta_allocator_free_params){ .size = entry->length, .alignment = 1 });
    }
    entry->spill = NULL;
}

/*
 * Frees every page and every spill still held. Live records at this point
 * belong to events a host has not released yet; like addresses (ADR 0065),
 * a host releases every event before destroying the system.
 */
void ta_io_origin_table_destroy(ta_io_origin_table *table) {
    for (uint32_t page = 0; page < table->pages.count; page += 1) {
        ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, page);
        uint32_t used = table->issued - page * TA_IO_ORIGIN_PAGE;
        used = used > TA_IO_ORIGIN_PAGE ? TA_IO_ORIGIN_PAGE : used;
        for (uint32_t index = 0; index < used; index += 1) {
            if (records[index].length != TA_IO_ORIGIN_FREE) {
                ta_io_origin_free_spill(&records[index]);
            }
        }
        ta_allocator_free_params layout = ta_io_origin_page_layout();
        ta_mem_free(table->allocator, records, &layout);
    }
    ta_vec_destroy(&table->pages);
    *table = (ta_io_origin_table){0};
}

ta_io_origin_entry *ta_io_origin_at(const ta_io_origin_table *table, uint32_t index) {
    if (index >= table->issued) {
        return NULL;
    }
    ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, index / TA_IO_ORIGIN_PAGE);
    ta_io_origin_entry *entry = &records[index % TA_IO_ORIGIN_PAGE];
    return entry->length == TA_IO_ORIGIN_FREE ? NULL : entry;
}

/* A record off the free list, or the next unissued one, with its index. */
static ta_io_origin_entry *ta_io_origin_slot(ta_io_origin_table *table, uint32_t *index) {
    if (table->free != NULL) {
        ta_io_origin_entry *entry = table->free;
        table->free = (ta_io_origin_entry *)(void *)entry->spill;
        entry->spill = NULL;
        *index = entry->refs;
        return entry;
    }
    if (table->issued % TA_IO_ORIGIN_PAGE == 0) {
        ta_allocator_free_params layout = ta_io_origin_page_layout();
        ta_io_origin_entry *page = ta_mem_alloc(table->allocator, layout.size, layout.alignment);
        if (page == NULL) {
            return NULL;
        }
        if (ta_vec_push(&table->pages, &page) != TA_OK) {
            ta_mem_free(table->allocator, page, &layout);
            return NULL;
        }
    }
    *index = table->issued;
    ta_io_origin_entry *records = *(ta_io_origin_entry **)ta_vec_at(&table->pages, *index / TA_IO_ORIGIN_PAGE);
    table->issued += 1;
    ta_io_origin_entry *entry = &records[*index % TA_IO_ORIGIN_PAGE];
    *entry = (ta_io_origin_entry){ .generation = 1, .table = table, .length = TA_IO_ORIGIN_FREE };
    return entry;
}

/* Puts a record on the free list; a free record keeps its index in `refs`. */
static void ta_io_origin_park(ta_io_origin_table *table, ta_io_origin_entry *entry, uint32_t index) {
    entry->refs = index;
    entry->length = TA_IO_ORIGIN_FREE;
    entry->spill = (char *)(void *)table->free;
    table->free = entry;
}

ta_io_origin_entry *ta_io_origin_take(ta_io_origin_table *table, const ta_io_origin_take_params *params) {
    uint32_t index = 0;
    ta_io_origin_entry *entry = ta_io_origin_slot(table, &index);
    if (entry == NULL) {
        return NULL;
    }
    ta_str uri = params->uri;
    char *spill = NULL;
    if (uri.len > TA_IO_ORIGIN_INLINE) {
        spill = ta_mem_alloc(table->allocator, uri.len, 1);
        if (spill == NULL) {
            ta_io_origin_park(table, entry, index);
            return NULL;
        }
        ta_mem_copy(spill, uri.ptr, uri.len);
    } else if (uri.len != 0) {
        ta_mem_copy(entry->text, uri.ptr, uri.len);
    }
    entry->refs = table->scoped ? index : 1u;
    entry->table = table;
    entry->processor = params->processor;
    entry->length = uri.len;
    entry->spill = spill;
    table->live += 1;
    if (params->index != NULL) {
        *params->index = index;
    }
    return entry;
}

void ta_io_origin_give(ta_io_origin_entry *entry) {
    if (entry == NULL || entry->table == NULL) {
        return;
    }
    ta_io_origin_table *table = entry->table;
    ta_io_origin_free_spill(entry);
    entry->generation += 1;
    table->live -= 1;
    /* Only scopes are found by index; an entry's index is never read. */
    ta_io_origin_park(table, entry, table->scoped ? entry->refs : 0u);
}

void ta_io_origin_entry_retain(ta_io_origin_entry *entry) {
    if (entry != NULL && entry->table != NULL) {
        entry->refs += 1;
    }
}

void ta_io_origin_entry_release(ta_io_origin_entry *entry) {
    if (entry == NULL || entry->table == NULL) {
        return;
    }
    entry->refs -= 1;
    if (entry->refs == 0) {
        ta_io_origin_give(entry);
    }
}

ta_str ta_io_origin_entry_uri(const ta_io_origin_entry *entry) {
    if (entry == NULL || entry->length == TA_IO_ORIGIN_FREE) {
        return (ta_str){0};
    }
    if (entry->table == NULL || entry->length > TA_IO_ORIGIN_INLINE) {
        return (ta_str){ .ptr = entry->spill, .len = entry->length };
    }
    return (ta_str){ .ptr = entry->text, .len = entry->length };
}

ta_result ta_io_origin_detach(ta_arena *arena, const ta_io_origin_detach_params *params) {
    ta_io_origin_entry *entry = ta_arena_alloc(arena, sizeof(ta_io_origin_entry), _Alignof(ta_io_origin_entry));
    if (entry == NULL) {
        return TA_ERR_NOMEM;
    }
    ta_str type;
    ta_str uri;
    ta_result result = ta_arena_copy_str(arena, params->type, &type);
    if (result == TA_OK) {
        result = ta_arena_copy_str(arena, params->uri, &uri);
    }
    if (result != TA_OK) {
        return result;
    }
    *entry = (ta_io_origin_entry){ .spill = (char *)uri.ptr, .length = uri.len };
    ta_mem_copy(entry->text, &type, sizeof(type));
    *params->out = entry;
    return TA_OK;
}

bool ta_io_origin_entry_detached(const ta_io_origin_entry *entry) {
    return entry != NULL && entry->table == NULL;
}

ta_str ta_io_origin_detached_type(const ta_io_origin_entry *entry) {
    ta_str type;
    ta_mem_copy(&type, entry->text, sizeof(type));
    return type;
}
