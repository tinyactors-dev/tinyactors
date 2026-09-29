#pragma once
/*
 * The addon's containers (ADR 0009). Queues, enumeration pages and tables use
 * these types and nothing else. Every one allocates through a ta_allocator,
 * normally a category of the system's counting allocator (ADR 0014).
 *
 * - vec: a growable array. It doubles, so it is for small or bounded things;
 *   a queue that can hold millions of entries is a ring.
 * - ring: a FIFO of fixed-size chunks. Growing never copies the queue, and an
 *   empty ring keeps one spare chunk so a queue that fills and drains does
 *   not allocate each time.
 * - map: open addressing, uint64_t keys and values, linear probing, deletion
 *   by tombstone.
 * - slice: a pointer and a length.
 * - free list: fixed-size records that are reused instead of freed.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tinyactors.h"

typedef struct {
    size_t size;
    size_t alignment;
} tah_layout;

#define TAH_LAYOUT(type) ((tah_layout){ .size = sizeof(type), .alignment = _Alignof(type) })

typedef struct {
    const void *ptr;
    uint32_t len;
} tah_slice;

/* ------------------------------------------------------------------ vec */

typedef struct {
    ta_allocator allocator;
    tah_layout layout;
    void *items;
    uint32_t count;
    uint32_t capacity;
} tah_vec;

void tah_vec_init(tah_vec *vec, ta_allocator allocator, tah_layout layout);
ta_result tah_vec_reserve(tah_vec *vec, uint32_t count);
ta_result tah_vec_push(tah_vec *vec, const void *element);
void *tah_vec_at(const tah_vec *vec, uint32_t index);
void tah_vec_clear(tah_vec *vec);
tah_slice tah_vec_slice(const tah_vec *vec);
void tah_vec_destroy(tah_vec *vec);

/* ----------------------------------------------------------------- ring */

typedef struct tah_ring_chunk tah_ring_chunk;

typedef struct {
    tah_layout layout;
    uint32_t chunk_capacity; /* elements per chunk */
} tah_ring_shape;

typedef struct {
    ta_allocator allocator;
    tah_ring_shape shape;
    tah_ring_chunk *head;
    tah_ring_chunk *tail;
    tah_ring_chunk *spare;
    uint32_t head_index; /* the first element's index in the head chunk */
    uint32_t tail_index; /* elements used in the tail chunk */
    uint32_t count;
} tah_ring;

void tah_ring_init(tah_ring *ring, ta_allocator allocator, tah_ring_shape shape);
ta_result tah_ring_push(tah_ring *ring, const void *element);
/* Copies the front element to `out` and removes it; false when empty. */
bool tah_ring_pop(tah_ring *ring, void *out);
/* The front element, or NULL when empty. */
void *tah_ring_front(const tah_ring *ring);
uint32_t tah_ring_count(const tah_ring *ring);
void tah_ring_destroy(tah_ring *ring);

/* ------------------------------------------------------------------ map */

typedef struct {
    ta_allocator allocator;
    uint64_t *keys;
    uint64_t *values;
    uint8_t *states;
    uint32_t capacity; /* a power of two, or 0 */
    uint32_t count;
    uint32_t tombstones;
} tah_map;

void tah_map_init(tah_map *map, ta_allocator allocator);
/* Inserts or replaces. TA_ERR_NOMEM leaves the map as it was. */
ta_result tah_map_put(tah_map *map, uint64_t key, uint64_t value);
bool tah_map_get(const tah_map *map, uint64_t key, uint64_t *out);
bool tah_map_remove(tah_map *map, uint64_t key);
/*
 * Iteration: start `*cursor` at 0; each true answer gives one live entry.
 * The map must not change during the walk.
 */
bool tah_map_next(const tah_map *map, uint32_t *cursor, uint64_t *key, uint64_t *value);
uint32_t tah_map_count(const tah_map *map);
uint32_t tah_map_tombstones(const tah_map *map);
void tah_map_destroy(tah_map *map);

/* ------------------------------------------------------------ free list */

typedef struct tah_free_record tah_free_record;
typedef struct tah_free_chunk tah_free_chunk;

/*
 * Records come from chunks, not one allocation each: an empty list takes a
 * chunk holding as many records as are live (at least 8, at most 1024), so
 * a burst of N records costs about N/1024 allocator calls, not N. Chunks
 * are kept until destroy, as single records on the list were before.
 */
typedef struct {
    ta_allocator allocator;
    tah_layout layout; /* at least a pointer, so a free record can link */
    tah_free_record *head;
    tah_free_chunk *chunks;
    uint32_t free_count;
    uint32_t live_count;
} tah_free_list;

void tah_free_list_init(tah_free_list *list, ta_allocator allocator, tah_layout layout);
/* A record from the list, or from a new chunk; NULL when allocation fails. */
void *tah_free_list_take(tah_free_list *list);
/* Returns a record taken from this list. */
void tah_free_list_give(tah_free_list *list, void *record);
/* Frees every chunk. Every record taken must have been given back first
 * (live_count is 0): a record still taken is freed with its chunk. */
void tah_free_list_destroy(tah_free_list *list);
