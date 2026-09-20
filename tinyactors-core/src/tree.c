#include "tree.h"

/* The arena block every tree starts from, as elsewhere in core. */
#define TA_TREE_BLOCK_SIZE 4096u
/* Elements in a list's first staging chunk; every next chunk doubles. */
#define TA_TREE_LIST_FIRST_CHUNK 8u

static bool ta_tree_power_of_two(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

/* Records the first failure only: an importer reports one result at its
 * outer boundary, and a later, smaller allocation must not clear it. */
static void ta_tree_fail(ta_tree *tree, ta_result result) {
    if (tree != NULL && tree->result == TA_OK) {
        tree->result = result;
    }
}

void ta_tree_init(ta_tree *tree, ta_allocator allocator) {
    if (tree == NULL) {
        return;
    }
    *tree = (ta_tree){ .allocator = allocator, .result = TA_OK, .impl = NULL };
    if (allocator.v == NULL || allocator.v->alloc == NULL || allocator.v->free == NULL) {
        tree->result = TA_ERR_INVALID;
        return;
    }
    ta_tree_state *state = ta_mem_alloc(allocator, sizeof(ta_tree_state), _Alignof(ta_tree_state));
    if (state == NULL) {
        tree->result = TA_ERR_NOMEM;
        return;
    }
    *state = (ta_tree_state){0};
    ta_result result = ta_arena_init(&state->arena, allocator, TA_TREE_BLOCK_SIZE);
    if (result != TA_OK) {
        ta_mem_free(allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
        tree->result = result;
        return;
    }
    tree->impl = state;
}

void ta_tree_destroy(ta_tree *tree) {
    if (tree == NULL) {
        return;
    }
    ta_tree_state *state = tree->impl;
    if (state != NULL) {
        ta_arena_destroy(&state->arena);
        ta_mem_free(tree->allocator, state,
                    &(ta_allocator_free_params){ .size = sizeof(ta_tree_state),
                                                 .alignment = _Alignof(ta_tree_state) });
    }
    *tree = (ta_tree){0};
}

void *ta_tree_alloc(ta_tree *tree, size_t size, size_t alignment) {
    if (tree == NULL || tree->impl == NULL || tree->result != TA_OK) {
        return NULL;
    }
    if (!ta_tree_power_of_two(alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return NULL;
    }
    ta_tree_state *state = tree->impl;
    /* A zero-sized request still has to yield its own address. */
    void *block = ta_arena_alloc(&state->arena, size == 0 ? 1u : size, alignment);
    if (block == NULL) {
        ta_tree_fail(tree, TA_ERR_NOMEM);
        return NULL;
    }
    return block;
}

ta_result ta_tree_result(const ta_tree *tree) {
    return tree == NULL ? TA_ERR_INVALID : tree->result;
}

void ta_tree_list_init(ta_tree_list *list, ta_tree *tree, const ta_tree_list_layout *layout) {
    if (list == NULL) {
        return;
    }
    *list = (ta_tree_list){ .tree = tree };
    if (tree == NULL) {
        return;
    }
    if (layout == NULL || layout->element_size == 0 || !ta_tree_power_of_two(layout->element_alignment)) {
        ta_tree_fail(tree, TA_ERR_INVALID);
        return;
    }
    list->element_size = layout->element_size;
    list->element_alignment = layout->element_alignment;
    ta_tree_list_state *state =
        ta_tree_alloc(tree, sizeof(ta_tree_list_state), _Alignof(ta_tree_list_state));
    if (state == NULL) {
        return;
    }
    *state = (ta_tree_list_state){ .next_capacity = TA_TREE_LIST_FIRST_CHUNK };
    list->impl = state;
}

/* Appends one more staging chunk, twice the size of the last. */
static ta_tree_chunk *ta_tree_list_grow(ta_tree_list *list, ta_tree_list_state *state) {
    size_t capacity = state->next_capacity == 0 ? TA_TREE_LIST_FIRST_CHUNK : state->next_capacity;
    if (capacity > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    ta_tree_chunk *chunk = ta_tree_alloc(list->tree, sizeof(ta_tree_chunk), _Alignof(ta_tree_chunk));
    if (chunk == NULL) {
        return NULL;
    }
    unsigned char *elements =
        ta_tree_alloc(list->tree, capacity * list->element_size, list->element_alignment);
    if (elements == NULL) {
        return NULL;
    }
    *chunk = (ta_tree_chunk){ .elements = elements, .capacity = capacity };
    if (state->last == NULL) {
        state->first = chunk;
    } else {
        state->last->next = chunk;
    }
    state->last = chunk;
    state->next_capacity = capacity > SIZE_MAX / 2u ? capacity : capacity * 2u;
    return chunk;
}

void ta_tree_put(ta_tree_list *list, const void *element) {
    if (list == NULL || list->impl == NULL || element == NULL) {
        return;
    }
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return;
    }
    ta_tree_list_state *state = list->impl;
    ta_tree_chunk *chunk = state->last;
    if (chunk == NULL || chunk->count == chunk->capacity) {
        chunk = ta_tree_list_grow(list, state);
        if (chunk == NULL) {
            return;
        }
    }
    ta_mem_copy(chunk->elements + chunk->count * list->element_size, element, list->element_size);
    chunk->count += 1u;
    state->count += 1u;
}

const void *ta_tree_close(ta_tree_list *list) {
    if (list == NULL || list->impl == NULL) {
        return NULL;
    }
    ta_tree_list_state *state = list->impl;
    /* A closed list is spent: a put after it must not reach the chunks the
     * returned array was copied from. */
    list->impl = NULL;
    if (list->tree == NULL || list->tree->result != TA_OK) {
        return NULL;
    }
    /* Every child list is terminated by a zero-initialized element. */
    size_t total = state->count + 1u;
    if (total > SIZE_MAX / list->element_size) {
        ta_tree_fail(list->tree, TA_ERR_NOMEM);
        return NULL;
    }
    unsigned char *array =
        ta_tree_alloc(list->tree, total * list->element_size, list->element_alignment);
    if (array == NULL) {
        return NULL;
    }
    size_t at = 0;
    for (const ta_tree_chunk *chunk = state->first; chunk != NULL; chunk = chunk->next) {
        ta_mem_copy(array + at, chunk->elements, chunk->count * list->element_size);
        at += chunk->count * list->element_size;
    }
    ta_mem_zero(array + at, list->element_size);
    return array;
}
