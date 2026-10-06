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
    sjs_compile_params params = { { e->context, e->goal, e->globals, e->count, e->modules, false },
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

/* Keys put through the API are interned per context: objects with the same
 * key share one string, which leaves the table when the last object holding
 * it goes, and a key put again after that is made anew. */
static void test_interned_keys(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    enum { KEYS = 300, OBJECTS = 7 };
    sjs_value objects[OBJECTS];
    char name[8];
    for (int o = 0; o < OBJECTS; o++) {
        CHECK(sjs_object_new(context, 0, &objects[o]) == SJS_OK);
        /* Each object holds a different part of the keys, so that releasing
         * them frees the strings in an order the table did not put them in. */
        for (int k = o; k < KEYS; k += o + 1) {
            int n = snprintf(name, sizeof name, "k%d", k);
            sjs_property p = { (sjs_str){ name, (uint32_t)n }, sjs_number(k) };
            CHECK(sjs_object_put(context, &objects[o], &p) == SJS_OK);
        }
    }
    sjs_property a, b;
    CHECK(sjs_object_at(objects[0], 5, &a) == SJS_OK && sjs_object_at(objects[1], 2, &b) == SJS_OK);
    CHECK(a.key.length == 2 && memcmp(a.key.ptr, "k5", 2) == 0 && a.key.ptr == b.key.ptr);
    int order[OBJECTS] = { 3, 0, 5, 1, 6, 2, 4 };
    for (int i = 0; i < OBJECTS; i++) {
        sjs_release(context, objects[order[i]]);
        objects[order[i]] = sjs_undefined();
        /* What the others hold is still found, and still shared. */
        for (int o = 0; o < OBJECTS; o++) {
            if (objects[o].kind != SJS_OBJECT) continue;
            for (int k = o; k < KEYS; k += o + 1) {
                int n = snprintf(name, sizeof name, "k%d", k);
                sjs_value got;
                CHECK(sjs_object_get(objects[o], (sjs_str){ name, (uint32_t)n }, &got) == SJS_OK && got.as.number == k);
            }
        }
        sjs_value probe;
        CHECK(sjs_object_new(context, 0, &probe) == SJS_OK);
        sjs_property p = { SJS_STR("k0"), sjs_null() };
        CHECK(sjs_object_put(context, &probe, &p) == SJS_OK);
        CHECK(sjs_object_at(probe, 0, &a) == SJS_OK);
        if (objects[0].kind == SJS_OBJECT) CHECK(sjs_object_at(objects[0], 0, &b) == SJS_OK && a.key.ptr == b.key.ptr);
        sjs_release(context, probe);
    }
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
    sjs_compile_params params = { { context, SJS_GOAL_MODULE, NULL, 0, { NULL, NULL }, false },
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
    sjs_compiler_config config = { context, SJS_GOAL_MODULE, NULL, 0, { &m, &find_vtable }, false };
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
    int nodes;   /* LOG_*: what log_event writes of each event */
} event_log;

/* Each event's kind, count and texts; or its kind, node and start, as
 * `KIND#node@offset` (LOG_NODES) or `KIND#node@line.column` (LOG_LINES). */
enum { LOG_TEXT = 0, LOG_NODES, LOG_LINES };

static const char *const event_names[SJS_EV_END + 1] = {
    [SJS_EV_NUMBER] = "NUMBER", [SJS_EV_STRING] = "STRING", [SJS_EV_TRUE] = "TRUE", [SJS_EV_FALSE] = "FALSE",
    [SJS_EV_NULL] = "NULL", [SJS_EV_NAME] = "NAME", [SJS_EV_TEMPLATE_BEGIN] = "TEMPLATE_BEGIN",
    [SJS_EV_TEMPLATE_END] = "TEMPLATE_END", [SJS_EV_ARRAY_BEGIN] = "ARRAY_BEGIN", [SJS_EV_ELEMENT] = "ELEMENT",
    [SJS_EV_SPREAD_ELEMENT] = "SPREAD_ELEMENT", [SJS_EV_ARRAY_END] = "ARRAY_END", [SJS_EV_OBJECT_BEGIN] = "OBJECT_BEGIN",
    [SJS_EV_KEY] = "KEY", [SJS_EV_COMPUTED_KEY] = "COMPUTED_KEY", [SJS_EV_PROPERTY] = "PROPERTY",
    [SJS_EV_SPREAD_PROPERTY] = "SPREAD_PROPERTY", [SJS_EV_OBJECT_END] = "OBJECT_END", [SJS_EV_MEMBER] = "MEMBER",
    [SJS_EV_INDEX] = "INDEX", [SJS_EV_OPTIONAL] = "OPTIONAL", [SJS_EV_CHAIN_END] = "CHAIN_END",
    [SJS_EV_CALL_BEGIN] = "CALL_BEGIN", [SJS_EV_ARGUMENT] = "ARGUMENT", [SJS_EV_SPREAD_ARGUMENT] = "SPREAD_ARGUMENT",
    [SJS_EV_CALL_END] = "CALL_END", [SJS_EV_NEW_BEGIN] = "NEW_BEGIN", [SJS_EV_NEW_END] = "NEW_END",
    [SJS_EV_UNARY] = "UNARY", [SJS_EV_BINARY] = "BINARY", [SJS_EV_LOGICAL] = "LOGICAL",
    [SJS_EV_LOGICAL_END] = "LOGICAL_END", [SJS_EV_CONDITIONAL] = "CONDITIONAL",
    [SJS_EV_CONDITIONAL_ELSE] = "CONDITIONAL_ELSE", [SJS_EV_CONDITIONAL_END] = "CONDITIONAL_END",
    [SJS_EV_ARROW_BEGIN] = "ARROW_BEGIN", [SJS_EV_PARAM] = "PARAM", [SJS_EV_REST_PARAM] = "REST_PARAM",
    [SJS_EV_PARAMS_END] = "PARAMS_END", [SJS_EV_ARROW_END] = "ARROW_END", [SJS_EV_BLOCK_BEGIN] = "BLOCK_BEGIN",
    [SJS_EV_RETURN] = "RETURN", [SJS_EV_BLOCK_END] = "BLOCK_END", [SJS_EV_BLOCK_STATEMENT_BEGIN] = "BLOCK_STATEMENT_BEGIN",
    [SJS_EV_BLOCK_STATEMENT_END] = "BLOCK_STATEMENT_END", [SJS_EV_IF] = "IF", [SJS_EV_ELSE] = "ELSE",
    [SJS_EV_IF_END] = "IF_END", [SJS_EV_BIND] = "BIND", [SJS_EV_ARRAY_PATTERN_BEGIN] = "ARRAY_PATTERN_BEGIN",
    [SJS_EV_PATTERN_INDEX] = "PATTERN_INDEX", [SJS_EV_ARRAY_PATTERN_END] = "ARRAY_PATTERN_END",
    [SJS_EV_OBJECT_PATTERN_BEGIN] = "OBJECT_PATTERN_BEGIN", [SJS_EV_PATTERN_KEY] = "PATTERN_KEY",
    [SJS_EV_PATTERN_COMPUTED_KEY] = "PATTERN_COMPUTED_KEY", [SJS_EV_PATTERN_REST] = "PATTERN_REST",
    [SJS_EV_OBJECT_PATTERN_END] = "OBJECT_PATTERN_END", [SJS_EV_DEFAULT] = "DEFAULT", [SJS_EV_DEFAULT_END] = "DEFAULT_END",
    [SJS_EV_CONST_BEGIN] = "CONST_BEGIN", [SJS_EV_CONST_END] = "CONST_END", [SJS_EV_EXPORT] = "EXPORT",
    [SJS_EV_IMPORT] = "IMPORT", [SJS_EV_EXPORT_DEFAULT] = "EXPORT_DEFAULT", [SJS_EV_EXPORT_NAME] = "EXPORT_NAME",
    [SJS_EV_EXPORT_FROM] = "EXPORT_FROM", [SJS_EV_END] = "END",
};

static sjs_result log_event(void *self, const sjs_event *event) {
    event_log *log = self;
    size_t used = strlen(log->text);
    if (log->nodes == LOG_NODES) {
        snprintf(log->text + used, sizeof log->text - used, "%s%s#%u@%u", used > 0 ? " " : "", event_names[event->kind],
                 event->node, event->start.offset);
    } else if (log->nodes == LOG_LINES) {
        snprintf(log->text + used, sizeof log->text - used, "%s%s#%u@%u.%u", used > 0 ? " " : "",
                 event_names[event->kind], event->node, event->start.line, event->start.column);
    } else {
        snprintf(log->text + used, sizeof log->text - used, "%d:%u:%.*s:%.*s:%.*s\n", (int)event->kind, event->count,
                 (int)event->text.length, event->text.ptr, (int)event->alias.length, event->alias.ptr,
                 (int)event->name.length, event->name.ptr);
    }
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
    sjs_compiler_config config = { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false };
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

/* `source`'s events as `KIND#node@start` (LOG_NODES or LOG_LINES) are `want`. */
static void expect_nodes(const char *source, sjs_goal goal, int nodes, const char *want) {
    static event_log log;
    log.nodes = nodes;
    parse_events(source, goal, &log);
    log.nodes = LOG_TEXT;
    if (strcmp(log.text, want) != 0) fprintf(stderr, "%s\n  events: %s\n  wanted: %s\n", source, log.text, want);
    CHECK(strcmp(log.text, want) == 0);
}

/* Each event carries its node's id and start (docs/matcher.md, section 1):
 * one id for every event of a bracket, an operator's markers and its last
 * event; a fresh one for every other node; the start of an operator, a
 * member, an index or a call at its first operand's. */
static void test_event_nodes(void) {
    /* Postfix nodes start at their first operand. */
    expect_nodes("a.b + c(d)[e]", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "NAME#1@0 MEMBER#2@0 NAME#4@6 CALL_BEGIN#5@6 NAME#6@8 ARGUMENT#5@6 CALL_END#5@6 NAME#8@11 INDEX#7@6 "
                 "BINARY#3@0 END#0@0");
    /* Parentheses are not part of an operand. */
    expect_nodes("(a + b) * c", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "NAME#1@1 NAME#3@5 BINARY#2@1 NAME#5@10 BINARY#4@1 END#0@0");
    expect_nodes("1 +\n  f(x)", SJS_GOAL_EXPRESSION, LOG_LINES,
                 "NUMBER#1@1.1 NAME#3@2.3 CALL_BEGIN#4@2.3 NAME#5@2.5 ARGUMENT#4@2.3 CALL_END#4@2.3 BINARY#2@1.1 END#0@1.1");
    /* An arrow's id across its parameters and body. */
    expect_nodes("x => x.k", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 NAME#3@5 MEMBER#4@5 ARROW_END#1@0 END#0@0");
    /* ... and an arrow in a parameter's default keeps its own. A target is
     * read before its default value and sent after it. */
    expect_nodes("(a, b = c => 1) => { return a; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@1 PARAM#1@0 DEFAULT#4@6 ARROW_BEGIN#5@8 PARAM#5@8 BIND#6@8 "
                 "PARAMS_END#5@8 NUMBER#7@13 ARROW_END#5@8 DEFAULT_END#4@6 BIND#3@4 PARAMS_END#1@0 BLOCK_BEGIN#1@0 "
                 "NAME#9@28 RETURN#8@21 BLOCK_END#1@0 ARROW_END#1@0 END#0@0");
    expect_nodes("(...r) => r", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 REST_PARAM#1@0 BIND#2@4 PARAMS_END#1@0 NAME#3@10 ARROW_END#1@0 END#0@0");
    /* A template's pieces are its own events; a string in it is a node. */
    expect_nodes("`s${\"x\"}t`", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "TEMPLATE_BEGIN#1@0 STRING#1@0 STRING#2@4 STRING#1@0 TEMPLATE_END#1@0 END#0@0");
    /* Logical and conditional markers, an optional chain, a unary operator. */
    expect_nodes("a && b ? c?.d.e : -f", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "NAME#1@0 LOGICAL#2@0 NAME#3@5 LOGICAL_END#2@0 CONDITIONAL#4@0 NAME#5@9 OPTIONAL#6@9 MEMBER#7@9 "
                 "MEMBER#8@9 CHAIN_END#9@9 CONDITIONAL_ELSE#4@0 NAME#11@19 UNARY#10@18 CONDITIONAL_END#4@0 END#0@0");
    /* An array, an object and new. */
    expect_nodes("{k: [1, ...a], [b]: c, 2: d, e, ...f}", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "OBJECT_BEGIN#1@0 KEY#1@0 ARRAY_BEGIN#2@4 NUMBER#3@5 ELEMENT#2@4 NAME#4@11 SPREAD_ELEMENT#2@4 "
                 "ARRAY_END#2@4 PROPERTY#1@0 NAME#5@16 COMPUTED_KEY#1@0 NAME#6@20 PROPERTY#1@0 NUMBER#7@23 "
                 "COMPUTED_KEY#1@0 NAME#8@26 PROPERTY#1@0 KEY#1@0 NAME#9@29 PROPERTY#1@0 NAME#10@35 "
                 "SPREAD_PROPERTY#1@0 OBJECT_END#1@0 END#0@0");
    expect_nodes("new Set([x])", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "NEW_BEGIN#1@0 ARRAY_BEGIN#2@8 NAME#3@9 ELEMENT#2@8 ARRAY_END#2@8 ARGUMENT#1@0 NEW_END#1@0 END#0@0");
    /* One id for every IF, ELSE and IF_END of an else-if chain. */
    expect_nodes("x => { if (x) return 1; else if (!x) return 2; else return 3; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 NAME#4@11 IF#3@7 NUMBER#6@21 "
                 "RETURN#5@14 ELSE#3@7 NAME#8@34 UNARY#7@33 IF#3@7 NUMBER#10@44 RETURN#9@37 ELSE#3@7 NUMBER#12@59 "
                 "RETURN#11@52 IF_END#3@7 BLOCK_END#1@0 ARROW_END#1@0 END#0@0");
    expect_nodes("x => { { const y = x; } return; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 BLOCK_STATEMENT_BEGIN#3@7 "
                 "CONST_BEGIN#4@9 NAME#6@19 BIND#5@15 CONST_END#4@9 BLOCK_STATEMENT_END#3@7 RETURN#7@24 BLOCK_END#1@0 "
                 "ARROW_END#1@0 END#0@0");
    /* Patterns; held and rotated events keep the ids they were made with. */
    expect_nodes("({k: a, [b]: c, ...d}) => 0", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 OBJECT_PATTERN_BEGIN#2@1 PATTERN_KEY#2@1 BIND#3@5 NAME#4@9 "
                 "PATTERN_COMPUTED_KEY#2@1 BIND#5@13 PATTERN_REST#2@1 BIND#6@19 OBJECT_PATTERN_END#2@1 PARAMS_END#1@0 "
                 "NUMBER#7@26 ARROW_END#1@0 END#0@0");
    expect_nodes("const [a = b] = c", SJS_GOAL_MODULE, LOG_NODES,
                 "CONST_BEGIN#1@0 NAME#6@16 ARRAY_PATTERN_BEGIN#2@6 PATTERN_INDEX#2@6 DEFAULT#4@9 NAME#5@11 "
                 "DEFAULT_END#4@9 BIND#3@7 ARRAY_PATTERN_END#2@6 CONST_END#1@0 END#0@0");
    /* Imports and exports: one node per specifier, from the statement's start. */
    expect_nodes("import a, {b as c, d} from \"m\";", SJS_GOAL_MODULE, LOG_NODES,
                 "IMPORT#1@0 IMPORT#2@0 IMPORT#3@0 END#0@0");
    expect_nodes("export * from \"m\"; export const f = 1, g = 2; export default 3;", SJS_GOAL_MODULE, LOG_NODES,
                 "EXPORT_FROM#1@0 EXPORT#2@19 CONST_BEGIN#3@26 NUMBER#5@36 BIND#4@32 CONST_END#3@26 EXPORT#6@19 "
                 "CONST_BEGIN#7@26 NUMBER#9@43 BIND#8@39 CONST_END#7@26 NUMBER#11@61 EXPORT_DEFAULT#10@46 END#0@0");
    expect_nodes("export { e as f }; const e = 1;", SJS_GOAL_MODULE, LOG_NODES,
                 "EXPORT_NAME#1@0 CONST_BEGIN#2@19 NUMBER#4@29 BIND#3@25 CONST_END#2@19 END#0@0");
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
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
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

/* The fuel an evaluation costs is part of the API: an embedder's pause
 * points depend on it. This one calls closures, a looping built-in, a
 * built-in that does not call back, and concatenates; its cost is pinned, so
 * a change to how the evaluator dispatches cannot move it. */
static void test_fuel_pinned(sjs_context *context) {
    const char *source = "[1, 2, 3, 4].map(x => [x, x * 2]).map(([a, b]) => `${a}:${b}`)"
                         ".reduce((s, t) => s + t + Math.max(t.length, 3), '') + ((f) => f(f(2)))(y => y * 3)";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    CHECK(sjs_vm_fuel_used(vm) == 194);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK && v.kind == SJS_STRING);
    sjs_release(context, v);
    /* Paused after every unit, it pauses as often and costs the same. */
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    sjs_run status;
    uint64_t pauses = 0;
    while ((status = sjs_vm_run(vm, 1)) == SJS_RUN_PAUSED) pauses++;
    CHECK(status == SJS_RUN_DONE && pauses == 185 && sjs_vm_fuel_used(vm) == 194);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
}

/* A set's lookups go through its index, so combining two sets costs fuel in
 * proportion to their sizes, not their product. */
static void test_set_fuel_linear(sjs_context *context) {
    static const sjs_str names[2] = { { "a", 1 }, { "b", 1 } };
    const char *source = "[a.intersection(b).size, a.union(b).size, a.difference(b).size, a.has(1999)]";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, names, 2, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_value sets[2];
    for (uint32_t s = 0; s < 2; s++) {
        CHECK(sjs_set_new(context, 2000, &sets[s]) == SJS_OK);
        for (uint32_t i = 0; i < 2000; i++) CHECK(sjs_set_add(context, &sets[s], sjs_number(i + s * 1000.0)) == SJS_OK);
    }
    sjs_vm *vm;
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, sets, 2 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    /* Three combinations of 2,000 by 2,000 members: 4,000 lookups each. */
    CHECK(sjs_vm_fuel_used(vm) < 13000);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    char out[64];
    write_text(context, v, out, sizeof out);
    CHECK(strcmp(out, "[1000,3000,1000,true]") == 0);
    sjs_release(context, v);
    sjs_vm_destroy(vm);
    sjs_release(context, sets[0]);
    sjs_release(context, sets[1]);
    sjs_program_release(program);
}

static void test_fuel_and_reset(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    const char *source = "[1, 2, 3, 4, 5, 6, 7, 8].map(x => x * x).filter(x => x % 2 === 0).reduce((a, b) => a + b, 0)";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
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
    test_fuel_pinned(context);
    test_set_fuel_linear(context);
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

/* A program with a vm of its own, for the loop tests: compiled once,
 * started as often as a test needs. */
typedef struct {
    sjs_context *context;
    sjs_program *program;
    sjs_vm *vm;
} loop_run;

static bool loop_open(loop_run *l, sjs_context *context, const char *source) {
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    l->context = context;
    if (sjs_compile(&params, &l->program) != SJS_OK) return false;
    sjs_vm_config config = { context };
    return sjs_vm_init(&l->vm, &config) == SJS_OK;
}

static void loop_close(loop_run *l) {
    sjs_vm_destroy(l->vm);
    sjs_program_release(l->program);
}

/* Run to the end, `per_run` units at a time; the result's text or the
 * fault's position as "fault LINE:COLUMN message". */
static sjs_run loop_finish(loop_run *l, uint64_t per_run, char *out) {
    sjs_run_params run = { l->program, NULL, 0 };
    CHECK(sjs_vm_start(l->vm, &run) == SJS_OK);
    sjs_run status;
    while ((status = sjs_vm_run(l->vm, per_run)) == SJS_RUN_PAUSED) continue;
    if (status == SJS_RUN_DONE) {
        sjs_value v;
        CHECK(sjs_vm_take_result(l->vm, &v) == SJS_OK);
        write_text(l->context, v, out, 4096);
        sjs_release(l->context, v);
    } else {
        sjs_error error;
        CHECK(sjs_vm_error(l->vm, &error) == SJS_OK);
        snprintf(out, 256, "fault %u:%u %.*s", error.at.line, error.at.column, (int)error.message.length, error.message.ptr);
    }
    return status;
}

/* A loop's callback runs in one frame kept for the whole loop (vm.c,
 * sjs_vm_call_back). Faults in it report where they were, the limits fall
 * where they did, an evaluation stopped at any unit frees everything, and
 * reduce's accumulator keeps one holder. */
static void test_loop_callbacks(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    char again[4096];
    loop_run l;

    /* A fault on the third element, at the instruction that faulted, whole
     * and one unit at a time. */
    CHECK(loop_open(&l, context, "[1, 2, 3, 4].map(x => {\n  const y = x * 2;\n  return x === 3 ? y.k.j : y;\n})"));
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_FAULT);
    CHECK(strncmp(out, "fault 3:22 ", 11) == 0);
    CHECK(loop_finish(&l, 1, out) == SJS_RUN_FAULT);
    CHECK(strncmp(out, "fault 3:22 ", 11) == 0);
    loop_close(&l);

    /* Captures, a rest parameter, nested loops, reduce with an object
     * accumulator and toSorted with a default parameter. Stopped and reset
     * after any unit, it frees all it took; paused after every unit, it
     * gives the same value for the same fuel. */
    CHECK(loop_open(&l, context,
                    "[3, 1, 2].toSorted((a, b, d = a - b) => d)\n"
                    "  .map((x, ...r) => [x, r.length, [x, x + 1].map(y => () => y + x)])\n"
                    "  .reduce((acc, [x, n, fs]) => ({ ...acc, [`k${x}`]: fs.map(f => f()).concat([n]) }), {})"));
    size_t baseline = sjs_context_live_bytes(context);
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_DONE);
    CHECK(strcmp(out, "{\"k1\":[2,3,2],\"k2\":[4,5,2],\"k3\":[6,7,2]}") == 0);
    uint64_t total = sjs_vm_fuel_used(l.vm);
    CHECK(loop_finish(&l, 1, again) == SJS_RUN_DONE);
    CHECK(strcmp(again, out) == 0 && sjs_vm_fuel_used(l.vm) == total);
    sjs_run_params run = { l.program, NULL, 0 };
    for (uint64_t stop = 1; stop < total; stop++) {
        CHECK(sjs_vm_start(l.vm, &run) == SJS_OK);
        CHECK(sjs_vm_run(l.vm, stop) == SJS_RUN_PAUSED);
        sjs_vm_reset(l.vm);
        CHECK(sjs_context_live_bytes(context) == baseline);
    }
    loop_close(&l);

    /* The accumulator moves into the callback and back, so [...acc, y]
     * grows it in place: 77 allocations, where copying it for each of the
     * 64 elements takes more than 250. */
    char source[1024];
    char *at = source;
    at += sprintf(at, "[");
    for (int i = 1; i <= 64; i++) at += sprintf(at, i == 1 ? "%d" : ", %d", i);
    sprintf(at, "].reduce((acc, x) => { const y = x; return [...acc, y]; }, []).length");
    CHECK(loop_open(&l, context, source));
    long before = c.allocations;
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "64") == 0);
    CHECK(c.allocations - before <= 90);
    loop_close(&l);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* max_frames: a loop and its callback take a frame each, once per loop. */
    sjs_limits limits = SJS_LIMITS_DEFAULT;
    limits.max_frames = 3;
    context = new_context(&c, limits);
    CHECK(loop_open(&l, context, "[1, 2].map(x => x)"));
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "[1,2]") == 0);
    loop_close(&l);
    CHECK(loop_open(&l, context, "[1].map(x =>\n [2].map(y => y))"));
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_FAULT);
    CHECK(strcmp(out, "fault 2:9 the evaluation needs more than max_frames frames") == 0);
    loop_close(&l);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* max_stack: this callback, with its six slots, needed 14 when it was
     * called with its callee and arguments pushed; in its kept frame it
     * needs 13, and faults with 12. */
    for (uint32_t max_stack = 12; max_stack <= 13; max_stack++) {
        limits = SJS_LIMITS_DEFAULT;
        limits.max_stack = max_stack;
        context = new_context(&c, limits);
        CHECK(loop_open(&l, context, "[1, 2].map((x, i) => { const a = x; const b = a + i; const c = b; const d = c; return [d, a]; })"));
        sjs_run status = loop_finish(&l, UINT64_MAX, out);
        if (max_stack == 13) CHECK(status == SJS_RUN_DONE && strcmp(out, "[[1,1],[3,2]]") == 0);
        else CHECK(status == SJS_RUN_FAULT && strstr(out, "max_stack") != NULL);
        loop_close(&l);
        CHECK(sjs_context_live_bytes(context) == 0);
        sjs_context_destroy(context);
    }
    CHECK(c.live == 0);
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
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, global_names, 2, { NULL, NULL }, false },
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
    sjs_compile_params module_params = { { context, SJS_GOAL_MODULE, NULL, 0, { NULL, NULL }, false },
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

/* The off switch (docs/matcher.md, section 8): with no_shapes the compiler
 * makes no matcher, so it does not allocate the matcher's records, and the
 * program gives the same value. A config that leaves the field out
 * matches. */
typedef struct {
    long allocations;
    char out[256];
} compiled;

static compiled compile_counting(sjs_compiler_config config, const char *source) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    config.context = context;
    compiled result = { 0, "" };
    long before = c.allocations;
    sjs_program *program;
    sjs_compile_params params = { config, { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    result.allocations = c.allocations - before;
    sjs_program_release(program);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, source, result.out) == SJS_OK);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
    return result;
}

