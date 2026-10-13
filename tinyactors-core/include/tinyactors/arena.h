/* Call-scoped scratch for embedders/bindings, not relocatable system memory.
 * Linked chunks keep addresses stable; nested calls use nested marks. The
 * caller lends the backing allocator. No libc or default allocator is used. */
#ifndef TINYACTORS_ARENA_H
#define TINYACTORS_ARENA_H

#include <tinyactors/allocator.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ta_growable_arena_chunk ta_growable_arena_chunk;

typedef struct ta_growable_arena_config {
    uint32_t size; /* sizeof(ta_growable_arena_config) */
    uint32_t pad;
    size_t chunk_size; /* regular payload bytes; 0: 65536 */
    const ta_allocator *allocator; /* borrowed until arena_free */
} ta_growable_arena_config;

/* API-maintained fields. used includes alignment gaps in active chunks;
 * reserved includes headers and payloads of all active and kept chunks.
 * Neither counts user objects stored through pointers in this arena. */
typedef struct ta_growable_arena {
    const ta_allocator *allocator;
    ta_growable_arena_chunk *head;
    ta_growable_arena_chunk *kept;
    size_t chunk_size, used, reserved;
} ta_growable_arena;

/* A borrowed position in this arena's current allocation history. A mark
 * becomes unusable after releasing to an earlier mark, or after arena_free.
 * Keep marks nested on one thread; they are not persistent snapshots. */
typedef struct ta_growable_arena_mark {
    ta_growable_arena_chunk *chunk;
    size_t offset;
} ta_growable_arena_mark;

/* Initialize without allocating. config is read during the call, but its
 * allocator object/context must outlive the arena and its chunks. Missing
 * callbacks, NULL or a short config leave an inert arena whose alloc refuses.
 * Do not init over live chunks. Serialize operations on one owning thread;
 * backing callbacks must not mutate this arena. */
TA_API void ta_growable_arena_init(ta_growable_arena *arena, const ta_growable_arena_config *config);

/* Return contiguous, uninitialized storage aligned to 1, 2, 4 or 8. Reject
 * zero size, invalid alignment, arithmetic overflow or allocator refusal
 * with NULL and no change to allocations, counters or marks. New chunks
 * have max(chunk_size,size) payload, so an oversized request fits in one.
 * Allocations never move and live until release to a preceding mark or free.
 * Reuse kept chunks before allocating; an identical warmed sequence needs
 * no backing allocator calls. Release keeps even oversized chunks. */
TA_API void *ta_growable_arena_alloc(ta_growable_arena *arena, size_t size, size_t align);

/* Capture the current position without allocating. Take a mark at call
 * entry and release it on EVERY exit, after consumers finish reading the
 * call's storage. An empty arena produces {NULL,0}. */
TA_API ta_growable_arena_mark ta_growable_arena_position(const ta_growable_arena *arena);

/* End all allocations after mark, preserving earlier bytes and retaining
 * every chunk for reuse. Chunks are not zeroed and no destructors run:
 * release resources stored in the scratch before releasing their bytes.
 * mark must be from this arena's still-active history. Detectably invalid
 * marks do nothing; a stale mark reused after an enclosing release cannot
 * always be detected, and passing one is a caller error. */
TA_API void ta_growable_arena_release(ta_growable_arena *arena, ta_growable_arena_mark mark);

/* A by-value allocator view, valid while the arena lives: alloc bumps;
 * free is a no-op. Store the returned struct somewhere that outlives every
 * consumer borrowing its address (e.g. a CBOR writer or table). Finish
 * those consumers before releasing their enclosing mark. Writer growth
 * retains earlier blocks until release; budget for those blocks too. */
TA_API ta_allocator ta_growable_arena_allocator(ta_growable_arena *arena);

/* Return active AND kept chunks to the backing allocator and clear counts.
 * All storage, marks and allocator views end here. Repeated free is safe;
 * the arena keeps its config and may allocate again as a new history. */
TA_API void ta_growable_arena_free(ta_growable_arena *arena);

#ifdef __cplusplus
}
#endif
#endif
