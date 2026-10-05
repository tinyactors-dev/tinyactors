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
    size_t bytes;     /* live bytes, and the most there were */
    size_t peak;
} counting;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    counting *c = self;
    (void)alignment;
    c->allocations++;
    if (c->fail_at != 0 && c->allocations >= c->fail_at) return NULL;
    c->live++;
    c->bytes += size;
    if (c->bytes > c->peak) c->peak = c->bytes;
    return malloc(size);
}

static void test_free(void *self, const sjs_allocator_free_params *params) {
    counting *c = self;
    c->live--;
    c->bytes -= params->size;
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

    /* Past SJS_INDEX_FROM items, lookups go through a hash index. */
    sjs_value large;
    CHECK(sjs_object_new(context, 0, &large) == SJS_OK);
    char name[2] = { 0, 0 };
    for (int i = 0; i < 20; i++) {
        name[0] = (char)('a' + i);
        sjs_property p = { (sjs_str){ name, 1 }, sjs_number(i) };
        CHECK(sjs_object_put(context, &large, &p) == SJS_OK);
    }
    sjs_property again = { SJS_STR("c"), sjs_number(-1) };
    CHECK(sjs_object_put(context, &large, &again) == SJS_OK);
    CHECK(sjs_length(large) == 20);
    CHECK(sjs_object_get(large, SJS_STR("t"), &got) == SJS_OK && got.as.number == 19);
    CHECK(sjs_object_get(large, SJS_STR("c"), &got) == SJS_OK && got.as.number == -1);
    CHECK(sjs_object_get(large, SJS_STR("z"), &got) == SJS_ERR_NOT_FOUND);
    sjs_release(context, large);
    CHECK(sjs_set_new(context, 0, &set) == SJS_OK);
    for (int i = 0; i < 40; i++) CHECK(sjs_set_add(context, &set, sjs_number(i % 20)) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(0.0 / 0.0)) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(0.0 / 0.0)) == SJS_OK);
    CHECK(sjs_set_add(context, &set, sjs_number(-0.0)) == SJS_OK);
    CHECK(sjs_length(set) == 21);
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

/* A name bound nowhere is refused even where it never runs: in a branch not
 * taken, or after a return. JavaScript throws only if it runs, so the spec,
 * which Node checks, cannot hold these. */
