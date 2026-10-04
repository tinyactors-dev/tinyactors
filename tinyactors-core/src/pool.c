#include "pool.h"

/*
 * The first directory has this many entries and doubles from there, as a
 * ta_vec does: a small pool pays for eight 16-byte entries, not for a page
 * table sized for four billion items.
 */
#define TA_POOL_FIRST_DIRECTORY 8u

/* The address of item `index` (1-based) without the extent check; the caller knows it is in a page. */
static unsigned char *ta_pool_item(const ta_pool *pool, uint32_t index) {
    uint32_t position = index - 1u;
    return (unsigned char *)pool->directory[position >> pool->page_shift].base +
           (size_t)(position & pool->page_mask) * pool->item_size;
}

/*
 * The free-list link of an item. Items are aligned to at least four bytes
 * and sized in multiples of their alignment, and link_offset is a multiple
 * of four (all checked by ta_pool_init), so the access is aligned.
 */
static uint32_t *ta_pool_link(const ta_pool *pool, unsigned char *item) {
    return (uint32_t *)(void *)(item + pool->link_offset);
}

static void ta_pool_free_page(const ta_pool *pool, void *base) {
    ta_mem_free(pool->allocator, base,
                &(ta_allocator_free_params){ .size = pool->page_bytes, .alignment = pool->page_alignment });
}

static void ta_pool_free_directory(const ta_pool *pool, ta_pool_page *directory, size_t bytes) {
    ta_mem_free(pool->allocator, directory,
                &(ta_allocator_free_params){ .size = bytes, .alignment = _Alignof(ta_pool_page) });
}

/* ta_pool_init: see pool.h. Allocates nothing; the pool is zero unless it answers TA_OK. */
ta_result ta_pool_init(ta_pool *pool, ta_allocator allocator, const ta_pool_params *params) {
    if (pool == NULL) {
        return TA_ERR_INVALID;
    }
    ta_mem_zero(pool, sizeof *pool);
    if (params == NULL || params->item_size < sizeof(uint32_t) || params->item_alignment < sizeof(uint32_t) ||
        (params->item_alignment & (params->item_alignment - 1u)) != 0 ||
        params->item_size % params->item_alignment != 0 || params->link_offset % sizeof(uint32_t) != 0 ||
        params->link_offset > params->item_size - sizeof(uint32_t) || params->page_items == 0 ||
        (params->page_items & (params->page_items - 1u)) != 0) {
        return TA_ERR_INVALID;
    }
    /* Whole pages only, and never more items than a 32-bit index can name. */
    uint32_t max_pages = UINT32_MAX / params->page_items;
    if (params->max_items != 0 && params->max_items / params->page_items < max_pages) {
        max_pages = params->max_items / params->page_items;
    }
    if (max_pages == 0) {
        return TA_ERR_INVALID;
    }
    ta_layout layout;
    ta_layout_init(&layout, 0);
    ta_layout_add(&layout, &(ta_layout_entry){ .size = params->item_size,
                                               .alignment = params->item_alignment,
                                               .count = params->page_items });
    size_t page_bytes;
    size_t page_alignment;
    ta_result result = ta_layout_finish(&layout, &page_bytes, &page_alignment);
    if (result != TA_OK) {
        return result;
    }
    uint32_t shift = 0;
    while (((uint32_t)1u << shift) != params->page_items) {
        shift += 1u;
    }
    pool->allocator = allocator;
    pool->page_bytes = page_bytes;
    pool->page_alignment = page_alignment;
    pool->item_size = params->item_size;
    pool->link_offset = params->link_offset;
    pool->fresh = params->fresh;
    pool->page_shift = shift;
    pool->page_mask = params->page_items - 1u;
    pool->max_capacity = max_pages * params->page_items;
    return TA_OK;
}

/* ta_pool_destroy: see pool.h. Sized frees come from the sizes init and refill stored. */
void ta_pool_destroy(ta_pool *pool) {
    if (pool == NULL) {
        return;
    }
    for (uint32_t page = 0; page < pool->pages; page += 1u) {
        ta_pool_free_page(pool, pool->directory[page].base);
    }
    if (pool->directory != NULL) {
        ta_pool_free_directory(pool, pool->directory, pool->directory_bytes);
    }
    ta_mem_zero(pool, sizeof *pool);
}

/*
 * ta_pool_refill: see pool.h. The order is what makes it all or nothing: the
 * new directory (if any) first, then the pages, every one recorded in the
 * directory that will be kept, and only when all of them exist does the
 * pool's own state change (the commit at the end cannot fail).
 */
