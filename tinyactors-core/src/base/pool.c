#include "base/pool.h"
#include "base/memory.h"

void ta_pool_init(ta_pool *pool, void *slots, unsigned char *generations, uint32_t payload,
                  uint32_t capacity) {
    TA_ASSERT(((uintptr_t)slots & (TA_ALIGN_MAX - 1)) == 0);
    TA_ASSERT(capacity <= TA_POOL_MAX_CAPACITY);
    pool->slots_relative = (int64_t)((intptr_t)slots - (intptr_t)pool);
    pool->generations_relative = (int64_t)((intptr_t)generations - (intptr_t)pool);
    pool->stride = ta_pool_stride(payload);
    pool->capacity = capacity;
    pool->free_head = TA_NIL;
    pool->high_water_mark = 0;
    pool->used = 0;
    pool->pad = 0;
}

uint32_t ta_pool_alloc(ta_pool *pool) {
    uint32_t index;
    if (pool->free_head != TA_NIL) {
        index = pool->free_head;
        pool->free_head = *(uint32_t *)ta_pool_at(pool, index);
    } else if (pool->high_water_mark < pool->capacity) {
        index = pool->high_water_mark++;
        ta_pool__generations(pool)[index] = 0;
    } else {
        return TA_NIL;
    }
    ta_pool__generations(pool)[index]++; /* even -> odd: live */
    pool->used++;
    return index;
}

void ta_pool_free(ta_pool *pool, uint32_t index) {
    TA_ASSERT(ta_pool_live(pool, index));
    ta_pool__generations(pool)[index]++; /* odd -> even: free (invalidates handles) */
    void *at = ta_pool_at(pool, index);
    *(uint32_t *)at = pool->free_head;
    ta_memset((unsigned char *)at + 4, 0, pool->stride - 4); /* dead bytes: never heap residue */
    pool->free_head = index;
    pool->used--;
}

void ta_pool_copy(ta_pool *dst, void *slots, unsigned char *generations, uint32_t capacity,
                  const ta_pool *source) {
    TA_ASSERT(capacity >= source->high_water_mark);
    uint32_t payload = source->stride;
    ta_pool_init(dst, slots, generations, payload, capacity);
    ta_memcpy(slots, ta_pool__slots(source), (size_t)source->high_water_mark * source->stride);
    ta_memcpy(generations, ta_pool__generations(source), source->high_water_mark);
    dst->free_head = source->free_head;
    dst->high_water_mark = source->high_water_mark;
    dst->used = source->used;
}