static void test_no_shapes(void) {
    const char *source = "[{k: 2}, {k: 1}].toSorted((a, b) => a.k - b.k).map(x => x.k)";
    sjs_compiler_config on = { NULL, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false };
    sjs_compiler_config off = on;
    off.no_shapes = true;
    sjs_compiler_config unsaid = { .goal = SJS_GOAL_EXPRESSION };
    compiled with = compile_counting(on, source);
    compiled without = compile_counting(off, source);
    compiled by_default = compile_counting(unsaid, source);
    CHECK(strcmp(with.out, "[1,2]") == 0 && strcmp(without.out, with.out) == 0);
    /* The matcher's records: two allocations, as the comparator's marker
     * and its two keys outgrow the first (src/matcher.c,
     * MATCHER_FIRST_RECORDS). Its text fits in itself. */
    CHECK(with.allocations == without.allocations + 2);
    CHECK(by_default.allocations == with.allocations);
}

/* The shapes' C paths (docs/matcher.md, sections 7 and 11): toSorted with
 * a COMPARE_BY_KEY or a COMPARE_NUMERIC comparator, map with a PLUCK
 * callback, the predicate loops with a KEY_COMPARE or an ELEMENT_COMPARE
 * callback, and reduce with a SUM or a SUM_KEY callback. A run of one
 * source, with shapes or without, whole and then one unit of fuel at a time:
 * its value or its fault ("fault LINE:COLUMN message"), the fuel it used and
 * how often it paused. */
