/*
 * The public API: building values, natives, modules, a sink of one's own,
 * the writer in a one-byte buffer, fuel and reset, limits, and running out of
 * memory at every allocation.
 *
 *   api_test          the fast pass
 *   api_test --full   also fails every allocation of a larger program
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "staticjs.h"

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);    \
            failures++;                                                         \
        }                                                                       \
    } while (0)

/* An allocator that counts, and fails from a chosen allocation on. */
typedef struct {
    long allocations;
    long fail_at;     /* 0: never */
    long live;
} counting;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    counting *c = self;
    (void)alignment;
    c->allocations++;
    if (c->fail_at != 0 && c->allocations >= c->fail_at) return NULL;
    c->live++;
    return malloc(size);
}

static void test_free(void *self, const sjs_allocator_free_params *params) {
    counting *c = self;
    c->live--;
    free(params->ptr);
}

static const sjs_allocator_vtable test_vtable = { test_alloc, test_free };

static sjs_context *new_context(counting *c, sjs_limits limits) {
    sjs_context *context = NULL;
    sjs_context_config config = { { c, &test_vtable }, limits };
    if (sjs_context_init(&context, &config) != SJS_OK) return NULL;
    return context;
}

static void write_text(sjs_context *context, sjs_value value, char *out, size_t capacity) {
    sjs_writer *writer;
    sjs_writer_config config = { context, value, { "<function>", 10 } };
    out[0] = '\0';
    if (sjs_writer_init(&writer, &config) != SJS_OK) return;
    sjs_buffer buffer = { (uint8_t *)out, (uint32_t)capacity - 1u, 0 };
    if (sjs_writer_write(writer, &buffer) == SJS_OK) out[buffer.length] = '\0';
    sjs_writer_destroy(writer);
}

typedef struct {
    sjs_context *context;
    const sjs_str *globals;
    const sjs_value *values;
    uint32_t count;
    sjs_modules modules;
    sjs_goal goal;
} env;

/* Compile and run; the result's text, or "error ..." / "fault ...". */
static sjs_result evaluate(const env *e, const char *source, char *out) {
    sjs_program *program;
    sjs_error error = { 0 };
    sjs_compile_params params = { { e->context, e->goal, e->globals, e->count, e->modules },
                                  { source, (uint32_t)strlen(source) }, &error };
    sjs_result r = sjs_compile(&params, &program);
    if (r == SJS_ERR_NOMEM) {
        snprintf(out, 256, "nomem");
        return r;
    }
    if (r != SJS_OK) {
        snprintf(out, 256, "error %d %.*s", (int)error.code, (int)error.message.length, error.message.ptr);
        return r;
    }
    sjs_vm *vm;
    sjs_vm_config config = { e->context };
    r = sjs_vm_init(&vm, &config);
    if (r != SJS_OK) {
        sjs_program_release(program);
        snprintf(out, 256, "vm %d", r);
        return r;
    }
    sjs_run_params run = { program, e->values, e->count };
    r = sjs_vm_start(vm, &run);
    if (r == SJS_OK) {
        sjs_run status = sjs_vm_run(vm, UINT64_MAX);
        if (status == SJS_RUN_DONE) {
            sjs_value v;
            sjs_vm_take_result(vm, &v);
            write_text(e->context, v, out, 4096);
            sjs_release(e->context, v);
        } else {
            sjs_vm_error(vm, &error);
            snprintf(out, 256, "fault %d %.*s", (int)error.code, (int)error.message.length, error.message.ptr);
            r = SJS_ERR_PROGRAM;
        }
    }
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    return r;
}

/* ------------------------------------------------------------------------ */

