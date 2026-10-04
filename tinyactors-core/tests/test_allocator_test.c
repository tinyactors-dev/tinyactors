/* test: standalone asan */
/*
 * The test allocator (tests/test_allocator.h) is what every other core test
 * trusts to count and to fail, so it is tested first: its counters agree
 * with what was done, each failure policy refuses exactly what it says, the
 * lanes add up to the totals, memory is aligned and, when asked, zeroed, and
 * strict mode catches the frees it exists to catch.
 *
 * Uses the C library; the core does not. Links the helper and nothing else
 * of core: it calls the allocator through the public ta_allocator types only.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

/* take allocates through the allocator value, as the library does. */
static void *take(ta_allocator allocator, size_t size, size_t alignment) {
    return allocator.v->alloc(allocator.self, size, alignment);
}

/* give frees through the allocator value, with the facts asked for. */
static void give(ta_allocator allocator, void *pointer, size_t size, size_t alignment) {
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = pointer, .size = size,
                                                                   .alignment = alignment });
}

static void test_counters(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    void *a = take(allocator, 100, 8);
    void *b = take(allocator, 40, 16);
    CHECK(a != NULL && b != NULL);
    CHECK(heap.stats.attempts == 2 && heap.stats.allocations == 2 && heap.stats.refused == 0);
    CHECK(heap.stats.live_blocks == 2 && heap.stats.live_bytes == 140);
    CHECK(heap.stats.peak_bytes == 140 && heap.stats.largest_block == 100);
    give(allocator, a, 100, 8);
    CHECK(heap.stats.frees == 1 && heap.stats.live_blocks == 1 && heap.stats.live_bytes == 40);
    CHECK(heap.stats.peak_bytes == 140);
    give(allocator, NULL, 0, 0); /* as libc: nothing, and not counted */
    CHECK(heap.stats.frees == 1);
    CHECK(ta_test_allocator_finish(&heap) == 1); /* b is still live; the leak line on stderr is expected */
    give(allocator, b, 40, 16);
    CHECK(ta_test_allocator_finish(&heap) == 0);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.live_bytes == 0);

    /* A size of zero is a block like another. */
    void *empty = take(allocator, 0, 1);
    CHECK(empty != NULL && heap.stats.live_blocks == 1 && heap.stats.live_bytes == 0);
    give(allocator, empty, 0, 1);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_reset_counts(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    void *a = take(allocator, 64, 8);
    ta_test_allocator_reset_counts(&heap);
    CHECK(heap.stats.attempts == 0 && heap.stats.allocations == 0 && heap.stats.frees == 0);
    CHECK(heap.stats.largest_block == 0);
    CHECK(heap.stats.live_blocks == 1 && heap.stats.live_bytes == 64); /* facts about memory are kept */
    CHECK(heap.stats.peak_bytes == 64);
    give(allocator, a, 64, 8);
    CHECK(heap.stats.frees == 1 && heap.stats.live_blocks == 0);
}

static void test_fail_nth(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_test_allocator_fail_nth(&heap, 3);
    void *first = take(allocator, 8, 8);
    void *second = take(allocator, 8, 8);
    void *third = take(allocator, 8, 8);
    void *fourth = take(allocator, 8, 8);
    CHECK(first != NULL && second != NULL && third == NULL && fourth != NULL); /* once */
    CHECK(heap.stats.attempts == 4 && heap.stats.allocations == 3 && heap.stats.refused == 1);
    CHECK(heap.stats.live_blocks == 3);
    give(allocator, first, 8, 8);
    give(allocator, second, 8, 8);
    give(allocator, fourth, 8, 8);

    /* Counted from now, not from the first call. */
    ta_test_allocator_fail_nth(&heap, 1);
    CHECK(take(allocator, 8, 8) == NULL);
    void *next = take(allocator, 8, 8);
    CHECK(next != NULL);
    give(allocator, next, 8, 8);
    CHECK(heap.stats.live_blocks == 0);
}

static void test_fail_from_nth(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_test_allocator_fail_from_nth(&heap, 2);
    void *first = take(allocator, 8, 8);
    CHECK(first != NULL);
    CHECK(take(allocator, 8, 8) == NULL);
    CHECK(take(allocator, 8, 8) == NULL); /* sticky */
    CHECK(take(allocator, 8, 8) == NULL);
    ta_test_allocator_disarm(&heap);
    void *again = take(allocator, 8, 8);
    CHECK(again != NULL);
    CHECK(heap.stats.refused == 3 && heap.stats.attempts == 5);
    give(allocator, first, 8, 8);
    give(allocator, again, 8, 8);
}

static void test_refuse_policies(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_test_allocator_refuse_from(&heap, 1000);
    void *small = take(allocator, 999, 8);
    CHECK(small != NULL);
    CHECK(take(allocator, 1000, 8) == NULL);
    CHECK(take(allocator, 5000, 8) == NULL);
    ta_test_allocator_refuse_from(&heap, 0);
    void *big = take(allocator, 5000, 8);
    CHECK(big != NULL);
    ta_test_allocator_refuse_all(&heap, true);
    CHECK(take(allocator, 1, 1) == NULL);
    ta_test_allocator_refuse_all(&heap, false);
    void *last = take(allocator, 1, 1);
    CHECK(last != NULL);
    CHECK(heap.stats.refused == 3);
    give(allocator, small, 999, 8);
    give(allocator, big, 5000, 8);
    give(allocator, last, 1, 1);
}

