/* ta_system: public ABI behaviour — lifetime, inbound/outbound, effects,
 * backpressure, external I/O, external invokes, requests, relocation,
 * snapshot/restore, termination, determinism. */
#include "../harness/harness.h"
#include "system/system_internal.h"
#include "../support/system_util.h"

#define SC(body)                                                                                   \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' " body "</scxml>"

static const char *ECHO =
    SC("name='echo' initial='idle'><datamodel><data id='n' expr='0'/></datamodel>"
       "<state id='idle'><transition event='ping' target='idle'><assign location='n' expr='n + 1'/>"
       "<log label='ping' expr='_event.data.v'/></transition>"
       "<transition event='quit' target='end'/></state><final id='end'/>");

static void test_layout_and_init(void) {
    test_system t;
    CHECK(tsys_open_image(&t, "echo.scxml", ECHO, NULL));
    ta_system_config config = test_system_config();
    size_t need = 0;
    CHECK_EQUAL(ta_system_layout(&t.image, &config, &need), TA_OK);
    CHECK(need > sizeof(ta_system));
    unsigned char *memory = (unsigned char *)malloc(need + 16);
    ta_system *system = NULL;
    CHECK_EQUAL(ta_system_init(
                    &(ta_system_setup){{memory + 3, need - 8}, &t.image, &config, &t.binding, 1, 0},
                    &system),
                TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(
        ta_system_init(&(ta_system_setup){{memory + 3, need}, &t.image, &config, &t.binding, 1, 0},
                       &system),
        TA_OK); /* misaligned ok */
    CHECK(system != NULL);
    ta_system_deinit(system);
    /* missing datamodel implementation */
    CHECK_EQUAL(
        ta_system_init(&(ta_system_setup){{memory, need}, &t.image, &config, NULL, 0, 0}, &system),
        TA_ERROR_UNSUPPORTED);
    free(memory);
    tsys_free(&t);
    fprintf(stderr, "  region for 64 actors (defaults): %zu bytes\n", need);
}

static void test_send_run_effects(void) {
    test_system t;
    CHECK(tsys_init(&t, ECHO, NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "echo");
    CHECK(a != 0);
    CHECK_EQUAL(tsys_run(&t, 10000, 10), TA_RUN_IDLE);
    CHECK(strcmp(tsys_config(&t, a), "idle") == 0);
    ta_event_in events[3];
    memset(events, 0, sizeof events);
    for (int i = 0; i < 3; i++) {
        events[i].size = sizeof events[i];
        events[i].target = a;
        events[i].name = (ta_string_view){"ping", 4};
        char buffer[32];
        snprintf(buffer, sizeof buffer, "{v: %d}", i + 1);
        events[i].data = datamodel_test_payload(t.datamodel, buffer);
    }
    CHECK_EQUAL(ta_system_send_many(t.system, events, 3), 3);
    ta_actor_info info;
    info.size = sizeof info;
    CHECK_EQUAL(ta_actor_info_get(t.system, a, &info), TA_OK);
    CHECK_EQUAL(info.mailbox_length, 3);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.runnable, 1); /* counted as it is queued (O(1) stats) */
    CHECK_EQUAL(tsys_run(&t, 10000, 10), TA_RUN_IDLE);
    CHECK(strcmp(t.log, "ping=1;ping=2;ping=3;") == 0);
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.size, sizeof stats);
    CHECK_EQUAL(stats.actors, 1);
    CHECK_EQUAL(stats.runnable, 0);
    CHECK_EQUAL(stats.delivered, 3);
    CHECK(stats.fuel > 0 && stats.turns >= 3 && stats.spawned == 1);
    /* the idle actor still pins its last _event */
    CHECK(stats.events <= 1 && stats.events_capacity == 256 && stats.region_bytes > 0);
    CHECK(strcmp(tsys_var(&t, a, "n"), "3") == 0);
    /* top-level final -> TA_EFFECT_DONE, actor reclaimed, handle goes stale */
    CHECK_EQUAL(tsys_send(&t, a, "quit", NULL), TA_OK);
    tsys_run(&t, 10000, 10);
    CHECK_EQUAL(t.effect_count[TA_EFFECT_DONE], 1);
    CHECK(strcmp(t.done_state, "end") == 0); /* aux names the top-level final */
    CHECK_EQUAL(t.last[TA_EFFECT_DONE].actor, a);
    CHECK_EQUAL(ta_actor_info_get(t.system, a, &info), TA_ERROR_STALE);
    CHECK_EQUAL(tsys_send(&t, a, "ping", NULL), TA_ERROR_STALE);
    tsys_free(&t);
}

