/* ta_datamodel_ecmascript: the quickjs-ng ECMAScript datamodel behind ta_system. */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"
#include "quickjs.h"

typedef struct ecmascript_system {
    void *bytes, *memory;
    ta_image image;
    ta_datamodel_ecmascript *ecmascript;
    ta_system *system;
    char log[8192];
} ecmascript_system;

static int ecmascript_open(ecmascript_system *t, const char *chart,
                           const ta_datamodel_ecmascript_config *ecmascript_config) {
    memset(t, 0, sizeof *t);
    size_t length;
    t->bytes = compile_chart("es.scxml", chart, NULL, &length, 0);
    if (!t->bytes || ta_image_open((ta_bytes){t->bytes, length}, &t->image) != TA_OK) return 0;
    t->ecmascript = ta_datamodel_ecmascript_new(ecmascript_config);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(t->ecmascript);
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    size_t need;
    ta_system_layout(&t->image, &config, &need);
    t->memory = malloc(need);
    return ta_system_init(&(ta_system_setup){{t->memory, need}, &t->image, &config, &binding, 1, 0},
                          &t->system) == TA_OK &&
           ta_system_boot(t->system) == TA_OK;
}

static void ecmascript_run(ecmascript_system *t) {
    for (int i = 0; i < 100; i++) {
        ta_run result = ta_system_run(t->system, 100000);
        const ta_effect *effect;
        uint32_t n = ta_system_effects(t->system, &effect);
        for (uint32_t k = 0; k < n; k++)
            if (effect[k].kind == TA_EFFECT_LOG) {
                size_t l = strlen(t->log);
                snprintf(t->log + l, sizeof t->log - l, "%.*s=%.*s;",
                         (int)effect[k].auxiliary.length,
                         effect[k].auxiliary.pointer ? effect[k].auxiliary.pointer : "",
                         (int)effect[k].event.length,
                         effect[k].event.pointer ? effect[k].event.pointer : "");
            }
        ta_system_effects_ack(t->system, n);
        if (result.status == TA_RUN_IDLE) break;
    }
}

static const char *ecmascript_eval(ecmascript_system *t, const char *expression) {
    static char buffer[1024];
    uint32_t a = ta_system_actor_by_name(t->system, (ta_string_view){"es.scxml", 8});
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    if (ta_actor_info_get(t->system, a, &actor_info) != TA_OK) return "<gone>";
    uint64_t instance =
        actor_info.datamodel_instance; /* the public way to the datamodel instance */
    uint32_t index = ta_system_resolve_actor(t->system, a);
    CHECK_EQUAL(instance, ta_actor_instance(t->system, ta_actor_at(t->system, index))->datamodel);
    if (eval_text(
            t->ecmascript, instance, (ta_string_view){expression, (uint32_t)strlen(expression)},
            buffer, sizeof buffer) == (size_t)-1)
        return "<error>";
    return buffer;
}

static void ecmascript_close(ecmascript_system *t) {
    ta_system_deinit(t->system);
    ta_datamodel_ecmascript_free(t->ecmascript); /* JS_FreeRuntime asserts nothing leaked */
    free(t->memory);
    free(t->bytes);
}

#define SC(body)                                                                                   \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

static void test_expressions_and_script(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='n' expr='1'/>"
           "<data id='obj'>{\"k\": [1, 2, 3]}</data><data id='txt'>  some \n  text </data>"
           "</datamodel><script>function sq(x) { return x * x; }</script>"
           "<state id='a'><onentry>"
           "<assign location='n' expr='sq(n + 2)'/>"
           "<assign location='obj.k[1]' expr='n'/>"
           "<log label='n' expr='n'/><log label='obj' expr='obj'/><log label='txt' expr='txt'/>"
           "<log label='sid' expr='typeof _sessionid'/>"
           "<foreach array='obj.k' item='fresh' index='i'><assign location='n' expr='n + "
           "fresh'/></foreach>"
           "<log label='n2' expr='n'/>"
           "</onentry></state>"),
        NULL));
    ecmascript_run(&t);
    CHECK(strcmp(t.log, "n=9;obj={\"k\":[1,9,3]};txt=some text;sid=string;n2=22;") == 0);
    if (strcmp(t.log, "n=9;obj={\"k\":[1,9,3]};txt=some text;sid=string;n2=22;"))
        fprintf(stderr, "  log: %s\n", t.log);
    CHECK(strcmp(ecmascript_eval(&t, "fresh"), "3") == 0); /* foreach declared a new variable */
    ecmascript_close(&t);
}

static void test_errors_and_readonly(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='errs' expr='0'/></datamodel>"
           "<state id='a'><onentry>"
           "<assign location='undeclared' expr='1'/>" /* undeclared location */
           "</onentry><onentry><assign location='_sessionid' expr='1'/>"
           "</onentry><onentry><assign location='n' expr='syntax error here'/>"
           "</onentry><onentry><log expr='_event'/>" /* unbound before first event */
           "</onentry>"
           "<transition event='error.execution'><assign location='errs' expr='errs + "
           "1'/></transition>"
           "</state>"),
        NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "errs"), "4") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "_event.name"), "\"error.execution\"") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "typeof undeclared"), "\"undefined\"") == 0);
    ecmascript_close(&t);
}

static void test_runaway_script_interrupted(void) {
    ta_datamodel_ecmascript_config config = {sizeof config, 10000, 0, 0, 0, NULL};
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='after' expr='0'/></datamodel>"
           "<state id='a'><onentry><script>while (true) {}</script></onentry>"
           "<transition event='error.execution' target='b'/></state>"
           "<state id='b'><onentry><assign location='after' expr='1'/></onentry></state>"),
        &config));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "after"), "1") == 0); /* interrupted -> error.execution */
    ecmascript_close(&t);
}

