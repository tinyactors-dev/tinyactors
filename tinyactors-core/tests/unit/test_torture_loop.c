#define _POSIX_C_SOURCE 200809L
#include "../harness/harness.h"
#include "../../tools/torture/loop.h"
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct fixture {
    bench_loop *loop;
    int id, other, calls, status;
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
static void exited(void *user, pid_t pid, int status) {
    fixture *f = user;
    CHECK(pid > 0);
    f->calls++;
    f->status = status;
}
static void test_children_and_kill_deadline(void) {
    fixture f = {0};
    f.loop = bench_loop_new();
    pid_t pid = fork();
    if (!pid) {
        setpgid(0, 0);
        _exit(23);
    }
    CHECK(pid > 0);
    setpgid(pid, pid);
    CHECK(bench_loop_child(f.loop, pid, exited, &f));
    double end = bench_loop_now() + 1000;
    while (!f.calls && bench_loop_now() < end)
        bench_loop_poll(f.loop, end);
    CHECK_EQUAL(f.calls, 1);
    CHECK(WIFEXITED(f.status));
    CHECK_EQUAL(WEXITSTATUS(f.status), 23);
    CHECK_EQUAL(bench_loop_children(f.loop), 0);
    int p[2];
    CHECK_EQUAL(pipe(p), 0);
    pid = fork();
    if (!pid) {
        close(p[0]);
        setpgid(0, 0);
        signal(SIGTERM, SIG_IGN);
        (void)!write(p[1], "x", 1);
        close(p[1]);
        for (;;)
            pause();
    }
    CHECK(pid > 0);
    setpgid(pid, pid);
    close(p[1]);
    char character;
    CHECK_EQUAL(read(p[0], &character, 1), 1);
    close(p[0]);
    CHECK(bench_loop_child(f.loop, pid, exited, &f));
    double start = bench_loop_now();
    bench_loop_terminate(f.loop, pid, 40);
    f.id = bench_loop_watch(f.loop, -1, -1, 0, start + 10, fired, &f);
    end = start + 1000;
    while (bench_loop_children(f.loop) && bench_loop_now() < end)
        bench_loop_poll(f.loop, end);
    CHECK_EQUAL(f.calls, 3); /* timer ran while SIGTERM-resistant child was alive */
    CHECK(WIFSIGNALED(f.status));
    CHECK_EQUAL(WTERMSIG(f.status), SIGKILL);
    CHECK(bench_loop_now() - start >= 35);
    CHECK_EQUAL(bench_loop_children(f.loop), 0);
    bench_loop_free(f.loop);
}
int main(void) {
    RUN(test_readiness_and_replaced_timer);
    RUN(test_children_and_kill_deadline);
    TEST_MAIN_END();
}