static void test_backpressure(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.outbox = 2;
    CHECK(tsys_init(&t,
                    SC("initial='a'><state id='a'><onentry>"
                       "<log expr='1'/><log expr='2'/><log expr='3'/><log expr='4'/><log expr='5'/>"
                       "</onentry></state>"),
                    NULL, &config));
    ta_system_boot(t.system);
    int full = 0;
    for (int i = 0; i < 10; i++) {
        ta_run result = ta_system_run(t.system, 100000);
        if (result.status == TA_RUN_EFFECTS_FULL) full++;
        CHECK(result.effect_count <= 2);
        tsys_drain(&t);
        if (result.status == TA_RUN_IDLE) break;
    }
    CHECK(full >= 2);
    CHECK(strcmp(t.log, "1;2;3;4;5;") == 0);
    tsys_free(&t);
}

static void test_external_io(void) {
    test_system t;
    CHECK(tsys_open_image(
        &t, "m.scxml",
        SC("initial='a'><state id='a'><onentry>"
           "<send type='http://example.com/io' event='out' target='http://x/y'>"
           "<param name='k' expr='7'/></send>"
           "<send type='nope' event='bad'/></onentry>"
           "<transition event='error.execution' target='b'/></state>"
           "<state id='b'><transition event='error.communication' target='c'/></state>"
           "<state id='c'/>"),
        NULL));
    ta_io_processor_descriptor io_processor = {
        {"http://example.com/io", 21}, {"http://me/", 10}, 0, 0};
    ta_system_config config = test_system_config();
    config.io_processors = &io_processor;
    config.io_processor_count = 1;
    CHECK(tsys_create(&t, &config));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "m.scxml");
    ta_run result = ta_system_run(t.system, 100000);
    CHECK_EQUAL(result.effect_count, 1);
    const ta_effect *effect;
    CHECK_EQUAL(ta_system_effects(t.system, &effect), 1);
    CHECK_EQUAL(effect[0].kind, TA_EFFECT_SEND);
    CHECK_EQUAL(effect[0].io_processor, 0);
    CHECK(effect[0].target.length == 10 && memcmp(effect[0].target.pointer, "http://x/y", 10) == 0);
    CHECK(effect[0].event.length == 3 && memcmp(effect[0].event.pointer, "out", 3) == 0);
    CHECK(strcmp(datamodel_test_payload_string(t.datamodel, effect[0].payload),
                 "[object Object]") == 0);
    ta_system_effects_ack(t.system, 1);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0); /* unknown type -> error.execution */
    /* the host reports the send failed */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(tsys_config(&t, a), "c") == 0);
    tsys_free(&t);
}

/* the core stays standard SCXML: spawning is not a <send>. Without a host
 * I/O processor for type "system", <send type="system" event="spawn"> is an
 * unsupported type (error.execution) and creates no session; with one, it is
 * an ordinary effect for that host to carry out. */
