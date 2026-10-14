/* eval: evaluate a staticjs expression or module and print its value.
 *
 *   eval [--module] [--fuel N] [--seed N] [--stats] [SOURCE]
 *
 * SOURCE is the program's text; without it, eval reads standard input. A
 * module's value is its namespace, the object of its exports. The clock that
 * Temporal.Now, Date.now() and new Date() read is set from the host's clock,
 * and Math.random draws from --seed (as `node --random-seed=N` does). Exit
 * status: 0 for a value, 1 when the program is refused or faults, 2 for a
 * usage or I/O error. --stats prints the fuel used and the peak of memory the
 * context held to standard error.
 *
 *   ./eval '[3, 1, 2].toSorted((a, b) => a - b)'
 *   [1,2,3]
 *   ./eval '({ a: 1 }).b.c'
 *   fault at 1:14: cannot read a property of undefined (c)
 */
#include "staticjs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* The allocator: the C library's heap, with the bytes the context holds and
 * their peak counted for --stats. */
typedef struct {
    size_t live;
    size_t peak;
} heap_usage;

static void *allocate(void *self, size_t size, size_t alignment) {
    heap_usage *usage = self;
    void *ptr = alignment <= _Alignof(max_align_t)
                    ? malloc(size)
                    : aligned_alloc(alignment, (size + alignment - 1) / alignment * alignment);
    if (ptr == NULL) return NULL;
    usage->live += size;
    if (usage->live > usage->peak) usage->peak = usage->live;
    return ptr;
}

static void release(void *self, const sjs_allocator_free_params *params) {
    heap_usage *usage = self;
    usage->live -= params->size;
    free(params->ptr);
}

static const sjs_allocator_vtable heap = { allocate, release };

typedef struct {
    sjs_goal goal;
    uint64_t fuel;
    int64_t seed;
    bool stats;
    const char *source;
} options;

static const char *const KINDS[] = { "", "syntax error", "unsupported", "name error", "limit", "out of memory", "fault" };

/* "fault at 1:14: cannot read a property of undefined (c)". The subject is
 * read only while the stage that reported the error lives: the evaluator
 * here. sjs_compile has destroyed its parser and compiler when it returns,
 * so a compile error is reported without one. */
static void report(const sjs_error *error, bool with_subject) {
    fprintf(stderr, "%s at %u:%u: %.*s", KINDS[error->code], error->at.line, error->at.column,
            (int)error->message.length, error->message.ptr);
    if (with_subject && error->subject.length > 0) {
        fprintf(stderr, " (%.*s)", (int)error->subject.length, error->subject.ptr);
    }
    fputc('\n', stderr);
}

static char *read_all(FILE *in, size_t *length) {
    size_t capacity = 4096;
    char *text = malloc(capacity);
    *length = 0;
    while (text != NULL) {
        *length += fread(text + *length, 1, capacity - *length, in);
        if (*length < capacity) return ferror(in) ? (free(text), NULL) : text;
        char *grown = realloc(text, capacity *= 2);
        if (grown == NULL) free(text);
        text = grown;
    }
    return NULL;
}

static bool parse_options(int argc, char **argv, options *out) {
    *out = (options){ SJS_GOAL_EXPRESSION, 10u * 1000u * 1000u, 1, false, NULL };
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--module") == 0) out->goal = SJS_GOAL_MODULE;
        else if (strcmp(argv[i], "--stats") == 0) out->stats = true;
        else if (strcmp(argv[i], "--fuel") == 0 && i + 1 < argc) out->fuel = strtoull(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) out->seed = strtoll(argv[++i], NULL, 10);
        else if (argv[i][0] != '-' && out->source == NULL) out->source = argv[i];
        else return false;
    }
    return true;
}

/* Run a compiled program to its end: its value written to standard output,
 * or the fault reported. */
static int run(sjs_context *context, sjs_program *program, const options *o) {
    sjs_vm *vm;
    if (sjs_vm_init(&vm, &(sjs_vm_config){ .context = context }) != SJS_OK) return 2;
    struct timespec now;
    timespec_get(&now, TIME_UTC);
    sjs_vm_set_clock(vm, &(sjs_epoch){ now.tv_sec, (uint32_t)now.tv_nsec });
    sjs_vm_set_seed(vm, o->seed);
    sjs_vm_start(vm, &(sjs_run_params){ .program = program });
    sjs_run status = sjs_vm_run(vm, o->fuel);
    if (o->stats) fprintf(stderr, "fuel: %llu\n", (unsigned long long)sjs_vm_fuel_used(vm));
    int exit_status = 1;
    if (status == SJS_RUN_PAUSED) {
        fprintf(stderr, "out of fuel after %llu units\n", (unsigned long long)o->fuel);
    } else if (status == SJS_RUN_FAULT) {
        sjs_error error;
        sjs_vm_error(vm, &error);
        report(&error, true);
    } else {
        sjs_value value;
        sjs_vm_take_result(vm, &value);
        sjs_writer *writer;
        sjs_writer_config config = { context, value, SJS_STR("<function>") };
        if (sjs_writer_init(&writer, &config) == SJS_OK) {
            uint8_t bytes[4096];
            sjs_result written;
            do {
                sjs_buffer buffer = { bytes, sizeof bytes, 0 };
                written = sjs_writer_write(writer, &buffer);
                fwrite(bytes, 1, buffer.length, stdout);
            } while (written == SJS_NEED_SPACE);
            putchar('\n');
            sjs_writer_destroy(writer);
            exit_status = written == SJS_OK ? 0 : 2;
        }
        sjs_release(context, value);
    }
    sjs_vm_destroy(vm);
    return exit_status;
}

int main(int argc, char **argv) {
    options o;
    if (!parse_options(argc, argv, &o)) {
        fprintf(stderr, "usage: eval [--module] [--fuel N] [--seed N] [--stats] [SOURCE]\n");
        return 2;
    }
    size_t length = o.source == NULL ? 0 : strlen(o.source);
    char *input = o.source == NULL ? read_all(stdin, &length) : NULL;
    if (o.source == NULL && input == NULL) return 2;

    heap_usage usage = { 0, 0 };
    sjs_context *context;
    sjs_context_config config = { { &usage, &heap }, SJS_LIMITS_DEFAULT };
    if (sjs_context_init(&context, &config) != SJS_OK) return 2;
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params compile = {
        .compiler = { .context = context, .goal = o.goal },
        .source = { input != NULL ? input : o.source, (uint32_t)length },
        .error = &error,
    };
    sjs_result compiled = sjs_compile(&compile, &program);
    int exit_status = 2;
    if (compiled == SJS_OK) {
        exit_status = run(context, program, &o);
        sjs_program_release(program);
    } else if (compiled == SJS_ERR_PROGRAM) {
        report(&error, false);
        exit_status = 1;
    }
    sjs_context_destroy(context);
    if (o.stats) fprintf(stderr, "peak memory: %zu bytes\n", usage.peak);
    free(input);
    return exit_status;
}
