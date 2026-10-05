/*
 * ta_index_pool: fixed-size items addressed by uint32_t index in pages that
 * never move, with an intrusive free list, refill up to a ceiling and trim
 * (src/index_pool.h). The actor table keeps its slots in one.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from the test
 * allocator (test_allocator.h), which can refuse any allocation and
 * counts the blocks that are live.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses an item size of 0, an alignment of 0 or not a power of
 *    two, an item size that is not a multiple of the alignment, a page
 *    shift above 31, a page too large for size_t, a link offset that is
 *    not a multiple of 4 or does not lie inside the item, a page smaller
 *    than a pointer and a ceiling of 0, with TA_ERR_INVALID, leaving the
 *    pool untouched; a valid init allocates nothing, holds no item, and
 *    an empty pool answers TA_ERR_FULL to take, NULL to at, and frees
 *    nothing when destroyed;
 * 2. refill of 0, or of no more than the free items, changes nothing and
 *    allocates nothing; refill(1) adds one page and its directory, every
 *    item of the page becomes free, and refill(n) adds the pages n items
 *    need;
 * 3. take hands out items never taken in ascending order from index 1;
 *    give puts an item at the head, so the item given last is taken
 *    first and every given item before any never taken; an item keeps its
 *    bytes from give to take apart from its link, which take sets to 0,
 *    and an item of a new page reads as zero;
 * 4. the counters: used and free move with every take and give, used +
 *    free is the items the pages hold, and high_water is the highest
 *    index taken, also with one item;
 * 5. index i lives in page (i - 1) >> k at item (i - 1) & mask, items of
 *    item_size bytes at the item alignment; at answers NULL for 0, an
 *    index above the ceiling and an index whose page is not allocated;
 *    ta_index_pool_item answers what at answers, and ta_index_pool_links
 *    serves a list.h list linked through another field of the items;
 * 6. items never move: across refills that add pages and replace the
 *    directory, every item keeps its address and its bytes;
 * 7. the ceiling: refill reaches it exactly, a last page the ceiling cuts
 *    short holds only the items up to it, refill past it answers
 *    TA_ERR_FULL and changes nothing, take of a pool at its ceiling with
 *    every item taken answers TA_ERR_FULL, and no index above the ceiling
 *    is ever issued; at the largest ceiling, 2^32 - 1, refill asks for
 *    the page it needs and, refused, answers TA_ERR_NOMEM;
 * 8. a refill refused at any of its allocations (any page, or the
 *    directory) answers TA_ERR_NOMEM and changes nothing: the same pages,
 *    directory, counters and free-list order, and no block leaked;
 * 9. trim frees only pages whose items are all free, from the last down,
 *    while at least keep items stay free; a page partly in use stays; a
 *    freed page below the last page kept leaves a hole that the next
 *    refill fills before it adds a page, its indices coming back in
 *    ascending order; at answers NULL on a freed page; high_water stays;
 *    trim to 0 of a pool with nothing taken frees every page and the
 *    directory; and trim with nothing to free changes nothing;
 * 10. take, give, at, item and links never allocate, and trim only frees;
 * 11. destroy frees every page and the directory, taken items included.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* Items of 16 bytes aligned to 8: a payload, the pool's link, and a link of the test's own list. */
typedef struct {
    uint64_t payload;
    uint32_t link;
    uint32_t other;
} item;

/* Four items to a page. */
#define PAGE_SHIFT 2u
#define PAGE_ITEMS (1u << PAGE_SHIFT)

static ta_index_pool_params item_params(test_allocator *heap, uint32_t ceiling) {
    return (ta_index_pool_params){
        .allocator = test_allocator_of(heap),
        .item_size = sizeof(item),
        .item_alignment = _Alignof(item),
        .link_offset = offsetof(item, link),
        .page_shift = PAGE_SHIFT,
        .ceiling = ceiling,
    };
}

static ta_index_pool make_pool(test_allocator *heap, uint32_t ceiling) {
    ta_index_pool pool;
    ta_index_pool_params params = item_params(heap, ceiling);
    ta_result result = ta_index_pool_init(&pool, &params);
    CHECK(result == TA_OK);
    return pool;
}

static item *at(const ta_index_pool *pool, uint32_t index) {
    return ta_index_pool_at(pool, index);
}

static uint32_t take(ta_index_pool *pool) {
    uint32_t index = 0;
    CHECK(ta_index_pool_take(pool, &index) == TA_OK);
    return index;
}