/* error.execution says why in its data: {reason: "exception", message}
 * for code that threw (an expression, a script, a <send>'s targetexpr, with
 * the send's id), "fuel" past max_ops, "memory" past memory_limit
 * (tinyactors-napi/docs/log.xml, l-p4-examples-iot-error-reason). */
#define WHY(content)                                                                               \
    SC("initial='a'><state id='a'><onentry>" content "</onentry>"                                  \
       "<transition event='error.execution'><log label='why' expr=\"_event.data.reason + ':' + "   \
       "(_event.data.message || '') + ':' + (_event.sendid || '')\"/></transition></state>")
static void test_error_execution_says_why(void) {
    static const struct {
        const char *chart;
        ta_datamodel_ecmascript_config config;
        const char *want;
    } cases[] = {
        {WHY("<script>throw new Error('boom')</script>"), {0}, "why=exception:Error: boom:;"},
        {WHY("<log expr='nosuch.x'/>"), {0}, "why=exception:ReferenceError: nosuch is not defined:;"},
        {WHY("<send event='x' targetexpr='nosuch.y' id='s1'/>"), {0},
         "why=exception:ReferenceError: nosuch is not defined:s1;"},
        {WHY("<script>while (true) {}</script>"), {sizeof(ta_datamodel_ecmascript_config), 10000, 0, 0, 0, NULL},
         "why=fuel::;"},
        {WHY("<script>let a = []; while (true) a.push(new Array(100000).fill(1));</script>"),
         {sizeof(ta_datamodel_ecmascript_config), 0, 8u << 20, 0, 0, NULL}, "why=memory::;"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ecmascript_system t;
        CHECK(ecmascript_open(&t, cases[i].chart, cases[i].config.size ? &cases[i].config : NULL));
        ecmascript_run(&t);
        CHECK(strcmp(t.log, cases[i].want) == 0);
        if (strcmp(t.log, cases[i].want)) fprintf(stderr, "  case %zu log: %s\n", i, t.log);
        ecmascript_close(&t);
    }
}

static void test_events_payloads_xml(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='got' expr='null'/></datamodel>"
           "<state id='a'><onentry>"
           "<send event='p'><param name='x' expr='[1, {y: 2}]'/></send>"
           "<send event='q'><content><doc><item k='v'/></doc></content></send></onentry>"
           "<transition event='p'><assign location='got' expr='_event.data.x[1].y'/></transition>"
           "<transition event='q' target='b'><log label='xml' "
           "expr=\"_event.data.getElementsByTagName('item')[0].getAttribute('k')\"/></transition>"
           "</state><state id='b'><transition event='host' target='c'>"
           "<assign location='got' expr='_event.data.a + _event.type'/></transition></state>"
           "<state id='c'/>"),
        NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "got"), "2") == 0);
    CHECK(strcmp(t.log, "xml=v;") == 0);
    /* host-injected JSON payload */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"host", 4};
    event.data = ta_datamodel_ecmascript_payload_eval(
        t.ecmascript, 0, (ta_string_view){"{\"a\": \"hi-\"}", 12});
    CHECK(event.data != 0);
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "got"), "\"hi-external\"") == 0);
    char json[64];
    ta_payload payload =
        ta_datamodel_ecmascript_payload_eval(t.ecmascript, 0, (ta_string_view){"[1,\"two\"]", 9});
    CHECK(payload_text(t.ecmascript, payload, json, sizeof json) == 9);
    CHECK(strcmp(json, "[1,\"two\"]") == 0);
    ta_datamodel_ecmascript_vtable()->release(t.ecmascript, payload);
    ecmascript_close(&t);
}

/* ta_datamodel_ecmascript_heap_bytes: an O(1) count that follows allocations */
static void test_heap_bytes(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='big' expr='null'/></datamodel>"
           "<state id='a'><transition event='grow' target='b'>"
           "<assign location='big' expr='Array.from({ length: 200000 }, (_, i) => ({ i: i }))'/>"
           "</transition></state><state id='b'><transition event='shrink' target='a'>"
           "<assign location='big' expr='null'/></transition></state>"),
        NULL));
    ecmascript_run(&t);
    size_t before = ta_datamodel_ecmascript_heap_bytes(t.ecmascript);
    CHECK(before > 0);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"grow", 4};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    size_t grown = ta_datamodel_ecmascript_heap_bytes(t.ecmascript);
    CHECK(grown > before + 200000 * 16); /* at least the objects */
    event.name = (ta_string_view){"shrink", 6};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    JS_RunGC((JSRuntime *)ta_datamodel_ecmascript_js_runtime(t.ecmascript));
    CHECK(ta_datamodel_ecmascript_heap_bytes(t.ecmascript) < grown / 2); /* frees are counted */
    ecmascript_close(&t);
}

/* With values traced, a record keeps an assigned value alive until the host
 * reads it: acknowledging the records lets it go. */
static void test_traced_values_are_let_go(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='big' expr='null'/></datamodel>"
           "<state id='a'><transition event='grow' target='b'>"
           "<assign location='big' expr='Array.from({ length: 200000 }, (_, i) => ({ i: i }))'/>"
           "</transition></state><state id='b'><transition event='shrink' target='a'>"
           "<assign location='big' expr='null'/></transition></state>"),
        NULL));
    ta_trace_policy policy;
    ta_trace_policy_default(&policy);
    policy.kinds[TA_TRACE_DETAIL_SAMPLED] = TA_TRACE_FULL | TA_TRACE_VALUES;
    CHECK_EQUAL(ta_system_trace_set(t.system, &policy), TA_OK);
    ecmascript_run(&t);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"grow", 4};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    size_t grown = ta_datamodel_ecmascript_heap_bytes(t.ecmascript);
    event.name = (ta_string_view){"shrink", 6};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    JSRuntime *runtime = (JSRuntime *)ta_datamodel_ecmascript_js_runtime(t.ecmascript);
    JS_RunGC(runtime);
    CHECK(ta_datamodel_ecmascript_heap_bytes(t.ecmascript) > grown / 2); /* the record has it */
    for (ta_trace_view view; ta_system_trace(t.system, &view);)
        ta_system_trace_ack(t.system, view.length);
    JS_RunGC(runtime);
    CHECK(ta_datamodel_ecmascript_heap_bytes(t.ecmascript) < grown / 2);
    ecmascript_close(&t);
}

