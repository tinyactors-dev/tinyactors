/* ta_host: many systems as one — segments sharing a base URI, routing between
 * them inside the library, versions and retirement, migration jobs (wake,
 * timeout), hibernation through the host, growth, enumeration. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define BASE "tinyactors://h/"
#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

typedef struct host_world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes[8];
    ta_image image[8];
    int image_count;
    char log[8192];
    int routed_out_count, wake_count, done_count, freed_count, migrated_count;
    uint32_t last_job;
    ta_host_effect last_done;
    uint64_t wake_session;
    unsigned char blob[1 << 16];
    size_t blob_length;
} host_world;

static void world_open(host_world *e, uint32_t max_actors, const ta_io_processor_descriptor *io,
                       uint32_t io_count) {
    memset(e, 0, sizeof *e);
    e->ecmascript = ta_datamodel_ecmascript_new(NULL);
    e->binding = ta_datamodel_ecmascript_binding(e->ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = max_actors;
    config.datamodels = &e->binding;
    config.datamodel_count = 1;
    config.migrate_quantum = 3; /* small steps: jobs span several runs */
    config.io_processors = io;
    config.io_processor_count = io_count;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(ta_host_new(&config, &e->host), TA_OK);
}

static uint32_t world_add(host_world *e, const char *text) {
    size_t length;
    int i = e->image_count++;
    e->image_bytes[i] = compile_chart("c.scxml", text, NULL, &length, 0);
    CHECK(e->image_bytes[i] != NULL);
    CHECK_EQUAL(ta_image_open(e->image_bytes[i], length, &e->image[i]), TA_OK);
    uint32_t segment = 0;
    CHECK_EQUAL(ta_host_add_image(e->host, &e->image[i], &segment), TA_OK);
    return segment;
}

static void world_close(host_world *e) {
    ta_host_free(e->host);
    ta_datamodel_ecmascript_free(e->ecmascript);
    for (int i = 0; i < e->image_count; i++)
        free(e->image_bytes[i]);
}

static void drain(host_world *e) {
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(e->host, &effects);
    for (uint32_t i = 0; i < n; i++) {
        const ta_host_effect *effect = &effects[i];
        if (effect->kind == TA_EFFECT_LOG) {
            size_t l = strlen(e->log);
            snprintf(e->log + l, sizeof e->log - l, "%.*s;", (int)effect->event.length,
                     effect->event.pointer);
        }
        if (effect->kind == TA_HOST_EFFECT_PEER) e->routed_out_count++;
        if (effect->kind == TA_EFFECT_WAKE) {
            e->wake_count++;
            e->wake_session = effect->session;
        }
        if (effect->kind == TA_EFFECT_DONE) e->done_count++;
        if (effect->kind == TA_HOST_EFFECT_SEGMENT_FREED) e->freed_count++;
        if (effect->kind == TA_HOST_EFFECT_MIGRATED) e->migrated_count++;
        if (effect->kind == TA_HOST_EFFECT_JOB_DONE) {
            e->last_job = effect->job;
            e->last_done = *effect;
        }
    }
    ta_host_effects_ack(e->host);
}

static uint32_t run(host_world *e) {
    ta_run result;
    result.status = TA_RUN_IDLE;
    for (int i = 0; i < 100; i++) {
        result = ta_host_run(e->host, 100000);
        drain(e);
        if (result.status == TA_RUN_IDLE) break;
    }
    return result.status;
}

static uint64_t spawn(host_world *e, const char *chart) {
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(e->host, (ta_string_view){chart, (uint32_t)strlen(chart)}, &ref),
                TA_OK);
    uint64_t s = 0;
    CHECK_EQUAL(ta_host_spawn(e->host,
                              &(ta_spawn){ref.segment, ref.chart, (ta_string_view){0, 0}, 0,
                                          (ta_string_view){0, 0}},
                              &s),
                TA_OK);
    return s;
}

static ta_status send(host_world *e, uint64_t session, const char *name, const char *json) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.data = json ? ta_datamodel_ecmascript_payload_eval(
                            e->ecmascript, 0, (ta_string_view){json, (uint32_t)strlen(json)})
                      : 0;
    event.session = session;
    ta_status status = ta_host_send(e->host, &event);
    if (status != TA_OK && event.data)
        ta_datamodel_ecmascript_vtable()->release(e->ecmascript, event.data);
    return status;
}

#define W1                                                                                         \
    SC("w",                                                                                        \
       "initial='a'><datamodel><data id='n' expr='0'/></datamodel>"                                \
       "<state id='a'><transition event='ask'><send targetexpr='_event.data.to' event='ping'/>"    \
       "</transition><transition event='pong'><assign location='n' expr='n + 1'/>"                 \
       "<log expr=\"'pong:' + n\"/></transition>"                                                  \
       "<transition event='error.communication'><log expr=\"'comm'\"/></transition>"               \
       "<transition event='sleep' target='b'/></state><state id='b'/>")
#define ECHO                                                                                       \
    SC("echo", "initial='e'><state id='e'><transition event='ping'>"                               \
               "<send targetexpr='_event.origin' event='pong'/></transition></state>")
#define W2                                                                                         \
    SC("w", "initial='a'><datamodel><data id='total' expr='0'/></datamodel>"                       \
            "<state id='a'><transition event='error.platform.upgrade'>"                            \
            "<assign location='total' expr='_event.data.n'/>"                                      \
            "<log expr=\"'upgraded:' + total\"/></transition>"                                     \
            "<transition event='pong'><log expr=\"'v2 pong'\"/></transition></state>"              \
            "<state id='b'><transition event='error.platform.upgrade'>"                            \
            "<log expr=\"'upgraded in b'\"/></transition></state>")