/* The free list, head first, into `out` (room for `room`); answers its length. */
static uint32_t free_order(ta_index_pool *pool, uint32_t *out, uint32_t room) {
    ta_list_links links = ta_index_pool_links(pool, pool->link_offset);
    uint32_t count = 0;
    for (uint32_t index = ta_list_head(&pool->free_list); index != 0; index = ta_list_next(&links, index)) {
        if (count < room) {
            out[count] = index;
        }
        count += 1u;
    }
    return count;
}

/* Everything a failed call must leave as it was. */
typedef struct {
    uint32_t used;
    uint32_t free;
    uint32_t high_water;
    uint32_t holes;
    uint32_t page_count;
    uint32_t directory_capacity;
    void *directory;
    unsigned char *pages[16];
    uint32_t order[64];
    uint32_t order_length;
    uint32_t live;
    size_t live_bytes;
} snapshot;

static snapshot take_snapshot(ta_index_pool *pool, const test_allocator *heap) {
    snapshot shot = {
        .used = pool->used,
        .free = pool->free,
        .high_water = pool->high_water,
        .holes = pool->holes,
        .page_count = pool->pages.count,
        .directory_capacity = pool->pages.capacity,
        .directory = pool->pages.items,
        .live = heap->live,
        .live_bytes = heap->live_bytes,
    };
    for (uint32_t page = 0; page < pool->pages.count && page < 16u; page += 1u) {
        shot.pages[page] = ((ta_index_pool_page *)pool->pages.items)[page].items;
    }
    shot.order_length = free_order(pool, shot.order, 64u);
    return shot;
}

static bool same_snapshot(const snapshot *a, const snapshot *b) {
    return memcmp(a, b, sizeof(snapshot)) == 0;
}

