/* The twelve walkthroughs of docs/scenarios.md, executed against ta_system. */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/system_util.h"

#define SC(body)                                                                                   \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

/* 1. normal external event causing a transition */
static void s01_external_event(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    SC("initial='off'><state id='off'><transition event='flip' target='on'>"
                       "<log label='t' expr='_event.name'/></transition></state>"
                       "<state id='on'/>"),
                    NULL, NULL));
    uint32_t a;
    ta_system_spawn(t.system,
                    &(ta_spawn){0, 0, (ta_string_view){"lamp", 4}, 0, (ta_string_view){0, 0}}, &a);
    tsys_run(&t, 1000, 5);
    CHECK_EQUAL(tsys_send(&t, a, "flip", NULL), TA_OK);
    CHECK_EQUAL(tsys_run(&t, 1000, 5), TA_RUN_IDLE);
    CHECK(strcmp(tsys_config(&t, a), "on") == 0);
    CHECK(strcmp(t.log, "t=flip;") == 0);
    CHECK_EQUAL(t.system->workspaces.used, 0); /* stable actors hold no workspace */
    tsys_free(&t);
}

/* 2. eventless transition chain reaching stability */
static void s02_eventless_chain(void) {
    test_system t;
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><transition target='b'/></state>"
                       "<state id='b'><transition target='c'/></state>"
                       "<state id='c'><onentry><log expr=\"'stable'\"/></onentry></state>"),
                    NULL, NULL));
    ta_system_boot(t.system);
    CHECK_EQUAL(tsys_run(&t, 1000, 5), TA_RUN_IDLE);
    CHECK(strcmp(tsys_config(&t, tsys_actor(&t, "main.scxml")), "c") == 0);
    CHECK(strcmp(t.log, "stable;") == 0);
    tsys_free(&t);
}

static const char *LOOPER = SC(
    "initial='a'><datamodel><data id='k' expr='0'/></datamodel>"
    "<state id='a'><transition target='b'><assign location='k' expr='k + 1'/></transition></state>"
    "<state id='b'><transition target='a'/></state>"
    "<state id='never'><transition event='ping' target='never'><log "
    "expr=\"'pong'\"/></transition></state>");

/* 3. an infinitely looping eventless actor is fairly rescheduled */
static void s03_infinite_loop_fair(void) {
    static const test_file files[] = {
        {"echo.scxml", SC("initial='e'><state id='e'><transition event='ping' target='e'>"
                          "<log expr=\"'pong'\"/></transition></state>")},
        {NULL, NULL}};
    test_system t;
    ta_system_config config = test_system_config();
    config.quantum = 50;
    CHECK(tsys_open_image(&t, "loop.scxml", LOOPER, files));
    /* image holds only loop.scxml; compile echo separately into the same system
     * by spawning a second instance of a two-chart image instead: */
    tsys_free(&t);
    const char *two = SC("initial='p'><parallel id='p'>"
                         "<state id='loop'><state id='l1'><transition target='l2'/></state><state "
                         "id='l2'><transition target='l1'/></state></state>"
                         "</parallel>");
    (void)two;
    /* Simpler: two actors of different charts via a manifest. */
    const char *manifest =
        "<system><chart src='loop.scxml'/><chart src='echo.scxml'/>"
        "<actor name='looper' chart='loop.scxml'/><actor name='echo' chart='echo.scxml'/></system>";
    static test_file mfiles[3];
    mfiles[0].name = "loop.scxml";
    mfiles[0].text = LOOPER;
    mfiles[1] = files[0];
    mfiles[2].name = NULL;
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.resolve = test_resolve;
    options.resolve_context = mfiles;
    ta_source m = {{"sys.xml", 7}, {manifest, (uint32_t)strlen(manifest)}};
    ta_compile_result *result;
    CHECK_EQUAL(ta_compile_system(&options, &m, &result), TA_OK);
    memset(&t, 0, sizeof t);
    ta_string_view image = ta_compile_image(result);
    t.bytes = aligned_alloc(8, (image.length + 7) & ~7u);
    memcpy(t.bytes, image.pointer, image.length);
    t.length = image.length;
    ta_compile_free(result);
    CHECK_EQUAL(ta_image_open((ta_bytes){t.bytes, t.length}, &t.image), TA_OK);
    t.datamodel = datamodel_test_new();
    t.binding = datamodel_test_binding(t.datamodel);
    CHECK(tsys_create(&t, &config));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t looper = tsys_actor(&t, "looper"), echo = tsys_actor(&t, "echo");
    ta_run run_result = ta_system_run(t.system, 500);
    CHECK_EQUAL(run_result.status, TA_RUN_YIELDED); /* returns to the host despite the loop */
    tsys_drain(&t);
    CHECK_EQUAL(tsys_send(&t, echo, "ping", NULL), TA_OK);
    for (int i = 0; i < 4; i++) {
        run_result = ta_system_run(t.system, 500);
        CHECK_EQUAL(run_result.status, TA_RUN_YIELDED);
        tsys_drain(&t);
    }
    CHECK(strcmp(t.log, "pong;") == 0); /* echo got its turn */
    ta_actor_info info;
    info.size = sizeof info;
    ta_actor_info_get(t.system, looper, &info);
    CHECK_EQUAL(info.state, TA_ACTOR_RUNNABLE); /* still runnable, no event needed */
    CHECK(atoi(tsys_var(&t, looper, "k")) > 100);
    tsys_free(&t);
}

