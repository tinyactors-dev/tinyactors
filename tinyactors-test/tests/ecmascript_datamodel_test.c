#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "allocator.h"
#include "ecmascript_datamodel.h"
#include "byte_buffers.h"

#define ARRAY_COUNT(a) (sizeof(a) / sizeof((a)[0]))
#define S(text) ((ta_str){.ptr = (text), .len = (uint32_t)(sizeof(text) - 1)})

static int failures = 0;

static void check(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "ecmascript datamodel test failed: %s\n", message);
        failures += 1;
    }
}

static void require(bool condition, const char *message) {
    if (!condition) {
        fprintf(stderr, "ecmascript datamodel test failed: %s\n", message);
        exit(1);
    }
}

/* ------------------------------------------------------------------------
 * A counting allocator over libc, to prove teardown frees everything.
 */

static size_t outstanding = 0;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    ta_allocator libc = tat_libc_allocator();
    void *ptr = libc.v->alloc(libc.self, size, alignment);
    if (ptr != NULL) {
        outstanding += size;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    ta_allocator libc = tat_libc_allocator();
    outstanding -= params->size;
    libc.v->free(libc.self, params);
}

static const ta_allocator_vtable COUNTING = {.alloc = counting_alloc, .free = counting_free};

/* ------------------------------------------------------------------------
 * Expressions, indexed by enum.
 */

#define EXPRESSIONS(X)                                                                  \
    X(E_ONE, "1")                                                                       \
    X(E_TWO, "2")                                                                       \
    X(E_THREE, "3")                                                                     \
    X(E_FIVE, "5")                                                                      \
    X(E_SEVEN, "7")                                                                     \
    X(E_VAR1, "var1")                                                                   \
    X(E_VAR1_UNDEFINED, "var1 == undefined")                                            \
    X(E_PREINCREMENT, "++var1==2")                                                      \
    X(E_VAR1_IS_2, "var1 === 2")                                                        \
    X(E_FOO_STRING, "'foo'")                                                            \
    X(E_ZERO, "0")                                                                      \
    X(E_EMPTY_STRING, "''")                                                             \
    X(E_UNDEFINED, "undefined")                                                         \
    X(E_FUNCTION, "function(invar) {return invar + 1;}")                                \
    X(E_CALL_FN, "fn(2) == 3")                                                          \
    X(E_SCRIPT, "function testobject() {\n this.bar = 0;}\n")                          \
    X(E_NEW_TESTOBJECT, "new testobject();")                                           \
    X(E_FOO, "foo")                                                                     \
    X(E_FOO_BAR, "foo.bar")                                                             \
    X(E_FOO_BAR_IS_1, "foo.bar == 1")                                                   \
    X(E_NOPE, "nope")                                                                   \
    X(E_EVENT, "_event")                                                                \
    X(E_OBJECT_LITERAL, "{a: 1, b: [1, 2]}")                                            \
    X(E_SYNTAX_ERROR, "1 +")                                                            \
    X(E_LOOP, "while (true) {}")                                                        \
    X(E_X, "x")                                                                         \
    X(E_X_IS_ARRAY, "x instanceof Array")                                               \
    X(E_X_TEXT, "x === 'this is a string'")                                             \
    X(E_VAR4, "Var4")                                                                   \
    X(E_ARRAY, "[1,2,3]")                                                               \
    X(E_FOREACH_DONE, "Var2 === 3 && Var3 === 2")                                       \
    X(E_OBJECT, "({n: 1})")                                                             \
    X(E_OBJ, "obj")                                                                     \
    X(E_OBJ_N, "obj.n")                                                                 \
    X(E_SHARED_SEES_5, "o2.n === 5")                                                    \
    X(E_CROSS_REALM, "Array.isArray(y) && !(y instanceof Array)")                       \
    X(E_LOSSY, "({u: undefined, f: function () {}, n: 1})")                             \
    X(E_CYCLE, "(function () { var o = {}; o.o = o; return o; })()")                    \
    X(E_P_IS_5, "p === 5")                                                              \
    X(E_EVENT_UNDEFINED, "typeof _event === 'undefined' && typeof _sessionid === 'undefined'") \
    X(E_IN, "In('s1')")                                                                 \
    X(E_ASSIGN_EVENT_THROWS, "(function () { try { _event = 1; return false; } catch (e) { return true; } })()")