static void test_large_threshold(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_test_allocator_set_large_threshold(&heap, 100);
    void *a = take(allocator, 99, 8);
    void *b = take(allocator, 100, 8);
    void *c = take(allocator, 4000, 8);
    CHECK(heap.stats.large_allocations == 2);
    ta_test_allocator_fail_nth(&heap, 1);
    CHECK(take(allocator, 4000, 8) == NULL);
    CHECK(heap.stats.large_allocations == 2); /* a refused request is not an allocation */
    give(allocator, a, 99, 8);
    give(allocator, b, 100, 8);
    give(allocator, c, 4000, 8);
}

static void test_lanes(void) {
    ta_test_allocator heap = { 0 };
    ta_allocator plain = ta_test_allocator_get(&heap);
    ta_allocator definitions = ta_test_allocator_lane(&heap, 1);
    ta_allocator sessions = ta_test_allocator_lane(&heap, 2);
    CHECK(ta_test_allocator_lane(&heap, 0).self == plain.self);
    CHECK(ta_test_allocator_lane(&heap, TA_TEST_ALLOCATOR_CATEGORIES).self == plain.self);
    void *p = take(plain, 10, 8);
    void *d = take(definitions, 20, 8);
    void *s = take(sessions, 30, 8);
    CHECK(heap.by_category[0].live_bytes == 10 && heap.by_category[1].live_bytes == 20 &&
          heap.by_category[2].live_bytes == 30);
    CHECK(heap.stats.live_bytes == 60 && heap.stats.live_blocks == 3);
    /* A failure policy counts across lanes. */
    ta_test_allocator_fail_nth(&heap, 2);
    void *first = take(definitions, 1, 1);
    CHECK(first != NULL);
    CHECK(take(sessions, 1, 1) == NULL);
    CHECK(heap.by_category[2].refused == 1 && heap.by_category[1].refused == 0 && heap.stats.refused == 1);
    /* A block leaves the category it was taken in, whichever lane frees it. */
    give(plain, d, 20, 8);
    CHECK(heap.by_category[1].live_bytes == 1 && heap.by_category[0].live_bytes == 10);
    give(plain, p, 10, 8);
    give(plain, s, 30, 8);
    give(plain, first, 1, 1);
    uint64_t attempts = 0;
    uint64_t allocations = 0;
    uint64_t frees = 0;
    for (uint32_t category = 0; category < TA_TEST_ALLOCATOR_CATEGORIES; category += 1u) {
        attempts += heap.by_category[category].attempts;
        allocations += heap.by_category[category].allocations;
        frees += heap.by_category[category].frees;
        CHECK(heap.by_category[category].live_blocks == 0 && heap.by_category[category].live_bytes == 0);
    }
    CHECK(attempts == heap.stats.attempts && allocations == heap.stats.allocations && frees == heap.stats.frees);
    CHECK(ta_test_allocator_finish(&heap) == 0);
}

static void test_memory(void) {
    ta_test_allocator heap = { .zero_fill = true };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    static const size_t alignments[] = { 1, 8, 16, 64, 4096 };
    for (size_t index = 0; index < sizeof(alignments) / sizeof(alignments[0]); index += 1u) {
        size_t alignment = alignments[index];
        unsigned char *block = take(allocator, 200, alignment);
        CHECK(block != NULL);
        if (block == NULL) {
            continue;
        }
        CHECK((uintptr_t)block % alignment == 0);
        bool zeroed = true;
        for (size_t offset = 0; offset < 200; offset += 1u) {
            zeroed = zeroed && block[offset] == 0;
            block[offset] = 0xa5; /* the whole block is writable */
        }
        CHECK(zeroed);
        give(allocator, block, 200, alignment);
    }
    CHECK(take(allocator, 8, 24) == NULL); /* not a power of two */
    CHECK(take(allocator, SIZE_MAX, 8) == NULL); /* the sum overflows: refused, not wrapped */
    CHECK(take(allocator, SIZE_MAX - 8, 8) == NULL);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.refused == 3);
}

static void test_strict(void) {
    ta_test_allocator heap = { .strict = true };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    void *block = take(allocator, 50, 16);
    CHECK(block != NULL);
    give(allocator, block, 49, 16);           /* the wrong size */
    CHECK(heap.stats.bad_frees == 1 && heap.stats.live_blocks == 0);
    block = take(allocator, 50, 16);
    give(allocator, block, 50, 8);            /* the wrong alignment */
    CHECK(heap.stats.bad_frees == 2 && heap.stats.live_blocks == 0);
    block = take(allocator, 50, 16);
    give(allocator, block, 50, 16);           /* the right facts */
    CHECK(heap.stats.bad_frees == 2 && heap.stats.live_blocks == 0);
    char local = 0;
    give(allocator, &local, 1, 1);            /* a pointer never handed out */
    CHECK(heap.stats.bad_frees == 3);
    give(allocator, block, 50, 16);           /* and one already given back */
    CHECK(heap.stats.bad_frees == 4 && heap.stats.frees == 3);
}

int main(void) {
    test_counters();
    test_reset_counts();
    test_fail_nth();
    test_fail_from_nth();
    test_refuse_policies();
    test_large_threshold();
    test_lanes();
    test_memory();
    test_strict();
    if (failures != 0) {
        fprintf(stderr, "test allocator test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core test allocator test passed");
    return 0;
}
