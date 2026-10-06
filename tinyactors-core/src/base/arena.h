/* arena.h — bump allocator over a caller buffer, with measure mode.
 *
 * Measure mode (no buffer) runs the exact same carving code to compute the
 * required size: ta_system_layout() and ta_system_init() share one function,
 * so sizing and placement cannot disagree.
 *
 *     ta_arena arena;
 *     ta_arena_init_measure(&arena);  carve(&arena, config);  need = ta_arena_required(&arena);
 *     ta_arena_init(&arena, memory, n);  carve(&arena, config);  if (arena.failed) ...
 *
 * Allocations return offsets (relocatable); ta_arena_pointer() converts.
 * Failure is sticky: check `failed` once after a sequence of allocations.
 */
#ifndef TINYACTORS_BASE_ARENA_H
#define TINYACTORS_BASE_ARENA_H

#include "base/config.h"

typedef struct ta_arena {
    unsigned char *base; /* NULL in measure mode */
    size_t capacity;
    size_t offset;
    int failed;
} ta_arena;

void ta_arena_init(ta_arena *a, void *memory, size_t capacity);
void ta_arena_init_measure(ta_arena *a);

/* Returns the offset of `size` bytes aligned to `align` (power of two,
 * <= TA_ALIGN_MAX). On overflow sets `failed` and returns 0. */
size_t ta_arena_alloc(ta_arena *a, size_t size, size_t align);

/* Bytes a caller must provide for what was carved so far (includes slack
 * for aligning an arbitrary base address). */
TA_INLINE size_t ta_arena_required(const ta_arena *arena) {
    return arena->offset + TA_ALIGN_MAX - 1;
}

TA_INLINE void *ta_arena_pointer(const ta_arena *arena, size_t offset) {
    return arena->base ? (void *)(arena->base + offset) : (void *)0;
}

TA_INLINE size_t ta_arena_mark(const ta_arena *arena) {
    return arena->offset;
}
TA_INLINE void ta_arena_reset(ta_arena *arena, size_t mark) {
    TA_ASSERT(mark <= arena->offset);
    arena->offset = mark;
}

#endif /* TINYACTORS_BASE_ARENA_H */