/* Shared realm (default): ten invoked children of one parent each keep their
 * own variables; implicit globals are errors; In() resolves per session; the
 * same system costs a fraction of the isolated heap. */
static size_t realm_run(uint32_t realm, char *log, size_t capacity) {
    static char chart[16384];
    size_t n = (size_t)snprintf(chart, sizeof chart, "%s",
                                "<scxml xmlns='http://www.w3.org/2005/07/scxml' "
                                "datamodel='ecmascript' initial='a'><datamodel>"
                                "<data id='sum' expr='0'/><data id='seen' expr='0'/></datamodel>"
                                "<state id='a'><onentry><script>leak = 1</script></onentry>"
                                "<onentry><script>globalThis.leak2 = 1</script></onentry>");
    for (int k = 1; k <= 10; k++)
        n += (size_t)snprintf(
            chart + n, sizeof chart - n,
            "<invoke id='c%d'><param name='v' expr='%d'/><content><scxml "
            "xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='s'>"
            "<datamodel><data id='v' expr='0'/></datamodel><state id='s'><onentry>"
            "<script>v = v * 10</script><send target='#_parent' event='r'>"
            "<param name='v' expr='In(\"s\") ? v : -1000'/></send></onentry></state>"
            "</scxml></content></invoke>",
            k, k);
    snprintf(chart + n, sizeof chart - n, "%s",
             "<transition event='r' cond='In(\"a\")'>"
             "<assign location='sum' expr='sum + _event.data.v'/>"
             "<assign location='seen' expr='seen + 1'/></transition>"
             "<transition event='error.execution'><log label='err' expr='typeof leak + \"/\" + "
             "typeof leak2'/></transition></state></scxml>");
    ta_datamodel_ecmascript_config config = {sizeof config, 0, 0, 0, realm, NULL};
    ecmascript_system t;
    CHECK(ecmascript_open(&t, chart, &config));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "seen"), "10") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "sum"), "550") ==
          0); /* 10 * (1 + ... + 10): no shared state */
    JS_RunGC((JSRuntime *)ta_datamodel_ecmascript_js_runtime(t.ecmascript));
    size_t heap = ta_datamodel_ecmascript_heap_bytes(t.ecmascript);
    snprintf(log, capacity, "%s", t.log);
    ecmascript_close(&t);
    return heap;
}

static void test_shared_realm(void) {
    char shared_log[512], iso_log[512];
    size_t shared = realm_run(TA_DATAMODEL_ECMASCRIPT_REALM_AUTO, shared_log, sizeof shared_log);
    size_t iso = realm_run(TA_DATAMODEL_ECMASCRIPT_REALM_ISOLATED, iso_log, sizeof iso_log);
    /* shared: both implicit globals fail (error.execution) and nothing leaked */
    CHECK(strcmp(shared_log, "err=undefined/undefined;err=undefined/undefined;") == 0);
    if (strcmp(shared_log, "err=undefined/undefined;err=undefined/undefined;"))
        fprintf(stderr, "  shared log: %s\n", shared_log);
    CHECK(strcmp(iso_log, "") == 0);  /* isolated: sloppy globals of the session's own */
    CHECK(iso > shared + 10 * 30000); /* > 30 KB saved per session (a context is ~54 KB) */
    fprintf(stderr, "  heap: shared %zu B, isolated %zu B (11 sessions)\n", shared, iso);
}

/* Is the session's realm the shared one? (its global object is frozen) */
static const char *realm_of(ecmascript_system *t) {
    return ecmascript_eval(
        t, "(function () { 'use strict'; try { globalThis.__p = 1; return 'isolated'; } "
           "catch (e) { return 'shared'; } })()");
}

/* payload_eval of a session variable reads it (no compiling); anything else is
 * evaluated as an expression in the session's scope, as before */
static void test_eval_variables(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='n' expr='2'/>"
           "<data id='obj' expr='({k: [1, 2], s: \"x\"})'/><data id='u'/></datamodel>"
           "<state id='a'/>"),
        NULL));
    ecmascript_run(&t);
    CHECK(strcmp(realm_of(&t), "\"shared\"") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "n"), "2") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "obj"), "{\"k\":[1,2],\"s\":\"x\"}") == 0);
    char undef[64];
    snprintf(undef, sizeof undef, "%s", ecmascript_eval(&t, "undefined"));
    CHECK(strcmp(ecmascript_eval(&t, "u"), undef) == 0);
    CHECK(strcmp(ecmascript_eval(&t, "obj.k"), "[1,2]") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "Infinity"), "Infinity") == 0); /* a builtin, not the session's */
    CHECK(strcmp(ecmascript_eval(&t, "true"), "true") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "missing"), "<error>") == 0);
    ecmascript_close(&t);
}

/* What a script declares is asked of the engine: names declared at the top
 * level keep the chart isolated (and visible to later expressions); names in
 * blocks, nested functions, comments, strings or regular expressions do not. */
/* A <script> in executable content runs in a block: its top-level let,
 * const and class are its own at each run, so it runs again (it failed with
 * a redeclaration from its second run), and they make the chart declare
 * nothing (the shared realm); its var stays the session's; the chart's
 * top-level script stays global code, its const read by the content
 * (tinyactors-napi/docs/log.xml, l-p4-examples-iot-script-let). */
