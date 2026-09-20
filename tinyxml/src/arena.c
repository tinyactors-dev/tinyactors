/*
 * The linked-chunk arena (memory-architecture.md, "The arena").
 *
 * A chunk is a header followed by its payload, obtained from the backing
 * allocator in one block whose alignment is CHUNK_ALIGNMENT. The header is
 * padded to that alignment so payload offset 0 is aligned for every request
 * up to it; a request for more is honoured by rounding the address instead.
 */
#include "../include/txml.h"

#define CHUNK_ALIGNMENT ((size_t)64u)
#define CHUNK_GROWTH_CAP ((size_t)1048576u)

struct txml_arena_chunk {
    txml_arena_chunk *prev;   /* the chunk allocated before this one */
    txml_arena_chunk *spare;  /* a released chunk kept for the next growth */
    size_t capacity;          /* payload bytes */
    size_t offset;            /* bump offset into the payload */
    size_t used_base;         /* arena `used` when this chunk became the head */
};

#define CHUNK_HEADER \
    (((sizeof(struct txml_arena_chunk) + CHUNK_ALIGNMENT - 1u) / CHUNK_ALIGNMENT) * CHUNK_ALIGNMENT)

static unsigned char *chunk_payload(txml_arena_chunk *chunk) {
    return (unsigned char *)chunk + CHUNK_HEADER;
}

static void chunk_free(const txml_arena *arena, txml_arena_chunk *chunk) {
    txml_allocator_free_params params = {
        .ptr = chunk,
        .size = CHUNK_HEADER + chunk->capacity,
        .alignment = CHUNK_ALIGNMENT,
    };
    arena->backing.v->free(arena->backing.self, &params);
}

/* Free `chunk` and the spare it holds. Iterative: a spare never has a spare. */
static void chunk_release(txml_arena *arena, txml_arena_chunk *chunk) {
    txml_arena_chunk *spare = chunk->spare;
    arena->reserved -= CHUNK_HEADER + chunk->capacity;
    if (spare != NULL) {
        arena->reserved -= CHUNK_HEADER + spare->capacity;
        chunk_free(arena, spare);
    }
    chunk_free(arena, chunk);
}

static txml_arena_chunk *chunk_new(txml_arena *arena, size_t capacity) {
    if (capacity > (size_t)-1 - CHUNK_HEADER) {
        return NULL;
    }
    txml_arena_chunk *chunk = (txml_arena_chunk *)arena->backing.v->alloc(
        arena->backing.self, CHUNK_HEADER + capacity, CHUNK_ALIGNMENT);
    if (chunk == NULL) {
        return NULL;
    }
    chunk->prev = NULL;
    chunk->spare = NULL;
    chunk->capacity = capacity;
    chunk->offset = 0u;
    chunk->used_base = 0u;
    arena->reserved += CHUNK_HEADER + capacity;
    return chunk;
}

/* Bump inside `chunk`. Returns NULL when the request does not fit. */
static void *chunk_bump(txml_arena *arena, txml_arena_chunk *chunk, size_t size, size_t alignment) {
    const uintptr_t base = (uintptr_t)chunk_payload(chunk);
    const uintptr_t mask = (uintptr_t)alignment - 1u;
    const uintptr_t address = (base + chunk->offset + mask) & ~mask;
    const size_t start = (size_t)(address - base);

    if (start > chunk->capacity || size > chunk->capacity - start) {
        return NULL;
    }
    arena->used += (start + size) - chunk->offset;
    chunk->offset = start + size;
    return (void *)address;
}

txml_result txml_arena_init(txml_arena *arena, txml_allocator backing, size_t chunk_size) {
    if (arena == NULL || backing.v == NULL || backing.v->alloc == NULL || backing.v->free == NULL) {
        return TXML_ERR_INVALID;
    }
    arena->backing = backing;
    arena->head = NULL;
    arena->chunk_size = chunk_size == 0u ? TXML_ARENA_CHUNK_DEFAULT : chunk_size;
    arena->used = 0u;
    arena->reserved = 0u;
    return TXML_OK;
}

