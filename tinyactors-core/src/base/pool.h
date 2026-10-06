/* pool.h — fixed-stride slot pool with u32 indices and generation handles.
 *
 * - Indices, not pointers; links between records are u32 indices.
 * - Self-relative offsets: the pool header and its storage must live in the
 *   same memory region, which can then be moved/snapshotted with memcpy.
 * - Lazy initialization: never-used slots are handed out from a high-water
 *   mark, so init is O(1) even for 100 000 slots.
 * - Free slots store the free-list link in their first 4 payload bytes; the
 *   rest of the slot is zeroed on free, so snapshots never carry residue.
 * - One generation byte per slot (separate array, so payloads keep 8-byte
 *   alignment). Even = free, odd = live. Handles = generation:8 | (index+1):24.
 */
#ifndef TINYACTORS_BASE_POOL_H
#define TINYACTORS_BASE_POOL_H

#include "base/config.h"

#define TA_HANDLE_INDEX_BITS 24u
#define TA_HANDLE_INDEX_MASK ((UINT32_C(1) << TA_HANDLE_INDEX_BITS) - 1u)
#define TA_POOL_MAX_CAPACITY (TA_HANDLE_INDEX_MASK - 1u)

typedef struct ta_pool {
    int64_t slots_relative;       /* slots - (char*)pool */
    int64_t generations_relative; /* generations - (char*)pool */
    uint32_t stride;              /* payload bytes, multiple of TA_ALIGN_MAX, >= 8 */
    uint32_t capacity;
    uint32_t free_head;       /* TA_NIL if empty */
    uint32_t high_water_mark; /* slots [high_water_mark, capacity) have never been allocated */
    uint32_t used;
    uint32_t pad;
} ta_pool;

TA_INLINE uint32_t ta_pool_stride(uint32_t payload) {
    return (uint32_t)ta_align_up(payload < 8 ? 8 : payload, TA_ALIGN_MAX);
}
TA_INLINE size_t ta_pool_slots_bytes(uint32_t payload, uint32_t capacity) {
    return (size_t)ta_pool_stride(payload) * capacity;
}
TA_INLINE size_t ta_pool_generations_bytes(uint32_t capacity) {
    return capacity;
}

/* `slots` must be TA_ALIGN_MAX-aligned; `generations` needs `capacity` bytes. */
void ta_pool_init(ta_pool *p, void *slots, unsigned char *generations, uint32_t payload,
                  uint32_t capacity);

uint32_t ta_pool_alloc(ta_pool *p); /* index, or TA_NIL when exhausted */
/* Copy state into a (possibly larger) pool, preserving indices/handles. */
void ta_pool_copy(ta_pool *dst, void *slots, unsigned char *generations, uint32_t capacity,
                  const ta_pool *source);
TA_INLINE uint32_t ta_pool_free_count(const ta_pool *pool) {
    return pool->capacity - pool->used;
}
void ta_pool_free(ta_pool *p, uint32_t index);

TA_INLINE unsigned char *ta_pool__slots(const ta_pool *pool) {
    return (unsigned char *)ta_pointer_add(pool, pool->slots_relative);
}
TA_INLINE unsigned char *ta_pool__generations(const ta_pool *pool) {
    return (unsigned char *)ta_pointer_add(pool, pool->generations_relative);
}
TA_INLINE void *ta_pool_at(const ta_pool *pool, uint32_t index) {
    TA_ASSERT(index < pool->high_water_mark);
    return ta_pointer_add(ta_pool__slots(pool), (int64_t)index * pool->stride);
}
TA_INLINE int ta_pool_live(const ta_pool *pool, uint32_t index) {
    return index < pool->high_water_mark && (ta_pool__generations(pool)[index] & 1u);
}

TA_INLINE uint32_t ta_pool_handle(const ta_pool *pool, uint32_t index) {
    TA_ASSERT(ta_pool_live(pool, index));
    return ((uint32_t)ta_pool__generations(pool)[index] << TA_HANDLE_INDEX_BITS) | (index + 1u);
}
/* Returns the index for a live handle, TA_NIL for 0/stale/out-of-range. */
TA_INLINE uint32_t ta_pool_resolve(const ta_pool *pool, uint32_t h) {
    uint32_t index = (h & TA_HANDLE_INDEX_MASK) - 1u;
    if (index >= pool->high_water_mark) return TA_NIL; /* also catches h == 0 (index wraps) */
    if (ta_pool__generations(pool)[index] != (unsigned char)(h >> TA_HANDLE_INDEX_BITS))
        return TA_NIL;
    if (!(ta_pool__generations(pool)[index] & 1u)) return TA_NIL;
    return index;
}

#endif /* TINYACTORS_BASE_POOL_H */