#define RERUN(top, script)                                                                         \
    SC("initial='a'><datamodel><data id='n' expr='0'/><data id='errors' expr='0'/></datamodel>"    \
       top "<state id='a'><onentry><raise event='go'/><raise event='go'/><raise event='go'/>"     \
           "</onentry><transition event='go'><script>" script "</script></transition>"            \
           "<transition event='error.execution'><assign location='errors' expr='errors + 1'/>"    \
           "</transition></state>")
static void test_executable_scripts_run_again(void) {
    static const char *const lexical = "let k = n + 1; const c = k; class K {} n = c;";
    for (uint32_t realm = 0; realm < 2; realm++) {
        ta_datamodel_ecmascript_config config = {sizeof config, 0, 0, 0, realm, NULL};
        ecmascript_system t;
        char chart[1024];
        snprintf(chart, sizeof chart, RERUN("", "%s"), lexical);
        CHECK(ecmascript_open(&t, chart, &config));
        ecmascript_run(&t);
        CHECK(strcmp(ecmascript_eval(&t, "n"), "3") == 0);
        CHECK(strcmp(ecmascript_eval(&t, "errors"), "0") == 0);
        CHECK(strcmp(realm_of(&t), realm ? "\"isolated\"" : "\"shared\"") == 0);
        ecmascript_close(&t);
    }
    ecmascript_system t;
    CHECK(ecmascript_open(&t, RERUN("<script>const LIMIT = 2;</script>",
                                    "let k = n + 1; n = Math.min(k, LIMIT); var seen = (typeof seen === "
                                    "'number' ? seen : 0) + 1;"),
                          NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "n"), "2") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "seen"), "3") == 0); /* the session's var */
    CHECK(strcmp(ecmascript_eval(&t, "errors"), "0") == 0);
    CHECK(strcmp(realm_of(&t), "\"isolated\"") == 0);
    ecmascript_close(&t);
}

static void test_toplevel_declarations_isolate(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(&t,
                          SC("initial='a'><script>var x = 2; const y = 3; /* let z */ "
                             "var re = /[/]var/;</script><state id='a'/>"),
                          NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "x * y"), "6") == 0);
    CHECK(strcmp(realm_of(&t), "\"isolated\"") == 0);
    ecmascript_close(&t);
    CHECK(ecmascript_open(&t,
                          SC("initial='a'><datamodel><data id='x' expr='1'/></datamodel>"
                             "<script>x = [1, 2].map(function (v) { const k = v; return k * 2; })"
                             ".length; // var</script><state id='a'/>"),
                          NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "x"), "2") == 0); /* shared: data written through the scope */
    CHECK(strcmp(ecmascript_eval(&t, "typeof globalThis.x"), "\"undefined\"") == 0);
    CHECK(strcmp(realm_of(&t), "\"shared\"") == 0);
    ecmascript_close(&t);
    /* block-scoped and function-local declarations: no global names */
    CHECK(
        ecmascript_open(&t,
                        SC("initial='a'><datamodel><data id='r' expr='0'/></datamodel>"
                           "<script>if (r === 0) { let t = 1; const u = 1; r = t + u; }\n"
                           "for (let i = 0; i &lt; 2; i++) r += i;\n"
                           "(function () { var v = 10; function w() { return v; } r += w(); })();\n"
                           "r += 'let s; class C {}'.length;</script><state id='a'/>"),
                        NULL));
    ecmascript_run(&t);
    CHECK(strcmp(ecmascript_eval(&t, "r"), "30") == 0);
    CHECK(strcmp(realm_of(&t), "\"shared\"") == 0);
    ecmascript_close(&t);
    /* a sloppy-mode function in a block is also a global var (Annex B) */
    CHECK(ecmascript_open(&t,
                          SC("initial='a'><script>if (true) { function g() { return 1; } }</script>"
                             "<state id='a'/>"),
                          NULL));
    ecmascript_run(&t);
    CHECK(strcmp(realm_of(&t), "\"isolated\"") == 0);
    CHECK(strcmp(ecmascript_eval(&t, "g()"), "1") == 0);
    ecmascript_close(&t);
}

static uint32_t ecmascript_actor(ecmascript_system *t) {
    return ta_system_actor_by_name(t->system, (ta_string_view){"es.scxml", 8});
}

static void ecmascript_send(ecmascript_system *t, const char *name, const char *json) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ecmascript_actor(t);
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    if (json)
        event.data = ta_datamodel_ecmascript_payload_eval(
            t->ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)});
    CHECK_EQUAL(ta_system_send(t->system, &event), TA_OK);
    ecmascript_run(t);
}

static unsigned char store_blob[1 << 20];

/* hibernate and wake the chart's actor: TA_OK or why not */
static ta_status ecmascript_store(ecmascript_system *t) {
    ta_buffer written = {store_blob, sizeof store_blob, 0};
    ta_status status = ta_actor_hibernate(t->system, ecmascript_actor(t), &written);
    if (status == TA_OK)
        status =
            ta_actor_wake(t->system, ecmascript_actor(t), (ta_bytes){store_blob, written.length});
    return status;
}

#define CHECK_JS(t, expr, want)                                                                    \
    do {                                                                                           \
        const char *ta__g = ecmascript_eval((t), (expr));                                          \
        CHECK(strcmp(ta__g, (want)) == 0);                                                         \
        if (strcmp(ta__g, (want))) fprintf(stderr, "  %s: got %s, want %s\n", expr, ta__g, want);  \
    } while (0)

/* Charts with top-level declarations are stored: var and implicit globals and
 * let/const bindings with their values (const stays const, an uninitialized
 * binding stays so), function declarations re-created by re-running them
 * (strict ones strict), closing over the restored globals — again and again. */
