/*
 * The test allocator (tests/test_allocator.h): the failing, counting
 * `ta_allocator` core's other test programs inject. Their verdicts on
 * out-of-memory paths, leaks and frees rest on it, so its own contract is
 * proven here, through the vtable as core calls it.
 *
 * The test needs only the public header and the helper; it uses the C
 * library, the core does not. The bad frees below are deliberate, and the
 * helper reports each of them on stderr.
 *
 * Claims proven here, in order:
 *
 * 1. a zeroed allocator refuses nothing: it serves blocks aligned to every
 *    alignment from 1 to 4096, at least to a pointer, filled with
 *    TEST_ALLOCATOR_FRESH, a size of 0 included, and counts each in
 *    attempts, allocations, live and live_bytes;
 * 2. every attempt, refused or served, sets last_size and last_alignment,
 *    raises largest_request, and records its size in requested for the
 *    first TEST_ALLOCATOR_REQUESTS attempts since attempts was zero;
 * 3. fail_from refuses that attempt and every later one; fail_at refuses
 *    that one attempt only; refuse refuses every attempt while set; each
 *    refused attempt counts in attempts and changes no other counter
 *    except the request facts of claim 2;
 * 4. largest refuses an attempt of more than that many bytes and serves
 *    one of exactly that many; large counts served attempts of at least
 *    that many bytes in large_allocations, and a refused one not;
 * 5. a size no block can have (SIZE_MAX) answers NULL without wrapping
 *    the rounding, counts as an attempt and is not served;
 * 6. free counts every call in frees; NULL changes nothing else; a block
 *    freed with its size and alignment leaves live and live_bytes, and
 *    finish then answers 0; thousands of blocks freed in a scrambled order
 *    are each found, however the table grew and shifted;
 * 7. a free with the wrong size or the wrong alignment is a bad free: the
 *    block still leaves live; a free of a pointer the allocator does not
 *    hold is a bad free and changes nothing else; finish answers the bad
 *    frees;
 * 8. finish answers live blocks plus bad frees, returns the live blocks
 *    (AddressSanitizer proves none leaks), zeroes every counter, keeps
 *    the policy, and leaves an allocator that serves again.
 */
#include "../include/tinyactors.h"
#include "test_allocator.h"

#include <stdint.h>
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

/* An allocation through the vtable, as `ta_mem_alloc` makes it. */
static void *take(ta_allocator allocator, size_t size, size_t alignment) {
    return allocator.v->alloc(allocator.self, size, alignment);
}

/* A free through the vtable, as `ta_mem_free` makes it. */
static void give(ta_allocator allocator, void *ptr, size_t size, size_t alignment) {
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = ptr, .size = size, .alignment = alignment });
}

/* Whether all `size` bytes at `ptr` are `value`. */
static bool filled(const void *ptr, size_t size, unsigned char value) {
    const unsigned char *bytes = ptr;
    for (size_t index = 0; index < size; index += 1u) {
        if (bytes[index] != value) {
            return false;
        }
    }
    return true;
}

