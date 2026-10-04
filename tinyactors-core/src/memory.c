#include "memory.h"
#include "../include/tinyactors.h"

void *ta_mem_alloc(ta_allocator allocator, size_t size, size_t alignment) {
    if (allocator.v == NULL || allocator.v->alloc == NULL || size == 0) {
        return NULL;
    }
    return allocator.v->alloc(allocator.self, size, alignment);
}

void ta_mem_free(ta_allocator allocator, void *ptr, const ta_allocator_free_params *layout) {
    if (ptr == NULL || allocator.v == NULL || allocator.v->free == NULL) {
        return;
    }
    ta_allocator_free_params params = *layout;
    params.ptr = ptr;
    allocator.v->free(allocator.self, &params);
}

void ta_mem_copy(void *dst, const void *src, size_t size) {
    unsigned char *out = dst;
    const unsigned char *in = src;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = in[index];
    }
}

void ta_mem_zero(void *dst, size_t size) {
    unsigned char *out = dst;
    for (size_t index = 0; index < size; index += 1) {
        out[index] = 0;
    }
}