static const char *STORE_CHART =
    SC("initial='a'><datamodel><data id='n' expr='1'/></datamodel>"
       "<script>var total = 10;\n"
       "let items = ['a'];\n"
       "const LIMIT = 3;\n"
       "let later, \\u00e9t\\u00e9 = 'summer';\n"
       "function add(k) { total += k; items.push(k); return total; }\n"
       "implicit = { nested: [1, 2], when: new Date(0) };</script>"
       "<script>'use strict'; function self() { return this === undefined; }</script>"
       "<script>let dead = boom();</script>" /* a chart-level script: its let is global */
       "<state id='a'>"
       "<transition event='add'><script>add(_event.data.k); later = items.length;</script>"
       "</transition>"
       "<transition event='bump'><assign location='LIMIT' expr='LIMIT + 1'/></transition>"
       "<transition event='error.execution'><log label='err' expr='LIMIT'/></transition>"
       "</state>");

static void check_store_state(ecmascript_system *t, const char *total, const char *items,
                              const char *later) {
    CHECK_JS(t, "total", total);
    CHECK_JS(t, "items.join()", items);
    CHECK_JS(t, "LIMIT", "3");
    CHECK_JS(t, "later", later);
    CHECK_JS(t, "\u00e9t\u00e9", "\"summer\"");
    CHECK_JS(t, "implicit.nested[1] + implicit.when.getTime()", "2");
    CHECK_JS(t, "n", "1");
    CHECK_JS(t, "self()", "true");
    CHECK_JS(t, "typeof dead", "<error>"); /* still uninitialized */
}

static void test_store_declarations(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(&t, STORE_CHART, NULL));
    ecmascript_run(&t);
    CHECK(strcmp(t.log, "err=3;") == 0); /* boom() */
    ecmascript_send(&t, "add", "{\"k\": 5}");
    CHECK_EQUAL(ecmascript_store(&t), TA_OK);
    check_store_state(&t, "15", "\"a,5\"", "2");
    CHECK_JS(&t, "add(1)", "16"); /* the re-created function sees the restored globals */
    CHECK_EQUAL(ecmascript_store(&t), TA_OK); /* a re-created function is stored again */
    check_store_state(&t, "16", "\"a,5,1\"", "2");
    CHECK_JS(&t, "add(1)", "17");
    t.log[0] = 0;
    ecmascript_send(&t, "bump", NULL); /* assigning the const: error.execution */
    CHECK(strcmp(t.log, "err=3;") == 0);
    ecmascript_send(&t, "add", "{\"k\": 2}");
    CHECK_EQUAL(ecmascript_store(&t), TA_OK);
    check_store_state(&t, "19", "\"a,5,1,1,2\"", "5");
    ta_buffer written = {store_blob, sizeof store_blob, 0};
    CHECK_EQUAL(ta_actor_hibernate(t.system, ecmascript_actor(&t), &written), TA_OK);
    ecmascript_close(&t); /* hibernated sessions are freed with the system */
}

/* What cannot be stored is refused, saying what and where; the session goes on. */
static void refused(const char *chart, const char *want1, const char *want2) {
    ecmascript_system t;
    CHECK(ecmascript_open(&t, chart, NULL));
    ecmascript_run(&t);
    CHECK_EQUAL(ecmascript_store(&t), TA_ERROR_UNSUPPORTED);
    const char *why = ta_datamodel_ecmascript_last_error(t.ecmascript);
    CHECK(strstr(why, want1) != NULL);
    CHECK(!want2 || strstr(why, want2) != NULL);
    if (!strstr(why, want1) || (want2 && !strstr(why, want2))) fprintf(stderr, "  why: %s\n", why);
    CHECK_JS(&t, "1 + 1", "2"); /* still there */
    /* a snapshot of the datamodel says the same */
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(t.ecmascript, &(ta_buffer){NULL, 0, 0}),
                TA_ERROR_UNSUPPORTED);
    CHECK(strstr(ta_datamodel_ecmascript_last_error(t.ecmascript), want1) != NULL);
    ecmascript_close(&t);
}

static void test_store_refuses_precisely(void) {
    refused("<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='a'>\n"
            "<script>\n"
            "var ok = 1;\n"
            "const helper = (x) => x * 2;</script><state id='a'/></scxml>",
            "variable 'helper' cannot be stored: it holds a function (line 4)",
            "write `function helper(...) {...}`");
    refused(SC("initial='a'><script>var handlers = { go: function () {} };</script>"
               "<state id='a'/>"),
            "variable 'handlers' cannot be stored: handlers.go is a function (line 1)", NULL);
    refused(SC("initial='a'><script>class Shape {}</script><state id='a'/>"),
            "variable 'Shape' cannot be stored: it is a class", NULL);
    refused(SC("initial='a'><script>function P() {} P.prototype.area = function () {};</script>"
               "<state id='a'/>"),
            "function 'P' cannot be stored: P.prototype.area was added", NULL);
    refused(SC("initial='a'><script>var err = new Error('x');</script><state id='a'/>"),
            "variable 'err' cannot be stored: err is an Error object", NULL);
    refused(SC("initial='a'><datamodel><data id='m' expr='new Map([[1, [Promise.resolve()]]])'/>"
               "</datamodel><state id='a'/>"),
            "variable 'm' cannot be stored: m (value 0)[0] is a Promise object", NULL);
    /* an alias of a declared function is not the declaration */
    refused(SC("initial='a'><script>function f() {} var g = f;</script><state id='a'/>"),
            "variable 'g' cannot be stored: it holds a function", NULL);
}

/* The whole datamodel to bytes and back (ta_datamodel_ecmascript_snapshot/restore):
 * an isolated session's globals, lexical bindings and declarations too. */
