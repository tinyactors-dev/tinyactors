/* Hibernation: the host unloads idle actors (ta_actor_hibernate -> blob) and
 * brings them back when the system asks (TA_EFFECT_WAKE -> ta_actor_wake). */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/system_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include "quickjs.h"

/* Run until idle; record which actors asked to be woken. */
static uint32_t wakes[64];
static int wakes_seen;

static void drain_all(test_system *t) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t->system, &effect);
    for (uint32_t i = 0; i < n; i++) {
        if (effect[i].kind == TA_EFFECT_WAKE && wakes_seen < 64)
            wakes[wakes_seen++] = effect[i].actor;
        if (effect[i].kind == TA_EFFECT_LOG) tsys_drain_one(t, &effect[i]);
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
    size_t length = 0;
    *status = ta_actor_hibernate(t->system, actor, blob, sizeof blob, &length);
    return length;
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

    size_t need = 0;
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, blob, 8, &need),
                TA_ERROR_NO_MEMORY); /* too small */
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
    CHECK_EQUAL(ta_actor_hibernate(t.system, a, blob, sizeof blob, &need), TA_ERROR_STATE);

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
    CHECK_EQUAL(ta_actor_wake(t.system, a, bad, length), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, 10), TA_ERROR_INVALID);

    /* relocation keeps the hibernated actor (and its mailbox) */
    ta_system_config config = t.system->config;
    config.max_actors *= 2;
    size_t nl;
    CHECK_EQUAL(ta_system_layout(&t.image, &config, &nl), TA_OK);
    void *nm = malloc(nl);
    ta_system *new_system;
    CHECK_EQUAL(ta_system_relocate(t.system, nm, nl, &config, &new_system), TA_OK);
    free(t.memory);
    t.memory = nm;
    t.system = new_system;

    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, length), TA_OK);
    stats_now = stats(t.system);
    CHECK(stats_now.hibernated == 0 && stats_now.wakes == 1 && stats_now.wakes_pending == 0);
    /* the per-chart gauges moved with the relocation and the wake */
    CHECK(chart_stats(t.system).actors == 1 && chart_stats(t.system).hibernated == 0);
    run_all(&t);
    CHECK(strcmp(t.log, "pong;pong;") == 0);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0);
    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, length), TA_ERROR_STATE); /* not hibernated */

    /* again, and woken proactively by the host (no request was needed) */
    hibernate(&t, a, &status);
    CHECK_EQUAL(status, TA_OK);
    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, length), TA_OK);
    CHECK_EQUAL(tsys_send(&t, a, "tick", NULL), TA_OK);
    run_all(&t);
    CHECK(strcmp(tsys_config(&t, a), "c") == 0);
    CHECK(strstr(t.log, "in c;") != NULL);
    /* finalizing with a hibernated actor and a pending wake leaks nothing */
    hibernate(&t, a, &status);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_OK);
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
    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, length), TA_OK);
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
    CHECK_EQUAL(ta_actor_wake(t.system, kid, blob, length), TA_OK);
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
    CHECK_EQUAL(ta_actor_wake(t.system, p, blob, length), TA_OK);
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
    CHECK_EQUAL(ta_actor_wake(t.system, a, blob, length), TA_OK);
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
        CHECK_EQUAL(ta_system_spawn(t.system, 0, (ta_string_view){"", 0}, 0, (ta_string_view){0, 0},
                                    &ids[i]),
                    TA_OK);
    run_all(&t);
    uint32_t cursor = 0, out[16];
    CHECK_EQUAL(ta_system_cold_actors(t.system, &cursor, 64, out, 16), 0); /* all just ran */
    CHECK_EQUAL(ta_system_cold_actors(t.system, &cursor, 64, out, 16), 10);
    CHECK_EQUAL(tsys_send(&t, ids[3], "x", NULL), TA_OK);
    run_all(&t);
    uint32_t n = ta_system_cold_actors(t.system, &cursor, 64, out, 16);
    CHECK_EQUAL(n, 9);
    for (uint32_t i = 0; i < n; i++)
        CHECK(out[i] != ids[3]);
    /* bounded scan: max_scan slots, the cursor carries on */
    cursor = 0;
    CHECK_EQUAL(ta_system_cold_actors(t.system, &cursor, 4, out, 16), 4);
    CHECK_EQUAL(cursor, 4);
    CHECK_EQUAL(ta_system_cold_actors(t.system, &cursor, 64, out, 2), 2); /* capacity */
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
    if (!t->bytes || ta_image_open(t->bytes, t->length, &t->image) != TA_OK) return 0;
    x->ecmascript = ta_datamodel_ecmascript_new(NULL);
    t->binding = ta_datamodel_ecmascript_binding(x->ecmascript);
    ta_system_config config = test_system_config();
    if (ta_system_layout(&t->image, &config, &t->memory_length) != TA_OK) return 0;
    t->memory = malloc(t->memory_length);
    return ta_system_init(t->memory, t->memory_length, &t->image, &config, &t->binding, 1,
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
    if (ta_datamodel_ecmascript_eval_json(
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
        ta_datamodel_ecmascript_payload_from_json(x.ecmascript, (ta_string_view){"{\"v\": 3}", 8});
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
    CHECK_EQUAL(ta_actor_wake(t->system, a, blob, length), TA_OK);
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
    CHECK_EQUAL(ta_actor_wake(x.test_system.system, a, blob, length), TA_OK);
    CHECK(strcmp(ecmascript_eval(&x, a, "inc()"), "2") == 0);
    ecmascript_close(&x);
}

int main(void) {
    RUN(test_roundtrip_and_single_wake);
    RUN(test_timer_wakes_actor);
    RUN(test_children_cancel_terminate);
    RUN(test_error_communication_order);
    RUN(test_cold_sweep);
    RUN(test_ecmascript_state_survives);
    RUN(test_ecmascript_unsupported);
    TEST_MAIN_END();
}
