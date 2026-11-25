/* platform.c — the tinyactors command's operating-system half: the only file
 * of the command that uses the C library (tool.h). It gives the commands
 * memory, reads files and standard input (whole, or as it comes for the run
 * server), writes standard output and error, ignores SIGPIPE so that a
 * client that went away is a failed write, and writes a file whole: to a temporary file beside it, renamed
 * over it once every byte is written, so a failed write leaves the old file
 * (or none) and never half of a new one. */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "tool.h"

typedef enum platform_limits {
    PLATFORM_PATH_MAX = 4096,
    PLATFORM_READ_CHUNK = 65536
} platform_limits;

static void *platform_alloc(void *context, size_t size, size_t align) {
    (void)context;
    (void)align; /* malloc is aligned for any object: at least the arena's 8 */
    return malloc(size);
}

static void platform_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

/* A view as a NUL-terminated path, in `room`; 0 when it does not fit. */
static int platform_path(ta_string_view path, char room[PLATFORM_PATH_MAX]) {
    if (path.length >= PLATFORM_PATH_MAX || memchr(path.pointer, 0, path.length)) return 0;
    memcpy(room, path.pointer, path.length);
    room[path.length] = 0;
    return 1;
}

/* Read the whole of `file` into a block of the arena, grown by doubling:
 * each step copies into a new block, and the old one stays in the arena
 * until the invocation ends, as everything else does. */
static ta_status platform_read_stream(FILE *file, ta_growable_arena *arena, ta_bytes *out) {
    size_t capacity = PLATFORM_READ_CHUNK, length = 0;
    unsigned char *bytes = (unsigned char *)ta_growable_arena_alloc(arena, capacity, 8);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    for (;;) {
        if (length == capacity) {
            unsigned char *larger = (unsigned char *)ta_growable_arena_alloc(arena, capacity * 2, 8);
            if (!larger) return TA_ERROR_NO_MEMORY;
            memcpy(larger, bytes, length);
            bytes = larger;
            capacity *= 2;
        }
        size_t got = fread(bytes + length, 1, capacity - length, file);
        length += got;
        if (got == 0) break;
    }
    if (ferror(file)) return TA_ERROR_INVALID;
    out->pointer = bytes;
    out->length = length;
    return TA_OK;
}

/* Standard input, once read: a command may read an input twice (inspect
 * tells a snapshot from an image's inputs first), and the bytes live in the
 * invocation's arena until it ends. */
typedef struct platform_state {
    int standard_input_read;
    ta_bytes standard_input;
} platform_state;

static ta_status platform_read(void *context, const tool_read *request, ta_bytes *out) {
    platform_state *state = (platform_state *)context;
    out->pointer = NULL;
    out->length = 0;
    if (request->path.length == 1 && request->path.pointer[0] == '-') {
        if (state->standard_input_read) {
            *out = state->standard_input;
            return TA_OK;
        }
        ta_status status = platform_read_stream(stdin, request->arena, out);
        if (status != TA_OK) return status;
        state->standard_input_read = 1;
        state->standard_input = *out;
        return TA_OK;
    }
    char path[PLATFORM_PATH_MAX];
    if (!platform_path(request->path, path)) return TA_ERROR_INVALID;
    FILE *file = fopen(path, "rb");
    if (!file) return TA_ERROR_INVALID;
    ta_status status = platform_read_stream(file, request->arena, out);
    fclose(file);
    return status;
}

static ta_status platform_write_all(FILE *file, ta_bytes bytes) {
    if (bytes.length && fwrite(bytes.pointer, 1, bytes.length, file) != bytes.length)
        return TA_ERROR_INVALID;
    return fflush(file) == 0 ? TA_OK : TA_ERROR_INVALID;
}

static ta_status platform_write(void *context, const tool_write *request) {
    (void)context;
    int to_standard_output = request->path.length == 1 && request->path.pointer[0] == '-';
    if (!request->path.length || to_standard_output) {
        FILE *stream =
            request->stream == TOOL_STANDARD_ERROR && !to_standard_output ? stderr : stdout;
        return platform_write_all(stream, request->bytes);
    }
    char path[PLATFORM_PATH_MAX], temporary[PLATFORM_PATH_MAX];
    if (!platform_path(request->path, path)) return TA_ERROR_INVALID;
    int written = snprintf(temporary, sizeof temporary, "%s.%ld.partial", path, (long)getpid());
    if (written < 0 || (size_t)written >= sizeof temporary) return TA_ERROR_INVALID;
    FILE *file = fopen(temporary, "wb");
    if (!file) return TA_ERROR_INVALID;
    ta_status status = platform_write_all(file, request->bytes);
    if (fclose(file) != 0) status = TA_ERROR_INVALID;
    if (status == TA_OK && rename(temporary, path) != 0) status = TA_ERROR_INVALID;
    if (status != TA_OK) remove(temporary);
    return status;
}

/* One read(2) of standard input: what has come, without waiting for a whole
 * chunk as fread would (the run server's client waits for each response
 * before it writes more). */
static ta_status platform_receive(void *context, ta_buffer *room) {
    (void)context;
    room->length = 0;
    for (;;) {
        ssize_t got = read(STDIN_FILENO, room->pointer, room->capacity);
        if (got >= 0) {
            room->length = (size_t)got;
            return TA_OK;
        }
        if (errno != EINTR) return TA_ERROR_INVALID;
    }
}

int main(int count, char **words) {
    /* a client that went away makes a write fail (exit 3) instead of
     * killing the process */
    signal(SIGPIPE, SIG_IGN);
    tool_platform platform;
    platform_state state = {0, {NULL, 0}};
    platform.context = &state;
    platform.allocator = (ta_allocator){NULL, platform_alloc, platform_free};
    platform.read = platform_read;
    platform.write = platform_write;
    platform.receive = platform_receive;
#ifdef TA_TOOL_VERSION
    platform.version = ta_string(TA_TOOL_VERSION);
#else
    platform.version = ta_string("");
#endif
    tool_command_line line = {count > 0 ? (uint32_t)(count - 1) : 0,
                              count > 0 ? (const char *const *)(words + 1) : NULL};
    return tool_main(&platform, line);
}
