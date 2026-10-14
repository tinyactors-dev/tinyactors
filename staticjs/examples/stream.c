/* stream: every stage pushed by its caller, as a host with an event loop
 * drives them.
 *
 * - the source arrives in chunks (from a socket, a file) and is pushed to
 *   the parser as it comes; a token may span two chunks;
 * - a sink of the host's own sees the parser's events before the compiler
 *   does (a linter, a formatter, a metric could be another);
 * - the evaluation runs on small slices of fuel, so the host can do other
 *   work, or give up, between them;
 * - the result is written through a buffer smaller than it.
 *
 *   ./stream
 */
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* A sink in front of the compiler: it counts the events and the functions
 * the program declares, then passes each event on. */
typedef struct {
    sjs_sink next;
    uint32_t events;
    uint32_t functions;
} counting_sink;

static sjs_result count_event(void *self, const sjs_event *event) {
    counting_sink *sink = self;
    sink->events++;
    if (event->kind == SJS_EV_ARROW_BEGIN) sink->functions++;
    return sink->next.v->event(sink->next.self, event);
}

static const sjs_sink_vtable counting_vtable = { count_event };

static const char SOURCE[] =
    "const range = (n) => Array.from({ length: n }, (_, i) => i);\n"
    "const primes = (n) => range(n).filter((k) => k > 1 &&\n"
    "  range(k).every((d) => d < 2 || k % d !== 0));\n"
    "export const first = primes(60);\n"
    "export const sum = first.reduce((a, b) => a + b, 0);\n";

static void report(const char *stage, const sjs_error *error) {
    fprintf(stderr, "%s: %u:%u: %.*s\n", stage, error->at.line, error->at.column, (int)error->message.length,
            error->message.ptr);
}

int main(void) {
    sjs_allocator allocator = { NULL, &heap };
    sjs_context *context;
    if (sjs_context_init(&context, &(sjs_context_config){ allocator, SJS_LIMITS_DEFAULT }) != SJS_OK) return 1;

    /* Bytes in, events out: the parser feeds our sink, which feeds the
     * compiler. */
    sjs_compiler *compiler;
    if (sjs_compiler_init(&compiler, &(sjs_compiler_config){ .context = context, .goal = SJS_GOAL_MODULE }) != SJS_OK)
        return 1;
    counting_sink sink = { sjs_compiler_sink(compiler), 0, 0 };
    sjs_parser *parser;
    sjs_parser_config parser_config = { allocator, SJS_LIMITS_DEFAULT, SJS_GOAL_MODULE, { &sink, &counting_vtable } };
    if (sjs_parser_init(&parser, &parser_config) != SJS_OK) return 1;

    const size_t chunk = 7; /* as small as a slow network delivers */
    sjs_result parsed = SJS_OK;
    for (size_t at = 0; parsed == SJS_OK && at < sizeof SOURCE - 1; at += chunk) {
        size_t length = sizeof SOURCE - 1 - at < chunk ? sizeof SOURCE - 1 - at : chunk;
        parsed = sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)SOURCE + at, (uint32_t)length });
    }
    if (parsed == SJS_OK) parsed = sjs_parser_finish(parser);
    sjs_error error;
    if (parsed != SJS_OK) {
        /* The parser's error, or the compiler's when our sink passed on a
         * refusal from it. */
        if (sjs_compiler_error(compiler, &error) == SJS_OK) report("compile", &error);
        else if (sjs_parser_error(parser, &error) == SJS_OK) report("parse", &error);
        return 1;
    }
    sjs_parser_destroy(parser);
    sjs_program *program;
    if (sjs_compiler_finish(compiler, &program) != SJS_OK) return 1;
    sjs_compiler_destroy(compiler);
    printf("compiled from %zu-byte chunks: %u events, %u functions\n", chunk, sink.events, sink.functions);

    /* Fuel in, a value out: 500 units at a time. */
    sjs_vm *vm;
    if (sjs_vm_init(&vm, &(sjs_vm_config){ .context = context }) != SJS_OK) return 1;
    if (sjs_vm_start(vm, &(sjs_run_params){ .program = program }) != SJS_OK) return 1;
    unsigned slices = 1;
    sjs_run status;
    while ((status = sjs_vm_run(vm, 500)) == SJS_RUN_PAUSED) {
        slices++; /* the host's other work goes here; sjs_vm_reset() gives up */
    }
    if (status == SJS_RUN_FAULT) {
        sjs_vm_error(vm, &error);
        report("fault", &error);
        return 1;
    }
    printf("evaluated in %u slices, %llu units of fuel\n", slices, (unsigned long long)sjs_vm_fuel_used(vm));
    sjs_value result;
    sjs_vm_take_result(vm, &result);

    /* A value out as text, through a buffer of 16 bytes. */
    sjs_writer *writer;
    if (sjs_writer_init(&writer, &(sjs_writer_config){ .context = context, .value = result }) != SJS_OK) return 1;
    uint8_t bytes[16];
    unsigned writes = 0;
    sjs_result written;
    do {
        sjs_buffer buffer = { bytes, sizeof bytes, 0 };
        written = sjs_writer_write(writer, &buffer);
        fwrite(bytes, 1, buffer.length, stdout);
        writes++;
    } while (written == SJS_NEED_SPACE);
    printf("\nwritten in %u pieces\n", writes);

    sjs_writer_destroy(writer);
    sjs_release(context, result);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    sjs_context_destroy(context);
    return written == SJS_OK ? 0 : 1;
}
