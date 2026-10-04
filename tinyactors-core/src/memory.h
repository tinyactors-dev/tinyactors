/**
 * @file memory.h
 *
 * # Memory helpers
 *
 * The freestanding replacements for `malloc`, `free`, `memcpy` and
 * `memset` that every unit of core uses, plus the arithmetic of sizes and
 * alignments. Core links no C library: all of its memory comes from a
 * `ta_allocator` the host injects (`include/tinyactors.h`), and it reaches
 * that allocator only through `ta_mem_alloc` and `ta_mem_free`.
 *
 * ## Contract
 *
 * - `ta_mem_alloc(allocator, size, alignment)` asks the allocator for
 *   `size` bytes aligned to `alignment`. It answers NULL, without calling
 *   the allocator, for a size of 0 or an allocator with no vtable or no
 *   `alloc`; otherwise it answers what the allocator answers, NULL when it
 *   is out of memory.
 * - `ta_mem_free(allocator, ptr, layout)` gives `ptr` back with the size
 *   and alignment it was allocated with (`layout->size`,
 *   `layout->alignment`; `layout->ptr` is ignored and replaced by `ptr`).
 *   Freeing NULL, or through an allocator with no vtable or no `free`,
 *   does nothing.
 * - `ta_mem_copy` and `ta_mem_zero` copy and clear bytes one at a time.
 *   The regions given to `ta_mem_copy` must not overlap.
 * - `ta_align_up(value, alignment)` rounds up to any non-zero multiple;
 *   `TA_ALIGN_UP(v, a)` rounds up to a power of two `a`, in the type of
 *   `v`, and is a constant expression when its operands are.
 * - `TA_ARRAY_COUNT(a)` is the element count of an array (not a
 *   pointer), as a `uint32_t`.
 *
 * ## Memory
 *
 * The unit owns nothing. `ta_mem_alloc` allocates, and its caller owns the
 * block until it hands it to `ta_mem_free` with the same allocator and
 * layout. Whether a caller may allocate inside a macrostep is that
 * caller's contract; the other helpers never allocate and may run
 * anywhere.
 *
 * ## Complexity
 *
 * `ta_mem_alloc` and `ta_mem_free` cost one call to the allocator;
 * `ta_mem_copy` and `ta_mem_zero` are O(size); the rest are O(1).
 *
 * ## Failure
 *
 * `ta_mem_alloc` answers NULL, which callers turn into `TA_ERR_NOMEM`.
 * Nothing else fails. A zero alignment, overlapping copies and a layout
 * that differs from the allocation's are programmer errors, not checked.
 *
 * ## Users
 *
 * Every unit that allocates: arenas, vectors, paged arrays, the atom
 * table, the system, sessions, actors and their definitions, the session
 * image and the text writer's renderer; and every unit that copies or
 * clears bytes.
 */
#pragma once

#include "../include/tinyactors.h"

/** The element count of array `a` (an array, not a pointer), as a `uint32_t`. */
#define TA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
/** `v` rounded up to a multiple of `a`, which is a power of two. */
#define TA_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

/**
 * Allocates `size` bytes aligned to `alignment` from `allocator`. Answers
 * NULL for a size of 0, an allocator without `alloc`, or when the
 * allocator is out of memory. The caller owns the block.
 */
void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment);
/**
 * Gives `ptr` back to `allocator`, with the size and alignment in
 * `layout` that it was allocated with. Does nothing for NULL or an
 * allocator without `free`. Never fails.
 */
void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout);
/** Copies `size` bytes from `src` to `dst`, which do not overlap. Never allocates. */
void ta_mem_copy(void *dst, const void *src, size_t size);
/** Sets `size` bytes at `dst` to 0. Never allocates. */
void ta_mem_zero(void *dst, size_t size);

/** `value` rounded up to a multiple of `alignment`, which is never zero. */
static inline size_t ta_align_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) / alignment * alignment;
}