enum {
#define X(name, source) name,
    EXPRESSIONS(X)
#undef X
        E_COUNT
};

static ta_expression expressions[E_COUNT];

static void setup_expressions(void) {
    static const char *const sources[] = {
#define X(name, source) source,
        EXPRESSIONS(X)
#undef X
    };
    for (size_t index = 0; index < E_COUNT; index += 1) {
        expressions[index] = (ta_expression){
            .kind = TA_EXPRESSION_SOURCE,
            .as.source = {.ptr = sources[index], .len = (uint32_t)strlen(sources[index])},
        };
    }
}

/* ------------------------------------------------------------------------
 * Helpers over the vtable, with no ta_dm_host (system variables unbound).
 */

typedef struct {
    ta_datamodel model;
    void *shared;
    void *instance;
} session;

static ta_datamodel model;
static void *shared;

static session open_session(const ta_value *binding, ta_result *result) {
    session s = {.model = model, .shared = shared};
    s.instance = calloc(1, model.v->instance_size(model.self, shared));
    require(s.instance != NULL, "instance allocation");
    *result = model.v->init_instance(model.self, &(ta_datamodel_init_instance_params){
                                                     .shared = shared, .instance = s.instance, .binding = binding});
    return s;
}

static void close_session(session *s) {
    s->model.v->destroy_instance(s->model.self, s->shared, s->instance);
    free(s->instance);
    s->instance = NULL;
}

static ta_op_status declare(session *s, ta_str name, bool *bound) {
    ta_fault fault = {0};
    return s->model.v->declare(s->model.self, &(ta_datamodel_declare_params){.shared = s->shared,
                                                                              .instance = s->instance,
                                                                              .name = name,
                                                                              .fault = &fault,
                                                                              .bound = bound});
}

static ta_op_status eval(session *s, ta_expr_id expr, ta_value *out) {
    ta_fault fault = {0};
    *out = (ta_value){0};
    return s->model.v->eval_value(s->model.self, &(ta_datamodel_eval_value_params){
                                                     .shared = s->shared, .instance = s->instance, .expr = expr,
                                                     .out = out, .fault = &fault});
}

static ta_op_status cond(session *s, ta_expr_id expr, bool *out) {
    ta_fault fault = {0};
    *out = false;
    return s->model.v->eval_condition(s->model.self, &(ta_datamodel_eval_condition_params){
                                                         .shared = s->shared, .instance = s->instance,
                                                         .expr = expr, .out = out, .fault = &fault});
}

static bool holds(session *s, ta_expr_id expr) {
    bool out = false;
    return cond(s, expr, &out) == TA_OP_OK && out;
}

static ta_op_status set_named(session *s, ta_str name, const ta_value *value) {
    ta_fault fault = {0};
    return s->model.v->set_named(s->model.self, &(ta_datamodel_set_named_params){
                                                    .shared = s->shared, .instance = s->instance, .name = name,
                                                    .value = value, .fault = &fault});
}

/* set_named(name, eval(expr)). */
static ta_op_status set_eval(session *s, ta_str name, ta_expr_id expr) {
    ta_value value;
    ta_op_status status = eval(s, expr, &value);
    if (status == TA_OP_OK) {
        status = set_named(s, name, &value);
        ta_value_release(&value);
    }
    return status;
}

