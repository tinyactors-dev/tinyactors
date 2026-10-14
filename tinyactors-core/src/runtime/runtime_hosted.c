/* runtime_hosted.c — the conveniences that need an operating system: files
 * and the clock. A platform file (docs/adr/0001-packaging-and-releases.md,
 * decision 9): built for every target with a C library, not for
 * freestanding wasm, whose module has neither (runtime.h declares these
 * functions under the same condition). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <tinyactors/runtime.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <time.h>
#endif

/* ---- files -------------------------------------------------------------------------- */

ta_status ta_runtime_load_file(const char *path, ta_bytes *out) {
    *out = (ta_bytes){0};
    FILE *f = fopen(path, "rb");
    if (!f) return TA_ERROR_INVALID;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return TA_ERROR_INVALID;
    }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return TA_ERROR_INVALID;
    }
    char *buffer = (char *)malloc((size_t)n + 1); /* malloc: >= 8-aligned */
    if (!buffer) {
        fclose(f);
        return TA_ERROR_NO_MEMORY;
    }
    size_t got = fread(buffer, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) {
        free(buffer);
        return TA_ERROR_INVALID;
    }
    buffer[n] = 0; /* convenient for text files; not counted in length */
    *out = (ta_bytes){buffer, (size_t)n};
    return TA_OK;
}

void ta_runtime_free_file(ta_bytes bytes) {
    free((void *)bytes.pointer);
}

struct ta_runtime_files {
    char **texts;
    char **names;
    size_t count, capacity;
};

ta_runtime_files *ta_runtime_files_new(void) {
    return (ta_runtime_files *)calloc(1, sizeof(ta_runtime_files));
}

void ta_runtime_files_free(ta_runtime_files *files) {
    if (!files) return;
    for (size_t i = 0; i < files->count; i++) {
        free(files->texts[i]);
        free(files->names[i]);
    }
    free(files->texts);
    free(files->names);
    free(files);
}

/* Preserve the attempted path before its malloc block is freed. A direct
 * caller without compilation scratch still gets the requested src name. */
static void runtime_resolve_error(const ta_resolve_request *request, ta_resolve_result *out,
                                  const char *message) {
    out->error.message = (ta_string_view){message, (uint32_t)strlen(message)};
    ta_string_view file = out->source.name;
    char *name = request->scratch
                     ? ta_growable_arena_alloc(request->scratch, (size_t)file.length + 1, 1)
                     : NULL;
    if (name) {
        if (file.length) memcpy(name, file.pointer, file.length);
        name[file.length] = 0;
        out->source.name = (ta_string_view){name, file.length};
    } else {
        out->source.name = request->source;
    }
}

ta_status ta_runtime_resolve(void *context, const ta_resolve_request *request, ta_resolve_result *out) {
    *out = (ta_resolve_result){0};
    out->source.name = request->source;
    ta_runtime_files *files = (ta_runtime_files *)context;
    ta_string_view base = request->base, source = request->source;
    /* accept file: URIs */
    if (source.length > 7 && memcmp(source.pointer, "file://", 7) == 0) {
        source.pointer += 7;
        source.length -= 7;
    } else if (source.length > 5 && memcmp(source.pointer, "file:", 5) == 0) {
        source.pointer += 5;
        source.length -= 5;
    }
    /* directory of the referring file + src (absolute src used as is) */
    size_t dir = 0;
    for (size_t i = 0; i < base.length; i++)
        if (base.pointer[i] == '/' || base.pointer[i] == '\\') dir = i + 1;
    int absolute = source.length &&
                   (source.pointer[0] == '/' || (source.length > 1 && source.pointer[1] == ':'));
    if (absolute) dir = 0;
    char *path = (char *)malloc(dir + source.length + 1);
    if (!path) {
        runtime_resolve_error(request, out, "cannot allocate resolver path");
        return TA_ERROR_NO_MEMORY;
    }
    if (dir) memcpy(path, base.pointer, dir);
    memcpy(path + dir, source.pointer, source.length);
    path[dir + source.length] = 0;
    out->source.name = (ta_string_view){path, (uint32_t)strlen(path)};
    ta_bytes bytes;
    ta_status status = ta_runtime_load_file(path, &bytes);
    if (status != TA_OK) {
        runtime_resolve_error(request, out, status == TA_ERROR_NO_MEMORY
                                             ? "cannot allocate source file"
                                             : "cannot read source file");
        free(path);
        return status;
    }
    if (files->count == files->capacity) {
        size_t nc = files->capacity ? files->capacity * 2 : 8;
        char **nt = (char **)realloc(files->texts, nc * sizeof(char *));
        if (nt) files->texts = nt;
        char **nn = (char **)realloc(files->names, nc * sizeof(char *));
        if (nn) files->names = nn;
        if (!nt || !nn) {
            runtime_resolve_error(request, out, "cannot retain resolved source");
            ta_runtime_free_file(bytes);
            free(path);
            return TA_ERROR_NO_MEMORY;
        }
        files->capacity = nc;
    }
    files->texts[files->count] = (char *)bytes.pointer;
    files->names[files->count] = path;
    files->count++;
    out->source.name.pointer = path;
    out->source.name.length = (uint32_t)strlen(path);
    out->source.text.pointer = (const char *)bytes.pointer;
    out->source.text.length = (uint32_t)bytes.length;
    return TA_OK;
}

ta_status ta_runtime_compile_file(const char *path, int manifest, ta_compile_result **out) {
    *out = NULL;
    ta_runtime_files *files = ta_runtime_files_new();
    if (!files) return TA_ERROR_NO_MEMORY;
    ta_bytes text;
    ta_status status = ta_runtime_load_file(path, &text);
    if (status != TA_OK) {
        ta_runtime_files_free(files);
        return status;
    }
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.alloc = ta_runtime_malloc();
    options.resolve = ta_runtime_resolve;
    options.resolve_context = files;
    ta_source source;
    source.name.pointer = path;
    source.name.length = (uint32_t)strlen(path);
    source.text.pointer = (const char *)text.pointer;
    source.text.length = (uint32_t)text.length;
    status = manifest ? ta_compile_system(&options, &source, out)
                      : ta_compile_charts(&options, (ta_source_list){&source, 1}, out);
    ta_runtime_free_file(text);
    ta_runtime_files_free(files);
    return status;
}

/* ---- clock --------------------------------------------------------------------------- */

ta_time ta_runtime_now_ns(void) {
#if defined(_WIN32)
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (ta_time)((double)now.QuadPart * 1e9 / (double)freq.QuadPart);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ta_time)ts.tv_sec * 1000000000ull + (ta_time)ts.tv_nsec;
#endif
}

