/* Hibernation: the host unloads idle actors (ta_actor_hibernate -> blob) and
 * brings them back when the system asks (TA_EFFECT_WAKE -> ta_actor_wake). */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/system_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "../support/payload_text.h"
#include "quickjs.h"

/* Run until idle; record which actors asked to be woken. */
static uint32_t wakes[64];
static int wakes_seen;
static int changed_seen; /* TA_EFFECT_CHANGED (ta_actor_watch) */

static void drain_all(test_system *t) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t->system, &effect);
    for (uint32_t i = 0; i < n; i++) {
        if (effect[i].kind == TA_EFFECT_WAKE && wakes_seen < 64)
            wakes[wakes_seen++] = effect[i].actor;
        if (effect[i].kind == TA_EFFECT_LOG) tsys_drain_one(t, &effect[i]);
        if (effect[i].kind == TA_EFFECT_CHANGED) changed_seen++;
        if (effect[i].kind < 8) t->effect_count[effect[i].kind]++;
    }
    ta_system_effects_ack(t->system, n);
}

static uint32_t run_all(test_system *t) {
    ta_run result;
    result.status = TA_RUN_IDLE;
    for (int i = 0; i < 50; i++) {
        result = ta_system_run(t->system, 100000);
        drain_all(t);
        if (result.status == TA_RUN_IDLE) break;
    }
    return result.status;
}

static unsigned char blob[1 << 16];

static size_t hibernate(test_system *t, uint32_t actor, ta_status *status) {
    ta_buffer out = {blob, sizeof blob, 0};
    *status = ta_actor_hibernate(t->system, actor, &out);
    return out.length;
}

static ta_chart_stats chart_stats(ta_system *system) {
    ta_chart_stats out;
    out.size = sizeof out;
    CHECK_EQUAL(ta_system_chart_stats_get(system, 0, &out), TA_OK);
    return out;
}

static ta_system_stats stats(ta_system *system) {
    ta_system_stats stats_now;
    stats_now.size = sizeof stats_now;
    ta_system_stats_get(system, &stats_now);
    return stats_now;
}

#define NULLSC(body)                                                                               \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' " body "</scxml>"