/* The class of a step's trace in segment `si`'s MACROSTEP_BEGIN of `name`
 * (records read and acknowledged), or -1. */
static int begin_class(host_world *e, uint32_t si, const char *name) {
    int found = -1;
    ta_system *system = ta_host_system(e->host, si);
    ta_trace_view view;
    while (ta_system_trace(system, &view)) {
        uint32_t cursor = 0;
        const ta_trace_record *record;
        while ((record = ta_trace_next(&view, &cursor)))
            if (record->kind == TA_TRACE_MACROSTEP_BEGIN) {
                const ta_trace_macrostep_begin *begin = ta_trace_body(record);
                ta_string_view n = ta_trace_dynamic_string(&view, begin->name);
                if (!n.length && begin->name != TA_NIL)
                    n = ta_image_string(&e->image[si], begin->name);
                if (n.length && n.length == strlen(name) && !memcmp(n.pointer, name, n.length))
                    found = (int)TA_TRACE_DETAIL(record->flags);
            }
        ta_system_trace_ack(system, view.length);
    }
    return found;
}

/* A watched step's class crosses segments with what it sends (docs/adr/0007). */
static void test_class_across_segments(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    world_add(&e, W1);
    world_add(&e, ECHO);
    ta_trace_rule watch;
    memset(&watch, 0, sizeof watch);
    watch.size = sizeof watch;
    watch.at = TA_TRACE_AT_TAKE;
    watch.chart = (ta_string_view){"w", 1};
    watch.event = (ta_string_view){"ask", 3};
    watch.sample = TA_TRACE_SAMPLE;
    watch.detail = TA_TRACE_FULL; /* a detail of its own: the first class after sampled */
    uint32_t id = 0;
    CHECK_EQUAL(ta_host_trace_watch(e.host, &(ta_host_watch){&watch, 1, 0, UINT64_MAX}, &id),
                TA_OK);
    watch.chart = (ta_string_view){"x", 1}; /* the host keeps its own copy */
    uint64_t w = spawn(&e, "w"), echo = spawn(&e, "echo");
    run(&e);
    begin_class(&e, 0, "");
    begin_class(&e, 1, "");
    char message[128];
    snprintf(message, sizeof message, "{\"to\": \"" BASE "#_scxml_%llu\"}",
             (unsigned long long)echo);
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "pong:1;") == 0);
    CHECK_EQUAL(begin_class(&e, 1, "ping"), TA_TRACE_DETAIL_WATCHED); /* echo's, in segment 1 */
    CHECK_EQUAL(begin_class(&e, 0, "pong"), TA_TRACE_DETAIL_WATCHED); /* and back */
    CHECK_EQUAL(ta_host_trace_unwatch(e.host, id), TA_OK);
    CHECK_EQUAL(ta_host_trace_unwatch(e.host, id), TA_ERROR_STALE);
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK_EQUAL(begin_class(&e, 1, "ping"), TA_TRACE_DETAIL_SAMPLED);
    /* a watch with an end: a deadline of the run, then gone */
    watch.chart = (ta_string_view){"w", 1};
    CHECK_EQUAL(ta_host_trace_watch(e.host, &(ta_host_watch){&watch, 1, 0, 500}, &id), TA_OK);
    ta_run result = ta_host_run(e.host, 1000);
    drain(&e);
    CHECK_EQUAL(result.next_deadline, 500u);
    ta_host_advance_time(e.host, 500);
    CHECK_EQUAL(ta_host_trace_unwatch(e.host, id), TA_ERROR_STALE);
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK_EQUAL(begin_class(&e, 1, "ping"), TA_TRACE_DETAIL_SAMPLED);
    /* more different details than the classes hold */
    uint32_t ids[TA_HOST_TRACE_DETAILS];
    for (uint32_t i = 0; i < TA_HOST_TRACE_DETAILS; i++) {
        watch.detail = TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(i + 2);
        CHECK_EQUAL(
            ta_host_trace_watch(e.host, &(ta_host_watch){&watch, 1, 0, UINT64_MAX}, &ids[i]),
            TA_OK);
    }
    watch.detail = TA_TRACE_BIT(TA_TRACE_FINISHED);
    CHECK_EQUAL(ta_host_trace_watch(e.host, &(ta_host_watch){&watch, 1, 0, UINT64_MAX}, &id),
                TA_ERROR_NO_MEMORY);
    world_close(&e);
}

