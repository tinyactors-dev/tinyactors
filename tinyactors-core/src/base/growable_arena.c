/* Linked chunks and marks follow tinyxml's arena. All released chunks are
 * retained here, and every new/reused chunk becomes the head (oversized
 * chunks included), so marks cover allocations in chronological order. */
#include <tinyactors/arena.h>
#include "base/memory.h"

struct ta_growable_arena_chunk {
    ta_growable_arena_chunk *prev;
    size_t capacity, offset, used_base;
};

#define TA_GROWABLE_ARENA_HEADER ((sizeof(ta_growable_arena_chunk) + 7u) & ~(size_t)7u)
#define TA_GROWABLE_ARENA_DEFAULT ((size_t)65536u)

void ta_growable_arena_init(ta_growable_arena *arena, const ta_growable_arena_config *config) {
    ta_memset(arena, 0, sizeof *arena);
    if (!config || config->size < sizeof *config || !config->allocator ||
        !config->allocator->alloc || !config->allocator->free) return;
    arena->allocator = config->allocator;
    arena->chunk_size = config->chunk_size ? config->chunk_size : TA_GROWABLE_ARENA_DEFAULT;
}

/* Bump only after capacity and aggregate accounting have been checked.
 * The payload starts at alignment 8; rounding an offset cannot move it. */
static void *arena_bump(ta_growable_arena *arena, size_t size, size_t align) {
    ta_growable_arena_chunk *chunk = arena->head;
    if (!chunk || chunk->offset > SIZE_MAX - (align - 1u)) return NULL;
    size_t start = (chunk->offset + align - 1u) & ~(align - 1u);
    if (start > chunk->capacity || size > chunk->capacity - start) return NULL;
    size_t consumed = start - chunk->offset + size;
    if (consumed > SIZE_MAX - arena->used) return NULL;
    chunk->offset = start + size;
    arena->used += consumed;
    return (unsigned char *)chunk + TA_GROWABLE_ARENA_HEADER + start;
}

void *ta_growable_arena_alloc(ta_growable_arena *arena, size_t size, size_t align) {
    if (!arena->allocator || !size || !align || align > 8 || (align & (align - 1u)) ||
        size > SIZE_MAX - arena->used) return NULL;
    void *result = arena_bump(arena, size, align);
    if (result) return result;

    /* Best fit avoids consuming a retained oversized chunk for a smaller
     * request when a smaller retained chunk can serve that request. */
    ta_growable_arena_chunk **chosen = NULL;
    for (ta_growable_arena_chunk **at = &arena->kept; *at; at = &(*at)->prev)
        if ((*at)->capacity >= size && (!chosen || (*at)->capacity < (*chosen)->capacity)) chosen = at;
    ta_growable_arena_chunk *chunk;
    if (chosen) {
        chunk = *chosen;
        *chosen = chunk->prev;
    } else {
        size_t capacity = size > arena->chunk_size ? size : arena->chunk_size;
        if (capacity > SIZE_MAX - TA_GROWABLE_ARENA_HEADER) return NULL;
        size_t bytes = TA_GROWABLE_ARENA_HEADER + capacity;
        if (bytes > SIZE_MAX - arena->reserved) return NULL;
        chunk = arena->allocator->alloc(arena->allocator->context, bytes, 8);
        if (!chunk) return NULL;
        chunk->capacity = capacity;
        arena->reserved += bytes;
    }
    chunk->prev = arena->head;
    chunk->offset = 0;
    chunk->used_base = arena->used;
    arena->head = chunk;
    return arena_bump(arena, size, align); /* aligned empty chunk always fits */
}

ta_growable_arena_mark ta_growable_arena_position(const ta_growable_arena *arena) {
    ta_growable_arena_mark mark = {arena->head, arena->head ? arena->head->offset : 0};
    return mark;
}

void ta_growable_arena_release(ta_growable_arena *arena, ta_growable_arena_mark mark) {
    /* Search before dereferencing caller-supplied mark.chunk. Pointer/offset
     * marks cannot prove freshness once their storage has been reused. */
    if (mark.chunk) {
        ta_growable_arena_chunk *at = arena->head;
        while (at && at != mark.chunk) at = at->prev;
        if (!at || mark.offset > at->offset) return;
    } else if (mark.offset) return;
    while (arena->head != mark.chunk) {
        ta_growable_arena_chunk *chunk = arena->head;
        arena->head = chunk->prev;
        chunk->prev = arena->kept;
        arena->kept = chunk;
    }
    if (arena->head) {
        arena->head->offset = mark.offset;
        arena->used = arena->head->used_base + mark.offset;
    } else arena->used = 0;
}

/* Adapter callbacks own no storage: marks, not individual free calls,
 * govern the lifetime of every allocation obtained through this view. */
static void *arena_allocator_alloc(void *context, size_t size, size_t align) {
    return ta_growable_arena_alloc((ta_growable_arena *)context, size, align);
}

static void arena_allocator_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)pointer;
    (void)size;
}

ta_allocator ta_growable_arena_allocator(ta_growable_arena *arena) {
    ta_allocator allocator = {arena, arena_allocator_alloc, arena_allocator_free};
    return allocator;
}

/* Detach each list before freeing it; no freed chunk is read again. */
static void arena_free_chunks(ta_growable_arena *arena, ta_growable_arena_chunk *chunk) {
    while (chunk) {
        ta_growable_arena_chunk *previous = chunk->prev;
        arena->allocator->free(arena->allocator->context, chunk, TA_GROWABLE_ARENA_HEADER + chunk->capacity);
        chunk = previous;
    }
}

void ta_growable_arena_free(ta_growable_arena *arena) {
    arena_free_chunks(arena, arena->head);
    arena_free_chunks(arena, arena->kept);
    arena->head = arena->kept = NULL;
    arena->used = arena->reserved = 0;
}