/* Claim 1 */
static void test_serves_aligned_fresh_blocks(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    void *blocks[13];
    size_t bytes = 0;
    uint32_t count = 0;
    for (size_t alignment = 1; alignment <= 4096u; alignment *= 2u) {
        size_t size = 3u * alignment + 1u;
        void *ptr = take(allocator, size, alignment);
        CHECK(ptr != NULL);
        CHECK((uintptr_t)ptr % alignment == 0 && (uintptr_t)ptr % sizeof(void *) == 0);
        CHECK(filled(ptr, size, TEST_ALLOCATOR_FRESH));
        blocks[count] = ptr;
        count += 1u;
        bytes += size;
        CHECK(heap.attempts == count && heap.allocations == count && heap.live == count);
        CHECK(heap.live_bytes == bytes);
    }
    void *empty = take(allocator, 0, 8);
    CHECK(empty != NULL && (uintptr_t)empty % 8u == 0);
    CHECK(heap.live == count + 1u && heap.live_bytes == bytes);
    give(allocator, empty, 0, 8);

    uint32_t index = 0;
    for (size_t alignment = 1; alignment <= 4096u; alignment *= 2u) {
        give(allocator, blocks[index], 3u * alignment + 1u, alignment);
        index += 1u;
    }
    CHECK(heap.live == 0 && heap.live_bytes == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 2 */
static void test_records_every_request(void) {
    test_allocator heap = { .policy.refuse = true };
    ta_allocator allocator = test_allocator_of(&heap);
    CHECK(take(allocator, 40, 8) == NULL);
    CHECK(heap.last_size == 40 && heap.last_alignment == 8 && heap.largest_request == 40);
    heap.policy.refuse = false;
    void *served = take(allocator, 24, 4);
    CHECK(served != NULL);
    CHECK(heap.last_size == 24 && heap.last_alignment == 4 && heap.largest_request == 40);
    CHECK(heap.requested[0] == 40 && heap.requested[1] == 24);
    give(allocator, served, 24, 4);

    for (uint32_t attempt = 2; attempt < TEST_ALLOCATOR_REQUESTS + 4u; attempt += 1u) {
        give(allocator, take(allocator, attempt * 100u, 1), attempt * 100u, 1);
    }
    CHECK(heap.requested[TEST_ALLOCATOR_REQUESTS - 1u] == (TEST_ALLOCATOR_REQUESTS - 1u) * 100u);
    CHECK(heap.largest_request == (TEST_ALLOCATOR_REQUESTS + 3u) * 100u);

    /* Counting from zero again records from the first entry again. */
    heap.attempts = 0;
    heap.largest_request = 0;
    give(allocator, take(allocator, 7, 1), 7, 1);
    CHECK(heap.requested[0] == 7 && heap.requested[1] == 24 && heap.largest_request == 7);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 3 */
static void test_refusal_policies(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    void *kept = take(allocator, 16, 8);

    heap.policy.fail_from = heap.attempts + 2u;
    void *before = take(allocator, 16, 8);
    CHECK(before != NULL);
    for (uint32_t attempt = 0; attempt < 5u; attempt += 1u) {
        CHECK(take(allocator, 16, 8) == NULL);
    }
    CHECK(heap.attempts == 7 && heap.allocations == 2 && heap.live == 2 && heap.live_bytes == 32);
    heap.policy.fail_from = 0;

    heap.policy.fail_at = heap.attempts + 2u;
    void *first = take(allocator, 16, 8);
    CHECK(first != NULL);
    CHECK(take(allocator, 16, 8) == NULL);
    void *after = take(allocator, 16, 8);
    CHECK(after != NULL);
    CHECK(heap.attempts == 10 && heap.allocations == 4 && heap.live == 4);
    heap.policy.fail_at = 0;

    heap.policy.refuse = true;
    CHECK(take(allocator, 1, 1) == NULL && take(allocator, 1, 1) == NULL);
    CHECK(heap.attempts == 12 && heap.allocations == 4 && heap.live == 4 && heap.frees == 0);
    heap.policy.refuse = false;
    void *resumed = take(allocator, 16, 8);
    CHECK(resumed != NULL && heap.live == 5);

    give(allocator, kept, 16, 8);
    give(allocator, before, 16, 8);
    give(allocator, first, 16, 8);
    give(allocator, after, 16, 8);
    give(allocator, resumed, 16, 8);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 4 */
static void test_sizes(void) {
    test_allocator heap = { .policy = { .largest = 1024, .large = 512 } };
    ta_allocator allocator = test_allocator_of(&heap);
    CHECK(take(allocator, 1025, 8) == NULL);
    CHECK(heap.attempts == 1 && heap.allocations == 0 && heap.large_allocations == 0);
    void *largest = take(allocator, 1024, 8);
    void *large = take(allocator, 512, 8);
    void *small = take(allocator, 511, 8);
    CHECK(largest != NULL && large != NULL && small != NULL);
    CHECK(heap.allocations == 3 && heap.large_allocations == 2);
    give(allocator, largest, 1024, 8);
    give(allocator, large, 512, 8);
    give(allocator, small, 511, 8);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 5 */
static void test_impossible_size(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    CHECK(take(allocator, SIZE_MAX, 16) == NULL);
    CHECK(take(allocator, SIZE_MAX - 4u, 8) == NULL);
    CHECK(heap.attempts == 2 && heap.allocations == 0 && heap.live == 0 && heap.live_bytes == 0);
    CHECK(heap.largest_request == SIZE_MAX);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 6 */
static void test_frees(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    give(allocator, NULL, 8, 8);
    CHECK(heap.frees == 1 && heap.bad_frees == 0 && heap.live == 0);

    enum { COUNT = 5000 };
    static void *blocks[COUNT];
    for (uint32_t index = 0; index < COUNT; index += 1u) {
        blocks[index] = take(allocator, index % 97u + 1u, 8);
        CHECK(blocks[index] != NULL);
    }
    CHECK(heap.live == COUNT);
    /* A scrambled order: index * 7919 mod COUNT visits every index once. */
    for (uint32_t step = 0; step < COUNT; step += 1u) {
        uint32_t index = (uint32_t)(((uint64_t)step * 7919u) % COUNT);
        give(allocator, blocks[index], index % 97u + 1u, 8);
    }
    CHECK(heap.live == 0 && heap.live_bytes == 0 && heap.bad_frees == 0);
    CHECK(heap.frees == COUNT + 1u);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 7 */
static void test_bad_frees(void) {
    test_allocator heap = {0};
    ta_allocator allocator = test_allocator_of(&heap);
    void *a = take(allocator, 32, 8);
    void *b = take(allocator, 32, 16);
    give(allocator, a, 31, 8);
    CHECK(heap.bad_frees == 1 && heap.live == 1 && heap.live_bytes == 32);
    give(allocator, b, 32, 8);
    CHECK(heap.bad_frees == 2 && heap.live == 0 && heap.live_bytes == 0);

    int local = 0;
    void *c = take(allocator, 8, 8);
    give(allocator, &local, 4, 4);
    CHECK(heap.bad_frees == 3 && heap.live == 1 && local == 0);
    give(allocator, c, 8, 8);
    CHECK(test_allocator_finish(&heap) == 3);
}

/* Claim 8 */
static void test_finish(void) {
    test_allocator heap = { .policy.largest = 64 };
    ta_allocator allocator = test_allocator_of(&heap);
    CHECK(take(allocator, 16, 8) != NULL);
    CHECK(take(allocator, 64, 8) != NULL);
    void *wrong = take(allocator, 8, 8);
    give(allocator, wrong, 9, 8);
    CHECK(test_allocator_finish(&heap) == 3);
    CHECK(heap.attempts == 0 && heap.allocations == 0 && heap.live == 0 && heap.live_bytes == 0);
    CHECK(heap.frees == 0 && heap.bad_frees == 0 && heap.blocks == NULL && heap.capacity == 0);
    CHECK(heap.policy.largest == 64);

    CHECK(take(allocator, 65, 8) == NULL);
    void *again = take(allocator, 64, 8);
    CHECK(again != NULL && heap.live == 1);
    give(allocator, again, 64, 8);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_serves_aligned_fresh_blocks();
    test_records_every_request();
    test_refusal_policies();
    test_sizes();
    test_impossible_size();
    test_frees();
    fprintf(stderr, "test allocator test: the bad frees and live blocks reported next are deliberate\n");
    test_bad_frees();
    test_finish();
    if (failures != 0) {
        fprintf(stderr, "test allocator test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core test allocator test passed");
    return 0;
}