static void test_unbound_where_it_never_runs(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    char out[4096];
    CHECK(evaluate(&e, "(x => { if (x) return 1; return 2; })(0)", out) == SJS_OK && strcmp(out, "2") == 0);
    static const char *const never_runs[] = {
        "(x => { if (x) return nope; return 1; })(0)",
        "(x => { if (x) return 1; else { const y = nope; } return 2; })(1)",
        "(x => { return 1; return nope; })(0)",
        "(x => { return 1; { const y = nope; } })(0)",
    };
    for (size_t i = 0; i < sizeof never_runs / sizeof never_runs[0]; i++) {
        CHECK(evaluate(&e, never_runs[i], out) == SJS_ERR_PROGRAM);
        CHECK(strncmp(out, "error ", 6) == 0 && strstr(out, "not bound") != NULL);
    }
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
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

static sjs_value object_of(sjs_context *context, const char *const *keys, int count) {
    sjs_value o;
    CHECK(sjs_object_new(context, 2, &o) == SJS_OK);
    for (int i = 0; i < count; i++) {
        sjs_property property = { { keys[i], (uint32_t)strlen(keys[i]) }, sjs_number(i + 1) };
        CHECK(sjs_object_put(context, &o, &property) == SJS_OK);
    }
    return o;
}

/* An embedder's module object may be in any order: a namespace of it is
 * sorted, as JavaScript sorts one. One with an array-index key cannot be. */
static void test_embedder_namespaces(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    static const char *const unsorted[] = { "b", "a" };
    one_module m = { object_of(context, unsorted, 2) };
    env e = { context, NULL, NULL, 0, { &m, &find_vtable }, SJS_GOAL_MODULE };
    CHECK(evaluate(&e, "import * as ns from \"lib\";\nexport const keys = Object.keys(ns);\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"keys\":[\"a\",\"b\"]}") == 0);
    CHECK(evaluate(&e, "export * from \"lib\";\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"a\":2,\"b\":1}") == 0);
    CHECK(evaluate(&e, "export * as ns from \"lib\";\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"ns\":{\"a\":2,\"b\":1}}") == 0);
    sjs_release(context, m.exports);

    static const char *const indexed[] = { "x", "0" };
    m.exports = object_of(context, indexed, 2);
    CHECK(evaluate(&e, "import * as ns from \"lib\";\n", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "error 2 ", 8) == 0 && strstr(out, "array index") != NULL);
    CHECK(evaluate(&e, "export * from \"lib\";\n", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "error 2 ", 8) == 0);
    CHECK(evaluate(&e, "import { x } from \"lib\";\nexport const y = x;\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"y\":1}") == 0);
    sjs_release(context, m.exports);

    /* By UTF-16 code units, U+1F600 (a surrogate pair, 0xD83D first) comes
     * before U+FF61; by UTF-8 bytes or by code points it comes after. */
    static const char *const wide[] = { "\xEF\xBD\xA1", "\xF0\x9F\x98\x80" };
    m.exports = object_of(context, wide, 2);
    CHECK(evaluate(&e, "import * as ns from \"lib\";\nexport const keys = Object.keys(ns);\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"keys\":[\"\xF0\x9F\x98\x80\",\"\xEF\xBD\xA1\"]}") == 0);
    CHECK(evaluate(&e, "export * from \"lib\";\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"\xF0\x9F\x98\x80\":2,\"\xEF\xBD\xA1\":1}") == 0);
    sjs_release(context, m.exports);

    /* Keys that are no identifier pass through export *, as JavaScript's do;
     * 4294967295 is not an array index. */
    static const char *const odd[] = { "a-b", "4294967295", "" };
    m.exports = object_of(context, odd, 3);
    CHECK(evaluate(&e, "export * from \"lib\";\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"\":3,\"4294967295\":2,\"a-b\":1}") == 0);
    CHECK(evaluate(&e, "import * as ns from \"lib\";\nexport const keys = Object.keys(ns);\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"keys\":[\"\",\"4294967295\",\"a-b\"]}") == 0);
    sjs_release(context, m.exports);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* What compiling holds grows with the source, and with each module it
 * imports from once: not with the source times the modules. */
static void test_compile_costs(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    enum { KEYS = 400, STATEMENTS = 400 };
    sjs_value o;
    CHECK(sjs_object_new(context, KEYS, &o) == SJS_OK);
    for (int i = KEYS; i-- > 0;) {
        char key[16];
        snprintf(key, sizeof key, "k%03d", i);
        sjs_property property = { { key, (uint32_t)strlen(key) }, sjs_number(i) };
        CHECK(sjs_object_put(context, &o, &property) == SJS_OK);
    }
    one_module m = { o };
    sjs_compiler_config config = { context, SJS_GOAL_MODULE, NULL, 0, { &m, &find_vtable } };
    static char source[STATEMENTS * 32];
    const char *const forms[] = { "export * from \"lib\";\n", "import * as n%d from \"lib\";\n" };
    for (int form = 0; form < 2; form++) {
        size_t used = 0;
        for (int i = 0; i < STATEMENTS; i++) used += (size_t)snprintf(source + used, sizeof source - used, forms[form], i);
        size_t before = c.bytes;
        c.peak = c.bytes;
        sjs_program *program;
        sjs_compile_params params = { config, { source, (uint32_t)used }, NULL };
        CHECK(sjs_compile(&params, &program) == SJS_OK);
        /* One module's keys, a few times over: not once per statement. */
        size_t held = c.peak - before;
        if (held > 512u * 1024u) fprintf(stderr, "compiling form %d held %zu bytes\n", form, held);
        CHECK(held <= 512u * 1024u);
        sjs_program_release(program);
    }
    sjs_release(context, o);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* A sink sees every import and export form as its events say. */
typedef struct {
    char text[2048];
} event_log;

static sjs_result log_event(void *self, const sjs_event *event) {
    event_log *log = self;
    size_t used = strlen(log->text);
    snprintf(log->text + used, sizeof log->text - used, "%d:%u:%.*s:%.*s:%.*s\n", (int)event->kind, event->count,
             (int)event->text.length, event->text.ptr, (int)event->alias.length, event->alias.ptr,
             (int)event->name.length, event->name.ptr);
    return SJS_OK;
}

static const sjs_sink_vtable log_vtable = { log_event };

static void test_module_events(void) {
    counting c = { 0 };
    static event_log log;
    log.text[0] = '\0';
    sjs_parser *parser;
    sjs_parser_config config = { { &c, &test_vtable }, SJS_LIMITS_DEFAULT, SJS_GOAL_MODULE, { &log, &log_vtable } };
    CHECK(sjs_parser_init(&parser, &config) == SJS_OK);
    const char *source = "import a, * as b from \"m\"; export { c as d } from \"m\"; export * from \"m\";\n"
                         "export default 1; export { e }; const e = 2; import \"m\";";
    for (size_t i = 0; source[i] != '\0'; i++) {
        CHECK(sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source + i, 1 }) == SJS_OK);
    }
    CHECK(sjs_parser_finish(parser) == SJS_OK);
    sjs_parser_destroy(parser);
    char want[2048];
    snprintf(want, sizeof want,
             "%d:%d:m:default:a\n%d:%d:m::b\n%d:%d:m:c:d\n%d:%d:m::\n"
             "%d:0:::\n%d:0:::\n%d:0::e:e\n"
             "%d:0:::\n%d:0:::\n%d:0:e::\n%d:0:::\n%d:%d:m::\n%d:0:::\n",
             SJS_EV_IMPORT, SJS_IMPORT_NAME, SJS_EV_IMPORT, SJS_IMPORT_NAMESPACE, SJS_EV_EXPORT_FROM, SJS_IMPORT_NAME,
             SJS_EV_EXPORT_FROM, SJS_IMPORT_STAR, SJS_EV_NUMBER, SJS_EV_EXPORT_DEFAULT, SJS_EV_EXPORT_NAME,
             SJS_EV_CONST_BEGIN, SJS_EV_NUMBER, SJS_EV_BIND, SJS_EV_CONST_END, SJS_EV_IMPORT, SJS_IMPORT_MODULE, SJS_EV_END);
    if (strcmp(log.text, want) != 0) fprintf(stderr, "events:\n%s\nwanted:\n%s\n", log.text, want);
    CHECK(strcmp(log.text, want) == 0);
    CHECK(c.live == 0);
}

/* The events of `source`, pushed one byte at a time, as log_event writes them. */
static void parse_events(const char *source, sjs_goal goal, event_log *log) {
    counting c = { 0 };
    log->text[0] = '\0';
    sjs_parser *parser;
    sjs_parser_config config = { { &c, &test_vtable }, SJS_LIMITS_DEFAULT, goal, { log, &log_vtable } };
    CHECK(sjs_parser_init(&parser, &config) == SJS_OK);
    for (size_t i = 0; source[i] != '\0'; i++) {
        CHECK(sjs_parser_push(parser, (sjs_bytes){ (const uint8_t *)source + i, 1 }) == SJS_OK);
    }
    CHECK(sjs_parser_finish(parser) == SJS_OK);
    sjs_parser_destroy(parser);
    CHECK(c.live == 0);
}

/* Each kind and count in `want`, as log_event writes them with no text. */
static void expect_events(const event_log *log, const int *want, int count, const char *const *texts) {
    char text[2048];
    size_t used = 0;
    text[0] = '\0';
    for (int i = 0; i < count; i++) {
        used += (size_t)snprintf(text + used, sizeof text - used, "%d:%d:%s::\n", want[2 * i], want[2 * i + 1],
                                 texts[i] != NULL ? texts[i] : "");
    }
    if (strcmp(log->text, text) != 0) fprintf(stderr, "events:\n%s\nwanted:\n%s\n", log->text, text);
    CHECK(strcmp(log->text, text) == 0);
}

/* A default value's events come before its target's, and a parameter list
 * says how many parameters it has. */
static void test_pattern_events(void) {
    static event_log log;
    parse_events("(a, {b = 1} = {}, ...c) => a", SJS_GOAL_EXPRESSION, &log);
    static const int params[] = {
        SJS_EV_ARROW_BEGIN, 3, SJS_EV_PARAM, 0, SJS_EV_BIND, 0, SJS_EV_PARAM, 1, SJS_EV_DEFAULT, 0,
        SJS_EV_OBJECT_BEGIN, 0, SJS_EV_OBJECT_END, 0, SJS_EV_DEFAULT_END, 0, SJS_EV_OBJECT_PATTERN_BEGIN, 0,
        SJS_EV_PATTERN_KEY, 0, SJS_EV_DEFAULT, 0, SJS_EV_NUMBER, 0, SJS_EV_DEFAULT_END, 0, SJS_EV_BIND, 0,
        SJS_EV_OBJECT_PATTERN_END, 0, SJS_EV_REST_PARAM, 2, SJS_EV_BIND, 0, SJS_EV_PARAMS_END, 0, SJS_EV_NAME, 0,
        SJS_EV_ARROW_END, 0, SJS_EV_END, 0
    };
    static const char *const params_text[] = {
        NULL, NULL, "a", NULL, NULL, NULL, NULL, NULL, NULL, "b", NULL, NULL, NULL, "b", NULL, NULL, "c", NULL, "a", NULL, NULL
    };
    expect_events(&log, params, (int)(sizeof params_text / sizeof params_text[0]), params_text);

    /* Inside a const's held pattern, a default and the arrow function in it
     * are reordered in place. */
    parse_events("const {x = (y = 2) => y, [k]: z, ...r} = o;", SJS_GOAL_MODULE, &log);
    static const int held[] = {
        SJS_EV_CONST_BEGIN, 0, SJS_EV_NAME, 0, SJS_EV_OBJECT_PATTERN_BEGIN, 0, SJS_EV_PATTERN_KEY, 0,
        SJS_EV_DEFAULT, 0, SJS_EV_ARROW_BEGIN, 1, SJS_EV_PARAM, 0, SJS_EV_DEFAULT, 0, SJS_EV_NUMBER, 0,
        SJS_EV_DEFAULT_END, 0, SJS_EV_BIND, 0, SJS_EV_PARAMS_END, 0, SJS_EV_NAME, 0, SJS_EV_ARROW_END, 0,
        SJS_EV_DEFAULT_END, 0, SJS_EV_BIND, 0, SJS_EV_NAME, 0, SJS_EV_PATTERN_COMPUTED_KEY, 0, SJS_EV_BIND, 0,
        SJS_EV_PATTERN_REST, 0, SJS_EV_BIND, 0, SJS_EV_OBJECT_PATTERN_END, 0, SJS_EV_CONST_END, 0, SJS_EV_END, 0
    };
    static const char *const held_text[] = {
        NULL, "o", NULL, "x", NULL, NULL, NULL, NULL, NULL, NULL, "y", NULL, "y", NULL, NULL, "x", "k", NULL, "z",
        NULL, "r", NULL, NULL, NULL
    };
    expect_events(&log, held, (int)(sizeof held_text / sizeof held_text[0]), held_text);
}

/* Statements: an else-if chain is one IF_END; a block statement has its
 * own events; inside a held pattern, the statements are reordered with it. */
static void test_statement_events(void) {
    static event_log log;
    parse_events("x => { if (x) return 1; else if (!x) { const y = 2; return y; } else ; }", SJS_GOAL_EXPRESSION, &log);
    static const int chain[] = {
        SJS_EV_ARROW_BEGIN, 1, SJS_EV_PARAM, 0, SJS_EV_BIND, 0, SJS_EV_PARAMS_END, 0, SJS_EV_BLOCK_BEGIN, 0,
        SJS_EV_NAME, 0, SJS_EV_IF, 0, SJS_EV_NUMBER, 0, SJS_EV_RETURN, 1, SJS_EV_ELSE, 1,
        SJS_EV_NAME, 0, SJS_EV_UNARY, SJS_OP_NOT, SJS_EV_IF, 0, SJS_EV_BLOCK_STATEMENT_BEGIN, 0,
        SJS_EV_CONST_BEGIN, 0, SJS_EV_NUMBER, 0, SJS_EV_BIND, 0, SJS_EV_CONST_END, 0, SJS_EV_NAME, 0, SJS_EV_RETURN, 1,
        SJS_EV_BLOCK_STATEMENT_END, 0, SJS_EV_ELSE, 0, SJS_EV_IF_END, 0, SJS_EV_BLOCK_END, 0, SJS_EV_ARROW_END, 0,
        SJS_EV_END, 0
    };
    static const char *const chain_text[] = {
        NULL, NULL, "x", NULL, NULL, "x", NULL, NULL, NULL, NULL, "x", NULL, NULL, NULL,
        NULL, NULL, "y", NULL, "y", NULL, NULL, NULL, NULL, NULL, NULL, NULL
    };
    expect_events(&log, chain, (int)(sizeof chain_text / sizeof chain_text[0]), chain_text);

    parse_events("const {f = x => { if (x) return 1; }} = o;", SJS_GOAL_MODULE, &log);
    static const int held[] = {
        SJS_EV_CONST_BEGIN, 0, SJS_EV_NAME, 0, SJS_EV_OBJECT_PATTERN_BEGIN, 0, SJS_EV_PATTERN_KEY, 0, SJS_EV_DEFAULT, 0,
        SJS_EV_ARROW_BEGIN, 1, SJS_EV_PARAM, 0, SJS_EV_BIND, 0, SJS_EV_PARAMS_END, 0, SJS_EV_BLOCK_BEGIN, 0,
        SJS_EV_NAME, 0, SJS_EV_IF, 0, SJS_EV_NUMBER, 0, SJS_EV_RETURN, 1, SJS_EV_IF_END, 0, SJS_EV_BLOCK_END, 0,
        SJS_EV_ARROW_END, 0, SJS_EV_DEFAULT_END, 0, SJS_EV_BIND, 0, SJS_EV_OBJECT_PATTERN_END, 0, SJS_EV_CONST_END, 0,
        SJS_EV_END, 0
    };
    static const char *const held_text[] = {
        NULL, "o", NULL, "f", NULL, NULL, NULL, "x", NULL, NULL, "x", NULL, NULL, NULL, NULL, NULL, NULL, NULL, "f",
        NULL, NULL, NULL
    };
    expect_events(&log, held, (int)(sizeof held_text / sizeof held_text[0]), held_text);
}

/* Events in an order no parser sends are refused, not compiled. */
static void compile_events(const sjs_event *events, int count, const char *want) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_compiler *compiler;
    sjs_compiler_config config = { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL } };
    CHECK(sjs_compiler_init(&compiler, &config) == SJS_OK);
    sjs_sink sink = sjs_compiler_sink(compiler);
    sjs_result r = SJS_OK;
    for (int i = 0; i < count && r == SJS_OK; i++) r = sink.v->event(sink.self, &events[i]);
    CHECK(r == SJS_ERR_PROGRAM);
    sjs_error error;
    CHECK(sjs_compiler_error(compiler, &error) == SJS_OK);
    CHECK(error.message.length == strlen(want) && memcmp(error.message.ptr, want, error.message.length) == 0);
    sjs_compiler_destroy(compiler);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