static ta_op_status assign(session *s, ta_expr_id location, ta_expr_id expr) {
    ta_value value;
    ta_op_status status = eval(s, expr, &value);
    if (status != TA_OP_OK) {
        return status;
    }
    ta_fault fault = {0};
    status = s->model.v->assign(s->model.self, &(ta_datamodel_assign_params){
                                                   .shared = s->shared, .instance = s->instance,
                                                   .location = location, .value = &value, .fault = &fault});
    ta_value_release(&value);
    return status;
}

static ta_op_status script(session *s, ta_expr_id source) {
    ta_fault fault = {0};
    return s->model.v->execute_script(s->model.self, &(ta_datamodel_execute_script_params){
                                                         .shared = s->shared, .instance = s->instance,
                                                         .script = source, .fault = &fault});
}

/* True when value's wire JSON equals `expected`. */
static bool json_is(const ta_value *value, const char *expected) {
    tat_js_json json = {0};
    if (tat_js_value_to_json(value, &json) != TA_OK) {
        return false;
    }
    bool same = json.len == strlen(expected) && memcmp(json.ptr, expected, json.len) == 0;
    if (!same) {
        fprintf(stderr, "  wire JSON %.*s, expected %s\n", (int)json.len, json.ptr, expected);
    }
    tat_js_json_release(&json);
    return same;
}

static bool eval_json_is(session *s, ta_expr_id expr, const char *expected) {
    ta_value value;
    if (eval(s, expr, &value) != TA_OP_OK) {
        return false;
    }
    bool same = json_is(&value, expected);
    ta_value_release(&value);
    return same;
}

