/*
 * Package test_allocator is the failing, counting allocator every core test
 * program shares (building block E2 of docs/proposals/ring-proto).
 *
 * # What it is for
 *
 * Core takes every byte it owns from a ta_allocator the host passes in, so a
 * test of core has to stand in for the host. Three questions come up in
 * nearly every test, and this unit answers them once:
 *
 *   - Did everything come back? After the last ta_*_destroy, live_blocks and
 *     live_bytes are zero, and finish() reports any block that is not.
 *   - What did it cost? Counters say how many allocations an operation made,
 *     how many were at least some size, the largest block, and the peak.
 *   - Does it survive running out of memory? A policy refuses one chosen
 *     allocation, or every allocation from a chosen one on, or every
 *     allocation of at least some size, or all of them. A test sweeps the
 *     policy over every allocation an operation makes and checks that each
 *     failure ends as a clean TA_ERR_NOMEM with nothing leaked (ADR 0047).
 *
 * Before this unit each test program carried its own copy of this: twenty-six
 * copies of 15 to 45 lines that differed in ways that mattered (calloc or
 * aligned_alloc, whether a refused attempt counts, whether the first
 * allocation is number 0 or 1). The policies and counters below are the union
 * of those, and each migrated test maps its old idiom onto one of them.
 *
 * It lives with the tests. It is compiled once (build/objects/test_allocator.o)
 * and linked into each test program; it is never part of the library or the
 * amalgamation, and nothing in src/ or include/ refers to it. It includes only
 * "tinyactors.h", for the ta_allocator types, and calls no ta_* function, so
 * it links beside a test that includes ../include/tinyactors.c itself without
 * colliding with the amalgamation's copies. Unlike core, it uses the C
 * library: aligned_alloc and free.
 *
 * # Lifetime
 *
 * A ta_test_allocator is test infrastructure, not a block of the library, so
 * none of the catalog's lifetimes (system, definition, session, ...) applies
 * to it. It is the outermost owner in a test program: declare one static or
 * on main's stack, before the first ta_system_create (or ta_arena_init,
 * ta_tree_init, ...) that is handed one of its allocators, and let it live
 * until after the last of those has been destroyed. Its storage is the
 * caller's: it never allocates for its own state, and it needs no destroy
 * call. A zero-initialized value is valid and means "libc memory, no policy,
 * not strict". The two modes (zero_fill and strict) are chosen in the
 * initializer and never change: `static ta_test_allocator heap = { .strict =
 * true };`.
 *
 * Every block it hands to the library belongs to whatever library lifetime
 * asked for it. The library gives it back through the allocator's free entry.
 * The helper never frees on the library's behalf: a block the library forgot
 * is reported by ta_test_allocator_finish() and then stays allocated, because
 * freeing it there would hide the bug the test exists to find.
 *
 * An allocator value (a ta_allocator naming the helper) is a plain pair of
 * pointers. It is valid as long as the helper is, may be copied freely, and
 * is the only thing the library should keep.
 *
 * # When it allocates
 *
 * Only inside a call of its alloc entry, from libc aligned_alloc. Each block
 * carries a small header directly in front of the pointer the library sees
 * (the size and alignment asked for, the category, and the links of the live
 * list). A request is refused, never wrapped, when its size plus the header
 * overflows size_t, or when its alignment is not a power of two. A refused
 * request returns NULL and changes nothing but attempts, refused and the
 * per-category copies of them, so a failure path under test finds no partial
 * state in the helper.
 *
 * # Categories
 *
 * The same helper hands out TA_TEST_ALLOCATOR_CATEGORIES lanes. Memory taken
 * through lane k counts in by_category[k] as well as in stats, so a test can
 * give each lifetime its own lane (definition, session, envelope, ...) and
 * assert a budget per lifetime. The meaning of a category number is the
 * test's. The plain allocator is lane 0.
 *
 * # Invariants
 *
 * Between calls, for stats and for every element of by_category:
 *
 *   attempts    == allocations + refused
 *   live_blocks == the number of blocks handed out and not yet freed
 *   live_bytes  == the sum of the sizes asked for by those blocks
 *   peak_bytes  >= live_bytes
 *
 * A block is counted in the category it was allocated in, whichever lane
 * frees it. For each additive counter, the sum over by_category equals the
 * same counter in stats; peak_bytes and largest_block are maxima, not sums.
 * reset_counts restarts the event counters but keeps the two live_ facts, so
 * the invariants hold across it.
 *
 * # Complexity
 *
 * Every call is O(1), with two exceptions that are O(live_blocks): a free in
 * strict mode, which looks the pointer up to prove it was handed out, and
 * finish(), which walks the live list.
 *
 * # Failure behaviour
 *
 * alloc: returns NULL and counts the attempt as refused, for any reason in
 * the policy list or any request it cannot satisfy. free of NULL is ignored,
 * as in libc, and is not counted. In strict mode a free whose pointer was
 * never handed out (or was handed out and freed already), or whose size or
 * alignment differ from what the allocation recorded, writes a line to
 * stderr and counts in bad_frees; the block, if known, is still released.
 * Outside strict mode neither is checked and a foreign pointer is undefined
 * behaviour, as with libc free.
 *
 * # Usage
 *
 * Count what an operation allocates, then fail each of those allocations in
 * turn:
 *
 *     static ta_test_allocator heap;
 *
 *     ta_test_allocator_reset_counts(&heap);
 *     run(ta_test_allocator_get(&heap));
 *     uint64_t used = heap.stats.attempts;
 *
 *     for (uint64_t k = 1; k <= used; k += 1) {
 *         ta_test_allocator_fail_nth(&heap, k);       // refuse the k-th from now, once
 *         CHECK(run(ta_test_allocator_get(&heap)) == TA_ERR_NOMEM);
 *         ta_test_allocator_disarm(&heap);
 *         CHECK(heap.stats.live_blocks == 0);          // and nothing leaked
 *     }
 *     CHECK(ta_test_allocator_finish(&heap) == 0);
 *
 * A test that needs the allocator in a file-scope initializer can write
 *
 *     static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);
 */
