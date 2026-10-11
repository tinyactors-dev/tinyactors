#define _POSIX_C_SOURCE 200809L
#include "loop.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#define WATCHES  128
typedef struct watch {
    int fd;
    short events;
    unsigned generation;
    double due;
    bench_ready function;
    void *user;
} watch;
struct bench_loop {
    watch watches[WATCHES];
    int pipe[2];
};
/* The bench has one loop; bench_loop_wakeup is called from a signal handler. */
static volatile sig_atomic_t signal_fd = -1;
void bench_loop_wakeup(void) {
    int saved = errno;
    if (signal_fd >= 0) (void)!write(signal_fd, "x", 1);
    errno = saved;
}
double bench_loop_now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1000 + (double)t.tv_nsec / 1e6;
}
bench_loop *bench_loop_new(void) {
    bench_loop *loop = calloc(1, sizeof *loop);
    if (!loop) return NULL;
    if (pipe(loop->pipe)) {
        free(loop);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(loop->pipe[i], F_SETFL, O_NONBLOCK);
        fcntl(loop->pipe[i], F_SETFD, FD_CLOEXEC);
    }
    signal_fd = loop->pipe[1];
    return loop;
}
void bench_loop_free(bench_loop *loop) {
    signal_fd = -1;
    close(loop->pipe[0]);
    close(loop->pipe[1]);
    free(loop);
}
int bench_loop_watch(bench_loop *loop, int id, int fd, short events, double due,
                     bench_ready function, void *user) {
    if (id < 0) {
        for (id = 0; id < WATCHES && loop->watches[id].function; id++) {
        }
        if (id == WATCHES) return -1;
    }
    watch *w = &loop->watches[id];
    w->generation++;
    w->fd = fd;
    w->events = events;
    w->due = due;
    w->function = function;
    w->user = user;
    return id;
}
void bench_loop_remove(bench_loop *loop, int id) {
    if (id < 0) return;
    loop->watches[id].function = NULL;
    loop->watches[id].generation++;
}
void bench_loop_poll(bench_loop *loop, double due) {
    struct pollfd fds[WATCHES + 1];
    unsigned generations[WATCHES];
    fds[0] = (struct pollfd){loop->pipe[0], POLLIN, 0};
    for (int i = 0; i < WATCHES; i++) {
        watch *w = &loop->watches[i];
        generations[i] = w->generation;
        fds[i + 1] = (struct pollfd){w->function ? w->fd : -1, w->events, 0};
        if (w->function && w->due < due) due = w->due;
    }
    double left = due - bench_loop_now();
    int ms = isinf(due) ? -1 : left <= 0 ? 0 : left > 2147483647 ? 2147483647 : (int)ceil(left);
    (void)poll(fds, WATCHES + 1, ms);
    char buffer[256];
    while (read(loop->pipe[0], buffer, sizeof buffer) > 0) {
    }
    double now = bench_loop_now();
    for (int i = 0; i < WATCHES; i++) {
        watch w = loop->watches[i];
        if (w.function && w.generation == generations[i] && (fds[i + 1].revents || now >= w.due))
            w.function(w.user, fds[i + 1].revents);
    }
}
