#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "jv.h"

enum {
    RPC_PARSE_ERROR = -32700,
    RPC_INVALID_REQUEST = -32600,
    RPC_METHOD_NOT_FOUND = -32601,
    RPC_INVALID_PARAMS = -32602,
    RPC_INTERNAL_ERROR = -32603
};

typedef jv (*rpc_dispatch_fn)(void *context,
                              const char *method,
                              jv params,
                              int *error_code);

typedef struct {
    void *context;
    rpc_dispatch_fn dispatch;
    FILE *output;
} rpc_server_config;

typedef struct {
    rpc_server_config config;
    uint64_t message_count;
    uint64_t parse_error_count;
    uint64_t invalid_request_count;
    uint64_t notifications_received;
    uint64_t notifications_sent;
} rpc_server;

void rpc_server_init(rpc_server *server, const rpc_server_config *config);
void rpc_server_handle_line(rpc_server *server, const char *line, size_t length);

/* Consumes params. Pass jv_invalid() to omit the params member. */
bool rpc_server_send_notification(rpc_server *server, const char *method, jv params);

