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

/* The fuel of test_regex_fuel's matches, pinned. */
#define REGEX_FUEL_TEST 14
#define REGEX_FUEL_REPLACE 31
#define REGEX_FUEL_SPLIT 27
#define REGEX_FUEL_SEARCH 707

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
    sjs_vm_config config = { e->context, false, false };
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
    sjs_vm_config config = { context, false, false };
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

/* The value a program gives, compiled for `goal`. */
static sjs_value result_of(sjs_context *context, sjs_goal goal, const char *source) {
    sjs_program *program;
    sjs_compile_params params = { { context, goal, NULL, 0, { NULL, NULL }, false }, { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    return v;
}

/* An array or an object a program froze or sealed, and a module's
 * namespace, which is sealed, take nothing from the embedder either. */
static void test_frozen_values(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_property p = { SJS_STR("a"), sjs_number(2) };
    sjs_value frozen = result_of(context, SJS_GOAL_EXPRESSION, "Object.freeze({ a: 1 })");
    CHECK(sjs_object_put(context, &frozen, &p) == SJS_ERR_INVALID);
    sjs_value sealed = result_of(context, SJS_GOAL_EXPRESSION, "Object.seal([1])");
    CHECK(sjs_array_append(context, &sealed, sjs_number(2)) == SJS_ERR_INVALID);
    sjs_value plain = result_of(context, SJS_GOAL_EXPRESSION, "[{ a: 1 }, [1]]");
    sjs_value inner = sjs_array_at(plain, 0);
    sjs_retain(inner);
    sjs_release(context, plain);
    CHECK(sjs_object_put(context, &inner, &p) == SJS_OK);
    sjs_value namespace = result_of(context, SJS_GOAL_MODULE, "export const a = 1;\n");
    CHECK(sjs_object_put(context, &namespace, &p) == SJS_ERR_INVALID);
    char text[4096];
    write_text(context, frozen, text, sizeof text);
    CHECK(strcmp(text, "{\"a\":1}") == 0);
    write_text(context, inner, text, sizeof text);
    CHECK(strcmp(text, "{\"a\":2}") == 0);
    sjs_release(context, frozen);
    sjs_release(context, sealed);
    sjs_release(context, inner);
    sjs_release(context, namespace);
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
    [SJS_EV_IF_END] = "IF_END", [SJS_EV_EXPRESSION_STATEMENT] = "EXPRESSION_STATEMENT", /* MUTATION */
    [SJS_EV_ASSIGN] = "ASSIGN", [SJS_EV_ASSIGN_END] = "ASSIGN_END", [SJS_EV_UPDATE] = "UPDATE", [SJS_EV_DELETE] = "DELETE", /* MUTATION */
    [SJS_EV_BIND] = "BIND", [SJS_EV_ARRAY_PATTERN_BEGIN] = "ARRAY_PATTERN_BEGIN",
    [SJS_EV_PATTERN_INDEX] = "PATTERN_INDEX", [SJS_EV_ARRAY_PATTERN_END] = "ARRAY_PATTERN_END",
    [SJS_EV_OBJECT_PATTERN_BEGIN] = "OBJECT_PATTERN_BEGIN", [SJS_EV_PATTERN_KEY] = "PATTERN_KEY",
    [SJS_EV_PATTERN_COMPUTED_KEY] = "PATTERN_COMPUTED_KEY", [SJS_EV_PATTERN_REST] = "PATTERN_REST",
    [SJS_EV_OBJECT_PATTERN_END] = "OBJECT_PATTERN_END", [SJS_EV_DEFAULT] = "DEFAULT", [SJS_EV_DEFAULT_END] = "DEFAULT_END",
    [SJS_EV_CONST_BEGIN] = "CONST_BEGIN", [SJS_EV_CONST_END] = "CONST_END", [SJS_EV_EXPORT] = "EXPORT",
    [SJS_EV_IMPORT] = "IMPORT", [SJS_EV_EXPORT_DEFAULT] = "EXPORT_DEFAULT", [SJS_EV_EXPORT_NAME] = "EXPORT_NAME",
    [SJS_EV_EXPORT_FROM] = "EXPORT_FROM", [SJS_EV_BIGINT] = "BIGINT", [SJS_EV_REGEXP] = "REGEXP", [SJS_EV_END] = "END",
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
    sjs_vm_config config = { context, false, false };
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
    sjs_vm_config config = { context, false, false };
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

/* toSpliced charges for the work it does: a copy pays for every element, a
 * receiver nothing else holds only for what it removes, shifts and inserts. */
static uint64_t fuel_of(sjs_context *context, const char *source) {
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    uint64_t used = sjs_vm_fuel_used(vm);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    sjs_release(context, v);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    return used;
}

static void test_to_spliced_fuel(sjs_context *context) {
    uint64_t taken = fuel_of(context, "[1, 2, 3, 4, 5, 6, 7, 8].toSpliced(6, 1, 9)");
    uint64_t copied = fuel_of(context, "(xs => xs.toSpliced(6, 1, 9))([1, 2, 3, 4, 5, 6, 7, 8])");
    CHECK(taken == 26);
    CHECK(copied == 31);
}

/* toSorted without a compare function charges a unit per element made a
 * string and per comparison, each a step of the loop, and one step to
 * finish; undefined is placed without either: [3, 1, 2] is three strings
 * and three comparisons, [3, undefined, 1] two and one. It grows as
 * n log n. */
static void test_text_sort_fuel(sjs_context *context) {
    uint64_t none = fuel_of(context, "[3, 1, 2].length");
    CHECK(none == 9);
    CHECK(fuel_of(context, "[3, 1, 2].toSorted()") == none + 1 + 3 + 3 + 1);
    CHECK(fuel_of(context, "[3, undefined, 1].toSorted()") == none + 1 + 2 + 1 + 1);
    uint64_t small = fuel_of(context, "'x'.repeat(1000).split('').toSorted().length");
    uint64_t large = fuel_of(context, "'x'.repeat(2000).split('').toSorted().length");
    CHECK(small == 7126 && large == 15241);
}

/* A regular expression's match charges for the instructions it visits and
 * the slots it copies (docs/regex.md, 9.1), pinned here; it grows linearly
 * with the input, a pattern that backtracks exponentially included; and an
 * evaluation pauses within it. */
static void test_regex_fuel(sjs_context *context) {
    CHECK(fuel_of(context, "/ab+c/.test('xxabbbcx')") == REGEX_FUEL_TEST);
    CHECK(fuel_of(context, "'a1b22c333'.replace(/(\\d)+/g, '<$1>')") == REGEX_FUEL_REPLACE);
    CHECK(fuel_of(context, "'a, b,c ,d'.split(/\\s*,\\s*/)") == REGEX_FUEL_SPLIT);
    CHECK(fuel_of(context, "'x'.repeat(1000).search(/y/)") == REGEX_FUEL_SEARCH);
    uint64_t small = fuel_of(context, "/(a+)+$/.test('a'.repeat(2000) + 'b')");
    uint64_t large = fuel_of(context, "/(a+)+$/.test('a'.repeat(4000) + 'b')");
    CHECK(large > small * 3 / 2 && large < small * 2 + 64);
    /* Paused after every unit, a long match pauses within itself, and costs
     * what it costs run whole. */
    const char *source = "'ab'.repeat(500).replace(/b/g, 'c').length";
    uint64_t whole = fuel_of(context, source);
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    sjs_run status;
    uint64_t pauses = 0;
    while ((status = sjs_vm_run(vm, 1)) == SJS_RUN_PAUSED) pauses++;
    CHECK(status == SJS_RUN_DONE && sjs_vm_fuel_used(vm) == whole && pauses > 20);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK && v.kind == SJS_NUMBER && v.as.number == 1000);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    /* An automaton past max_regex_states is a limit when it compiles. */
    sjs_error error;
    sjs_compile_params limit = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false }, SJS_STR("/a{1000000}/"), &error };
    CHECK(sjs_compile(&limit, &program) == SJS_ERR_PROGRAM && error.code == SJS_ERROR_LIMIT);
}

/* A test() over an ASCII input that runs as a loop matches from a cache of
 * the automaton's steps (docs/regex.md, 12.3); by the cache and without it,
 * run whole, paused after every unit and after every 7, a test gives the
 * same result, the same fuel and the same pauses. */
typedef struct {
    const char *source;
    bool no_dfa;
    uint64_t fuel;         /* per sjs_vm_run */
} regex_run;

typedef struct {
    char text[64];
    uint64_t fuel;
    uint64_t pauses;       /* the hash of the fuel used at each pause */
} regex_ran;

static void run_regex(sjs_context *context, const regex_run *r, regex_ran *out) {
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { r->source, (uint32_t)strlen(r->source) }, NULL };
    memset(out, 0, sizeof *out);
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, r->no_dfa };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    sjs_run status;
    out->pauses = 14695981039346656037ull;
    while ((status = sjs_vm_run(vm, r->fuel)) == SJS_RUN_PAUSED) out->pauses = (out->pauses ^ sjs_vm_fuel_used(vm)) * 1099511628211ull;
    CHECK(status == SJS_RUN_DONE);
    out->fuel = sjs_vm_fuel_used(vm);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK && v.kind == SJS_STRING);
    if (v.kind == SJS_STRING) {
        sjs_str t = sjs_string_text(v);
        snprintf(out->text, sizeof out->text, "%.*s", (int)t.length, t.ptr);
    }
    sjs_release(context, v);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
}

