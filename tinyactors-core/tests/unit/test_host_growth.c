/* A segment's pools fill during a run, through what its actors do: sends
 * between them (one segment or two), <foreach> sends, raises, invokes and
 * their done events. The host grows the segment between runs (between a
 * drain's effects for a send into the segment being drained), or the run
 * reports TA_RUN_EXHAUSTED naming the pool; it never crashes and loses no
 * event. The first case is the crash of 200 actors that each send one event
 * at entry to one named actor (tinyactors-napi/docs/log.xml,
 * l-p4-examples-chat-events-crash). */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_config.h"

#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

/* Counts its `hello`s and logs each: the test counts the lines. */
#define SINK SC("sink", "initial='s'><datamodel><data id='n' expr='0'/></datamodel>"               \
                        "<state id='s'><transition event='hello'>"                                 \
                        "<assign location='n' expr='n + 1'/><log expr=\"'hello'\"/>"               \
                        "</transition></state>")
/* One send at entry to the named sink: the path between actors. */
#define SENDER                                                                                     \
    SC("sender", "initial='s'><state id='s'><onentry>"                                             \
                 "<send event='hello' target='tinyactors://g/@sink'/></onentry></state>")
/* Two sends at entry, each with its own sendid and a string in its data:
 * the slab (strings) fills with the events. */
#define TWICE SC("twice", "initial='s'><datamodel><data id='x' expr='\"a fairly long string\"'/>"  \
                          "</datamodel><state id='s'><onentry>"                                    \
                          "<send event='hello' target='tinyactors://g/@sink' namelist='x'/>"       \
                          "<send event='hello' target='tinyactors://g/@sink' namelist='x'/>"       \
                          "</onentry></state>")
/* 1000 sends from one <foreach>, in one microstep. */
#define FANOUT SC("fanout", "initial='s'><datamodel><data id='items' "                             \
                            "expr='Array.from({length: 1000}, (_, i) => i)'/></datamodel>"         \
                            "<state id='s'><onentry><foreach array='items' item='i'>"              \
                            "<send event='hello' target='tinyactors://g/@sink'/>"                  \
                            "</foreach></onentry></state>")
/* 1000 internal events raised in one microstep, each logged as it is taken. */
#define RAISER SC("raiser", "initial='s'><datamodel><data id='items' "                             \
                            "expr='Array.from({length: 1000}, (_, i) => i)'/></datamodel>"         \
                            "<state id='s'><onentry><foreach array='items' item='i'>"              \
                            "<raise event='hello'/></foreach></onentry>"                           \
                            "<transition event='hello'><log expr=\"'hello'\"/></transition>"       \
                            "</state>")
/* An SCXML child invoked at entry, done at once: its done.invoke is logged. */
#define PARENT                                                                                     \
    SC("parent", "initial='s'><state id='s'><invoke type='scxml'><content>"                        \
                 "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' "          \
                 "initial='f'><final id='f'/></scxml></content></invoke>"                          \
                 "<transition event='done.invoke'><log expr=\"'hello'\"/></transition>"            \
                 "</state>")

/* A delayed send at entry: a timer each while it waits. */
#define DELAYED SC("delayed", "initial='s'><state id='s'><onentry>"                              \
                              "<send event='x' delay='10s'/></onentry></state>")

/* An external service invoked at entry, which the host runs (one request
 * each while it runs); an error.execution is logged. */
#define CLIENT SC("client", "initial='s'><state id='s'><invoke type='urn:svc' src='x:y'/>"           \
                            "<transition event='error.execution'><log expr=\"'error'\"/>"          \
                            "</transition></state>")

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *images[2];
    uint32_t image_count;
    uint32_t hellos;  /* the lines logged 'hello' */
    uint32_t errors;  /* the lines logged 'error' */
    uint32_t invokes; /* TA_EFFECT_INVOKE effects */
} world;

#define SOURCE(name, text) {{name, (uint32_t)sizeof name - 1}, {text, (uint32_t)sizeof text - 1}}

/* An 8-aligned copy of the image of `sources`, or NULL (diagnostics printed). */
static void *compile_all(ta_source_list sources, size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_compile_result *result = NULL;
    void *copy = NULL;
    if (ta_compile_charts(&options, sources, &result) == TA_OK) {
        ta_string_view image = ta_compile_image(result);
        copy = aligned_alloc(8, (image.length + 7) & ~(size_t)7);
        memcpy(copy, image.pointer, image.length);
        *length = image.length;
    } else {
        print_diagnostics(result);
    }
    ta_compile_free(result);
    return copy;
}

/* The C library's allocator, which refuses every block of `refuse_from`
 * bytes or more while that is set. */