static void test_values(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_value object;
    CHECK(sjs_object_new(context, 1, &object) == SJS_OK);
    sjs_value s;
    CHECK(sjs_string_new(context, SJS_STR("v"), &s) == SJS_OK);
    sjs_property p1 = { SJS_STR("b"), s };
    sjs_property p2 = { SJS_STR("3"), sjs_number(3) };
    sjs_property p3 = { SJS_STR("a"), sjs_boolean(true) };
    sjs_property p4 = { SJS_STR("b"), sjs_null() };
    CHECK(sjs_object_put(context, &object, &p1) == SJS_OK);
    CHECK(sjs_object_put(context, &object, &p2) == SJS_OK);
    CHECK(sjs_object_put(context, &object, &p3) == SJS_OK);
    CHECK(sjs_object_put(context, &object, &p4) == SJS_OK);
    sjs_property proto = { SJS_STR("__proto__"), sjs_null() };
    CHECK(sjs_object_put(context, &object, &proto) == SJS_ERR_INVALID);
    char text[4096];
    write_text(context, object, text, sizeof text);
    CHECK(strcmp(text, "{\"3\":3,\"b\":null,\"a\":true}") == 0);
    sjs_value got;
    CHECK(sjs_object_get(object, SJS_STR("a"), &got) == SJS_OK && got.kind == SJS_BOOLEAN);
    CHECK(sjs_object_get(object, SJS_STR("z"), &got) == SJS_ERR_NOT_FOUND);

    /* A container someone else holds cannot change. */
    sjs_retain(object);
    CHECK(sjs_object_put(context, &object, &p3) == SJS_ERR_INVALID);
    sjs_release(context, object);

    sjs_value array;
    CHECK(sjs_array_new(context, 0, &array) == SJS_OK);
    for (int i = 0; i < 100; i++) CHECK(sjs_array_append(context, &array, sjs_number(i)) == SJS_OK);
    CHECK(sjs_length(array) == 100 && sjs_array_at(array, 99).as.number == 99);
    CHECK(sjs_array_append(context, &array, object) == SJS_OK);

    sjs_value set;
    CHECK(sjs_set_new(context, 0, &set) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(1)) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(1)) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(-0.0)) == SJS_OK);
    CHECK(sjs_length(set) == 2);
    sjs_retain(array);
    CHECK(sjs_set_add(context, &set, array) == SJS_ERR_INVALID);
    sjs_release(context, array);
    write_text(context, set, text, sizeof text);
    CHECK(strcmp(text, "new Set([1,0])") == 0);
    sjs_release(context, set);

    CHECK(sjs_string_new(context, (sjs_str){ "\xff", 1 }, &s) == SJS_ERR_INVALID);
    sjs_release(context, array);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* SCXML's In(): a native that answers from the embedder's state. */
static sjs_result in_state(void *self, sjs_native_call *call) {
    const char *active = self;
    if (call->count < 1 || call->arguments[0].kind != SJS_STRING) {
        call->fault = SJS_STR("In() takes a state's ID");
        return SJS_ERR_PROGRAM;
    }
    sjs_str id = sjs_string_text(call->arguments[0]);
    call->result = sjs_boolean(strlen(active) == id.length && memcmp(active, id.ptr, id.length) == 0);
    return SJS_OK;
}

static const sjs_native_vtable in_vtable = { in_state };

