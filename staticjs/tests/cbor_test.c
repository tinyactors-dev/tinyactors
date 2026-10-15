/*
 * CBOR through staticjs (sjs_value_read and the writer's SJS_FORMAT_CBOR),
 * against the conformance suite's neutral values
 * (tinyactors-test/suites/protocol/values.txt, whose head says what each
 * kind of case means): a roundtrip case read and written back, an encode
 * case evaluated and written, an invalid one refused when written, a
 * malformed one refused when read.
 *
 *   cbor_test <values.txt>
 *
 * Not applicable, each for its reason (ADR 0013): the cases named "not
 * ECMAScript"; bytes (staticjs has none); cycles (values are immutable);
 * lone surrogates (a staticjs string cannot hold one). Two expressions use
 * what staticjs leaves out, a method and a loop: they are given here as
 * staticjs writes them.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);     \
            failures++;                                                         \
        }                                                                       \
    } while (0)

typedef struct {
    char kind[16];
    char name[160];
    uint8_t cbor[256];
    uint32_t cbor_length;
    uint8_t back[256];
    uint32_t back_length;
    char ecmascript[512];
} test_case;

/* What applies to staticjs; NULL to run the case. */
static const char *not_applicable(const test_case *t) {
    if (strstr(t->name, "not ECMAScript") != NULL) return "not ECMAScript";
    if (strstr(t->name, "bytes") != NULL || strstr(t->name, "Uint8Array") != NULL ||
        strstr(t->name, "ArrayBuffer") != NULL || strstr(t->name, "a view's own bytes") != NULL) {
        return "staticjs has no bytes";
    }
    if (strcmp(t->name, "cycles") == 0) return "staticjs values are immutable: no cycle";
    if (strstr(t->name, "lone surrogate in") != NULL && strcmp(t->kind, "invalid") == 0) {
        return "a staticjs string cannot hold a lone surrogate";
    }
    return NULL;
}

/* The case's expression, or staticjs's own for one that uses what it
 * leaves out. */
static const char *expression(const test_case *t) {
    if (strcmp(t->name, "functions are dropped") == 0) return "({ f: () => 0, g: 1 })";
    if (strcmp(t->name, "nested deeper than 64") == 0) return "Array.from({ length: 65 }).reduce((v) => [v], 0)";
    return t->ecmascript;
}

static uint32_t from_hex(const char *hex, uint8_t *out) {
    uint32_t n = 0;
    for (; hex[0] != '\0' && hex[1] != '\0' && hex[0] != '\n'; hex += 2) {
        unsigned byte;
        sscanf(hex, "%2x", &byte);
        out[n++] = (uint8_t)byte;
    }
    return n;
}

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)alignment;
    ++*(long *)self;
    return malloc(size);
}

static void test_free(void *self, const sjs_allocator_free_params *params) {
    --*(long *)self;
    free(params->ptr);
}

static const sjs_allocator_vtable vtable = { test_alloc, test_free };

/* A program's value, or SJS_ERR_PROGRAM when it is refused or faults. */
static sjs_result evaluate(sjs_context *context, const char *source, sjs_value *out) {
    sjs_program *program;
    sjs_compile_params params = { .compiler = { .context = context, .goal = SJS_GOAL_EXPRESSION },
                                  .source = { source, (uint32_t)strlen(source) } };
    sjs_result r = sjs_compile(&params, &program);
    if (r != SJS_OK) return r;
    sjs_vm *vm;
    r = sjs_vm_init(&vm, &(sjs_vm_config){ .context = context });
    if (r == SJS_OK) {
        r = sjs_vm_start(vm, &(sjs_run_params){ .program = program });
        if (r == SJS_OK) r = sjs_vm_run(vm, 1000000) == SJS_RUN_DONE ? sjs_vm_take_result(vm, out) : SJS_ERR_PROGRAM;
        sjs_vm_destroy(vm);
    }
    sjs_program_release(program);
    return r;
}

/* The value as CBOR, three bytes at a time, into out. */
static sjs_result write_cbor(sjs_context *context, sjs_value v, uint8_t *out, uint32_t *length) {
    sjs_writer *w;
    sjs_error error = { 0 };
    sjs_result r = sjs_writer_init(&w, &(sjs_writer_config){ .context = context, .value = v, .format = SJS_FORMAT_CBOR,
                                                            .error = &error });
    if (r != SJS_OK) return r;
    *length = 0;
    do {
        sjs_buffer piece = { out + *length, 3, 0 };
        r = sjs_writer_write(w, &piece);
        *length += piece.length;
    } while (r == SJS_NEED_SPACE && *length < 4096u - 3u);
    sjs_writer_destroy(w);
    if (r == SJS_ERR_PROGRAM) CHECK(error.code == SJS_ERROR_UNSUPPORTED && error.message.length > 0);
    return r;
}