static void test_no_spawn_by_send(void) {
    const char *chart = SC("initial='a'><state id='a'><onentry>"
                           "<send type='system' event='spawn'><param name='src' expr=\"'kid'\"/>"
                           "</send></onentry>"
                           "<transition event='error.execution' target='refused'/>"
                           "<transition event='done.spawn' target='spawned'/></state>"
                           "<state id='refused'/><state id='spawned'/>");
    test_system t;
    CHECK(tsys_open_image(&t, "m.scxml", chart, NULL));
    ta_system_config config = test_system_config();
    CHECK(tsys_create(&t, &config));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "m.scxml");
    ta_run result = ta_system_run(t.system, 100000);
    CHECK_EQUAL(result.effect_count, 0);
    CHECK(strcmp(tsys_config(&t, a), "refused") == 0);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.actors, 1u);
    tsys_free(&t);
    /* a host that provides "system" gets a plain send effect, nothing more */
    CHECK(tsys_open_image(&t, "m.scxml", chart, NULL));
    ta_io_processor_descriptor io_processor = {{"system", 6}, {"system:", 7}, 0, 0};
    config = test_system_config();
    config.io_processors = &io_processor;
    config.io_processor_count = 1;
    CHECK(tsys_create(&t, &config));
    ta_system_boot(t.system);
    a = tsys_actor(&t, "m.scxml");
    result = ta_system_run(t.system, 100000);
    CHECK_EQUAL(result.effect_count, 1);
    const ta_effect *effect;
    CHECK_EQUAL(ta_system_effects(t.system, &effect), 1);
    CHECK_EQUAL(effect[0].kind, TA_EFFECT_SEND);
    CHECK(effect[0].event.length == 5 && memcmp(effect[0].event.pointer, "spawn", 5) == 0);
    ta_system_effects_ack(t.system, 1);
    CHECK(strcmp(tsys_config(&t, a), "a") == 0);
    ta_system_stats_get(t.system, &stats);
    CHECK_EQUAL(stats.actors, 1u);
    tsys_free(&t);
}

static void test_external_invoke_and_request(void) {
    static const test_file files[] = {
        {"kid.scxml",
         "<scxml xmlns='http://www.w3.org/2005/07/scxml' initial='k'><final id='k'/></scxml>"},
        {NULL, NULL}};
    test_system t;
    CHECK(tsys_init(
        &t,
        SC("initial='a'><datamodel><data id='src' expr=\"'kid.scxml'\"/>"
           "<data id='got' expr=\"''\"/></datamodel>"
           "<state id='a'><invoke id='ext' type='http://example.com/svc' src='svc://x'>"
           "<finalize><assign location='got' expr='got + _event.name'/></finalize></invoke>"
           "<transition event='done.invoke.ext' target='b'/></state>"
           "<state id='b'><invoke id='dyn' srcexpr=\"'other.scxml'\"/>"
           "<transition event='done.invoke.dyn' target='c'/></state><final id='c'/>"),
        files, NULL));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "main.scxml");
    ta_system_run(t.system, 100000);
    const ta_effect *effect;
    uint32_t n = ta_system_effects(t.system, &effect);
    CHECK_EQUAL(n, 1);
    CHECK_EQUAL(effect[0].kind, TA_EFFECT_INVOKE);
    uint32_t tok = effect[0].token;
    CHECK(effect[0].event.length == 22); /* type */
    CHECK(effect[0].target.length == 7 && memcmp(effect[0].target.pointer, "svc://x", 7) == 0);
    CHECK(effect[0].sendid.length == 3 &&
          memcmp(effect[0].sendid.pointer, "ext", 3) == 0); /* invoke id */
    ta_system_effects_ack(t.system, n);
    /* the external service talks back, then completes */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = a;
    event.token = tok;
    event.flags = TA_EVENT_IN_FROM_INVOKE;
    event.name = (ta_string_view){"progress", 8};
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    event.flags = TA_EVENT_IN_INVOKE_DONE;
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_OK);
    event.token = tok + 12345; /* bogus token */
    CHECK_EQUAL(ta_system_send(t.system, &event), TA_ERROR_STALE);
    ta_system_run(t.system, 100000);
    CHECK(strcmp(tsys_var(&t, a, "got"), "progress") == 0); /* finalize ran for child events */
    /* now in b: srcexpr names a chart not in the image -> host request */
    n = ta_system_effects(t.system, &effect);
    int found = 0;
    uint32_t req = 0;
    for (uint32_t i = 0; i < n; i++)
        if (effect[i].kind == TA_EFFECT_REQUEST) {
            found = 1;
            req = effect[i].token;
            CHECK(effect[i].flags & TA_EFFECT_FLAG_NEEDS_RESPONSE);
            CHECK(effect[i].auxiliary.length == 11 &&
                  memcmp(effect[i].auxiliary.pointer, "other.scxml", 11) == 0);
        }
    CHECK(found);
    ta_system_effects_ack(t.system, n);
    uint32_t kid = ta_image_chart_by_name(&t.image, (ta_string_view){"kid.scxml", 9});
    CHECK(kid == TA_NIL); /* only linked if statically referenced */
    /* resolve to chart 0 (main) would recurse; resolve with an error instead */
    CHECK_EQUAL(ta_system_resume(t.system, &(ta_resume){0, req, TA_ERROR_INVALID, 0, 0}), TA_OK);
    CHECK_EQUAL(ta_system_resume(t.system, &(ta_resume){0, req, TA_OK, 0, 1}),
                TA_ERROR_STALE); /* already answered */
    tsys_run(&t, 100000, 5);
    CHECK(strcmp(tsys_config(&t, a), "b") == 0); /* error.execution, no done.invoke.dyn */
    tsys_free(&t);
}

