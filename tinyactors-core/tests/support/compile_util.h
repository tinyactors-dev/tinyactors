/* compile_util.h — helpers for tests that need compiled images. */
#ifndef TINYACTORS_TESTS_SUPPORT_COMPILE_UTIL_H
#define TINYACTORS_TESTS_SUPPORT_COMPILE_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/compile.h>
#include <tinyactors/image.h>

#define TEST_UNUSED __attribute__((unused))

typedef struct test_file {
    const char *name;
    const char *text;
} test_file;

/* In-memory resolver over a NULL-terminated test_file array. */
TEST_UNUSED static ta_status test_resolve(void *context, ta_string_view base, ta_string_view source,
                                          ta_source *out) {
    (void)base;
    for (const test_file *f = (const test_file *)context; f && f->name; f++)
        if (strlen(f->name) == source.length &&
            memcmp(f->name, source.pointer, source.length) == 0) {
            out->name.pointer = f->name;
            out->name.length = (uint32_t)strlen(f->name);
            out->text.pointer = f->text;
            out->text.length = (uint32_t)strlen(f->text);
            return TA_OK;
        }
    return TA_ERROR_INVALID;
}

TEST_UNUSED static void print_diagnostics(const ta_compile_result *result) {
    const ta_diagnostic *diagnostic;
    uint32_t n = ta_compile_diagnostics(result, &diagnostic);
    for (uint32_t i = 0; i < n; i++)
        fprintf(stderr, "  %.*s:%u:%u: %s: %.*s\n", (int)diagnostic[i].file.length,
                diagnostic[i].file.pointer ? diagnostic[i].file.pointer : "", diagnostic[i].line,
                diagnostic[i].column, diagnostic[i].severity == 2 ? "error" : "warning",
                (int)diagnostic[i].message.length, diagnostic[i].message.pointer);
}

/* Compile one chart (+ optional files for src resolution). Returns an
 * 8-aligned malloc'd copy of the image or NULL (diagnostics printed unless
 * quiet). */
TEST_UNUSED static void *compile_chart(const char *name, const char *text, const test_file *files,
                                       size_t *length, int quiet) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    options.resolve = test_resolve;
    options.resolve_context = (void *)files;
    ta_source source = {{name, (uint32_t)strlen(name)}, {text, (uint32_t)strlen(text)}};
    ta_compile_result *result = NULL;
    ta_status status = ta_compile_charts(&options, &source, 1, &result);
    void *copy = NULL;
    if (status == TA_OK) {
        ta_string_view image = ta_compile_image(result);
        copy = aligned_alloc(8, (image.length + 7) & ~(size_t)7);
        memcpy(copy, image.pointer, image.length);
        *length = image.length;
    } else if (!quiet) {
        print_diagnostics(result);
    }
    ta_compile_free(result);
    return copy;
}

/* Number of diagnostics of a failing compile containing `needle`. */
TEST_UNUSED static int compile_error_contains(const char *text, const char *needle) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"bad.scxml", 9}, {text, (uint32_t)strlen(text)}};
    ta_compile_result *result = NULL;
    ta_status status = ta_compile_charts(&options, &source, 1, &result);
    int found = 0;
    const ta_diagnostic *diagnostic;
    uint32_t n = ta_compile_diagnostics(result, &diagnostic);
    for (uint32_t i = 0; i < n; i++) {
        char buffer[600];
        snprintf(buffer, sizeof buffer, "%.*s", (int)diagnostic[i].message.length,
                 diagnostic[i].message.pointer);
        if (strstr(buffer, needle)) found = 1;
    }
    ta_compile_free(result);
    return status != TA_OK && found;
}

#endif
