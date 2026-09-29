#pragma once
/*
 * The system's memory (ADR 0014, ADR 0009).
 *
 * One counting allocator per system, over the platform allocator the
 * binding gives the kernel (kernel ADR 0001: the kernel links no C library,
 * so malloc is the binding's). It is the system's one
 * ta_allocator, and it is also handed to core. Every allocation names a
 * category, so stats() can say what the addon's memory is for. `free`
 * receives the size and alignment, so the counters need no header.
 *
 * Scratch memory is core's ta_arena over this allocator: a turn arena reset at
 * the end of each pump turn, and a call arena reset after each call. They are
 * reset, not destroyed, so after warm-up they allocate nothing.
 *
 * Size-class pools are deferred (ADR 0014, point 5). If a measurement asks for
 * them, they replace the backing allocator behind tah_alloc and tah_free and nothing else
 * changes.
 *
 * A system is used by one thread (ADR 0001): nothing here locks.
 */
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

typedef enum {
    TAH_MEMORY_CORE = 0,   /* everything core allocates */
    TAH_MEMORY_ENVELOPES,  /* mailbox envelopes (ADR 0005) */
    TAH_MEMORY_SCHEDULING, /* the scheduling table and queues (ADR 0005) */
    TAH_MEMORY_VALUES,     /* value boxes (ADR 0011) */
    TAH_MEMORY_DIRECTORY,  /* the actor directory (ADR 0006) */
    TAH_MEMORY_SCRATCH,    /* the turn and call arenas */
    TAH_MEMORY_OTHER,      /* everything else the addon keeps */
    TAH_MEMORY_CATEGORY_COUNT
} tah_memory_category;

typedef struct {
    size_t bytes_in_use;
    size_t peak_bytes;    /* the high-water mark of bytes_in_use */
    size_t largest_block; /* the largest single allocation so far */
    uint64_t allocations; /* successful allocator calls */
    uint64_t frees;
} tah_memory_counters;

typedef struct tah_allocator tah_allocator;

/* What a category's ta_allocator points at: the owner and the category. */
typedef struct {
    tah_allocator *owner;
    tah_memory_category category;
} tah_allocator_lane;

struct tah_allocator {
    tah_allocator_lane lanes[TAH_MEMORY_CATEGORY_COUNT];
    tah_memory_counters counters[TAH_MEMORY_CATEGORY_COUNT];
    /* The platform allocator every lane allocates from (the binding's). */
    ta_allocator backing;
    /* Tests only: refuse every allocation of at least this many bytes; 0 refuses none. */
    size_t refuse_from;
};

/* `backing` is the binding's platform allocator; every lane allocates from it. */
void tah_allocator_init(tah_allocator *allocator, ta_allocator backing);
/* A ta_allocator whose allocations count against `category`. */
ta_allocator tah_allocator_for(tah_allocator *allocator, tah_memory_category category);
tah_memory_counters tah_allocator_counters(const tah_allocator *allocator, tah_memory_category category);
/* The sum over every category; peak_bytes is the sum of the peaks, and
 * largest_block the largest of any category. */
tah_memory_counters tah_allocator_total(const tah_allocator *allocator);
/* Allocations plus frees, over every category: the steady-state test signal. */
uint64_t tah_allocator_calls(const tah_allocator *allocator);
/*
 * Tests only: from now on, refuse every allocation of at least `bytes`, in
 * every category, as a full pool would; 0 refuses none again. A size, not a
 * count, names the allocation to fail, so a test can fail one large
 * allocation without depending on how many small ones come before it.
 */
void tah_allocator_refuse_from(tah_allocator *allocator, size_t bytes);

/*
 * Convenience over a ta_allocator (a lane or any other). A zero size or an
 * alignment that is not a power of two yields NULL.
 */
void *tah_alloc(ta_allocator allocator, size_t size, size_t alignment);
void tah_free(ta_allocator allocator, const ta_allocator_free_params *params);

/* The turn and call arenas. */
typedef struct {
    ta_arena turn;
    ta_arena call;
} tah_scratch;

ta_result tah_scratch_init(tah_scratch *scratch, tah_allocator *allocator);
void tah_scratch_reset_turn(tah_scratch *scratch);
void tah_scratch_reset_call(tah_scratch *scratch);
void tah_scratch_destroy(tah_scratch *scratch);