static void test_exhaustion_and_relocate(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.max_events = 24;
    CHECK(tsys_init(
        &t,
        SC("initial='a'><datamodel><data id='i' expr='0'/></datamodel>"
           "<state id='a'><onentry><foreach "
           "array='[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,"
           "30]' item='x'>"
           "<raise event='tick'/></foreach></onentry>"
           "<transition event='tick'><assign location='i' expr='i + 1'/></transition></state>"),
        NULL, &config));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "main.scxml");
    ta_run result = ta_system_run(t.system, 100000);
    CHECK_EQUAL(result.status, TA_RUN_EXHAUSTED);
    CHECK_EQUAL(result.detail, TA_POOL_EVENTS);
    /* grow by relocation into a bigger block and continue where we stopped */
    ta_system_config big = config;
    big.max_events = 256;
    size_t need;
    CHECK_EQUAL(ta_system_layout(&t.image, &big, &need), TA_OK);
    void *memory2 = malloc(need);
    ta_system *s2 = t.system;
    CHECK_EQUAL(ta_system_relocate(&s2, (ta_memory){memory2, need}, &big), TA_OK);
    free(t.memory);
    t.memory = memory2;
    t.system = s2;
    CHECK_EQUAL(tsys_run(&t, 100000, 10), TA_RUN_IDLE);
    CHECK(strcmp(tsys_var(&t, a, "i"), "30") == 0);
    tsys_free(&t);
}

/* A full timer pool is backpressure, not a failed send: the delayed send is
 * retried after growth and every timer fires. */
static void test_timer_exhaustion(void) {
    test_system t;
    ta_system_config config = test_system_config();
    config.max_timers = 2;
    CHECK(tsys_init(
        &t,
        SC("initial='a'><datamodel><data id='n' expr='0'/></datamodel>"
           "<state id='a'><onentry><send event='t' delay='5ms'/><send event='t' delay='5ms'/>"
           "<send event='t' delay='5ms'/></onentry>"
           "<transition event='t'><assign location='n' expr='n + 1'/></transition>"
           "<transition event='error.execution' target='failed'/></state>"
           "<state id='failed'/>"),
        NULL, &config));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "main.scxml");
    ta_run result = ta_system_run(t.system, 100000);
    CHECK_EQUAL(result.status, TA_RUN_EXHAUSTED);
    CHECK_EQUAL(result.detail, TA_POOL_TIMERS);
    ta_system_config big = config;
    big.max_timers = 8;
    size_t need;
    CHECK_EQUAL(ta_system_layout(&t.image, &big, &need), TA_OK);
    void *memory2 = malloc(need);
    ta_system *s2 = t.system;
    CHECK_EQUAL(ta_system_relocate(&s2, (ta_memory){memory2, need}, &big), TA_OK);
    free(t.memory);
    t.memory = memory2;
    t.system = s2;
    CHECK_EQUAL(tsys_run(&t, 100000, 10), TA_RUN_IDLE);
    ta_system_advance_time(t.system, 10);
    tsys_run(&t, 100000, 10);
    CHECK(strcmp(tsys_config(&t, a), "a") == 0); /* no error.execution */
    CHECK(strcmp(tsys_var(&t, a, "n"), "3") == 0);
    tsys_free(&t);
}

/* A snapshot must be a pure function of the state: dead storage (never-used
 * capacity, freed pool/slab/heap/outbox records) carries no residue. Two
 * systems reach the same empty state through different histories (their last
 * freed event and final state differ), over differently poisoned regions:
 * their snapshots must be identical, with no trace of either poison. */
