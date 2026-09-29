#pragma once
/* A minimal assertion for the native tests: report the line and fail. */
#include <stdio.h>
#include <stdlib.h>

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition)) {                                                           \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            exit(1);                                                                  \
        }                                                                             \
    } while (0)

/*
 * The platform allocator a binding gives the kernel (kernel ADR 0001): the
 * tests may use the host's C library, so it is malloc and free here.
 */
#include <stdint.h>

#include "tinyactors.h"

static inline void *check_platform_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) {
        return malloc(size);
    }
    size_t rounded = (size + alignment - 1u) & ~(alignment - 1u);
    return aligned_alloc(alignment, rounded);
}

static inline void check_platform_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable check_platform_vtable = {
    .alloc = check_platform_alloc,
    .free = check_platform_free,
};

/* malloc and free as a ta_allocator, for tah_allocator_init() in tests. */
static inline ta_allocator check_platform_allocator(void) {
    return (ta_allocator){ .self = NULL, .v = &check_platform_vtable };
}
