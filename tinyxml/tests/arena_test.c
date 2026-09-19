/*
 * txml_arena: the allocation substrate everything else depends on.
 *
 * The properties under test are the ones the rest of the design leans on
 * (ADR 0002): an address never changes once handed out, an allocation is
 * always contiguous, a mark releases everything newer and resumes bump
 * allocation there, and a refusal from the backing allocator is a clean NULL
 * rather than a corrupted arena.
 *
 * Test programs may use the C library. The library itself may not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/txml.h"

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------------
 * A counting allocator: outstanding bytes, call counts, and a fail-after-N
 * mode. It also verifies that free() is handed back the exact size and
 * alignment that alloc() was asked for, which is the contract that lets an
 * embedder avoid block headers of its own.
 */

#define MAX_BLOCKS 4096

typedef struct {
    void *ptr;
    size_t size;
    size_t alignment;
} block;

typedef struct {
    block blocks[MAX_BLOCKS];
    size_t live;
    size_t outstanding;  /* bytes currently held by the arena */
    size_t peak;         /* high-water mark of `outstanding` */
    size_t attempts;     /* alloc() calls, refused or not */
    size_t allocations;  /* alloc() calls that returned memory */
    size_t frees;
    long fail_from;      /* refuse once `attempts` reaches this; -1 never */
} counting_allocator;

static void counting_reset(counting_allocator *state) {
    memset(state, 0, sizeof(*state));
    state->fail_from = -1;
}

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *state = (counting_allocator *)self;
    state->attempts += 1;

    if (state->fail_from >= 0 && (long)state->attempts > state->fail_from) {
        return NULL;
    }
    if (state->live == MAX_BLOCKS) {
        return NULL;
    }

    size_t actual = alignment < sizeof(void *) ? sizeof(void *) : alignment;
    size_t rounded = (size + actual - 1u) / actual * actual;
    void *ptr = aligned_alloc(actual, rounded == 0u ? actual : rounded);
    if (ptr == NULL) {
        return NULL;
    }

    state->blocks[state->live].ptr = ptr;
    state->blocks[state->live].size = size;
    state->blocks[state->live].alignment = alignment;
    state->live += 1;
    state->allocations += 1;
    state->outstanding += size;
    if (state->outstanding > state->peak) {
        state->peak = state->outstanding;
    }
    return ptr;
}

static void counting_free(void *self, const txml_allocator_free_params *params) {
    counting_allocator *state = (counting_allocator *)self;
    state->frees += 1;

    for (size_t i = 0; i < state->live; i++) {
        if (state->blocks[i].ptr != params->ptr) {
            continue;
        }
        /* free() must be told exactly what alloc() was asked for. */
        CHECK(state->blocks[i].size == params->size);
        CHECK(state->blocks[i].alignment == params->alignment);
        state->outstanding -= state->blocks[i].size;
        free(state->blocks[i].ptr);
        state->blocks[i] = state->blocks[state->live - 1u];
        state->live -= 1;
        return;
    }

    fprintf(stderr, "arena_test: free of a pointer this allocator never returned\n");
    failures += 1;
}

static const txml_allocator_vtable counting_vtable = {
    .alloc = counting_alloc,
    .free = counting_free,
};

static txml_allocator counting_allocator_of(counting_allocator *state) {
    txml_allocator allocator = { .self = state, .v = &counting_vtable };
    return allocator;
}

static bool aligned(const void *ptr, size_t alignment) {
    return ((uintptr_t)ptr % (uintptr_t)alignment) == 0u;
}

/* ------------------------------------------------------------------------
 * Initialization and simple allocation.
 */

static void test_init_and_alloc(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 0) == TXML_OK);
    /* A zero chunk size selects the documented default. */
    CHECK(arena.chunk_size == TXML_ARENA_CHUNK_DEFAULT);
    CHECK(arena.used == 0u);

    txml_arena_destroy(&arena);

    counting_reset(&state);
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 4096u) == TXML_OK);
    CHECK(arena.chunk_size == 4096u);

    const size_t alignments[] = { 1u, 2u, 4u, 8u, 16u, 32u, 64u };
    for (size_t i = 0; i < sizeof(alignments) / sizeof(alignments[0]); i++) {
        void *ptr = txml_arena_alloc(&arena, 24u, alignments[i]);
        CHECK(ptr != NULL);
        CHECK(aligned(ptr, alignments[i]));
        memset(ptr, 0x5a, 24u);
    }

    CHECK(arena.used >= 7u * 24u);
    CHECK(arena.reserved >= arena.used);

    /* Small allocations come out of the chunk already held: the backing
     * allocator is not consulted per allocation. */
    size_t before = state.attempts;
    for (int i = 0; i < 16; i++) {
        CHECK(txml_arena_alloc(&arena, 8u, 8u) != NULL);
    }
    CHECK(state.attempts == before);

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
}

/* ------------------------------------------------------------------------
 * Addresses never change, however much the arena grows afterwards.
 */

static void test_addresses_are_stable(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 1024u) == TXML_OK);

    enum { EARLY = 64, EARLY_SIZE = 100 };
    unsigned char *early[EARLY];

    for (int i = 0; i < EARLY; i++) {
        early[i] = (unsigned char *)txml_arena_alloc(&arena, EARLY_SIZE, 8u);
        CHECK(early[i] != NULL);
        memset(early[i], (unsigned char)(i + 1), EARLY_SIZE);
    }

    /* Force many more chunks. */
    for (int i = 0; i < 200; i++) {
        void *ptr = txml_arena_alloc(&arena, 700u, 16u);
        CHECK(ptr != NULL);
        memset(ptr, 0xcc, 700u);
    }

    for (int i = 0; i < EARLY; i++) {
        for (int byte = 0; byte < EARLY_SIZE; byte++) {
            if (early[i][byte] != (unsigned char)(i + 1)) {
                fprintf(stderr, "arena_test: block %d byte %d moved or was overwritten\n", i, byte);
                failures += 1;
                break;
            }
        }
    }

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
}