static void test_routing_between_segments(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    uint32_t s0 = world_add(&e, W1), s1 = world_add(&e, ECHO);
    CHECK(s0 == 0 && s1 == 1);
    uint64_t w = spawn(&e, "w"), echo = spawn(&e, "echo");
    CHECK_EQUAL(w, 1);
    CHECK_EQUAL(echo, (1ull << 32) + 1); /* segment 1's session range */
    run(&e);
    char message[128];
    snprintf(message, sizeof message, "{\"to\": \"" BASE "#_scxml_%llu\"}",
             (unsigned long long)echo);
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "pong:1;") == 0); /* there and back, inside the host layer */
    CHECK_EQUAL(e.routed_out_count, 0);
    ta_host_stats stats;
    stats.size = sizeof stats;
    ta_host_stats_get(e.host, &stats);
    CHECK(stats.routed == 2 && stats.segments == 2 && stats.actors == 2);

    /* nobody has that session: error.communication; another system without a
     * route to it: error.communication too (unreachable, docs/protocol.md) */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, w, "ask", "{\"to\": \"" BASE "#_scxml_999\"}"), TA_OK);
    CHECK_EQUAL(send(&e, w, "ask", "{\"to\": \"tinyactors://elsewhere/#_scxml_1\"}"), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "comm;comm;") == 0);
    CHECK_EQUAL(e.routed_out_count, 0);
    /* with a peer for it: out as a TA_HOST_EFFECT_PEER effect */
    ta_peer_config peer_config;
    memset(&peer_config, 0, sizeof peer_config);
    peer_config.size = sizeof peer_config;
    peer_config.id = (ta_string_view){"elsewhere", 9};
    uint32_t peer = 0;
    CHECK_EQUAL(ta_host_peer_add(e.host, &peer_config, &peer), TA_OK);
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, w, "ask", "{\"to\": \"tinyactors://elsewhere/#_scxml_1\"}"), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "") == 0);
    CHECK_EQUAL(e.routed_out_count, 1);

    /* growth: more actors than the configured 8, spawned and messaged */

    ta_event_in events[40];
    for (int i = 0; i < 40; i++) {
        memset(&events[i], 0, sizeof events[i]);
        events[i].size = sizeof events[i];
        events[i].name = (ta_string_view){"noop", 4};
        events[i].session = spawn(&e, "echo");
    }
    CHECK_EQUAL(ta_host_send_many(e.host, events, 40), 40);
    run(&e);
    ta_host_stats_get(e.host, &stats);
    CHECK_EQUAL(stats.actors, 42);
    /* enumeration, in chunks */
    uint64_t cursor = 0, out[16];
    uint32_t total = 0, got;
    while ((got = ta_host_actors(e.host, &cursor, (ta_uint64_array){out, 16})) != 0)
        total += got;
    CHECK_EQUAL(total, 42);
    world_close(&e);
}

static void test_versions_and_migration(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, W1);
    world_add(&e, ECHO);
    uint64_t w[5];
    for (int i = 0; i < 5; i++)
        w[i] = spawn(&e, "w");
    run(&e);
    for (int i = 0; i < 5; i++)
        CHECK_EQUAL(send(&e, w[i], "pong", NULL), TA_OK);
    CHECK_EQUAL(send(&e, w[4], "sleep", NULL), TA_OK);
    run(&e);
    uint32_t s2 = world_add(&e, W2); /* a new version of "w" */
    ta_host_chart_ref ref;
    CHECK_EQUAL(ta_host_chart(e.host, (ta_string_view){"w", 1}, &ref), TA_OK);
    CHECK_EQUAL(ref.segment, s2);
    uint64_t fresh = spawn(&e, "w"); /* new actors: the new version */
    CHECK_EQUAL(fresh >> 32, s2);
    e.log[0] = 0;
    uint32_t job;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){0, 0}, 0, 0, 0, 0}, &job),
        TA_OK);
    CHECK_EQUAL(ta_host_run(e.host, 100000).status, TA_RUN_YIELDED);
    drain(&e);
    CHECK_EQUAL(e.migrated_count, 3); /* one quantum per run */
    run(&e);
    CHECK_EQUAL(e.migrated_count, 5);
    CHECK(e.last_job == job && e.last_done.migrated == 5 && e.last_done.pending == 0);
    CHECK(strstr(e.log, "upgraded:1;") && strstr(e.log, "upgraded in b;")); /* states by id */
    uint32_t at;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(e.host, w[0], &location), TA_OK);
    at = location.segment;
    CHECK_EQUAL(at, s2);           /* same session id, another segment */
    CHECK_EQUAL(e.freed_count, 1); /* version 1: superseded and empty */
    ta_host_segment_info segment_info;
    segment_info.size = sizeof segment_info;
    CHECK_EQUAL(ta_host_segment_info_get(e.host, 0, &segment_info), TA_OK);
    CHECK_EQUAL(segment_info.live, 0);
    /* the migrated actor gets its events at the same session id, in its new version */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, w[0], "pong", NULL), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "v2 pong;") == 0);
    /* per-chart counts by name: both versions summed, the freed one's kept */
    ta_chart_stats charts[4];
    for (int i = 0; i < 4; i++)
        charts[i].size = sizeof charts[i];
    CHECK_EQUAL(ta_host_chart_stats(e.host, charts, 4), 2u);
    const ta_chart_stats *w_stats = NULL;
    for (int i = 0; i < 2; i++)
        if (charts[i].name.length == 1 && charts[i].name.pointer[0] == 'w') w_stats = &charts[i];
    CHECK(w_stats != NULL);
    CHECK_EQUAL(w_stats->spawned, 6); /* five in version 1, one in version 2 */
    CHECK_EQUAL(w_stats->actors, 6);  /* the migrated ones left and arrived: not ended */
    CHECK_EQUAL(w_stats->ended, 0);
    CHECK(w_stats->macrosteps >= 13); /* version 1: 5 + 5 + 1; version 2: 1 + 1 at least */
    ta_chart_stats before = *w_stats;
    CHECK_EQUAL(ta_host_chart_stats(e.host, charts, 1), 2u); /* room for one: still the count */
    /* a host snapshot keeps the retired totals */
    size_t hl = 0, dl = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_ERROR_NO_MEMORY);
    hl = written.length;
    void *hs = malloc(hl);
    written = (ta_buffer){hs, hl, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_OK);
    hl = written.length;
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_ERROR_NO_MEMORY);
    dl = written.length;
    void *ds = malloc(dl);
    written = (ta_buffer){ds, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_OK);
    dl = written.length;
    ta_host_free(e.host);
    ta_datamodel_ecmascript_free(e.ecmascript);
    e.ecmascript = ta_datamodel_ecmascript_new(NULL);
    e.binding = ta_datamodel_ecmascript_binding(e.ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    const ta_image *images[3] = {&e.image[0], &e.image[1], &e.image[2]};
    CHECK_EQUAL(
        ta_host_restore(&config, &(ta_host_snapshot_input){{hs, hl}, images, 3, 0}, &e.host),
        TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    CHECK_EQUAL(ta_host_chart_stats(e.host, charts, 4), 2u);
    w_stats = NULL;
    for (int i = 0; i < 2; i++)
        if (charts[i].name.length == 1 && charts[i].name.pointer[0] == 'w') w_stats = &charts[i];
    CHECK(w_stats && w_stats->spawned == before.spawned &&
          w_stats->macrosteps == before.macrosteps && w_stats->turns == before.turns &&
          w_stats->actors == before.actors);
    free(hs);
    free(ds);
    world_close(&e);
}

static void test_migration_wake_and_timeout(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, W1);
    uint64_t a = spawn(&e, "w"), b = spawn(&e, "w");
    run(&e);
    size_t length = 0;
    ta_buffer written = {e.blob, sizeof e.blob, 0};
    CHECK_EQUAL(ta_host_hibernate(e.host, a, &written), TA_OK);
    length = written.length;
    e.blob_length = length;
    world_add(&e, W2);
    /* without wake, with a timeout: b moves, a is left behind and reported */
    ta_host_advance_time(e.host, 1000);
    uint32_t job;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, 0, 0, 50}, &job),
        TA_OK);
    ta_run result = ta_host_run(e.host, 100000);
    CHECK(result.next_deadline <= 1050); /* idle, but due at the timeout */
    drain(&e);
    run(&e);
    CHECK_EQUAL(e.last_job, 0u); /* not over yet: a waits */
    ta_host_advance_time(e.host, 1060);
    run(&e);
    CHECK_EQUAL(e.last_job, job);
    CHECK(e.last_done.migrated == 1 && e.last_done.pending == 1);
    uint64_t left[4];
    CHECK_EQUAL(ta_host_job_pending(e.host, job, (ta_uint64_array){left, 4}),
                1); /* until the next run */
    CHECK_EQUAL(left[0], a);
    CHECK_EQUAL(e.wake_count, 0);
    /* with wake: the host is asked for the blob, then a moves */
    CHECK_EQUAL(
        ta_host_migrate(e.host,
                        &(ta_host_migration){(ta_string_view){0, 0}, a, TA_HOST_MIGRATE_WAKE, 0, 0},
                        &job),
        TA_OK);
    run(&e);
    CHECK(e.wake_count == 1 && e.wake_session == a);
    CHECK_EQUAL(ta_host_wake(e.host, a, (ta_bytes){e.blob, e.blob_length}), TA_OK);
    run(&e);
    CHECK(e.last_job == job && e.last_done.migrated == 1 && e.last_done.pending == 0);
    uint32_t at;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(e.host, a, &location), TA_OK);
    at = location.segment;
    CHECK_EQUAL(at, 1u);
    (void)b;
    /* the sweep reports actors that did not run since the previous sweep */
    uint64_t cold[8];
    ta_host_sweep(e.host, 1000, (ta_uint64_array){cold, 8});
    uint32_t n = ta_host_sweep(e.host, 1000, (ta_uint64_array){cold, 8});
    CHECK(n >= 1);
    world_close(&e);
}