static void test_statement_order(void) {
    static const char *const out_of_order = "the events are out of order";
    const sjs_event body[] = { { .kind = SJS_EV_ARROW_BEGIN }, { .kind = SJS_EV_PARAMS_END }, { .kind = SJS_EV_BLOCK_BEGIN } };
    /* IF_END with no IF. */
    const sjs_event no_if[] = { body[0], body[1], body[2], { .kind = SJS_EV_IF_END } };
    compile_events(no_if, 4, out_of_order);
    /* RETURN outside a function. */
    const sjs_event top_return[] = { { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_RETURN, .count = 1 } };
    compile_events(top_return, 2, out_of_order);
    /* RETURN in an expression body. */
    const sjs_event expression_return[] = { body[0], body[1], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_RETURN, .count = 1 } };
    compile_events(expression_return, 4, out_of_order);
    /* ELSE(1) waits for the next test and its IF, not a statement. */
    const sjs_event else_if[] = { body[0], body[1], body[2], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_IF },
                                  { .kind = SJS_EV_ELSE, .count = 1 }, { .kind = SJS_EV_RETURN } };
    compile_events(else_if, 7, out_of_order);
    /* The body ends while an if is open. */
    const sjs_event open_if[] = { body[0], body[1], body[2], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_IF },
                                  { .kind = SJS_EV_BLOCK_END } };
    compile_events(open_if, 6, out_of_order);
    /* A block statement ends that never began, and one that is not a statement. */
    const sjs_event no_block[] = { body[0], body[1], body[2], { .kind = SJS_EV_BLOCK_STATEMENT_END } };
    compile_events(no_block, 4, out_of_order);
    const sjs_event block_in_expression[] = { { .kind = SJS_EV_BLOCK_STATEMENT_BEGIN } };
    compile_events(block_in_expression, 1, out_of_order);
    /* A const as the statement of an if or an else: JavaScript has no such
     * program, and its binding would outlive the branch that never set it. */
    const sjs_event const_if[] = { body[0], body[1], body[2], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_IF },
                                   { .kind = SJS_EV_CONST_BEGIN } };
    compile_events(const_if, 6, out_of_order);
    const sjs_event const_else[] = { body[0], body[1], body[2], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_IF },
                                     { .kind = SJS_EV_RETURN }, { .kind = SJS_EV_ELSE }, { .kind = SJS_EV_CONST_BEGIN } };
    compile_events(const_else, 8, out_of_order);
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

    /* A parameter list, defaults included, must fit in max_lookahead tokens;
     * a parenthesized expression is told apart at its first token no pattern
     * takes. */
    limits = SJS_LIMITS_DEFAULT;
    limits.max_lookahead = 16;
    context = new_context(&c, limits);
    e.context = context;
    CHECK(evaluate(&e, "((a, b = 1) => a + b)(1)", out) == SJS_OK && strcmp(out, "2") == 0);
    CHECK(evaluate(&e, "((a = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1]) => a)()", out) == SJS_ERR_PROGRAM);
    CHECK(strncmp(out, "error 4", 7) == 0);
    CHECK(evaluate(&e, "([1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1]).length", out) == SJS_OK);
    CHECK(strcmp(out, "40") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* The lookahead after `(` opens as many pattern levels as max_nesting
     * allows, as the parser itself does: 70 nested arrays in parentheses,
     * and a parameter 70 patterns deep. */
    char source[1024];
    char *at = source;
    at += sprintf(at, "(");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = '1';
    for (int i = 0; i < 70; i++) *at++ = ']';
    sprintf(at, ").length");
    context = new_context(&c, SJS_LIMITS_DEFAULT);
    e.context = context;
    CHECK(evaluate(&e, source, out) == SJS_OK && strcmp(out, "1") == 0);
    at = source;
    at += sprintf(at, "((");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = 'a';
    for (int i = 0; i < 70; i++) *at++ = ']';
    at += sprintf(at, ") => a)(");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = '7';
    for (int i = 0; i < 70; i++) *at++ = ']';
    sprintf(at, ")");
    CHECK(evaluate(&e, source, out) == SJS_OK && strcmp(out, "7") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    limits = SJS_LIMITS_DEFAULT;
    limits.max_nesting = 8;
    context = new_context(&c, limits);
    e.context = context;
    CHECK(evaluate(&e, "(([[[[[[[[[a]]]]]]]]]) => a)", out) == SJS_ERR_PROGRAM);
    CHECK(strcmp(out, "error 4 the program nests deeper than max_nesting") == 0);
    /* Each if and block nests, but an else-if chain of any length nests once. */
    CHECK(evaluate(&e, "(a => { if (a) if (a) if (a) if (a) if (a) if (a) if (a) if (a) if (a) return 1; })", out) == SJS_ERR_PROGRAM);
    CHECK(strcmp(out, "error 4 the program nests deeper than max_nesting") == 0);
    CHECK(evaluate(&e, "(a => { { { { { { { { { return 1; } } } } } } } } })", out) == SJS_ERR_PROGRAM);
    CHECK(strcmp(out, "error 4 the program nests deeper than max_nesting") == 0);
    {
        static char chain[64 * 1024];
        char *end = chain;
        end += sprintf(end, "(x => { if (x === 0) return 0;");
        for (int i = 1; i < 1000; i++) end += sprintf(end, " else if (x === %d) { const y = %d; return y; }", i, i);
        sprintf(end, " return -1; })(999)");
        CHECK(evaluate(&e, chain, out) == SJS_OK && strcmp(out, "999") == 0);
    }
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* A const's pattern, its default values included, is held until its
     * value has been sent: max_held_events bounds it. */
    limits = SJS_LIMITS_DEFAULT;
    limits.max_held_events = 16;
    context = new_context(&c, limits);
    e.context = context;
    CHECK(evaluate(&e, "(() => { const [a = [1, 2]] = []; return a.length; })()", out) == SJS_OK && strcmp(out, "2") == 0);
    CHECK(evaluate(&e, "(() => { const [a = [1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1]] = []; return a; })()", out) ==
          SJS_ERR_PROGRAM);
    CHECK(strcmp(out, "error 4 the parser holds more events than max_held_events") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* A value updated in place grows only to max_value_items, as a copy
     * does, and a fault there frees it. */
    limits = SJS_LIMITS_DEFAULT;
    limits.max_value_items = 4;
    context = new_context(&c, limits);
    e.context = context;
    static const char *const grow_in_place[] = {
        "[1, 2, 3, 4, 5].reduce((acc, x) => [...acc, x], [])",
        "[1, 2, 3, 4, 5].reduce((acc, x) => acc.concat(x), [])",
        "[1, 2, 3].reduce((acc, x) => acc.concat([x, x]), [])",
        "[\"a\", \"b\", \"c\", \"d\", \"e\"].reduce((acc, k) => ({ ...acc, [k]: 1 }), {})",
        "[\"a\", \"b\", \"c\"].reduce((acc, k) => ({ ...acc, [k]: 1, [k + k]: 2 }), {})",
    };
    for (size_t i = 0; i < sizeof grow_in_place / sizeof grow_in_place[0]; i++) {
        CHECK(evaluate(&e, grow_in_place[i], out) == SJS_ERR_PROGRAM);
        CHECK(strncmp(out, "fault 4", 7) == 0);
        CHECK(sjs_context_live_bytes(context) == 0);
    }
    CHECK(evaluate(&e, "[1, 2, 3, 4].reduce((acc, x) => [...acc, x], [])", out) == SJS_OK && strcmp(out, "[1,2,3,4]") == 0);
    CHECK(evaluate(&e, "[\"a\", \"b\"].reduce((acc, k) => ({ ...acc, [k]: 1, [k + k]: 2 }), {})", out) == SJS_OK);
    CHECK(strcmp(out, "{\"a\":1,\"aa\":2,\"b\":1,\"bb\":2}") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

/* Natives for test_reuse_leaves_others_alone: `keep` retains each value it
 * is given, `held` gives a retained copy of a value the embedder holds. */
typedef struct {
    sjs_value kept[4];
    int count;
    sjs_value held;
} keeper;

static sjs_result keep_value(void *self, sjs_native_call *call) {
    keeper *k = self;
    if (call->count < 1 || k->count >= 4) {
        call->fault = SJS_STR("keep() keeps four values");
        return SJS_ERR_PROGRAM;
    }
    sjs_retain(call->arguments[0]);
    k->kept[k->count++] = call->arguments[0];
    call->result = sjs_undefined();
    return SJS_OK;
}

static sjs_result held_value(void *self, sjs_native_call *call) {
    keeper *k = self;
    sjs_retain(k->held);
    call->result = k->held;
    return SJS_OK;
}

static const sjs_native_vtable keep_vtable = { keep_value };
static const sjs_native_vtable held_vtable = { held_value };

static sjs_value number_array(sjs_context *context, const double *numbers, int count) {
    sjs_value a;
    CHECK(sjs_array_new(context, 2, &a) == SJS_OK);
    for (int i = 0; i < count; i++) CHECK(sjs_array_append(context, &a, sjs_number(numbers[i])) == SJS_OK);
    return a;
}

/* Every way a value is updated in place, in one module: a moved
 * accumulator, literals that take over a spread, concat, with and toSorted
 * on an array nothing else holds. */
static const char reuse_module[] =
    "export const f = (xs) => xs.reduce((acc, x, i) => {\n"
    "  const list = [...acc.list, x].concat([String(i)]).with(0, x);\n"
    "  return { ...acc, [x]: i, list };\n"
    "}, { list: [\"z\"] });\n"
    "export const g = f([\"b\", \"a\", \"c\"]);\n"
    "export const h = g.list.toSorted((p, q) => (p < q ? -1 : 1));\n";

/* A value is updated in place only when nothing else holds it: never one a
 * native keeps or gives, a global, or a module's export, each of which the
 * embedder holds a reference to, even when that is the only one. */
static void test_reuse_leaves_others_alone(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    keeper k = { 0 };
    static const char *const just_a[] = { "a" };
    k.held = object_of(context, just_a, 1);
    sjs_value natives[2];
    CHECK(sjs_function_native(context, (sjs_native){ &k, &keep_vtable }, &natives[0]) == SJS_OK);
    CHECK(sjs_function_native(context, (sjs_native){ &k, &held_vtable }, &natives[1]) == SJS_OK);
    sjs_str native_names[2] = { SJS_STR("keep"), SJS_STR("held") };
    env natives_env = { context, native_names, natives, 2, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    /* The accumulator a native kept is copied, not changed. */
    CHECK(evaluate(&natives_env, "[[\"a\", 1], [\"b\", 2]].reduce((acc, [k, v]) => { const kept = keep(acc); return { ...acc, [k]: v }; }, {})",
                   out) == SJS_OK);
    CHECK(strcmp(out, "{\"a\":1,\"b\":2}") == 0);
    CHECK(k.count == 2 && sjs_length(k.kept[0]) == 0 && sjs_length(k.kept[1]) == 1);
    write_text(context, k.kept[1], out, sizeof out);
    CHECK(strcmp(out, "{\"a\":1}") == 0);
    /* A value a native gives, holding it still, is copied. */
    CHECK(evaluate(&natives_env, "[{ ...held(), b: 2 }, held()]", out) == SJS_OK);
    CHECK(strcmp(out, "[{\"a\":1,\"b\":2},{\"a\":1}]") == 0);
    CHECK(sjs_length(k.held) == 1);
    for (int i = 0; i < k.count; i++) sjs_release(context, k.kept[i]);
    sjs_release(context, k.held);
    sjs_release(context, natives[0]);
    sjs_release(context, natives[1]);

    /* Globals the embedder holds alone, through two runs of one vm. */
    sjs_value globals[2] = { object_of(context, just_a, 1), number_array(context, (const double[]){ 1, 2 }, 2) };
    sjs_str global_names[2] = { SJS_STR("g"), SJS_STR("xs") };
    const char *source = "[{ ...g, b: 2 }, [...xs, 3], xs.concat(4), xs.with(0, 9), xs.toSorted((a, b) => b - a)]";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, global_names, 2, { NULL, NULL } },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, globals, 2 };
    for (int i = 0; i < 2; i++) {
        CHECK(sjs_vm_start(vm, &run) == SJS_OK);
        CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
        sjs_value v;
        CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
        write_text(context, v, out, sizeof out);
        CHECK(strcmp(out, "[{\"a\":1,\"b\":2},[1,2,3],[1,2,4],[9,2],[2,1]]") == 0);
        sjs_release(context, v);
    }
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    CHECK(sjs_length(globals[0]) == 1);
    write_text(context, globals[1], out, sizeof out);
    CHECK(strcmp(out, "[1,2]") == 0);
    sjs_release(context, globals[0]);
    sjs_release(context, globals[1]);

    /* A module's exports, lent while a program compiles: its constants hold
     * them, and the embedder holds the namespace. */
    one_module m;
    env module_env = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_MODULE };
    const char *module = "export const o = {a: 1};\nexport const xs = [1, 2];\n";
    sjs_compile_params module_params = { { context, SJS_GOAL_MODULE, NULL, 0, { NULL, NULL } },
                                         { module, (uint32_t)strlen(module) }, NULL };
    CHECK(sjs_compile(&module_params, &program) == SJS_OK);
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params module_run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &module_run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    CHECK(sjs_vm_take_result(vm, &m.exports) == SJS_OK);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    module_env.modules = (sjs_modules){ &m, &find_vtable };
    CHECK(evaluate(&module_env, "import { o, xs } from \"lib\";\nexport const r = [{ ...o, b: 2 }, [...xs, 3], xs.concat(4)];\n",
                   out) == SJS_OK);
    CHECK(strcmp(out, "{\"r\":[{\"a\":1,\"b\":2},[1,2,3],[1,2,4]]}") == 0);
    CHECK(evaluate(&module_env, "import * as ns from \"lib\";\nexport const r = { ...ns, z: 1 };\n", out) == SJS_OK);
    CHECK(strcmp(out, "{\"r\":{\"o\":{\"a\":1},\"xs\":[1,2],\"z\":1}}") == 0);
    sjs_value o;
    sjs_value xs;
    CHECK(sjs_object_get(m.exports, SJS_STR("o"), &o) == SJS_OK && sjs_length(o) == 1);
    CHECK(sjs_object_get(m.exports, SJS_STR("xs"), &xs) == SJS_OK && sjs_length(xs) == 2);
    CHECK(sjs_length(m.exports) == 2);
    sjs_release(context, m.exports);

    /* The module whose every allocation api_test fails, as Node gives it. */
    module_env.modules = (sjs_modules){ NULL, NULL };
    CHECK(evaluate(&module_env, reuse_module, out) == SJS_OK);
    CHECK(strcmp(out, "{\"f\":<function>,\"g\":{\"list\":[\"c\",\"b\",\"0\",\"a\",\"1\",\"c\",\"2\"],\"b\":0,\"a\":1,\"c\":2},"
                      "\"h\":[\"0\",\"1\",\"2\",\"a\",\"b\",\"c\",\"c\"]}") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
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
    test_unbound_where_it_never_runs();
    test_modules();
    test_embedder_namespaces();
    test_compile_costs();
    test_module_events();
    test_pattern_events();
    test_statement_events();
    test_statement_order();
    test_sink();
    test_writer_resumes();
    test_fuel_and_reset();
    test_limits();
    test_reuse_leaves_others_alone();
    test_out_of_memory("[1, 2, 3].map(x => ({x, s: `${x}`}))", SJS_GOAL_EXPRESSION);
    /* Values updated in place: a moved accumulator, literals that take over
     * a spread, concat, with and toSorted on an array nothing else holds.
     * Small enough for every run. */
    test_out_of_memory(reuse_module, SJS_GOAL_MODULE);
    if (full) {
        test_out_of_memory("export const f = (xs) => xs.toSorted((a, b) => b - a).flatMap(x => [x, [x]]);\n"
                           "export const g = new Set(f([3, 1, 2]).flat()).union(new Set([9]));\n"
                           "export const h = Object.fromEntries(Object.entries({a: 1, b: 2}).map(([k, v]) => [k, `${v}`]));\n",
                           SJS_GOAL_MODULE);
        test_out_of_memory("(({a = [1], ...r}, ...xs) => [a, r, xs])({b: 2}, 3)", SJS_GOAL_EXPRESSION);
        test_out_of_memory("export const [h = 0, ...t] = [undefined, \"\xc3\xa9" "x\", 2];\n"
                           "export const {[`k${1}`]: k = h, ...rest} = {k1: undefined, z: [...t]};\n"
                           "export default ((...[a, b = a]) => [a, b])(...\"ab\");\n",
                           SJS_GOAL_MODULE);
        test_out_of_memory("(x => { if (x > 1) { const {a, ...r} = {a: [x], b: 2}; if (a) return [a, r]; }\n"
                           "  else if (x) return `${x}`;\n"
                           "  { const f = y => { if (y) { const z = [y]; return z; } }; return f(x); } })(2)",
                           SJS_GOAL_EXPRESSION);
        test_out_of_memory("export { a, b as c };\n"
                           "const a = [1, 2].map(x => x * 2);\n"
                           "const b = { a };\n"
                           "export default b.a.length;\n",
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
