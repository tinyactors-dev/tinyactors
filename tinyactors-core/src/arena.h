#pragma once

#include "layout.h"
#include "types.h"

struct ta_arena_block {
    struct ta_arena_block *next;
    size_t allocation_size;
    size_t alignment;
    size_t capacity;
    size_t used;
    unsigned char data[];
};

/* Copies text into the arena. An empty string yields a non-null empty ta_str. */
ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out);
/*
 * An arena over `bytes` of memory the caller owns, aligned for
 * ta_arena_block, which never grows (ADR 0092): an allocation that does not
 * fit answers NULL. Reset and destroy work as for any arena; destroy frees
 * nothing, since the memory is the caller's.
 */
void ta_arena_init_fixed(ta_arena *arena, void *memory, size_t bytes);
/*
 * ta_arena_fixed_add adds to `layout` the memory one fixed arena needs to
 * offer `capacity` bytes (its block header, then the data), and returns the
 * offset where it starts. The arena block's size is a fact of this unit, so
 * the layout's checked arithmetic covers the sum: nothing adds a capacity to
 * a header by hand. As for ta_layout_add, the offset means something only
 * after ta_layout_finish has answered TA_OK. Never allocates.
 */
size_t ta_arena_fixed_add(ta_layout *layout, size_t capacity);
/* Bytes the arena holds from its backing allocator, block headers included. */
size_t ta_arena_allocated_bytes(const ta_arena *arena);
