/* test: standalone asan */
/*
 * Block A5 (src/pool.h): the index pool.
 *
 * Covers the contract of each function (take and give order, the fresh
 * template and the fields a give leaves alone, refill growth and what it
 * allocates, the ceiling, trim), the property that matters most to the
 * tables built on the pool (an item never moves, so a pointer to a taken
 * item stays valid across any number of refills), and the failure path: a
 * refill is swept over every allocation it makes, from a pool with no
 * directory and from one whose directory is full, and each failure must
 * leave the counters, the free list and the heap exactly as they were. A
 * short seeded model test checks the counters against a bitmap. It is fast,
 * not exhaustive: about twenty thousand steps over a few dozen items.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap = { .strict = true };

/* A 16-byte record shaped like an actor slot: the link is `next`, the generation must survive give and take. */
typedef struct {
    uint64_t payload;
    uint32_t generation;
    uint32_t next;
} record;

static const record fresh_record = { .payload = 0, .generation = 1, .next = 0 };

#define PAGE_ITEMS 8u
#define PAGE_BYTES (PAGE_ITEMS * sizeof(record))

static ta_pool_params record_params(uint32_t max_items) {
    return (ta_pool_params){ .item_size = sizeof(record),
                             .item_alignment = _Alignof(record),
                             .link_offset = offsetof(record, next),
                             .page_items = PAGE_ITEMS,
                             .max_items = max_items,
                             .fresh = &fresh_record };
}

static void record_init(ta_pool *pool, uint32_t max_items) {
    ta_pool_params params = record_params(max_items);
    CHECK(ta_pool_init(pool, ta_test_allocator_get(&heap), &params) == TA_OK);
}

static bool pool_is_zero(const ta_pool *pool) {
    const unsigned char *bytes = (const unsigned char *)pool;
    for (size_t index = 0; index < sizeof *pool; index += 1u) {
        if (bytes[index] != 0) {
            return false;
        }
    }
    return true;
}

static void expect_counts(const ta_pool *pool, uint32_t used, uint32_t free_items, uint32_t high_water,
                          uint32_t capacity, uint32_t extent, uint32_t pages) {
    ta_pool_counts counts;
    ta_pool_counts_get(pool, &counts);
    CHECK(counts.used == used);
    CHECK(counts.free == free_items);
    CHECK(counts.high_water == high_water);
    CHECK(counts.capacity == capacity);
    CHECK(counts.extent == extent);
    CHECK(counts.pages == pages);
    CHECK(ta_pool_extent(pool) == extent);
}

static bool same_counts(const ta_pool_counts *left, const ta_pool_counts *right) {
    return left->used == right->used && left->free == right->free && left->high_water == right->high_water &&
           left->capacity == right->capacity && left->extent == right->extent && left->pages == right->pages;
}