ta_result ta_pool_refill(ta_pool *pool, uint32_t min_free) {
    uint32_t free_items = pool->capacity - pool->used;
    if (min_free <= free_items) {
        return TA_OK;
    }
    /* 64-bit: pages * page_items stays under 2^34 whatever the page size, so none of this wraps. */
    uint64_t page_items = (uint64_t)pool->page_mask + 1u;
    uint64_t added = ((uint64_t)(min_free - free_items) + page_items - 1u) / page_items;
    uint64_t pages = pool->pages + added;
    if (pages * page_items > pool->max_capacity) {
        return TA_ERR_NOMEM;
    }
    ta_pool_page *directory = pool->directory;
    size_t directory_bytes = pool->directory_bytes;
    uint64_t directory_capacity = pool->directory_capacity;
    bool grown = pages > directory_capacity;
    if (grown) {
        directory_capacity = directory_capacity == 0 ? TA_POOL_FIRST_DIRECTORY : directory_capacity;
        while (directory_capacity < pages) {
            directory_capacity *= 2u;
        }
        if (directory_capacity > pool->max_capacity / page_items) {
            directory_capacity = pool->max_capacity / page_items; /* never more entries than the ceiling has pages */
        }
        if (directory_capacity > SIZE_MAX) {
            return TA_ERR_NOMEM; /* a 32-bit size_t cannot count them */
        }
        ta_layout layout;
        ta_layout_init(&layout, 0);
        ta_layout_add(&layout, &(ta_layout_entry){ .size = sizeof(ta_pool_page),
                                                   .alignment = _Alignof(ta_pool_page),
                                                   .count = (size_t)directory_capacity });
        ta_result result = ta_layout_finish(&layout, &directory_bytes, NULL);
        if (result != TA_OK) {
            return result;
        }
        directory = ta_mem_alloc(pool->allocator, directory_bytes, _Alignof(ta_pool_page));
        if (directory == NULL) {
            return TA_ERR_NOMEM;
        }
        ta_mem_copy(directory, pool->directory, (size_t)pool->pages * sizeof(ta_pool_page));
    }
    for (uint64_t page = pool->pages; page < pages; page += 1u) {
        void *base = ta_mem_alloc(pool->allocator, pool->page_bytes, pool->page_alignment);
        if (base == NULL) {
            /* Give back this call's pages (not the ones before it), then the new directory. */
            while (page > pool->pages) {
                page -= 1u;
                ta_pool_free_page(pool, directory[page].base);
            }
            if (grown) {
                ta_pool_free_directory(pool, directory, directory_bytes);
            }
            return TA_ERR_NOMEM;
        }
        directory[page] = (ta_pool_page){ .base = base, .used = 0 };
    }
    /* Commit: nothing below allocates or fails. */
    if (grown) {
        if (pool->directory != NULL) {
            ta_pool_free_directory(pool, pool->directory, pool->directory_bytes);
        }
        pool->directory = directory;
        pool->directory_bytes = directory_bytes;
        pool->directory_capacity = (uint32_t)directory_capacity;
    }
    pool->pages = (uint32_t)pages;
    pool->capacity = (uint32_t)(pages * page_items);
    return TA_OK;
}

/* ta_pool_take: see pool.h. The free list first (LIFO), else the cursor, else 0. */
uint32_t ta_pool_take(ta_pool *pool) {
    uint32_t index = pool->free_head;
    unsigned char *item;
    if (index != 0) {
        item = ta_pool_item(pool, index);
        pool->free_head = *ta_pool_link(pool, item);
    } else if (pool->extent < pool->capacity) {
        pool->extent += 1u;
        index = pool->extent;
        item = ta_pool_item(pool, index);
        if (pool->fresh != NULL) {
            ta_mem_copy(item, pool->fresh, pool->item_size);
        } else {
            ta_mem_zero(item, pool->item_size);
        }
    } else {
        return 0;
    }
    *ta_pool_link(pool, item) = 0;
    pool->directory[(index - 1u) >> pool->page_shift].used += 1u;
    pool->used += 1u;
    if (pool->used > pool->high_water) {
        pool->high_water = pool->used;
    }
    return index;
}

/* ta_pool_give: see pool.h. Writes the four link bytes and nothing else of the item. */
void ta_pool_give(ta_pool *pool, uint32_t index) {
    *ta_pool_link(pool, ta_pool_item(pool, index)) = pool->free_head;
    pool->free_head = index;
    pool->directory[(index - 1u) >> pool->page_shift].used -= 1u;
    pool->used -= 1u;
}

/* ta_pool_at: see pool.h. */
void *ta_pool_at(const ta_pool *pool, uint32_t index) {
    if (index == 0 || index > pool->extent) {
        return NULL;
    }
    return ta_pool_item(pool, index);
}

/* ta_pool_extent: see pool.h. */
uint32_t ta_pool_extent(const ta_pool *pool) {
    return pool->extent;
}

/* ta_pool_counts_get: see pool.h. */
void ta_pool_counts_get(const ta_pool *pool, ta_pool_counts *out) {
    *out = (ta_pool_counts){ .used = pool->used,
                             .free = pool->capacity - pool->used,
                             .high_water = pool->high_water,
                             .capacity = pool->capacity,
                             .extent = pool->extent,
                             .pages = pool->pages };
}

/*
 * ta_pool_trim: see pool.h. The free list is unlinked before any page is
 * freed, because unlinking reads the links of items in the pages that are
 * about to go.
 */
uint32_t ta_pool_trim(ta_pool *pool) {
    uint32_t kept = pool->pages;
    while (kept > 0 && pool->directory[kept - 1u].used == 0) {
        kept -= 1u;
    }
    uint32_t freed = pool->pages - kept;
    if (freed == 0) {
        return 0;
    }
    uint32_t capacity = kept << pool->page_shift;
    if (pool->extent > capacity) {
        uint32_t previous = 0;
        uint32_t index = pool->free_head;
        while (index != 0) {
            unsigned char *item = ta_pool_item(pool, index);
            uint32_t next = *ta_pool_link(pool, item);
            if (index > capacity) {
                if (previous == 0) {
                    pool->free_head = next;
                } else {
                    *ta_pool_link(pool, ta_pool_item(pool, previous)) = next;
                }
            } else {
                previous = index;
            }
            index = next;
        }
        pool->extent = capacity;
    }
    for (uint32_t page = kept; page < pool->pages; page += 1u) {
        ta_pool_free_page(pool, pool->directory[page].base);
    }
    pool->pages = kept;
    pool->capacity = capacity;
    return freed;
}