static void test_natives_and_globals(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_value in;
    CHECK(sjs_function_native(context, (sjs_native){ (void *)"active", &in_vtable }, &in) == SJS_OK);
    sjs_value limit = sjs_number(3);
    sjs_str names[2] = { SJS_STR("In"), SJS_STR("limit") };
    sjs_value values[2] = { in, limit };
    env e = { context, names, values, 2, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    char out[4096];
    CHECK(evaluate(&e, "[In(\"active\"), In(\"idle\"), limit + 1, [\"active\"].map(In)]", out) == SJS_OK);
    CHECK(strcmp(out, "[true,false,4,[true]]") == 0);
    CHECK(evaluate(&e, "In(1)", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "native") != NULL);
    sjs_release(context, in);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

typedef struct {
    sjs_value exports;
} one_module;

static sjs_result find(void *self, sjs_str name, sjs_value *exports) {
    one_module *m = self;
    if (name.length != 3 || memcmp(name.ptr, "lib", 3) != 0) return SJS_ERR_NOT_FOUND;
    *exports = m->exports;
    return SJS_OK;
}

static const sjs_modules_vtable find_vtable = { find };

static void test_modules(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    /* Compile and evaluate the module; its value is its exports. */
    const char *source = "export const inc = x => x + 1;\nexport const two = inc(1);\n";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_MODULE, NULL, 0, { NULL, NULL } },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    one_module m;
    CHECK(sjs_vm_take_result(vm, &m.exports) == SJS_OK);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    /* A module made from another: the second program keeps the first's
     * functions alive after the exports are released. */
    char out[4096];
    env e = { context, NULL, NULL, 0, { &m, &find_vtable }, SJS_GOAL_MODULE };
    CHECK(evaluate(&e, "import { inc, two } from \"lib\";\nexport const three = inc(two);\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"three\":3}") == 0);
    sjs_release(context, m.exports);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

/* A sink of one's own: count the events, and see the tokens' positions. */
typedef struct {
    int events;
    int names;
    sjs_position last_name;
} tally;

static sjs_result count_event(void *self, const sjs_event *event) {
    tally *t = self;
    t->events++;
    if (event->kind == SJS_EV_NAME) {
        t->names++;
        t->last_name = event->at;
    }
    return SJS_OK;
}

static const sjs_sink_vtable tally_vtable = { count_event };

static void test_sink(void) {
    counting c = { 0 };
    tally t = { 0 };
    sjs_parser *parser;
    sjs_parser_config config = { { &c, &test_vtable }, SJS_LIMITS_DEFAULT, SJS_GOAL_EXPRESSION, { &t, &tally_vtable } };
    CHECK(sjs_parser_init(&parser, &config) == SJS_OK);
    const char *source = "a +\n  b";
    for (size_t i = 0; source[i] != '\0'; i++) {
        CHECK(sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source + i, 1 }) == SJS_OK);
    }
    CHECK(sjs_parser_finish(parser) == SJS_OK);
    CHECK(t.names == 2 && t.events == 4);
    CHECK(t.last_name.line == 2 && t.last_name.column == 3 && t.last_name.offset == 6);
    sjs_parser_destroy(parser);

    /* A syntax error says where. */
    t = (tally){ 0 };
    CHECK(sjs_parser_init(&parser, &config) == SJS_OK);
    const char *bad = "[1,\n 2 3]";
    CHECK(sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)bad, (uint32_t)strlen(bad) }) == SJS_ERR_PROGRAM);
    sjs_error error;
    CHECK(sjs_parser_error(parser, &error) == SJS_OK);
    CHECK(error.code == SJS_ERROR_SYNTAX && error.at.line == 2 && error.at.column == 4);
    sjs_parser_destroy(parser);
    CHECK(c.live == 0);
}

static void test_writer_resumes(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    char whole[4096];
    const char *source = "({a: [1, \"two\\n\", new Set([3])], \"b c\": {d: null, e: -0}})";
    CHECK(evaluate(&e, source, whole) == SJS_OK);
    /* The same value, written a byte at a time. */
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL } },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    sjs_writer *writer;
    sjs_writer_config wc = { context, v, { "", 0 } };
    CHECK(sjs_writer_init(&writer, &wc) == SJS_OK);
    char text[4096];
    size_t length = 0;
    for (;;) {
        uint8_t byte;
        sjs_buffer one = { &byte, 1, 0 };
        sjs_result r = sjs_writer_write(writer, &one);
        if (one.length == 1) text[length++] = (char)byte;
        if (r == SJS_OK) break;
        CHECK(r == SJS_NEED_SPACE);
        if (r != SJS_NEED_SPACE) break;
    }
    text[length] = '\0';
    CHECK(strcmp(text, whole) == 0);
    sjs_writer_destroy(writer);
    sjs_release(context, v);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

