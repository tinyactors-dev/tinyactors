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