static void test_snapshot_isolated(void) {
    ecmascript_system t;
    CHECK(ecmascript_open(&t, STORE_CHART, NULL));
    ecmascript_run(&t);
    ecmascript_send(&t, "add", "{\"k\": 5}");
    uint32_t a = ecmascript_actor(&t);
    ta_actor_info actor_info;
    actor_info.size = sizeof actor_info;
    CHECK_EQUAL(ta_actor_info_get(t.system, a, &actor_info), TA_OK);
    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(t.ecmascript, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    void *snap = malloc(length);
    written = (ta_buffer){snap, length, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(t.ecmascript, &written), TA_OK);
    length = written.length;
    ta_datamodel_ecmascript *ecmascript2 = ta_datamodel_ecmascript_new(NULL);
    const ta_datamodel_vtable *vtable = ta_datamodel_ecmascript_vtable();
    ta_datamodel_chart chart = {sizeof chart, actor_info.chart, &t.image, TA_BINDING_EARLY, 0};
    void *cc = NULL;
    CHECK_EQUAL(vtable->chart_attach(ecmascript2, &chart, &cc), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(ecmascript2, (ta_bytes){snap, length}), TA_OK);
    static const char *const want[][2] = {
        {"total + ':' + items.join() + ':' + LIMIT + ':' + later", "\"15:a,5:3:2\""},
        {"add(1) + ':' + self() + ':' + _sessionid", "\"16:true:1\""},
        {"(function () { try { return dead; } catch (e) { return e.name; } })()",
         "\"ReferenceError\""}};
    char buffer[256];
    for (int i = 0; i < 3; i++) {
        size_t n = eval_text(
            ecmascript2, actor_info.datamodel_instance,
            (ta_string_view){want[i][0], (uint32_t)strlen(want[i][0])}, buffer, sizeof buffer);
        CHECK(n != (size_t)-1 && strcmp(buffer, want[i][1]) == 0);
        if (n == (size_t)-1 || strcmp(buffer, want[i][1])) fprintf(stderr, "  got %s\n", buffer);
    }
    /* stored again from the restored datamodel */
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(ecmascript2, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    vtable->destroy(&(ta_datamodel_call){ecmascript2, actor_info.datamodel_instance, 0});
    vtable->chart_detach(ecmascript2, cc);
    ta_datamodel_ecmascript_free(ecmascript2);
    free(snap);
    ecmascript_close(&t);
}

/* A host that builds a system per actor (the PostgreSQL binding: adopt,
 * process, export, deinit) attaches the same chart thousands of times to one
 * datamodel: attached charts are shared and freed or kept idle (bounded),
 * never accumulated — the heap stays flat; distinct images are bounded too;
 * and a chart in use is never shared through an image that can go away. */
static ta_system *system_for(const ta_image *image, ta_datamodel_binding *binding, void **memory) {
    ta_system_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.max_actors = 4;
    size_t need;
    ta_system *system = NULL;
    if (ta_system_layout(image, &config, &need) != TA_OK) return NULL;
    *memory = malloc(need);
    if (ta_system_init(&(ta_system_setup){{*memory, need}, image, &config, binding, 1, 0},
                       &system) != TA_OK ||
        ta_system_boot(system) != TA_OK)
        return NULL;
    return system;
}

static void cycle(ta_datamodel_ecmascript *ecmascript, const ta_image *image) {
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    void *memory = NULL;
    ta_system *system = system_for(image, &binding, &memory);
    CHECK(system != NULL);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(system, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"touch", 5};
    CHECK_EQUAL(ta_system_send(system, &event), TA_OK);
    for (int i = 0; i < 10 && ta_system_run(system, 100000).status != TA_RUN_IDLE; i++)
        ;
    const ta_effect *effect;
    ta_system_effects_ack(system, ta_system_effects(system, &effect));
    ta_system_deinit(system);
    free(memory);
}

static void *image_of_text(const char *text, ta_image *image) {
    size_t length;
    void *bytes = compile_chart("es.scxml", text, NULL, &length, 0);
    CHECK(bytes && ta_image_open((ta_bytes){bytes, length}, image) == TA_OK);
    return bytes;
}

static void test_system_per_actor(void) {
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    JSRuntime *rt = (JSRuntime *)ta_datamodel_ecmascript_js_runtime(ecmascript);
    const char *text =
        SC("initial='a'><datamodel><data id='ids' expr='[]'/><data id='n' expr='0'/></datamodel>"
           "<state id='a'><transition event='touch' cond='ids.length &lt; 16' target='a'>"
           "<script>ids.push(n)</script><assign location='n' expr='n + 1'/></transition></state>");
    ta_image image;
    void *bytes = image_of_text(text, &image);
    for (int i = 0; i < 50; i++)
        cycle(ecmascript, &image);
    JS_RunGC(rt);
    size_t warm = ta_datamodel_ecmascript_heap_bytes(ecmascript);
    for (int i = 0; i < 2000; i++)
        cycle(ecmascript, &image);
    JS_RunGC(rt);
    size_t after = ta_datamodel_ecmascript_heap_bytes(ecmascript);
    CHECK(after <= warm + 4096); /* was: a chart (and its compiled expressions) per system */

    /* 40 distinct charts, twice round: at most ECMASCRIPT_IDLE_CHARTS stay idle */
    enum { K = 40 };
    void *many[K];
    ta_image images[K];
    for (int k = 0; k < K; k++) {
        char t[1024];
        snprintf(t, sizeof t,
                 SC("initial='a'><datamodel><data id='n' expr='%d'/></datamodel><state id='a'>"
                    "<transition event='touch' cond='n &gt;= 0' target='a'>"
                    "<assign location='n' expr='n + %d'/></transition></state>"),
                 k, k + 1);
        many[k] = image_of_text(t, &images[k]);
    }
    for (int k = 0; k < K; k++)
        cycle(ecmascript, &images[k]);
    JS_RunGC(rt);
    size_t round1 = ta_datamodel_ecmascript_heap_bytes(ecmascript);
    for (int k = 0; k < K; k++)
        cycle(ecmascript, &images[k]);
    JS_RunGC(rt);
    CHECK(ta_datamodel_ecmascript_heap_bytes(ecmascript) <= round1 + 4096);
    for (int k = 0; k < K; k++)
        free(many[k]);

    /* two live systems over two copies of one image: the first one's image
     * is freed while the second still runs (ASan: no use after free) */
    ta_image a, b2;
    void *ba = image_of_text(text, &a), *bb = image_of_text(text, &b2);
    ta_datamodel_binding bind = ta_datamodel_ecmascript_binding(ecmascript);
    void *memory_a = NULL, *memory_b = NULL;
    ta_system *sa = system_for(&a, &bind, &memory_a), *sb = system_for(&b2, &bind, &memory_b);
    CHECK(sa && sb);
    ta_system_deinit(sa);
    free(memory_a);
    memset(ba, 0, 64); /* the first copy is gone */
    free(ba);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(sb, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"touch", 5};
    CHECK_EQUAL(ta_system_send(sb, &event), TA_OK);
    CHECK_EQUAL(ta_system_run(sb, 100000).status, TA_RUN_IDLE);
    ta_system_deinit(sb);
    free(memory_b);
    free(bb);
    free(bytes);
    ta_datamodel_ecmascript_free(ecmascript); /* JS_FreeRuntime asserts nothing leaked */
}

/* ---- neutral values against the shared vectors (tinyactors-test/suites/protocol/values.txt) ---------- */
#ifndef TA_VALUES
#define TA_VALUES "../tinyactors-test/suites/protocol/values.txt"
#endif

static char *slurp_file(const char *path, size_t *length) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = (char *)malloc((size_t)n + 1);
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    if (b) b[n] = 0;
    *length = (size_t)n;
    return b;
}

/* One case of values.txt: its kind and name, and the lines read so far. */
typedef struct values_case {
    char kind[16], name[128], expression[256];
    unsigned char cbor[256], back[256];
    size_t cbor_length, back_length;
    int has_cbor, has_back;
} values_case;

static size_t values_hex(const char *hex, unsigned char *out, size_t capacity) {
    size_t length = 0;
    for (; hex[0] && hex[1] && length < capacity; hex += 2) {
        unsigned value = 0;
        for (int k = 0; k < 2; k++)
            value = value * 16 + (unsigned)(hex[k] <= '9' ? hex[k] - '0' : (hex[k] | 0x20) - 'a' + 10);
        out[length++] = (unsigned char)value;
    }
    return length;
}

static void values_run(ta_datamodel_ecmascript *ecmascript, const values_case *c) {
    const ta_datamodel_vtable *vtable = ta_datamodel_ecmascript_vtable();
    ta_payload payload = 0;
    if (!strcmp(c->kind, "roundtrip") || !strcmp(c->kind, "malformed")) {
        ta_status status =
            vtable->from_wire(ecmascript, (ta_bytes){c->cbor, c->cbor_length}, &payload);
        if (!strcmp(c->kind, "malformed")) {
            if (status != TA_ERROR_INVALID) fprintf(stderr, "  values.txt \"%s\" was read\n", c->name);
            CHECK_EQUAL(status, TA_ERROR_INVALID);
            CHECK(payload == 0);
            return;
        }
        CHECK_EQUAL(status, TA_OK);
    } else {
        payload = ta_datamodel_ecmascript_payload_eval(
            ecmascript, 0, (ta_string_view){c->expression, (uint32_t)strlen(c->expression)});
        CHECK(payload != 0);
    }
    ta_bytes out = {NULL, 0}; /* the datamodel's view */
    ta_status status = vtable->to_wire(ecmascript, payload, &out);
    const unsigned char *written = (const unsigned char *)out.pointer;
    if (!strcmp(c->kind, "invalid")) {
        CHECK_EQUAL(status, TA_ERROR_INVALID);
    } else {
        const unsigned char *want = c->has_back ? c->back : c->cbor;
        size_t length = c->has_back ? c->back_length : c->cbor_length;
        int same = status == TA_OK && out.length == length && !memcmp(written, want, length);
        if (!same) {
            fprintf(stderr, "  values.txt %s \"%s\": status %d, got ", c->kind, c->name, (int)status);
            for (size_t k = 0; status == TA_OK && k < out.length; k++)
                fprintf(stderr, "%02x", written[k]);
            fprintf(stderr, "\n");
        }
        CHECK(same);
    }
    if (payload) vtable->release(ecmascript, payload);
}

/* A case to run here: every one but those for a language with a reference
 * type and a second empty value ("not ECMAScript: ..."). */
static int values_for_ecmascript(const values_case *c) {
    static const char other[] = "not ECMAScript:";
    return c->kind[0] && strncmp(c->name, other, sizeof other - 1) != 0;
}

static void test_wire_values(void) {
    size_t n = 0;
    char *text = slurp_file(TA_VALUES, &n);
    CHECK(text != NULL);
    if (!text) return;
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    const ta_datamodel_vtable *vtable = ta_datamodel_ecmascript_vtable();
    CHECK(vtable->size >= offsetof(ta_datamodel_vtable, from_wire) + sizeof vtable->from_wire);
    values_case c;
    memset(&c, 0, sizeof c);
    int cases = 0, diagnostics = 0, last_back = 0;
    for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (line[0] == '#' || !line[0]) continue;
        char *space = strchr(line, ' ');
        if (!space) continue;
        *space = 0;
        const char *rest = space + 1;
        if (!strcmp(line, "cbor")) {
            c.cbor_length = values_hex(rest, c.cbor, sizeof c.cbor);
            c.has_cbor = 1;
            last_back = 0;
        } else if (!strcmp(line, "back")) {
            c.back_length = values_hex(rest, c.back, sizeof c.back);
            c.has_back = 1;
            last_back = 1;
        } else if (!strcmp(line, "ecmascript")) {
            snprintf(c.expression, sizeof c.expression, "%s", rest);
        } else if (!strcmp(line, "python")) {
            /* the Python binding's: the same value made in Python */
        } else if (!strcmp(line, "diag")) {
            char got[512];
            ta_buffer out = {got, sizeof got - 1, 0};
            ta_bytes bytes = last_back ? (ta_bytes){c.back, c.back_length}
                                       : (ta_bytes){c.cbor, c.cbor_length};
            CHECK_EQUAL(ta_cbor_diagnostic(bytes, &out), TA_OK);
            got[out.length < sizeof got - 1 ? out.length : sizeof got - 1] = 0;
            if (strcmp(got, rest))
                fprintf(stderr, "  values.txt \"%s\": diagnostic %s, expected %s\n", c.name, got,
                        rest);
            CHECK(strcmp(got, rest) == 0);
            diagnostics++;
        } else { /* a new case: run the one before */
            if (values_for_ecmascript(&c)) values_run(ecmascript, &c), cases++;
            memset(&c, 0, sizeof c);
            snprintf(c.kind, sizeof c.kind, "%s", line);
            snprintf(c.name, sizeof c.name, "%s", rest);
            CHECK(!strcmp(line, "roundtrip") || !strcmp(line, "encode") ||
                  !strcmp(line, "invalid") || !strcmp(line, "malformed"));
        }
    }
    if (values_for_ecmascript(&c)) values_run(ecmascript, &c), cases++;
    CHECK(cases > 30);
    CHECK(diagnostics > 20);
    ta_datamodel_ecmascript_free(ecmascript);
    free(text);
}

