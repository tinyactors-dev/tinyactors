#pragma once
/* clock_gettime under -std=c11; every test includes this header first. */
#if !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif
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

/*
 * The platform a binding gives every system (tinyactors_host.h): the tests'
 * is the host's C library, as napi's is.
 */
#include <time.h>

#include "tinyactors_host.h"

static inline uint64_t check_monotonic_ns(void *self) {
    (void)self;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static inline uint64_t check_wall_ns(void *self) {
    (void)self;
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

/* Tests want the same trace IDs on every run. */
static inline uint64_t check_random_seed(void *self) {
    (void)self;
    return 0x5eedu;
}

static const tah_platform_vtable check_platform_vtable_clock = {
    .monotonic_ns = check_monotonic_ns,
    .wall_ns = check_wall_ns,
    .random_seed = check_random_seed,
};

static inline tah_platform check_platform(void) {
    return (tah_platform){ .self = NULL, .v = &check_platform_vtable_clock, .allocator = check_platform_allocator() };
}

