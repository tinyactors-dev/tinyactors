/* hello: the smallest whole staticjs program. It compiles one expression,
 * evaluates it on a budget of fuel, and prints the value as JavaScript
 * source.
 *
 *   cc -std=c11 -I. hello.c staticjs.c -o hello      (from a source tarball)
 *   ./hello
 *   {"total":55,"names":["ada","grace"]}
 */
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>

/* staticjs allocates only through the embedder. It asks for 16-byte
 * alignment, which malloc gives on 64-bit hosts; aligned_alloc covers any
 * larger alignment. */
static void *allocate(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment <= _Alignof(max_align_t)) return malloc(size);
    return aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
}

static void release(void *self, const sjs_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const sjs_allocator_vtable heap = { allocate, release };

static const char SOURCE[] =
    "({ total: [1, 2, 3, 4, 5].map((n) => n * n).reduce((sum, n) => sum + n, 0),"
    "   names: [\"grace\", \"ada\"].toSorted() })";

int main(void) {
    sjs_context *context;
    sjs_context_config config = { .allocator = { NULL, &heap }, .limits = SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return 1;

    /* Compile: a source held in memory, as one expression. */
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params compile = {
        .compiler = { .context = context, .goal = SJS_GOAL_EXPRESSION },
        .source = SJS_STR(SOURCE),
        .error = &error,
    };
    if (sjs_compile(&compile, &program) != SJS_OK) {
        fprintf(stderr, "%u:%u: %.*s\n", error.at.line, error.at.column, (int)error.message.length,
                error.message.ptr);
        return 1;
    }

    /* Evaluate: fuel is pushed, and the evaluation stops where it runs out. */
    sjs_vm *vm;
    if (sjs_vm_init(&vm, &(sjs_vm_config){ .context = context }) != SJS_OK) return 1;
    if (sjs_vm_start(vm, &(sjs_run_params){ .program = program }) != SJS_OK) return 1;
    sjs_run status = sjs_vm_run(vm, 100000);
    if (status != SJS_RUN_DONE) {
        if (status == SJS_RUN_FAULT) sjs_vm_error(vm, &error);
        fprintf(stderr, "%s: %.*s\n", status == SJS_RUN_PAUSED ? "out of fuel" : "fault",
                (int)error.message.length, error.message.ptr);
        return 1;
    }
    sjs_value result;
    sjs_vm_take_result(vm, &result);

    /* Write the value as text, into a buffer of any size. */
    sjs_writer *writer;
    if (sjs_writer_init(&writer, &(sjs_writer_config){ .context = context, .value = result }) != SJS_OK) return 1;
    uint8_t bytes[64];
    sjs_result written;
    do {
        sjs_buffer buffer = { bytes, sizeof bytes, 0 };
        written = sjs_writer_write(writer, &buffer);
        fwrite(bytes, 1, buffer.length, stdout);
    } while (written == SJS_NEED_SPACE);
    putchar('\n');

    /* Everything made with the context goes before the context does. */
    sjs_writer_destroy(writer);
    sjs_release(context, result);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    sjs_context_destroy(context);
    return written == SJS_OK ? 0 : 1;
}
