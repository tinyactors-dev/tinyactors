#include "platform.h"

#include <stdint.h>
#include <stdlib.h>

static void *tna_platform_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) {
        return malloc(size);
    }
    /* aligned_alloc wants a size that is a multiple of the alignment. */
    if (size > SIZE_MAX - (alignment - 1u)) {
        return NULL;
    }
    size_t rounded = (size + alignment - 1u) & ~(alignment - 1u);
    return aligned_alloc(alignment, rounded);
}

static void tna_platform_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable tna_platform_vtable = {
    .alloc = tna_platform_alloc,
    .free = tna_platform_free,
};

ta_allocator tna_platform_allocator(void) {
    return (ta_allocator){ .self = NULL, .v = &tna_platform_vtable };
}
