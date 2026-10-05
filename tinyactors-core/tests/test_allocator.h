/**
 * @file test_allocator.h
 *
 * # Test allocator
 *
 * The `ta_allocator` core's test programs inject: a failing, counting
 * allocator over the C library. It refuses allocations on a policy the
 * test sets (from the Nth attempt on, one chosen attempt, every attempt,
 * or anything larger than a size), counts what it is asked for and what it
 * hands out, and remembers every live block, so a test can prove that a
 * failed call changed nothing and leaked nothing and that every block came
 * back with the size and alignment it was allocated with.
 *
 * It lives with the tests and is never compiled into the library. Every
 * test program is one translation unit, so the helper is a header of
 * static functions; it uses the C library, which tests may and core may
 * not. Include it after `include/tinyactors.h` or the amalgamation, or on
 * its own: it includes the public header itself.
 *
 * ## Use
 *
 * ```c
 * test_allocator heap = { .policy = { .largest = 4096 } };
 * ta_allocator allocator = test_allocator_of(&heap);
 * ...
 * heap.policy.fail_from = heap.attempts + 1u; // every later attempt fails
 * ...
 * CHECK(test_allocator_finish(&heap) == 0);  // nothing leaked, no bad free
 * ```
 *
 * A test whose allocator lives at file scope names the vtable in a
 * constant initializer instead of calling `test_allocator_of`:
 *
 * ```c
 * static test_allocator heap;
 * static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };
 * ```
 *
 * A zeroed `test_allocator` is ready and refuses nothing. The policy and
 * the counters are plain fields: a test sets the policy at any point
 * (between two calls into core, or from inside a callback core makes),
 * reads the counters, and may reset a counter to zero to count from there.
 *
 * ## Contract
 *
 * - Every call to `alloc` is an *attempt*. It counts in `attempts`,
 *   records its size and alignment in `last_size` and `last_alignment`
 *   and its size in `requested` (for the first
 *   `TEST_ALLOCATOR_REQUESTS` attempts since `attempts` was zero), and
 *   raises `largest_request`, whether or not it is refused.
 * - An attempt is refused, answering NULL, when the policy says so (see
 *   `test_allocator_policy`). Otherwise it is served: a block of at least
 *   `size` bytes aligned to `alignment` (at least to a pointer), filled
 *   with `TEST_ALLOCATOR_FRESH` so a read of memory core did not write is
 *   visible. A size of 0 is served with a block of `alignment` bytes. A
 *   size the C library cannot serve answers NULL as well, without
 *   counting as served.
 * - A served attempt counts in `allocations`, in `live` and, by its
 *   requested size, in `live_bytes`; when it is at least `policy.large`
 *   bytes, it counts in `large_allocations` too.
 * - Every call to `free` counts in `frees`. Freeing NULL does nothing
 *   else. Freeing a live block with the size and alignment it was
 *   allocated with fills it with `TEST_ALLOCATOR_FREED`, so a later read
 *   of it is visible, and returns it to the C library. Freeing a block
 *   with another size or alignment, or a pointer this allocator does not
 *   hold, is a *bad free*: it is reported on `stderr` and counted in
 *   `bad_frees`. A mismatched block is still returned; an unknown pointer
 *   is left alone.
 * - `test_allocator_finish` ends the allocator's life: it reports every
 *   block still live and every bad free, returns the live blocks and its
 *   own bookkeeping to the C library, and answers how many problems it
 *   found. Every `test_allocator` ends with it.
 *
 * ## Memory
 *
 * The `test_allocator` itself belongs to the test, which may keep it on
 * the stack, in a fixture or in a static. The blocks it serves come from
 * `aligned_alloc`; the caller owns each one until it frees it through
 * the allocator. To remember live blocks it keeps a hash table of them,
 * keyed by address, from `calloc`; it doubles the table when it would be
 * more than half full and frees it in `test_allocator_finish`. If the C
 * library cannot grow that table, the test cannot go on: the helper
 * reports it and aborts the program. That is the only way the allocator
 * itself allocates; nothing it does is subject to its policy.
 *
 * ## Complexity
 *
 * An attempt and a free are O(1) expected, amortized over the table's
 * growth, plus the time to fill the block. `test_allocator_finish` is
 * O(table size).
 *
 * ## Failure
 *
 * A refused attempt answers NULL, which core turns into `TA_ERR_NOMEM` or
 * `TA_OP_NOMEM`. A bad free never fails the call; it counts against
 * `test_allocator_finish`'s answer. The fill values make a read of fresh
 * or freed memory wrong rather than lucky; AddressSanitizer
 * (`mise run asan`) catches the read itself.
 *
 * ## Users
 *
 * Core's test programs: every out-of-memory sweep (over compilation,
 * creation, registration, spawn, load and the internal tables), and every
 * test that proves a call allocated nothing, freed everything, or asked
 * for the size it should.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"

/** How many attempts' sizes `requested` keeps. */
#define TEST_ALLOCATOR_REQUESTS 8u
/** The byte every served block is filled with. */
#define TEST_ALLOCATOR_FRESH 0xA5
/** The byte every block is filled with as it is freed. */
#define TEST_ALLOCATOR_FREED 0x5A