/* 1 */
static void test_init(void) {
    test_allocator heap = {0};
    ta_index_pool_params base = item_params(&heap, 100u);
    ta_index_pool_params bad[] = { base, base, base, base, base, base, base, base, base, base, base };
    bad[0].item_size = 0;
    bad[1].item_alignment = 0;
    bad[2].item_alignment = 12;
    bad[3].item_size = 20; /* not a multiple of 8 */
    bad[4].page_shift = 32;
    bad[5].page_shift = 31;
    bad[5].item_size = (size_t)1 << (sizeof(size_t) * 8u - 4u); /* page past SIZE_MAX */
    bad[5].item_alignment = 8;
    bad[6].link_offset = 2;
    bad[7].link_offset = sizeof(item); /* past the item */
    bad[8].item_size = 2; /* no room for a link */
    bad[8].item_alignment = 2;
    bad[8].link_offset = 0;
    bad[8].page_shift = 4;
    bad[9].ceiling = 0;
    bad[10].item_size = 4; /* a page of one 4-byte item: smaller than a pointer */
    bad[10].item_alignment = 4;
    bad[10].link_offset = 0;
    bad[10].page_shift = 0;
    for (uint32_t i = 0; i < TA_ARRAY_COUNT(bad); i += 1u) {
        ta_index_pool pool;
        memset(&pool, 0x3c, sizeof pool);
        ta_index_pool before = pool;
        CHECK(ta_index_pool_init(&pool, &bad[i]) == TA_ERR_INVALID);
        CHECK(memcmp(&pool, &before, sizeof pool) == 0);
    }
    CHECK(heap.attempts == 0);

    ta_index_pool pool = make_pool(&heap, 100u);
    CHECK(heap.attempts == 0);
    CHECK(pool.used == 0 && pool.free == 0 && pool.high_water == 0 && pool.pages.count == 0);
    uint32_t index = 77u;
    CHECK(ta_index_pool_take(&pool, &index) == TA_ERR_FULL);
    CHECK(index == 77u);
    CHECK(ta_index_pool_at(&pool, 0) == NULL);
    CHECK(ta_index_pool_at(&pool, 1) == NULL);
    CHECK(ta_index_pool_at(&pool, 100u) == NULL);
    ta_index_pool_trim(&pool, 0);
    ta_index_pool_destroy(&pool);
    CHECK(heap.attempts == 0 && heap.frees == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 2 */
static void test_refill(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 0) == TA_OK);
    CHECK(heap.attempts == 0 && pool.pages.count == 0);

    CHECK(ta_index_pool_refill(&pool, 1) == TA_OK);
    CHECK(heap.live == 2u); /* the page and the directory */
    CHECK(pool.pages.count == 1u && pool.free == PAGE_ITEMS && pool.used == 0);
    uint32_t attempts = heap.attempts;
    CHECK(ta_index_pool_refill(&pool, 1) == TA_OK);
    CHECK(ta_index_pool_refill(&pool, PAGE_ITEMS) == TA_OK);
    CHECK(heap.attempts == attempts);

    /* Nine free items need two more pages; a page is never half added. */
    CHECK(ta_index_pool_refill(&pool, 2u * PAGE_ITEMS + 1u) == TA_OK);
    CHECK(pool.pages.count == 3u && pool.free == 3u * PAGE_ITEMS);
    CHECK(heap.live == 4u);

    /* Refill counts free items, not pages: with two taken, one more page. */
    (void)take(&pool);
    (void)take(&pool);
    CHECK(ta_index_pool_refill(&pool, 3u * PAGE_ITEMS) == TA_OK);
    CHECK(pool.pages.count == 4u && pool.free == 4u * PAGE_ITEMS - 2u && pool.used == 2u);
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 3 */
static void test_order(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 2u * PAGE_ITEMS) == TA_OK);
    for (uint32_t index = 1; index <= 2u * PAGE_ITEMS; index += 1u) {
        const item *fresh = at(&pool, index);
        CHECK(fresh->payload == 0 && fresh->other == 0);
    }
    uint32_t a = take(&pool);
    uint32_t b = take(&pool);
    uint32_t c = take(&pool);
    CHECK(a == 1u && b == 2u && c == 3u);
    CHECK(at(&pool, a)->payload == 0 && at(&pool, a)->link == 0 && at(&pool, a)->other == 0);

    /* Bytes survive from give to take, apart from the link. */
    at(&pool, a)->payload = 0x1111u;
    at(&pool, a)->other = 7u;
    at(&pool, c)->payload = 0x3333u;
    ta_index_pool_give(&pool, a);
    ta_index_pool_give(&pool, c);
    /* Given last, taken first; both before 4, never taken. */
    CHECK(take(&pool) == c);
    CHECK(at(&pool, c)->payload == 0x3333u && at(&pool, c)->link == 0);
    CHECK(take(&pool) == a);
    CHECK(at(&pool, a)->payload == 0x1111u && at(&pool, a)->other == 7u && at(&pool, a)->link == 0);
    CHECK(take(&pool) == 4u);
    CHECK(take(&pool) == 5u);

    /* Given items go before fresh ones, also after a refill added more. */
    ta_index_pool_give(&pool, b);
    CHECK(ta_index_pool_refill(&pool, 3u * PAGE_ITEMS) == TA_OK);
    CHECK(take(&pool) == b);
    for (uint32_t expect = 6u; expect <= 4u * PAGE_ITEMS; expect += 1u) {
        CHECK(take(&pool) == expect);
    }
    uint32_t none = 0;
    CHECK(ta_index_pool_take(&pool, &none) == TA_ERR_FULL);
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 4 */
static void test_counters(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 1) == TA_OK);
    /* One item. */
    uint32_t one = take(&pool);
    CHECK(one == 1u && pool.used == 1u && pool.free == PAGE_ITEMS - 1u && pool.high_water == 1u);
    ta_index_pool_give(&pool, one);
    CHECK(pool.used == 0 && pool.free == PAGE_ITEMS && pool.high_water == 1u);
    CHECK(take(&pool) == one && pool.high_water == 1u);
    ta_index_pool_give(&pool, one);

    CHECK(ta_index_pool_refill(&pool, 3u * PAGE_ITEMS) == TA_OK);
    uint32_t taken[3 * PAGE_ITEMS];
    for (uint32_t i = 0; i < 3u * PAGE_ITEMS; i += 1u) {
        taken[i] = take(&pool);
        CHECK(pool.used == i + 1u && pool.used + pool.free == 3u * PAGE_ITEMS);
        CHECK(taken[i] == i + 1u && pool.high_water == i + 1u);
    }
    uint32_t free_index = 0;
    CHECK(ta_index_pool_take(&pool, &free_index) == TA_ERR_FULL);
    CHECK(pool.used == 3u * PAGE_ITEMS && pool.free == 0);
    for (uint32_t i = 0; i < 3u * PAGE_ITEMS; i += 1u) {
        ta_index_pool_give(&pool, taken[i]);
        CHECK(pool.used + pool.free == 3u * PAGE_ITEMS && pool.high_water == 3u * PAGE_ITEMS);
    }
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 5 */
static void test_addressing(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 10u);
    CHECK(ta_index_pool_refill(&pool, 5u) == TA_OK);
    CHECK(pool.pages.count == 2u);
    const ta_index_pool_page *pages = pool.pages.items;
    for (uint32_t index = 1; index <= 8u; index += 1u) {
        uint32_t position = index - 1u;
        unsigned char *expect = pages[position >> PAGE_SHIFT].items + (position & (PAGE_ITEMS - 1u)) * sizeof(item);
        CHECK((unsigned char *)ta_index_pool_at(&pool, index) == expect);
        CHECK(ta_index_pool_item(&pool, index) == ta_index_pool_at(&pool, index));
        CHECK((uintptr_t)expect % _Alignof(item) == 0);
    }
    CHECK(ta_index_pool_at(&pool, 0) == NULL);
    CHECK(ta_index_pool_item(&pool, 0) == NULL);
    CHECK(ta_index_pool_at(&pool, 9u) == NULL);   /* page 2 is not allocated */
    CHECK(ta_index_pool_at(&pool, 11u) == NULL);  /* above the ceiling */
    CHECK(ta_index_pool_at(&pool, UINT32_MAX) == NULL);

    /* A list of the user's own, through another field of taken items. */
    uint32_t a = take(&pool);
    uint32_t b = take(&pool);
    uint32_t c = take(&pool);
    ta_list_links others = ta_index_pool_links(&pool, offsetof(item, other));
    CHECK(others.item == ta_index_pool_item && others.items == &pool && others.offset == offsetof(item, other));
    ta_list list = TA_LIST_EMPTY;
    ta_list_push_back(&list, &others, c);
    ta_list_push_back(&list, &others, a);
    ta_list_push_back(&list, &others, b);
    CHECK(ta_list_pop_front(&list, &others) == c);
    CHECK(ta_list_pop_front(&list, &others) == a);
    CHECK(ta_list_pop_front(&list, &others) == b);
    CHECK(ta_list_empty(&list));
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 6 */
static void test_never_moves(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 100000u);
    CHECK(ta_index_pool_refill(&pool, 1) == TA_OK);
    item *first[PAGE_ITEMS];
    for (uint32_t i = 0; i < PAGE_ITEMS; i += 1u) {
        uint32_t index = take(&pool);
        first[i] = at(&pool, index);
        first[i]->payload = 1000u + index;
    }
    void *directory = pool.pages.items;
    /* Forty pages: the directory is replaced several times. */
    for (uint32_t pages = 2; pages <= 40u; pages += 1u) {
        CHECK(ta_index_pool_refill(&pool, pool.free + PAGE_ITEMS) == TA_OK);
        CHECK(pool.pages.count == pages);
        uint32_t index = take(&pool);
        at(&pool, index)->payload = 1000u + index;
    }
    CHECK(pool.pages.items != directory);
    for (uint32_t i = 0; i < PAGE_ITEMS; i += 1u) {
        CHECK(at(&pool, i + 1u) == first[i]);
        CHECK(first[i]->payload == 1000u + i + 1u);
    }
    for (uint32_t index = PAGE_ITEMS + 1u; index <= pool.high_water; index += 1u) {
        CHECK(at(&pool, index)->payload == 1000u + index);
    }
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 7 */
static void test_ceiling(void) {
    test_allocator heap = {0};
    /* Ten items: two whole pages and a third cut to two. */
    ta_index_pool pool = make_pool(&heap, 10u);
    snapshot before = take_snapshot(&pool, &heap);
    CHECK(ta_index_pool_refill(&pool, 11u) == TA_ERR_FULL);
    snapshot after = take_snapshot(&pool, &heap);
    CHECK(same_snapshot(&before, &after) && heap.attempts == 0);

    CHECK(ta_index_pool_refill(&pool, 10u) == TA_OK);
    CHECK(pool.pages.count == 3u && pool.free == 10u);
    for (uint32_t expect = 1; expect <= 10u; expect += 1u) {
        CHECK(take(&pool) == expect);
    }
    uint32_t index = 0;
    CHECK(ta_index_pool_take(&pool, &index) == TA_ERR_FULL && index == 0);
    CHECK(pool.used == 10u && pool.free == 0 && pool.high_water == 10u);

    /* Full at the ceiling: refill of even one answers TA_ERR_FULL, changing nothing. */
    before = take_snapshot(&pool, &heap);
    uint32_t attempts = heap.attempts;
    CHECK(ta_index_pool_refill(&pool, 1) == TA_ERR_FULL);
    after = take_snapshot(&pool, &heap);
    CHECK(same_snapshot(&before, &after) && heap.attempts == attempts);

    /* With two given back, refill of two is satisfied, of three is not. */
    ta_index_pool_give(&pool, 4u);
    ta_index_pool_give(&pool, 9u);
    CHECK(ta_index_pool_refill(&pool, 2u) == TA_OK);
    before = take_snapshot(&pool, &heap);
    CHECK(ta_index_pool_refill(&pool, 3u) == TA_ERR_FULL);
    after = take_snapshot(&pool, &heap);
    CHECK(same_snapshot(&before, &after) && heap.attempts == attempts);
    CHECK(take(&pool) == 9u && take(&pool) == 4u);
    ta_index_pool_destroy(&pool);

    /* A refill well below the ceiling stops at the items it needs. */
    pool = make_pool(&heap, 6u);
    CHECK(ta_index_pool_refill(&pool, 6u) == TA_OK);
    CHECK(pool.pages.count == 2u && pool.free == 6u);
    ta_index_pool_destroy(&pool);

    /* The largest ceiling: pages of 2^31 four-byte items. Refused, nothing changes. */
    pool = (ta_index_pool){0};
    CHECK(ta_index_pool_init(&pool, &(ta_index_pool_params){ .allocator = test_allocator_of(&heap),
                                                             .item_size = 4,
                                                             .item_alignment = 4,
                                                             .link_offset = 0,
                                                             .page_shift = 31,
                                                             .ceiling = UINT32_MAX }) == TA_OK);
    heap.policy.largest = 4096;
    attempts = heap.attempts;
    before = take_snapshot(&pool, &heap);
    CHECK(ta_index_pool_refill(&pool, UINT32_MAX) == TA_ERR_NOMEM);
    CHECK(heap.attempts == attempts + 1u && heap.last_size == ((size_t)4u << 31));
    after = take_snapshot(&pool, &heap);
    CHECK(same_snapshot(&before, &after));
    heap.policy.largest = 0;
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 8 */
static void test_refill_failure(void) {
    for (uint32_t fail = 1;; fail += 1u) {
        /* Eight pages, a full directory of 8. Every item taken, then page 1
         * and page 7 given back: trim frees page 7 from the end of the
         * directory and leaves page 1 as a hole. */
        test_allocator heap = {0};
        ta_index_pool pool = make_pool(&heap, 1000u);
        CHECK(ta_index_pool_refill(&pool, 8u * PAGE_ITEMS) == TA_OK);
        CHECK(pool.pages.count == 8u && pool.pages.capacity == 8u);
        for (uint32_t i = 0; i < 8u * PAGE_ITEMS; i += 1u) {
            (void)take(&pool);
        }
        for (uint32_t index = PAGE_ITEMS + 1u; index <= 2u * PAGE_ITEMS; index += 1u) {
            ta_index_pool_give(&pool, index);
        }
        for (uint32_t index = 7u * PAGE_ITEMS + 1u; index <= 8u * PAGE_ITEMS; index += 1u) {
            ta_index_pool_give(&pool, index);
        }
        ta_index_pool_trim(&pool, 0);
        CHECK(pool.pages.count == 7u && pool.holes == 1u && pool.free == 0);
        ta_index_pool_give(&pool, 2u);

        /* Twelve more free items: the hole, then pages 7 and 8, then a directory of 16. */
        uint32_t needed = pool.free + 3u * PAGE_ITEMS;
        snapshot before = take_snapshot(&pool, &heap);
        heap.policy.fail_at = heap.attempts + fail;
        uint32_t attempts = heap.attempts;
        ta_result result = ta_index_pool_refill(&pool, needed);
        uint32_t made = heap.attempts - attempts;
        heap.policy.fail_at = 0;
        if (result == TA_OK) {
            CHECK(fail > made);
            CHECK(made == 4u);
            CHECK(pool.holes == 0 && pool.pages.count == 9u && pool.pages.capacity == 16u && pool.free == needed);
            /* The given item first, then the hole's items, then the new pages', ascending. */
            CHECK(take(&pool) == 2u);
            for (uint32_t index = PAGE_ITEMS + 1u; index <= 2u * PAGE_ITEMS; index += 1u) {
                CHECK(take(&pool) == index);
            }
            for (uint32_t index = 7u * PAGE_ITEMS + 1u; index <= 9u * PAGE_ITEMS; index += 1u) {
                CHECK(take(&pool) == index);
            }
            ta_index_pool_destroy(&pool);
            CHECK(test_allocator_finish(&heap) == 0);
            CHECK(fail == 5u);
            break;
        }
        CHECK(result == TA_ERR_NOMEM && made == fail);
        snapshot after = take_snapshot(&pool, &heap);
        CHECK(same_snapshot(&before, &after));
        ta_index_pool_destroy(&pool);
        CHECK(test_allocator_finish(&heap) == 0);
        if (fail > 10u) {
            CHECK(false);
            break;
        }
    }
}

/* 9 */
static void test_trim(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 4u * PAGE_ITEMS) == TA_OK);
    /* Take pages 0 to 2 whole, and one item of page 3. */
    uint32_t taken[3u * PAGE_ITEMS + 1u];
    for (uint32_t i = 0; i < TA_ARRAY_COUNT(taken); i += 1u) {
        taken[i] = take(&pool);
    }
    CHECK(pool.high_water == 3u * PAGE_ITEMS + 1u);

    /* Nothing whole is free: trim changes nothing and frees nothing. */
    snapshot before = take_snapshot(&pool, &heap);
    uint32_t attempts = heap.attempts;
    uint32_t frees = heap.frees;
    ta_index_pool_trim(&pool, 0);
    snapshot after = take_snapshot(&pool, &heap);
    CHECK(same_snapshot(&before, &after) && heap.frees == frees);

    /* Give page 1 back whole, and one item of page 0. */
    for (uint32_t i = PAGE_ITEMS; i < 2u * PAGE_ITEMS; i += 1u) {
        ta_index_pool_give(&pool, taken[i]);
    }
    ta_index_pool_give(&pool, taken[0]);
    CHECK(pool.free == 3u + PAGE_ITEMS + 1u);

    /* keep above what freeing page 1 would leave: nothing goes. */
    ta_index_pool_trim(&pool, 5u);
    CHECK(pool.pages.count == 4u && pool.holes == 0 && pool.free == 3u + PAGE_ITEMS + 1u);
    CHECK(heap.frees == frees);

    /* keep 4: page 1 goes, a hole below page 3, which is partly used, and page 0 stays. */
    void *page0 = ((ta_index_pool_page *)pool.pages.items)[0].items;
    ta_index_pool_trim(&pool, 4u);
    CHECK(heap.frees == frees + 1u);
    CHECK(pool.pages.count == 4u && pool.holes == 1u && pool.free == 4u && pool.used == 2u * PAGE_ITEMS);
    CHECK(((ta_index_pool_page *)pool.pages.items)[0].items == page0);
    CHECK(ta_index_pool_at(&pool, PAGE_ITEMS + 1u) == NULL && ta_index_pool_at(&pool, 2u * PAGE_ITEMS) == NULL);
    CHECK(ta_index_pool_at(&pool, 1u) != NULL && ta_index_pool_at(&pool, 4u * PAGE_ITEMS) != NULL);
    CHECK(pool.high_water == 3u * PAGE_ITEMS + 1u);
    CHECK(heap.attempts == attempts);
    /* The free list lost page 1's items and kept the order of the rest. */
    uint32_t order[8];
    CHECK(free_order(&pool, order, 8u) == 4u);
    CHECK(order[0] == taken[0] && order[1] == 3u * PAGE_ITEMS + 2u && order[2] == 3u * PAGE_ITEMS + 3u &&
          order[3] == 4u * PAGE_ITEMS);

    /* Refill fills the hole first, its indices in ascending order after the free ones. */
    CHECK(ta_index_pool_refill(&pool, 5u) == TA_OK);
    CHECK(pool.pages.count == 4u && pool.holes == 0 && pool.free == 4u + PAGE_ITEMS);
    CHECK(take(&pool) == taken[0]);
    CHECK(take(&pool) == 3u * PAGE_ITEMS + 2u);
    CHECK(take(&pool) == 3u * PAGE_ITEMS + 3u);
    CHECK(take(&pool) == 4u * PAGE_ITEMS);
    for (uint32_t index = PAGE_ITEMS + 1u; index <= 2u * PAGE_ITEMS; index += 1u) {
        CHECK(take(&pool) == index);
        CHECK(at(&pool, index)->payload == 0);
    }

    /* Give everything back: trim to 0 frees every page and the directory. */
    for (uint32_t index = 1; index <= 4u * PAGE_ITEMS; index += 1u) {
        ta_index_pool_give(&pool, index);
    }
    CHECK(pool.used == 0 && pool.free == 4u * PAGE_ITEMS);
    ta_index_pool_trim(&pool, 0);
    CHECK(pool.pages.count == 0 && pool.pages.items == NULL && pool.free == 0 && pool.holes == 0);
    CHECK(ta_list_empty(&pool.free_list) && heap.live == 0);
    CHECK(pool.high_water == 4u * PAGE_ITEMS);

    /* Trimmed to nothing, it refills from index 1 again. */
    CHECK(ta_index_pool_refill(&pool, 1u) == TA_OK);
    CHECK(take(&pool) == 1u);
    ta_index_pool_destroy(&pool);

    /* Trim from the top: with keep, the last pages go first, the shrinking directory leaves no hole. */
    pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 4u * PAGE_ITEMS) == TA_OK);
    ta_index_pool_trim(&pool, 2u * PAGE_ITEMS);
    CHECK(pool.pages.count == 2u && pool.holes == 0 && pool.free == 2u * PAGE_ITEMS);
    CHECK(free_order(&pool, order, 8u) == 2u * PAGE_ITEMS);
    for (uint32_t i = 0; i < 2u * PAGE_ITEMS; i += 1u) {
        CHECK(order[i] == i + 1u);
    }
    /* A page the ceiling cut short counts only its items against keep. */
    ta_index_pool_destroy(&pool);
    pool = make_pool(&heap, 10u);
    CHECK(ta_index_pool_refill(&pool, 10u) == TA_OK);
    ta_index_pool_trim(&pool, 8u);
    CHECK(pool.pages.count == 2u && pool.free == 8u);
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 10 */
static void test_no_allocation(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 3u * PAGE_ITEMS) == TA_OK);
    heap.policy.refuse = true;
    uint32_t attempts = heap.attempts;
    uint32_t a = take(&pool);
    uint32_t b = take(&pool);
    CHECK(ta_index_pool_at(&pool, a) != NULL && ta_index_pool_item(&pool, b) != NULL);
    ta_list_links links = ta_index_pool_links(&pool, offsetof(item, other));
    (void)links;
    ta_index_pool_give(&pool, a);
    ta_index_pool_give(&pool, b);
    for (uint32_t i = 0; i < 3u * PAGE_ITEMS; i += 1u) {
        (void)take(&pool);
    }
    uint32_t index = 0;
    CHECK(ta_index_pool_take(&pool, &index) == TA_ERR_FULL);
    for (uint32_t i = 1; i <= 3u * PAGE_ITEMS; i += 1u) {
        ta_index_pool_give(&pool, i);
    }
    ta_index_pool_trim(&pool, PAGE_ITEMS);
    CHECK(heap.attempts == attempts);
    heap.policy.refuse = false;
    ta_index_pool_destroy(&pool);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* 11 */
static void test_destroy(void) {
    test_allocator heap = {0};
    ta_index_pool pool = make_pool(&heap, 1000u);
    CHECK(ta_index_pool_refill(&pool, 5u * PAGE_ITEMS) == TA_OK);
    for (uint32_t i = 0; i < 2u * PAGE_ITEMS + 1u; i += 1u) {
        (void)take(&pool);
    }
    CHECK(heap.live == 6u);
    ta_index_pool_destroy(&pool);
    CHECK(heap.live == 0);
    CHECK(pool.pages.count == 0 && pool.pages.items == NULL && pool.used == 0 && pool.free == 0);
    CHECK(ta_index_pool_at(&pool, 1u) == NULL);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_init();
    test_refill();
    test_order();
    test_counters();
    test_addressing();
    test_never_moves();
    test_ceiling();
    test_refill_failure();
    test_trim();
    test_no_allocation();
    test_destroy();
    if (failures != 0) {
        fprintf(stderr, "index_pool_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("index_pool_test: ok");
    return 0;
}
