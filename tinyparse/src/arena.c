/*
** arena.c -- the region allocator every byte of pocketparse comes from.
**
** pocketparse never calls malloc. The host hands pocketparse_open one block of
** memory (pocketparse_memory) and, optionally, a grow callback for more. This
** file turns that grant into arenas: bump allocators that are never freed one
** object at a time, only reset as a whole.
**
** Three pieces cooperate:
**
**   pp_pool   owns the grant. It bump-allocates raw bytes from the current host
**             block, asks the host's grow callback for a FRESH block when the
**             current one is spent (never a realloc, so no address ever moves),
**             and keeps a free list of chunks that arena resets handed back.
**   pp_chunk  one contiguous run the pool hands an arena. The arena bumps
**             through it; a reset threads it onto the pool's free list.
**   pp_arena  a bump pointer over a chain of chunks drawn from one pool.
**
** The engine owns one long-lived arena over the whole grant. Each parser gets a
** CHILD arena on the same pool (pp_arena_child): pocketparse_parser_free resets
** it, every chunk the parse used goes back on the free list, and the next parse
** reuses them, so one engine parses a whole codebase without accreting trees.
**
** Allocation rules, which the rest of the library relies on:
**   - every hand-out is aligned to PP_ARENA_ALIGN bytes (offsets are rounded up
**     relative to the block start, so the host's base should be aligned too);
**   - pp_alloc returns zeroed bytes, pp_alloc_raw does not;
**   - NULL is the single out-of-memory signal, also for a request too large to
**     round up (which ax's allocator let wrap around to a tiny block);
**   - an arena never reallocates, so growing an array means allocating a fresh,
**     larger block and copying;
**   - a request larger than the chunk floor gets a chunk of its own size;
**   - a reused chunk is the FIRST one on the free list that is large enough.
**
** Freestanding: no libc and no statics; the only state is in the grant.
*/
#include "internal.h"

enum {
    PP_ARENA_ALIGN     = 16,      /* every hand-out is aligned to this       */
    PP_ARENA_CHUNK_MIN = 4096     /* default chunk floor; chunk_minimum wins */
};

/* ** One run of bytes a pool hands to an arena. While it backs an arena it
** bump-allocates from `used'; once the arena is reset it is threaded onto the
** pool's free list through `next' and reused. The payload follows the header. */
struct pp_chunk {
    pp_chunk *next;     /* arena chain, or pool free-list link */
    size_t    capacity; /* usable payload bytes                */
    size_t    used;     /* bump offset within the payload      */
};

/* ** The owner of the host grant. It bump-allocates from the current host block
** (block / capacity / used), asks memory.grow for a fresh block when one is
** spent, and keeps the free list of chunks returned by arena resets. */
struct pp_pool {
    pocketparse_memory memory;        /* the host grant and its grow callback   */
    char              *block;         /* the current host block                 */
    size_t             capacity;
    size_t             used;
    size_t             chunk_minimum; /* resolved floor on a pulled chunk       */
    pp_chunk          *freed;         /* chunks returned by arena resets        */
};

/* ** Round n up to the next multiple of PP_ARENA_ALIGN. */
static size_t pp_align_up(size_t n)
{
    return (n + (PP_ARENA_ALIGN - 1)) & ~(size_t)(PP_ARENA_ALIGN - 1);
}

/* ** The size of a chunk header, rounded so the payload after it is aligned. */
static size_t pp_chunk_header(void)
{
    return pp_align_up(sizeof(pp_chunk));
}

/* ** The usable payload of a chunk: the bytes just past its header. */
static void *pp_chunk_data(pp_chunk *chunk)
{
    return (char *)chunk + pp_chunk_header();
}

/* ** Bump `need' raw bytes off the current host block, or NULL when the grant is
** spent and the host's grow callback is absent or refuses. A grow returns a
** fresh block whose true usable size comes back through `got'; the pool keeps
** the whole of it and spends it chunk by chunk before growing again. */
static void *pp_pool_raw(pp_pool *pool, size_t need)
{
    size_t offset = pp_align_up(pool->used);

    if (offset > pool->capacity || need > pool->capacity - offset) {
        size_t want = need > pool->chunk_minimum ? need : pool->chunk_minimum;
        size_t got  = want;
        void  *block;
        if (pool->memory.grow == 0)
            return 0;
        block = pool->memory.grow(pool->memory.user, want, &got);
        if (block == 0)
            return 0;
        pool->block    = (char *)block;
        pool->capacity = got;
        pool->used     = 0;
        offset         = 0;
    }
    pool->used = offset + need;
    return pool->block + offset;
}