static void test_roundtrip_and_single_wake(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    NULLSC("initial='a'><state id='a'><transition event='go' target='b'/></state>"
                           "<state id='b'><transition event='ping'><log expr=\"'pong'\"/>"
                           "</transition><transition event='tick' target='c'/></state>"
                           "<state id='c'><onentry><log expr=\"'in c'\"/></onentry></state>"),
                    NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "main.scxml");
    run_all(&t);
    CHECK_EQUAL(tsys_send(&t, a, "go", NULL), TA_OK);
    run_all(&t);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0);
    uint32_t pages_before = stats(t.system).slab_pages;

    ta_buffer small = {blob, 8, 0};
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, &small), TA_ERROR_NO_MEMORY); /* too small */
    size_t need = small.length;
    CHECK(need > 8);
    ta_status status;
    size_t length = hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(length, need);
    ta_system_stats stats_now = stats(t.system);
    CHECK_EQUAL(stats_now.hibernated, 1);
    CHECK_EQUAL(stats_now.hibernations, 1);
    CHECK(chart_stats(t.system).actors == 1 && chart_stats(t.system).hibernated == 1);
    CHECK(stats_now.slab_pages <= pages_before);
    ta_actor_info info;
    info.size = sizeof info;
    CHECK_EQUAL(ta_actor_info_get(t.system, a, &info), TA_OK);
    CHECK_EQUAL(info.state, TA_ACTOR_HIBERNATED);
    CHECK(strcmp(tsys_config(&t, a), "") == 0); /* the configuration is in the blob */
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, &(ta_buffer){blob, sizeof blob, 0}),
                TA_ERROR_STATE);

    /* events wait in the mailbox; one wake request however many arrive */
    wakes_seen = 0;
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
    run_all(&t);
    CHECK_EQUAL(wakes_seen, 1);
    CHECK(wakes_seen == 1 && wakes[0] == a);
    CHECK(strcmp(t.log, "") == 0);
    CHECK_EQUAL(stats(t.system).wakes_pending, 1);

    /* a blob for someone else is refused */
    unsigned char bad[1 << 10];
    memcpy(bad, blob, length < sizeof bad ? length : sizeof bad);
    bad[32] ^= 1; /* serial */
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){bad, length}), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, 10}), TA_ERROR_INVALID);

    /* relocation keeps the hibernated actor (and its mailbox) */
    ta_system_config config = t.system->config;
    config.max_actors *= 2;
    size_t nl;
    CHECK_EQUAL(ta_system_layout(&t.image, &config, &nl), TA_OK);
    void *nm = malloc(nl);
    ta_system *new_system = t.system;
    CHECK_EQUAL(ta_system_relocate(&new_system, (ta_memory){nm, nl}, &config), TA_OK);
    free(t.memory);
    t.memory = nm;
    t.system = new_system;

    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    stats_now = stats(t.system);
    CHECK(stats_now.hibernated == 0 && stats_now.wakes == 1 && stats_now.wakes_pending == 0);
    /* the per-chart gauges moved with the relocation and the wake */
    CHECK(chart_stats(t.system).actors == 1 && chart_stats(t.system).hibernated == 0);
    run_all(&t);
    CHECK(strcmp(t.log, "pong;pong;") == 0);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0);
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}),
                TA_ERROR_STATE); /* not hibernated */

    /* again, and woken proactively by the host (no request was needed) */
    hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "tick", NULL), TA_OK);
    run_all(&t);
    CHECK(strcmp(tsys_config(&t, a), "c") == 0);
    CHECK(strstr(t.log, "in c;") != NULL);
    /* finalizing with a hibernated actor and a pending wake leaks nothing */
    hibernate(&t, a, &status);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
    tsys_free(&t);
}

/* A watch ends when the actor hibernates: waking it does not bring back a
 * watch its embedder may have dropped meanwhile; the embedder watches it
 * again before its next run if it still wants to. */
static void test_watch_ends_with_hibernation(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    NULLSC("initial='a'><state id='a'><transition event='ping'>"
                           "<log expr=\"'pong'\"/></transition></state>"),
                    NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "main.scxml");
    run_all(&t);
    changed_seen = 0;
    CHECK_EQUAL(ta_actor_watch(t.system, a, 1), TA_OK);
    ta_status status;
    size_t length = hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(ta_actor_watch(t.system, a, 1), TA_ERROR_STALE); /* hibernated */
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
    run_all(&t);
    CHECK_EQUAL(changed_seen, 0); /* the watch did not survive */
    CHECK_EQUAL(ta_actor_watch(t.system, a, 1), TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
    run_all(&t);
    CHECK_EQUAL(changed_seen, 1); /* watched again */
    tsys_free(&t);
}

static void test_timer_wakes_actor(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    NULLSC("initial='w'><state id='w'><onentry><send event='t' delay='50ms'/>"
                           "</onentry><transition event='t' target='fired'/></state>"
                           "<state id='fired'><onentry><log expr=\"'fired'\"/></onentry></state>"),
                    NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "main.scxml");
    run_all(&t);
    ta_status status;
    size_t length = hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK); /* a pending delayed send stays resident */
    wakes_seen = 0;
    ta_system_advance_time(t.system, 100);
    run_all(&t);
    CHECK_EQUAL(wakes_seen, 1);
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    run_all(&t);
    CHECK(strcmp(t.log, "fired;") == 0);
    tsys_free(&t);
}

/* Parents with invoked children stay resident; a hibernated child that gets
 * cancelled (or terminated) is woken first so it can exit normally. */
