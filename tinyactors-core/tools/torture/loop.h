/* One owner of waiting: monotonic deadlines and descriptor readiness.
 * Callbacks are cooperative, must not wait, and may change registrations. */
#ifndef TINYACTORS_TORTURE_LOOP_H
#define TINYACTORS_TORTURE_LOOP_H
#include <poll.h>

typedef struct bench_loop bench_loop;
typedef void (*bench_ready)(void *user, short events); /* events == 0: deadline */
double bench_loop_now(void);  /* monotonic milliseconds; never scenario-relative */
void bench_loop_wakeup(void); /* async-signal-safe, wakes the single loop */
bench_loop *bench_loop_new(void);
void bench_loop_free(bench_loop *loop);
/* id == -1 adds; otherwise replaces. fd == -1 is a timer, INFINITY disables
 * its deadline. Returns -1 if full. Descriptors belong to the caller. */
int bench_loop_watch(bench_loop *loop, int id, int fd, short events, double due,
                     bench_ready function, void *user);
void bench_loop_remove(bench_loop *loop, int id);
void bench_loop_poll(bench_loop *loop, double due); /* caller's next ready-work deadline */
#endif
