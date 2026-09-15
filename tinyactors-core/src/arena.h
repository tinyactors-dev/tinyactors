#pragma once

#include "types.h"

typedef struct ta_arena_block {
    struct ta_arena_block *next;
    size_t capacity;
    size_t used;
    unsigned char data[];
} ta_arena_block;

typedef struct {
    ta_allocator backing;
    ta_arena_block *head;
    size_t block_size;
} ta_arena;

ta_result ta_arena_init(ta_arena *arena, ta_allocator backing, size_t block_size);
void ta_arena_reset(ta_arena *arena);
void ta_arena_destroy(ta_arena *arena);
void *ta_arena_alloc(ta_arena *arena, size_t size, size_t alignment);
