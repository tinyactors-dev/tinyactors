#include "base/arena.h"

void ta_arena_init(ta_arena *arena, void *memory, size_t capacity) {
    uintptr_t p = (uintptr_t)memory;
    uintptr_t aligned = (p + (TA_ALIGN_MAX - 1)) & ~(uintptr_t)(TA_ALIGN_MAX - 1);
    size_t skip = (size_t)(aligned - p);
    arena->base = (unsigned char *)aligned;
    arena->capacity = capacity > skip ? capacity - skip : 0;
    arena->offset = 0;
    arena->failed = 0;
}

void ta_arena_init_measure(ta_arena *arena) {
    arena->base = (unsigned char *)0;
    arena->capacity = (size_t)-1;
    arena->offset = 0;
    arena->failed = 0;
}

size_t ta_arena_alloc(ta_arena *arena, size_t size, size_t align) {
    TA_ASSERT(align && (align & (align - 1)) == 0 && align <= TA_ALIGN_MAX);
    size_t offset = ta_align_up(arena->offset, align);
    if (offset < arena->offset || size > arena->capacity || offset > arena->capacity - size) {
        arena->failed = 1;
        return 0;
    }
    arena->offset = offset + size;
    return offset;
}