/* 4. several infinitely runnable actors share the thread */
static void s04_many_loopers(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.quantum = 100;
    config.workspaces = 2; /* fewer workspaces than loopers */
    CHECK(tsys_init(&t, LOOPER, NULL, &config));
    uint32_t a[3];
    for (int i = 0; i < 3; i++) {
        char nm[8];
        snprintf(nm, sizeof nm, "L%d", i);
        ta_system_spawn(
            t.system, &(ta_spawn){0, 0, (ta_string_view){nm, 2}, 0, (ta_string_view){0, 0}}, &a[i]);
    }
    for (int i = 0; i < 30; i++) {
        ta_run result = ta_system_run(t.system, 3000);
        CHECK_EQUAL(result.status, TA_RUN_YIELDED);
        tsys_drain(&t);
    }
    int k[3], mn = 1 << 30, mx = 0;
    for (int i = 0; i < 3; i++) {
        k[i] = atoi(tsys_var(&t, a[i], "k"));
        if (k[i] < mn) mn = k[i];
        if (k[i] > mx) mx = k[i];
    }
    fprintf(stderr, "  loopers: k = %d %d %d\n", k[0], k[1], k[2]);
    CHECK(mn > 100);
    CHECK(mx - mn <= mx / 5); /* within 20%: round-robin, no starvation */
    tsys_free(&t);
}

/* Two-actor system helper (chart 0 instances, named A and B). */
static const char *PEER = SC(
    "initial='s'><datamodel><data id='peer' expr=\"''\"/><data id='got' expr=\"''\"/></datamodel>"
    "<state id='s'>"
    "<transition event='init'><assign location='peer' expr='_event.data.peer'/>"
    "<send event='hello' targetexpr='peer'><param name='from' expr='_name'/></send></transition>"
    "<transition event='hello'><assign location='got' expr='_event.data.from'/>"
    "<send event='ack' targetexpr='_event.origin'/></transition>"
    "<transition event='ack'><log label='ack' expr='_name'/></transition>"
    "<transition event='later'><log label='later' expr='_name'/></transition>"
    "<transition event='arm'><send event='later' targetexpr='peer' delay='2s' "
    "id='t1'/></transition>"
    "<transition event='arm2'><send event='later' targetexpr='peer' delay='2s' id='t2'/>"
    "<cancel sendid='t2'/></transition>"
    "</state>");

