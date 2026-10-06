/* net.c — see net.h. */
#define _POSIX_C_SOURCE 200809L
#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#define MAX_CLIENTS 64
#define MAX_OUT     (32u << 20) /* drop a client that falls this far behind */
#define MAX_IN      (1u << 20)

enum { C_FREE = 0, C_HTTP, C_WEBSOCKET, C_CLOSING };

typedef struct byte_buffer {
    char *data;
    size_t length, capacity;
} byte_buffer;

typedef struct client {
    int fd, state;
    int watch;
    bench_net *net;
    byte_buffer instance, out;
} client;

struct bench_net {
    int fd, port;
    int watch;
    bench_loop *loop;
    char webroot[1024];
    bench_net_handlers host;
    client clients[MAX_CLIENTS];
};

static int buffer_add(byte_buffer *b, const void *p, size_t n) {
    if (b->length + n > b->capacity) {
        size_t capacity = b->capacity ? b->capacity : 4096;
        while (capacity < b->length + n)
            capacity *= 2;
        char *q = (char *)realloc(b->data, capacity);
        if (!q) return 0;
        b->data = q;
        b->capacity = capacity;
    }
    memcpy(b->data + b->length, p, n);
    b->length += n;
    return 1;
}

static void buffer_drop(byte_buffer *b, size_t n) {
    memmove(b->data, b->data + n, b->length - n);
    b->length -= n;
}

/* ---- SHA-1 and base64 (for Sec-WebSocket-Accept) ------------------------------------ */

static uint32_t rol(uint32_t x, int n) {
    return (x << n) | (x >> (32 - n));
}

static void sha1(const unsigned char *message, size_t length, unsigned char out[20]) {
    uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    size_t total = ((length + 8) / 64 + 1) * 64;
    unsigned char *m = (unsigned char *)calloc(total, 1);
    if (!m) return;
    memcpy(m, message, length);
    m[length] = 0x80;
    uint64_t bits = (uint64_t)length * 8;
    for (int i = 0; i < 8; i++)
        m[total - 1 - i] = (unsigned char)(bits >> (8 * i));
    for (size_t offset = 0; offset < total; offset += 64) {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (uint32_t)m[offset + 4 * i] << 24 | (uint32_t)m[offset + 4 * i + 1] << 16 |
                   (uint32_t)m[offset + 4 * i + 2] << 8 | m[offset + 4 * i + 3];
        for (int i = 16; i < 80; i++)
            w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; i++) {
            uint32_t f, k;
            if (i < 20) {
                f = (b & c) | (~b & d);
                k = 0x5A827999;
            } else if (i < 40) {
                f = b ^ c ^ d;
                k = 0x6ED9EBA1;
            } else if (i < 60) {
                f = (b & c) | (b & d) | (c & d);
                k = 0x8F1BBCDC;
            } else {
                f = b ^ c ^ d;
                k = 0xCA62C1D6;
            }
            uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rol(b, 30);
            b = a;
            a = t;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    free(m);
    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 4; j++)
            out[4 * i + j] = (unsigned char)(h[i] >> (24 - 8 * j));
}

static void base64(const unsigned char *in, size_t length, char *out) {
    static const char tab[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16 | (i + 1 < length ? (uint32_t)in[i + 1] << 8 : 0) |
                     (i + 2 < length ? in[i + 2] : 0);
        out[o++] = tab[v >> 18 & 63];
        out[o++] = tab[v >> 12 & 63];
        out[o++] = i + 1 < length ? tab[v >> 6 & 63] : '=';
        out[o++] = i + 2 < length ? tab[v & 63] : '=';
    }
    out[o] = 0;
}

/* ---- server ---------------------------------------------------------------------------- */

static void accept_ready(void *user, short events);
static void client_ready(void *user, short events);
static void client_watch(client *c) {
    c->watch = bench_loop_watch(c->net->loop, c->watch, c->fd,
                                (short)(POLLIN | (c->out.length ? POLLOUT : 0)), INFINITY,
                                client_ready, c);
}

bench_net *bench_net_listen(bench_loop *loop, const char *host, int port, const char *webroot,
                            bench_net_handlers h) {
    bench_net *net = (bench_net *)calloc(1, sizeof *net);
    if (!net) return NULL;
    net->loop = loop;
    net->host = h;
    snprintf(net->webroot, sizeof net->webroot, "%s", webroot);
    net->fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(net->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a;
    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1 ||
        bind(net->fd, (struct sockaddr *)&a, sizeof a) || listen(net->fd, 32)) {
        close(net->fd);
        free(net);
        return NULL;
    }
    socklen_t al = sizeof a;
    getsockname(net->fd, (struct sockaddr *)&a, &al);
    net->port = ntohs(a.sin_port);
    fcntl(net->fd, F_SETFL, fcntl(net->fd, F_GETFL) | O_NONBLOCK);
    fcntl(net->fd, F_SETFD, FD_CLOEXEC);
    net->watch = bench_loop_watch(loop, -1, net->fd, POLLIN, INFINITY, accept_ready, net);
    return net;
}

int bench_net_port(const bench_net *net) {
    return net->port;
}