/* an actor with running invocations is migrated once they finished: its
 * session tree is not translated (the invoked sessions run old charts) */
#define P1                                                                                         \
    SC("p", "initial='a'><state id='a'><invoke id='kid'><content><scxml "                          \
            "xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' initial='k'>"          \
            "<state id='k'><transition event='finish' target='f'/></state><final id='f'/>"         \
            "</scxml></content></invoke><transition event='finish'>"                               \
            "<send target='#_kid' event='finish'/></transition>"                                   \
            "<transition event='done.invoke.kid' target='b'/></state><state id='b'/>")
#define P2                                                                                         \
    SC("p", "initial='top'><state id='top' initial='a'><transition "                               \
            "event='error.platform.upgrade'><log expr=\"'upgraded'\"/></transition>"               \
            "<state id='a'/><state id='b'/></state>")

static void test_migration_waits_for_invocations(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, P1);
    uint64_t p = spawn(&e, "p");
    run(&e);
    uint32_t actor;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(e.host, p, &location), TA_OK);
    actor = location.actor;
    world_add(&e, P2);
    uint32_t job;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"p", 1}, 0, 0, 0, 0}, &job),
        TA_OK);
    for (int i = 0; i < 5; i++)
        run(&e);
    CHECK_EQUAL(e.migrated_count, 0); /* waiting: its kid runs */
    CHECK_EQUAL(e.last_job, 0u);
    uint32_t again;
    CHECK_EQUAL(ta_host_locate(e.host, p, &location), TA_OK);
    again = location.actor;
    CHECK_EQUAL(again, actor); /* left alone meanwhile: not exported and re-adopted */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, p, "finish", NULL), TA_OK);
    run(&e);
    run(&e);
    CHECK_EQUAL(e.migrated_count, 1);
    CHECK(e.last_job == job && e.last_done.migrated == 1 && e.last_done.pending == 0);
    CHECK(strstr(e.log, "upgraded;") != NULL);
    world_close(&e);
}