typedef struct {
    char out[256];
    uint64_t fuel;
    uint64_t pauses;
} shaped_run;

typedef struct {
    const char *source;
    bool no_shapes;
    sjs_limits limits;
} shaped_source;

static shaped_run run_shaped(shaped_source s) {
    shaped_run result = { "", 0, 0 };
    counting c = { 0 };
    sjs_context *context = new_context(&c, s.limits);
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, s.no_shapes },
                                  { s.source, (uint32_t)strlen(s.source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    loop_run l = { context, program, NULL };
    sjs_vm_config config = { context };
    CHECK(sjs_vm_init(&l.vm, &config) == SJS_OK);
    loop_finish(&l, UINT64_MAX, result.out);
    result.fuel = sjs_vm_fuel_used(l.vm);
    /* Paused after every unit: the same answer for the same fuel. */
    char again[4096];
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(l.vm, &run) == SJS_OK);
    sjs_run status;
    while ((status = sjs_vm_run(l.vm, 1)) == SJS_RUN_PAUSED) result.pauses++;
    if (status == SJS_RUN_DONE) {
        sjs_value v;
        CHECK(sjs_vm_take_result(l.vm, &v) == SJS_OK);
        write_text(context, v, again, sizeof again);
        sjs_release(context, v);
        CHECK(strcmp(again, result.out) == 0);
    }
    CHECK(sjs_vm_fuel_used(l.vm) == result.fuel);
    loop_close(&l);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
    return result;
}

