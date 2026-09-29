/* clock_gettime and CLOCK_MONOTONIC under -std=c11. */
#if !defined(_POSIX_C_SOURCE) && !defined(__APPLE__)
#define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"

#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#if !defined(__APPLE__) && !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__)
#include <sys/random.h>
#endif

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

uint64_t tna_monotonic_ns(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t tna_platform_monotonic(void *self) {
    (void)self;
    return tna_monotonic_ns();
}

static uint64_t tna_platform_wall(void *self) {
    (void)self;
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) {
        return 0;
    }
    return (uint64_t)now.tv_sec * 1000000000u + (uint64_t)now.tv_nsec;
}

static uint64_t tna_platform_random(void *self) {
    uint64_t seed = 0;
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__)
    (void)self;
    arc4random_buf(&seed, sizeof seed);
#else
    if (getrandom(&seed, sizeof seed, 0) != (ssize_t)sizeof seed) {
        /* No entropy: trace IDs only have to be unlikely to collide. */
        seed = tna_platform_wall(self) ^ tna_monotonic_ns();
    }
#endif
    return seed;
}

static const tah_platform_vtable tna_platform_clock_vtable = {
    .monotonic_ns = tna_platform_monotonic,
    .wall_ns = tna_platform_wall,
    .random_seed = tna_platform_random,
};

tah_platform tna_platform(void) {
    return (tah_platform){ .self = NULL, .v = &tna_platform_clock_vtable, .allocator = tna_platform_allocator() };
}
