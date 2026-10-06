#define _POSIX_C_SOURCE 200809L
/* macOS hides what POSIX does not define (INADDR_LOOPBACK,
 * _SC_NPROCESSORS_ONLN, st_mtimespec) unless asked; Linux ignores this. */
#define _DARWIN_C_SOURCE
#include "postgres.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/tcp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif
typedef struct string_buffer {
    char *data;
    size_t length, capacity;
} string_buffer;
enum { CONNECT, STARTUP, IDLE, QUERY, BROKEN };
struct bench_postgres {
    bench_loop *loop;
    int fd, watch, state;
    double due;
    unsigned char header[5], *body;
    size_t header_length, body_length, got;
    string_buffer out, rows;
    size_t sent;
    char **names;
    unsigned name_count, row_count;
    char *sql;
    char error[512];
    bench_postgres_done done;
    void *user;
};
static uint32_t be32(const unsigned char *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static void put32(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}
static int add(string_buffer *b, const void *s, size_t n) {
    if (b->length + n + 1 > (16u << 20)) return 0;
    if (b->length + n + 1 > b->capacity) {
        size_t capacity = b->capacity ? b->capacity * 2 : 256;
        while (capacity < b->length + n + 1)
            capacity *= 2;
        char *p = realloc(b->data, capacity);
        if (!p) return 0;
        b->data = p;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, s, n);
    b->length += n;
    b->data[b->length] = 0;
    return 1;
}
static int json_string(string_buffer *b, const unsigned char *s, size_t n) {
    if (!add(b, "\"", 1)) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char character = s[i];
        char e[8];
        if (character == '"' || character == '\\') {
            e[0] = '\\';
            e[1] = (char)character;
            if (!add(b, e, 2)) return 0;
        } else if (character < 0x20) {
            snprintf(e, sizeof e, "\\u%04x", character);
            if (!add(b, e, 6)) return 0;
        } else if (!add(b, &character, 1))
            return 0;
    }
    return add(b, "\"", 1);
}
static void ready(void *user, short events);
static void arm(bench_postgres *connection) {
    short events =
        (short)(POLLIN |
                ((connection->state == CONNECT || connection->sent < connection->out.length)
                     ? POLLOUT
                     : 0));
    connection->watch = bench_loop_watch(connection->loop, connection->watch, connection->fd,
                                         events, connection->due, ready, connection);
}
static void clear_rows(bench_postgres *connection) {
    for (unsigned i = 0; i < connection->name_count; i++)
        free(connection->names[i]);
    free(connection->names);
    connection->names = NULL;
    connection->name_count = connection->row_count = 0;
    connection->rows.length = 0;
}
/* Must be the last use of c: completion can destroy it. */
static void complete(bench_postgres *connection, const char *fatal) {
    if (fatal) {
        snprintf(connection->error, sizeof connection->error, "%s", fatal);
        connection->state = BROKEN;
        bench_loop_remove(connection->loop, connection->watch);
        connection->watch = -1;
    } else {
        connection->state = IDLE;
        connection->due = INFINITY;
        arm(connection);
    }
    bench_postgres_done done = connection->done;
    void *user = connection->user;
    connection->done = NULL;
    free(connection->sql);
    connection->sql = NULL;
    string_buffer rows = connection->rows;
    connection->rows = (string_buffer){0};
    char error[sizeof connection->error];
    snprintf(error, sizeof error, "%s", connection->error);
    connection->error[0] = 0;
    clear_rows(connection);
    if (done) done(user, rows.data ? rows.data : "[]", error[0] ? error : NULL);
    free(rows.data);
}
static int send_query(bench_postgres *connection) {
    if (!connection->sql) return 1;
    connection->out.length = connection->sent = 0;
    size_t length = strlen(connection->sql) + 1;
    unsigned char h[5] = {'Q', 0, 0, 0, 0};
    put32(h + 1, (uint32_t)(length + 4));
    if (!add(&connection->out, h, 5) || !add(&connection->out, connection->sql, length)) return 0;
    connection->state = QUERY;
    connection->due = bench_loop_now() + 30000;
    arm(connection);
    return 1;
}
bench_postgres *bench_postgres_connect(bench_loop *loop, int port, const char *db, int timeout_ms) {
    bench_postgres *connection = calloc(1, sizeof *connection);
    if (!connection) return NULL;
    connection->loop = loop;
    connection->watch = -1;
    connection->fd = socket(AF_INET, SOCK_STREAM, 0);
    if (connection->fd < 0) {
        free(connection);
        return NULL;
    }
    fcntl(connection->fd, F_SETFL, O_NONBLOCK);
    fcntl(connection->fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(connection->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(connection->fd, (struct sockaddr *)&a, sizeof a) < 0 && errno != EINPROGRESS)
        snprintf(connection->error, sizeof connection->error, "connect: %s", strerror(errno));
    unsigned char h[8] = {0};
    put32(h + 4, 196608);
    const char *kv[] = {"user", "postgres", "database", db, "application_name", "torture-bench"};
    if (!add(&connection->out, h, 8)) {
        bench_postgres_close(connection);
        return NULL;
    }
    for (int i = 0; i < 6; i++)
        if (!add(&connection->out, kv[i], strlen(kv[i]) + 1)) {
            bench_postgres_close(connection);
            return NULL;
        }
    if (!add(&connection->out, "", 1)) {
        bench_postgres_close(connection);
        return NULL;
    }
    put32((unsigned char *)connection->out.data, (uint32_t)connection->out.length);
    connection->due = bench_loop_now() + (connection->error[0] ? 0 : timeout_ms);
    arm(connection);
    if (connection->watch < 0) {
        bench_postgres_close(connection);
        return NULL;
    }
    return connection;
}
int bench_postgres_query(bench_postgres *connection, const char *sql, bench_postgres_done done,
                         void *user) {
    if (connection->state == BROKEN || connection->done) return -1;
    connection->sql = strdup(sql);
    if (!connection->sql) return -1;
    connection->done = done;
    connection->user = user;
    if (connection->state == IDLE && !send_query(connection)) {
        connection->due = bench_loop_now(); /* fail asynchronously */
        arm(connection);
    }
    return 0;
}
/* Validate framing before walking fields. Return 1 to continue, 0 when a
 * completion was dispatched, -1 on malformed/oversized data. */
static int message(bench_postgres *connection) {
    unsigned char *p = connection->body, *end = p + connection->body_length;
    char t = (char)connection->header[0];
    if (t == 'E') {
        while (p < end && *p) {
            unsigned char code = *p++;
            unsigned char *z = memchr(p, 0, (size_t)(end - p));
            if (!z) return -1;
            if (code == 'M') snprintf(connection->error, sizeof connection->error, "%s", p);
            p = z + 1;
        }
        if (connection->state == STARTUP) {
            char error[512];
            snprintf(error, sizeof error, "%s", connection->error);
            complete(connection, error);
            return 0;
        }
    } else if (t == 'R') {
        if (end - p < 4) return -1;
        if (be32(p)) {
            complete(connection, "authentication is not trust");
            return 0;
        }
    } else if (t == 'Z') {
        if (connection->state == STARTUP) {
            connection->state = IDLE;
            connection->due = INFINITY;
            if (!send_query(connection)) return -1;
        } else if (connection->state == QUERY) {
            if (!connection->rows.length && !add(&connection->rows, "[", 1)) return -1;
            if (!add(&connection->rows, "]", 1)) return -1;
            complete(connection, NULL);
            return 0;
        }
    } else if (t == 'T') {
        if (end - p < 2) return -1;
        clear_rows(connection);
        connection->name_count = (unsigned)p[0] << 8 | p[1];
        p += 2;
        connection->names = calloc(connection->name_count + 1, sizeof *connection->names);
        if (!connection->names) {
            connection->name_count = 0;
            return -1;
        }
        for (unsigned i = 0; i < connection->name_count; i++) {
            unsigned char *z = memchr(p, 0, (size_t)(end - p));
            if (!z || end - z < 19) return -1;
            connection->names[i] = strdup((char *)p);
            if (!connection->names[i]) return -1;
            p = z + 19;
        }
        if (!add(&connection->rows, "[", 1)) return -1;
    } else if (t == 'D') {
        if (end - p < 2) return -1;
        unsigned n = (unsigned)p[0] << 8 | p[1];
        p += 2;
        if (n != connection->name_count) return -1;
        if (!add(&connection->rows, connection->row_count ? ",{" : "{",
                 connection->row_count ? 2 : 1))
            return -1;
        connection->row_count++;
        for (unsigned i = 0; i < n; i++) {
            if (end - p < 4) return -1;
            uint32_t length = be32(p);
            p += 4;
            if (i && !add(&connection->rows, ",", 1)) return -1;
            if (!json_string(&connection->rows, (unsigned char *)connection->names[i],
                             strlen(connection->names[i])) ||
                !add(&connection->rows, ":", 1))
                return -1;
            if (length == UINT32_MAX) {
                if (!add(&connection->rows, "null", 4)) return -1;
            } else {
                if (length > (size_t)(end - p) || !json_string(&connection->rows, p, length))
                    return -1;
                p += length;
            }
        }
        if (!add(&connection->rows, "}", 1)) return -1;
    }
    return 1;
}
static void ready(void *user, short events) {
    bench_postgres *connection = user;
    if (bench_loop_now() >= connection->due) {
        complete(connection, "PostgreSQL timed out");
        return;
    }
    if (connection->state == CONNECT) {
        int error = 0;
        socklen_t length = sizeof error;
        if (getsockopt(connection->fd, SOL_SOCKET, SO_ERROR, &error, &length) || error) {
            complete(connection, error ? strerror(error) : "connect failed");
            return;
        }
        connection->state = STARTUP;
    }
    if (connection->sent < connection->out.length) {
        size_t n = connection->out.length - connection->sent;
        if (n > 65536) n = 65536;
        ssize_t w = send(connection->fd, connection->out.data + connection->sent, n, MSG_NOSIGNAL);
        if (w > 0)
            connection->sent += (size_t)w;
        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            complete(connection, "cannot send query");
            return;
        }
    }
    /* Partial headers and bodies survive EAGAIN and the 64 KiB / 2 ms slice. */
    double until = bench_loop_now() + 2;
    size_t budget = 65536;
    while (budget && bench_loop_now() < until && (events & (POLLIN | POLLHUP | POLLERR))) {
        int header = connection->header_length < 5;
        size_t n =
            header ? 5 - connection->header_length : connection->body_length - connection->got;
        if (n) {
            if (n > budget) n = budget;
            ssize_t r = recv(connection->fd,
                             header ? connection->header + connection->header_length
                                    : connection->body + connection->got,
                             n, 0);
            if (r <= 0) {
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
                complete(connection, "PostgreSQL closed the connection");
                return;
            }
            budget -= (size_t)r;
            if (header)
                connection->header_length += (size_t)r;
            else
                connection->got += (size_t)r;
        }
        if (header && connection->header_length == 5) {
            uint32_t length = be32(connection->header + 1);
            if (length < 4 || length > (1u << 20)) {
                complete(connection, "invalid PostgreSQL message length");
                return;
            }
            connection->body_length = length - 4;
            unsigned char *body = realloc(connection->body, connection->body_length + 1);
            if (!body) {
                complete(connection, "out of memory");
                return;
            }
            connection->body = body;
            connection->got = 0;
        }
        if (connection->header_length == 5 && connection->got == connection->body_length) {
            connection->body[connection->body_length] = 0;
            connection->header_length = 0;
            int rc = message(connection);
            if (!rc) return;
            if (rc < 0) {
                complete(connection, "invalid or oversized PostgreSQL result");
                return;
            }
        }
    }
    arm(connection);
}
int bench_postgres_broken(const bench_postgres *connection) {
    return connection->state == BROKEN;
}
void bench_postgres_close(bench_postgres *connection) {
    if (!connection) return;
    bench_loop_remove(connection->loop, connection->watch);
    close(connection->fd);
    clear_rows(connection);
    free(connection->sql);
    free(connection->body);
    free(connection->out.data);
    free(connection->rows.data);
    free(connection);
}