static void peer_setup(test_system *t, uint32_t *a, uint32_t *b) {
    ta_system_spawn(t->system,
                    &(ta_spawn){0, 0, (ta_string_view){"A", 1}, 0, (ta_string_view){0, 0}}, a);
    ta_system_spawn(t->system,
                    &(ta_spawn){0, 0, (ta_string_view){"B", 1}, 0, (ta_string_view){0, 0}}, b);
    tsys_run(t, 10000, 5);
    ta_actor_info ib;
    ib.size = sizeof ib;
    ta_actor_info_get(t->system, *b, &ib);
    char data[64];
    snprintf(data, sizeof data, "{peer: '#_scxml_%llu'}", (unsigned long long)ib.serial);
    tsys_send(t, *a, "init", data);
}

/* 5. actor A sending to actor B internally: no host round-trip */
static void s05_internal_send(void) {
    test_system t;
    CHECK(tsys_init(&t, PEER, NULL, NULL));
    uint32_t a, b;
    peer_setup(&t, &a, &b);
    ta_run result = ta_system_run(t.system, 100000); /* one call: A -> B -> A */
    CHECK_EQUAL(result.status, TA_RUN_IDLE);
    tsys_drain(&t);
    CHECK_EQUAL(t.effect_count[TA_EFFECT_SEND], 0); /* nothing crossed the boundary */
    CHECK(strcmp(tsys_var(&t, b, "got"), "A") == 0);
    CHECK(strcmp(t.log, "ack=A;") == 0);
    tsys_free(&t);
}

/* 6. invoked SCXML child sending to / replying to its parent */
static void s06_invoke_parent_child(void) {
    test_system t;
    CHECK(tsys_init(
        &t,
        SC("initial='w'><datamodel><data id='trace' expr=\"''\"/></datamodel>"
           "<state id='w'><invoke id='kid'><content>"
           "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='k'>"
           "<state id='k'><onentry><send event='progress' target='#_parent'/></onentry>"
           "<transition event='more' target='kf'/></state>"
           "<final id='kf'><donedata><param name='r' expr='42'/></donedata></final></scxml>"
           "</content></invoke>"
           "<transition event='progress'><assign location='trace' expr=\"trace + 'p(' + "
           "_event.invokeid + ')'\"/>"
           "<send event='more' target='#_kid'/></transition>"
           "<transition event='done.invoke.kid' target='end'>"
           "<assign location='trace' expr=\"trace + 'd' + _event.data.r\"/></transition></state>"
           "<state id='end'/>"),
        NULL, NULL));
    ta_system_boot(t.system);
    uint32_t p = tsys_actor(&t, "main.scxml");
    CHECK_EQUAL(tsys_run(&t, 100000, 5), TA_RUN_IDLE);
    CHECK(strcmp(tsys_config(&t, p), "end") == 0);
    CHECK(strcmp(tsys_var(&t, p, "trace"), "p(kid)d42") == 0);
    CHECK_EQUAL(t.system->actors.used, 1); /* child reclaimed */
    tsys_free(&t);
}

/* 7. delayed cross-actor send (+ cancel) with host-supplied time */
static void s07_delayed_send(void) {
    test_system t;
    CHECK(tsys_init(&t, PEER, NULL, NULL));
    uint32_t a, b;
    peer_setup(&t, &a, &b);
    tsys_run(&t, 100000, 5);
    t.log[0] = 0;
    ta_system_advance_time(t.system, 1000);
    tsys_send(&t, a, "arm", NULL);
    tsys_send(&t, a, "arm2", NULL);
    ta_run result = ta_system_run(t.system, 100000);
    tsys_drain(&t);
    CHECK_EQUAL(result.next_deadline, 3000); /* 2 s at 1000 ticks/s, armed at t=1000 */
    CHECK_EQUAL(t.system->timers.used, 1);   /* t2 was cancelled */
    ta_system_advance_time(t.system, 2999);
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(t.log, "") == 0);
    ta_system_advance_time(t.system, 3000);
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(t.log, "later=B;") == 0);
    CHECK_EQUAL(ta_system_next_deadline(t.system), UINT64_MAX);
    tsys_free(&t);
}