int bench_net_clients(const bench_net *net) {
    int k = 0;
    for (int i = 0; i < MAX_CLIENTS; i++)
        k += net->clients[i].state == C_WEBSOCKET;
    return k;
}

static void drop(client *c) {
    if (c->state == C_FREE) return;
    bench_loop_remove(c->net->loop, c->watch);
    close(c->fd);
    free(c->instance.data);
    free(c->out.data);
    memset(c, 0, sizeof *c);
}

static void queue(client *c, const void *p, size_t length) {
    if (c->state == C_FREE) return;
    if (c->out.length + length > MAX_OUT || !buffer_add(&c->out, p, length))
        drop(c);
    else
        client_watch(c);
}

static void websocket_frame(client *c, int opcode, const char *message, size_t length) {
    unsigned char header[10];
    size_t hl = 2;
    header[0] = (unsigned char)(0x80 | opcode);
    if (length < 126) {
        header[1] = (unsigned char)length;
    } else if (length < 65536) {
        header[1] = 126;
        header[2] = (unsigned char)(length >> 8);
        header[3] = (unsigned char)length;
        hl = 4;
    } else {
        header[1] = 127;
        for (int i = 0; i < 8; i++)
            header[2 + i] = (unsigned char)((uint64_t)length >> (56 - 8 * i));
        hl = 10;
    }
    queue(c, header, hl);
    queue(c, message, length);
}

void bench_net_send(bench_net *net, int ci, const char *message, size_t length) {
    if (ci >= 0 && ci < MAX_CLIENTS && net->clients[ci].state == C_WEBSOCKET)
        websocket_frame(&net->clients[ci], 1, message, length);
}

void bench_net_broadcast(bench_net *net, const char *message, size_t length) {
    for (int i = 0; i < MAX_CLIENTS; i++)
        if (net->clients[i].state == C_WEBSOCKET)
            websocket_frame(&net->clients[i], 1, message, length);
}

static const char *content_type(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (!strcmp(dot, ".html")) return "text/html; charset=utf-8";
    if (!strcmp(dot, ".css")) return "text/css; charset=utf-8";
    if (!strcmp(dot, ".js") || !strcmp(dot, ".mjs")) return "text/javascript; charset=utf-8";
    if (!strcmp(dot, ".json")) return "application/json";
    if (!strcmp(dot, ".svg")) return "image/svg+xml";
    if (!strcmp(dot, ".png")) return "image/png";
    if (!strcmp(dot, ".ico")) return "image/x-icon";
    return "application/octet-stream";
}

