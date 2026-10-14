/* host: staticjs embedded as a statechart's data model would embed it.
 *
 * - a library module, compiled and evaluated once; its exports are a value;
 * - a guard, an expression compiled once with the names the host binds
 *   (globals), evaluated for each event with that event's values;
 * - a native function, In(), answering from the host's own state;
 * - a second module importing the first by name, through a modules adapter;
 * - the clock, pushed by the host: the library never reads one.
 *
 *   ./host
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

/* What every step needs: the context, one evaluator kept for every run, and
 * the library module's exports. */
typedef struct {
    sjs_context *context;
    sjs_vm *vm;
    sjs_value units;
} host;

static void fail(const char *what, const sjs_error *error) {
    fprintf(stderr, "%s: %u:%u: %.*s\n", what, error->at.line, error->at.column, (int)error->message.length,
            error->message.ptr);
    exit(1);
}

static void check(sjs_result result, const char *what) {
    if (result != SJS_OK) {
        fprintf(stderr, "%s: %d\n", what, (int)result);
        exit(1);
    }
}

static void print(host *h, sjs_value value) {
    sjs_writer *writer;
    check(sjs_writer_init(&writer, &(sjs_writer_config){ .context = h->context, .value = value }), "writer");
    char text[256];
    sjs_buffer buffer = { (uint8_t *)text, sizeof text, 0 };
    check(sjs_writer_write(writer, &buffer), "write");
    printf("%.*s\n", (int)buffer.length, text);
    sjs_writer_destroy(writer);
}

static sjs_program *compile(const sjs_compiler_config *config, const char *source) {
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params params = { *config, { source, (uint32_t)strlen(source) }, &error };
    if (sjs_compile(&params, &program) != SJS_OK) fail("compile", &error);
    return program;
}

/* Evaluate a program with values for its globals; the caller owns the result. */
static sjs_result evaluate(host *h, const sjs_run_params *run, sjs_value *out) {
    check(sjs_vm_start(h->vm, run), "start");
    sjs_run status = sjs_vm_run(h->vm, 100000);
    if (status == SJS_RUN_DONE) return sjs_vm_take_result(h->vm, out);
    sjs_error error = { 0 };
    if (status == SJS_RUN_FAULT) sjs_vm_error(h->vm, &error);
    else sjs_vm_reset(h->vm); /* out of fuel: abandon the evaluation */
    printf("fault: %.*s", (int)error.message.length, error.message.ptr);
    if (error.subject.length > 0) printf(" (%.*s)", (int)error.subject.length, error.subject.ptr);
    putchar('\n');
    return SJS_ERR_PROGRAM;
}

/* In(id): whether the statechart is in a state. The host's state is the
 * native's `self`; a native must not change anything a program can see. */
static sjs_result in_state(void *self, sjs_native_call *call) {
    const char *active = self;
    if (call->count != 1 || call->arguments[0].kind != SJS_STRING) {
        call->fault = SJS_STR("In() takes one state's ID");
        return SJS_ERR_PROGRAM;
    }
    sjs_str id = sjs_string_text(call->arguments[0]);
    call->result = sjs_boolean(strlen(active) == id.length && memcmp(active, id.ptr, id.length) == 0);
    return SJS_OK;
}

static const sjs_native_vtable in_vtable = { in_state };

/* The modules adapter: one module, "units", whose exports the host holds. */
static sjs_result find_module(void *self, sjs_str name, sjs_value *exports) {
    host *h = self;
    if (name.length != 5 || memcmp(name.ptr, "units", 5) != 0) return SJS_ERR_NOT_FOUND;
    *exports = h->units; /* borrowed: the program retains what it imports */
    return SJS_OK;
}

static const sjs_modules_vtable modules_vtable = { find_module };

/* An event as the host builds it: { name, data: { f } }. */
static sjs_value event(host *h, const char *name, double fahrenheit) {
    sjs_value e, data, text;
    check(sjs_object_new(h->context, 2, &e), "event");
    check(sjs_object_new(h->context, 1, &data), "data");
    check(sjs_string_new(h->context, (sjs_str){ name, (uint32_t)strlen(name) }, &text), "name");
    check(sjs_object_put(h->context, &data, &(sjs_property){ SJS_STR("f"), sjs_number(fahrenheit) }), "f");
    check(sjs_object_put(h->context, &e, &(sjs_property){ SJS_STR("name"), text }), "put name");
    check(sjs_object_put(h->context, &e, &(sjs_property){ SJS_STR("data"), data }), "put data");
    return e;
}

