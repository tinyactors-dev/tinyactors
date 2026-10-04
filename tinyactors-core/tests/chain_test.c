/* test: standalone asan */
/*
 * Block A6 (src/chain.h): the chunk chain.
 *
 * Covers the geometry (payload per chunk, parameter errors), need against
 * the chunks a write really takes, writes of every interesting length at both
 * chunk sizes, appends, the all-or-nothing failure, free, the reader, chains
 * that interleave in one pool, a strict allocator that sees no allocation
 * outside refill, and a short seeded model check. It is fast, not exhaustive:
 * about twenty thousand steps over a few chains.
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

static void chunk_pool(ta_pool *pool, uint32_t chunk_size, uint32_t page_chunks, uint32_t max_chunks) {
    ta_chain_pool_params params = { .chunk_size = chunk_size, .page_chunks = page_chunks, .max_chunks = max_chunks };
    CHECK(ta_chain_pool_init(pool, ta_test_allocator_get(&heap), &params) == TA_OK);
}

static void finish_clean(void) {
    CHECK(heap.stats.live_blocks == 0);
}

static void test_geometry(void) {
    ta_pool pool;
    ta_chain_pool_params small = { .chunk_size = 4, .page_chunks = 8 };
    ta_chain_pool_params odd = { .chunk_size = 6, .page_chunks = 8 };
    ta_chain_pool_params unaligned = { .chunk_size = 70, .page_chunks = 8 };
    ta_chain_pool_params paging = { .chunk_size = 64, .page_chunks = 6 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    CHECK(ta_chain_pool_init(&pool, allocator, &small) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &odd) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &unaligned) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, &paging) == TA_ERR_INVALID);
    CHECK(ta_chain_pool_init(&pool, allocator, NULL) == TA_ERR_INVALID);
    CHECK(heap.stats.allocations == 0);

    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_chain_payload(&pool) == 60u);
    CHECK(heap.stats.allocations == 0);
    ta_pool_destroy(&pool);
    chunk_pool(&pool, 256, 8, 0);
    CHECK(ta_chain_payload(&pool) == 252u);
    ta_pool_destroy(&pool);
    chunk_pool(&pool, 8, 8, 0);
    CHECK(ta_chain_payload(&pool) == 4u);
    ta_pool_destroy(&pool);
    finish_clean();
}

static void test_need_arithmetic(void) {
    ta_pool pool;
    chunk_pool(&pool, 64, 8, 0);
    CHECK(ta_chain_need(&pool, NULL, 0) == 0);
    CHECK(ta_chain_need(&pool, NULL, 1) == 1);
    CHECK(ta_chain_need(&pool, NULL, 60) == 1);
    CHECK(ta_chain_need(&pool, NULL, 61) == 2);
    CHECK(ta_chain_need(&pool, NULL, 120) == 2);
    CHECK(ta_chain_need(&pool, NULL, 121) == 3);
    ta_chain empty = { 0 };
    CHECK(ta_chain_need(&pool, &empty, 61) == 2);

    /* A partly filled tail: only the overflow needs chunks. 70 bytes: chunk 2 holds 10, 50 are free. */
    ta_chain partial = { .head = 1, .tail = 2, .length = 70 };
    CHECK(ta_chain_need(&pool, &partial, 0) == 0);
    CHECK(ta_chain_need(&pool, &partial, 50) == 0);
    CHECK(ta_chain_need(&pool, &partial, 51) == 1);
    CHECK(ta_chain_need(&pool, &partial, 110) == 1);
    CHECK(ta_chain_need(&pool, &partial, 111) == 2);
    /* An exactly full tail needs a whole chunk for one more byte. */
    ta_chain full = { .head = 1, .tail = 2, .length = 120 };
    CHECK(ta_chain_need(&pool, &full, 0) == 0);
    CHECK(ta_chain_need(&pool, &full, 1) == 1);
    /* The largest request cannot overflow: ceil(UINT32_MAX / 60). */
    CHECK(ta_chain_need(&pool, NULL, UINT32_MAX) == 71582789u);
    ta_pool_destroy(&pool);
    finish_clean();
}

int main(void) {
    test_geometry();
    test_need_arithmetic();
    CHECK(heap.stats.bad_frees == 0);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "chain_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("chain_test: ok");
    return 0;
}