static size_t refuse_from;
static void *refusing_alloc(void *context, size_t size, size_t align) {
    (void)context;
    if (refuse_from && size >= refuse_from) return NULL;
    ta_allocator c = ta_runtime_malloc();
    return c.alloc(c.context, size, align);
}
static void refusing_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator c = ta_runtime_malloc();
    c.free(c.context, pointer, size);
}

/* A host on `system` (NULL: the defaults) with one image of `texts`. */
static void world_open(world *w, const ta_system_config *system) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"g", 1};
    if (system) config.system = *system;
    config.system.size = sizeof config.system;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, refusing_alloc, refusing_free};
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
}

/* An image of `sources`, as its own segment. */
static void world_add(world *w, ta_source_list sources) {
    size_t length = 0;
    CHECK(w->image_count < 2);
    void *image = compile_all(sources, &length);
    CHECK(image != NULL);
    w->images[w->image_count++] = image;
    uint32_t segment;
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){image, length}, &segment), TA_OK);
}

/* Every chart in one image. */
static void world_all(world *w, const ta_system_config *system) {
    world_open(w, system);
    static const ta_source sources[] = {
        SOURCE("sink.scxml", SINK),       SOURCE("sender.scxml", SENDER),
        SOURCE("twice.scxml", TWICE),     SOURCE("fanout.scxml", FANOUT),
        SOURCE("raiser.scxml", RAISER),   SOURCE("parent.scxml", PARENT),
        SOURCE("client.scxml", CLIENT), SOURCE("delayed.scxml", DELAYED)};
    world_add(w, (ta_source_list){sources, sizeof sources / sizeof *sources});
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    for (uint32_t i = 0; i < w->image_count; i++) free(w->images[i]);
}

static uint64_t spawn(world *w, const char *chart, const char *name) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(w->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref),
                TA_OK);
    uint64_t session = 0;
    ta_spawn spawn = {ref.segment, ref.chart, {name, name ? (uint32_t)strlen(name) : 0}, 0, {0, 0}};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_OK);
    return session;
}

static void drain(world *w) {
    const ta_host_effect *effects;
    uint32_t n;
    while ((n = ta_host_effects(w->host, &effects)) > 0) {
        for (uint32_t i = 0; i < n; i++) {
            if (effects[i].kind == TA_EFFECT_LOG && effects[i].event.length == 5 &&
                !memcmp(effects[i].event.pointer, "hello", 5))
                w->hellos++;
            if (effects[i].kind == TA_EFFECT_LOG && effects[i].event.length == 5 &&
                !memcmp(effects[i].event.pointer, "error", 5))
                w->errors++;
            if (effects[i].kind == TA_EFFECT_INVOKE) w->invokes++;
        }
        ta_host_effects_ack(w->host);
    }
}

/* Runs until idle, with a binding's budget: the allocator never refuses, so
 * every pool that fills grows, and no run is exhausted or faults. */
static void run_until_idle(world *w) {
    for (int i = 0; i < 10000; i++) {
        ta_run run = ta_host_run(w->host, 10000);
        drain(w);
        if (run.status == TA_RUN_IDLE) return;
        CHECK(run.status != TA_RUN_EXHAUSTED && run.status != TA_RUN_FAULT);
        if (run.status == TA_RUN_EXHAUSTED || run.status == TA_RUN_FAULT) {
            fprintf(stderr, "  run %d: status %u, detail %u\n", i, run.status, run.detail);
            return;
        }
    }
    CHECK(0); /* never idle */
}

static void spawn_many(world *w, const char *chart, int count) {
    for (int i = 0; i < count; i++) spawn(w, chart, NULL);
}

/* 200 actors of the default config send to one: the events pool fills while
 * the segment's effects are routed (the crash: the segment grew under the
 * effect being routed). */
static void test_sends_between_actors_fill_the_event_pool(void) {
    world w;
    world_all(&w, NULL);
    spawn(&w, "sink", "sink");
    spawn_many(&w, "sender", 200);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 200u);
    world_close(&w);
}

/* The same between two segments: the sink's grows while the senders' is
 * drained. */
static void test_sends_to_another_segment_fill_its_pool(void) {
    world w;
    world_open(&w, NULL);
    static const ta_source sink = SOURCE("sink.scxml", SINK);
    static const ta_source sender = SOURCE("sender.scxml", SENDER);
    world_add(&w, (ta_source_list){&sink, 1});
    world_add(&w, (ta_source_list){&sender, 1});
    spawn(&w, "sink", "sink");
    spawn_many(&w, "sender", 300);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 300u);
    world_close(&w);
}

/* Room for every event but not for their strings (the slab derived from
 * maxActors): the slab fills while the effects are routed. */
static void test_sends_between_actors_fill_the_slab(void) {
    ta_system_config system;
    memset(&system, 0, sizeof system);
    system.max_events = 100000;
    system.max_actors = 2000;
    world w;
    world_all(&w, &system);
    spawn(&w, "sink", "sink");
    spawn_many(&w, "twice", 1000);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 2000u);
    world_close(&w);
}