/* a segment with a runaway actor gets a share of each run, not all of it */
static void test_fair_segments(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, SC("loop", "initial='a'><state id='a'><transition target='b'/></state>"
                             "<state id='b'><transition target='a'/></state>"));
    world_add(&e,
              SC("x", "initial='x'><state id='x'><onentry><log expr=\"'x'\"/></onentry></state>"));
    spawn(&e, "loop");
    CHECK_EQUAL(ta_host_run(e.host, 1000).status, TA_RUN_YIELDED);
    drain(&e);
    spawn(&e, "x");
    for (int i = 0; i < 2; i++) { /* whichever segment comes first in the rotation */
        CHECK_EQUAL(ta_host_run(e.host, 1000).status, TA_RUN_YIELDED);
        drain(&e);
        CHECK(strcmp(e.log, "x;") == 0);
        e.log[0] = 0;
        if (i == 0) spawn(&e, "x");
    }
    world_close(&e);
}

/* with a remote processor, a target that is no absolute URI is still invalid
 * (error.execution), and a malformed session id of our own base fails at once
 * (error.communication in the internal queue, before external events) */
static void test_bad_targets(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, SC("t",
                     "initial='a'><state id='a'><onentry>"
                     "<send target='baz' event='x'/><send event='after'/></onentry>"
                     "<transition event='error.execution' target='b'/>"
                     "<transition event='*' target='bad'/></state>"
                     "<state id='b'><onentry><log expr=\"'exec'\"/>"
                     "<send target='" BASE "#_scxml_foo' event='x'/><send event='after'/></onentry>"
                     "<transition event='error.communication' target='c'/>"
                     "<transition event='*' target='bad'/></state>"
                     "<state id='c'><onentry><log expr=\"'comm'\"/></onentry></state>"
                     "<state id='bad'><onentry><log expr=\"'bad:' + _event.name\"/></onentry>"
                     "</state>"));
    spawn(&e, "t");
    run(&e);
    CHECK(strcmp(e.log, "exec;comm;") == 0);
    CHECK_EQUAL(e.routed_out_count, 0);
    world_close(&e);
}

/* TA_IO_PROCESSOR_SYNC through the host: the run stops right after the send,
 * and what the host answers (an error, or an event back) comes before what
 * the sender does next — the BasicHTTP cases of W3C 577 and 201 */
#define IO_TYPE "http://example.com/io"
static void test_sync_io_processor(void) {
    ta_io_processor_descriptor io = {
        {IO_TYPE, (uint32_t)strlen(IO_TYPE)}, {"http://example.com/", 19}, TA_IO_PROCESSOR_SYNC, 0};
    for (int answer = 0; answer < 2; answer++) { /* 0: error.communication, 1: an event back */
        host_world e;
        world_open(&e, 16, &io, 1);
        world_add(&e,
                  SC("s", "initial='a'><state id='a'><onentry>"
                          "<send type='" IO_TYPE "' target='out' event='first' id='s1'/>"
                          "<send event='after'/><log expr=\"'next'\"/></onentry>"
                          "<transition event='error.communication' target='ok'/>"
                          "<transition event='back' target='ok'/>"
                          "<transition event='*' target='bad'/></state>"
                          "<state id='ok'><onentry><log expr=\"'ok:' + _event.name\"/></onentry>"
                          "</state><state id='bad'><onentry>"
                          "<log expr=\"'bad:' + _event.name\"/></onentry></state>"));
        uint64_t session = spawn(&e, "s");
        ta_run result = ta_host_run(e.host, 100000);
        CHECK_EQUAL(result.status, TA_RUN_EFFECTS);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(e.host, &effects);
        CHECK(n >= 1);
        CHECK_EQUAL(effects[n - 1].kind, TA_EFFECT_SEND);
        CHECK_EQUAL(effects[n - 1].io_processor, 0);
        CHECK_EQUAL(effects[n - 1].session, session);
        drain(&e);
        CHECK(strcmp(e.log, "") == 0); /* the sender has not gone on */
        if (answer == 0) {
            ta_event_in event;
            memset(&event, 0, sizeof event);
            event.size = sizeof event;
            event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
            event.sendid = (ta_string_view){"s1", 2};
            event.session = session;
            CHECK_EQUAL(ta_host_send(e.host, &event), TA_OK);
        } else {
            CHECK_EQUAL(send(&e, session, "back", NULL), TA_OK);
        }
        run(&e);
        CHECK(strcmp(e.log, answer == 0 ? "next;ok:error.communication;" : "next;ok:back;") == 0);
        world_close(&e);
    }
}

/* a whole host to bytes and back, as after a process restart: a fresh
 * datamodel restored from its own snapshot, the host from its own; timers,
 * queued events with payloads, variables, sessions and routing go on */