/* The host's abort flag stops JavaScript at once (a database cancel), and
 * JavaScript work is charged as fuel: a long script costs what it does. */
static void test_abort_flag_and_fuel(void) {
    volatile int stop = 0;
    ta_datamodel_ecmascript_config config = {
        sizeof config, 0, 0, 0, 0, (const volatile int *)&stop};
    ecmascript_system t;
    CHECK(ecmascript_open(&t,
                          SC("initial='a'><state id='a'><onentry><script>while (true) {}</script>"
                             "</onentry><transition event='error.execution' target='b'/></state>"
                             "<state id='b'/>"),
                          &config));
    stop = 1;
    ecmascript_run(&t); /* without the flag this would run for 10^10 operations */
    stop = 0;
    {
        uint32_t a = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8}), states[8];
        uint32_t n = ta_actor_config(t.system, a, (ta_uint32_array){states, 8});
        CHECK(n == 1 && states[0] == ta_image_state_by_name(&t.image, 0, (ta_string_view){"b", 1}));
    }
    ecmascript_close(&t);
    /* fuel: ~10^7 operations cost ~10^5 fuel */
    CHECK(ecmascript_open(
        &t,
        SC("initial='a'><datamodel><data id='n' expr='0'/></datamodel>"
           "<state id='a'><transition event='go'><script>for (let i = 0; i &lt; 1e7; i++) "
           "n++;</script></transition></state>"),
        NULL));
    ecmascript_run(&t);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8});
    event.name = (ta_string_view){"go", 2};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    uint64_t used = 0;
    for (int i = 0; i < 1000; i++) {
        ta_run result = ta_system_run(t.system, 1000000);
        used += result.used;
        if (result.status == TA_RUN_IDLE) break;
    }
    CHECK(used > 50000);
    ecmascript_close(&t);
}