static void run(sjs_context *context, const test_case *t, int *ran) {
    uint8_t written[4096];
    uint32_t length = 0;
    sjs_value v;
    sjs_read_params read = { SJS_FORMAT_CBOR, { t->cbor, t->cbor_length }, NULL, NULL };
    if (strcmp(t->kind, "roundtrip") == 0) {
        CHECK(sjs_value_read(context, &read, &v) == SJS_OK);
        CHECK(write_cbor(context, v, written, &length) == SJS_OK);
        const uint8_t *want = t->back_length > 0 ? t->back : t->cbor;
        uint32_t want_length = t->back_length > 0 ? t->back_length : t->cbor_length;
        if (length != want_length || memcmp(written, want, length) != 0) fprintf(stderr, "roundtrip %s: differs\n", t->name);
        CHECK(length == want_length && memcmp(written, want, length) == 0);
        sjs_release(context, v);
    } else if (strcmp(t->kind, "encode") == 0 || strcmp(t->kind, "invalid") == 0) {
        bool encode = t->kind[0] == 'e';
        CHECK(evaluate(context, expression(t), &v) == SJS_OK);
        sjs_result r = write_cbor(context, v, written, &length);
        if (encode && (r != SJS_OK || length != t->cbor_length || memcmp(written, t->cbor, length) != 0)) {
            fprintf(stderr, "encode %s: result %d, %u bytes\n", t->name, (int)r, length);
        }
        if (encode) CHECK(r == SJS_OK && length == t->cbor_length && memcmp(written, t->cbor, length) == 0);
        else CHECK(r == SJS_ERR_PROGRAM);
        sjs_release(context, v);
    } else if (strcmp(t->kind, "malformed") == 0) {
        sjs_error error = { 0 };
        read.error = &error;
        v = sjs_number(7);
        CHECK(sjs_value_read(context, &read, &v) == SJS_ERR_PROGRAM);
        CHECK(error.code == SJS_ERROR_SYNTAX && v.kind == SJS_NUMBER);
        if (error.code != SJS_ERROR_SYNTAX) fprintf(stderr, "malformed %s: code %d\n", t->name, (int)error.code);
    } else {
        return;
    }
    ++*ran;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: cbor_test values.txt\n");
        return 2;
    }
    FILE *in = fopen(argv[1], "r");
    if (in == NULL) {
        perror(argv[1]);
        return 2;
    }
    long live = 0;
    sjs_context *context;
    if (sjs_context_init(&context, &(sjs_context_config){ { &live, &vtable }, SJS_LIMITS_DEFAULT }) != SJS_OK) return 2;
    test_case t;
    bool open = false;
    int ran = 0, skipped = 0;
    char line[1024];
    for (;;) {
        bool more = fgets(line, sizeof line, in) != NULL;
        bool head = more && line[0] != '#' && line[0] != '\n' && strncmp(line, "cbor ", 5) != 0 &&
                    strncmp(line, "back ", 5) != 0 && strncmp(line, "diag ", 5) != 0 &&
                    strncmp(line, "ecmascript ", 11) != 0 && strncmp(line, "python ", 7) != 0;
        if ((head || !more) && open) {
            if (not_applicable(&t) != NULL) skipped++;
            else run(context, &t, &ran);
            open = false;
        }
        if (!more) break;
        line[strcspn(line, "\n")] = '\0';
        if (head) {
            memset(&t, 0, sizeof t);
            sscanf(line, "%15s", t.kind);
            snprintf(t.name, sizeof t.name, "%s", line + strlen(t.kind) + 1);
            open = true;
        } else if (open && strncmp(line, "cbor ", 5) == 0) {
            t.cbor_length = from_hex(line + 5, t.cbor);
        } else if (open && strncmp(line, "back ", 5) == 0) {
            t.back_length = from_hex(line + 5, t.back);
        } else if (open && strncmp(line, "ecmascript ", 11) == 0) {
            snprintf(t.ecmascript, sizeof t.ecmascript, "%s", line + 11);
        }
    }
    fclose(in);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(live == 0);
    CHECK(ran > 30);
    if (failures > 0) {
        fprintf(stderr, "cbor_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("cbor_test: %d neutral value cases, %d not applicable to staticjs\n", ran, skipped);
    return 0;
}
