#include "allocator.h"

#include <stddef.h>
#include <stdlib.h>

static void *tat_libc_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) {
        return malloc(size);
    }
    if (size > SIZE_MAX - (alignment - 1u)) {
        return NULL;
    }
    size_t rounded = (size + alignment - 1u) & ~(alignment - 1u);
    return aligned_alloc(alignment, rounded);
}

static void tat_libc_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable TAT_LIBC_ALLOCATOR_VTABLE = {
    .alloc = tat_libc_alloc,
    .free = tat_libc_free,
};

ta_allocator tat_libc_allocator(void) {
    return (ta_allocator){.self = NULL, .v = &TAT_LIBC_ALLOCATOR_VTABLE};
}