#ifndef TA_TEST_ALLOCATOR_H
#define TA_TEST_ALLOCATOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

/* TA_TEST_ALLOCATOR_CATEGORIES is the number of lanes, and so of per-category
 * counter sets, a ta_test_allocator has. */
#define TA_TEST_ALLOCATOR_CATEGORIES 8u

/*
 * ta_test_allocator_counters is one set of facts about memory handed out and
 * taken back. The helper keeps one for the whole allocator (stats) and one per
 * category. Counters are plain integers a test reads directly; only the
 * helper writes them.
 */
typedef struct {
    uint64_t attempts;          /* every call to alloc, refused or not */
    uint64_t allocations;       /* calls that returned memory */
    uint64_t refused;           /* calls that returned NULL; attempts - allocations */
    uint64_t frees;             /* calls to free with a non-NULL pointer */
    uint64_t large_allocations; /* allocations of at least the large threshold */
    uint64_t bad_frees;         /* strict mode: wrong size or alignment, or an unknown pointer */
    size_t live_blocks;         /* allocations not yet freed */
    size_t live_bytes;          /* the sizes asked for by those blocks */
    size_t peak_bytes;          /* the most live_bytes has been since the last reset_counts */
    size_t largest_block;       /* the largest size an allocation asked for since the last reset_counts */
} ta_test_allocator_counters;

typedef struct ta_test_allocator ta_test_allocator;

/* ta_test_allocator_header is a live block's bookkeeping, private to
 * test_allocator.c; it sits directly before the memory the library sees. */
typedef struct ta_test_allocator_header ta_test_allocator_header;

/* ta_test_allocator_lane_state is what a lane's self pointer names: the
 * allocator and the category. Private; the helper fills it in. */
typedef struct {
    ta_test_allocator *heap;
    uint32_t category;
} ta_test_allocator_lane_state;

/*
 * ta_test_allocator is the allocator. A test reads the counters directly and
 * chooses the two modes in the initializer; everything below "private" is the
 * helper's, and a test arms policies and reads facts through the functions.
 */
struct ta_test_allocator {
    ta_test_allocator_counters stats;
    ta_test_allocator_counters by_category[TA_TEST_ALLOCATOR_CATEGORIES];

    /* zero_fill hands out zeroed memory, as calloc does. The tests that were
     * written against calloc (offload, session ID) keep it: they read memory
     * the library did not write. Set it in the initializer; it must not change
     * while blocks are live. */
    bool zero_fill;
    /* strict checks each free against its allocation: the size and alignment
     * must be the ones asked for, and the pointer must be live. A violation
     * writes a line to stderr and counts in bad_frees. It costs O(live_blocks)
     * a free, so it suits tests of tens or hundreds of blocks. Set it in the
     * initializer; it must not change while blocks are live. */
    bool strict;

