#pragma once
/*
 * The kernel's own layout of what tinyactors_host/allocator.h declares
 * opaque: private to the kernel (kernel ADR 0001, "Step 2b"). The kernel's
 * sources and tests include it; a binding reaches these types only through
 * the functions of the public header.
 */
#include "tinyactors_host/allocator.h"

/* What a category's ta_allocator points at: the owner and the category. */
typedef struct tah_allocator_lane {
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

/* The turn and call arenas. */
struct tah_scratch {
    ta_arena turn;
    ta_arena call;
};