static void test_snapshot_no_residue(void) {
    const char *chart = SC("name='m' initial='idle'><datamodel><data id='n' expr='0'/></datamodel>"
                           "<state id='idle'><transition event='ping' target='idle'>"
                           "<assign location='n' expr='n + 1'/></transition>"
                           "<transition event='quit' target='end1'/>"
                           "<transition event='halt' target='end2'/></state>"
                           "<final id='end1'/><final id='end2'/>");
    test_system t;
    CHECK(tsys_open_image(&t, "main.scxml", chart, NULL));
    ta_system_config config = test_system_config();
    void *snap[2] = {0};
    size_t slen[2] = {0};
    for (int h = 0; h < 2; h++) {
        t.log[0] = 0;
        memset(t.effect_count, 0, sizeof t.effect_count);
        size_t need;
        CHECK_EQUAL(ta_system_layout(&t.image, &config, &need), TA_OK);
        unsigned char *memory = (unsigned char *)malloc(need);
        memset(memory, h ? 0x5C : 0xA5, need); /* native-heap residue, made visible */
        CHECK_EQUAL(
            ta_system_init(&(ta_system_setup){{memory, need}, &t.image, &config, &t.binding, 1, 0},
                           &t.system),
            TA_OK);
        t.memory = memory;
        t.memory_length = need;
        CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
        uint32_t a = tsys_actor(&t, "m");
        CHECK(a != 0);
        CHECK_EQUAL(tsys_send(&t, a, "ping", "{v: 1}"), TA_OK);
        tsys_run(&t, 10000, 5);
        CHECK(strcmp(tsys_var(&t, a, "n"), "1") == 0);
        CHECK_EQUAL(tsys_send(&t, a, h ? "halt" : "quit", NULL), TA_OK); /* same length: the
                                                                         slab layout matches */
        tsys_run(&t, 10000, 5);
        CHECK_EQUAL(t.effect_count[TA_EFFECT_DONE], 1); /* actor and its records are freed */
        for (ta_trace_view view; ta_system_trace(t.system, &view);) /* and its trace, read */
            ta_system_trace_ack(t.system, view.length);
        ta_buffer out = {NULL, 0, 0};
        CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_ERROR_NO_MEMORY);
        size_t length = out.length;
        snap[h] = malloc(length);
        out = (ta_buffer){snap[h], length, 0};
        CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_OK);
        slen[h] = out.length;
        unsigned char poison = h ? 0x5C : 0xA5;
        int run = 0, bad = 0;
        for (size_t i = 0; i < length; i++) {
            run = ((unsigned char *)snap[h])[i] == poison ? run + 1 : 0;
            if (run >= 8) bad = 1; /* 8 in a row: no live data looks like this */
        }
        CHECK(!bad);
        ta_system_deinit(t.system); /* each system owns its datamodel instances */
        t.system = NULL;
        free(memory);
        t.memory = NULL;
    }
    CHECK_EQUAL(slen[0], slen[1]);
    CHECK(memcmp(snap[0], snap[1], slen[0]) == 0); /* same state, same bytes */
    free(snap[0]);
    free(snap[1]);
    tsys_free(&t);
}

static void test_snapshot_restore(void) {
    test_system t;
    CHECK(tsys_init(&t, ECHO, NULL, NULL));
    ta_system_boot(t.system);
    uint32_t a = tsys_actor(&t, "echo");
    tsys_run(&t, 10000, 5);
    tsys_send(&t, a, "ping", "{v: 'x'}");
    tsys_send(&t, a, "ping", "{v: 'y'}"); /* queued in the mailbox at snapshot time */
    ta_buffer out = {NULL, 0, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_ERROR_NO_MEMORY);
    size_t length = out.length;
    void *snap = malloc(length);
    out = (ta_buffer){snap, length, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_OK);
    /* the original continues ... */
    tsys_run(&t, 10000, 5);
    CHECK(strcmp(t.log, "ping=x;ping=y;") == 0);
    /* ... and so does the restored copy, independently (same datamodel object) */
    void *memory2 = malloc(length);
    ta_system *s2;
    CHECK_EQUAL(
        ta_system_restore(&(ta_system_setup){{memory2, length}, &t.image, NULL, &t.binding, 1, 0},
                          (ta_bytes){snap, length}, &s2),
        TA_OK);
    t.log[0] = 0;
    ta_system *orig = t.system;
    t.system = s2;
    tsys_run(&t, 10000, 5);
    CHECK(strcmp(t.log, "ping=x;ping=y;") == 0);
    t.system = orig;
    /* restored systems must not be deinitialized twice for the same datamodel instances:
     * drop the copy without deinit (its datamodel handles alias the original's) */
    free(memory2);
    free(snap);
    tsys_free(&t);
}