static void finish_clean(void) {
    CHECK(heap.stats.bad_frees == 0);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.live_bytes == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_layout_probes(void) {
    /* Per-actor bytes must not grow: the pool's link is a field the slots already had. */
    CHECK(sizeof(ta_actor_slot) == 16u);
    CHECK(sizeof(ta_definition_slot) == 16u);
    CHECK(sizeof(record) == 16u);
}

static void test_init_validation(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    ta_pool_params good = record_params(0);
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &good) == TA_OK);
    CHECK(!pool_is_zero(&pool));
    CHECK(ta_pool_extent(&pool) == 0 && ta_pool_at(&pool, 1) == NULL);
    ta_pool_destroy(&pool);
    CHECK(pool_is_zero(&pool));
    ta_pool_destroy(&pool); /* idempotent */

    ta_pool_params bad[10];
    for (size_t index = 0; index < 10u; index += 1u) {
        bad[index] = good;
    }
    bad[0].item_size = 2u;
    bad[1].link_offset = 2u;                       /* not a multiple of 4 */
    bad[2].link_offset = sizeof(record);           /* link does not fit */
    bad[3].page_items = 0;
    bad[4].page_items = 6u;                        /* not a power of two */
    bad[5].item_alignment = 0;
    bad[6].item_alignment = 12u;                   /* not a power of two */
    bad[7].item_size = 20u;                        /* not a multiple of the alignment */
    bad[8].max_items = PAGE_ITEMS - 1u;            /* under one page */
    bad[9].item_alignment = 2u;                    /* too small for an aligned link */
    for (size_t index = 0; index < 10u; index += 1u) {
        ta_mem_zero(&pool, sizeof pool);
        pool.used = 77u; /* init must reset the whole pool, even on failure */
        CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &bad[index]) == TA_ERR_INVALID);
        CHECK(pool_is_zero(&pool));
    }
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), NULL) == TA_ERR_INVALID);
    CHECK(ta_pool_init(NULL, ta_test_allocator_get(&heap), &good) == TA_ERR_INVALID);

    /* item_size * page_items does not fit a size_t. */
    ta_pool_params huge = good;
    huge.page_items = (uint32_t)1u << 31;
    huge.item_size = sizeof(size_t) == 8u ? ((size_t)1u << 33) : ((size_t)1u << 30);
    huge.link_offset = 0;
    huge.fresh = NULL;
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &huge) == TA_ERR_NOMEM);
    CHECK(pool_is_zero(&pool));
    CHECK(heap.stats.attempts == 0);
    finish_clean();
}

