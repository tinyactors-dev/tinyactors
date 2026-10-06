/* net.h — minimal single-threaded HTTP/1.1 static file server with
 * RFC 6455 WebSockets (text frames), driven by poll(). POSIX only. */
#ifndef TINYACTORS_TORTURE_NET_H
#define TINYACTORS_TORTURE_NET_H

#include <stddef.h>
#include "loop.h"

typedef struct bench_net bench_net;

typedef struct bench_net_handlers {
    void *user;
    void (*on_open)(void *user, int client); /* WebSocket opened */
    void (*on_message)(void *user, int client, const char *message, size_t length); /* text frame */
    /* dynamic GET routes: return a malloc'd body (and its content type) or NULL */
    char *(*on_http)(void *user, const char *path, size_t *length, const char **type);
} bench_net_handlers;

/* Listen on host:port (port 0 = ephemeral) serving files below `webroot`
 * and upgrading "/ws" to a WebSocket. NULL on failure. */
bench_net *bench_net_listen(bench_loop *loop, const char *host, int port, const char *webroot,
                            bench_net_handlers h);
int bench_net_port(const bench_net *n);
void bench_net_send(bench_net *n, int client, const char *message, size_t length);
void bench_net_broadcast(bench_net *n, const char *message, size_t length);
int bench_net_clients(const bench_net *n); /* open WebSockets */
void bench_net_close(bench_net *n);

#endif