static void test_terminate_and_determinism(void) {
    const char *chart = SC("initial='a'><datamodel><data id='k' expr='0'/></datamodel>"
                           "<state id='p'><onexit><log label='bye' expr='k'/></onexit>"
                           "<state id='a'><transition target='a' cond='k &lt; 1000000'>"
                           "<assign location='k' expr='k + 1'/></transition></state></state>");
    static char first[1 << 16];
    for (int round = 0; round < 2; round++) {
        test_system t;
        ta_system_config config = test_system_config();
        config.quantum = 100;
        CHECK(tsys_init(&t, chart, NULL, &config));
        ta_system_boot(t.system);
        uint32_t a = tsys_actor(&t, "main.scxml");
        ta_run result = ta_system_run(t.system, 5000);
        CHECK_EQUAL(result.status, TA_RUN_YIELDED); /* always returns to the host */
        CHECK(result.used <= 5000 + 50);
        CHECK_EQUAL(ta_system_terminate(t.system, a), TA_OK);
        tsys_run(&t, 100000, 1000);
        CHECK_EQUAL(t.effect_count[TA_EFFECT_DONE], 0); /* terminated, not finished */
        ta_actor_info info;
        info.size = sizeof info;
        CHECK_EQUAL(ta_actor_info_get(t.system, a, &info), TA_ERROR_STALE);
        if (round == 0)
            memcpy(first, t.log, sizeof first);
        else
            CHECK(strcmp(first, t.log) == 0); /* identical inputs, identical trace */
        CHECK(strncmp(t.log, "bye=", 4) == 0);
        CHECK(atoi(t.log + 4) < 10000); /* stopped promptly, not after 1e6 iterations */
        tsys_free(&t);
    }
}

/* Two systems: SCXML sends to another system's session go through the host's
 * TA_IO_PROCESSOR_SCXML_REMOTE processor; replies use _event.origin; a system's
 * own base URI short-circuits to local delivery. */
static void forward(test_system *from, const char *from_base, test_system *to,
                    const char *to_base) {
    const ta_effect *effect;
    uint32_t n = ta_system_effects(from->system, &effect);
    for (uint32_t i = 0; i < n; i++) {
        if (effect[i].kind == TA_EFFECT_LOG) tsys_drain_one(from, &effect[i]);
        if (effect[i].kind != TA_EFFECT_SEND) continue;
        size_t bl = strlen(to_base);
        CHECK(effect[i].target.length > bl + 8 &&
              memcmp(effect[i].target.pointer, to_base, bl) == 0);
        uint64_t serial = strtoull(effect[i].target.pointer + bl + 8, NULL, 10);
        ta_actor_info info;
        info.size = sizeof info;
        CHECK_EQUAL(ta_actor_info_get(from->system, effect[i].actor, &info), TA_OK);
        char origin[96];
        snprintf(origin, sizeof origin, "%s#_scxml_%llu", from_base,
                 (unsigned long long)info.serial);
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.size = sizeof event;
        event.target = ta_system_actor_by_session(to->system, serial);
        CHECK(event.target != 0);
        event.name = effect[i].event;
        event.origin = (ta_string_view){origin, (uint32_t)strlen(origin)};
        event.origintype = (ta_string_view){"http://www.w3.org/TR/scxml/#SCXMLEventProcessor", 48};
        CHECK_EQUAL(ta_system_send(to->system, &event), TA_OK);
    }
    ta_system_effects_ack(from->system, n);
}

