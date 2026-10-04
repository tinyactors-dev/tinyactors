/*
 * test_allocator.c implements tests/test_allocator.h; read the package
 * comment there for the contract, the lifetimes and the invariants. This file
 * records only how the contract is kept.
 *
 * Layout of a block. One libc allocation holds the header and the block the
 * library sees:
 *
 *     base                     user - sizeof(header)        user
 *     |  padding               |  ta_test_allocator_header  |  size bytes ...
 *
 * user is a multiple of the alignment the library asked for, and the header
 * is the sizeof(header) bytes before it, so a free finds its bookkeeping from
 * the pointer alone, in O(1), with no table to outgrow and no limit on live
 * blocks. The padding is whatever is needed to put user on its alignment; it
 * is at least sizeof(header) rounded up to that alignment. base is what libc
 * returned and what libc is given back.
 *
 * Every size is computed with checked arithmetic: a request whose total
 * would overflow size_t is refused, as a host allocator must.
 */
#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct ta_test_allocator_header {
    ta_test_allocator_header *previous; /* the live list, newest first */
    ta_test_allocator_header *next;
    void *base;                         /* what libc returned */
    size_t size;                        /* what the library asked for */
    size_t alignment;                   /* as the library asked, before rounding up */
    uint32_t category;
};

/* Alignment of every libc allocation: no less than a pointer, and enough
 * for the header that sits in front of the library's memory. */
#define MINIMUM_ALIGNMENT (_Alignof(ta_test_allocator_header) > sizeof(void *) ? _Alignof(ta_test_allocator_header) : sizeof(void *))

/* is_power_of_two says whether value is 1, 2, 4, ...; aligned_alloc is
 * undefined for anything else, so the helper refuses such a request. */
static bool is_power_of_two(size_t value) {
    return value != 0 && (value & (value - 1u)) == 0;
}

/* round_up sets *out to value rounded up to a multiple of the power of two
 * alignment, and says whether it fit in size_t. */
static bool round_up(size_t value, size_t alignment, size_t *out) {
    if (value > SIZE_MAX - (alignment - 1u)) {
        return false;
    }
    *out = (value + alignment - 1u) & ~(alignment - 1u);
    return true;
}

/* count_attempt updates the counters that every attempt moves. */
static void count_attempt(ta_test_allocator_counters *counters, bool refused) {
    counters->attempts += 1u;
    counters->refused += refused ? 1u : 0u;
}

/* count_allocation updates the counters for a block handed out. */
static void count_allocation(ta_test_allocator_counters *counters, size_t size, bool large) {
    counters->allocations += 1u;
    counters->large_allocations += large ? 1u : 0u;
    counters->live_blocks += 1u;
    counters->live_bytes += size;
    if (counters->live_bytes > counters->peak_bytes) {
        counters->peak_bytes = counters->live_bytes;
    }
    if (size > counters->largest_block) {
        counters->largest_block = size;
    }
}

/* count_free updates the counters for a block taken back. */
static void count_free(ta_test_allocator_counters *counters, size_t size) {
    counters->frees += 1u;
    counters->live_blocks -= 1u;
    counters->live_bytes -= size;
}

/* decide_refusal advances every armed policy by one attempt and says whether
 * this attempt is refused. All countdowns advance on every attempt, so that
 * "the 3rd from now" means the same whichever policy is meant to fire. */
static bool decide_refusal(ta_test_allocator *heap, size_t size) {
    bool refuse = heap->refuse_every || (heap->refuse_size != 0 && size >= heap->refuse_size);
    if (heap->fail_nth_remaining != 0) {
        heap->fail_nth_remaining -= 1u;
        refuse = refuse || heap->fail_nth_remaining == 0;
    }
    if (heap->fail_from_remaining != 0) {
        refuse = refuse || heap->fail_from_remaining == 1u;
        if (heap->fail_from_remaining > 1u) {
            heap->fail_from_remaining -= 1u;
        }
    }
    return refuse;
}

/* lane_alloc is the alloc entry of every lane: it takes `size` bytes aligned
 * to `alignment` from libc (the lane is passed as its state, which names the
 * allocator and the category, to keep the parameters at three), counts them
 * in the lane's category and in the totals, or returns NULL and counts the refusal. */
static void *lane_alloc(const ta_test_allocator_lane_state *state, size_t size, size_t alignment) {
    ta_test_allocator *heap = state->heap;
    uint32_t category = state->category;
    ta_test_allocator_counters *lane = &heap->by_category[category];
    size_t align = alignment < MINIMUM_ALIGNMENT ? MINIMUM_ALIGNMENT : alignment;
    size_t offset = 0;
    size_t total = 0;
    bool refuse = decide_refusal(heap, size);
    /* The padding in front: the header, rounded up so the user pointer is
     * aligned. The libc size must itself be a multiple of the alignment. */
    if (refuse || !is_power_of_two(align) || !round_up(sizeof(ta_test_allocator_header), align, &offset) ||
        size > SIZE_MAX - offset || !round_up(offset + size, align, &total)) {
        count_attempt(&heap->stats, true);
        count_attempt(lane, true);
        return NULL;
    }
    unsigned char *base = aligned_alloc(align, total);
    if (base == NULL) {
        count_attempt(&heap->stats, true);
        count_attempt(lane, true);
        return NULL;
    }
    unsigned char *user = base + offset;
    ta_test_allocator_header *header = (ta_test_allocator_header *)(void *)(user - sizeof(*header));
    *header = (ta_test_allocator_header){ .previous = NULL, .next = heap->blocks, .base = base, .size = size,
                                          .alignment = alignment, .category = category };
    if (heap->blocks != NULL) {
        heap->blocks->previous = header;
    }
    heap->blocks = header;
    if (heap->zero_fill) {
        memset(user, 0, size);
    }
    bool large = heap->large_threshold != 0 && size >= heap->large_threshold;
    count_attempt(&heap->stats, false);
    count_attempt(lane, false);
    count_allocation(&heap->stats, size, large);
    count_allocation(lane, size, large);
    return user;
}

