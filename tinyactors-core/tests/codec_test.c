/*
 * Chart codecs (ADR 0053): the arena allocator a decode builds on.
 *
 * The test includes the amalgamation, as export_test does, so it can look at
 * a compiled definition before registration; it uses the C library, the core
 * does not.
 */
#include "../include/tinyactors.c"

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

/* ------------------------------------------------------------------------
 * Host fixtures: a counting libc allocator.
 */

static size_t live_blocks = 0;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    void *block = aligned_alloc(alignment, size == 0 ? alignment : size);
    live_blocks += block != NULL ? 1u : 0u;
    return block;
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) {
        live_blocks -= 1u;
    }
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * ta_arena_allocator: an arena behind the ta_allocator interface.
 */

static void test_arena_allocator(void) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 64) == TA_OK);
    ta_allocator allocator = ta_arena_allocator(&arena);
    CHECK(allocator.v != NULL && allocator.v->alloc != NULL && allocator.v->free != NULL);

    /* Allocations are aligned, distinct, and outlive free(). */
    uint64_t *first = allocator.v->alloc(allocator.self, sizeof(uint64_t), _Alignof(uint64_t));
    char *second = allocator.v->alloc(allocator.self, 200, 1);
    CHECK(first != NULL && second != NULL);
    CHECK(((uintptr_t)first % _Alignof(uint64_t)) == 0);
    *first = 42;
    memset(second, 'x', 200);
    allocator.v->free(allocator.self, &(ta_allocator_free_params){ .ptr = first, .size = sizeof(uint64_t),
                                                                     .alignment = _Alignof(uint64_t) });
    CHECK(*first == 42);
    CHECK(second[199] == 'x');

    /* Everything goes back to the backing allocator with the arena. */
    CHECK(live_blocks > 0);
    ta_arena_destroy(&arena);
    CHECK(live_blocks == 0);
}

int main(void) {
    test_arena_allocator();
    if (failures != 0) {
        fprintf(stderr, "codec test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core codec test passed");
    return 0;
}
