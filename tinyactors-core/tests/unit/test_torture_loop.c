#define _POSIX_C_SOURCE 200809L
#include "../harness/harness.h"
#include "../../tools/torture/loop.h"
#include <math.h>
#include <unistd.h>

typedef struct fixture {
    bench_loop *loop;
    int id, other, calls;
} fixture;
static void fired(void *user, short events) {
    fixture *f = user;
    CHECK_EQUAL(events, 0);
    f->calls++;
    bench_loop_remove(f->loop, f->id);
}
static void replace(void *user, short events) {
    fixture *f = user;
    CHECK(events & POLLIN);
    bench_loop_remove(f->loop, f->id);
    bench_loop_remove(f->loop, f->other);
    f->id = bench_loop_watch(f->loop, -1, -1, 0, bench_loop_now() + 25, fired, f);
}
static void test_readiness_and_replaced_timer(void) {
    fixture f = {0};
    f.loop = bench_loop_new();
    CHECK(f.loop != NULL);
    int p[2];
    CHECK_EQUAL(pipe(p), 0);
    f.id = bench_loop_watch(f.loop, -1, p[0], POLLIN, INFINITY, replace, &f);
    f.other = bench_loop_watch(f.loop, -1, -1, 0, bench_loop_now(), fired, &f);
    CHECK_EQUAL(write(p[1], "x", 1), 1);
    bench_loop_poll(f.loop, bench_loop_now() + 500);
    CHECK_EQUAL(f.calls, 0); /* stale snapshot must not dispatch the replacement */
    double start = bench_loop_now();
    bench_loop_poll(f.loop, start + 500);
    CHECK_EQUAL(f.calls, 1);
    CHECK(bench_loop_now() - start >= 20);
    close(p[0]);
    close(p[1]);
    bench_loop_free(f.loop);
}
int main(void) {
    RUN(test_readiness_and_replaced_timer);
    TEST_MAIN_END();
}