int main(void) {
    host h = { 0 };
    check(sjs_context_init(&h.context, &(sjs_context_config){ { NULL, &heap }, SJS_LIMITS_DEFAULT }), "context");
    check(sjs_vm_init(&h.vm, &(sjs_vm_config){ .context = h.context }), "evaluator");

    /* 1. A library module: compiled and evaluated once. Its value is its
     * namespace, an object of its exports. */
    sjs_program *units = compile(&(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_MODULE },
                                 "export const celsius = (f) => Math.round((f - 32) * 50 / 9) / 10;\n"
                                 "export const describe = (c) => c > 25 ? \"hot\" : c < 15 ? \"cold\" : \"fine\";\n");
    if (evaluate(&h, &(sjs_run_params){ .program = units }, &h.units) != SJS_OK) return 1;
    sjs_program_release(units);

    /* 2. A guard: an expression compiled once against the names the host
     * binds, then evaluated for each event with that event's values. */
    static const sjs_str names[] = { SJS_STR("In"), SJS_STR("celsius"), SJS_STR("event"), SJS_STR("limit") };
    sjs_program *guard = compile(
                                 &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION,
                                                         .globals = names, .global_count = 4 },
                                 "In(\"heating\") && celsius(event.data.f) > limit");
    sjs_value in, celsius;
    check(sjs_function_native(h.context, (sjs_native){ (void *)"heating", &in_vtable }, &in), "native");
    check(sjs_object_get(h.units, SJS_STR("celsius"), &celsius), "celsius"); /* borrowed from the exports */
    static const double readings[] = { 68, 79, 95 };
    for (int i = 0; i < 3; i++) {
        sjs_value values[] = { in, celsius, event(&h, "reading", readings[i]), sjs_number(25) };
        sjs_value result;
        if (evaluate(&h, &(sjs_run_params){ guard, values, 4 }, &result) == SJS_OK) {
            printf("%g F: the guard is %s\n", readings[i], result.kind == SJS_BOOLEAN && result.boolean ? "true" : "false");
            sjs_release(h.context, result);
        }
        sjs_release(h.context, values[2]);
    }
    sjs_program_release(guard);

    /* A native that refuses its arguments faults the evaluation. */
    sjs_program *misuse = compile(
                                  &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION,
                                                          .globals = names, .global_count = 1 },
                                  "In(42)");
    sjs_value result;
    if (evaluate(&h, &(sjs_run_params){ misuse, &in, 1 }, &result) == SJS_OK) sjs_release(h.context, result);
    sjs_program_release(misuse);
    sjs_release(h.context, in);

    /* 3. A module importing "units" by name, through the adapter. */
    sjs_program *report = compile(
                                  &(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_MODULE,
                                                          .modules = { &h, &modules_vtable } },
                                  "import { celsius, describe } from \"units\";\n"
                                  "export const today = [50, 72, 88].map((f) => describe(celsius(f)));\n");
    if (evaluate(&h, &(sjs_run_params){ .program = report }, &result) == SJS_OK) {
        print(&h, result);
        sjs_release(h.context, result);
    }
    sjs_program_release(report);

    /* 4. The clock: set by the host, advanced between runs, never read by
     * the library itself. 2024-01-31T10:00:00Z, then 90 minutes later. */
    sjs_program *now = compile(&(sjs_compiler_config){ .context = h.context, .goal = SJS_GOAL_EXPRESSION },
                               "Temporal.Now.plainDateTimeISO().toString()");
    check(sjs_vm_set_clock(h.vm, &(sjs_epoch){ 1706695200, 0 }), "clock");
    for (int i = 0; i < 2; i++) {
        if (evaluate(&h, &(sjs_run_params){ .program = now }, &result) == SJS_OK) {
            print(&h, result);
            sjs_release(h.context, result);
        }
        check(sjs_vm_advance_clock(h.vm, 90ull * 60u * 1000000000u), "advance");
    }
    sjs_program_release(now);

    sjs_release(h.context, h.units);
    sjs_vm_destroy(h.vm);
    if (sjs_context_live_bytes(h.context) != 0) return fprintf(stderr, "leaked\n"), 1;
    sjs_context_destroy(h.context);
    return 0;
}