    /* private */
    uint64_t fail_nth_remaining;  /* 0: disarmed; else attempts left up to and including the refused one */
    uint64_t fail_from_remaining; /* 0: disarmed; else likewise, and stays at 1 once reached */
    size_t refuse_size;           /* 0: none; else refuse sizes at least this */
    size_t large_threshold;       /* 0: none; else the size a large allocation has at least */
    bool refuse_every;
    ta_test_allocator_header *blocks;
    ta_test_allocator_lane_state lanes[TA_TEST_ALLOCATOR_CATEGORIES];
};

/* ta_test_allocator_vtable is the entry table of the plain allocator and its
 * lanes; ta_test_allocator_get and TA_TEST_ALLOCATOR_STATIC refer to it. */
extern const ta_allocator_vtable ta_test_allocator_vtable;

/* TA_TEST_ALLOCATOR_STATIC(heap_pointer) is an initializer for a ta_allocator
 * object with static storage duration that names the plain allocator (lane 0)
 * of the static ta_test_allocator at heap_pointer. */
#define TA_TEST_ALLOCATOR_STATIC(heap_pointer) { .self = (heap_pointer), .v = &ta_test_allocator_vtable }

/*
 * ta_test_allocator_finish returns the number of blocks still live and writes
 * a line for each to stderr with its size, alignment and category. It does
 * not free them: a leak is the test's finding, and freeing here would hide
 * it. Call it once the last user of the allocator has been destroyed. It
 * does not allocate. It is O(live_blocks).
 */
size_t ta_test_allocator_finish(ta_test_allocator *heap);

/*
 * ta_test_allocator_get returns the plain allocator, which counts in category
 * 0. The result is a value that stays valid while heap does.
 */
ta_allocator ta_test_allocator_get(ta_test_allocator *heap);

/*
 * ta_test_allocator_lane returns an allocator whose memory counts in
 * by_category[category] as well as in stats. A category at or past
 * TA_TEST_ALLOCATOR_CATEGORIES is lane 0. Lanes share the block list and the
 * failure policies, so "refuse the 5th allocation" counts across all lanes.
 * The result stays valid while heap does and does not move.
 */
ta_allocator ta_test_allocator_lane(ta_test_allocator *heap, uint32_t category);

/*
 * ta_test_allocator_fail_nth refuses the n-th attempt from now, once: with
 * n == 1 the very next alloc fails, and then the policy disarms itself. A
 * refused attempt counts as an attempt, so the sweep length of an operation
 * is the difference of stats.attempts around it. n == 0 disarms. It replaces
 * any earlier fail_nth.
 */
void ta_test_allocator_fail_nth(ta_test_allocator *heap, uint64_t n);

/*
 * ta_test_allocator_fail_from_nth refuses the n-th attempt from now and every
 * attempt after it, until disarmed. It models an allocator that has run dry,
 * and finds code that ignores the first NULL and carries on. n == 0 disarms.
 */
void ta_test_allocator_fail_from_nth(ta_test_allocator *heap, uint64_t n);

/*
 * ta_test_allocator_refuse_from refuses every request of at least bytes, until
 * called with 0. It lets a test refuse the one big region an operation takes
 * without counting the small ones in front of it.
 */
void ta_test_allocator_refuse_from(ta_test_allocator *heap, size_t bytes);

/*
 * ta_test_allocator_refuse_all refuses every request while on is true, and
 * allocates normally again when it is false.
 */
void ta_test_allocator_refuse_all(ta_test_allocator *heap, bool on);

/*
 * ta_test_allocator_disarm clears the four failure policies. It leaves the
 * counters and the large threshold alone.
 */
void ta_test_allocator_disarm(ta_test_allocator *heap);

/*
 * ta_test_allocator_set_large_threshold sets the size from which a successful
 * allocation also counts in large_allocations; 0 turns the count off. The
 * count restarts only through reset_counts.
 */
void ta_test_allocator_set_large_threshold(ta_test_allocator *heap, size_t bytes);

/*
 * ta_test_allocator_reset_counts zeroes the event counters (attempts,
 * allocations, refused, frees, large_allocations, bad_frees) and largest_block,
 * and starts peak_bytes from the current live_bytes, in stats and in every
 * category. live_blocks and live_bytes are facts about memory and are kept, as
 * are the policies. A sweep calls it between its steps.
 */
void ta_test_allocator_reset_counts(ta_test_allocator *heap);

#endif