static void test_cross_system(void) {
    test_system a, b;
    CHECK(tsys_open_image(
        &a, "a.scxml",
        SC("name='a' initial='s'><state id='s'><onentry>"
           "<send event='self' target='tinyactors://a/#_scxml_1'/>"
           "</onentry><transition event='self' target='t'/></state>"
           "<state id='t'><onentry><send event='hi' target='tinyactors://b/#_scxml_1'/></onentry>"
           "<transition event='ack' target='done'><log label='ack' "
           "expr='_event.origin'/></transition>"
           "</state><state id='done'/>"),
        NULL));
    CHECK(
        tsys_open_image(&b, "b.scxml",
                        SC("name='b' initial='w'><state id='w'><transition event='hi' target='w2'>"
                           "<log label='hi' expr='_event.origin'/>"
                           "<send event='ack' targetexpr='_event.origin'/></transition></state>"
                           "<state id='w2'/>"),
                        NULL));
    ta_io_processor_descriptor ioa = {
        {"tinyactors", 10}, {"tinyactors://a/", 15}, TA_IO_PROCESSOR_SCXML_REMOTE, 0};
    ta_io_processor_descriptor iob = {
        {"tinyactors", 10}, {"tinyactors://b/", 15}, TA_IO_PROCESSOR_SCXML_REMOTE, 0};
    ta_system_config config_a = test_system_config(), config_b = test_system_config();
    config_a.io_processors = &ioa;
    config_a.io_processor_count = 1;
    config_b.io_processors = &iob;
    config_b.io_processor_count = 1;
    CHECK(tsys_create(&a, &config_a));
    CHECK(tsys_create(&b, &config_b));
    ta_system_boot(a.system);
    ta_system_boot(b.system);
    for (int round = 0; round < 4; round++) {
        ta_system_run(a.system, 100000);
        forward(&a, "tinyactors://a/", &b, "tinyactors://b/");
        ta_system_run(b.system, 100000);
        forward(&b, "tinyactors://b/", &a, "tinyactors://a/");
    }
    CHECK(strcmp(tsys_config(&a, tsys_actor(&a, "a")), "done") == 0);
    CHECK(strcmp(tsys_config(&b, tsys_actor(&b, "b")), "w2") == 0);
    CHECK(strcmp(b.log, "hi=tinyactors://a/#_scxml_1;") == 0);
    CHECK(strcmp(a.log, "ack=tinyactors://b/#_scxml_1;") == 0);
    tsys_free(&a);
    tsys_free(&b);
}

/* ta_system_send_many sends in order and stops at the first failure: the
 * accepted prefix, so the caller knows which event (and payload) is its own. */
/* Per-chart counts (tinyactors/stats.h): exact for a small chart, kept by
 * relocation and by a snapshot. */