/* 8. external asynchronous work: request/response without blocking the actor */
static void s08_async_request(void) {
    static const test_file files[] = {
        {"job.scxml",
         SC("initial='j'><final id='j'><donedata><content expr=\"'ok'\"/></donedata></final>")},
        {NULL, NULL}};
    test_system t;
    CHECK(tsys_init(
        &t,
        SC("initial='a'><datamodel><data id='n' expr='0'/><data id='res' expr=\"''\"/></datamodel>"
           "<state id='a'><invoke id='j' srcexpr=\"'remote:job'\"/>"
           "<transition event='tick'><assign location='n' expr='n + 1'/></transition>"
           "<transition event='done.invoke.j' target='b'><assign location='res' "
           "expr='_event.data'/></transition>"
           "</state><state id='b'/>"
           "<state id='linker'><invoke src='job.scxml'/></state>"),
        files, NULL));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "main.scxml");
    ta_system_run(t.system, 100000);
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t.system, &effect), req = 0;
    for (uint32_t i = 0; i < n; i++)
        if (effect[i].kind == TA_EFFECT_REQUEST) req = effect[i].token;
    CHECK(req != 0);
    ta_system_effects_ack(t.system, n);
    /* while the host works, the actor keeps processing events */
    tsys_send(&t, a, "tick", NULL);
    tsys_send(&t, a, "tick", NULL);
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(tsys_var(&t, a, "n"), "2") == 0);
    uint32_t job = ta_image_chart_by_name(&t.image, (ta_string_view){"job.scxml", 9});
    CHECK(job != TA_NIL);
    CHECK_EQUAL(ta_system_resume(t.system, &(ta_resume){0, req, TA_OK, 0, (ta_payload)job + 1}),
                TA_OK);
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0);
    CHECK(strcmp(tsys_var(&t, a, "res"), "ok") == 0);
    tsys_free(&t);
}

static const char *FOREACH =
    SC("initial='a'><datamodel><data id='arr' expr='[]'/><data id='sum' expr='0'/></datamodel>"
       "<state id='a'><onentry><script>var i = 0</script>"
       "<foreach "
       "array='[0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,"
       "31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47,48,49]' item='v'>"
       "<foreach array='[1,2,3,4]' item='w'><assign location='sum' expr='sum + v * w'/></foreach>"
       "</foreach><log label='sum' expr='sum'/></onentry></state>");

/* 9. scheduler suspension during executable content */
static void s09_suspend_in_content(void) {
    const char *expect = "sum=12250;"; /* sum(0..49) * (1+2+3+4) */
    for (int q = 0; q < 2; q++) {
        test_system t;
        ta_system_config config = test_system_config();
        config.quantum = q ? 7 : 1000000;
        CHECK(tsys_init(&t, FOREACH, NULL, &config));
        ta_system_boot(t.system);
        int mid_content = 0;
        for (int i = 0; i < 100000; i++) {
            ta_run result = ta_system_run(t.system, 7);
            tsys_drain(&t);
            if (result.status == TA_RUN_IDLE) break;
            /* suspended inside executable content: continuation in a workspace */
            if (t.system->workspaces.used == 1) mid_content++;
        }
        CHECK(strcmp(t.log, expect) == 0);
        if (q) CHECK(mid_content > 10); /* ~500 fuel of content in small slices */
        tsys_free(&t);
    }
}

/* 10 + 11. _event survives suspension during eventless processing; an
 * internal event then replaces it. */
