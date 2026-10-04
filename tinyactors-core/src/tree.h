#pragma once

#include "../include/tinyactors.h"
#include "arena.h"

/*
 * Storage behind the public ta_tree / ta_tree_list (include/tinyactors.h).
 *
 * An importer authors a specification tree bottom-up: the transitions of a
 * state are opened, filled and closed while the state list is still open. A
 * single bump region cannot serve that, because the inner list's elements
 * would land between the outer list's. Each open list therefore stages its
 * elements in a chain of chunks of its own, and ta_tree_close() copies the
 * chain into one contiguous, zero-terminated array. Every allocation, the
 * staging chunks included, comes from one arena the tree owns, so
 * ta_tree_destroy() returns all of it and no list needs a matching close.
 */

typedef struct ta_tree_chunk {
    struct ta_tree_chunk *next;
    unsigned char *elements;
    size_t count;
    size_t capacity;
} ta_tree_chunk;

/* One open list: its chunks in insertion order, and what it has staged. */
typedef struct {
    ta_tree_chunk *first;
    ta_tree_chunk *last;
    size_t count;
    size_t next_capacity;
} ta_tree_list_state;

typedef struct {
    ta_arena arena;
} ta_tree_state;
