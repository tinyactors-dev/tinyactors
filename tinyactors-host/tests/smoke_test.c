/*
 * The kernel links, answers its version, and its memory helpers work. It
 * also names every type of the host interface, so a change to their shape
 * is a compile error here.
 */

#include <stdio.h>

#include "tinyactors_host.h"
#include "../src/memory.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond); \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

static uint64_t monotonic(void *self) { *(int *)self += 1; return 42u; }
static uint64_t wall(void *self) { (void)self; return 7u; }
static uint64_t seed(void *self) { (void)self; return 9u; }

int main(void) {
    CHECK(tah_version() == TAH_VERSION);

    unsigned char a[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    unsigned char b[8];
    tah_mem_set(b, 0xAB, sizeof b);
    CHECK(b[0] == 0xAB && b[7] == 0xAB);
    tah_mem_copy(b, a, sizeof a);
    CHECK(tah_mem_equal(a, b, sizeof a));
    b[3] = 0;
    CHECK(!tah_mem_equal(a, b, sizeof a));
    CHECK(tah_mem_equal(a, b, 0));

    int reads = 0;
    static const tah_platform_vtable vtable = { monotonic, wall, seed };
    tah_platform platform = { .self = &reads, .v = &vtable };
    CHECK(platform.v->monotonic_ns(platform.self) == 42u && reads == 1);
    CHECK(platform.v->wall_ns(platform.self) == 7u && platform.v->random_seed(platform.self) == 9u);

    if (failures != 0) {
        return 1;
    }
    puts("smoke_test: ok");
    return 0;
}