/* ** A chunk whose payload holds at least `need' bytes: the first large-enough
** chunk on the free list, else a fresh one carved from the grant. NULL when the
** grant is spent. */
static pp_chunk *pp_pool_take(pp_pool *pool, size_t need)
{
    pp_chunk **link = &pool->freed;
    pp_chunk  *chunk;

    for (; *link != 0; link = &(*link)->next) {
        if ((*link)->capacity >= need) {
            chunk       = *link;
            *link       = chunk->next;
            chunk->next = 0;
            chunk->used = 0;
            return chunk;
        }
    }
    chunk = (pp_chunk *)pp_pool_raw(pool, pp_chunk_header() + need);
    if (chunk == 0)
        return 0;
    chunk->capacity = need;
    chunk->used     = 0;
    chunk->next     = 0;
    return chunk;
}

/* ** Open the root arena over a host grant. The pool and the arena are carved
** from the front of the block and the pool manages the bytes after them. NULL
** when the grant cannot hold that bookkeeping plus one chunk's working room. */
pp_arena *pp_arena_open(pocketparse_memory memory)
{
    pp_pool  *pool;
    pp_arena *arena;
    size_t    pool_offset  = pp_align_up(sizeof(pp_pool));
    size_t    offset       = pool_offset + pp_align_up(sizeof(pp_arena));
    size_t    chunk_minimum = memory.chunk_minimum != 0
                              ? memory.chunk_minimum : PP_ARENA_CHUNK_MIN;

    if (memory.base == 0 || memory.size < offset + chunk_minimum)
        return 0;
    pool  = (pp_pool *)memory.base;
    arena = (pp_arena *)((char *)memory.base + pool_offset);
    __builtin_memset(pool, 0, sizeof *pool);
    pool->memory        = memory;
    pool->block         = (char *)memory.base;
    pool->capacity      = memory.size;
    pool->used          = offset;
    pool->chunk_minimum = chunk_minimum;
    arena->pool = pool;
    arena->head = 0;
    return arena;
}

/* ** A child arena on the parent's pool. Its bookkeeping is allocated from the
** parent, so the parent must outlive it; its chunks come from, and return to,
** the shared pool. NULL on a spent grant. */
pp_arena *pp_arena_child(pp_arena *parent)
{
    pp_arena *child;
    if (parent == 0 || parent->pool == 0)
        return 0;
    child = (pp_arena *)pp_alloc(parent, sizeof *child);
    if (child == 0)
        return 0;
    child->pool = parent->pool;
    child->head = 0;
    return child;
}

/* ** Hand every chunk of the arena back to its pool's free list and leave the
** arena empty but usable. Everything allocated from it becomes invalid. */
void pp_arena_reset(pp_arena *arena)
{
    pp_chunk *chunk;
    pp_chunk *next;
    if (arena == 0)
        return;
    for (chunk = arena->head; chunk != 0; chunk = next) {
        next              = chunk->next;
        chunk->next       = arena->pool->freed;
        arena->pool->freed = chunk;
    }
    arena->head = 0;
}

/* ** Allocate `n' aligned bytes WITHOUT clearing them, or NULL. Served from the
** head chunk when it fits; otherwise a new chunk (at least the chunk floor,
** larger for an oversized request) becomes the head. The bytes hold whatever the
** chunk last carried, so only a caller that writes before it reads uses this. */
void *pp_alloc_raw(pp_arena *arena, size_t n)
{
    size_t need;
    void  *result;

    /* A request that cannot be rounded up, or whose chunk header would not fit
    ** beside it, can never be served: refuse it rather than let the size wrap
    ** around to a tiny allocation (ax's allocator had that wrap). */
    if (n > (size_t)-1 - PP_ARENA_ALIGN - pp_chunk_header())
        return 0;
    need = pp_align_up(n);

    if (arena->head == 0 || arena->head->capacity - arena->head->used < need) {
        size_t    want  = need > arena->pool->chunk_minimum
                          ? need : arena->pool->chunk_minimum;
        pp_chunk *chunk = pp_pool_take(arena->pool, want);
        if (chunk == 0)
            return 0;
        chunk->next = arena->head;
        arena->head = chunk;
    }
    result = (char *)pp_chunk_data(arena->head) + arena->head->used;
    arena->head->used += need;
    return result;
}

/* ** Allocate `n' zeroed, aligned bytes, or NULL on a spent grant. */
void *pp_alloc(pp_arena *arena, size_t n)
{
    void *result = pp_alloc_raw(arena, n);
    if (result != 0)
        __builtin_memset(result, 0, pp_align_up(n));
    return result;
}