static void s10_11_event_lifetime(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.quantum = 3;
    CHECK(tsys_init(&t,
                    SC("initial='idle'><datamodel><data id='seen' expr=\"''\"/><data id='c' "
                       "expr='0'/></datamodel>"
                       "<state id='idle'><transition event='E' target='work'/></state>"
                       "<state id='work'><transition cond=\"c &lt; 20\" target='work'>"
                       "<assign location='c' expr='c + 1'/><assign location='seen' expr='seen + "
                       "_event.name'/></transition>"
                       "<transition cond='c == 20' target='r'/></state>"
                       "<state id='r'><onentry><raise event='I'/></onentry>"
                       "<transition event='I' target='done'><assign location='seen' expr=\"seen + "
                       "'|' + _event.name\"/></transition></state>"
                       "<state id='done'/>"),
                    NULL, &config));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "main.scxml");
    tsys_run(&t, 3, 100000);
    tsys_send(&t, a, "E", NULL);
    int mid_yields = 0;
    for (int i = 0; i < 100000; i++) {
        ta_run result = ta_system_run(t.system, 3);
        if (result.status == TA_RUN_IDLE) break;
        ta_actor_info info;
        info.size = sizeof info;
        ta_actor_info_get(t.system, a, &info);
        if (info.in_macrostep) mid_yields++;
        /* inject a competing external event mid-macrostep: must not become _event */
        if (i == 5) tsys_send(&t, a, "X", NULL);
    }
    CHECK(mid_yields > 10);
    const char *seen = tsys_var(&t, a, "seen");
    CHECK(strcmp(seen, "EEEEEEEEEEEEEEEEEEEE|I") == 0);
    tsys_free(&t);
}

/* 12. 100,000 mostly idle actors sharing 4 continuation workspaces */
static void s12_hundred_thousand(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.max_actors = 100000;
    config.max_events = 4096;
    config.workspaces = 4;
    config.quantum = 1000;
    CHECK(
        tsys_open_image(&t, "cell.scxml",
                        "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='idle'>"
                        "<state id='idle'><transition event='poke' target='busy'/></state>"
                        "<state id='busy'><transition event='poke' target='idle'/></state></scxml>",
                        NULL));
    CHECK(tsys_create(&t, &config));
    static uint32_t h[100000];
    for (int i = 0; i < 100000; i++)
        CHECK_EQUAL(
            ta_system_spawn(t.system,
                            &(ta_spawn){0, 0, (ta_string_view){"c", 1}, 0, (ta_string_view){0, 0}},
                            &h[i]),
            TA_OK);
    CHECK_EQUAL(tsys_run(&t, 1u << 30, 10), TA_RUN_IDLE);
    for (int i = 0; i < 100000; i += 100)
        tsys_send(&t, h[i], "poke", NULL);
    CHECK_EQUAL(tsys_run(&t, 1u << 30, 10), TA_RUN_IDLE);
    int busy = 0;
    for (int i = 0; i < 100000; i++) {
        uint32_t states[4];
        ta_actor_config(t.system, h[i], (ta_uint32_array){states, 4});
        busy += states[0] == 2; /* state 'busy' */
    }
    CHECK_EQUAL(busy, 1000);
    CHECK_EQUAL(t.system->workspaces.used, 0);
    CHECK(t.system->workspaces.high_water_mark <= 4);
    size_t instance = ta_core_instance_size(&t.image, 0);
    fprintf(stderr, "  100k actors: region %.1f MB (%zu B/actor incl. pools), instance %zu B\n",
            (double)t.memory_length / 1e6, t.memory_length / 100000, instance);
    CHECK(t.memory_length / 100000 < 200);
    tsys_free(&t);
}

int main(void) {
    RUN(s01_external_event);
    RUN(s02_eventless_chain);
    RUN(s03_infinite_loop_fair);
    RUN(s04_many_loopers);
    RUN(s05_internal_send);
    RUN(s06_invoke_parent_child);
    RUN(s07_delayed_send);
    RUN(s08_async_request);
    RUN(s09_suspend_in_content);
    RUN(s10_11_event_lifetime);
    RUN(s12_hundred_thousand);
    TEST_MAIN_END();
}
