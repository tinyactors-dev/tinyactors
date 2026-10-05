#include "index_pool.h"
#include "../include/tinyactors.h"
#include "list.h"
#include "memory.h"
#include "vec.h"

/* A page freed by trim is marked with this count while trim unlinks its items. */
#define TA_INDEX_POOL_FREEING UINT32_MAX

static size_t ta_index_pool_page_bytes(const ta_index_pool *pool) {
    return pool->item_size << pool->page_shift;
}

static ta_index_pool_page *ta_index_pool_entry(const ta_index_pool *pool, uint32_t page) {
    return (ta_index_pool_page *)pool->pages.items + page;
}

/* Items page `page` holds below the ceiling: a whole page, except the one the ceiling cuts. */
static uint32_t ta_index_pool_page_items(const ta_index_pool *pool, uint32_t page) {
    uint64_t first = (uint64_t)page << pool->page_shift;
    uint64_t whole = (uint64_t)1u << pool->page_shift;
    uint64_t left = first >= pool->ceiling ? 0u : (uint64_t)pool->ceiling - first;
    return (uint32_t)(left < whole ? left : whole);
}

/* Pages that indices 1 to the ceiling occupy. */
static uint32_t ta_index_pool_page_limit(const ta_index_pool *pool) {
    return (uint32_t)((((uint64_t)pool->ceiling - 1u) >> pool->page_shift) + 1u);
}

/* The first page at or after `page` that refill may allocate: a hole, or
 * any page at or past the end of the directory. */
static uint32_t ta_index_pool_next_vacant(const ta_index_pool *pool, uint32_t page) {
    if (pool->holes != 0) {
        while (page < pool->pages.count && ta_index_pool_entry(pool, page)->items != NULL) {
            page += 1u;
        }
        return page;
    }
    return page > pool->pages.count ? page : pool->pages.count;
}

static void ta_index_pool_free_page(const ta_index_pool *pool, unsigned char *items) {
    ta_mem_free(pool->allocator, items,
                &(ta_allocator_free_params){ .size = ta_index_pool_page_bytes(pool), .alignment = pool->item_alignment });
}

ta_result ta_index_pool_init(ta_index_pool *pool, const ta_index_pool_params *params) {
    size_t alignment = params->item_alignment;
    if (params->item_size == 0 || alignment == 0 || (alignment & (alignment - 1u)) != 0 ||
        params->item_size % alignment != 0 || params->page_shift > 31u ||
        params->item_size > (SIZE_MAX >> params->page_shift) || params->item_size < sizeof(uint32_t) ||
        params->link_offset % sizeof(uint32_t) != 0 || params->link_offset > params->item_size - sizeof(uint32_t) ||
        (params->item_size << params->page_shift) < sizeof(void *) || params->ceiling == 0) {
        return TA_ERR_INVALID;
    }
    *pool = (ta_index_pool){
        .allocator = params->allocator,
        .free_list = TA_LIST_EMPTY,
        .item_size = params->item_size,
        .item_alignment = alignment,
        .link_offset = params->link_offset,
        .page_shift = params->page_shift,
        .page_mask = (uint32_t)((UINT64_C(1) << params->page_shift) - 1u),
        .ceiling = params->ceiling,
    };
    ta_vec_init(&pool->pages, params->allocator, TA_VEC_LAYOUT(ta_index_pool_page));
    return TA_OK;
}

void ta_index_pool_destroy(ta_index_pool *pool) {
    for (uint32_t page = 0; page < pool->pages.count; page += 1u) {
        unsigned char *items = ta_index_pool_entry(pool, page)->items;
        if (items != NULL) {
            ta_index_pool_free_page(pool, items);
        }
    }
    ta_vec_destroy(&pool->pages);
    *pool = (ta_index_pool){0};
}

void *ta_index_pool_item(void *pool, uint32_t index) {
    return ta_index_pool_at(pool, index);
}

/*
 * Refill runs in two passes over the same plan, the vacant pages in
 * ascending order. The first allocates every page the plan needs and
 * chains them through their own first bytes, so nothing in the pool
 * changes until every allocation, the directory's last, has succeeded.
 * The second puts each page in its entry and threads its items.
 */
