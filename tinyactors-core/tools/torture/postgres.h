/* Nonblocking PostgreSQL protocol 3.0, trust authentication, simple queries.
 * Local numeric IPv4 only: no DNS or libpq dependency. */
#ifndef TINYACTORS_TORTURE_POSTGRES_H
#define TINYACTORS_TORTURE_POSTGRES_H
#include "loop.h"
typedef struct bench_postgres bench_postgres;
/* Completion is always dispatched by the loop, never inline. json/err are
 * borrowed for the call. A callback may close the connection or query again. */
typedef void (*bench_postgres_done)(void *user, const char *json, const char *error);
bench_postgres *bench_postgres_connect(bench_loop *loop, int port, const char *db, int timeout_ms);
/* One query at a time, including while connecting. Returns 0 on acceptance. */
int bench_postgres_query(bench_postgres *c, const char *sql, bench_postgres_done done, void *user);
int bench_postgres_broken(const bench_postgres *c);
/* Cancels without calling done; the owner releases its callback state. */
void bench_postgres_close(bench_postgres *c);
#endif