static void test_children_cancel_terminate(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    NULLSC("initial='p'><state id='p'><invoke id='kid'><content><scxml "
                           "xmlns='http://www.w3.org/2005/07/scxml' datamodel='null' "
                           "initial='k'><state id='k'/></scxml></content></invoke>"
                           "<transition event='stop' target='q'/></state><state id='q'>"
                           "<transition event='done.invoke.kid'><log expr=\"'unexpected'\"/>"
                           "</transition></state>"),
                    NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t p = tsys_actor(&t, "main.scxml");
    run_all(&t);
    CHECK_EQUAL(stats(t.system).actors, 2);
    ta_status status;
    hibernate(&t, p, &status);
    CHECK_EQUAL(status, TA_ERROR_STATE); /* children read the parent's invoke slots */
    uint32_t kid = 0;
    for (uint32_t i = 0; i < t.system->actors.high_water_mark && !kid; i++)
        if (ta_pool_live(&t.system->actors, i) && ta_actor_at(t.system, i)->parent)
            kid = ta_pool_handle(&t.system->actors, i);
    CHECK(kid != 0);
    size_t length = hibernate(&t, kid, &status);
    CHECK_EQUAL(status, TA_OK);
    wakes_seen = 0;
    CHECK_EQUAL(tsys_send(&t, p, "stop", NULL), TA_OK); /* exits the invoking state */
    run_all(&t);
    CHECK_EQUAL(wakes_seen, 1);
    CHECK(wakes_seen == 1 && wakes[0] == kid);
    CHECK_EQUAL(stats(t.system).actors, 2); /* still waiting for its blob */
    CHECK_EQUAL(ta_actor_wake(t.system, kid, (ta_bytes){blob, length}), TA_OK);
    run_all(&t);
    CHECK_EQUAL(stats(t.system).actors, 1);
    CHECK(strcmp(t.log, "") == 0); /* cancelled: no done.invoke */

    /* host terminate of a hibernated actor */
    length = hibernate(&t, p, &status);
    CHECK_EQUAL(status, TA_OK);
    wakes_seen = 0;
    CHECK_EQUAL(ta_system_terminate(t.system, p), TA_OK);
    run_all(&t);
    CHECK_EQUAL(wakes_seen, 1);
    CHECK_EQUAL(ta_actor_wake(t.system, p, (ta_bytes){blob, length}), TA_OK);
    run_all(&t);
    CHECK_EQUAL(stats(t.system).actors, 0);
    tsys_free(&t);
}

/* error.communication reported while hibernated is internal: it is handled
 * before external events that were already waiting. */
static void test_error_communication_order(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    NULLSC("initial='s'><state id='s'><transition event='error.communication'>"
                           "<log expr=\"'err'\"/></transition><transition event='ext'>"
                           "<log expr=\"'ext'\"/></transition></state>"),
                    NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "main.scxml");
    run_all(&t);
    ta_status status;
    size_t length = hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "ext", NULL), TA_OK);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    event.sendid = (ta_string_view){"s1", 2};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    run_all(&t);
    CHECK_EQUAL(ta_actor_wake(t.system, a, (ta_bytes){blob, length}), TA_OK);
    run_all(&t);
    CHECK(strcmp(t.log, "err;ext;") == 0);
    tsys_free(&t);
}

