#include "tinyactors_host/allocator.h"
#include "allocator_private.h"

/* Block size of the turn and call arenas; an arena grows past it on demand. */
#define TAH_SCRATCH_BLOCK_BYTES ((size_t)16384)

static int tah_valid_alignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

static void *tah_lane_alloc(void *self, size_t size, size_t alignment) {
    tah_allocator_lane *lane = self;
    if (size == 0 || !tah_valid_alignment(alignment)) {
        return NULL;
    }
    if (lane->owner->refuse_from != 0 && size >= lane->owner->refuse_from) {
        return NULL;
    }
    ta_allocator backing = lane->owner->backing;
    void *ptr = backing.v->alloc(backing.self, size, alignment);
    if (ptr == NULL) {
        return NULL;
    }
    tah_memory_counters *counters = &lane->owner->counters[lane->category];
    counters->bytes_in_use += size;
    counters->allocations += 1u;
    if (counters->bytes_in_use > counters->peak_bytes) {
        counters->peak_bytes = counters->bytes_in_use;
    }
    if (size > counters->largest_block) {
        counters->largest_block = size;
    }
    return ptr;
}

static void tah_lane_free(void *self, const ta_allocator_free_params *params) {
    tah_allocator_lane *lane = self;
    if (params == NULL || params->ptr == NULL) {
        return;
    }
    lane->owner->backing.v->free(lane->owner->backing.self, params);
    tah_memory_counters *counters = &lane->owner->counters[lane->category];
    counters->bytes_in_use -= params->size;
    counters->frees += 1u;
}

static const ta_allocator_vtable tah_lane_vtable = {
    .alloc = tah_lane_alloc,
    .free = tah_lane_free,
};

void tah_allocator_refuse_from(tah_allocator *allocator, size_t bytes) {
    allocator->refuse_from = bytes;
}

void tah_allocator_init(tah_allocator *allocator, ta_allocator backing) {
    *allocator = (tah_allocator){ .backing = backing };
    for (int category = 0; category < TAH_MEMORY_CATEGORY_COUNT; category += 1) {
        allocator->lanes[category] = (tah_allocator_lane){
            .owner = allocator,
            .category = (tah_memory_category)category,
        };
    }
}

ta_allocator tah_allocator_for(tah_allocator *allocator, tah_memory_category category) {
    return (ta_allocator){ .self = &allocator->lanes[category], .v = &tah_lane_vtable };
}

tah_memory_counters tah_allocator_counters(const tah_allocator *allocator, tah_memory_category category) {
    return allocator->counters[category];
}

tah_memory_counters tah_allocator_total(const tah_allocator *allocator) {
    tah_memory_counters total = {0};
    for (int category = 0; category < TAH_MEMORY_CATEGORY_COUNT; category += 1) {
        const tah_memory_counters *counters = &allocator->counters[category];
        total.bytes_in_use += counters->bytes_in_use;
        total.peak_bytes += counters->peak_bytes;
        if (counters->largest_block > total.largest_block) {
            total.largest_block = counters->largest_block;
        }
        total.allocations += counters->allocations;
        total.frees += counters->frees;
    }
    return total;
}

uint64_t tah_allocator_calls(const tah_allocator *allocator) {
    tah_memory_counters total = tah_allocator_total(allocator);
    return total.allocations + total.frees;
}

void *tah_alloc(ta_allocator allocator, size_t size, size_t alignment) {
    if (allocator.v == NULL || allocator.v->alloc == NULL) {
        return NULL;
    }
    return allocator.v->alloc(allocator.self, size, alignment);
}

void tah_free(ta_allocator allocator, const ta_allocator_free_params *params) {
    if (params == NULL || params->ptr == NULL || allocator.v == NULL || allocator.v->free == NULL) {
        return;
    }
    allocator.v->free(allocator.self, params);
}

ta_result tah_scratch_init(tah_scratch *scratch, tah_allocator *allocator) {
    ta_allocator backing = tah_allocator_for(allocator, TAH_MEMORY_SCRATCH);
    *scratch = (tah_scratch){0};
    ta_result result = ta_arena_init(&scratch->turn, backing, TAH_SCRATCH_BLOCK_BYTES);
    if (result == TA_OK) {
        result = ta_arena_init(&scratch->call, backing, TAH_SCRATCH_BLOCK_BYTES);
    }
    if (result != TA_OK) {
        ta_arena_destroy(&scratch->turn);
    }
    return result;
}

void tah_scratch_reset_turn(tah_scratch *scratch) {
    ta_arena_reset(&scratch->turn);
}

void tah_scratch_reset_call(tah_scratch *scratch) {
    ta_arena_reset(&scratch->call);
}

ta_arena *tah_scratch_turn(tah_scratch *scratch) {
    return &scratch->turn;
}

ta_arena *tah_scratch_call(tah_scratch *scratch) {
    return &scratch->call;
}

void tah_scratch_destroy(tah_scratch *scratch) {
    ta_arena_destroy(&scratch->turn);
    ta_arena_destroy(&scratch->call);
}