/**
 * When the allocator refuses. Each field at zero refuses nothing; an
 * attempt is refused when any field that is set says so.
 */
typedef struct test_allocator_policy {
    /** The attempt, counting from 1, from which every attempt is refused. */
    uint32_t fail_from;
    /** The one attempt, counting from 1, that is refused. */
    uint32_t fail_at;
    /** Refuse every attempt. */
    bool refuse;
    /** Refuse any attempt of more than this many bytes. */
    size_t largest;
    /** Count served attempts of at least this many bytes in `large_allocations`. */
    size_t large;
} test_allocator_policy;

/** A block the allocator handed out and has not had back. */
typedef struct test_allocator_block {
    void *ptr;
    size_t size;
    size_t alignment;
} test_allocator_block;

/** A failing, counting allocator. Zeroed, it is ready and refuses nothing. */
typedef struct test_allocator {
    test_allocator_policy policy;

    /** Calls to `alloc`, refused or served. */
    uint32_t attempts;
    /** Blocks served. */
    uint32_t allocations;
    /** Served blocks of at least `policy.large` bytes. */
    uint32_t large_allocations;
    /** Calls to `free`, NULL included. */
    uint32_t frees;
    /** Blocks served and not yet freed. */
    uint32_t live;
    /** Their requested sizes, summed. */
    size_t live_bytes;
    /** The largest size an attempt asked for. */
    size_t largest_request;
    /** The size and alignment the last attempt asked for. */
    size_t last_size;
    size_t last_alignment;
    /** The sizes the first attempts asked for, since `attempts` was zero. */
    size_t requested[TEST_ALLOCATOR_REQUESTS];
    /** Frees of an unknown pointer, or with the wrong size or alignment. */
    uint32_t bad_frees;

    /**
     * The live blocks: an open-addressed hash table of `capacity` slots (0
     * or a power of two), keyed by `ptr`, an empty slot's `ptr` NULL, at
     * most half full. Owned by the allocator.
     */
    test_allocator_block *blocks;
    uint32_t capacity;
} test_allocator;

/** Whether the policy refuses the attempt just counted, of `size` bytes. */
static inline bool test_allocator_refuses(const test_allocator *heap, size_t size) {
    const test_allocator_policy *policy = &heap->policy;
    return policy->refuse || (policy->fail_from != 0 && heap->attempts >= policy->fail_from) ||
           (policy->fail_at != 0 && heap->attempts == policy->fail_at) ||
           (policy->largest != 0 && size > policy->largest);
}

/** The slot where a search for `ptr` starts. */
static inline uint32_t test_allocator_home(const test_allocator *heap, const void *ptr) {
    uint64_t hash = (uint64_t)(uintptr_t)ptr * UINT64_C(0x9E3779B97F4A7C15);
    return (uint32_t)(hash >> 32) & (heap->capacity - 1u);
}

/** Puts `block` in the first empty slot from its home. The table has room. */
static inline void test_allocator_place(test_allocator *heap, test_allocator_block block) {
    uint32_t slot = test_allocator_home(heap, block.ptr);
    while (heap->blocks[slot].ptr != NULL) {
        slot = (slot + 1u) & (heap->capacity - 1u);
    }
    heap->blocks[slot] = block;
}

/** Makes room for one more live block, doubling the table, or aborts the test. */
static inline void test_allocator_reserve(test_allocator *heap) {
    if ((heap->live + 1u) * 2u <= heap->capacity) {
        return;
    }
    uint32_t capacity = heap->capacity == 0 ? 64u : heap->capacity * 2u;
    test_allocator_block *blocks = calloc(capacity, sizeof(*blocks));
    if (blocks == NULL) {
        fprintf(stderr, "test allocator: cannot track %u live blocks\n", heap->live + 1u);
        abort();
    }
    test_allocator_block *old = heap->blocks;
    uint32_t old_capacity = heap->capacity;
    heap->blocks = blocks;
    heap->capacity = capacity;
    for (uint32_t slot = 0; slot < old_capacity; slot += 1u) {
        if (old[slot].ptr != NULL) {
            test_allocator_place(heap, old[slot]);
        }
    }
    free(old);
}

/** The slot holding `ptr`, or `capacity` when no live block is at `ptr`. */
static inline uint32_t test_allocator_find(const test_allocator *heap, const void *ptr) {
    if (heap->capacity == 0) {
        return heap->capacity;
    }
    uint32_t mask = heap->capacity - 1u;
    for (uint32_t slot = test_allocator_home(heap, ptr); heap->blocks[slot].ptr != NULL; slot = (slot + 1u) & mask) {
        if (heap->blocks[slot].ptr == ptr) {
            return slot;
        }
    }
    return heap->capacity;
}

/**
 * Empties `slot`, shifting back the blocks after it that it would hide
 * from a search (backward-shift deletion), so the table needs no
 * tombstones.
 */