static void test_snapshot_restore(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, SC("c", "initial='a'><datamodel><data id='n' expr='0'/><data id='peer' expr='0'/>"
                          "</datamodel>"
                          "<state id='a'><onentry><send event='tick' delay='100ms'/></onentry>"
                          "<transition event='add'><assign location='n' expr='n + _event.data.k'/>"
                          "<assign location='peer' expr='_event.data.peer || peer'/>"
                          "<log expr=\"'n=' + n\"/></transition>"
                          "<transition event='tick' target='b'/></state>"
                          "<state id='b'><onentry><log expr=\"'ticked:' + n\"/>"
                          "<send targetexpr=\"'" BASE "#_scxml_' + peer\" event='ping'/></onentry>"
                          "<transition event='pong' target='done'/></state>"
                          "<final id='done'><onentry><log expr=\"'done'\"/></onentry></final>"));
    world_add(&e, ECHO); /* another segment */
    uint64_t c = spawn(&e, "c"), echo = spawn(&e, "echo");
    run(&e);
    char json[64];
    snprintf(json, sizeof json, "{\"k\": 2}");
    CHECK_EQUAL(send(&e, c, "add", json), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "n=2;") == 0);
    /* the peer's session into c's datamodel; one more event left queued */
    char peer[64];
    snprintf(peer, sizeof peer, "{\"k\": 3, \"peer\": %llu}", (unsigned long long)echo);
    CHECK_EQUAL(send(&e, c, "add", peer), TA_OK); /* not run before the snapshot */
    ta_host_advance_time(e.host, 50);
    size_t hl = 0, dl = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_ERROR_NO_MEMORY);
    hl = written.length;
    void *hs = malloc(hl);
    written = (ta_buffer){hs, hl, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_OK);
    hl = written.length;
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_ERROR_NO_MEMORY);
    dl = written.length;
    void *ds = malloc(dl);
    written = (ta_buffer){ds, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_OK);
    dl = written.length;
    uint64_t hashes[4];
    CHECK_EQUAL(ta_host_snapshot_segments((ta_bytes){hs, hl}, (ta_uint64_array){hashes, 4}), 2u);
    CHECK(hashes[0] != 0 && hashes[1] != 0);
    /* "restart": everything goes; the images are loaded again */
    ta_host_free(e.host);
    ta_datamodel_ecmascript_free(e.ecmascript);
    e.ecmascript = ta_datamodel_ecmascript_new(NULL);
    e.binding = ta_datamodel_ecmascript_binding(e.ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    const ta_image *images[2] = {&e.image[0], &e.image[1]};
    CHECK_EQUAL(
        ta_host_restore(&config, &(ta_host_snapshot_input){{hs, hl}, images, 1, 0}, &e.host),
        TA_ERROR_INVALID); /* too few */
    const ta_image *swapped[2] = {&e.image[1], &e.image[0]};
    CHECK_EQUAL(
        ta_host_restore(&config, &(ta_host_snapshot_input){{hs, hl}, swapped, 2, 0}, &e.host),
        TA_ERROR_INVALID); /* wrong ones */
    CHECK_EQUAL(
        ta_host_restore(&config, &(ta_host_snapshot_input){{hs, hl}, images, 2, 0}, &e.host),
        TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}),
                TA_ERROR_STATE); /* not fresh any more */
    e.log[0] = 0;
    run(&e); /* the queued event, with its payload */
    CHECK(strcmp(e.log, "n=5;") == 0);
    ta_host_advance_time(e.host, 120); /* the delayed send, armed before the snapshot */
    run(&e);
    if (strcmp(e.log, "n=5;ticked:5;done;")) fprintf(stderr, "  log: %s\n", e.log);
    CHECK(strcmp(e.log, "n=5;ticked:5;done;") == 0); /* routed to the other segment and back */
    ta_host_stats stats;
    stats.size = sizeof stats;
    ta_host_stats_get(e.host, &stats);
    CHECK(stats.routed >= 2 && stats.segments == 2);
    free(hs);
    free(ds);
    world_close(&e);
}

/* An actor moves to another host with its own datamodel (another process): its
 * delayed send's payload travels as a value, the blob is rebased to the new
 * host's clock, the session keeps its id and is routed to in whatever segment
 * adopted it, and no segment of its home range hands that id out again. */
#define MOVER                                                                                      \
    SC("mover",                                                                                    \
       "initial='a'><datamodel><data id='n' expr='0'/></datamodel>"                                \
       "<state id='a'><onentry><send event='tick' delay='100ms'>"                                  \
       "<param name='v' expr='[41]'/></send></onentry>"                                            \
       "<transition event='tick' target='b'><assign location='n' expr='_event.data.v[0] + 1'/>"    \
       "<log expr=\"'tick:' + n\"/></transition></state>"                                          \
       "<state id='b'><transition event='ping'><log expr=\"'ping:' + n\"/></transition>"           \
       "</state>")

static void test_export_adopt_between_hosts(void) {
    host_world a, b;
    world_open(&a, 8, NULL, 0);
    world_open(&b, 8, NULL, 0);
    world_add(&a, ECHO);
    world_add(&a, MOVER); /* segment 1 in a, 0 in b */
    world_add(&b, MOVER);
    uint64_t s = spawn(&a, "mover");
    CHECK_EQUAL(s, (1ull << 32) + 1);
    run(&a); /* its delayed send is due at 100 */
    ta_host_advance_time(a.host, 30);
    run(&a);
    size_t length = 0;
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_export_actor(a.host, s, &written), TA_ERROR_NO_MEMORY);
    length = written.length;
    size_t capacity = 2 * length + 4096;
    unsigned char *blob = (unsigned char *)malloc(capacity);
    written = (ta_buffer){blob, capacity, 0};
    CHECK_EQUAL(ta_host_export_actor(a.host, s, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(ta_host_locate(a.host, s, &(ta_host_location){0, 0}), TA_ERROR_STALE); /* it left */
    CHECK_EQUAL(ta_host_adopt(b.host, &(ta_host_adoption){0, 0, {blob, 7}}, NULL),
                TA_ERROR_INVALID);
    CHECK_EQUAL(ta_blob_rebase((ta_memory){blob, 9}, 0, 0), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_blob_rebase((ta_memory){blob, length}, 30, 1000),
                TA_OK); /* 70 ms left, on b's clock */
    ta_host_advance_time(b.host, 1000);
    uint64_t got = 0;
    CHECK_EQUAL(ta_host_adopt(b.host, &(ta_host_adoption){0, 0, {blob, length}}, &got), TA_OK);
    CHECK_EQUAL(got, s);
    CHECK_EQUAL(ta_host_adopt(b.host, &(ta_host_adoption){0, 0, {blob, length}}, &got),
                TA_ERROR_STATE); /* it is here already */
    uint32_t segment = 9;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(b.host, s, &location), TA_OK);
    segment = location.segment;
    CHECK_EQUAL(segment, 0);
    ta_host_advance_time(b.host, 1069);
    run(&b);
    CHECK(strcmp(b.log, "") == 0);
    ta_host_advance_time(b.host, 1070);
    run(&b);
    CHECK(strcmp(b.log, "tick:42;") == 0); /* the payload, as a value of b's datamodel */
    CHECK_EQUAL(send(&b, s, "ping", NULL), TA_OK);
    run(&b);
    CHECK(strcmp(b.log, "tick:42;ping:42;") == 0);
    /* its home range (segment 1) is added later: past its id */
    CHECK_EQUAL(world_add(&b, ECHO), 1);
    uint64_t fresh = spawn(&b, "echo");
    CHECK(fresh > s && (fresh >> 32) == 1);
    /* back to a, into its home segment, from a blob that is overdue there */
    written = (ta_buffer){blob, capacity, 0};
    CHECK_EQUAL(ta_host_export_actor(b.host, s, &written), TA_OK);
    length = written.length;
    CHECK_EQUAL(ta_host_adopt(a.host, &(ta_host_adoption){1, 0, {blob, length}}, &got), TA_OK);
    CHECK_EQUAL(ta_host_locate(a.host, s, &location), TA_OK);
    segment = location.segment;
    CHECK_EQUAL(segment, 1);
    a.log[0] = 0;
    CHECK_EQUAL(send(&a, s, "ping", NULL), TA_OK);
    run(&a);
    CHECK(strcmp(a.log, "ping:42;") == 0);
    free(blob);
    world_close(&a);
    world_close(&b);
}