/* is_live says whether pointer is the user pointer of a block on the live
 * list. O(live_blocks); strict mode only. */
static bool is_live(const ta_test_allocator *heap, const void *pointer) {
    for (const ta_test_allocator_header *block = heap->blocks; block != NULL; block = block->next) {
        if ((const unsigned char *)(const void *)(block + 1) == (const unsigned char *)pointer) {
            return true;
        }
    }
    return false;
}

/* lane_free is the free entry of every lane: it unlinks the block, takes it
 * off the counters of the category it was allocated in (not the lane that
 * frees it, which would corrupt the per-category facts), and gives it back to
 * libc. */
static void lane_free(ta_test_allocator *heap, const ta_allocator_free_params *params) {
    if (params->ptr == NULL) {
        return;
    }
    if (heap->strict && !is_live(heap, params->ptr)) {
        fprintf(stderr, "free of %p, which this allocator never handed out\n", params->ptr);
        heap->stats.bad_frees += 1u; /* no header, so no category: stats only */
        return;
    }
    ta_test_allocator_header *header = (ta_test_allocator_header *)params->ptr - 1;
    if (heap->strict && (header->size != params->size || header->alignment != params->alignment)) {
        fprintf(stderr, "free of %p as %zu/%zu, allocated as %zu/%zu\n", params->ptr, params->size,
                params->alignment, header->size, header->alignment);
        heap->stats.bad_frees += 1u;
        heap->by_category[header->category].bad_frees += 1u;
    }
    if (header->previous != NULL) {
        header->previous->next = header->next;
    } else {
        heap->blocks = header->next;
    }
    if (header->next != NULL) {
        header->next->previous = header->previous;
    }
    count_free(&heap->stats, header->size);
    count_free(&heap->by_category[header->category], header->size);
    free(header->base);
}

/* plain_alloc and plain_free are lane 0: self is the allocator itself. */
static void *plain_alloc(void *self, size_t size, size_t alignment) {
    const ta_test_allocator_lane_state state = { .heap = self, .category = 0 };
    return lane_alloc(&state, size, alignment);
}

static void plain_free(void *self, const ta_allocator_free_params *params) {
    lane_free(self, params);
}

const ta_allocator_vtable ta_test_allocator_vtable = { .alloc = plain_alloc, .free = plain_free };

/* category_alloc and category_free serve the other lanes: self is a lane
 * state, which names the allocator and the category. */
static void *category_alloc(void *self, size_t size, size_t alignment) {
    const ta_test_allocator_lane_state *lane = self;
    return lane_alloc(lane, size, alignment);
}

static void category_free(void *self, const ta_allocator_free_params *params) {
    const ta_test_allocator_lane_state *lane = self;
    lane_free(lane->heap, params);
}

static const ta_allocator_vtable category_vtable = { .alloc = category_alloc, .free = category_free };

size_t ta_test_allocator_finish(ta_test_allocator *heap) {
    size_t leaked = 0;
    for (const ta_test_allocator_header *block = heap->blocks; block != NULL; block = block->next) {
        fprintf(stderr, "test allocator: leaked %zu bytes aligned to %zu (category %u)\n", block->size,
                block->alignment, (unsigned)block->category);
        leaked += 1u;
    }
    return leaked;
}

ta_allocator ta_test_allocator_get(ta_test_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &ta_test_allocator_vtable };
}

ta_allocator ta_test_allocator_lane(ta_test_allocator *heap, uint32_t category) {
    if (category == 0 || category >= TA_TEST_ALLOCATOR_CATEGORIES) {
        return ta_test_allocator_get(heap);
    }
    heap->lanes[category] = (ta_test_allocator_lane_state){ .heap = heap, .category = category };
    return (ta_allocator){ .self = &heap->lanes[category], .v = &category_vtable };
}

void ta_test_allocator_fail_nth(ta_test_allocator *heap, uint64_t n) {
    heap->fail_nth_remaining = n;
}

void ta_test_allocator_fail_from_nth(ta_test_allocator *heap, uint64_t n) {
    heap->fail_from_remaining = n;
}

void ta_test_allocator_refuse_from(ta_test_allocator *heap, size_t bytes) {
    heap->refuse_size = bytes;
}

void ta_test_allocator_refuse_all(ta_test_allocator *heap, bool on) {
    heap->refuse_every = on;
}

void ta_test_allocator_disarm(ta_test_allocator *heap) {
    heap->fail_nth_remaining = 0;
    heap->fail_from_remaining = 0;
    heap->refuse_size = 0;
    heap->refuse_every = false;
}

void ta_test_allocator_set_large_threshold(ta_test_allocator *heap, size_t bytes) {
    heap->large_threshold = bytes;
}

/* reset_counters is the per-set half of reset_counts. */
static void reset_counters(ta_test_allocator_counters *counters) {
    counters->attempts = 0;
    counters->allocations = 0;
    counters->refused = 0;
    counters->frees = 0;
    counters->large_allocations = 0;
    counters->bad_frees = 0;
    counters->peak_bytes = counters->live_bytes;
    counters->largest_block = 0;
}

void ta_test_allocator_reset_counts(ta_test_allocator *heap) {
    reset_counters(&heap->stats);
    for (uint32_t category = 0; category < TA_TEST_ALLOCATOR_CATEGORIES; category += 1u) {
        reset_counters(&heap->by_category[category]);
    }
}
