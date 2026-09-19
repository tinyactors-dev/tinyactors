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
/* Bytes the arena holds from its backing allocator, block headers included. */
size_t ta_arena_allocated_bytes(const ta_arena *arena);