ta_result ta_index_pool_refill(ta_index_pool *pool, uint32_t min_free) {
    if (pool->free >= min_free) {
        return TA_OK;
    }
    if ((uint64_t)pool->used + min_free > pool->ceiling) {
        return TA_ERR_FULL;
    }
    uint32_t needed = min_free - pool->free;
    size_t page_bytes = ta_index_pool_page_bytes(pool);

    unsigned char *chain = NULL;
    unsigned char **chain_end = &chain;
    uint32_t gained = 0;
    uint32_t end = pool->pages.count; /* the directory's count once the plan is in */
    uint32_t page = ta_index_pool_next_vacant(pool, 0);
    uint32_t limit = ta_index_pool_page_limit(pool);
    ta_result result = TA_OK;
    while (gained < needed && page < limit) {
        unsigned char *items = ta_mem_alloc(pool->allocator, page_bytes, pool->item_alignment);
        if (items == NULL) {
            result = TA_ERR_NOMEM;
            break;
        }
        *(unsigned char **)(void *)items = NULL;
        *chain_end = items;
        chain_end = (unsigned char **)(void *)items;
        gained += ta_index_pool_page_items(pool, page);
        end = page + 1u > end ? page + 1u : end;
        page = ta_index_pool_next_vacant(pool, page + 1u);
    }
    if (result == TA_OK) {
        result = ta_vec_reserve(&pool->pages, end);
    }
    if (result != TA_OK) {
        while (chain != NULL) {
            unsigned char *next = *(unsigned char **)(void *)chain;
            ta_index_pool_free_page(pool, chain);
            chain = next;
        }
        return result;
    }

    ta_list_links links = ta_index_pool_links(pool, pool->link_offset);
    page = ta_index_pool_next_vacant(pool, 0);
    while (chain != NULL) {
        unsigned char *items = chain;
        chain = *(unsigned char **)(void *)items;
        ta_mem_zero(items, page_bytes);
        if (page < pool->pages.count) {
            pool->holes -= 1u;
        } else {
            /* Reserved above, so this cannot fail. */
            (void)ta_vec_push(&pool->pages, &(ta_index_pool_page){0});
        }
        *ta_index_pool_entry(pool, page) = (ta_index_pool_page){ .items = items, .used = 0 };
        uint32_t count = ta_index_pool_page_items(pool, page);
        uint32_t first = (page << pool->page_shift) + 1u;
        for (uint32_t item = 0; item < count; item += 1u) {
            ta_list_push_back(&pool->free_list, &links, first + item);
        }
        pool->free += count;
        page = ta_index_pool_next_vacant(pool, page + 1u);
    }
    return TA_OK;
}

ta_result ta_index_pool_take(ta_index_pool *pool, uint32_t *out) {
    ta_list_links links = ta_index_pool_links(pool, pool->link_offset);
    uint32_t index = ta_list_pop_front(&pool->free_list, &links);
    if (index == 0u) {
        return TA_ERR_FULL;
    }
    ta_index_pool_entry(pool, (index - 1u) >> pool->page_shift)->used += 1u;
    pool->used += 1u;
    pool->free -= 1u;
    if (index > pool->high_water) {
        pool->high_water = index;
    }
    *out = index;
    return TA_OK;
}

void ta_index_pool_give(ta_index_pool *pool, uint32_t index) {
    ta_list_links links = ta_index_pool_links(pool, pool->link_offset);
    ta_list_push_front(&pool->free_list, &links, index);
    ta_index_pool_entry(pool, (index - 1u) >> pool->page_shift)->used -= 1u;
    pool->used -= 1u;
    pool->free += 1u;
}

void ta_index_pool_trim(ta_index_pool *pool, uint32_t keep) {
    /* Mark the pages to free, from the last down, while `keep` items stay free. */
    uint32_t free = pool->free;
    uint32_t marked = 0;
    for (uint32_t page = pool->pages.count; page > 0; page -= 1u) {
        ta_index_pool_page *entry = ta_index_pool_entry(pool, page - 1u);
        uint32_t count = ta_index_pool_page_items(pool, page - 1u);
        if (entry->items != NULL && entry->used == 0 && free >= count && free - count >= keep) {
            entry->used = TA_INDEX_POOL_FREEING;
            free -= count;
            marked += 1u;
        }
    }
    if (marked == 0) {
        return;
    }

    /* Take the marked pages' items off the free list, in one walk. */
    ta_list_links links = ta_index_pool_links(pool, pool->link_offset);
    uint32_t previous = 0;
    uint32_t current = ta_list_head(&pool->free_list);
    while (current != 0) {
        uint32_t next = ta_list_next(&links, current);
        if (ta_index_pool_entry(pool, (current - 1u) >> pool->page_shift)->used == TA_INDEX_POOL_FREEING) {
            (void)ta_list_remove_after(&pool->free_list, &links, previous);
        } else {
            previous = current;
        }
        current = next;
    }
    pool->free = free;

    /* Free them; the directory keeps a hole below the last page kept. */
    for (uint32_t page = 0; page < pool->pages.count; page += 1u) {
        ta_index_pool_page *entry = ta_index_pool_entry(pool, page);
        if (entry->items != NULL && entry->used == TA_INDEX_POOL_FREEING) {
            ta_index_pool_free_page(pool, entry->items);
            *entry = (ta_index_pool_page){0};
            pool->holes += 1u;
        }
    }
    while (pool->pages.count > 0 && ta_index_pool_entry(pool, pool->pages.count - 1u)->items == NULL) {
        pool->pages.count -= 1u;
        pool->holes -= 1u;
    }
    if (pool->pages.count == 0) {
        ta_vec_destroy(&pool->pages);
    }
}