static void test_cold_sweep(void) {
    test_system t;
    CHECK(tsys_init(&t, NULLSC("initial='s'><state id='s'><transition event='x'/></state>"), NULL,
                    NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t ids[10];
    ids[0] = tsys_actor(&t, "main.scxml");
    for (int i = 1; i < 10; i++)
        CHECK_EQUAL(
            ta_system_spawn(t.system,
                            &(ta_spawn){0, 0, (ta_string_view){"", 0}, 0, (ta_string_view){0, 0}},
                            &ids[i]),
            TA_OK);
    run_all(&t);
    uint32_t out[16];
    ta_sweep sweep = {0, 64};
    ta_uint32_array cold = {out, 16};
    CHECK_EQUAL(ta_system_cold_actors(t.system, &sweep, cold), 0); /* all just ran */
    CHECK_EQUAL(ta_system_cold_actors(t.system, &sweep, cold), 10);
    CHECK_EQUAL(tsys_send(&t, ids[3], "x", NULL), TA_OK);
    run_all(&t);
    uint32_t n = ta_system_cold_actors(t.system, &sweep, cold);
    CHECK_EQUAL(n, 9);
    for (uint32_t i = 0; i < n; i++)
        CHECK(out[i] != ids[3]);
    /* bounded scan: max_scan slots, the cursor carries on */
    sweep = (ta_sweep){0, 4};
    CHECK_EQUAL(ta_system_cold_actors(t.system, &sweep, cold), 4);
    CHECK_EQUAL(sweep.cursor, 4);
    sweep.max_scan = 64;
    CHECK_EQUAL(ta_system_cold_actors(t.system, &sweep, (ta_uint32_array){out, 2}),
                2); /* capacity */
    tsys_free(&t);
}

/* ---- ECMAScript: data, _event and system variables survive ---- */

typedef struct ecmascript_test {
    test_system test_system; /* reuses the image/memory fields; datamodel = ECMAScript */
    ta_datamodel_ecmascript *ecmascript;
} ecmascript_test;

static int ecmascript_open(ecmascript_test *x, const char *chart) {
    memset(x, 0, sizeof *x);
    test_system *t = &x->test_system;
    t->bytes = compile_chart("es.scxml", chart, NULL, &t->length, 0);
    if (!t->bytes || ta_image_open((ta_bytes){t->bytes, t->length}, &t->image) != TA_OK) return 0;
    x->ecmascript = ta_datamodel_ecmascript_new(NULL);
    t->binding = ta_datamodel_ecmascript_binding(x->ecmascript);
    ta_system_config config = test_system_config();
    if (ta_system_layout(&t->image, &config, &t->memory_length) != TA_OK) return 0;
    t->memory = malloc(t->memory_length);
    return ta_system_init(
               &(ta_system_setup){
                   {t->memory, t->memory_length}, &t->image, &config, &t->binding, 1, 0},
               &t->system) == TA_OK &&
           ta_system_boot(t->system) == TA_OK;
}

static const char *ecmascript_eval(ecmascript_test *x, uint32_t actor, const char *expression) {
    static char buffer[512];
    uint32_t index = ta_system_resolve_actor(x->test_system.system, actor);
    if (index == TA_NIL || ta_actor_at(x->test_system.system, index)->state == TA_ACTOR_HIBERNATED)
        return "<n/a>";
    uint64_t instance =
        ta_actor_instance(x->test_system.system, ta_actor_at(x->test_system.system, index))
            ->datamodel;
    if (eval_text(
            x->ecmascript, instance, (ta_string_view){expression, (uint32_t)strlen(expression)},
            buffer, sizeof buffer) == (size_t)-1)
        return "<error>";
    return buffer;
}

static void ecmascript_close(ecmascript_test *x) {
    ta_system_deinit(x->test_system.system);
    ta_datamodel_ecmascript_free(x->ecmascript);
    free(x->test_system.memory);
    free(x->test_system.bytes);
}

#define ESSC(body)                                                                                 \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

static int64_t js_malloc_size(ta_datamodel_ecmascript *ecmascript) {
    JSRuntime *rt = (JSRuntime *)ta_datamodel_ecmascript_js_runtime(ecmascript);
    JS_RunGC(rt);
    JSMemoryUsage mu;
    JS_ComputeMemoryUsage(rt, &mu);
    return mu.malloc_size;
}

static void test_ecmascript_state_survives(void) {
    ecmascript_test x;
    CHECK(ecmascript_open(
        &x, ESSC("initial='a'><datamodel><data id='ids' expr='[1, 2]'/>"
                 "<data id='m' expr='new Map([[\"k\", {deep: [7]}]])'/>"
                 "<data id='cyc' expr='(function () { const o = {}; o.self = o; "
                 "return o; })()'/></datamodel>"
                 "<state id='a'><transition event='add'>"
                 "<assign location='ids' expr='ids.concat([_event.data.v])'/>"
                 "</transition><transition event='show'><log label='show' "
                 "expr='ids.join(\",\") + \"|\" + m.get(\"k\").deep[0] + \"|\" + "
                 "(cyc.self === cyc) + \"|\" + _event.name'/></transition></state>")));
    test_system *t = &x.test_system;
    uint32_t a = ta_system_actor_by_name(t->system, (ta_string_view){"es.scxml", 8});
    run_all(t);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.name = (ta_string_view){"add", 3};
    event.data =
        ta_datamodel_ecmascript_payload_eval(x.ecmascript, 0, (ta_string_view){"{\"v\": 3}", 8});
    CHECK_EQUAL(ta_system_send(t->system, &event), TA_OK);
    run_all(t);
    char sid[64];
    snprintf(sid, sizeof sid, "%s", ecmascript_eval(&x, a, "_sessionid"));
    int64_t before = js_malloc_size(x.ecmascript);
    ta_status status;
    size_t length = hibernate(t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    /* the scope and its values are freed (QuickJS keeps its arenas, so the
     * datamodel's malloc-level heap_bytes only shrinks in arena steps) */
    CHECK(js_malloc_size(x.ecmascript) < before - 1000);
    CHECK_EQUAL(ta_actor_wake(t->system, a, (ta_bytes){blob, length}), TA_OK);
    /* the same _event (still "add"), data, and system variables */
    CHECK(strcmp(ecmascript_eval(&x, a, "_event.name + ':' + _event.data.v"), "\"add:3\"") == 0);
    CHECK(strcmp(ecmascript_eval(&x, a, "_sessionid"), sid) == 0);
    CHECK(strcmp(ecmascript_eval(&x, a, "typeof _ioprocessors.scxml.location"), "\"string\"") == 0);
    event.name = (ta_string_view){"show", 4};
    event.data = 0;
    CHECK_EQUAL(ta_system_send(t->system, &event), TA_OK);
    run_all(t);
    CHECK(strcmp(t->log, "show=1,2,3|7|true|show;") == 0);
    if (strcmp(t->log, "show=1,2,3|7|true|show;")) fprintf(stderr, "  log: %s\n", t->log);
    /* read-only system variables stay read-only after a wake */
    CHECK(strcmp(ecmascript_eval(&x, a, "(_sessionid = 1, typeof _sessionid)"), "\"string\"") == 0);
    ecmascript_close(&x);
}

static void test_ecmascript_unsupported(void) {
    ecmascript_test x;
    /* a function in the data cannot be written */
    CHECK(ecmascript_open(&x,
                          ESSC("initial='a'><datamodel><data id='f' expr='(function () { return 1; "
                               "})'/></datamodel><state id='a'/>")));
    uint32_t a = ta_system_actor_by_name(x.test_system.system, (ta_string_view){"es.scxml", 8});
    run_all(&x.test_system);
    ta_status status;
    hibernate(&x.test_system, a, &status);
    CHECK_EQUAL(status, TA_ERROR_UNSUPPORTED);
    CHECK(strstr(ta_datamodel_ecmascript_last_error(x.ecmascript), "variable 'f'") != NULL);
    CHECK(strcmp(ecmascript_eval(&x, a, "f()"), "1") == 0); /* untouched */
    ecmascript_close(&x);
    /* isolated sessions (scripts declaring globals) are stored too */
    CHECK(ecmascript_open(
        &x, ESSC("initial='a'><script>var g = 1; function inc() { return ++g; }</script>"
                 "<state id='a'/>")));
    a = ta_system_actor_by_name(x.test_system.system, (ta_string_view){"es.scxml", 8});
    run_all(&x.test_system);
    size_t length = hibernate(&x.test_system, a, &status);
    CHECK_EQUAL(status, TA_OK);
    if (status != TA_OK)
        fprintf(stderr, "  %s\n", ta_datamodel_ecmascript_last_error(x.ecmascript));
    CHECK_EQUAL(ta_actor_wake(x.test_system.system, a, (ta_bytes){blob, length}), TA_OK);
    CHECK(strcmp(ecmascript_eval(&x, a, "inc()"), "2") == 0);
    ecmascript_close(&x);
}

/* ---- the index of names ---- */

static uint32_t spawn_named(test_system *t, const char *name) {
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.name = (ta_string_view){name, (uint32_t)strlen(name)};
    uint32_t actor = 0;
    CHECK_EQUAL(ta_system_spawn(t->system, &spawn, &actor), TA_OK);
    return actor;
}

static uint32_t by_name(test_system *t, const char *name) {
    return ta_system_actor_by_name(t->system, (ta_string_view){name, (uint32_t)strlen(name)});
}

/* The longest run of occupied slots in the index: a lookup probes at most
 * that many, whatever the population. */
static uint32_t longest_cluster(const ta_system *system) {
    const uint32_t *names = SYSTEM_POINTER(system, names_relative, const uint32_t);
    uint32_t longest = 0, run = 0;
    for (uint32_t i = 0; i < 2 * system->name_capacity; i++) {
        run = names[i & (system->name_capacity - 1)] == TA_NIL ? 0 : run + 1;
        if (run > longest) longest = run;
    }
    return longest;
}

#define NAMED 2000u

/* ta_system_actor_by_name answers from an index of names, not by walking
 * the actors: each distinct name has one slot, holding its oldest live
 * actor, so a lookup costs a hash and a few probes. The answer is the
 * oldest live actor of the name (hibernated ones count), across
 * terminations, hibernation, relocation and a snapshot. */
static void test_names_index(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.max_actors = 4096;
    config.max_events = 4096;
    CHECK(tsys_init(&t,
                    NULLSC("initial='a'><state id='a'><transition event='quit' target='end'/>"
                           "</state><final id='end'/>"),
                    NULL, &config));
    static uint32_t named[NAMED];
    char text[32];
    uint32_t first = spawn_named(&t, ""); /* the chart's name */
    for (uint32_t i = 0; i < NAMED; i++) {
        snprintf(text, sizeof text, "a%u", i);
        named[i] = spawn_named(&t, text);
    }
    uint32_t second = spawn_named(&t, "main.scxml"); /* the chart's name, given */
    uint32_t dup[3] = {spawn_named(&t, "dup"), spawn_named(&t, "dup"), spawn_named(&t, "dup")};
    run_all(&t);
    CHECK_EQUAL(t.system->name_count, NAMED + 2);
    CHECK(longest_cluster(t.system) <= 64); /* load 2002/8192 */
    for (uint32_t i = 0; i < NAMED; i++) {
        snprintf(text, sizeof text, "a%u", i);
        CHECK_EQUAL(by_name(&t, text), named[i]);
    }
    CHECK_EQUAL(by_name(&t, "main.scxml"), first); /* the oldest */
    CHECK_EQUAL(by_name(&t, "dup"), dup[0]);
    CHECK_EQUAL(by_name(&t, "nobody"), 0u);
    CHECK_EQUAL(by_name(&t, ""), 0u);
    /* the oldest ends: the next oldest answers; the middle one ends: the order stays */
    CHECK_EQUAL(ta_system_terminate(t.system, first), TA_OK);
    CHECK_EQUAL(ta_system_terminate(t.system, dup[1]), TA_OK);
    /* every other named actor ends: the slots left behind are filled by
     * moving the later ones of their clusters back, so each is still found */
    for (uint32_t i = 0; i < NAMED; i += 2)
        CHECK_EQUAL(ta_system_terminate(t.system, named[i]), TA_OK);
    run_all(&t);
    CHECK_EQUAL(t.system->name_count, NAMED / 2 + 2);
    CHECK_EQUAL(by_name(&t, "main.scxml"), second);
    CHECK_EQUAL(by_name(&t, "dup"), dup[0]);
    for (uint32_t i = 0; i < NAMED; i++) {
        snprintf(text, sizeof text, "a%u", i);
        CHECK_EQUAL(by_name(&t, text), i % 2 ? named[i] : 0u);
    }
    /* a hibernated actor keeps its name */
    ta_status status;
    size_t blob_length = hibernate(&t, dup[0], &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(by_name(&t, "dup"), dup[0]);
    CHECK_EQUAL(ta_system_terminate(t.system, dup[0]), TA_OK); /* applied at its wake */
    run_all(&t);
    /* the index and the rings move with the region, to a larger index */
    ta_system_config big = config;
    big.max_actors = 3 * config.max_actors;
    size_t need;
    CHECK_EQUAL(ta_system_layout(&t.image, &big, &need), TA_OK);
    void *memory = malloc(need);
    ta_system *moved = t.system;
    CHECK_EQUAL(ta_system_relocate(&moved, (ta_memory){memory, need}, &big), TA_OK);
    free(t.memory);
    t.memory = memory;
    t.system = moved;
    CHECK(t.system->name_capacity > 8192u);
    CHECK_EQUAL(by_name(&t, "dup"), dup[0]); /* hibernated, terminated when woken */
    CHECK_EQUAL(by_name(&t, "main.scxml"), second);
    for (uint32_t i = 1; i < NAMED; i += 2) {
        snprintf(text, sizeof text, "a%u", i);
        CHECK_EQUAL(by_name(&t, text), named[i]);
    }
    uint32_t dup3 = spawn_named(&t, "dup");
    CHECK_EQUAL(ta_actor_wake(t.system, dup[0], (ta_bytes){blob, blob_length}), TA_OK);
    run_all(&t);
    CHECK_EQUAL(by_name(&t, "dup"), dup[2]); /* dup[0] ended at its wake; dup[2] came before dup3 */
    CHECK_EQUAL(ta_system_terminate(t.system, dup[2]), TA_OK);
    run_all(&t);
    CHECK_EQUAL(by_name(&t, "dup"), dup3);
    /* a snapshot holds the index as it is */
    ta_buffer out = {NULL, 0, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_ERROR_NO_MEMORY);
    void *snap = malloc(out.length);
    out = (ta_buffer){snap, out.length, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_OK);
    void *memory2 = malloc(out.length);
    ta_system *restored;
    CHECK_EQUAL(ta_system_restore(&(ta_system_setup){{memory2, out.length}, &t.image, NULL,
                                                      &t.binding, 1, 0},
                                  (ta_bytes){snap, out.length}, &restored),
                TA_OK);
    CHECK_EQUAL(ta_system_actor_by_name(restored, (ta_string_view){"dup", 3}), dup3);
    CHECK_EQUAL(ta_system_actor_by_name(restored, (ta_string_view){"a1999", 5}), named[1999]);
    free(memory2); /* its datamodel handles alias the original's: no deinit */
    free(snap);
    tsys_free(&t);
}

int main(void) {
    RUN(test_roundtrip_and_single_wake);
    RUN(test_watch_ends_with_hibernation);
    RUN(test_timer_wakes_actor);
    RUN(test_children_cancel_terminate);
    RUN(test_error_communication_order);
    RUN(test_cold_sweep);
    RUN(test_names_index);
    RUN(test_ecmascript_state_survives);
    RUN(test_ecmascript_unsupported);
    TEST_MAIN_END();
}