/* ------------------------------------------------------------------------
 * A request larger than the chunk size is still one contiguous block.
 */

static void test_oversized_allocation_is_contiguous(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 1024u) == TXML_OK);

    CHECK(txml_arena_alloc(&arena, 64u, 8u) != NULL);

    enum { BIG = 8192 };
    unsigned char *big = (unsigned char *)txml_arena_alloc(&arena, BIG, 16u);
    CHECK(big != NULL);
    CHECK(aligned(big, 16u));

    for (int i = 0; i < BIG; i++) {
        big[i] = (unsigned char)(i & 0xff);
    }
    for (int i = 0; i < BIG; i++) {
        if (big[i] != (unsigned char)(i & 0xff)) {
            fprintf(stderr, "arena_test: oversized block is not contiguous at %d\n", i);
            failures += 1;
            break;
        }
    }

    /* Small allocations still work after the oversized one. */
    CHECK(txml_arena_alloc(&arena, 32u, 8u) != NULL);

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
}

/* ------------------------------------------------------------------------
 * Mark, release, and reuse. This is what txml_doc_rewind() is built on.
 */

static void test_mark_and_release(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 1024u) == TXML_OK);

    unsigned char *kept = (unsigned char *)txml_arena_alloc(&arena, 128u, 8u);
    CHECK(kept != NULL);
    memset(kept, 0x11, 128u);

    const txml_arena_mark mark = txml_arena_position(&arena);
    const size_t used_at_mark = arena.used;
    const size_t outstanding_at_mark = state.outstanding;

    void *first_after_mark = txml_arena_alloc(&arena, 64u, 8u);
    CHECK(first_after_mark != NULL);

    /* Enough to need several new chunks, so "chunks were returned" is
     * observable even though one spare is retained. */
    for (int i = 0; i < 64; i++) {
        CHECK(txml_arena_alloc(&arena, 500u, 8u) != NULL);
    }
    CHECK(state.outstanding > outstanding_at_mark);

    txml_arena_release(&arena, mark);

    CHECK(arena.used == used_at_mark);
    CHECK(state.outstanding < state.peak);
    /* Everything before the mark survives untouched. */
    for (int i = 0; i < 128; i++) {
        if (kept[i] != 0x11) {
            fprintf(stderr, "arena_test: release damaged memory older than the mark\n");
            failures += 1;
            break;
        }
    }

    /* Bump allocation resumes at the mark: the same request returns the same
     * address it did the first time. */
    void *again = txml_arena_alloc(&arena, 64u, 8u);
    CHECK(again == first_after_mark);

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
    CHECK(state.allocations == state.frees);
}

/* ------------------------------------------------------------------------
 * Reset empties the arena but leaves it usable; destroy returns everything.
 */

static void test_reset_and_destroy(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 1024u) == TXML_OK);

    for (int i = 0; i < 32; i++) {
        CHECK(txml_arena_alloc(&arena, 200u, 8u) != NULL);
    }
    CHECK(state.outstanding > 0u);

    txml_arena_reset(&arena);
    CHECK(arena.used == 0u);
    CHECK(state.outstanding == 0u);
    CHECK(state.allocations == state.frees);

    /* Usable again. */
    void *ptr = txml_arena_alloc(&arena, 64u, 16u);
    CHECK(ptr != NULL);
    CHECK(aligned(ptr, 16u));
    memset(ptr, 0x22, 64u);

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
    CHECK(state.allocations == state.frees);
    CHECK(arena.head == NULL);
}

/* ------------------------------------------------------------------------
 * A refusal from the backing allocator is a NULL, not a broken arena.
 */

static void test_allocation_failure(void) {
    counting_allocator state;
    counting_reset(&state);

    txml_arena arena;
    CHECK(txml_arena_init(&arena, counting_allocator_of(&state), 4096u) == TXML_OK);

    void *before = txml_arena_alloc(&arena, 64u, 8u);
    CHECK(before != NULL);
    memset(before, 0x33, 64u);
    const size_t used_before = arena.used;

    /* Refuse every further request, then ask for something that cannot fit in
     * the current chunk. */
    state.fail_from = (long)state.attempts;
    CHECK(txml_arena_alloc(&arena, 1u << 20, 16u) == NULL);
    CHECK(arena.used == used_before);

    /* The arena is intact: the earlier block is untouched and allocation
     * resumes once the allocator cooperates again. */
    for (int i = 0; i < 64; i++) {
        if (((unsigned char *)before)[i] != 0x33) {
            fprintf(stderr, "arena_test: a refused allocation damaged the arena\n");
            failures += 1;
            break;
        }
    }

    state.fail_from = -1;
    void *after = txml_arena_alloc(&arena, 32u, 8u);
    CHECK(after != NULL);
    CHECK(after != before);
    CHECK(arena.used > used_before);

    txml_arena_destroy(&arena);
    CHECK(state.outstanding == 0u);
}

int main(void) {
    test_init_and_alloc();
    test_addresses_are_stable();
    test_oversized_allocation_is_contiguous();
    test_mark_and_release();
    test_reset_and_destroy();
    test_allocation_failure();

    if (failures != 0) {
        fprintf(stderr, "arena_test: %d check(s) failed\n", failures);
        return 1;
    }

    printf("arena_test: ok\n");
    return 0;
}
