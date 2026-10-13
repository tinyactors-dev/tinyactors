/* Small allocation fixture for base storage tests. Refusal is switchable;
 * each successful block remembers its exact size, so frees prove ownership
 * and accounting as well as absence of leaked chunks. */
#ifndef TA_TEST_COUNTING_ALLOCATOR_H
#define TA_TEST_COUNTING_ALLOCATOR_H
#include <stdlib.h>
#include <string.h>
#include <tinyactors/allocator.h>
#include "../harness/harness.h"

typedef struct counting_allocator {
    ta_allocator api;
    uint32_t attempts, allocations, frees;
    size_t live_bytes;
    int refuse;
} counting_allocator;

typedef union counted_block {
    uint64_t alignment;
    size_t bytes;
} counted_block;

static void *counted_alloc(void *context, size_t size, size_t align) {
    counting_allocator *state = context;
    ++state->attempts;
    CHECK(align <= 8);
    if (state->refuse || size > SIZE_MAX - sizeof(counted_block)) return NULL;
    counted_block *block = malloc(sizeof *block + size);
    if (!block) return NULL;
    block->bytes = size;
    ++state->allocations;
    state->live_bytes += size;
    return block + 1;
}

static void counted_free(void *context, void *pointer, size_t size) {
    counting_allocator *state = context;
    counted_block *block = (counted_block *)pointer - 1;
    CHECK_EQUAL(block->bytes, size);
    ++state->frees;
    state->live_bytes -= size;
    free(block);
}

static void counting_init(counting_allocator *state) {
    memset(state, 0, sizeof *state);
    state->api = (ta_allocator){state, counted_alloc, counted_free};
}
#endif