static void test_regex_cache(sjs_context *context) {
    static const char *const sources[] = {
        ("(t => JSON.stringify([/^(?:r\\d{6},)*r\\d{6}$/.test(t), /r\\d{6};/.test(t), /r\\d{6}$/.test(t), /r0/.test(t)]))"
         "('r000000,'.repeat(300) + 'r000000')"),
        "JSON.stringify([/(a+)+$/.test('a'.repeat(3000) + 'b'), /(?:a*)*c/.test('ab'.repeat(500)), /(a*)+?b$/.test('a'.repeat(900) + 'b')])",
        "(t => JSON.stringify([/\\bfoo\\b/.test(t), /\\Boo\\B/.test(t), /\\w+\\b$/u.test(t), /^foo$/m.test(t)]))('xfoo foox '.repeat(200) + '\\nfoo')",
        "(t => JSON.stringify([/^b+$/m.test(t), /^b+$/.test(t), /a$/m.test(t), /x.*y/s.test(t), /x.*y/.test(t)]))('a\\n'.repeat(500) + 'x\\nbbb\\ny')",
        "(t => JSON.stringify([/QR\\d/i.test(t), /qr\\d/.test(t), /[^,]{7}$/.test(t), /(?:a|ab)(?:c|bcd)(d*)/.test(t)]))('xqr,'.repeat(500) + 'Qr5abcd')",
    };
    /* From Node 22 (with --enable-experimental-regexp-engine-on-excessive-backtracks
     * for /(a+)+$/). */
    static const char *const expected[] = {
        "[true,false,true,true]", "[false,false,true]", "[true,true,true,true]", "[true,false,true,true,false]", "[true,false,true,true]",
    };
    static const uint64_t fuels[] = { UINT64_MAX, 1, 7 };
    for (size_t i = 0; i < sizeof sources / sizeof sources[0]; i++) {
        regex_ran whole;
        run_regex(context, &(regex_run){ sources[i], true, UINT64_MAX }, &whole);
        CHECK(strcmp(whole.text, expected[i]) == 0);
        for (size_t f = 0; f < sizeof fuels / sizeof fuels[0]; f++) {
            regex_ran cached;
            regex_ran uncached;
            run_regex(context, &(regex_run){ sources[i], false, fuels[f] }, &cached);
            run_regex(context, &(regex_run){ sources[i], true, fuels[f] }, &uncached);
            CHECK(strcmp(cached.text, whole.text) == 0 && strcmp(uncached.text, whole.text) == 0);
            CHECK(cached.fuel == whole.fuel && uncached.fuel == whole.fuel && cached.pauses == uncached.pauses);
            if (strcmp(cached.text, uncached.text) != 0 || cached.fuel != uncached.fuel || cached.pauses != uncached.pauses) {
                fprintf(stderr, "  %s\n  cached:   %s (fuel %llu)\n  uncached: %s (fuel %llu)\n", sources[i], cached.text,
                        (unsigned long long)cached.fuel, uncached.text, (unsigned long long)uncached.fuel);
            }
        }
    }
    /* Inputs of every length over a range, long enough to match as a loop,
     * so that a step's budget (256 instructions visited) runs out at every
     * place in the patterns. */
    for (int n = 2000; n < 2260; n++) {
        char source[256];
        snprintf(source, sizeof source,
                 "(t => JSON.stringify([/^(?:ab)*$/.test(t), /^(?:a|b)+b$/.test(t), /(?:ab|b)c?$/.test(t), /b$/.test(t), /ba?$/m.test(t)]))"
                 "('ab'.repeat(%d))", n);
        regex_ran cached;
        regex_ran uncached;
        run_regex(context, &(regex_run){ source, false, UINT64_MAX }, &cached);
        run_regex(context, &(regex_run){ source, true, UINT64_MAX }, &uncached);
        CHECK(strcmp(cached.text, "[true,true,true,true,true]") == 0 && strcmp(uncached.text, cached.text) == 0 && cached.fuel == uncached.fuel);
    }
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
    sjs_vm_config config = { context, false, false };
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

/* An iterator's flatMap charges a unit for each element it copies out of
 * an array its callback returns, so a callback that returns one large
 * array again and again pays for each copy. */
static void test_iterator_flat_map_fuel(sjs_context *context) {
    uint64_t one = fuel_of(context, "(xs => new Map([[1, 1], [2, 2]]).keys().flatMap(k => xs).toArray().length)('x'.split(''))");
    uint64_t many = fuel_of(context, "(xs => new Map([[1, 1], [2, 2]]).keys().flatMap(k => xs).toArray().length)('x'.repeat(1000).split(''))");
    CHECK(many - one >= 2 * 999);
}

/* The growth in fuel from one element of an array to 1000, of `body` with
 * `xs` the array. */
static uint64_t fuel_growth(sjs_context *context, const char *body) {
    char one[256];
    char many[256];
    snprintf(one, sizeof one, "(xs => %s)('x'.split(''))", body);
    snprintf(many, sizeof many, "(xs => %s)('x'.repeat(1000).split(''))", body);
    return fuel_of(context, many) - fuel_of(context, one);
}

/* An array's keys(), values() and entries() charge a unit for each value
 * read out of the array (an entry's new [index, element] array included),
 * as a map's and a set's do; its forEach runs as a map's forEach's loop, so
 * reading its values first and then iterating costs those units more. */
static void test_array_iterator_fuel(sjs_context *context) {
    uint64_t base = fuel_growth(context, "xs.length");
    CHECK(fuel_growth(context, "xs.values().toArray().length") - base == 999);
    CHECK(fuel_growth(context, "xs.keys().toArray().length") - base == 999);
    CHECK(fuel_growth(context, "xs.entries().toArray().length") - base == 999);
    CHECK(fuel_growth(context, "xs.values().forEach(x => x)") - fuel_growth(context, "xs.forEach(x => x)") == 999);
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
    sjs_vm_config config = { context, false, false };
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
    test_regex_fuel(context);
    test_regex_cache(context);
    test_to_spliced_fuel(context);
    test_text_sort_fuel(context);
    test_set_fuel_linear(context);
    test_iterator_flat_map_fuel(context);
    test_array_iterator_fuel(context);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

/* The host clock (sjs_vm_set_clock): set, advanced and read back; kept
 * across a pause, sjs_vm_start and sjs_vm_reset; refused past Temporal's
 * limits and before it is set; never allocating. */
static bool same_epoch(const sjs_epoch *a, int64_t seconds, uint32_t nanoseconds) {
    return a->seconds == seconds && a->nanoseconds == nanoseconds;
}

static void test_host_clock(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    long allocations = c.allocations;
    sjs_epoch t = { 0, 0 };
    /* Never set. */
    CHECK(sjs_vm_clock(vm, &t) == SJS_ERR_NOT_FOUND);
    CHECK(sjs_vm_advance_clock(vm, 1) == SJS_ERR_INVALID);
    CHECK(sjs_vm_clock(vm, &t) == SJS_ERR_NOT_FOUND);
    /* Set and read. */
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ 1704067200, 123456789 }) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, 1704067200, 123456789));
    /* Advanced: the nanoseconds carry into the seconds. */
    CHECK(sjs_vm_advance_clock(vm, 876543211) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, 1704067201, 0));
    CHECK(sjs_vm_advance_clock(vm, UINT64_C(2999999999)) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, 1704067203, 999999999));
    CHECK(sjs_vm_advance_clock(vm, 0) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, 1704067203, 999999999));
    CHECK(sjs_vm_advance_clock(vm, UINT64_MAX) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, INT64_C(1704067203) + INT64_C(18446744073) + 1, 709551614));
    /* Before the epoch: seconds toward negative infinity. */
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ -1, 999999999 }) == SJS_OK);
    CHECK(sjs_vm_advance_clock(vm, 1) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, 0, 0));
    /* Temporal's limits, 8.64 x 10^12 seconds either way; a refused call
     * leaves the clock as it was. */
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_C(-8640000000000), 0 }) == SJS_OK);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_C(8640000000000), 0 }) == SJS_OK);
    CHECK(sjs_vm_advance_clock(vm, 1) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_C(8640000000000), 1 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_C(-8640000000001), 999999999 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ 0, 1000000000 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_MIN, 0 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_MAX, 0 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, INT64_C(8640000000000), 0));
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ INT64_C(8639999999999), 999999999 }) == SJS_OK);
    CHECK(sjs_vm_advance_clock(vm, 2) == SJS_ERR_INVALID);
    CHECK(sjs_vm_advance_clock(vm, 1) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, INT64_C(8640000000000), 0));
    /* Null handles. */
    CHECK(sjs_vm_set_clock(NULL, &t) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_clock(vm, NULL) == SJS_ERR_INVALID);
    CHECK(sjs_vm_advance_clock(NULL, 1) == SJS_ERR_INVALID);
    CHECK(sjs_vm_clock(NULL, &t) == SJS_ERR_INVALID);
    CHECK(sjs_vm_clock(vm, NULL) == SJS_ERR_INVALID);
    CHECK(c.allocations == allocations);
    /* Set while an evaluation is paused, kept across sjs_vm_reset and
     * sjs_vm_start. */
    const char *source = "[1, 2, 3].map(x => x * 2)";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, 2) == SJS_RUN_PAUSED);
    CHECK(sjs_vm_set_clock(vm, &(sjs_epoch){ -86400, 1 }) == SJS_OK);
    CHECK(sjs_vm_run(vm, 2) == SJS_RUN_PAUSED);
    CHECK(sjs_vm_advance_clock(vm, 1000) == SJS_OK);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, -86400, 1001));
    sjs_vm_reset(vm);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, -86400, 1001));
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    CHECK(sjs_vm_clock(vm, &t) == SJS_OK && same_epoch(&t, -86400, 1001));
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    sjs_release(context, v);
    sjs_program_release(program);
    sjs_vm_destroy(vm);
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
    at += snprintf(at, sizeof source - (size_t)(at - source), "(");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = '1';
    for (int i = 0; i < 70; i++) *at++ = ']';
    snprintf(at, sizeof source - (size_t)(at - source), ").length");
    context = new_context(&c, SJS_LIMITS_DEFAULT);
    e.context = context;
    CHECK(evaluate(&e, source, out) == SJS_OK && strcmp(out, "1") == 0);
    at = source;
    at += snprintf(at, sizeof source - (size_t)(at - source), "((");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = 'a';
    for (int i = 0; i < 70; i++) *at++ = ']';
    at += snprintf(at, sizeof source - (size_t)(at - source), ") => a)(");
    for (int i = 0; i < 70; i++) *at++ = '[';
    *at++ = '7';
    for (int i = 0; i < 70; i++) *at++ = ']';
    snprintf(at, sizeof source - (size_t)(at - source), ")");
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
        end += snprintf(end, sizeof chain - (size_t)(end - chain), "(x => { if (x === 0) return 0;");
        for (int i = 1; i < 1000; i++) end += snprintf(end, sizeof chain - (size_t)(end - chain), " else if (x === %d) { const y = %d; return y; }", i, i);
        snprintf(end, sizeof chain - (size_t)(end - chain), " return -1; })(999)");
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
    sjs_vm_config config = { context, false, false };
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

/* A program reads the clock (Temporal.Now): what the host set before the
 * run, the same at every read with no call between them, the clock as
 * moved at a pause after it; never set, a fault. */
static void now_reads(loop_run *l, uint64_t advance, long long out[2]) {
    sjs_run_params run = { l->program, NULL, 0 };
    char text[4096];
    CHECK(sjs_vm_start(l->vm, &run) == SJS_OK);
    sjs_run status;
    while ((status = sjs_vm_run(l->vm, 1)) == SJS_RUN_PAUSED) {
        if (advance != 0) CHECK(sjs_vm_advance_clock(l->vm, advance) == SJS_OK);
    }
    out[0] = out[1] = -1;
    CHECK(status == SJS_RUN_DONE);
    sjs_value v;
    CHECK(sjs_vm_take_result(l->vm, &v) == SJS_OK);
    write_text(l->context, v, text, sizeof text);
    sjs_release(l->context, v);
    CHECK(sscanf(text, "[%lldn,%lldn]", &out[0], &out[1]) == 2);
}

static void test_now_reads(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    loop_run l;
    CHECK(loop_open(&l, context, "[Temporal.Now.instant().epochNanoseconds, [1, 2, 3].map(x => x * 2).length,"
                                 " Temporal.Now.instant().epochNanoseconds].filter(x => x !== 3)"));
    char out[4096];
    /* Never set. */
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_FAULT && strstr(out, "the host has not set the clock (sjs_vm_set_clock)") != NULL);
    long long t[2];
    CHECK(sjs_vm_set_clock(l.vm, &(sjs_epoch){ 1704067200, 123456789 }) == SJS_OK);
    /* Paused at every unit, the clock untouched: every read agrees. */
    now_reads(&l, 0, t);
    CHECK(t[0] == 1704067200123456789LL && t[1] == t[0]);
    /* Advanced at every pause: each read sees the clock as the host left it,
     * the second later by a microsecond for each pause between them. */
    now_reads(&l, 1000, t);
    CHECK(t[0] > 1704067200123456789LL && (t[0] - 1704067200123456789LL) % 1000 == 0);
    CHECK(t[1] > t[0] && (t[1] - t[0]) % 1000 == 0);
    sjs_epoch at;
    CHECK(sjs_vm_clock(l.vm, &at) == SJS_OK && (long long)at.seconds * 1000000000LL + at.nanoseconds >= t[1]);
    /* Set again: whole runs read it. */
    CHECK(sjs_vm_set_clock(l.vm, &(sjs_epoch){ -1, 999999999 }) == SJS_OK);
    CHECK(loop_finish(&l, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "[-1n,-1n]") == 0);
    loop_close(&l);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
}

/* Math.random's seed (sjs_vm_set_seed): V8's sequence (the values below
 * are what `node --random-seed=42` draws), continued across pauses,
 * sjs_vm_reset and sjs_vm_start; saved with sjs_vm_random and restored in
 * another evaluator with sjs_vm_set_random, either side of V8's cache of
 * 64; held by the evaluator, so one never seeded faults; no call
 * allocating. */
static bool same_random(const sjs_random *a, const sjs_random *b) {
    return a->state[0] == b->state[0] && a->state[1] == b->state[1] && a->left == b->left;
}