/* A session tree is refused whole when any of its sessions — not only the
 * root — already lives in another segment of the adopting host. */
static void test_adopt_tree_collision(void) {
    host_world a, b;
    world_open(&a, 8, NULL, 0);
    world_open(&b, 8, NULL, 0);
    world_add(&a, P1); /* segment 0 in a, 1 in b */
    world_add(&b, ECHO);
    world_add(&b, P1);
    uint64_t root = spawn(&a, "p");
    run(&a);
    uint64_t ids[4], cursor = 0;
    CHECK_EQUAL(ta_host_actors(a.host, &cursor, (ta_uint64_array){ids, 4}), 2u);
    uint64_t kid = ids[0] == root ? ids[1] : ids[0];
    CHECK_EQUAL(root, 1u);
    CHECK_EQUAL(kid, 2u);
    size_t capacity = 65536, length = 0;
    unsigned char *blob = (unsigned char *)malloc(capacity),
                  *temporary = (unsigned char *)malloc(capacity);
    ta_buffer written = {blob, capacity, 0};
    CHECK_EQUAL(ta_host_export_actor(a.host, root, &written), TA_OK);
    length = written.length;
    /* b: session 2 lives in segment 0, session 1 does not */
    CHECK_EQUAL(spawn(&b, "echo"), 1u);
    CHECK_EQUAL(spawn(&b, "echo"), 2u);
    run(&b);
    written = (ta_buffer){temporary, capacity, 0};
    CHECK_EQUAL(ta_host_export_actor(b.host, 1, &written), TA_OK);
    uint64_t got = 9;
    CHECK_EQUAL(ta_host_adopt(b.host, &(ta_host_adoption){1, 0, {blob, length}}, &got),
                TA_ERROR_STATE);
    CHECK_EQUAL(got, 0u);
    uint32_t segment = 9;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(b.host, kid, &location), TA_OK);
    segment = location.segment;
    CHECK_EQUAL(segment, 0); /* still the echo */
    CHECK_EQUAL(ta_host_locate(b.host, root, &(ta_host_location){0, 0}), TA_ERROR_STALE);
    ta_host_stats stats;
    stats.size = sizeof stats;
    ta_host_stats_get(b.host, &stats);
    CHECK_EQUAL(stats.actors, 1u);
    /* once session 2 has left b, the tree moves in */
    written = (ta_buffer){temporary, capacity, 0};
    CHECK_EQUAL(ta_host_export_actor(b.host, kid, &written), TA_OK);
    CHECK_EQUAL(ta_host_adopt(b.host, &(ta_host_adoption){1, 0, {blob, length}}, &got), TA_OK);
    CHECK_EQUAL(got, root);
    CHECK_EQUAL(ta_host_locate(b.host, kid, &location), TA_OK);
    segment = location.segment;
    CHECK_EQUAL(segment, 1);
    free(blob);
    free(temporary);
    world_close(&a);
    world_close(&b);
}

/* configurations as nested state values: atomic, compound, parallel regions
 * (an atomic region is {}), a missing session, a short buffer */