static void test_chart_stats(void) {
    test_system t;
    CHECK(
        tsys_init(&t,
                  SC("name='counted' initial='idle'><state id='idle'>"
                     "<transition event='ping'/>"
                     "<transition event='bad'><send type='nope' event='x'/></transition>"
                     "<transition event='lost'><send target='#_scxml_foo' event='x'/></transition>"
                     "<transition event='quit' target='end'/></state><final id='end'/>"),
                  NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "counted");
    tsys_run(&t, 10000, 10);
    tsys_send(&t, a, "ping", NULL);
    tsys_send(&t, a, "ping", NULL);
    tsys_send(&t, a, "bad", NULL);
    tsys_send(&t, a, "lost", NULL);
    tsys_run(&t, 10000, 10);
    ta_chart_stats chart;
    chart.size = sizeof chart;
    CHECK_EQUAL(ta_system_chart_stats_get(t.system, 0, &chart), TA_OK);
    CHECK_EQUAL(chart.size, sizeof chart);
    CHECK(chart.name.length == 7 && memcmp(chart.name.pointer, "counted", 7) == 0);
    CHECK(chart.actors == 1 && chart.hibernated == 0 && chart.spawned == 1 && chart.ended == 0);
    CHECK_EQUAL(chart.macrosteps, 5); /* the bootstrap and four events; raised errors add none */
    CHECK_EQUAL(chart.errors_execution, 1);
    CHECK_EQUAL(chart.errors_communication, 1);
    CHECK_EQUAL(chart.errors_platform, 0);
    CHECK_EQUAL(chart.faults, 0);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(t.system, &stats);
    CHECK(chart.turns >= 2 && chart.turns <= stats.turns);
    CHECK(chart.fuel > 0 && chart.fuel <= stats.fuel);
    CHECK_EQUAL(ta_system_chart_stats_get(t.system, 1, &chart), TA_ERROR_INVALID);
    /* a snapshot keeps them */
    ta_buffer out = {NULL, 0, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_ERROR_NO_MEMORY);
    size_t length = out.length;
    void *snap = malloc(length), *memory2 = malloc(length);
    out = (ta_buffer){snap, length, 0};
    CHECK_EQUAL(ta_system_snapshot(t.system, &out), TA_OK);
    ta_system *copy;
    CHECK_EQUAL(
        ta_system_restore(&(ta_system_setup){{memory2, length}, &t.image, NULL, &t.binding, 1, 0},
                          (ta_bytes){snap, length}, &copy),
        TA_OK);
    ta_chart_stats restored;
    restored.size = sizeof restored;
    CHECK_EQUAL(ta_system_chart_stats_get(copy, 0, &restored), TA_OK);
    CHECK(restored.macrosteps == 5 && restored.errors_execution == 1 && restored.actors == 1);
    free(memory2); /* its datamodel handles alias the original's: no deinit */
    free(snap);
    /* ... and so does relocation; the end is counted */
    ta_system_config big = t.system->config;
    big.max_actors *= 2;
    size_t need;
    CHECK_EQUAL(ta_system_layout(&t.image, &big, &need), TA_OK);
    void *memory3 = malloc(need);
    ta_system *moved = t.system;
    CHECK_EQUAL(ta_system_relocate(&moved, (ta_memory){memory3, need}, &big), TA_OK);
    free(t.memory);
    t.memory = memory3;
    t.system = moved;
    tsys_send(&t, a, "quit", NULL);
    tsys_run(&t, 10000, 10);
    CHECK_EQUAL(ta_system_chart_stats_get(t.system, 0, &chart), TA_OK);
    CHECK(chart.actors == 0 && chart.ended == 1 && chart.macrosteps == 6);
    /* an older reader sets a smaller size and gets that much */
    ta_chart_stats small;
    memset(&small, 0xAB, sizeof small);
    small.size = (uint32_t)offsetof(ta_chart_stats, spawned);
    CHECK_EQUAL(ta_system_chart_stats_get(t.system, 0, &small), TA_OK);
    CHECK(small.size == offsetof(ta_chart_stats, spawned) &&
          small.spawned == 0xABABABABABABABABull);
    tsys_free(&t);
}

static void test_send_many_prefix(void) {
    test_system t;
    CHECK(tsys_init(&t, ECHO, NULL, NULL));
    CHECK_EQUAL(ta_system_boot(t.system), TA_OK);
    uint32_t a = tsys_actor(&t, "echo");
    CHECK_EQUAL(tsys_run(&t, 10000, 10), TA_RUN_IDLE);
    ta_event_in events[3];
    memset(events, 0, sizeof events);
    for (int i = 0; i < 3; i++) {
        events[i].size = sizeof events[i];
        events[i].target = a;
        events[i].name = (ta_string_view){"ping", 4};
        events[i].data = datamodel_test_payload(t.datamodel, "{v: 1}");
    }
    events[1].target = 12345; /* stale */
    CHECK_EQUAL(ta_system_send_many(t.system, events, 3), 1);
    CHECK(strcmp(datamodel_test_payload_string(t.datamodel, events[1].data), "{\"v\":1}") == 0 ||
          datamodel_test_payload_string(t.datamodel,
                                        events[1].data)[0]);      /* still valid: the caller's */
    CHECK_EQUAL(ta_system_send_many(t.system, events + 2, 1), 1); /* go on after it */
    CHECK_EQUAL(tsys_run(&t, 10000, 10), TA_RUN_IDLE);
    CHECK(strcmp(t.log, "ping=1;ping=1;") == 0);
    tsys_free(&t);
}

int main(void) {
    RUN(test_layout_and_init);
    RUN(test_send_run_effects);
    RUN(test_send_many_prefix);
    RUN(test_chart_stats);
    RUN(test_backpressure);
    RUN(test_external_io);
    RUN(test_no_spawn_by_send);
    RUN(test_external_invoke_and_request);
    RUN(test_exhaustion_and_relocate);
    RUN(test_timer_exhaustion);
    RUN(test_snapshot_no_residue);
    RUN(test_snapshot_restore);
    RUN(test_terminate_and_determinism);
    RUN(test_cross_system);
    TEST_MAIN_END();
}