/* A value too deep to read back is refused when stored (serialize fails
 * cleanly) instead of producing a blob that cannot be loaded again. */
static void test_deep_value_not_stored(void) {
    /* a small engine stack (the wasm build's native stack is smaller than the
     * engine's default; the rule is the same at any size) */
    ta_datamodel_ecmascript_config config = {sizeof config, 0, 0, 64 * 1024, 0, NULL};
    ecmascript_system t;
    CHECK(ecmascript_open(&t,
                          SC("initial='a'><datamodel><data id='deep' expr='null'/></datamodel>"
                             "<state id='a'><transition event='build'><script>var v = []; "
                             "for (var i = 0; i &lt; 5000; i++) v = [v]; deep = v;</script>"
                             /* shallow enough for any build's frames: how many
                              * levels fit in a quarter of 64 KiB depends on the
                              * compiler and the sanitizers (about 30 with
                              * clang 22, AddressSanitizer and -O1) */
                             "</transition><transition event='shallow'><script>v = []; "
                             "for (var i = 0; i &lt; 5; i++) v = [v]; deep = v;</script>"
                             "</transition></state>"),
                          &config));
    ecmascript_run(&t);
    uint32_t a = ta_system_actor_by_name(t.system, (ta_string_view){"es.scxml", 8});
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"build", 5};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    static unsigned char blob[1 << 20];
    size_t length = 0;
    ta_buffer written = {blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, &written), TA_ERROR_UNSUPPORTED);
    length = written.length;
    CHECK(strstr(ta_datamodel_ecmascript_last_error(t.ecmascript),
                 "variable 'deep' cannot be stored") != NULL);
    event.name = (ta_string_view){"shallow", 7};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    ecmascript_run(&t);
    written = (ta_buffer){blob, sizeof blob, 0};
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    ecmascript_close(&t);
}

int main(void) {
    RUN(test_expressions_and_script);
    RUN(test_errors_and_readonly);
    RUN(test_runaway_script_interrupted);
    RUN(test_error_execution_says_why);
    RUN(test_events_payloads_xml);
    RUN(test_heap_bytes);
    RUN(test_traced_values_are_let_go);
    RUN(test_shared_realm);
    RUN(test_eval_variables);
    RUN(test_toplevel_declarations_isolate);
    RUN(test_executable_scripts_run_again);
    RUN(test_store_declarations);
    RUN(test_store_refuses_precisely);
    RUN(test_snapshot_isolated);
    RUN(test_system_per_actor);
    RUN(test_wire_values);
    RUN(test_abort_flag_and_fuel);
    RUN(test_deep_value_not_stored);
    TEST_MAIN_END();
}
