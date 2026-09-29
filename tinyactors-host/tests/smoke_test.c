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

static void request_turn(void *self) { *(int *)self += 1; }
static void arm_timer(void *self, uint64_t due) { (void)self; (void)due; }
static uint64_t now(void *self) { (void)self; return 42u; }
static void effects_ready(void *self) { (void)self; }

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

    int turns = 0;
    static const tah_pump_driver_vtable pump = { request_turn, arm_timer, now };
    static const tah_hooks_vtable hooks = { effects_ready };
    tah_host host = { .pump = { &turns, &pump }, .hooks = { NULL, &hooks } };
    host.pump.v->request_turn(host.pump.self);
    CHECK(turns == 1 && host.pump.v->now(host.pump.self) == 42u);

    if (failures != 0) {
        return 1;
    }
    puts("smoke_test: ok");
    return 0;
}
