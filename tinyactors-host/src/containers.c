#include "containers.h"

#include "memory.h"

#include "allocator.h"

static size_t tah_round_up(size_t value, size_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

static size_t tah_stride(tah_layout layout) {
    return tah_round_up(layout.size, layout.alignment);
}

static void tah_release(ta_allocator allocator, void *ptr, tah_layout layout) {
    tah_free(allocator, &(ta_allocator_free_params){ .ptr = ptr, .size = layout.size, .alignment = layout.alignment });
}

/* ------------------------------------------------------------------ vec */

void tah_vec_init(tah_vec *vec, ta_allocator allocator, tah_layout layout) {
    *vec = (tah_vec){ .allocator = allocator, .layout = layout };
}

static tah_layout tah_vec_block(const tah_vec *vec, uint32_t capacity) {
    return (tah_layout){ .size = tah_stride(vec->layout) * capacity, .alignment = vec->layout.alignment };
}

ta_result tah_vec_reserve(tah_vec *vec, uint32_t count) {
    if (count <= vec->capacity) {
        return TA_OK;
    }
    uint32_t capacity = vec->capacity == 0 ? 8u : vec->capacity;
    while (capacity < count) {
        if (capacity > UINT32_MAX / 2u) {
            return TA_ERR_NOMEM;
        }
        capacity *= 2u;
    }
    tah_layout block = tah_vec_block(vec, capacity);
    void *items = tah_alloc(vec->allocator, block.size, block.alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->count != 0) {
        tah_mem_copy(items, vec->items, tah_stride(vec->layout) * vec->count);
    }
    if (vec->items != NULL) {
        tah_release(vec->allocator, vec->items, tah_vec_block(vec, vec->capacity));
    }
    vec->items = items;
    vec->capacity = capacity;
    return TA_OK;
}

ta_result tah_vec_push(tah_vec *vec, const void *element) {
    if (vec->count == UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    ta_result result = tah_vec_reserve(vec, vec->count + 1u);
    if (result != TA_OK) {
        return result;
    }
    tah_mem_copy(tah_vec_at(vec, vec->count), element, vec->layout.size);
    vec->count += 1u;
    return TA_OK;
}

void *tah_vec_at(const tah_vec *vec, uint32_t index) {
    return (unsigned char *)vec->items + tah_stride(vec->layout) * index;
}

void tah_vec_clear(tah_vec *vec) {
    vec->count = 0;
}

tah_slice tah_vec_slice(const tah_vec *vec) {
    return (tah_slice){ .ptr = vec->items, .len = vec->count };
}

void tah_vec_destroy(tah_vec *vec) {
    if (vec->items != NULL) {
        tah_release(vec->allocator, vec->items, tah_vec_block(vec, vec->capacity));
    }
    tah_vec_init(vec, vec->allocator, vec->layout);
}

/* ----------------------------------------------------------------- ring */

struct tah_ring_chunk {
    tah_ring_chunk *next;
};

static size_t tah_ring_data_offset(const tah_ring *ring) {
    return tah_round_up(sizeof(tah_ring_chunk), ring->shape.layout.alignment);
}

static tah_layout tah_ring_chunk_layout(const tah_ring *ring) {
    size_t alignment = ring->shape.layout.alignment > _Alignof(tah_ring_chunk) ? ring->shape.layout.alignment
                                                                               : _Alignof(tah_ring_chunk);
    return (tah_layout){
        .size = tah_ring_data_offset(ring) + tah_stride(ring->shape.layout) * ring->shape.chunk_capacity,
        .alignment = alignment,
    };
}

static void *tah_ring_slot(const tah_ring *ring, tah_ring_chunk *chunk, uint32_t index) {
    return (unsigned char *)chunk + tah_ring_data_offset(ring) + tah_stride(ring->shape.layout) * index;
}

void tah_ring_init(tah_ring *ring, ta_allocator allocator, tah_ring_shape shape) {
    if (shape.chunk_capacity == 0) {
        shape.chunk_capacity = 1;
    }
    *ring = (tah_ring){ .allocator = allocator, .shape = shape };
}

static tah_ring_chunk *tah_ring_take_chunk(tah_ring *ring) {
    tah_ring_chunk *chunk = ring->spare;
    if (chunk != NULL) {
        ring->spare = NULL;
    } else {
        tah_layout layout = tah_ring_chunk_layout(ring);
        chunk = tah_alloc(ring->allocator, layout.size, layout.alignment);
        if (chunk == NULL) {
            return NULL;
        }
    }
    chunk->next = NULL;
    return chunk;
}

/* Keeps the first chunk that empties as the spare and frees the rest. */
static void tah_ring_drop_chunk(tah_ring *ring, tah_ring_chunk *chunk) {
    if (ring->spare == NULL) {
        ring->spare = chunk;
        return;
    }
    tah_release(ring->allocator, chunk, tah_ring_chunk_layout(ring));
}

ta_result tah_ring_push(tah_ring *ring, const void *element) {
    if (ring->count == UINT32_MAX) {
        return TA_ERR_NOMEM;
    }
    if (ring->tail == NULL || ring->tail_index == ring->shape.chunk_capacity) {
        tah_ring_chunk *chunk = tah_ring_take_chunk(ring);
        if (chunk == NULL) {
            return TA_ERR_NOMEM;
        }
        if (ring->tail == NULL) {
            ring->head = chunk;
            ring->head_index = 0;
        } else {
            ring->tail->next = chunk;
        }
        ring->tail = chunk;
        ring->tail_index = 0;
    }
    tah_mem_copy(tah_ring_slot(ring, ring->tail, ring->tail_index), element, ring->shape.layout.size);
    ring->tail_index += 1u;
    ring->count += 1u;
    return TA_OK;
}

void *tah_ring_front(const tah_ring *ring) {
    return ring->count == 0 ? NULL : tah_ring_slot(ring, ring->head, ring->head_index);
}

bool tah_ring_pop(tah_ring *ring, void *out) {
    if (ring->count == 0) {
        return false;
    }
    if (out != NULL) {
        tah_mem_copy(out, tah_ring_slot(ring, ring->head, ring->head_index), ring->shape.layout.size);
    }
    ring->head_index += 1u;
    ring->count -= 1u;
    if (ring->count == 0) {
        tah_ring_drop_chunk(ring, ring->head);
        ring->head = NULL;
        ring->tail = NULL;
        ring->head_index = 0;
        ring->tail_index = 0;
    } else if (ring->head_index == ring->shape.chunk_capacity) {
        tah_ring_chunk *drained = ring->head;
        ring->head = drained->next;
        ring->head_index = 0;
        tah_ring_drop_chunk(ring, drained);
    }
    return true;
}

uint32_t tah_ring_count(const tah_ring *ring) {
    return ring->count;
}

void tah_ring_destroy(tah_ring *ring) {
    tah_layout layout = tah_ring_chunk_layout(ring);
    tah_ring_chunk *chunk = ring->head;
    while (chunk != NULL) {
        tah_ring_chunk *next = chunk->next;
        tah_release(ring->allocator, chunk, layout);
        chunk = next;
    }
    if (ring->spare != NULL) {
        tah_release(ring->allocator, ring->spare, layout);
    }
    tah_ring_init(ring, ring->allocator, ring->shape);
}

/* ------------------------------------------------------------------ map */

enum { TAH_MAP_EMPTY = 0, TAH_MAP_FULL = 1, TAH_MAP_TOMBSTONE = 2 };

/* splitmix64's finalizer: sequential keys (slots, pointers) spread well. */
static uint64_t tah_map_hash(uint64_t key) {
    key ^= key >> 30;
    key *= 0xbf58476d1ce4e5b9u;
    key ^= key >> 27;
    key *= 0x94d049bb133111ebu;
    key ^= key >> 31;
    return key;
}

static tah_layout tah_map_block(uint32_t capacity) {
    return (tah_layout){ .size = (size_t)capacity * (2u * sizeof(uint64_t) + 1u), .alignment = _Alignof(uint64_t) };
}

void tah_map_init(tah_map *map, ta_allocator allocator) {
    *map = (tah_map){ .allocator = allocator };
}

/* The slot holding `key`, or UINT32_MAX. */
static uint32_t tah_map_find(const tah_map *map, uint64_t key) {
    if (map->capacity == 0) {
        return UINT32_MAX;
    }
    uint32_t mask = map->capacity - 1u;
    for (uint32_t probe = 0, slot = (uint32_t)tah_map_hash(key) & mask; probe < map->capacity;
         probe += 1, slot = (slot + 1u) & mask) {
        if (map->states[slot] == TAH_MAP_EMPTY) {
            return UINT32_MAX;
        }
        if (map->states[slot] == TAH_MAP_FULL && map->keys[slot] == key) {
            return slot;
        }
    }
    return UINT32_MAX;
}

/* Places a key known to be absent into a table with no tombstones. */
static void tah_map_place(tah_map *map, uint64_t key, uint64_t value) {
    uint32_t mask = map->capacity - 1u;
    uint32_t slot = (uint32_t)tah_map_hash(key) & mask;
    while (map->states[slot] == TAH_MAP_FULL) {
        slot = (slot + 1u) & mask;
    }
    map->keys[slot] = key;
    map->values[slot] = value;
    map->states[slot] = TAH_MAP_FULL;
    map->count += 1u;
}

/* Rebuilds the table at `capacity`, which also clears every tombstone. */
static ta_result tah_map_rehash(tah_map *map, uint32_t capacity) {
    tah_layout block = tah_map_block(capacity);
    unsigned char *memory = tah_alloc(map->allocator, block.size, block.alignment);
    if (memory == NULL) {
        return TA_ERR_NOMEM;
    }
    tah_map old = *map;
    map->keys = (uint64_t *)memory;
    map->values = map->keys + capacity;
    map->states = (uint8_t *)(map->values + capacity);
    tah_mem_set(map->states, TAH_MAP_EMPTY, capacity);
    map->capacity = capacity;
    map->count = 0;
    map->tombstones = 0;
    for (uint32_t slot = 0; slot < old.capacity; slot += 1) {
        if (old.states[slot] == TAH_MAP_FULL) {
            tah_map_place(map, old.keys[slot], old.values[slot]);
        }
    }
    if (old.keys != NULL) {
        tah_release(map->allocator, old.keys, tah_map_block(old.capacity));
    }
    return TA_OK;
}

ta_result tah_map_put(tah_map *map, uint64_t key, uint64_t value) {
    uint32_t found = tah_map_find(map, key);
    if (found != UINT32_MAX) {
        map->values[found] = value;
        return TA_OK;
    }
    /* Keep live entries and tombstones under 3/4; a rebuild leaves live
     * entries at most half the table. */
    if ((uint64_t)(map->count + map->tombstones + 1u) * 4u > (uint64_t)map->capacity * 3u) {
        uint32_t capacity = map->capacity == 0 ? 16u : map->capacity;
        while ((uint64_t)(map->count + 1u) * 2u > capacity) {
            if (capacity > UINT32_MAX / 2u) {
                return TA_ERR_NOMEM;
            }
            capacity *= 2u;
        }
        ta_result result = tah_map_rehash(map, capacity);
        if (result != TA_OK) {
            return result;
        }
    }
    uint32_t mask = map->capacity - 1u;
    uint32_t slot = (uint32_t)tah_map_hash(key) & mask;
    while (map->states[slot] == TAH_MAP_FULL) {
        slot = (slot + 1u) & mask;
    }
    if (map->states[slot] == TAH_MAP_TOMBSTONE) {
        map->tombstones -= 1u;
    }
    map->keys[slot] = key;
    map->values[slot] = value;
    map->states[slot] = TAH_MAP_FULL;
    map->count += 1u;
    return TA_OK;
}

bool tah_map_get(const tah_map *map, uint64_t key, uint64_t *out) {
    uint32_t slot = tah_map_find(map, key);
    if (slot == UINT32_MAX) {
        return false;
    }
    if (out != NULL) {
        *out = map->values[slot];
    }
    return true;
}

bool tah_map_remove(tah_map *map, uint64_t key) {
    uint32_t slot = tah_map_find(map, key);
    if (slot == UINT32_MAX) {
        return false;
    }
    map->states[slot] = TAH_MAP_TOMBSTONE;
    map->count -= 1u;
    map->tombstones += 1u;
    return true;
}

bool tah_map_next(const tah_map *map, uint32_t *cursor, uint64_t *key, uint64_t *value) {
    while (*cursor < map->capacity) {
        uint32_t slot = *cursor;
        *cursor += 1u;
        if (map->states[slot] == TAH_MAP_FULL) {
            *key = map->keys[slot];
            *value = map->values[slot];
            return true;
        }
    }
    return false;
}

uint32_t tah_map_count(const tah_map *map) {
    return map->count;
}

uint32_t tah_map_tombstones(const tah_map *map) {
    return map->tombstones;
}

void tah_map_destroy(tah_map *map) {
    if (map->keys != NULL) {
        tah_release(map->allocator, map->keys, tah_map_block(map->capacity));
    }
    tah_map_init(map, map->allocator);
}

/* ------------------------------------------------------------ free list */

struct tah_free_record {
    tah_free_record *next;
};

struct tah_free_chunk {
    tah_free_chunk *next;
    size_t bytes; /* the whole allocation, header included */
};

#define TAH_FREE_CHUNK_MIN 8u
#define TAH_FREE_CHUNK_MAX 1024u

void tah_free_list_init(tah_free_list *list, ta_allocator allocator, tah_layout layout) {
    if (layout.size < sizeof(tah_free_record)) {
        layout.size = sizeof(tah_free_record);
    }
    if (layout.alignment < _Alignof(tah_free_record)) {
        layout.alignment = _Alignof(tah_free_record);
    }
    /* Records sit back to back in a chunk, so each size is a multiple of the alignment. */
    layout.size = (layout.size + layout.alignment - 1u) & ~(layout.alignment - 1u);
    *list = (tah_free_list){ .allocator = allocator, .layout = layout };
}

/* A chunk's alignment: its header's or its records', whichever is larger. */
static size_t tah_free_chunk_alignment(const tah_free_list *list) {
    return list->layout.alignment < _Alignof(tah_free_chunk) ? _Alignof(tah_free_chunk) : list->layout.alignment;
}

/* Where a chunk's first record starts: past the header, aligned. */
static size_t tah_free_chunk_offset(const tah_free_list *list) {
    size_t alignment = tah_free_chunk_alignment(list);
    return (sizeof(tah_free_chunk) + alignment - 1u) & ~(alignment - 1u);
}

/* Puts a new chunk's records on the list; false when allocation fails. */
static bool tah_free_list_grow(tah_free_list *list) {
    uint32_t count = list->live_count;
    if (count < TAH_FREE_CHUNK_MIN) {
        count = TAH_FREE_CHUNK_MIN;
    }
    if (count > TAH_FREE_CHUNK_MAX) {
        count = TAH_FREE_CHUNK_MAX;
    }
    size_t offset = tah_free_chunk_offset(list);
    size_t alignment = tah_free_chunk_alignment(list);
    size_t bytes = offset + (size_t)count * list->layout.size;
    tah_free_chunk *chunk = tah_alloc(list->allocator, bytes, alignment);
    if (chunk == NULL) {
        return false;
    }
    chunk->bytes = bytes;
    chunk->next = list->chunks;
    list->chunks = chunk;
    uint8_t *records = (uint8_t *)chunk + offset;
    for (uint32_t at = count; at > 0; at -= 1u) {
        tah_free_record *record = (tah_free_record *)(void *)(records + (size_t)(at - 1u) * list->layout.size);
        record->next = list->head;
        list->head = record;
    }
    list->free_count += count;
    return true;
}

void *tah_free_list_take(tah_free_list *list) {
    if (list->head == NULL && !tah_free_list_grow(list)) {
        return NULL;
    }
    tah_free_record *record = list->head;
    list->head = record->next;
    list->free_count -= 1u;
    list->live_count += 1u;
    return record;
}

void tah_free_list_give(tah_free_list *list, void *record) {
    if (record == NULL) {
        return;
    }
    tah_free_record *free_record = record;
    free_record->next = list->head;
    list->head = free_record;
    list->free_count += 1u;
    list->live_count -= 1u;
}

void tah_free_list_destroy(tah_free_list *list) {
    size_t alignment = tah_free_chunk_alignment(list);
    tah_free_chunk *chunk = list->chunks;
    while (chunk != NULL) {
        tah_free_chunk *next = chunk->next;
        tah_release(list->allocator, chunk, (tah_layout){ .size = chunk->bytes, .alignment = alignment });
        chunk = next;
    }
    list->chunks = NULL;
    list->head = NULL;
    list->free_count = 0;
}