static void test_take_give_order(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    record_init(&pool, 0);
    CHECK(ta_pool_take(&pool) == 0); /* empty: nothing to serve, nothing allocated */
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    expect_counts(&pool, 0, PAGE_ITEMS, 0, PAGE_ITEMS, 0, 1);
    uint64_t allocations = heap.stats.allocations;
    CHECK(allocations == 2u); /* the directory and one page */

    /* Fresh items come in ascending order, initialised from the template. */
    for (uint32_t index = 1; index <= PAGE_ITEMS; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
        record *item = ta_pool_at(&pool, index);
        CHECK(item != NULL && item->generation == 1u && item->payload == 0 && item->next == 0);
        item->payload = 1000u + index;
    }
    expect_counts(&pool, PAGE_ITEMS, 0, PAGE_ITEMS, PAGE_ITEMS, PAGE_ITEMS, 1);
    CHECK(ta_pool_take(&pool) == 0); /* full: answers 0 and does not allocate */
    CHECK(heap.stats.allocations == allocations);

    /* A caller's own field survives a give and a take; only the link bytes are written. */
    ((record *)ta_pool_at(&pool, 3))->generation = 41u;
    ta_pool_give(&pool, 3);
    ta_pool_give(&pool, 1);
    ta_pool_give(&pool, 2);
    expect_counts(&pool, PAGE_ITEMS - 3u, 3, PAGE_ITEMS, PAGE_ITEMS, PAGE_ITEMS, 1);
    CHECK(((record *)ta_pool_at(&pool, 3))->generation == 41u);
    CHECK(((record *)ta_pool_at(&pool, 3))->payload == 1003u);
    CHECK(ta_pool_take(&pool) == 2u); /* LIFO: the last given is the first taken */
    CHECK(ta_pool_take(&pool) == 1u);
    CHECK(ta_pool_take(&pool) == 3u);
    CHECK(((record *)ta_pool_at(&pool, 3))->generation == 41u);
    CHECK(((record *)ta_pool_at(&pool, 3))->next == 0); /* a taken item's link is zero */
    CHECK(ta_pool_take(&pool) == 0);

    /* The hot loop does not allocate. */
    for (uint32_t round = 0; round < 100u; round += 1u) {
        ta_pool_give(&pool, 5);
        CHECK(ta_pool_take(&pool) == 5u);
    }
    CHECK(heap.stats.allocations == allocations);
    CHECK(ta_pool_at(&pool, 0) == NULL);
    CHECK(ta_pool_at(&pool, ta_pool_extent(&pool) + 1u) == NULL);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_zero_fill(void) {
    ta_pool pool;
    ta_pool_params params = record_params(0);
    params.fresh = NULL;
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &params) == TA_OK);
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    uint32_t index = ta_pool_take(&pool);
    const record *item = ta_pool_at(&pool, index);
    CHECK(index == 1u && item->generation == 0 && item->payload == 0 && item->next == 0);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_refill_growth(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    record_init(&pool, 0);
    CHECK(ta_pool_refill(&pool, 0) == TA_OK);
    CHECK(heap.stats.attempts == 0);
    expect_counts(&pool, 0, 0, 0, 0, 0, 0);

    CHECK(ta_pool_refill(&pool, 1) == TA_OK); /* directory + 1 page */
    CHECK(heap.stats.allocations == 2u);
    expect_counts(&pool, 0, 8, 0, 8, 0, 1);
    CHECK(ta_pool_refill(&pool, 8) == TA_OK); /* already enough */
    CHECK(heap.stats.allocations == 2u);
    CHECK(ta_pool_refill(&pool, 9) == TA_OK); /* one more page, the directory has room */
    CHECK(heap.stats.allocations == 3u);
    expect_counts(&pool, 0, 16, 0, 16, 0, 2);

    /* 72 items need 9 pages: seven new pages and the directory doubles from 8 to 16 entries. */
    CHECK(ta_pool_refill(&pool, 72) == TA_OK);
    CHECK(heap.stats.allocations == 3u + 7u + 1u);
    expect_counts(&pool, 0, 72, 0, 72, 0, 9);

    /* Taking moves used, free, high water and the extent exactly. */
    for (uint32_t index = 1; index <= 20u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
    }
    expect_counts(&pool, 20, 52, 20, 72, 20, 9);
    for (uint32_t index = 1; index <= 20u; index += 1u) {
        ta_pool_give(&pool, index);
    }
    expect_counts(&pool, 0, 72, 20, 72, 20, 9);
    CHECK(ta_pool_refill(&pool, 72) == TA_OK && heap.stats.allocations == 11u);
    /* free counts given items too: it is what take can serve before a refill. */
    CHECK(ta_pool_refill(&pool, 73) == TA_OK && heap.stats.allocations == 12u);
    expect_counts(&pool, 0, 80, 20, 80, 20, 10);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_addresses_never_move(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    record_init(&pool, 0);
    enum { PAGES = 40, COUNT = PAGES * PAGE_ITEMS };
    static record *address[COUNT + 1];
    uint32_t taken = 0;
    for (uint32_t page = 0; page < PAGES; page += 1u) {
        CHECK(ta_pool_refill(&pool, 1) == TA_OK); /* one page at a time: the directory doubles twice */
        for (uint32_t item = 0; item < PAGE_ITEMS; item += 1u) {
            uint32_t index = ta_pool_take(&pool);
            CHECK(index == taken + 1u);
            taken += 1u;
            address[index] = ta_pool_at(&pool, index);
            address[index]->payload = index * 31u;
        }
        for (uint32_t index = 1; index <= taken; index += 1u) {
            CHECK(ta_pool_at(&pool, index) == address[index]);
            CHECK(address[index]->payload == index * 31u);
        }
    }
    CHECK(ta_pool_take(&pool) == 0);
    CHECK(ta_pool_at(&pool, COUNT + 1u) == NULL);
    expect_counts(&pool, COUNT, 0, COUNT, COUNT, COUNT, PAGES);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_ceiling(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    record_init(&pool, 3u * PAGE_ITEMS + 5u); /* rounds down to three pages */
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    CHECK(ta_pool_refill(&pool, 2u * PAGE_ITEMS) == TA_OK);
    CHECK(ta_pool_refill(&pool, 3u * PAGE_ITEMS) == TA_OK);
    expect_counts(&pool, 0, 24, 0, 24, 0, 3);
    ta_test_allocator_reset_counts(&heap);
    ta_pool_counts before;
    ta_pool_counts_get(&pool, &before);
    CHECK(ta_pool_refill(&pool, 3u * PAGE_ITEMS + 1u) == TA_ERR_NOMEM);
    CHECK(ta_pool_refill(&pool, UINT32_MAX) == TA_ERR_NOMEM);
    ta_pool_counts after;
    ta_pool_counts_get(&pool, &after);
    CHECK(same_counts(&before, &after));
    CHECK(heap.stats.attempts == 0); /* refused before any allocation was tried */
    for (uint32_t index = 1; index <= 24u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
    }
    CHECK(ta_pool_take(&pool) == 0);
    /* Items given back are served again at the ceiling. */
    ta_pool_give(&pool, 7);
    CHECK(ta_pool_take(&pool) == 7u);
    CHECK(ta_pool_refill(&pool, 1) == TA_ERR_NOMEM);
    ta_pool_destroy(&pool);
    finish_clean();

    /* A directory is never bigger than the ceiling has pages, and a refill past the largest index fails. */
    record_init(&pool, 2u * PAGE_ITEMS);
    CHECK(ta_pool_refill(&pool, 2u * PAGE_ITEMS) == TA_OK);
    ta_pool_destroy(&pool);
    record_init(&pool, 0);
    CHECK(ta_pool_refill(&pool, UINT32_MAX) == TA_ERR_NOMEM); /* 2^29 pages would pass UINT32_MAX items */
    CHECK(pool.pages == 0 && pool.directory == NULL);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_trim(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_pool pool;
    record_init(&pool, 0);
    CHECK(ta_pool_trim(&pool) == 0); /* an empty pool has nothing to give back */

    /* Everything free: every page goes. */
    CHECK(ta_pool_refill(&pool, 40) == TA_OK);
    for (uint32_t index = 1; index <= 40u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
    }
    for (uint32_t index = 1; index <= 40u; index += 1u) {
        ta_pool_give(&pool, index);
    }
    size_t live_before = heap.stats.live_bytes;
    ta_test_allocator_reset_counts(&heap);
    CHECK(ta_pool_trim(&pool) == 5u);
    CHECK(heap.stats.attempts == 0 && heap.stats.frees == 5u);
    CHECK(live_before - heap.stats.live_bytes == 5u * PAGE_BYTES);
    expect_counts(&pool, 0, 0, 40, 0, 0, 0);
    CHECK(ta_pool_take(&pool) == 0);
    CHECK(ta_pool_trim(&pool) == 0);

    /* A pool that is refilled after a trim serves fresh items again from the start. */
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    CHECK(ta_pool_take(&pool) == 1u);
    ta_pool_give(&pool, 1);
    CHECK(ta_pool_trim(&pool) == 1u);

    /* Taken items pin their page and every page before it; free pages behind them go. */
    CHECK(ta_pool_refill(&pool, 40) == TA_OK);
    for (uint32_t index = 1; index <= 40u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
        ((record *)ta_pool_at(&pool, index))->payload = index;
    }
    record *first = ta_pool_at(&pool, 1);
    record *last_kept = ta_pool_at(&pool, 24);
    static const uint32_t order[] = { 25, 26, 27, 5, 28, 29, 6, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40 };
    for (size_t position = 0; position < sizeof order / sizeof order[0]; position += 1u) {
        ta_pool_give(&pool, order[position]);
    }
    /* The free list, head first, is 40..30, 6, 29, 28, 5, 27, 26, 25: the reverse of the order given. */
    expect_counts(&pool, 22, 18, 40, 40, 40, 5);
    ta_test_allocator_reset_counts(&heap);
    CHECK(ta_pool_trim(&pool) == 2u);
    expect_counts(&pool, 22, 2, 40, 24, 24, 3);
    CHECK(ta_pool_at(&pool, 1) == first && ta_pool_at(&pool, 24) == last_kept);
    CHECK(ta_pool_at(&pool, 25) == NULL);
    CHECK(((record *)ta_pool_at(&pool, 24))->payload == 24u);
    CHECK(ta_pool_trim(&pool) == 0);
    CHECK(heap.stats.attempts == 0);
    /* The free list survived the unlinking in order: 6 was given after 5. */
    CHECK(ta_pool_take(&pool) == 6u);
    CHECK(ta_pool_take(&pool) == 5u);
    CHECK(ta_pool_take(&pool) == 0);
    /* And the pool grows again past the trimmed capacity with continuing indexes. */
    CHECK(ta_pool_refill(&pool, 1) == TA_OK);
    CHECK(ta_pool_take(&pool) == 25u);
    ta_pool_destroy(&pool);
    finish_clean();

    /* Free items in the middle pages are not trimmed: only trailing pages go. */
    ta_test_allocator_reset_counts(&heap);
    record_init(&pool, 0);
    CHECK(ta_pool_refill(&pool, 24) == TA_OK);
    for (uint32_t index = 1; index <= 24u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
    }
    for (uint32_t index = 9; index <= 16u; index += 1u) {
        ta_pool_give(&pool, index);
    }
    CHECK(ta_pool_trim(&pool) == 0); /* page 3 (items 17..24) is still in use */
    expect_counts(&pool, 16, 8, 24, 24, 24, 3);
    ta_pool_destroy(&pool);
    finish_clean();
}

/* Refill from `pool`, failing the n-th allocation; checks the pool and the heap are as before. */
static void sweep_refill(ta_pool *pool, uint32_t min_free, uint64_t allocations) {
    for (uint64_t nth = 1; nth <= allocations; nth += 1u) {
        ta_pool_counts before;
        ta_pool_counts_get(pool, &before);
        size_t live_blocks = heap.stats.live_blocks;
        size_t live_bytes = heap.stats.live_bytes;
        const ta_pool_page *directory = pool->directory;
        ta_test_allocator_fail_nth(&heap, nth);
        CHECK(ta_pool_refill(pool, min_free) == TA_ERR_NOMEM);
        ta_test_allocator_disarm(&heap);
        ta_pool_counts after;
        ta_pool_counts_get(pool, &after);
        CHECK(same_counts(&before, &after));
        CHECK(pool->directory == directory);
        CHECK(heap.stats.live_blocks == live_blocks && heap.stats.live_bytes == live_bytes);
        /* The free list is intact: the first take is the head, and it is put back. */
        uint32_t head = ta_pool_take(pool);
        if (head != 0) {
            ta_pool_give(pool, head);
        }
    }
    CHECK(heap.stats.bad_frees == 0);
}

static void test_refill_sweep(void) {
    /* A pool with no directory: ten pages need the directory and ten page allocations. */
    ta_pool pool;
    record_init(&pool, 0);
    ta_test_allocator_reset_counts(&heap);
    CHECK(ta_pool_refill(&pool, 10u * PAGE_ITEMS) == TA_OK);
    uint64_t allocations = heap.stats.allocations;
    CHECK(allocations == 11u);
    ta_pool_destroy(&pool);
    finish_clean();

    record_init(&pool, 0);
    sweep_refill(&pool, 10u * PAGE_ITEMS, allocations);
    CHECK(heap.stats.live_blocks == 0);
    CHECK(ta_pool_refill(&pool, 10u * PAGE_ITEMS) == TA_OK); /* a retry succeeds */
    expect_counts(&pool, 0, 80, 0, 80, 0, 10);
    ta_pool_destroy(&pool);
    finish_clean();

    /* A full directory (eight entries): 64 items with 18 taken leave 46 free, so 98 free need seven more pages
     * (fifteen in all) and a new directory of 16 entries: eight allocations. */
    record_init(&pool, 0);
    CHECK(ta_pool_refill(&pool, 8u * PAGE_ITEMS) == TA_OK);
    for (uint32_t index = 1; index <= 20u; index += 1u) {
        CHECK(ta_pool_take(&pool) == index);
    }
    ta_pool_give(&pool, 4);
    ta_pool_give(&pool, 9);
    size_t live_blocks = heap.stats.live_blocks;
    CHECK(live_blocks == 9u);
    sweep_refill(&pool, 98u, 8u);
    CHECK(heap.stats.live_blocks == live_blocks);
    CHECK(ta_pool_take(&pool) == 9u && ta_pool_take(&pool) == 4u);
    ta_pool_give(&pool, 4);
    ta_pool_give(&pool, 9);
    ta_test_allocator_reset_counts(&heap);
    CHECK(ta_pool_refill(&pool, 98u) == TA_OK);
    CHECK(heap.stats.allocations == 1u + 7u);
    ta_pool_destroy(&pool);
    finish_clean();
}

/* A seeded xorshift: the model test must not depend on the C library. */
static uint32_t next_random(uint32_t *state) {
    uint32_t value = *state;
    value ^= value << 13;
    value ^= value >> 17;
    value ^= value << 5;
    *state = value;
    return value;
}

static void test_model(void) {
    enum { LIMIT = 64, STEPS = 20000 };
    ta_pool pool;
    ta_pool_params params = record_params(0);
    params.page_items = 4u;
    CHECK(ta_pool_init(&pool, ta_test_allocator_get(&heap), &params) == TA_OK);
    bool taken[LIMIT + 1] = { false };
    uint32_t used = 0;
    uint32_t high_water = 0;
    uint32_t state = 0x9e3779b9u;
    for (uint32_t step = 0; step < STEPS; step += 1u) {
        uint32_t choice = next_random(&state) % 16u;
        if (choice < 7u && used < LIMIT) {
            uint32_t index = ta_pool_take(&pool);
            if (index == 0) {
                CHECK(ta_pool_refill(&pool, 1u + next_random(&state) % 9u) == TA_OK);
                index = ta_pool_take(&pool);
            }
            CHECK(index != 0 && index <= LIMIT && !taken[index]); /* never handed out twice */
            if (index != 0 && index <= LIMIT) {
                taken[index] = true;
                used += 1u;
                high_water = used > high_water ? used : high_water;
                ((record *)ta_pool_at(&pool, index))->payload = (uint64_t)index * 7u + 3u;
            }
        } else if (choice < 14u && used > 0) {
            uint32_t pick = next_random(&state) % LIMIT + 1u;
            while (!taken[pick]) {
                pick = pick % LIMIT + 1u;
            }
            CHECK(((record *)ta_pool_at(&pool, pick))->payload == (uint64_t)pick * 7u + 3u);
            taken[pick] = false;
            used -= 1u;
            ta_pool_give(&pool, pick);
        } else if (choice == 14u) {
            CHECK(ta_pool_refill(&pool, next_random(&state) % 20u) == TA_OK);
        } else {
            (void)ta_pool_trim(&pool);
        }
        ta_pool_counts counts;
        ta_pool_counts_get(&pool, &counts);
        CHECK(counts.used == used && counts.high_water == high_water);
        CHECK(counts.free == counts.capacity - used);
        CHECK(counts.extent <= counts.capacity && counts.used <= counts.extent);
        CHECK(counts.capacity == counts.pages * params.page_items);
    }
    for (uint32_t index = 1; index <= LIMIT; index += 1u) {
        if (taken[index]) {
            CHECK(((record *)ta_pool_at(&pool, index))->payload == (uint64_t)index * 7u + 3u);
        }
    }
    ta_pool_destroy(&pool);
    finish_clean();
}

int main(void) {
    test_layout_probes();
    test_init_validation();
    test_take_give_order();
    test_zero_fill();
    test_refill_growth();
    test_addresses_never_move();
    test_ceiling();
    test_trim();
    test_refill_sweep();
    test_model();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "pool_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("pool_test: ok");
    return 0;
}
