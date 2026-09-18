#pragma once

/* Private base header: every implementation module reaches the public API here. */
#include "../include/tinyactors.h"

#define TA_ARRAY_COUNT(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))
#define TA_ALIGN_UP(v, a) (((v) + ((a) - 1u)) & ~((a) - 1u))

typedef struct {
    uint32_t offset;
    uint32_t length;
} ta_span32;

typedef struct {
    uint32_t slot;
    uint32_t generation;
} ta_actor_key;

/* Freestanding memory helpers over an injected ta_allocator. */
void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment);
void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout);
void ta_mem_copy(void *dst, const void *src, size_t size);
void ta_mem_zero(void *dst, size_t size);
bool ta_str_equal(ta_str left, ta_str right);

/*
 * A growable array of fixed-size elements. `items` is reallocated as it
 * grows, so callers keep indices, never element pointers, across a push.
 */
typedef struct {
    ta_allocator allocator;
    void *items;
    uint32_t count;
    uint32_t capacity;
    size_t element_size;
    size_t element_alignment;
} ta_vec;

void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout);
void ta_vec_destroy(ta_vec *vec);
ta_result ta_vec_reserve(ta_vec *vec, uint32_t count);
/* Appends `count` zeroed elements and stores the index of the first in `*out`. */
ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out);
ta_result ta_vec_push(ta_vec *vec, const void *element);
void *ta_vec_at(const ta_vec *vec, uint32_t index);

/* Bitsets of 64-bit words, used for state sets. */
static inline uint32_t ta_bits_words(uint32_t bits) { return (bits + 63u) / 64u; }
static inline bool ta_bits_get(const uint64_t *words, uint32_t bit) {
    return (words[bit / 64u] >> (bit % 64u)) & 1u;
}
static inline void ta_bits_put(uint64_t *words, uint32_t bit, bool value) {
    uint64_t mask = (uint64_t)1u << (bit % 64u);
    if (value) {
        words[bit / 64u] |= mask;
    } else {
        words[bit / 64u] &= ~mask;
    }
}

#define TA_VEC_LAYOUT(T) (&(ta_tree_list_layout){ .element_size = sizeof(T), .element_alignment = _Alignof(T) })
