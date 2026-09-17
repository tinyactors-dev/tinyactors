#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

#include "tinyactors.h"

typedef struct {
    ta_arena arena;
    void *impl;
} tat_server;

ta_result tat_server_init(tat_server *server, FILE *output, ta_allocator allocator);
void tat_server_destroy(tat_server *server);
bool tat_server_running(const tat_server *server);
void tat_server_handle_line(tat_server *server, const char *line, size_t length);
