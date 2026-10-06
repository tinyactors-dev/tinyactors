#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>

#include "allocator.h"
#include "server.h"

/* The largest request the server reads. */
enum { TAT_MAX_REQUEST_BYTES = 16 * 1024 * 1024 };

int main(void) {
    tat_server server = {0};
    if (tat_server_init(&server, stdout, tat_libc_allocator()) != TA_OK) {
        fputs("tinyactors-test: could not initialize server\n", stderr);
        return 2;
    }

    char *line = NULL;
    size_t capacity = 0;
    while (tat_server_running(&server)) {
        ssize_t length = getline(&line, &capacity, stdin);
        if (length < 0) {
            break;
        }
        if (length > TAT_MAX_REQUEST_BYTES) {
            fputs("tinyactors-test: request exceeds 16 MiB\n", stderr);
            free(line);
            tat_server_destroy(&server);
            return 2;
        }
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            length -= 1;
        }
        if (length == 0) {
            continue;
        }
        tat_server_handle_line(&server, line, (size_t)length);
    }

    free(line);
    tat_server_destroy(&server);
    return 0;
}