static void test_fuel_and_reset(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    const char *source = "[1, 2, 3, 4, 5, 6, 7, 8].map(x => x * x).filter(x => x % 2 === 0).reduce((a, b) => a + b, 0)";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL } },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, 10) == SJS_RUN_PAUSED);
    CHECK(sjs_vm_fuel_used(vm) == 10);
    /* Abandoned inside a callback: everything it held is released. */
    sjs_vm_reset(vm);
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    uint64_t total = sjs_vm_fuel_used(vm);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK && v.kind == SJS_NUMBER && v.as.number == 120);
    /* The same evaluation in steps of one costs the same fuel. */
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    sjs_run status;
    long steps = 0;
    while ((status = sjs_vm_run(vm, 1)) == SJS_RUN_PAUSED) steps++;
    CHECK(status == SJS_RUN_DONE && (uint64_t)steps + 1 >= total && sjs_vm_fuel_used(vm) == total);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

static void test_limits(void) {
    counting c = { 0 };
    sjs_limits limits = SJS_LIMITS_DEFAULT;
    limits.max_nesting = 8;
    limits.max_stack = 64;
    limits.max_frames = 4;
    sjs_context *context = new_context(&c, limits);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    char out[4096];
    CHECK(evaluate(&e, "[[[[[[[[[[1]]]]]]]]]]", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "error 4", 7) == 0);
    CHECK(evaluate(&e, "[...[" "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                       "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]].map(x => x).length", out) == SJS_OK);
    CHECK(evaluate(&e, "Math.max(...[1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,"
                       "1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1])", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "fault 4", 7) == 0);
    CHECK(evaluate(&e, "(a => (b => (c => (d => (e => 1)())())())())()", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "fault 4", 7) == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

/* Fail every allocation in turn: each run ends with an error, never a
 * crash, and frees all it took. */
static void test_out_of_memory(const char *source, sjs_goal goal) {
    long needed = 0;
    for (long fail_at = 1;; fail_at++) {
        counting c = { 0 };
        sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
        if (context == NULL) {
            CHECK(c.live == 0);
            continue;
        }
        c.fail_at = fail_at;
        env e = { context, NULL, NULL, 0, { NULL, NULL }, goal };
        char out[4096];
        sjs_result r = evaluate(&e, source, out);
        c.fail_at = 0;
        bool ok = r == SJS_OK;
        if (!ok && strncmp(out, "error 5", 7) != 0 && strncmp(out, "fault 5", 7) != 0 && strcmp(out, "nomem") != 0 &&
            strncmp(out, "vm -2", 5) != 0) {
            fprintf(stderr, "allocation %ld: %s\n", fail_at, out);
            failures++;
        }
        CHECK(sjs_context_live_bytes(context) == 0);
        sjs_context_destroy(context);
        CHECK(c.live == 0);
        if (ok) {
            needed = fail_at;
            break;
        }
    }
    CHECK(needed > 1);
}

int main(int argc, char **argv) {
    bool full = argc > 1 && strcmp(argv[1], "--full") == 0;
    test_values();
    test_natives_and_globals();
    test_modules();
    test_sink();
    test_writer_resumes();
    test_fuel_and_reset();
    test_limits();
    test_out_of_memory("[1, 2, 3].map(x => ({x, s: `${x}`}))", SJS_GOAL_EXPRESSION);
    if (full) {
        test_out_of_memory("export const f = (xs) => xs.toSorted((a, b) => b - a).flatMap(x => [x, [x]]);\n"
                           "export const g = new Set(f([3, 1, 2]).flat()).union(new Set([9]));\n"
                           "export const h = Object.fromEntries(Object.entries({a: 1, b: 2}).map(([k, v]) => [k, `${v}`]));\n",
                           SJS_GOAL_MODULE);
    }
    if (failures != 0) {
        fprintf(stderr, "api_test: %d failure(s)\n", failures);
        return 1;
    }
    printf("api_test: values, natives, modules, sinks, the writer, fuel, limits and out of memory%s\n",
           full ? " (every allocation of the larger program)" : "");
    return 0;
}
