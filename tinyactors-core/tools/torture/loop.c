#define _POSIX_C_SOURCE 200809L
#include "loop.h"
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define WATCHES  128
#define CHILDREN 64
typedef struct watch {
    int fd;
    short events;
    unsigned generation;
    double due;
    bench_ready function;
    void *user;
} watch;
typedef struct child {
    pid_t pid;
    double kill_due;
    bench_exited function;
    void *user;
} child;
struct bench_loop {
    watch watches[WATCHES];
    child children[CHILDREN];
    int pipe[2];
    struct sigaction old_child_action;
};
/* The bench has one loop. Only async-signal-safe work in the handler. */
static volatile sig_atomic_t signal_fd = -1;
void bench_loop_wakeup(void) {
    int saved = errno;
    if (signal_fd >= 0) (void)!write(signal_fd, "x", 1);
    errno = saved;
}
static void child_signal(int sig) {
    (void)sig;
    bench_loop_wakeup();
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
    struct sigaction sa = {0};
    sa.sa_handler = child_signal;
    sa.sa_flags = SA_NOCLDSTOP;
    sigemptyset(&sa.sa_mask);
    signal_fd = loop->pipe[1];
    sigaction(SIGCHLD, &sa, &loop->old_child_action);
    return loop;
}
void bench_loop_free(bench_loop *loop) {
    signal_fd = -1;
    sigaction(SIGCHLD, &loop->old_child_action, NULL);
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
int bench_loop_child(bench_loop *loop, pid_t pid, bench_exited function, void *user) {
    for (int i = 0; i < CHILDREN; i++)
        if (!loop->children[i].pid) {
            loop->children[i] = (child){pid, INFINITY, function, user};
            return 1;
        }
    return 0;
}
void bench_loop_terminate(bench_loop *loop, pid_t pid, double grace_ms) {
    for (int i = 0; i < CHILDREN; i++)
        if (loop->children[i].pid == pid) {
            if (isinf(loop->children[i].kill_due)) {
                kill(-pid,
                     SIGTERM); /* children own a process group, including mise's descendants */
                loop->children[i].kill_due = bench_loop_now() + grace_ms;
            }
            return;
        }
}
int bench_loop_children(const bench_loop *loop) {
    int n = 0;
    for (int i = 0; i < CHILDREN; i++)
        n += loop->children[i].pid != 0;
    return n;
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
    for (int i = 0; i < CHILDREN; i++)
        if (loop->children[i].pid && loop->children[i].kill_due < due)
            due = loop->children[i].kill_due;
    double left = due - bench_loop_now();
    int ms = isinf(due) ? -1 : left <= 0 ? 0 : left > 2147483647 ? 2147483647 : (int)ceil(left);
    (void)poll(fds, WATCHES + 1, ms);
    char buffer[256];
    while (read(loop->pipe[0], buffer, sizeof buffer) > 0) {
    }
    double now = bench_loop_now();
    for (int i = 0; i < CHILDREN; i++) {
        child c = loop->children[i];
        if (!c.pid) continue;
        int status;
        if (waitpid(c.pid, &status, WNOHANG) == c.pid) {
            loop->children[i].pid = 0;
            if (c.function) c.function(c.user, c.pid, status);
        } else if (now >= c.kill_due) {
            kill(-c.pid, SIGKILL);
            loop->children[i].kill_due = INFINITY;
        }
    }
    for (int i = 0; i < WATCHES; i++) {
        watch w = loop->watches[i];
        if (w.function && w.generation == generations[i] && (fds[i + 1].revents || now >= w.due))
            w.function(w.user, fds[i + 1].revents);
    }
}