void *txml_arena_alloc(txml_arena *arena, size_t size, size_t alignment) {
    if (alignment == 0u || (alignment & (alignment - 1u)) != 0u) {
        return NULL;
    }
    if (arena->head != NULL) {
        void *ptr = chunk_bump(arena, arena->head, size, alignment);
        if (ptr != NULL) {
            return ptr;
        }
    }

    /* Room for the worst-case alignment padding when the chunk's own
     * alignment is not enough. */
    const size_t slack = alignment > CHUNK_ALIGNMENT ? alignment : 0u;
    if (size > (size_t)-1 - slack) {
        return NULL;
    }
    const size_t needed = size + slack;

    if (needed > arena->chunk_size) {
        /* Oversized: a chunk of exactly this request, linked behind the head
         * so the head's remaining space stays usable. */
        txml_arena_chunk *big = chunk_new(arena, needed);
        if (big == NULL) {
            return NULL;
        }
        if (arena->head == NULL) {
            arena->head = big;
        } else {
            big->prev = arena->head->prev;
            arena->head->prev = big;
        }
        big->used_base = arena->used;
        return chunk_bump(arena, big, size, alignment);
    }

    txml_arena_chunk *chunk = NULL;
    if (arena->head != NULL && arena->head->spare != NULL && arena->head->spare->capacity >= needed) {
        chunk = arena->head->spare;
        arena->head->spare = NULL;
        chunk->offset = 0u;
    } else {
        chunk = chunk_new(arena, arena->chunk_size);
        if (chunk == NULL) {
            return NULL;
        }
        if (arena->chunk_size < CHUNK_GROWTH_CAP) {
            arena->chunk_size *= 2u;
            if (arena->chunk_size > CHUNK_GROWTH_CAP) {
                arena->chunk_size = CHUNK_GROWTH_CAP;
            }
        }
    }
    chunk->prev = arena->head;
    chunk->used_base = arena->used;
    arena->head = chunk;
    return chunk_bump(arena, chunk, size, alignment);
}

txml_arena_mark txml_arena_position(const txml_arena *arena) {
    txml_arena_mark mark = { arena->head, arena->head == NULL ? 0u : arena->head->offset };
    return mark;
}

void txml_arena_release(txml_arena *arena, txml_arena_mark mark) {
    txml_arena_chunk *spare = NULL;
    txml_arena_chunk *chunk = arena->head;

    while (chunk != mark.chunk && chunk != NULL) {
        txml_arena_chunk *prev = chunk->prev;
        if (spare == NULL && mark.chunk != NULL) {
            /* Keep one regular chunk for the next growth. */
            if (chunk->spare != NULL) {
                arena->reserved -= CHUNK_HEADER + chunk->spare->capacity;
                chunk_free(arena, chunk->spare);
                chunk->spare = NULL;
            }
            spare = chunk;
        } else {
            chunk_release(arena, chunk);
        }
        chunk = prev;
    }

    arena->head = mark.chunk;
    if (mark.chunk == NULL) {
        arena->used = 0u;
        return;
    }
    mark.chunk->offset = mark.offset;
    arena->used = mark.chunk->used_base + mark.offset;
    if (spare != NULL) {
        if (mark.chunk->spare != NULL) {
            chunk_release(arena, mark.chunk->spare);
        }
        mark.chunk->spare = spare;
    }
}

void txml_arena_reset(txml_arena *arena) {
    txml_arena_chunk *chunk = arena->head;
    while (chunk != NULL) {
        txml_arena_chunk *prev = chunk->prev;
        chunk_release(arena, chunk);
        chunk = prev;
    }
    arena->head = NULL;
    arena->used = 0u;
    arena->reserved = 0u;
}

void txml_arena_destroy(txml_arena *arena) {
    txml_arena_reset(arena);
}