static void test_host_seed(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    loop_run two;
    loop_run skip;
    loop_run unseeded;
    CHECK(loop_open(&two, context, "[Math.random(), Math.random()]"));
    CHECK(loop_open(&skip, context, "Array.from({ length: 63 }, () => Math.random()).length"));
    CHECK(loop_open(&unseeded, context, "[1, 2].map(x => x * Math.random())"));
    char out[4096];
    sjs_random r;
    sjs_random again;
    /* Never set. */
    CHECK(sjs_vm_random(two.vm, &r) == SJS_ERR_NOT_FOUND);
    CHECK(loop_finish(&two, UINT64_MAX, out) == SJS_RUN_FAULT);
    CHECK(strcmp(out, "fault 1:13 the host has not set the seed (sjs_vm_set_seed)") == 0);
    long allocations = c.allocations;
    CHECK(sjs_vm_set_seed(two.vm, 42) == SJS_OK);
    CHECK(sjs_vm_random(two.vm, &r) == SJS_OK && (r.state[0] | r.state[1]) != 0 && r.left == 0);
    /* Refused, the generator unchanged: a state of zeros, more than 64 left,
     * null handles. */
    CHECK(sjs_vm_set_random(two.vm, &(sjs_random){ { 0, 0 }, 0 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_random(two.vm, &(sjs_random){ { 1, 0 }, 65 }) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_seed(NULL, 42) == SJS_ERR_INVALID);
    CHECK(sjs_vm_random(NULL, &again) == SJS_ERR_INVALID);
    CHECK(sjs_vm_random(two.vm, NULL) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_random(NULL, &r) == SJS_ERR_INVALID);
    CHECK(sjs_vm_set_random(two.vm, NULL) == SJS_ERR_INVALID);
    CHECK(sjs_vm_random(two.vm, &again) == SJS_OK && same_random(&again, &r));
    CHECK(c.allocations == allocations);
    /* Node's values; a second evaluation continues the first's sequence,
     * paused at every unit. */
    CHECK(loop_finish(&two, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "[0.7939112874678715,0.5254990606499601]") == 0);
    CHECK(loop_finish(&two, 1, out) == SJS_RUN_DONE && strcmp(out, "[0.3518347850388237,0.963056226312738]") == 0);
    /* Set again, the sequence starts over. Reset after the first draw keeps
     * it drawn. */
    CHECK(sjs_vm_set_seed(two.vm, 42) == SJS_OK);
    sjs_run_params run = { two.program, NULL, 0 };
    CHECK(sjs_vm_start(two.vm, &run) == SJS_OK);
    while (sjs_vm_random(two.vm, &r) == SJS_OK && r.left == 0) CHECK(sjs_vm_run(two.vm, 1) == SJS_RUN_PAUSED);
    CHECK(r.left == 63);
    sjs_vm_reset(two.vm);
    CHECK(sjs_vm_random(two.vm, &again) == SJS_OK && same_random(&again, &r));
    CHECK(loop_finish(&two, 1, out) == SJS_RUN_DONE && strcmp(out, "[0.5254990606499601,0.3518347850388237]") == 0);
    /* 63 draws in one evaluator; its state restored in another draws the
     * 64th, the cache's last, and the 65th, the next refill's first. */
    CHECK(sjs_vm_set_seed(skip.vm, 42) == SJS_OK);
    CHECK(loop_finish(&skip, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "63") == 0);
    CHECK(sjs_vm_random(skip.vm, &r) == SJS_OK && r.left == 1);
    CHECK(sjs_vm_set_random(two.vm, &r) == SJS_OK);
    CHECK(sjs_vm_random(two.vm, &again) == SJS_OK && same_random(&again, &r));
    CHECK(loop_finish(&two, 1, out) == SJS_RUN_DONE && strcmp(out, "[0.08156904043271651,0.4706713645501157]") == 0);
    /* The same state with none left is the one after the 64th draw. */
    r.left = 0;
    CHECK(sjs_vm_set_random(two.vm, &r) == SJS_OK);
    CHECK(loop_finish(&two, UINT64_MAX, out) == SJS_RUN_DONE && strcmp(out, "[0.4706713645501157,0.2603201442309373]") == 0);
    /* The seed is the evaluator's: another of the same context has none. */
    CHECK(loop_finish(&unseeded, 1, out) == SJS_RUN_FAULT);
    CHECK(strstr(out, "the host has not set the seed (sjs_vm_set_seed)") != NULL);
    loop_close(&two);
    loop_close(&skip);
    loop_close(&unseeded);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
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
    at += snprintf(at, sizeof source - (size_t)(at - source), "[");
    for (int i = 1; i <= 64; i++) at += snprintf(at, sizeof source - (size_t)(at - source), i == 1 ? "%d" : ", %d", i);
    snprintf(at, sizeof source - (size_t)(at - source), "].reduce((acc, x) => { const y = x; return [...acc, y]; }, []).length");
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
    sjs_vm_config config = { context, false, false };
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
 * matches the rewrites alone (MUTATION: it made no matcher before), so it
 * does not allocate the matcher's records, and the program gives the same
 * value. A config that leaves the field out matches. */
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
    sjs_vm_config config = { context, false, false };
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
/* ------------------------------------------------------------------------
 * MUTATION: owned mutation (docs/mutation.md).
 */

/* MUTATION: reduce with a PUSH_KEY or a PUSH callback (docs/matcher.md,
 * 13.2), as test_shape_paths runs the other shapes: the same value or
 * fault both ways, for less fuel. */
static void test_mutation_push_paths(void) {
    /* Fuel, pinned. Four elements: 10 units each in bytecode (the step,
     * RECEIVER, METHOD_MUTATE, TAKE_LOCAL, GET_MEMBER, MUTATE and push's
     * unit, POP, TAKE_LOCAL, RETURN), 3 in C (the step, the lookup and
     * push's unit), which is one pause. An empty literal has room for
     * four: every element is pushed in C. */
    const char *key = "[{k: 1}, {k: 2}, {k: 3}, {k: 4}].reduce((a, x) => { a.push(x.k); return a; }, [])";
    shaped_run on = run_shaped((shaped_source){ key, false, SJS_LIMITS_DEFAULT });
    shaped_run off = run_shaped((shaped_source){ key, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[1,2,3,4]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 60 && on.fuel == 32 && off.fuel - on.fuel == 4 * (10 - 3));
    CHECK(off.pauses == 55 && on.pauses == 55 - 4 * 8);
    /* The element: 9 units each in bytecode (no GET_MEMBER), 2 in C. */
    const char *element = "[1, 2, 3, 4].reduce((a, x) => { a.push(x); return a; }, [])";
    on = run_shaped((shaped_source){ element, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ element, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[1,2,3,4]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 52 && on.fuel == 24 && off.fuel - on.fuel == 4 * (9 - 2));
    CHECK(off.pauses == 47 && on.pauses == 47 - 4 * 7);
    /* An array without room: the bytecode pushes the element that grows
     * it (here the first, into [0], which holds one), C the rest. */
    const char *grows = "[1, 2, 3, 4, 5, 6].reduce((a, x) => { a.push(x); return a; }, [0])";
    on = run_shaped((shaped_source){ grows, false, SJS_LIMITS_DEFAULT });
    off = run_shaped((shaped_source){ grows, true, SJS_LIMITS_DEFAULT });
    CHECK(strcmp(on.out, "[0,1,2,3,4,5,6]") == 0 && strcmp(off.out, on.out) == 0);
    CHECK(off.fuel == 76 && on.fuel == 41 && off.fuel - on.fuel == 5 * (9 - 2));
    /* Faults after C steps: where the bytecode faults, for less fuel. */
    expect_same_both_ways("[{k: 1},\n {k: 2}, null].reduce((a, x) =>\n { a.push(x.k); return a; }, [])", SJS_LIMITS_DEFAULT,
                          "fault 3:13 cannot read a property of null");
    expect_same_both_ways("[{toString: 1}, {toString: 2},\n {}].reduce((a, x) =>\n { a.push(x.toString); return a; }, [])",
                          SJS_LIMITS_DEFAULT, "fault 3:13 staticjs objects have no prototype");
    expect_same_both_ways("(xs => { const z = []; Object.freeze(z); return xs.reduce((a, x) => { a.push(x); return a; }, z); })([1, 2])",
                          SJS_LIMITS_DEFAULT,
                          "fault 1:77 staticjs changes no key or element of a frozen object or array, where JavaScript throws a TypeError");
    expect_same_both_ways("(z => [1, 2, z].reduce((a, x) => { a.push(x); return a; }, z))([])", SJS_LIMITS_DEFAULT,
                          "fault 1:42 staticjs changes a value only while one variable holds it, and this one is held elsewhere too");
    /* max_stack: the callback's frame has two slots above the loop's five;
     * a push's bytecode pushes the reference to the accumulator's slot,
     * the method and the argument: the eighth, ninth and tenth values.
     * With 5 and 6 the frame cannot be entered, with 7, 8 and 9 one of the
     * three does not fit, and each faults there on both paths. */
    static const struct {
        const char *source;
        const char *outcomes[6];
    } push_stack[] = {
        { "[1, 2].reduce((a, x) => { a.push(x); return a; }, [7])",
          { "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:14 the evaluation needs more than max_stack values",
            "fault 1:27 the evaluation needs more than max_stack values",
            "fault 1:29 the evaluation needs more than max_stack values",
            "fault 1:34 the evaluation needs more than max_stack values", "[7,1,2]" } },
        { "[{k: 2}, {k: 1}].reduce((a, x) => { a.push(x.k); return a; }, [])",
          { "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:24 the evaluation needs more than max_stack values",
            "fault 1:37 the evaluation needs more than max_stack values",
            "fault 1:39 the evaluation needs more than max_stack values",
            "fault 1:44 the evaluation needs more than max_stack values", "[2,1]" } },
    };
    sjs_limits limits = SJS_LIMITS_DEFAULT;
    for (uint32_t i = 0; i < sizeof push_stack / sizeof push_stack[0]; i++) {
        for (uint32_t max_stack = 5; max_stack <= 10; max_stack++) {
            limits.max_stack = max_stack;
            expect_same_both_ways(push_stack[i].source, limits, push_stack[i].outcomes[max_stack - 5]);
        }
    }
    /* max_frames: the program, the loop, and the callback the C path
     * enters as the bytecode's first call would. */
    limits = SJS_LIMITS_DEFAULT;
    for (uint32_t max_frames = 2; max_frames <= 3; max_frames++) {
        limits.max_frames = max_frames;
        expect_same_both_ways(push_stack[0].source, limits,
                              max_frames < 3 ? "fault 1:14 the evaluation needs more than max_frames frames" : "[7,1,2]");
        expect_same_both_ways(push_stack[1].source, limits,
                              max_frames < 3 ? "fault 1:24 the evaluation needs more than max_frames frames" : "[2,1]");
    }
}

/* A statement that is a call is sent, with EXPRESSION_STATEMENT after the
 * call's events: a node of its own that starts where the statement does. */
static void test_mutation_events(void) {
    static event_log log;
    parse_events("xs => { xs.push(1); return xs; }", SJS_GOAL_EXPRESSION, &log);
    static const int events[] = {
        SJS_EV_ARROW_BEGIN, 1, SJS_EV_PARAM, 0, SJS_EV_BIND, 0, SJS_EV_PARAMS_END, 0, SJS_EV_BLOCK_BEGIN, 0,
        SJS_EV_NAME, 0, SJS_EV_MEMBER, 0, SJS_EV_CALL_BEGIN, 0, SJS_EV_NUMBER, 0, SJS_EV_ARGUMENT, 0,
        SJS_EV_CALL_END, 1, SJS_EV_EXPRESSION_STATEMENT, 0, SJS_EV_NAME, 0, SJS_EV_RETURN, 1, SJS_EV_BLOCK_END, 0,
        SJS_EV_ARROW_END, 0, SJS_EV_END, 0
    };
    static const char *const texts[] = {
        NULL, NULL, "xs", NULL, NULL, "xs", "push", NULL, NULL, NULL, NULL, NULL, "xs", NULL, NULL, NULL, NULL
    };
    expect_events(&log, events, (int)(sizeof texts / sizeof texts[0]), texts);
    expect_nodes("x => { x.add(1).add(2); }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 NAME#4@7 MEMBER#5@7 CALL_BEGIN#6@7 "
                 "NUMBER#7@13 ARGUMENT#6@7 CALL_END#6@7 MEMBER#8@7 CALL_BEGIN#9@7 NUMBER#10@20 ARGUMENT#9@7 "
                 "CALL_END#9@7 EXPRESSION_STATEMENT#3@7 BLOCK_END#1@0 ARROW_END#1@0 END#0@0");
}

static sjs_result fresh_value(void *self, sjs_native_call *call) {
    (void)self;
    return sjs_array_new(call->context, 2, &call->result);
}

static const sjs_native_vtable fresh_vtable = { fresh_value };

/* A value a native gives and still holds, or keeps, is never changed: the
 * mutation faults when it runs, or, for one the function passed to it
 * before, is refused when compiled. A value a native gives away, or keeps
 * after the last mutation, is the function's to change. */
static void test_mutation_natives(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    keeper k = { 0 };
    k.held = number_array(context, (const double[]){ 1, 2 }, 2);
    sjs_value natives[3];
    CHECK(sjs_function_native(context, (sjs_native){ &k, &keep_vtable }, &natives[0]) == SJS_OK);
    CHECK(sjs_function_native(context, (sjs_native){ &k, &held_vtable }, &natives[1]) == SJS_OK);
    CHECK(sjs_function_native(context, (sjs_native){ NULL, &fresh_vtable }, &natives[2]) == SJS_OK);
    sjs_str names[3] = { SJS_STR("keep"), SJS_STR("held"), SJS_STR("fresh") };
    env e = { context, names, natives, 3, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "(() => { const xs = held(); xs.push(3); return xs; })()", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "fault") == out && strstr(out, "held elsewhere") != NULL);
    CHECK(evaluate(&e, "(xs => { xs.sort((a, b) => b - a); return xs; })(held())", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "held elsewhere") != NULL);
    write_text(context, k.held, out, sizeof out);
    CHECK(strcmp(out, "[1,2]") == 0);
    CHECK(evaluate(&e, "(() => { const xs = fresh(); xs.push(1); xs.unshift(0); return xs; })()", out) == SJS_OK);
    CHECK(strcmp(out, "[0,1]") == 0);
    CHECK(evaluate(&e, "(() => { const xs = [1]; xs.push(2); const kept = keep(xs); return xs.length; })()", out) == SJS_OK);
    CHECK(strcmp(out, "2") == 0);
    CHECK(k.count == 1);
    write_text(context, k.kept[0], out, sizeof out);
    CHECK(strcmp(out, "[1,2]") == 0);
    CHECK(evaluate(&e, "(() => { const xs = [1]; const kept = keep(xs); xs.push(2); return xs; })()", out) == SJS_ERR_PROGRAM);
    char unsupported[16];
    snprintf(unsupported, sizeof unsupported, "error %d ", (int)SJS_ERROR_UNSUPPORTED);
    CHECK(strstr(out, unsupported) == out && strstr(out, "passed") != NULL);
    CHECK(k.count == 1);
    for (int i = 0; i < k.count; i++) sjs_release(context, k.kept[i]);
    sjs_release(context, k.held);
    for (int i = 0; i < 3; i++) sjs_release(context, natives[i]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* Each method charges for the elements it moves or writes: shift for the
 * ones it moves down, unshift for the ones it moves up, reverse and fill
 * for each one; pop for none. sort costs what toSorted costs, and the
 * instruction that puts the array back. */
static void test_mutation_fuel(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char source[256];
    uint64_t fuel[6];
    static const char *const calls[6] = { "xs.pop()", "xs.shift()", "xs.unshift(0)", "xs.push(0)", "xs.reverse().length", "xs.fill(0).length" };
    for (int i = 0; i < 6; i++) {
        snprintf(source, sizeof source, "(() => { const xs = Array.from({length: 1000}, (_, i) => i); return %s; })()", calls[i]);
        fuel[i] = fuel_of(context, source);
    }
    CHECK(fuel[1] - fuel[0] == 999);
    CHECK(fuel[2] - fuel[3] == 1000);
    /* push's argument is one more instruction. */
    CHECK(fuel[3] - fuel[0] == 2);
    /* reverse and fill read a member after: one GET_MEMBER more than pop. */
    CHECK(fuel[4] - fuel[0] == 1001);
    CHECK(fuel[5] - fuel[0] == 1002);
    const char *sorted = "(() => { const xs = [5, 3, 1, 4, 2, 0]; return xs.toSorted((a, b) => (a < b ? -1 : 1)); })()";
    const char *sorts = "(() => { const xs = [5, 3, 1, 4, 2, 0]; return xs.sort((a, b) => (a < b ? -1 : 1)); })()";
    /* The one more instruction that puts the sorted array back. */
    CHECK(fuel_of(context, sorts) == fuel_of(context, sorted) + 1u);
    /* A comparator of a shape runs in C for both (docs/matcher.md). */
    const char *sorted_shape = "(() => { const xs = [5, 3, 1, 4, 2, 0]; return xs.toSorted((a, b) => a - b); })()";
    const char *sorts_shape = "(() => { const xs = [5, 3, 1, 4, 2, 0]; return xs.sort((a, b) => a - b); })()";
    CHECK(fuel_of(context, sorts_shape) == fuel_of(context, sorted_shape) + 1u);
    CHECK(fuel_of(context, sorts_shape) < fuel_of(context, sorts));
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* An assignment's target comes first, then ASSIGN, the value and
 * ASSIGN_END, a node that starts with the target; ++ and -- are UPDATE
 * after their target, a node that starts with the target or, prefix, the
 * operator. Neither sends EXPRESSION_STATEMENT. */
static void test_mutation_assign_events(void) {
    expect_nodes("o => { o.a[i] += 1; o.n++; --o.m; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 NAME#4@7 MEMBER#5@7 NAME#7@11 INDEX#6@7 "
                 "ASSIGN#8@7 NUMBER#9@17 ASSIGN_END#8@7 NAME#11@20 MEMBER#12@20 UPDATE#13@20 NAME#16@29 MEMBER#17@29 "
                 "UPDATE#15@27 BLOCK_END#1@0 ARROW_END#1@0 END#0@0");
}

/* Events in an order no parser sends: an assignment outside a statement,
 * an ASSIGN_END with no ASSIGN or another operator, an UPDATE that neither
 * adds nor subtracts. */
static void test_mutation_assign_order(void) {
    static const char *const out_of_order = "the events are out of order";
    const sjs_event body[] = { { .kind = SJS_EV_ARROW_BEGIN, .count = 1 }, { .kind = SJS_EV_PARAM }, { .kind = SJS_EV_BIND, .text = SJS_STR("o") },
                               { .kind = SJS_EV_PARAMS_END }, { .kind = SJS_EV_BLOCK_BEGIN } };
    const sjs_event member[] = { { .kind = SJS_EV_NAME, .text = SJS_STR("o"), .at = { 7, 1, 8 }, .start = { 7, 1, 8 } },
                                 { .kind = SJS_EV_MEMBER, .text = SJS_STR("k"), .at = { 9, 1, 10 }, .start = { 7, 1, 8 } } };
    const sjs_event expression[] = { body[0], body[1], body[2], body[3], member[0], member[1], { .kind = SJS_EV_ASSIGN } };
    compile_events(expression, 7, out_of_order);
    const sjs_event no_assign[] = { body[0], body[1], body[2], body[3], body[4], { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_ASSIGN_END } };
    compile_events(no_assign, 7, out_of_order);
    const sjs_event other[] = { body[0], body[1], body[2], body[3], body[4], member[0], member[1], { .kind = SJS_EV_ASSIGN },
                                { .kind = SJS_EV_TRUE }, { .kind = SJS_EV_ASSIGN_END, .count = SJS_OP_ADD } };
    compile_events(other, 10, out_of_order);
    const sjs_event bitwise[] = { body[0], body[1], body[2], body[3], body[4], member[0], member[1],
                                  { .kind = SJS_EV_ASSIGN, .count = SJS_OP_LESS } };
    compile_events(bitwise, 8, out_of_order);
    const sjs_event update[] = { body[0], body[1], body[2], body[3], body[4], member[0], member[1],
                                 { .kind = SJS_EV_UPDATE, .count = SJS_OP_MULTIPLY } };
    compile_events(update, 8, out_of_order);
}

/* A value a native gives and still holds is never assigned to; one it
 * gives away is the function's. */
static void test_mutation_assign_natives(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    keeper k = { 0 };
    k.held = number_array(context, (const double[]){ 1, 2 }, 2);
    sjs_value natives[2];
    CHECK(sjs_function_native(context, (sjs_native){ &k, &held_vtable }, &natives[0]) == SJS_OK);
    CHECK(sjs_function_native(context, (sjs_native){ NULL, &fresh_vtable }, &natives[1]) == SJS_OK);
    sjs_str names[2] = { SJS_STR("held"), SJS_STR("fresh") };
    env e = { context, names, natives, 2, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "(() => { const xs = held(); xs[0] = 9; return xs; })()", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "fault") == out && strstr(out, "held elsewhere") != NULL);
    CHECK(evaluate(&e, "(() => { const o = {xs: held()}; o.xs[1] += 1; return o; })()", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "held elsewhere") != NULL);
    write_text(context, k.held, out, sizeof out);
    CHECK(strcmp(out, "[1,2]") == 0);
    CHECK(evaluate(&e, "(() => { const o = {xs: fresh()}; o.xs[0] = 1; o.xs[1] ?\?= 2; o.xs[0]++; return o; })()", out) == SJS_OK);
    CHECK(strcmp(out, "{\"xs\":[2,2]}") == 0);
    sjs_release(context, k.held);
    for (int i = 0; i < 2; i++) sjs_release(context, natives[i]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* A path costs a unit of fuel for each key after the first, besides its
 * instructions: one per key pushed and the PATH. */
static void test_mutation_assign_fuel(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    uint64_t one = fuel_of(context, "(() => { const o = {a: {b: {c: {}}}}; o.a = 1; return 0; })()");
    uint64_t four = fuel_of(context, "(() => { const o = {a: {b: {c: {}}}}; o.a.b.c.d = 1; return 0; })()");
    CHECK(four - one == 6);
    uint64_t plain = fuel_of(context, "(() => { const o = {a: 1}; o.a = 2; return 0; })()");
    uint64_t compound = fuel_of(context, "(() => { const o = {a: 1}; o.a += 2; return 0; })()");
    uint64_t update = fuel_of(context, "(() => { const o = {a: 1}; o.a++; return 0; })()");
    /* The LOAD and the BINARY. */
    CHECK(compound - plain == 2);
    CHECK(update == compound);
    /* ??= of a defined value: the LOAD, the jump and the DROP, for the
     * value and the STORE. */
    uint64_t kept = fuel_of(context, "(() => { const o = {a: 1}; o.a ?\?= 2; return 0; })()");
    CHECK(kept - plain == 1);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* let is CONST_BEGIN with count 1, or 2 for a declarator without a value,
 * whose pattern comes with no value before it. */
static void test_mutation_let_events(void) {
    expect_nodes("() => { let a = 1, b; b = a; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 CONST_BEGIN#2@8 NUMBER#4@16 BIND#3@12 CONST_END#2@8 "
                 "CONST_BEGIN#5@8 BIND#6@19 CONST_END#5@8 NAME#8@22 ASSIGN#9@22 NAME#10@26 ASSIGN_END#9@22 BLOCK_END#1@0 "
                 "ARROW_END#1@0 END#0@0");
    static event_log log;
    parse_events("() => { let a = 1, b; b += a; }", SJS_GOAL_EXPRESSION, &log);
    static const int events[] = {
        SJS_EV_ARROW_BEGIN, 0, SJS_EV_PARAMS_END, 0, SJS_EV_BLOCK_BEGIN, 0, SJS_EV_CONST_BEGIN, 1, SJS_EV_NUMBER, 0,
        SJS_EV_BIND, 0, SJS_EV_CONST_END, 0, SJS_EV_CONST_BEGIN, 2, SJS_EV_BIND, 0, SJS_EV_CONST_END, 0, SJS_EV_NAME, 0,
        SJS_EV_ASSIGN, SJS_OP_ADD, SJS_EV_NAME, 0, SJS_EV_ASSIGN_END, SJS_OP_ADD, SJS_EV_BLOCK_END, 0, SJS_EV_ARROW_END, 0,
        SJS_EV_END, 0
    };
    static const char *const texts[] = {
        NULL, NULL, NULL, NULL, NULL, "a", NULL, NULL, "b", NULL, "b", NULL, "a", NULL, NULL, NULL, NULL
    };
    expect_events(&log, events, (int)(sizeof texts / sizeof texts[0]), texts);
    /* A CONST_BEGIN of let outside a function, or of a count no parser sends. */
    const sjs_event top_let[] = { { .kind = SJS_EV_CONST_BEGIN, .count = 1 } };
    compile_events(top_let, 1, "the events are out of order");
    const sjs_event body[] = { { .kind = SJS_EV_ARROW_BEGIN }, { .kind = SJS_EV_PARAMS_END }, { .kind = SJS_EV_BLOCK_BEGIN },
                               { .kind = SJS_EV_CONST_BEGIN, .count = 3 } };
    compile_events(body, 4, "the events are out of order");
}

static void test_out_of_memory(const char *source, sjs_goal goal);

/* Every allocation of a program with let variables, assigned, compounded
 * and moved, fails in turn. */
static void test_mutation_let_out_of_memory(void) {
    test_out_of_memory("[1, 2, 3].map(n => {\n"
                       "  let s = \"\", xs = [], o;\n"
                       "  s += n;\n"
                       "  xs = [...xs, s];\n"
                       "  o ?\?= {};\n"
                       "  o[s] = xs;\n"
                       "  { let k = n; k++; o.k = k; }\n"
                       "  return o;\n"
                       "})",
                       SJS_GOAL_EXPRESSION);
}

/* Every allocation of a program that assigns, appending to arrays and to
 * objects past the size that gives them an index, an integer key among
 * them, fails in turn. */
static void test_mutation_assign_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const o = [\"a\", \"b\", \"c\", \"d\", \"e\", \"f\", \"g\", \"h\", \"i\"].reduce((acc, k) => { acc[k] = [k]; return acc; }, {});\n"
                       "  o[3] = 0;\n"
                       "  o.a[1] = 1;\n"
                       "  o.b[0] += \"x\";\n"
                       "  o.c[1] ?\?= [];\n"
                       "  o.c[1][0] = 2;\n"
                       "  o[3]++;\n"
                       "  return o;\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* Every allocation of a program that grows arrays and a set in place,
 * past the size that gives the set an index, and sorts, fails in turn. */
static void test_mutation_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const xs = [5, 3, 9, 1, 7, 2, 8, 6, 4, 0].reduce((a, x) => { a.push(x); a.unshift(-x); return a; }, []);\n"
                       "  const s = xs.reduce((t, x) => t.add(x % 13), new Set());\n"
                       "  xs.sort((a, b) => a - b).fill(0, 15).reverse();\n"
                       "  const p = xs.pop();\n"
                       "  const q = xs.shift();\n"
                       "  const n = xs.push(...xs);\n"
                       "  return [xs, [...s], p, q, n];\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* MUTATION: removal and shrinking (docs/mutation.md, "Removal"). live_of
 * and allocations_of serve STRING_APPEND's tests too: rolling mutation back
 * keeps them. */

/* The bytes the context holds while it holds the value `source` gives,
 * and nothing else. */
static size_t live_of(sjs_context *context, const char *source) {
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    CHECK(sjs_compile(&params, &program) == SJS_OK);
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, NULL, 0 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    size_t live = sjs_context_live_bytes(context);
    sjs_release(context, v);
    return live;
}

/* The allocations a program makes, compiled and run. */
static long allocations_of(counting *c, sjs_context *context, const char *source) {
    long before = c->allocations;
    fuel_of(context, source);
    return c->allocations - before;
}

/* An array that pop and shift take most elements from gives its memory
 * back; one that push and pop alternate on is not reallocated each time. */
static void test_mutation_shrink_array(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char source[512];
    static const char *const build = "(() => { const xs = Array.from({length: 4000}, (_, i) => i); return ";
    snprintf(source, sizeof source, "%sxs; })()", build);
    size_t full = live_of(context, source);
    snprintf(source, sizeof source, "%sArray.from({length: 3990}).reduce((a, _, i) => { if (i %% 2) a.pop(); else a.shift(); return a; }, xs); })()", build);
    size_t emptied = live_of(context, source);
    size_t small = live_of(context, "Array.from({length: 10}, (_, i) => i)");
    CHECK(full > 4000 * 16);
    CHECK(emptied < full / 50);
    CHECK(emptied <= small + 512);
    snprintf(source, sizeof source, "%s[xs.splice(5, 3990).length, xs]; })()", build);
    size_t spliced = live_of(context, source);
    CHECK(spliced < full / 50);
    long alternations[2];
    long plain[2];
    static const int counts[2] = { 10, 2000 };
    for (int i = 0; i < 2; i++) {
        snprintf(source, sizeof source,
                 "(() => { const xs = Array.from({length: 100}, (_, i) => i); return Array.from({length: %d}).reduce((a) => { a.push(1); a.pop(); a.pop(); a.push(2); return a; }, xs).length; })()",
                 counts[i]);
        alternations[i] = allocations_of(&c, context, source);
        snprintf(source, sizeof source,
                 "(() => { const xs = Array.from({length: 100}, (_, i) => i); return Array.from({length: %d}).reduce((a) => { return a; }, xs).length; })()",
                 counts[i]);
        plain[i] = allocations_of(&c, context, source);
    }
    /* No allocation per round: only the first push grows the array. */
    CHECK(alternations[1] - alternations[0] == plain[1] - plain[0]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* splice costs one unit per element it removes, moves or adds. */
static void test_mutation_splice_fuel(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char source[256];
    uint64_t fuel[3];
    static const char *const calls[3] = { "xs.splice(990, 10)", "xs.splice(500, 10)", "xs.splice(500, 10, 1, 2)" };
    for (int i = 0; i < 3; i++) {
        snprintf(source, sizeof source, "(() => { const xs = Array.from({length: 1000}, (_, i) => i); return %s.length; })()", calls[i]);
        fuel[i] = fuel_of(context, source);
    }
    /* 490 more elements move down. */
    CHECK(fuel[1] - fuel[0] == 490);
    /* Two arguments more to push, two elements more to add. */
    CHECK(fuel[2] - fuel[1] == 4);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* Every allocation of a program that splices, growing and shrinking an
 * array in place, fails in turn. */
static void test_mutation_splice_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const xs = Array.from({length: 40}, (_, i) => [i]);\n"
                       "  const a = xs.splice(2, 30);\n"
                       "  const b = xs.splice(1, 1, \"x\", \"y\", \"z\", ...a.slice(0, 5));\n"
                       "  const c = xs.splice(-2);\n"
                       "  return [xs, a.length, b, c];\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* A set that delete takes most members from, or clear empties, gives its
 * memory back, its index included. Removing the last member costs one
 * unit, as a missing one does; any other, the set's length more, for the
 * members that move down; clear, one per member. */
static void test_mutation_set_removal(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    size_t full = live_of(context, "new Set(Array.from({length: 4000}, (_, i) => i))");
    size_t emptied = live_of(context, "Array.from({length: 3990}, (_, i) => i).reduce((s, x) => { s.delete(x); return s; }, new Set(Array.from({length: 4000}, (_, i) => i)))");
    size_t cleared = live_of(context, "(() => { const s = new Set(Array.from({length: 4000}, (_, i) => i)); s.clear(); return s; })()");
    size_t small = live_of(context, "new Set(Array.from({length: 10}, (_, i) => i))");
    CHECK(full > 4000 * 16);
    CHECK(emptied < full / 50);
    CHECK(emptied <= small + 512);
    CHECK(cleared <= small);
    char source[256];
    uint64_t fuel[4];
    static const char *const calls[4] = { "s.delete(999)", "s.delete(5000)", "s.delete(0)", "s.clear()" };
    for (int i = 0; i < 4; i++) {
        snprintf(source, sizeof source, "(() => { const s = new Set(Array.from({length: 1000}, (_, i) => i)); return %s; })()", calls[i]);
        fuel[i] = fuel_of(context, source);
    }
    CHECK(fuel[1] == fuel[0]);
    CHECK(fuel[2] - fuel[0] == 1000);
    /* clear has no argument to push, and costs 1,000 for 1 */
    CHECK(fuel[3] + 2u - fuel[0] == 1000);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* Every allocation of a program that removes from an indexed set until it
 * shrinks, and clears one, fails in turn: a shrink that cannot allocate
 * keeps the larger block. */
static void test_mutation_set_removal_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const s = Array.from({length: 40}, (_, i) => `k${i}`).reduce((t, k) => t.add(k), new Set());\n"
                       "  const d = Array.from({length: 36}, (_, i) => `k${i * 7 % 40}`).reduce((t, k) => { t.delete(k); return t; }, s);\n"
                       "  const before = [...d];\n"
                       "  d.add(\"k1\").add(\"z\");\n"
                       "  const has = [d.has(\"k1\"), d.has(\"k2\"), d.delete(\"z\")];\n"
                       "  d.clear();\n"
                       "  d.add(1);\n"
                       "  return [before, has, [...d]];\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* MUTATION: a map's delete and clear shrink it as a set's do, and its set
 * and delete cost a lookup through the index, and its entries when one
 * moves down. */
static void test_mutation_map_removal(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    size_t full = live_of(context, "new Map(Array.from({length: 4000}, (_, i) => [i, i]))");
    size_t emptied = live_of(context, "Array.from({length: 3990}, (_, i) => i).reduce((m, x) => { m.delete(x); return m; }, new Map(Array.from({length: 4000}, (_, i) => [i, i])))");
    size_t cleared = live_of(context, "(() => { const m = new Map(Array.from({length: 4000}, (_, i) => [i, i])); m.clear(); return m; })()");
    size_t small = live_of(context, "new Map(Array.from({length: 10}, (_, i) => [i, i]))");
    CHECK(full > 4000 * 32);
    CHECK(emptied < full / 50);
    /* A set's bound, for entries of two values. */
    CHECK(emptied <= small + 1024);
    CHECK(cleared <= small);
    char source[256];
    uint64_t fuel[6];
    static const char *const calls[6] = { "m.delete(999)", "m.delete(5000)", "m.delete(0)", "m.clear()", "m.set(5, 1)", "m.set(5000, 1)" };
    for (int i = 0; i < 6; i++) {
        snprintf(source, sizeof source, "(() => { const m = new Map(Array.from({length: 1000}, (_, i) => [i, i])); return %s; })()", calls[i]);
        fuel[i] = fuel_of(context, source);
    }
    /* The last entry, and a key the map lacks, cost one lookup; the first
     * moves the other 999 entries down and renumbers the index. */
    CHECK(fuel[1] == fuel[0]);
    CHECK(fuel[2] - fuel[0] == 1000);
    /* clear has no argument to push, and costs 1,000 for 1 */
    CHECK(fuel[3] + 2u - fuel[0] == 1000);
    /* set has one argument more than delete, and costs a lookup, whether
     * the key is there or goes in last. */
    CHECK(fuel[4] == fuel[0] + 1u && fuel[5] == fuel[4]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* MUTATION: every allocation of a program that sets and deletes in an
 * indexed map until it shrinks, and clears one, fails in turn. */
static void test_mutation_map_removal_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const m = Array.from({length: 40}, (_, i) => `k${i}`).reduce((t, k) => t.set(k, [k]), new Map());\n"
                       "  const d = Array.from({length: 36}, (_, i) => `k${i * 7 % 40}`).reduce((t, k) => { t.delete(k); return t; }, m);\n"
                       "  const before = [...d];\n"
                       "  d.set(\"k1\", 1).set(\"z\", 2);\n"
                       "  const has = [d.has(\"k1\"), d.get(\"k2\"), d.delete(\"z\")];\n"
                       "  d.clear();\n"
                       "  d.set(1, 1);\n"
                       "  return [before, has, [...d]];\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* delete is DELETE after its target, a node that starts with the word; it
 * sends no EXPRESSION_STATEMENT. */
static void test_mutation_delete_events(void) {
    expect_nodes("o => { delete o.a; delete o[k].b; }", SJS_GOAL_EXPRESSION, LOG_NODES,
                 "ARROW_BEGIN#1@0 PARAM#1@0 BIND#2@0 PARAMS_END#1@0 BLOCK_BEGIN#1@0 NAME#5@14 MEMBER#6@14 DELETE#4@7 NAME#9@26 "
                 "NAME#11@28 INDEX#10@26 MEMBER#12@26 DELETE#8@19 BLOCK_END#1@0 ARROW_END#1@0 END#0@0");
}

/* DELETE in an order no parser sends: outside a statement, or after a name
 * alone, which the parser refuses. */
static void test_mutation_delete_order(void) {
    static const char *const out_of_order = "the events are out of order";
    const sjs_event body[] = { { .kind = SJS_EV_ARROW_BEGIN, .count = 1 }, { .kind = SJS_EV_PARAM }, { .kind = SJS_EV_BIND, .text = SJS_STR("o") },
                               { .kind = SJS_EV_PARAMS_END }, { .kind = SJS_EV_BLOCK_BEGIN } };
    const sjs_event name = { .kind = SJS_EV_NAME, .text = SJS_STR("o"), .at = { 14, 1, 15 }, .start = { 14, 1, 15 } };
    const sjs_event member = { .kind = SJS_EV_MEMBER, .text = SJS_STR("k"), .at = { 16, 1, 17 }, .start = { 14, 1, 15 } };
    const sjs_event expression[] = { body[0], body[1], body[2], body[3], name, member, { .kind = SJS_EV_DELETE } };
    compile_events(expression, 7, out_of_order);
    const sjs_event bare[] = { body[0], body[1], body[2], body[3], body[4], name, { .kind = SJS_EV_DELETE } };
    compile_events(bare, 7, out_of_order);
}

/* An object that delete takes most keys from gives its memory back, its
 * index included. Deleting its last key costs one unit, as a missing one
 * does; any other, the object's length more. */
static void test_mutation_delete_removal(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    static const char *const build = "Object.fromEntries(Array.from({length: 4000}, (_, i) => [`k${i}`, i]))";
    char source[512];
    size_t full = live_of(context, build);
    snprintf(source, sizeof source, "Array.from({length: 3990}, (_, i) => `k${i}`).reduce((o, k) => { delete o[k]; return o; }, %s)", build);
    size_t emptied = live_of(context, source);
    size_t small = live_of(context, "Object.fromEntries(Array.from({length: 10}, (_, i) => [`k${i + 3990}`, i + 3990]))");
    CHECK(full > 4000 * 24);
    CHECK(emptied < full / 50);
    CHECK(emptied <= 2 * small);
    uint64_t fuel[3];
    static const char *const keys[3] = { "k999", "none", "k0" };
    for (int i = 0; i < 3; i++) {
        snprintf(source, sizeof source,
                 "(() => { const o = Object.fromEntries(Array.from({length: 1000}, (_, i) => [`k${i}`, i])); delete o.%s; return 0; })()", keys[i]);
        fuel[i] = fuel_of(context, source);
    }
    CHECK(fuel[1] == fuel[0]);
    CHECK(fuel[2] - fuel[0] == 1000);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* Every allocation of a program that deletes from objects, past the size
 * that indexes them and back, through a path, and adds again, fails in
 * turn. */
static void test_mutation_delete_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const o = Array.from({length: 20}, (_, i) => i).reduce((acc, i) => { acc[i % 3 ? `k${i}` : i] = [i]; return acc; }, {});\n"
                       "  const d = Array.from({length: 17}, (_, i) => i).reduce((acc, i) => { delete acc[i % 3 ? `k${i}` : i]; return acc; }, o);\n"
                       "  d.k17[1] = 1;\n"
                       "  d.n = {a: {b: 1, c: 2}};\n"
                       "  delete d.n.a.b;\n"
                       "  delete d.k19;\n"
                       "  d[1] = 1;\n"
                       "  return d;\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

/* MUTATION: methods through a path (docs/mutation.md, "Methods through a
 * path"). */

/* The rewrite is semantics, so no_shapes leaves it on: the same value, and
 * no allocation more, as the anchored lane keeps its first cells in the
 * matcher; with shapes, the matcher's records, for the arrows. */
static void test_mutation_path_no_shapes(void) {
    const char *source = "(() => { const o = {a: {b: [3, 1, 2]}, s: new Set()}; o.a.b.sort((x, y) => x - y).reverse(); "
                         "const p = o.a.b.pop(); o.s.add(p); const n = o.a.b.push(9); return [o, n]; })()";
    sjs_compiler_config on = { NULL, SJS_GOAL_EXPRESSION, NULL, 0, { NULL, NULL }, false };
    sjs_compiler_config off = on;
    off.no_shapes = true;
    compiled with = compile_counting(on, source);
    compiled without = compile_counting(off, source);
    CHECK(strcmp(with.out, "[{\"a\":{\"b\":[3,2,9]},\"s\":new Set([1])},3]") == 0 && strcmp(without.out, with.out) == 0);
    /* The comparator's marker and keys outgrow the first records. */
    CHECK(with.allocations == without.allocations + 2);
    /* A path nested past the lane's own cells allocates the same both
     * ways; with shapes, the records the arrow's open reserves besides. */
    const char *deep = "(() => { const o = {a: [[], []]}; const k = [0, 1]; "
                       "o.a[k[k[k[k[k[k[k[k[k[0]]]]]]]]]].push(1); return o; })()";
    with = compile_counting(on, deep);
    without = compile_counting(off, deep);
    CHECK(strcmp(with.out, "{\"a\":[[1],[]]}") == 0 && strcmp(without.out, with.out) == 0);
    CHECK(with.allocations == without.allocations + 1);
}

/* A method through a path costs a unit of fuel for each key, besides its
 * instructions (one per key pushed, the name's, the call's) and what the
 * method costs on a variable; a sort, the instruction that puts it back. */
static void test_mutation_path_fuel(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    uint64_t variable = fuel_of(context, "(() => { const xs = [1, 2]; return xs.pop(); })()");
    uint64_t one = fuel_of(context, "(() => { const o = {a: [1, 2]}; return o.a.pop(); })()");
    uint64_t three = fuel_of(context, "(() => { const o = {a: {b: {c: [1, 2]}}}; return o.a.b.c.pop(); })()");
    uint64_t sorts = fuel_of(context, "(() => { const o = {a: [2, 1]}; return o.a.sort((x, y) => (x < y ? -1 : 1)); })()");
    uint64_t sorted = fuel_of(context, "(() => { const xs = [2, 1]; return xs.sort((x, y) => (x < y ? -1 : 1)); })()");
    /* The object literal, the key pushed, the key's unit; PATH_METHOD and
     * PATH_CALL are METHOD_MUTATE and MUTATE. */
    CHECK(one - variable == 3);
    /* Two more object literals, two more keys pushed, two more units. */
    CHECK(three - one == 6);
    /* As one more key does; PATH_SORTED is SORTED. */
    CHECK(sorts - sorted == 3);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* A container a native holds, inside a root the function owns, is never
 * changed through a path: the call faults when it runs. One the native
 * gives away is the function's to change. */
static void test_mutation_path_natives(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[4096];
    keeper k = { 0 };
    k.held = number_array(context, (const double[]){ 1, 2 }, 2);
    sjs_value natives[2];
    CHECK(sjs_function_native(context, (sjs_native){ &k, &held_vtable }, &natives[0]) == SJS_OK);
    CHECK(sjs_function_native(context, (sjs_native){ NULL, &fresh_vtable }, &natives[1]) == SJS_OK);
    sjs_str names[2] = { SJS_STR("held"), SJS_STR("fresh") };
    env e = { context, names, natives, 2, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "(() => { const o = {xs: held()}; o.xs.push(3); return o; })()", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "fault") == out && strstr(out, "held elsewhere") != NULL);
    CHECK(evaluate(&e, "(() => { const o = {a: {xs: held()}}; o.a.xs.sort((x, y) => y - x); return o; })()", out) == SJS_ERR_PROGRAM);
    CHECK(strstr(out, "held elsewhere") != NULL);
    write_text(context, k.held, out, sizeof out);
    CHECK(strcmp(out, "[1,2]") == 0);
    CHECK(evaluate(&e, "(() => { const o = {xs: fresh()}; o.xs.push(1); o.xs.unshift(0); return o; })()", out) == SJS_OK);
    CHECK(strcmp(out, "{\"xs\":[0,1]}") == 0);
    sjs_release(context, k.held);
    for (int i = 0; i < 2; i++) sjs_release(context, natives[i]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* Every allocation of a program that grows arrays and a set through paths,
 * past the size that gives the set an index, sorts and chains through one,
 * and nests a path past the anchored lane's own cells, fails in turn. */
static void test_mutation_path_out_of_memory(void) {
    test_out_of_memory("(() => {\n"
                       "  const g = [5, 3, 9, 1, 7, 2, 8, 6, 4, 0].reduce((acc, x) => { acc.by[x % 2].push(x); acc.s.tags.add(x % 13); return acc; },\n"
                       "                                                 {by: [[], []], s: {tags: new Set()}});\n"
                       "  g.by[0].sort((a, b) => a - b).fill(0, 3).reverse();\n"
                       "  const p = g.by[1].pop();\n"
                       "  const q = g.by[1].splice(0, 2, \"x\", \"y\", \"z\");\n"
                       "  const k = [0, 1];\n"
                       "  g.by[k[k[k[k[k[k[k[k[k[1]]]]]]]]]].unshift(-1);\n"
                       "  return [g.by, [...g.s.tags], p, q];\n"
                       "})()",
                       SJS_GOAL_EXPRESSION);
}

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

/* STRING_APPEND: +, a template and concat extend a string nothing else
 * holds, so a string built a piece at a time, as reduce's accumulator,
 * takes O(log n) allocations, not one per piece, and holds at most twice its
 * length. */
static void test_string_append_memory(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    static const char *const callbacks[] = { "(acc, x) => acc + x", "(acc, x) => `${acc}${x}`", "(acc, x) => acc + x + \"\"",
                                              "(acc, x) => acc.concat(x)" };
    static const int sizes[2] = { 1000, 100000 };
    char source[256];
    for (int f = 0; f < 4; f++) {
        long allocations[2];
        size_t live[2];
        for (int i = 0; i < 2; i++) {
            snprintf(source, sizeof source, "Array.from({length: %d}, () => \"x\").reduce(%s, \"\")", sizes[i], callbacks[f]);
            allocations[i] = allocations_of(&c, context, source);
            live[i] = live_of(context, source);
        }
        if (getenv("SJS_TEST_VERBOSE") != NULL) {
            printf("%s: allocations %ld -> %ld, live %zu -> %zu\n", callbacks[f], allocations[0], allocations[1], live[0], live[1]);
        }
        /* From 1,000 to 100,000 pieces: 7 more doublings, not 99,000 copies. */
        CHECK(allocations[1] - allocations[0] <= 10);
        CHECK(live[1] >= (size_t)sizes[1] && live[1] <= 2u * (size_t)sizes[1] + 256u);
    }
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

static sjs_result interned_key_object(void *self, sjs_native_call *call) {
    (void)self;
    sjs_result r = sjs_object_new(call->context, 1, &call->result);
    sjs_property p = { SJS_STR("ab"), sjs_number(1) };
    return r == SJS_OK ? sjs_object_put(call->context, &call->result, &p) : r;
}

static const sjs_native_vtable interned_key_vtable = { interned_key_object };

/* STRING_APPEND: an interned string is found by its text, so one that
 * nothing else holds is copied, not extended: here a key put through the
 * API, read out of a temporary object. The table still finds the key. */
static void test_string_append_interned(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    char out[256];
    sjs_value native;
    CHECK(sjs_function_native(context, (sjs_native){ NULL, &interned_key_vtable }, &native) == SJS_OK);
    sjs_str names[1] = { SJS_STR("o") };
    env e = { context, names, &native, 1, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "Object.keys(o())[0] + \"c\"", out) == SJS_OK && strcmp(out, "\"abc\"") == 0);
    CHECK(evaluate(&e, "`${Object.keys(o())[0]}c${1}`", out) == SJS_OK && strcmp(out, "\"abc1\"") == 0);
    sjs_value kept;
    CHECK(sjs_object_new(context, 1, &kept) == SJS_OK);
    sjs_property p = { SJS_STR("ab"), sjs_number(2) };
    CHECK(sjs_object_put(context, &kept, &p) == SJS_OK);
    CHECK(evaluate(&e, "[Object.keys(o())[0] + \"c\", Object.keys(o())[0]]", out) == SJS_OK && strcmp(out, "[\"abc\",\"ab\"]") == 0);
    sjs_property a, b;
    sjs_value fresh;
    CHECK(sjs_object_new(context, 1, &fresh) == SJS_OK);
    CHECK(sjs_object_put(context, &fresh, &p) == SJS_OK);
    CHECK(sjs_object_at(kept, 0, &a) == SJS_OK && sjs_object_at(fresh, 0, &b) == SJS_OK && a.key.ptr == b.key.ptr);
    sjs_release(context, fresh);
    sjs_release(context, kept);
    sjs_release(context, native);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* MUTATION: appending to a let (docs/mutation.md, "Appending to a let"):
 * s += e, s = s + e1 + e2 and s = `${s}...` move the let's value to +, so
 * a string built by them takes O(log n) allocations and holds at most twice
 * its length. */
static void test_mutation_append_memory(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    static const char *const bodies[] = {
        "{ let s = acc; s += x; return s; }",
        "{ let s = acc; s = s + x + \"\"; return s; }",
        "{ let s = acc; s = `${s}${x}`; return s; }",
        "{ let s = acc; if (x) { s += x; } return s; }",
    };
    static const int sizes[2] = { 1000, 100000 };
    char source[256];
    for (int f = 0; f < 4; f++) {
        long allocations[2];
        size_t live[2];
        for (int i = 0; i < 2; i++) {
            snprintf(source, sizeof source, "Array.from({length: %d}, () => \"x\").reduce((acc, x) => %s, \"\")", sizes[i], bodies[f]);
            allocations[i] = allocations_of(&c, context, source);
            live[i] = live_of(context, source);
        }
        if (getenv("SJS_TEST_VERBOSE") != NULL) {
            printf("%s: allocations %ld -> %ld, live %zu -> %zu\n", bodies[f], allocations[0], allocations[1], live[0], live[1]);
        }
        CHECK(allocations[1] - allocations[0] <= 10);
        CHECK(live[1] >= (size_t)sizes[1] && live[1] <= 2u * (size_t)sizes[1] + 256u);
    }
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);
}

/* ------------------------------------------------------------------------
 * BigInts through the API: made from text, read as text, written, and the
 * limit on their size. */

static void test_bigint_api(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_value v;
    CHECK(sjs_bigint_new(context, SJS_STR(" -123456789012345678901234567890\n"), &v) == SJS_OK);
    CHECK(v.kind == SJS_BIGINT);
    char text[64];
    sjs_buffer b = { (uint8_t *)text, sizeof text, 0 };
    CHECK(sjs_bigint_text_bound(v) >= 31u && sjs_bigint_text_bound(v) <= 34u);
    CHECK(sjs_bigint_text(context, v, &b) == SJS_OK);
    CHECK(b.length == 31u && memcmp(text, "-123456789012345678901234567890", 31) == 0);
    sjs_buffer small = { (uint8_t *)text, 10, 0 };
    CHECK(sjs_bigint_text(context, v, &small) == SJS_NEED_SPACE && small.length == 0);
    char out[4096];
    write_text(context, v, out, sizeof out);
    CHECK(strcmp(out, "-123456789012345678901234567890n") == 0);
    /* The writer resumes a BigInt's digits a byte at a time. */
    sjs_writer *w;
    CHECK(sjs_writer_init(&w, &(sjs_writer_config){ context, v, { 0 } }) == SJS_OK);
    char one[64];
    uint32_t got = 0;
    sjs_result r;
    do {
        sjs_buffer tiny = { (uint8_t *)one + got, 1, 0 };
        r = sjs_writer_write(w, &tiny);
        got += tiny.length;
    } while (r == SJS_NEED_SPACE && got < sizeof one - 1u);
    CHECK(r == SJS_OK && got == 32u && memcmp(one, "-123456789012345678901234567890n", 32) == 0);
    sjs_writer_destroy(w);
    sjs_release(context, v);
    CHECK(sjs_bigint_new(context, SJS_STR("0x"), &v) == SJS_ERR_INVALID);
    CHECK(sjs_bigint_new(context, SJS_STR("-0x1"), &v) == SJS_ERR_INVALID);
    CHECK(sjs_bigint_new(context, SJS_STR("1n"), &v) == SJS_ERR_INVALID);
    CHECK(sjs_bigint_new(context, SJS_STR("  "), &v) == SJS_OK);
    b.length = 0;
    CHECK(sjs_bigint_text(context, v, &b) == SJS_OK && b.length == 1u && text[0] == '0');
    sjs_release(context, v);
    CHECK(sjs_bigint_text_bound(sjs_number(1)) == 0);
    CHECK(sjs_bigint_text(context, sjs_number(1), &b) == SJS_ERR_INVALID);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* max_bigint_bits: 2^64 - 1 fits 64 bits, 2^64 does not. */
    sjs_limits limits = SJS_LIMITS_DEFAULT;
    limits.max_bigint_bits = 64;
    context = new_context(&c, limits);
    CHECK(sjs_bigint_new(context, SJS_STR("18446744073709551615"), &v) == SJS_OK);
    sjs_release(context, v);
    CHECK(sjs_bigint_new(context, SJS_STR("0xffffffffffffffff"), &v) == SJS_OK);
    sjs_release(context, v);
    CHECK(sjs_bigint_new(context, SJS_STR("18446744073709551616"), &v) == SJS_ERR_LIMIT);
    CHECK(sjs_bigint_new(context, SJS_STR("0x10000000000000000"), &v) == SJS_ERR_LIMIT);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "18446744073709551615n", out) == SJS_OK && strcmp(out, "18446744073709551615n") == 0);
    CHECK(evaluate(&e, "18446744073709551616n", out) == SJS_ERR_PROGRAM && strncmp(out, "error 4", 7) == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    /* Running out of memory while writing one. */
    for (long fail = 1; fail < 8; fail++) {
        counting f = { 0 };
        context = new_context(&f, SJS_LIMITS_DEFAULT);
        f.fail_at = 0;
        if (sjs_bigint_new(context, SJS_STR("123456789012345678901234567890"), &v) != SJS_OK) {
            CHECK(false);
            break;
        }
        f.fail_at = f.allocations + fail;
        write_text(context, v, out, sizeof out);
        b.length = 0;
        (void)sjs_bigint_text(context, v, &b);
        f.fail_at = 0;
        sjs_release(context, v);
        CHECK(sjs_context_live_bytes(context) == 0);
        sjs_context_destroy(context);
        CHECK(f.live == 0);
    }
}

/* The fuel of `m.get(k)` and `m.has(k)` with the map `m` given as a global. */
static uint64_t map_lookup_fuel(sjs_context *context, sjs_value map) {
    static const sjs_str names[1] = { { "m", 1 } };
    const char *source = "[m.get(3), m.has(-1), m.get(\"3\")]";
    sjs_program *program;
    sjs_compile_params params = { { context, SJS_GOAL_EXPRESSION, names, 1, { NULL, NULL }, false },
                                  { source, (uint32_t)strlen(source) }, NULL };
    if (sjs_compile(&params, &program) != SJS_OK) return 0;
    sjs_vm *vm;
    sjs_vm_config config = { context, false, false };
    CHECK(sjs_vm_init(&vm, &config) == SJS_OK);
    sjs_run_params run = { program, &map, 1 };
    CHECK(sjs_vm_start(vm, &run) == SJS_OK);
    CHECK(sjs_vm_run(vm, UINT64_MAX) == SJS_RUN_DONE);
    uint64_t used = sjs_vm_fuel_used(vm);
    sjs_value v;
    CHECK(sjs_vm_take_result(vm, &v) == SJS_OK);
    char out[64];
    write_text(context, v, out, sizeof out);
    CHECK(strcmp(out, "[9,false,undefined]") == 0);
    sjs_release(context, v);
    sjs_vm_destroy(vm);
    sjs_program_release(program);
    return used;
}

/* Maps through the API: built, read, written, given to a program, and their
 * lookups one unit of fuel through the index whatever their size. */
static void test_map_api(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    sjs_value map;
    CHECK(sjs_map_new(context, 0, &map) == SJS_OK && map.kind == SJS_MAP);
    sjs_value s;
    CHECK(sjs_string_new(context, SJS_STR("v"), &s) == SJS_OK);
    sjs_map_entry e1 = { sjs_number(1), s };
    CHECK(sjs_map_set(context, &map, &e1) == SJS_OK);
    CHECK(sjs_string_new(context, SJS_STR("k"), &s) == SJS_OK);
    sjs_map_entry e2 = { s, sjs_boolean(true) };
    CHECK(sjs_map_set(context, &map, &e2) == SJS_OK);
    sjs_map_entry e3 = { sjs_number(-0.0), sjs_null() };
    CHECK(sjs_map_set(context, &map, &e3) == SJS_OK);
    /* A key it has keeps its place and takes the value. */
    sjs_map_entry e4 = { sjs_number(1), sjs_number(2) };
    CHECK(sjs_map_set(context, &map, &e4) == SJS_OK);
    CHECK(sjs_map_size(map) == 3 && sjs_length(map) == 3);
    char text[4096];
    write_text(context, map, text, sizeof text);
    CHECK(strcmp(text, "new Map([[1,2],[\"k\",true],[0,null]])") == 0);
    sjs_value got;
    CHECK(sjs_map_get(map, sjs_number(0), &got) == SJS_OK && got.kind == SJS_NULL);
    CHECK(sjs_map_get(map, sjs_number(1), &got) == SJS_OK && got.as.number == 2);
    CHECK(sjs_map_get(map, sjs_number(3), &got) == SJS_ERR_NOT_FOUND);
    sjs_map_entry at;
    CHECK(sjs_map_at(map, 1, &at) == SJS_OK && at.key.kind == SJS_STRING && at.value.kind == SJS_BOOLEAN);
    CHECK(sjs_map_at(map, 2, &at) == SJS_OK && at.key.kind == SJS_NUMBER && at.key.as.number == 0);
    CHECK(sjs_map_at(map, 3, &at) == SJS_ERR_NOT_FOUND);
    /* Only a primitive is a key; the caller keeps its reference. */
    sjs_value array;
    CHECK(sjs_array_new(context, 0, &array) == SJS_OK);
    sjs_map_entry bad = { array, sjs_number(1) };
    CHECK(sjs_map_set(context, &map, &bad) == SJS_ERR_INVALID);
    sjs_release(context, array);
    /* A map someone else holds cannot change. */
    sjs_retain(map);
    CHECK(sjs_map_set(context, &map, &e3) == SJS_ERR_INVALID);
    sjs_release(context, map);
    /* Other values are no maps. */
    CHECK(sjs_map_size(sjs_number(1)) == 0);
    CHECK(sjs_map_get(sjs_number(1), sjs_number(1), &got) == SJS_ERR_INVALID);
    CHECK(sjs_map_at(sjs_null(), 0, &at) == SJS_ERR_INVALID);
    CHECK(sjs_map_set(context, &array, &e3) == SJS_ERR_INVALID);
    env e = { context, (const sjs_str[]){ { "m", 1 } }, &map, 1, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    CHECK(evaluate(&e, "[m.get(1), m.get(\"k\"), m.has(-0), m.size, [...m]]", text) == SJS_OK);
    CHECK(strcmp(text, "[2,true,true,3,[[1,2],[\"k\",true],[0,null]]]") == 0);
    CHECK(evaluate(&e, "new Map(m)", text) == SJS_OK && strcmp(text, "new Map([[1,2],[\"k\",true],[0,null]])") == 0);
    sjs_release(context, map);

    /* A lookup is one unit through the index: a map of 5,000 entries costs
     * what one of 10 does. */
    sjs_value maps[2];
    for (uint32_t m = 0; m < 2; m++) {
        uint32_t n = m == 0 ? 10u : 5000u;
        CHECK(sjs_map_new(context, n, &maps[m]) == SJS_OK);
        for (uint32_t i = 0; i < n; i++) {
            sjs_map_entry entry = { sjs_number(i), sjs_number(i * 3.0) };
            CHECK(sjs_map_set(context, &maps[m], &entry) == SJS_OK);
        }
    }
    uint64_t small = map_lookup_fuel(context, maps[0]);
    CHECK(small != 0 && small == map_lookup_fuel(context, maps[1]));
    sjs_release(context, maps[0]);
    sjs_release(context, maps[1]);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);
    CHECK(c.live == 0);

    /* Building one and writing it, each allocation failing in turn. */
    for (long fail = 1; fail < 40; fail++) {
        counting f = { 0 };
        context = new_context(&f, SJS_LIMITS_DEFAULT);
        f.fail_at = fail;
        sjs_value m;
        if (sjs_map_new(context, 0, &m) == SJS_OK) {
            for (uint32_t i = 0; i < 12; i++) {
                sjs_value key;
                if (sjs_string_new(context, (sjs_str){ &"abcdefghijkl"[i], 1 }, &key) != SJS_OK) continue;
                sjs_map_entry entry = { key, sjs_number(i) };
                if (sjs_map_set(context, &m, &entry) != SJS_OK) sjs_release(context, key);
            }
            write_text(context, m, text, sizeof text);
            sjs_release(context, m);
        }
        CHECK(sjs_context_live_bytes(context) == 0);
        sjs_context_destroy(context);
        CHECK(f.live == 0);
    }
}

/* The shapes' C paths run for Map.groupBy and an iterator's map, filter and
 * reduce, which run as the array's loop of their name: the same value, and
 * per element what the array's loop saves. */
static void test_map_shape_paths(void) {
    static const struct {
        const char *source;
        const char *out;
        uint64_t saved;    /* per element */
        uint64_t elements;
    } cases[] = {
        /* A pluck: 4 units in bytecode (the step, TAKE_LOCAL, GET_MEMBER,
         * RETURN), 2 in C (the step and the lookup). */
        { "Map.groupBy([{k: 3}, {k: 1}, {}, {k: 3}], x => x.k)",
          "new Map([[3,[{\"k\":3},{\"k\":3}]],[1,[{\"k\":1}]],[undefined,[{}]]])", 2, 4 },
        { "new Map([[1, {k: 3}], [2, {k: 1}], [3, {}]]).values().map(x => x.k)", "[3,1,undefined].values()", 2, 3 },
        /* A predicate on the element: 5 units in bytecode (the step,
         * TAKE_LOCAL, NUMBER_, BINARY, RETURN), 1 in C. */
        { "new Map([[1, 1], [2, 2], [3, 3], [4, 4]]).keys().filter(x => x > 2)", "[3,4].values()", 4, 4 },
        /* A sum: 5 units in bytecode (the step, TAKE_LOCAL, TAKE_LOCAL,
         * BINARY, RETURN), 1 in C. */
        { "new Map([[1, 1], [2, 2], [3, 3], [4, 4]]).values().reduce((a, x) => a + x, 0.5)", "10.5", 4, 4 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        shaped_run on = run_shaped((shaped_source){ cases[i].source, false, SJS_LIMITS_DEFAULT });
        shaped_run off = run_shaped((shaped_source){ cases[i].source, true, SJS_LIMITS_DEFAULT });
        CHECK(strcmp(on.out, cases[i].out) == 0 && strcmp(off.out, on.out) == 0);
        CHECK(off.fuel - on.fuel == cases[i].saved * cases[i].elements);
        if (off.fuel - on.fuel != cases[i].saved * cases[i].elements || strcmp(on.out, cases[i].out) != 0) {
            fprintf(stderr, "  %s: %s, fuel %llu with shapes, %llu without\n", cases[i].source, on.out,
                    (unsigned long long)on.fuel, (unsigned long long)off.fuel);
        }
    }
}

/* JSON.parse and JSON.stringify: fuel a unit per 16 bytes and per value,
 * pinned; their limits; and every allocation failing in turn. */
static void test_json(void) {
    counting c = { 0 };
    sjs_context *context = new_context(&c, SJS_LIMITS_DEFAULT);
    /* The instructions (4 for the parse, 15 with the array literal built),
     * then a unit per value (7 made; 7 visited) and per 16 bytes (the
     * 34-byte text, 2; the 27 bytes written, 1). */
    CHECK(fuel_of(context, "JSON.parse('[1, \"two\", {\"k\": [true]}, null]   ')") == 4 + 7 + 2);
    CHECK(fuel_of(context, "JSON.stringify([1, \"two\", {\"k\": [true]}, null])") == 15 + 7 + 1);
    /* An index key put before the index keys an object has moves them,
     * a unit each. */
    CHECK(fuel_of(context, "JSON.parse('{\"3\":0,\"2\":0,\"1\":0}')") == fuel_of(context, "JSON.parse('{\"1\":0,\"2\":0,\"3\":0}')") + 3);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    sjs_limits limits = SJS_LIMITS_DEFAULT;
    limits.max_string_bytes = 32;
    limits.max_nesting = 8;
    limits.max_value_items = 4;
    context = new_context(&c, limits);
    env e = { context, NULL, NULL, 0, { NULL, NULL }, SJS_GOAL_EXPRESSION };
    char out[4096];
    static const char *const over[] = {
        /* Output past max_string_bytes. */
        "JSON.stringify([\"0123456789\", \"0123456789\", \"0123456789\"])",
        "JSON.stringify([[1, 2, 3, 4], [1, 2, 3, 4]], null, 10)",
        /* Members left out count toward it, so one call's work is bounded. */
        "(o => JSON.stringify([[o, o, o, o], [o, o, o, o]]))({a: undefined, b: undefined, c: undefined, d: undefined})",
        /* Past max_nesting and max_value_items. */
        "JSON.parse(\"[[[[[[[[[]]]]]]]]]\")",
        "JSON.parse(\"[1,2,3,4,5]\")",
        "JSON.parse('{\"a\":1,\"b\":2,\"c\":3,\"d\":4,\"e\":5}')",
    };
    for (size_t i = 0; i < sizeof over / sizeof over[0]; i++) {
        CHECK(evaluate(&e, over[i], out) == SJS_ERR_PROGRAM);
        CHECK(strncmp(out, "fault 4", 7) == 0);
        CHECK(sjs_context_live_bytes(context) == 0);
    }
    CHECK(evaluate(&e, "JSON.stringify([\"0123456789\", \"0123456789\"])", out) == SJS_OK);
    CHECK(evaluate(&e, "JSON.parse(\"[[[[[[[[]]]]]]]]\").length", out) == SJS_OK && strcmp(out, "1") == 0);
    CHECK(sjs_context_live_bytes(context) == 0);
    sjs_context_destroy(context);

    test_out_of_memory("[JSON.parse(' {\"b\": [1, -0.5e3, \"x\\\\u00e9\\\\n\", {\"2\": null, \"1\": true, \"2\": [] }], \"a\": {}} '),\n"
                       " JSON.stringify({a: [1, \"q\\\"\", {b: undefined, c: new Map()}], d: \"\\u0001\"}, [\"a\", \"c\", \"d\", 1], \"--\")]",
                       SJS_GOAL_EXPRESSION);
}

int main(int argc, char **argv) {
    bool full = argc > 1 && strcmp(argv[1], "--full") == 0;
    test_values();
    test_interned_keys();
    test_natives_and_globals();
    test_unbound_where_it_never_runs();
    test_modules();
    test_frozen_values();
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
    test_host_clock();
    test_now_reads();
    test_host_seed();
    test_limits();
    test_loop_callbacks();
    test_reuse_leaves_others_alone();
    test_no_shapes();
    test_shape_paths();
    /* MUTATION */
    test_mutation_events();
    test_mutation_push_paths();
    test_mutation_natives();
    test_mutation_fuel();
    test_mutation_out_of_memory();
    test_mutation_assign_events();
    test_mutation_assign_order();
    test_mutation_assign_natives();
    test_mutation_assign_fuel();
    test_mutation_assign_out_of_memory();
    test_mutation_let_events();
    test_mutation_let_out_of_memory();
    test_mutation_shrink_array();
    test_mutation_set_removal();
    test_mutation_set_removal_out_of_memory();
    test_mutation_map_removal();
    test_mutation_map_removal_out_of_memory();
    test_mutation_splice_fuel();
    test_mutation_splice_out_of_memory();
    test_mutation_delete_events();
    test_mutation_delete_order();
    test_mutation_delete_removal();
    test_mutation_delete_out_of_memory();
    /* MUTATION: methods through a path. */
    test_mutation_path_no_shapes();
    test_mutation_path_fuel();
    test_mutation_path_natives();
    test_mutation_path_out_of_memory();
    test_mutation_append_memory();
    /* STRING_APPEND */
    test_string_append_memory();
    test_string_append_interned();
    test_bigint_api();
    test_map_api();
    test_map_shape_paths();
    test_json();
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
        test_out_of_memory("const m = new Map([[1, \"a\"], [2, [1]], [1, \"c\"]]);\n"
                           "export const xs = [...m, ...new Map(m)];\n"
                           "export const o = Object.fromEntries(m);\n"
                           "export const [[k, v]] = m;\n",
                           SJS_GOAL_MODULE);
        test_out_of_memory("const m = new Map([[1, {k: \"a\"}], [2, {k: \"b\"}], [3, {k: \"a\"}]]);\n"
                           "export const g = Map.groupBy([...m.values()], v => v.k);\n"
                           "export const xs = m.values().map(v => v.k).filter(k => k === \"a\").toArray();\n"
                           "export const s = m.keys().reduce((a, x) => a + x, 0);\n"
                           "export const it = m.entries();\n"
                           "export const f = m.forEach(v => v.k);\n",
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