/* A <foreach> of 1000 sends in one microstep. */
static void test_foreach_sends_fill_the_event_pool(void) {
    world w;
    world_all(&w, NULL);
    spawn(&w, "sink", "sink");
    spawn(&w, "fanout", NULL);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 1000u);
    world_close(&w);
}

/* 1000 raised events in one microstep: internal events, inside the run. */
static void test_raises_fill_the_event_pool(void) {
    world w;
    world_all(&w, NULL);
    spawn(&w, "raiser", NULL);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 1000u);
    world_close(&w);
}

/* 300 parents each invoke a child at entry (the actors pool and the slab
 * fill inside the run: the spawns park) and get its done.invoke. */
static void test_invokes_and_done_events_fill_the_pools(void) {
    world w;
    world_all(&w, NULL);
    spawn_many(&w, "parent", 300);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 300u);
    world_close(&w);
}

/* 200 actors invoke a service the host runs, all in one run and then one
 * per run: the requests (16 by default) grow as the other pools do, and no
 * invoke fails (tinyactors-napi/docs/log.xml,
 * l-p4-examples-billing-requests-pool). */
static void test_external_invokes_fill_the_requests(void) {
    world w;
    world_all(&w, NULL);
    spawn_many(&w, "client", 200);
    run_until_idle(&w);
    for (int i = 0; i < 40; i++) {
        spawn(&w, "client", NULL);
        run_until_idle(&w);
    }
    CHECK_EQUAL(w.invokes, 240u);
    CHECK_EQUAL(w.errors, 0u);
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(ta_host_system(w.host, 0), &stats);
    CHECK(stats.requests_capacity >= 240u);
    world_close(&w);
}

/* A small pool beside a large region grows in proportion to itself: 65
 * delayed sends beside 100,000 events take the timers from 64 to at most 16
 * times that, not to a quarter of the region (1,048,576 timers beside a
 * million events before; tinyactors-napi/docs/log.xml, l-timer-pool-jump). */
static void test_a_small_pool_grows_in_proportion_to_itself(void) {
    ta_system_config system;
    memset(&system, 0, sizeof system);
    system.max_events = 100000;
    world w;
    world_all(&w, &system);
    ta_system_stats before, after;
    before.size = after.size = sizeof before;
    ta_system_stats_get(ta_host_system(w.host, 0), &before);
    spawn_many(&w, "delayed", 65);
    run_until_idle(&w);
    ta_system_stats_get(ta_host_system(w.host, 0), &after);
    CHECK(after.timers >= 65u);
    CHECK(after.timers_capacity > before.timers_capacity);
    CHECK(after.timers_capacity <= 16u * before.timers_capacity);
    world_close(&w);
}

/* 2000 actors spawned before a run, each sending at entry: the actors pool
 * grows at the spawns, the events and the slab in the drains. */
static void test_spawns_then_sends_at_entry(void) {
    world w;
    world_all(&w, NULL);
    spawn(&w, "sink", "sink");
    spawn_many(&w, "sender", 2000);
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 2000u);
    world_close(&w);
}

/* The growth refused: the run is exhausted and names the events, no event is
 * lost, and once the allocator gives the block the next runs go on. */
static void test_a_refused_growth_is_exhausted(void) {
    world w;
    world_all(&w, NULL);
    spawn(&w, "sink", "sink");
    spawn_many(&w, "sender", 2000);
    refuse_from = 1; /* every block from here: the actors grew at the spawns */
    ta_run run = {0};
    for (int i = 0; i < 100 && run.status != TA_RUN_EXHAUSTED; i++) {
        run = ta_host_run(w.host, 10000);
        drain(&w);
    }
    CHECK_EQUAL(run.status, TA_RUN_EXHAUSTED);
    CHECK_EQUAL(run.detail, TA_POOL_EVENTS);
    CHECK(w.hellos < 2000u);
    refuse_from = 0;
    run_until_idle(&w);
    CHECK_EQUAL(w.hellos, 2000u);
    world_close(&w);
}

int main(void) {
    RUN(test_sends_between_actors_fill_the_event_pool);
    RUN(test_sends_to_another_segment_fill_its_pool);
    RUN(test_sends_between_actors_fill_the_slab);
    RUN(test_foreach_sends_fill_the_event_pool);
    RUN(test_raises_fill_the_event_pool);
    RUN(test_invokes_and_done_events_fill_the_pools);
    RUN(test_spawns_then_sends_at_entry);
    RUN(test_external_invokes_fill_the_requests);
    RUN(test_a_small_pool_grows_in_proportion_to_itself);
    RUN(test_a_refused_growth_is_exhausted);
    TEST_MAIN_END();
}