/* The same source with shapes and without: the same value or fault. */
static void expect_same_both_ways(const char *source, sjs_limits limits, const char *want) {
    shaped_run on = run_shaped((shaped_source){ source, false, limits });
    shaped_run off = run_shaped((shaped_source){ source, true, limits });
    if (strcmp(on.out, off.out) != 0 || strcmp(on.out, want) != 0 || on.fuel > off.fuel) {
        fprintf(stderr, "%s\n  with shapes:    %s (fuel %llu)\n  without shapes: %s (fuel %llu)\n  expected:       %s\n",
                source, on.out, (unsigned long long)on.fuel, off.out, (unsigned long long)off.fuel, want);
        failures++;
    }
}

static void test_shape_paths(void) {
    /* Fuel, pinned. Five comparisons: 7 units each in bytecode (the step,
     * TAKE_LOCAL, GET_MEMBER, TAKE_LOCAL, GET_MEMBER, BINARY, RETURN), 3 in
     * C (the step and two lookups). */
    const char *sort = "[{k: 3}, {k: 1}, {k: 2}, {k: 0}].toSorted((a, b) => a.k - b.k)";
    shaped_run on = run_shaped((shaped_source){ sort, false, SJS_LIMITS_DEFAULT });
    shaped_run off = run_shaped((shaped_source){ sort, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[{\"k\":0},{\"k\":1},{\"k\":2},{\"k\":3}]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 54 && on.fuel == 34 && off.fuel - on.fuel == 5 * (7 - 3));
    /* A C step charges its lookups with its unit, as a built-in charges its
     * cost: on one unit per run, each comparison in C is one pause. */
    CHECK(off.pauses == 53 && on.pauses == 53 - 5 * 6);
    /* Four elements: 4 units each in bytecode (the step, TAKE_LOCAL,
     * GET_MEMBER, RETURN), 2 in C. */
    const char *map = "[{k: 3}, {k: 1}, {}, {k: 0}].map(x => x.k)";
    on = run_shaped((shaped_source){ map, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ map, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[3,1,undefined,0]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 34 && on.fuel == 26 && off.fuel - on.fuel == 4 * (4 - 2));
    CHECK(off.pauses == 33 && on.pauses == 33 - 4 * 3);
    /* groupBy by a pluck, like map: 4 units each in bytecode, 1 in C. */
    const char *group = "Object.groupBy([{k: 3}, {k: 1}, {}, {k: 3}], x => x.k)";
    on = run_shaped((shaped_source){ group, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ group, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, off.out) == 0 && off.fuel > on.fuel);
    /* Five comparisons of numbers: 5 units each in bytecode (the step,
     * TAKE_LOCAL, TAKE_LOCAL, BINARY, RETURN), 1 in C (the step). */
    const char *numbers = "[3, 1, 2, 0].toSorted((a, b) => a - b)";
    on = run_shaped((shaped_source){ numbers, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ numbers, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[0,1,2,3]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 40 && on.fuel == 20 && off.fuel - on.fuel == 5 * (5 - 1));
    CHECK(off.pauses == 39 && on.pauses == 39 - 5 * 4);
    /* Four elements of a filter: 6 units each in bytecode (the step,
     * TAKE_LOCAL, GET_MEMBER, NUMBER_, BINARY, RETURN), 2 in C (the step
     * and a lookup), which is one pause. */
    const char *filter = "[{k: 3}, {k: 1}, {k: 0}, {k: 4}].filter(x => x.k > 2)";
    on = run_shaped((shaped_source){ filter, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ filter, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[{\"k\":3},{\"k\":4}]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 43 && on.fuel == 27 && off.fuel - on.fuel == 4 * (6 - 2));
    CHECK(off.pauses == 42 && on.pauses == 42 - 4 * 5);
    /* some, answered at its third element: 5 units each in bytecode (the
     * step, TAKE_LOCAL, NUMBER_, BINARY, RETURN), 1 in C; and the bytecode
     * reads the last answer in a step of its own, where C finishes in the
     * step that decides. */
    const char *some = "[1, 2, 3, 4].some(x => x === 3)";
    on = run_shaped((shaped_source){ some, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ some, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "true") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 30 && on.fuel == 17 && off.fuel - on.fuel == 3 * (5 - 1) + 1);
    CHECK(off.pauses == 29 && on.pauses == 29 - 3 * 4 - 1);
    /* Four elements of a sum: 5 units each in bytecode (the step,
     * TAKE_LOCAL, TAKE_LOCAL, BINARY, RETURN), 1 in C. */
    const char *sum = "[1, 2, 3, 4].reduce((a, x) => a + x, 0.5)";
    on = run_shaped((shaped_source){ sum, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ sum, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "10.5") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 36 && on.fuel == 20 && off.fuel - on.fuel == 4 * (5 - 1));
    CHECK(off.pauses == 35 && on.pauses == 35 - 4 * 4);
    /* And of a key's sum: 6 units each in bytecode (the step, TAKE_LOCAL,
     * TAKE_LOCAL, GET_MEMBER, BINARY, RETURN), 2 in C (the step and a
     * lookup), which is one pause. */
    const char *sum_key = "[{k: 1}, {k: 2}, {k: 3}, {k: 4}].reduce((a, x) => x.k + a, 0)";
    on = run_shaped((shaped_source){ sum_key, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ sum_key, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "10") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 44 && on.fuel == 28 && off.fuel - on.fuel == 4 * (6 - 2));
    CHECK(off.pauses == 43 && on.pauses == 43 - 4 * 5);
    /* A fault after C steps: where the bytecode faults, for less fuel. */
    expect_same_both_ways("[{k: 2}, {k: 1}, {k: 4},\n {j: 3}].toSorted((a, b) =>\n a.k - b.k)", SJS_LIMITS_DEFAULT,
                          "fault 3:6 arithmetic in staticjs is on numbers");
    expect_same_both_ways("[{k: 2}, {k: 1}, null].map(x =>\n x.k)", SJS_LIMITS_DEFAULT,
                          "fault 2:4 cannot read a property of null");
    expect_same_both_ways("[4, 3, 2, 1,\n \"0\"].toSorted((a, b) =>\n a - b)", SJS_LIMITS_DEFAULT,
                          "fault 3:4 arithmetic in staticjs is on numbers");
    expect_same_both_ways("[{k: 2}, {k: 1}, null].filter(x =>\n x.k > 0)", SJS_LIMITS_DEFAULT,
                          "fault 2:4 cannot read a property of null");
    expect_same_both_ways("[{k: 2}, {k: 1}, {k: \"a\"}].every(x =>\n 0 < x.k)", SJS_LIMITS_DEFAULT,
                          "fault 2:4 staticjs compares two numbers or two strings");
    expect_same_both_ways("[{}, {toString: 2}].findLast(x =>\n x.toString === 1)", SJS_LIMITS_DEFAULT,
                          "fault 2:4 staticjs objects have no prototype");
    expect_same_both_ways("[1, 2, 3,\n true].reduce((a, x) =>\n a + x)", SJS_LIMITS_DEFAULT,
                          "fault 3:4 staticjs adds two numbers, or a string and a primitive");
    expect_same_both_ways("[{k: 1}, {k: 2},\n {}].reduce((a, x) =>\n x.k + a, 0)", SJS_LIMITS_DEFAULT,
                          "fault 3:6 staticjs adds two numbers, or a string and a primitive");
    expect_same_both_ways("[{k: 1}, {k: 2},\n {}].reduce((a, x) =>\n a + x.toString, 0)", SJS_LIMITS_DEFAULT,
                          "fault 3:8 staticjs objects have no prototype");

    /* max_stack: a C step succeeds only where the bytecode has room for
     * what it pushes. The comparator's frame sits on the loop's five slots
     * and has two of its own: a.k is the eighth value and b.k the ninth.
     * With 6 the frame cannot be entered, with 7 a.k does not fit, with 8
     * b.k does not, and each faults there on both paths. */
    static const char *const sort_stack[] = {
        "fault 1:26 the evaluation needs more than max_stack values",
        "fault 1:37 the evaluation needs more than max_stack values",
        "fault 1:43 the evaluation needs more than max_stack values",
        "[{\"k\":1},{\"k\":2}]",
        "[{\"k\":1},{\"k\":2}]",
    };
    sjs_limits limits = SJS_LIMITS_DEFAULT;
    for (uint32_t max_stack = 6; max_stack <= 10; max_stack++) {
        limits.max_stack = max_stack;
        expect_same_both_ways("[{k: 2}, {k: 1}].toSorted((a, b) => a.k - b.k)", limits, sort_stack[max_stack - 6]);
    }
    /* A numeric comparator pushes a and b: the eighth and the ninth value
     * again. */
    static const char *const numbers_stack[] = {
        "fault 1:16 the evaluation needs more than max_stack values",
        "fault 1:27 the evaluation needs more than max_stack values",
        "fault 1:31 the evaluation needs more than max_stack values",
        "[1,2]",
        "[1,2]",
    };
    for (uint32_t max_stack = 6; max_stack <= 10; max_stack++) {
        limits.max_stack = max_stack;
        expect_same_both_ways("[2, 1].toSorted((a, b) => a - b)", limits, numbers_stack[max_stack - 6]);
    }
    /* A pluck's frame has one slot, and x.k is the seventh value. */
    static const char *const map_stack[] = {
        "fault 1:21 the evaluation needs more than max_stack values",
        "fault 1:27 the evaluation needs more than max_stack values",
        "[2,1]",
        "[2,1]",
    };
    for (uint32_t max_stack = 5; max_stack <= 8; max_stack++) {
        limits.max_stack = max_stack;
        expect_same_both_ways("[{k: 2}, {k: 1}].map(x => x.k)", limits, map_stack[max_stack - 5]);
    }
    /* A predicate's frame has one slot; it pushes x.k (or x) and the
     * literal, in either order: the seventh and the eighth value. With 5
     * the frame cannot be entered, with 6 the first push does not fit, with
     * 7 the second does not. */
    static const struct {
        const char *source;
        const char *outcomes[5];
    } predicate_stack[] = {
        { "[{k: 2}, {k: 1}].filter(x => x.k === 1)",
          { "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:30 the evaluation needs more than max_stack values",
            "fault 1:38 the evaluation needs more than max_stack values", "[{\"k\":1}]", "[{\"k\":1}]" } },
        { "[{k: 2}, {k: 1}].filter(x => 1 === x.k)",
          { "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:30 the evaluation needs more than max_stack values",
            "fault 1:36 the evaluation needs more than max_stack values", "[{\"k\":1}]", "[{\"k\":1}]" } },
        { "[2, 1].findIndex(x => x < 2)",
          { "fault 1:17 the evaluation needs more than max_stack values",
            "fault 1:23 the evaluation needs more than max_stack values",
            "fault 1:27 the evaluation needs more than max_stack values", "1", "1" } },
        { "[2, 1].findIndex(x => 2 > x)",
          { "fault 1:17 the evaluation needs more than max_stack values",
            "fault 1:23 the evaluation needs more than max_stack values",
            "fault 1:27 the evaluation needs more than max_stack values", "1", "1" } },
    };
    for (uint32_t i = 0; i < sizeof predicate_stack / sizeof predicate_stack[0]; i++) {
        for (uint32_t max_stack = 5; max_stack <= 9; max_stack++) {
            limits.max_stack = max_stack;
            expect_same_both_ways(predicate_stack[i].source, limits, predicate_stack[i].outcomes[max_stack - 5]);
        }
    }
    /* A sum's frame has two slots; it pushes the accumulator and x (then
     * x.k in its place), in either order: the eighth and the ninth value.
     * With 5 and 6 the frame cannot be entered, with 7 the first push does
     * not fit, with 8 the second does not. */
    static const struct {
        const char *source;
        const char *outcomes[5];
    } sum_stack[] = {
        { "[2, 1].reduce((a, x) => a + x, 0)",
          { "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:25 the evaluation needs more than max_stack values",
            "fault 1:29 the evaluation needs more than max_stack values", "3" } },
        { "[2, 1].reduce((a, x) => x + a, 0)",
          { "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:25 the evaluation needs more than max_stack values",
            "fault 1:29 the evaluation needs more than max_stack values", "3" } },
        { "[{k: 2}, {k: 1}].reduce((a, x) => a + x.k, 0)",
          { "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:35 the evaluation needs more than max_stack values",
            "fault 1:39 the evaluation needs more than max_stack values", "3" } },
        { "[{k: 2}, {k: 1}].reduce((a, x) => x.k + a, 0)",
          { "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:35 the evaluation needs more than max_stack values",
            "fault 1:41 the evaluation needs more than max_stack values", "3" } },
    };
    for (uint32_t i = 0; i < sizeof sum_stack / sizeof sum_stack[0]; i++) {
        for (uint32_t max_stack = 5; max_stack <= 9; max_stack++) {
            limits.max_stack = max_stack;
            expect_same_both_ways(sum_stack[i].source, limits, sum_stack[i].outcomes[max_stack - 5]);
        }
    }
    /* max_frames: the program, the loop, and the callback the C path
     * enters as the bytecode's first call would. */
    limits = SJS_LIMITS_DEFAULT;
    for (uint32_t max_frames = 2; max_frames <= 3; max_frames++) {
        limits.max_frames = max_frames;
        expect_same_both_ways("[{k: 2}, {k: 1}].toSorted((a, b) => a.k - b.k)", limits,
                              max_frames < 3 ? "fault 1:26 the evaluation needs more than max_frames frames"
                                             : "[{\"k\":1},{\"k\":2}]");
        expect_same_both_ways("[2, 1].toSorted((a, b) => a - b)", limits,
                              max_frames < 3 ? "fault 1:16 the evaluation needs more than max_frames frames" : "[1,2]");
        expect_same_both_ways("[{k: 2}, {k: 1}].map(x => x.k)", limits,
                              max_frames < 3 ? "fault 1:21 the evaluation needs more than max_frames frames" : "[2,1]");
        expect_same_both_ways("[{k: 2}, {k: 1}].filter(x => x.k > 1)", limits,
                              max_frames < 3 ? "fault 1:24 the evaluation needs more than max_frames frames"
                                             : "[{\"k\":2}]");
        expect_same_both_ways("[2, 1].every(x => x >= 2)", limits,
                              max_frames < 3 ? "fault 1:13 the evaluation needs more than max_frames frames" : "false");
        /* reduce enters its callback at its first element, or its second
         * without an initial value; over one element and none, never. */
        expect_same_both_ways("[2, 1].reduce((a, x) => a + x, 0)", limits,
                              max_frames < 3 ? "fault 1:14 the evaluation needs more than max_frames frames" : "3");
        expect_same_both_ways("[2, 1, 4].reduce((a, x) => a + x)", limits,
                              max_frames < 3 ? "fault 1:17 the evaluation needs more than max_frames frames" : "7");
        expect_same_both_ways("[{k: 2}].reduce((a, x) => a + x.k, 0)", limits,
                              max_frames < 3 ? "fault 1:16 the evaluation needs more than max_frames frames" : "2");
        expect_same_both_ways("[{k: 2}].reduce((a, x) => a + x.k)", limits, "{\"k\":2}");
    }
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
    test_interned_keys();
    test_natives_and_globals();
    test_unbound_where_it_never_runs();
    test_modules();
    test_embedder_namespaces();
    test_compile_costs();
    test_module_events();
    test_pattern_events();
    test_statement_events();
    test_event_nodes();
    test_statement_order();
    test_sink();
    test_writer_resumes();
    test_fuel_and_reset();
    test_limits();
    test_loop_callbacks();
    test_reuse_leaves_others_alone();
    test_no_shapes();
    test_shape_paths();
    test_out_of_memory("[1, 2, 3].map(x => ({x, s: `${x}`}))", SJS_GOAL_EXPRESSION);
    /* Functions the matcher tags: its records and text, and the shapes'
     * arguments, fail too. */
    test_out_of_memory("[{k: 2, n: \"b\"}, {k: 1, n: \"a\"}].toSorted((a, b) => { return b.k - a.k; }).map(r => r.n)",
                       SJS_GOAL_EXPRESSION);
    /* Values updated in place: a moved accumulator, literals that take over
     * a spread, concat, with and toSorted on an array nothing else holds.
     * Small enough for every run. */
    test_out_of_memory(reuse_module, SJS_GOAL_MODULE);
    /* A loop's callback with a rest parameter, and reduce destructuring. */
    test_out_of_memory("[1, 2, 3].map((x, ...r) => [x, r]).reduce((acc, [x, r]) => ({...acc, [x]: r.length}), {})",
                       SJS_GOAL_EXPRESSION);
    if (full) {
        test_out_of_memory("const pairs = [[2, \"b\"], [1, \"a\"]].toSorted(([p], [q], d = p - q) => d);\n"
                           "export const nested = pairs.map(([n, k]) => [n, n + 1].map(m => { const f = () => `${k}${m}`; return f; }));\n"
                           "export default nested.flatMap(fs => fs.map(f => f()));\n",
                           SJS_GOAL_MODULE);
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
