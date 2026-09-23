#include "arena.h"

static bool ta_arena_valid_alignment(size_t alignment) {
    return alignment != 0 && (alignment & (alignment - 1u)) == 0;
}

static size_t ta_arena_max_size(size_t left, size_t right) {
    return left > right ? left : right;
}

static void *ta_arena_allocate_from_block(ta_arena_block *block,
                                          size_t size,
                                          size_t alignment) {
    uintptr_t base = (uintptr_t)block->data;
    uintptr_t current = base + block->used;
    uintptr_t aligned = (current + alignment - 1u) & ~(uintptr_t)(alignment - 1u);
    size_t offset = (size_t)(aligned - base);
    if (offset > block->capacity || size > block->capacity - offset) {
        return NULL;
    }
    block->used = offset + size;
    return (void *)aligned;
}

ta_result ta_arena_init(ta_arena *arena, ta_allocator backing, size_t block_size) {
    if (arena == NULL || backing.v == NULL || backing.v->alloc == NULL ||
        backing.v->free == NULL || block_size == 0) {
        return TA_ERR_INVALID;
    }
    *arena = (ta_arena){
        .backing = backing,
        .block_size = block_size,
    };
    return TA_OK;
}

void ta_arena_reset(ta_arena *arena) {
    if (arena == NULL) {
        return;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        block->used = 0;
    }
}

void ta_arena_destroy(ta_arena *arena) {
    if (arena == NULL || arena->backing.v == NULL) {
        return;
    }
    ta_arena_block *block = arena->head;
    while (block != NULL) {
        ta_arena_block *next = block->next;
        arena->backing.v->free(
            arena->backing.self,
            &(ta_allocator_free_params){
                .ptr = block,
                .size = block->allocation_size,
                .alignment = block->alignment,
            });
        block = next;
    }
    *arena = (ta_arena){0};
}

void *ta_arena_alloc(ta_arena *arena, size_t size, size_t alignment) {
    if (arena == NULL || !ta_arena_valid_alignment(alignment) || size == 0) {
        return NULL;
    }
    for (ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        void *result = ta_arena_allocate_from_block(block, size, alignment);
        if (result != NULL) {
            return result;
        }
    }

    if (size > SIZE_MAX - (alignment - 1u)) {
        return NULL;
    }
    size_t capacity = ta_arena_max_size(arena->block_size, size + alignment - 1u);
    if (capacity > SIZE_MAX - offsetof(ta_arena_block, data)) {
        return NULL;
    }
    size_t allocation_size = offsetof(ta_arena_block, data) + capacity;
    size_t block_alignment = ta_arena_max_size(_Alignof(ta_arena_block), alignment);
    ta_arena_block *block = arena->backing.v->alloc(arena->backing.self,
                                                    allocation_size,
                                                    block_alignment);
    if (block == NULL) {
        return NULL;
    }
    *block = (ta_arena_block){
        .next = arena->head,
        .allocation_size = allocation_size,
        .alignment = block_alignment,
        .capacity = capacity,
    };
    arena->head = block;
    return ta_arena_allocate_from_block(block, size, alignment);
}

size_t ta_arena_allocated_bytes(const ta_arena *arena) {
    size_t total = 0;
    for (const ta_arena_block *block = arena->head; block != NULL; block = block->next) {
        total += block->allocation_size;
    }
    return total;
}

ta_result ta_arena_copy_str(ta_arena *arena, ta_str text, ta_str *out) {
    char *copy = ta_arena_alloc(arena, (size_t)text.len + 1u, 1);
    if (copy == NULL) {
        return TA_ERR_NOMEM;
    }
    if (text.len != 0) {
        ta_mem_copy(copy, text.ptr, text.len);
    }
    copy[text.len] = '\0';
    *out = (ta_str){ .ptr = copy, .len = text.len };
    return TA_OK;
}

static void *ta_arena_allocator_alloc(void *self, size_t size, size_t alignment) {
    return ta_arena_alloc(self, size, alignment);
}

static void ta_arena_allocator_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const ta_allocator_vtable ta_arena_allocator_vtable = {
    .alloc = ta_arena_allocator_alloc,
    .free = ta_arena_allocator_free,
};

ta_allocator ta_arena_allocator(ta_arena *arena) {
    return (ta_allocator){ .self = arena, .v = &ta_arena_allocator_vtable };
}
