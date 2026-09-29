#pragma once

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
/* Bytes one fixed arena needs to offer `capacity` bytes. */
size_t ta_arena_fixed_bytes(size_t capacity);
/* Bytes the arena holds from its backing allocator, block headers included. */
size_t ta_arena_allocated_bytes(const ta_arena *arena);