static inline void test_allocator_remove(test_allocator *heap, uint32_t slot) {
    uint32_t mask = heap->capacity - 1u;
    uint32_t hole = slot;
    for (uint32_t next = (hole + 1u) & mask; heap->blocks[next].ptr != NULL; next = (next + 1u) & mask) {
        uint32_t home = test_allocator_home(heap, heap->blocks[next].ptr);
        /* The block at `next` may fill the hole when the hole lies between its home and it. */
        if (((next - home) & mask) >= ((next - hole) & mask)) {
            heap->blocks[hole] = heap->blocks[next];
            hole = next;
        }
    }
    heap->blocks[hole] = (test_allocator_block){0};
}

/** `size` rounded up to a non-zero multiple of `alignment`. */
static inline size_t test_allocator_rounded(size_t size, size_t alignment) {
    return size == 0 ? alignment : (size + alignment - 1u) / alignment * alignment;
}

/** The fallback alignment: at least a pointer's, as `aligned_alloc` needs. */
static inline size_t test_allocator_alignment(size_t alignment) {
    return alignment < sizeof(void *) ? sizeof(void *) : alignment;
}

/**
 * The vtable's `alloc`: counts the attempt, applies the policy and serves
 * a fresh-filled block, or answers NULL. See the file comment.
 */
static inline void *test_allocator_alloc(void *self, size_t size, size_t alignment) {
    test_allocator *heap = self;
    if (heap->attempts < TEST_ALLOCATOR_REQUESTS) {
        heap->requested[heap->attempts] = size;
    }
    heap->attempts += 1u;
    heap->last_size = size;
    heap->last_alignment = alignment;
    heap->largest_request = size > heap->largest_request ? size : heap->largest_request;
    if (test_allocator_refuses(heap, size)) {
        return NULL;
    }
    size_t align = test_allocator_alignment(alignment);
    if (size > SIZE_MAX - align) {
        return NULL; /* no block that large exists; the rounding would wrap */
    }
    test_allocator_reserve(heap);
    size_t rounded = test_allocator_rounded(size, align);
    void *ptr = aligned_alloc(align, rounded);
    if (ptr == NULL) {
        return NULL;
    }
    memset(ptr, TEST_ALLOCATOR_FRESH, rounded);
    test_allocator_place(heap, (test_allocator_block){ .ptr = ptr, .size = size, .alignment = alignment });
    heap->live += 1u;
    heap->live_bytes += size;
    heap->allocations += 1u;
    if (heap->policy.large != 0 && size >= heap->policy.large) {
        heap->large_allocations += 1u;
    }
    return ptr;
}

/**
 * The vtable's `free`: takes back a live block, checking the size and
 * alignment it is given; reports a bad free. See the file comment.
 */
static inline void test_allocator_free(void *self, const ta_allocator_free_params *params) {
    test_allocator *heap = self;
    heap->frees += 1u;
    if (params->ptr == NULL) {
        return;
    }
    uint32_t slot = test_allocator_find(heap, params->ptr);
    if (slot == heap->capacity) {
        fprintf(stderr, "test allocator: free of %p, which it does not hold\n", params->ptr);
        heap->bad_frees += 1u;
        return;
    }
    test_allocator_block block = heap->blocks[slot];
    if (block.size != params->size || block.alignment != params->alignment) {
        fprintf(stderr, "test allocator: free of %p as %zu bytes aligned to %zu, allocated as %zu aligned to %zu\n",
                params->ptr, params->size, params->alignment, block.size, block.alignment);
        heap->bad_frees += 1u;
    }
    test_allocator_remove(heap, slot);
    heap->live -= 1u;
    heap->live_bytes -= block.size;
    memset(block.ptr, TEST_ALLOCATOR_FREED,
           test_allocator_rounded(block.size, test_allocator_alignment(block.alignment)));
    free(block.ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_allocator_alloc,
                                                           .free = test_allocator_free };

/** The `ta_allocator` over `heap`. It borrows `heap`, which must outlive every block. */
static inline ta_allocator test_allocator_of(test_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &test_allocator_vtable };
}

/**
 * Ends `heap`'s life: reports on `stderr` every block still live and the
 * bad frees counted, returns the live blocks and the allocator's table to
 * the C library, and answers the number of live blocks plus bad frees, 0
 * when everything came back as it should. Afterwards `heap` is zeroed
 * except for its policy, and may be used again.
 */
static inline uint32_t test_allocator_finish(test_allocator *heap) {
    for (uint32_t slot = 0; slot < heap->capacity; slot += 1u) {
        const test_allocator_block *block = &heap->blocks[slot];
        if (block->ptr == NULL) {
            continue;
        }
        fprintf(stderr, "test allocator: %zu bytes aligned to %zu still live at %p\n", block->size,
                block->alignment, block->ptr);
        free(block->ptr);
    }
    if (heap->bad_frees != 0) {
        fprintf(stderr, "test allocator: %u bad frees\n", heap->bad_frees);
    }
    uint32_t problems = heap->live + heap->bad_frees;
    free(heap->blocks);
    *heap = (test_allocator){ .policy = heap->policy };
    return problems;
}
