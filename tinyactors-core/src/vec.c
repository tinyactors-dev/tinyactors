#include "vec.h"
#include "memory.h"
#include "../include/tinyactors.h"

void ta_vec_init(ta_vec *vec, ta_allocator allocator, const ta_tree_list_layout *layout) {
    *vec = (ta_vec){
        .allocator = allocator,
        .element_size = layout->element_size,
        .element_alignment = layout->element_alignment,
    };
}

void ta_vec_destroy(ta_vec *vec) {
    if (vec->items != NULL) {
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = NULL;
    vec->count = 0;
    vec->capacity = 0;
}

ta_result ta_vec_reserve(ta_vec *vec, uint32_t count) {
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
    void *items = ta_mem_alloc(vec->allocator, capacity * vec->element_size, vec->element_alignment);
    if (items == NULL) {
        return TA_ERR_NOMEM;
    }
    if (vec->items != NULL) {
        ta_mem_copy(items, vec->items, vec->count * vec->element_size);
        ta_mem_free(vec->allocator, vec->items,
                    &(ta_allocator_free_params){
                        .size = vec->capacity * vec->element_size,
                        .alignment = vec->element_alignment,
                    });
    }
    vec->items = items;
    vec->capacity = capacity;
    return TA_OK;
}

ta_result ta_vec_extend(ta_vec *vec, uint32_t count, uint32_t *out) {
    if (count > UINT32_MAX - vec->count) {
        return TA_ERR_NOMEM;
    }
    ta_result result = ta_vec_reserve(vec, vec->count + count);
    if (result != TA_OK) {
        return result;
    }
    if (out != NULL) {
        *out = vec->count;
    }
    if (count != 0) {
        ta_mem_zero((unsigned char *)vec->items + vec->count * vec->element_size,
                    count * vec->element_size);
    }
    vec->count += count;
    return TA_OK;
}

ta_result ta_vec_push(ta_vec *vec, const void *element) {
    uint32_t index = 0;
    ta_result result = ta_vec_extend(vec, 1, &index);
    if (result == TA_OK) {
        ta_mem_copy(ta_vec_at(vec, index), element, vec->element_size);
    }
    return result;
}

void *ta_vec_at(const ta_vec *vec, uint32_t index) {
    return (unsigned char *)vec->items + (size_t)index * vec->element_size;
}