static bool text_is(session *s, const ta_value *value, const char *expected) {
    ta_fault fault = {0};
    ta_str text = {0};
    if (s->model.v->value_text(s->model.self, &(ta_datamodel_value_text_params){
                                                  .shared = s->shared, .instance = s->instance, .value = value,
                                                  .out = &text, .fault = &fault}) != TA_OP_OK) {
        return false;
    }
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

static ta_op_status import_content(session *s, const char *content, ta_value *out) {
    ta_fault fault = {0};
    return s->model.v->import_content(s->model.self, &(ta_datamodel_import_content_params){
                                                         .shared = s->shared, .instance = s->instance,
                                                         .content = {.ptr = (const uint8_t *)content,
                                                                     .len = (uint32_t)strlen(content)},
                                                         .out = out, .fault = &fault});
}

/* A value of no data model the adapter knows. */
static void foreign_retain(void *self) { (void)self; }
static void foreign_release(void *self) { (void)self; }
static const ta_value_vtable FOREIGN = {.retain = foreign_retain, .release = foreign_release};

/* ------------------------------------------------------------------------
 * Operations with no host
 */

static void test_operations(tat_js_datamodel_context *context) {
    ta_result result;
    session s = open_session(NULL, &result);
    require(result == TA_OK, "instance initialization");

    /* declare → undefined; ++ side effects in a condition (444, 445). */
    check(declare(&s, S("var1"), NULL) == TA_OP_OK, "declare var1");
    check(holds(&s, E_VAR1_UNDEFINED), "declared data is undefined");
    check(set_eval(&s, S("var1"), E_ONE) == TA_OP_OK, "set_named var1");
    check(holds(&s, E_PREINCREMENT), "++var1==2 holds");
    check(holds(&s, E_VAR1_IS_2), "the condition's side effect persists");

    /* ToBoolean (449). */
    check(holds(&s, E_FOO_STRING), "'foo' is true");
    check(!holds(&s, E_ZERO) && !holds(&s, E_EMPTY_STRING) && !holds(&s, E_UNDEFINED), "falsy values are false");

    /* A function stored in data and called later (453). */
    check(declare(&s, S("fn"), NULL) == TA_OP_OK && set_eval(&s, S("fn"), E_FUNCTION) == TA_OP_OK,
          "function-valued data");
    check(holds(&s, E_CALL_FN), "stored function is callable");

    /* Global script functions; assign to a substructure (452). */
    check(script(&s, E_SCRIPT) == TA_OP_OK, "script declares a function");
    check(declare(&s, S("foo"), NULL) == TA_OP_OK, "declare foo");
    check(assign(&s, E_FOO, E_NEW_TESTOBJECT) == TA_OP_OK, "assign an expression with a trailing ;");
    check(assign(&s, E_FOO_BAR, E_ONE) == TA_OP_OK, "assign foo.bar");
    check(holds(&s, E_FOO_BAR_IS_1), "foo.bar == 1");
    check(assign(&s, E_NOPE, E_ONE) == TA_OP_ERROR, "assign to an undeclared root fails");
    check(assign(&s, E_EVENT, E_ONE) == TA_OP_ERROR, "assign to _event fails");
    check(holds(&s, E_ASSIGN_EVENT_THROWS), "_event is read-only in scripts");
    check(declare(&s, S("_event"), NULL) == TA_OP_ERROR, "a system variable cannot be declared");

    /* Expressions. */
    check(eval_json_is(&s, E_OBJECT_LITERAL, "{\"a\":1,\"b\":[1,2]}"), "an object literal is a value");
    ta_value scratch;
    check(eval(&s, E_SYNTAX_ERROR, &scratch) == TA_OP_ERROR, "a syntax error faults");
    check(eval(&s, E_LOOP, &scratch) == TA_OP_ERROR, "an endless loop is interrupted");
    check(holds(&s, E_ONE), "the session works after an interrupt");
    check(holds(&s, E_EVENT_UNDEFINED), "system variables are undefined without a host");
    check(!holds(&s, E_IN), "In() is false without a host");

    /* Wire JSON: markers at the top level, null when nested. */
    check(eval_json_is(&s, E_UNDEFINED, "{\"$value\":\"undefined\"}"), "undefined marker");
    check(eval_json_is(&s, E_FUNCTION, "{\"$value\":\"function\"}"), "function marker");
    check(eval_json_is(&s, E_LOSSY, "{\"u\":null,\"f\":null,\"n\":1}"), "nested unrepresentable values are null");
    check(eval_json_is(&s, E_CYCLE, "{\"$value\":\"unserializable\"}"), "cycle marker");

    /* Content (446, 558, 578). */
    ta_value content;
    check(declare(&s, S("x"), NULL) == TA_OP_OK, "declare x");
    check(import_content(&s, "[1, 2, 3]", &content) == TA_OP_OK && set_named(&s, S("x"), &content) == TA_OP_OK,
          "JSON content");
    ta_value_release(&content);
    check(holds(&s, E_X_IS_ARRAY), "JSON content is an Array of this realm");
    check(import_content(&s, "\nthis  is \na string \n", &content) == TA_OP_OK &&
              set_named(&s, S("x"), &content) == TA_OP_OK,
          "text content");
    check(text_is(&s, &content, "this is a string"), "text content is space-normalized");
    ta_value_release(&content);
    check(holds(&s, E_X_TEXT), "normalized text reads back");

    /* Wire values parse in the reading session's realm. */
    ta_value wire;
    require(tat_js_value_from_json(context, S("[1,2]"), &wire) == TA_OK, "wire value");
    check(set_named(&s, S("x"), &wire) == TA_OP_OK && holds(&s, E_X_IS_ARRAY), "wire array is an Array here");
    check(json_is(&wire, "[1,2]"), "wire value encodes its text");
    check(text_is(&s, &wire, "1,2"), "value_text of a wire array");
    ta_value_release(&wire);

    /* foreach (457, 459, 460). */
    ta_fault fault = {0};
    ta_value snapshot;
    uint32_t count = 0;
    check(declare(&s, S("Var4"), NULL) == TA_OP_OK && set_eval(&s, S("Var4"), E_SEVEN) == TA_OP_OK, "Var4 = 7");
    check(s.model.v->foreach_snapshot(s.model.self, &(ta_datamodel_foreach_snapshot_params){
                                                        .shared = s.shared, .instance = s.instance,
                                                        .array = E_VAR4, .out = &snapshot, .count = &count,
                                                        .fault = &fault}) == TA_OP_ERROR,
          "foreach over a non-array faults");
    check(s.model.v->foreach_snapshot(s.model.self, &(ta_datamodel_foreach_snapshot_params){
                                                        .shared = s.shared, .instance = s.instance,
                                                        .array = E_ARRAY, .out = &snapshot, .count = &count,
                                                        .fault = &fault}) == TA_OP_OK &&
              count == 3,
          "foreach snapshot");
    static const ta_str bad_items[] = {{.ptr = "continue", .len = 8}, {.ptr = "'continue'", .len = 10},
                                       {.ptr = "a, b", .len = 4}, {.ptr = "a;b", .len = 3}};
    for (size_t index = 0; index < ARRAY_COUNT(bad_items); index += 1) {
        check(s.model.v->foreach_bind(s.model.self, &(ta_datamodel_foreach_bind_params){
                                                        .shared = s.shared, .instance = s.instance,
                                                        .snapshot = &snapshot, .index = 0, .item = bad_items[index],
                                                        .index_name = S("Var3"), .fault = &fault}) == TA_OP_ERROR,
              "an illegal foreach item faults");
    }
    check(s.model.v->foreach_bind(s.model.self, &(ta_datamodel_foreach_bind_params){
                                                    .shared = s.shared, .instance = s.instance,
                                                    .snapshot = &snapshot, .index = 2, .item = S("Var2"),
                                                    .index_name = S("Var3"), .fault = &fault}) == TA_OP_OK,
          "foreach bind");
    check(holds(&s, E_FOREACH_DONE), "foreach item and index");
    ta_value_release(&snapshot);

    /* make_params: the last duplicate wins. */
    ta_value values[3];
    check(eval(&s, E_ONE, &values[0]) == TA_OP_OK && eval(&s, E_TWO, &values[1]) == TA_OP_OK &&
              eval(&s, E_THREE, &values[2]) == TA_OP_OK,
          "param values");
    ta_named_value named[] = {{S("a"), values[0]}, {S("b"), values[1]}, {S("a"), values[2]}};
    ta_value params;
    check(s.model.v->make_params(s.model.self, &(ta_datamodel_make_params_params){
                                                   .shared = s.shared, .instance = s.instance, .values = named,
                                                   .count = 3, .out = &params, .fault = &fault}) == TA_OP_OK &&
              json_is(&params, "{\"a\":3,\"b\":2}"),
          "make_params");
    check(text_is(&s, &values[0], "1") && text_is(&s, &params, "[object Object]"), "value_text is ToString");
    ta_value_release(&params);
    for (size_t index = 0; index < 3; index += 1) {
        ta_value_release(&values[index]);
    }

    /* Foreign values fault. */
    ta_value foreign = {.self = &foreign, .v = &FOREIGN};
    check(set_named(&s, S("x"), &foreign) == TA_OP_ERROR, "set_named rejects a foreign value");
    check(!text_is(&s, &foreign, ""), "value_text rejects a foreign value");
    session other = open_session(&foreign, &result);
    check(result == TA_ERR_UNSUPPORTED, "a foreign binding is unsupported");
    free(other.instance);

    /* Inspection: the declared data only, as an ECMAScript object. */
    ta_value root;
    check(s.model.v->inspect(s.model.self, &(ta_datamodel_inspect_params){
                                               .shared = s.shared, .instance = s.instance, .out = &root}) == TA_OK &&
              tat_js_value_is(&root) &&
              json_is(&root, "{\"var1\":2,\"fn\":null,\"foo\":{\"bar\":1},\"x\":[1,2],"
                             "\"Var4\":7,\"Var2\":3,\"Var3\":2}"),
          "inspect reports the declared data");
    ta_value_release(&root);

    /* A retained handle shares; values from another session keep their realm. */
    session b = open_session(NULL, &result);
    require(result == TA_OK, "second instance");
    ta_value object = {0};
    ta_value shared;
    check(declare(&s, S("obj"), NULL) == TA_OP_OK && eval(&s, E_OBJECT, &object) == TA_OP_OK &&
              set_named(&s, S("obj"), &object) == TA_OP_OK,
          "obj in session A");
    /* A value is a handle: retain is the whole contract (ADR 0050). */
    shared = object;
    ta_value_retain(&shared);
    check(shared.self == object.self, "retain shares the handle");
    check(set_named(&b, S("o2"), &shared) == TA_OP_OK, "shared object into session B");
    check(assign(&s, E_OBJ_N, E_FIVE) == TA_OP_OK && holds(&b, E_SHARED_SEES_5), "a retained handle shares the object");
    ta_value array;
    check(eval(&s, E_ARRAY, &array) == TA_OP_OK && set_named(&b, S("y"), &array) == TA_OP_OK &&
              holds(&b, E_CROSS_REALM),
          "a shared array keeps its realm");
    ta_value_release(&array);
    ta_value_release(&shared);

    /* A value as bytes (core ADR 0090): both forms read back to an equal
     * value from pieces of any size; bytes that are no value are refused. */
    for (int durable = 0; durable < 2; durable += 1) {
        test_sink sink = {0};
        ta_fault fault = {0};
        check(model.v->value_write(model.self, &(ta_datamodel_value_write_params){
                                                   .shared = s.shared, .value = &object,
                                                   .sink = { .self = &sink, .v = &TEST_SINK },
                                                   .durable = durable != 0, .fault = &fault }) == TA_OP_OK &&
                  sink.len > 1,
              "value_write writes an object");
        static const uint32_t pieces[] = { 1, 7, 0 };
        for (size_t index = 0; index < sizeof pieces / sizeof pieces[0]; index += 1) {
            test_source source = { .bytes = sink.bytes, .len = sink.len, .piece = pieces[index] };
            ta_value back = {0};
            check(model.v->value_read(model.self, &(ta_datamodel_value_read_params){
                                                      .shared = s.shared,
                                                      .source = { .self = &source, .v = &TEST_SOURCE },
                                                      .durable = durable != 0, .out = &back,
                                                      .fault = &fault }) == TA_OP_OK,
                  "value_read reads it back");
            check(json_is(&back, "{\"n\":5}"), "the read value equals the written one");
            ta_value_release(&back);
        }
        free(sink.bytes);
    }
    uint8_t junk[] = { 0, 0xff, 0xfe };
    for (int durable = 0; durable < 2; durable += 1) {
        test_source source = { .bytes = junk, .len = sizeof junk, .piece = 1 };
        ta_value back = {0};
        ta_fault fault = {0};
        check(model.v->value_read(model.self, &(ta_datamodel_value_read_params){
                                                  .shared = s.shared, .source = { .self = &source, .v = &TEST_SOURCE },
                                                  .durable = durable != 0, .out = &back, .fault = &fault }) ==
                  TA_OP_ERROR,
              "value_read refuses bytes that are no value");
    }

    /* A value outlives its session. */
    close_session(&s);
    check(json_is(&object, "{\"n\":5}"), "a value outlives its session");
    ta_value_release(&object);
    close_session(&b);

    /* Bound names from the binding (invoke params) skip their initializers. */
    ta_value binding;
    require(tat_js_value_from_json(context, S("{\"p\":5}"), &binding) == TA_OK, "binding value");
    session c = open_session(&binding, &result);
    ta_value_release(&binding);
    require(result == TA_OK, "instance with a binding");
    bool bound = false;
    check(declare(&c, S("p"), &bound) == TA_OP_OK && bound, "declare reports a bound name");
    check(holds(&c, E_P_IS_5), "bound value survives declare");
    close_session(&c);
}

/* ------------------------------------------------------------------------
 * With a running system: _event, _sessionid, _name, _ioprocessors, In().
 */

#define END {0}

static ta_expression source(const char *text) {
    return (ta_expression){.kind = TA_EXPRESSION_SOURCE, .as.source = {.ptr = text, .len = (uint32_t)strlen(text)}};
}

static ta_event_name_id event_name(ta_system *system, const char *name) {
    ta_atom atom = 0;
    require(ta_system_intern_atom(system, (ta_str){.ptr = name, .len = (uint32_t)strlen(name)}, &atom) == TA_OK,
            "intern atom");
    ta_event_name_id id = 0;
    require(ta_event_name_intern(system, &(ta_atom_list){.atoms = &atom, .count = 1}, &id) == TA_OK,
            "intern event name");
    return id;
}

static void run(ta_system *system, ta_actor_id actor) {
    ta_run_status status = TA_RUN_PAUSED;
    for (int round = 0; round < 100 && status == TA_RUN_PAUSED; round += 1) {
        require(ta_actor_resume(system, &(ta_actor_resume_params){.actor = actor, .budget = 64, .status = &status}) ==
                    TA_OK,
                "resume");
    }
}

static bool root_contains(ta_system *system, ta_actor_id actor, const char *needle) {
    ta_value root;
    if (ta_actor_datamodel_root(system, actor, &root) != TA_OK) {
        return false;
    }
    tat_js_json json = {0};
    bool found = tat_js_value_to_json(&root, &json) == TA_OK && strstr(json.ptr, needle) != NULL;
    if (!found) {
        fprintf(stderr, "  root %.*s lacks %s\n", (int)json.len, json.ptr, needle);
    }
    tat_js_json_release(&json);
    ta_value_release(&root);
    return found;
}

static void test_system(tat_js_datamodel_context *context) {
    ta_system *system = NULL;
    require(ta_system_create(&(ta_system_config){.allocator = tat_libc_allocator()}, &system) == TA_OK,
            "system");
    static const char *const names[] = {"before", "inB", "sid", "nm", "io", "evname", "evtype", "sendid",
                                        "isarr", "same", "ro"};
    ta_scxml_data_spec data[ARRAY_COUNT(names) + 1];
    for (size_t index = 0; index < ARRAY_COUNT(names); index += 1) {
        data[index] = (ta_scxml_data_spec){.name = {.ptr = names[index], .len = (uint32_t)strlen(names[index])}};
    }
    data[ARRAY_COUNT(names)] = (ta_scxml_data_spec){0};
    const ta_scxml_action_spec entry[] = {
        {.kind = TA_SCXML_ACTION_SCRIPT,
         .as.script = {source("before = typeof _event; sid = typeof _sessionid;"
                              " nm = _name; io = _ioprocessors.scxml.location;"
                              " io = io === _ioprocessors['http://www.w3.org/TR/scxml/#SCXMLEventProcessor'].location"
                              " && typeof io === 'string';")}},
        END,
    };
    const ta_scxml_action_spec on_go[] = {
        {.kind = TA_SCXML_ACTION_SCRIPT,
         .as.script = {source("inB = In('b') && In('p') && !In('nowhere'); evname = _event.name; evtype = _event.type; sendid = typeof _event.sendid;"
                              " isarr = _event.data instanceof Array && _event.data[1] === 2;"
                              " same = _event === _event;"
                              " try { _event = 1; ro = false; } catch (e) { ro = true; }")}},
        END,
    };
    const ta_scxml_handler_spec handlers[] = {{.actions = entry}, END};
    const ta_str no_targets[] = {END};
    const ta_scxml_transition_spec transitions[] = {
        {.kind = TA_SCXML_TRANSITION_INTERNAL, .event = S("go"), .targets = no_targets, .actions = on_go},
        END,
    };
    const ta_scxml_state_spec regions[] = {
        {.kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions, .onentry = handlers},
        {.kind = TA_SCXML_STATE_ATOMIC, .id = S("b")},
        END,
    };
    const ta_scxml_state_spec states[] = {
        {.kind = TA_SCXML_STATE_PARALLEL, .id = S("p"), .states = regions},
        END,
    };
    ta_scxml_spec spec = {.name = S("probe"), .datamodel = S("ecmascript"), .states = states, .data = data};
    ta_definition *definition = NULL;
    require(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                   .spec = &spec,
                                                   .datamodel = tat_js_datamodel(context),
                                                   .allocator = tat_libc_allocator(),
                                                   .out = &definition,
                                               }) == TA_OK,
            "compile an ECMAScript chart");
    ta_definition_id id = 0;
    require(ta_system_register_definition(system, definition, &id) == TA_OK, "register");
    ta_actor_id actor = 0;
    require(ta_actor_spawn(system, &(ta_actor_spawn_params){.definition = id, .out = &actor}) == TA_OK, "spawn");
    run(system, actor);
    check(root_contains(system, actor, "\"before\":\"undefined\""), "_event is undefined before the first event");
    check(root_contains(system, actor, "\"sid\":\"string\""), "_sessionid is a string");
    check(root_contains(system, actor, "\"nm\":\"probe\""), "_name");
    check(root_contains(system, actor, "\"io\":true"), "_ioprocessors under both names");

    ta_value data_value;
    require(tat_js_value_from_json(context, S("[1,2]"), &data_value) == TA_OK, "event data");
    ta_event event = {.name = event_name(system, "go"), .type = TA_EVENT_EXTERNAL,
                      .invoke_id = TA_INVOKE_ID_NONE, .data = data_value};
    require(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = actor, .event = &event }) == TA_OK, "deliver");
    ta_value_release(&data_value);
    run(system, actor);
    check(root_contains(system, actor, "\"inB\":true"), "In() sees the other parallel region");
    check(root_contains(system, actor, "\"evname\":\"go\""), "_event.name");
    check(root_contains(system, actor, "\"evtype\":\"external\""), "_event.type");
    check(root_contains(system, actor, "\"sendid\":\"undefined\""), "an absent _event field is undefined");
    check(root_contains(system, actor, "\"isarr\":true"), "wire event data is an Array of the session");
    check(root_contains(system, actor, "\"same\":true"), "_event is stable within an event");
    check(root_contains(system, actor, "\"ro\":true"), "_event is read-only");
    ta_system_destroy(system);
}

int main(void) {
    setup_expressions();
    tat_js_datamodel_context context = {0};
    require(tat_js_datamodel_context_init(&context, &(tat_js_datamodel_config){
                                                        .allocator = {.self = NULL, .v = &COUNTING},
                                                        .interrupt_budget = 200,
                                                    }) == TA_OK,
            "context initialization");
    model = tat_js_datamodel(&context);
    require(model.v->prepare_definition(model.self, &(ta_datamodel_prepare_definition_params){
                                                        .expressions = expressions,
                                                        .expression_count = E_COUNT,
                                                        .allocator = tat_libc_allocator(),
                                                        .shared = &shared,
                                                    }) == TA_OK,
            "definition preparation");
    test_operations(&context);
    model.v->destroy_definition(model.self, shared, tat_libc_allocator());
    test_system(&context);
    tat_js_datamodel_context_destroy(&context);
    check(outstanding == 0, "the context frees everything it allocated");
    if (outstanding != 0) {
        fprintf(stderr, "  %zu bytes outstanding\n", outstanding);
    }
    if (failures != 0) {
        fprintf(stderr, "%d ecmascript datamodel checks failed\n", failures);
        return 1;
    }
    puts("ecmascript datamodel test: ok");
    return 0;
}