static void respond(client *c, const char *status, const char *type, const char *body,
                    size_t length) {
    char header[512];
    int hl = snprintf(header, sizeof header,
                      "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                      status, type, length);
    queue(c, header, (size_t)hl);
    queue(c, body, length);
    if (c->state != C_FREE) c->state = C_CLOSING;
}

/* header value (case-insensitive name) inside the request head, or NULL */
static int header(const char *head, const char *name, char *out, size_t capacity) {
    size_t nl = strlen(name);
    for (const char *p = strstr(head, "\r\n"); p && p[2]; p = strstr(p + 2, "\r\n")) {
        const char *line = p + 2;
        if (!strncasecmp(line, name, nl) && line[nl] == ':') {
            const char *v = line + nl + 1;
            while (*v == ' ')
                v++;
            const char *e = strstr(v, "\r\n");
            size_t l = e ? (size_t)(e - v) : strlen(v);
            if (l >= capacity) l = capacity - 1;
            memcpy(out, v, l);
            out[l] = 0;
            return 1;
        }
    }
    return 0;
}

static void http_request(bench_net *net, int ci, const char *head) {
    client *c = &net->clients[ci];
    char method[8], path[512];
    if (sscanf(head, "%7s %511s", method, path) != 2 || strcmp(method, "GET")) {
        respond(c, "405 Method Not Allowed", "text/plain", "method not allowed\n", 19);
        return;
    }
    char *q = strchr(path, '?');
    if (q) *q = 0;
    char key[128], up[64];
    if (!strcmp(path, "/ws") && header(head, "Sec-WebSocket-Key", key, sizeof key) &&
        header(head, "Upgrade", up, sizeof up) && !strcasecmp(up, "websocket")) {
        char cat[256];
        unsigned char dig[20];
        char acc[64];
        snprintf(cat, sizeof cat, "%s258EAFA5-E914-47DA-95CA-C5AB0DC85B11", key);
        sha1((const unsigned char *)cat, strlen(cat), dig);
        base64(dig, 20, acc);
        char resp[256];
        int rl = snprintf(resp, sizeof resp,
                          "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                          "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n",
                          acc);
        queue(c, resp, (size_t)rl);
        if (c->state == C_FREE) return;
        c->state = C_WEBSOCKET;
        int one = 1;
        setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        if (net->host.on_open) net->host.on_open(net->host.user, ci);
        return;
    }
    if (net->host.on_http) {
        size_t blen = 0;
        const char *type = "application/json";
        char *body = net->host.on_http(net->host.user, path, &blen, &type);
        if (body) {
            respond(c, "200 OK", type, body, blen);
            free(body);
            return;
        }
    }
    if (!strcmp(path, "/")) strcpy(path, "/index.html");
    if (strstr(path, "..") || path[0] != '/') {
        respond(c, "400 Bad Request", "text/plain", "bad path\n", 9);
        return;
    }
    char file[1600];
    snprintf(file, sizeof file, "%s%s", net->webroot, path);
    FILE *f = fopen(file, "rb");
    if (!f) {
        respond(c, "404 Not Found", "text/plain", "not found\n", 10);
        return;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *body = (char *)malloc(length > 0 ? (size_t)length : 1);
    size_t got = body ? fread(body, 1, (size_t)length, f) : 0;
    fclose(f);
    respond(c, "200 OK", content_type(file), body, got);
    free(body);
}

/* Parse complete WebSocket frames from the input buffer. */
static void websocket_input(bench_net *net, int ci) {
    client *c = &net->clients[ci];
    for (;;) {
        if (c->state != C_WEBSOCKET || c->instance.length < 2) return;
        const unsigned char *p = (const unsigned char *)c->instance.data;
        int opcode = p[0] & 15, masked = p[1] & 0x80;
        uint64_t length = p[1] & 127;
        size_t hl = 2;
        if (length == 126) {
            if (c->instance.length < 4) return;
            length = (uint64_t)p[2] << 8 | p[3];
            hl = 4;
        } else if (length == 127) {
            if (c->instance.length < 10) return;
            length = 0;
            for (int i = 0; i < 8; i++)
                length = length << 8 | p[2 + i];
            hl = 10;
        }
        if (length > MAX_IN) {
            drop(c);
            return;
        }
        size_t mk = masked ? 4 : 0;
        if (c->instance.length < hl + mk + length) return;
        char *data = c->instance.data + hl + mk;
        if (masked)
            for (uint64_t i = 0; i < length; i++)
                data[i] ^= (char)p[hl + (i & 3)];
        if (opcode == 1 && net->host.on_message) {
            net->host.on_message(net->host.user, ci, data, (size_t)length);
            if (c->state == C_FREE) return;
        } else if (opcode == 8) {
            websocket_frame(c, 8, "", 0);
            if (c->state != C_FREE) c->state = C_CLOSING;
        } else if (opcode == 9) {
            websocket_frame(c, 10, data, (size_t)length);
        }
        if (c->state == C_FREE) return;
        buffer_drop(&c->instance, hl + mk + (size_t)length);
    }
}

static void accept_ready(void *user, short events) {
    bench_net *net = user;
    if (events & POLLIN) {
        for (int accepted = 0; accepted < 16; accepted++) {
            int fd = accept(net->fd, NULL, NULL);
            if (fd < 0) break;
            int slot = -1;
            for (int i = 0; i < MAX_CLIENTS && slot < 0; i++)
                if (net->clients[i].state == C_FREE) slot = i;
            if (slot < 0) {
                close(fd);
                continue;
            }
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
            net->clients[slot].fd = fd;
            net->clients[slot].state = C_HTTP;
            net->clients[slot].net = net;
            net->clients[slot].watch = -1;
            client_watch(&net->clients[slot]);
        }
    }
}

static void client_ready(void *user, short events) {
    client *c = user;
    bench_net *net = c->net;
    int ci = (int)(c - net->clients);
    if (events & (POLLERR | POLLHUP | POLLNVAL) && !(events & POLLIN)) {
        drop(c);
        return;
    }
    if (events & POLLIN) {
        char temporary[65536];
        ssize_t r = recv(c->fd, temporary, sizeof temporary, 0);
        if (r <= 0) {
            if (r == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) drop(c);
            return;
        }
        if (c->instance.length + (size_t)r > MAX_IN ||
            !buffer_add(&c->instance, temporary, (size_t)r)) {
            drop(c);
            return;
        }
        if (c->state == C_HTTP) {
            buffer_add(&c->instance, "", 1); /* NUL for string search */
            c->instance.length--;
            char *end = strstr(c->instance.data, "\r\n\r\n");
            if (end) {
                size_t hl = (size_t)(end - c->instance.data) + 4;
                char *head = (char *)malloc(hl + 1);
                memcpy(head, c->instance.data, hl);
                head[hl] = 0;
                buffer_drop(&c->instance, hl);
                http_request(net, ci, head);
                free(head);
            }
        }
        if (c->state == C_WEBSOCKET) websocket_input(net, ci);
    }
    if (c->state != C_FREE && c->out.length) {
        ssize_t w = send(c->fd, c->out.data, c->out.length, MSG_NOSIGNAL);
        if (w > 0)
            buffer_drop(&c->out, (size_t)w);
        else if (w < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
            drop(c);
    }
    if (c->state == C_CLOSING && !c->out.length) drop(c);
    if (c->state != C_FREE) client_watch(c);
}

void bench_net_close(bench_net *net) {
    if (!net) return;
    for (int i = 0; i < MAX_CLIENTS; i++)
        drop(&net->clients[i]);
    bench_loop_remove(net->loop, net->watch);
    close(net->fd);
    free(net);
}