static void test_state_value(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, SC("v", "initial='top'><state id='top' initial='p'>"
                          "<parallel id='p'><state id='r1' initial='x'><state id='x'>"
                          "<transition event='go' target='y'/></state><state id='y'/></state>"
                          "<state id='r2'/><state id='r3' initial='k'><state id='k' initial='k1'>"
                          "<history id='h'><transition target='k1'/></history><state id='k1'/></state></state></parallel>"
                          "<transition event='flat' target='flat'/></state>"
                          "<state id='flat'/>"));
    uint64_t session = spawn(&e, "v");
    run(&e);
    char value[256];
    ta_buffer out = {value, sizeof value - 1, 0}; /* room for a terminator */
    CHECK_EQUAL(ta_host_state_value(e.host, session, &out), TA_OK);
    value[out.length] = 0;
    size_t n = out.length;
    CHECK(strcmp(value, "{\"top\":{\"p\":{\"r1\":\"x\",\"r2\":{},\"r3\":{\"k\":\"k1\"}}}}") == 0);
    CHECK_EQUAL(n, strlen(value));
    out = (ta_buffer){value, 4, 0};
    CHECK_EQUAL(ta_host_state_value(e.host, session, &out), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, n); /* measured, cut short */
    out = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_host_state_value(e.host, session, &out), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, n);
    CHECK_EQUAL(send(&e, session, "go", NULL), TA_OK);
    run(&e);
    out = (ta_buffer){value, sizeof value - 1, 0};
    CHECK_EQUAL(ta_host_state_value(e.host, session, &out), TA_OK);
    value[out.length] = 0;
    CHECK(strcmp(value, "{\"top\":{\"p\":{\"r1\":\"y\",\"r2\":{},\"r3\":{\"k\":\"k1\"}}}}") == 0);
    CHECK_EQUAL(send(&e, session, "flat", NULL), TA_OK);
    run(&e);
    out = (ta_buffer){value, sizeof value - 1, 0};
    CHECK_EQUAL(ta_host_state_value(e.host, session, &out), TA_OK);
    value[out.length] = 0;
    CHECK(strcmp(value, "\"flat\"") == 0);
    CHECK_EQUAL(ta_host_state_value(e.host, session + 1000, &out), TA_ERROR_INVALID);
    world_close(&e);
}

/* a whole system in one blob: the host snapshot, the datamodel's state and the
 * images, read back by ta_host_snapshot_unpack and restored from */
static void test_snapshot_pack(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, ECHO);
    uint64_t session = spawn(&e, "echo");
    run(&e);
    size_t length = 0;
    ta_string_view label = {"label", 5};
    ta_buffer packed = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot_pack(e.host, &(ta_datamodel_state){label, {"state", 5}}, &packed),
                TA_ERROR_NO_MEMORY);
    length = packed.length;
    uint64_t *pack = calloc(length / 8 + 1, 8);
    packed = (ta_buffer){pack, length, 0};
    CHECK_EQUAL(ta_host_snapshot_pack(e.host, &(ta_datamodel_state){label, {"state", 5}}, &packed),
                TA_OK);
    length = packed.length;
    ta_host_snapshot_contents contents;
    contents.size = sizeof contents;
    CHECK_EQUAL(ta_host_snapshot_unpack((ta_bytes){pack, length}, &contents), TA_OK);
    CHECK(contents.system_id.length == 1 && contents.system_id.pointer[0] == 'h');
    CHECK(contents.datamodel.length == 5 && !memcmp(contents.datamodel.pointer, "label", 5));
    CHECK(contents.state_length == 5 && !memcmp(contents.state, "state", 5));
    CHECK_EQUAL(contents.image_count, 1u);
    CHECK_EQUAL(((uintptr_t)contents.host & 7u), 0u);
    size_t original = 0;
    ta_bytes image = ta_host_snapshot_image((ta_bytes){pack, length}, 0);
    const void *bytes = ta_image_bytes(&e.image[0], &original);
    CHECK(image.pointer && image.length == original && !memcmp(image.pointer, bytes, original));
    ta_bytes none = ta_host_snapshot_image((ta_bytes){pack, length}, 1);
    CHECK(none.pointer == NULL && none.length == 0);
    CHECK_EQUAL(ta_host_snapshot_unpack((ta_bytes){pack, length - 8}, &contents), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_host_snapshot_unpack((ta_bytes){contents.host, contents.host_length}, &contents),
                TA_ERROR_INVALID);
    ta_host_snapshot_unpack((ta_bytes){pack, length}, &contents);
    /* restored from its parts: the datamodel's own snapshot is the caller's */
    size_t dl = 0;
    ta_buffer written = {NULL, 0, 0};
    ta_datamodel_ecmascript_snapshot(e.ecmascript, &written);
    dl = written.length;
    void *state = malloc(dl);
    written = (ta_buffer){state, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_OK);
    dl = written.length;
    host_world f;
    memset(&f, 0, sizeof f);
    f.ecmascript = ta_datamodel_ecmascript_new(NULL);
    f.binding = ta_datamodel_ecmascript_binding(f.ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = contents.system_id;
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &f.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_image opened;
    CHECK_EQUAL(ta_image_open(image.pointer, image.length, &opened), TA_OK);
    const ta_image *images[1] = {&opened};
    CHECK_EQUAL(ta_host_restore(
                    &config,
                    &(ta_host_snapshot_input){{contents.host, contents.host_length}, images, 1, 0},
                    &f.host),
                TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(f.ecmascript, (ta_bytes){state, dl}), TA_OK);
    CHECK_EQUAL(ta_host_locate(f.host, session, &(ta_host_location){0, 0}), TA_OK);
    ta_host_free(f.host);
    ta_datamodel_ecmascript_free(f.ecmascript);
    free(state);
    free(pack);
    world_close(&e);
}

int main(void) {
    RUN(test_snapshot_pack);
    RUN(test_state_value);
    RUN(test_export_adopt_between_hosts);
    RUN(test_adopt_tree_collision);
    RUN(test_snapshot_restore);
    RUN(test_bad_targets);
    RUN(test_sync_io_processor);
    RUN(test_fair_segments);
    RUN(test_routing_between_segments);
    RUN(test_class_across_segments);
    RUN(test_versions_and_migration);
    RUN(test_migration_wake_and_timeout);
    RUN(test_migration_waits_for_invocations);
    TEST_MAIN_END();
}
