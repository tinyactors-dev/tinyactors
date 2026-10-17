/* ta_host: many systems as one — segments sharing a base URI, routing between
 * them inside the library, versions and retirement, migrations (wake,
 * timeout), hibernation through the host, requests, growth, enumeration. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"
#include "../support/payload_text.h"
#include "../support/host_output.h"

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
    uint32_t last_migration;
    ta_host_effect last_done;
    uint64_t wake_session;
    uint64_t done_session;  /* the last DONE effect's */
    char done_data[256];    /* and its payload as text ("" for none) */
    int request_count;      /* REQUEST effects, and the last one's: */
    uint32_t request_segment, request_token;
    char request_source[64];
    unsigned char blob[1 << 16];
    uint32_t wake_migration; /* the last WAKE's migration: 0 when no migration asked */
    size_t blob_length;
} host_world;

/* The host's allocator: the C library's, refusing while `refusing` is set. */
static int refusing;
static void *refusable_alloc(void *context, size_t size, size_t align) {
    (void)context;
    if (refusing) return NULL;
    ta_allocator c = ta_runtime_malloc();
    return c.alloc(c.context, size, align);
}
static void refusable_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator c = ta_runtime_malloc();
    c.free(c.context, pointer, size);
}

static void world_open(host_world *e, uint32_t max_actors, const ta_io_processor_entry *io,
                       uint32_t io_count) {
    memset(e, 0, sizeof *e);
    e->ecmascript = ta_datamodel_ecmascript_new(NULL);
    e->binding = ta_datamodel_ecmascript_binding(e->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = max_actors;
    config.datamodels = &e->binding;
    config.datamodel_count = 1;
    config.migrate_quantum = 3; /* small steps: migrations span several runs */
    config.io_processors = io;
    config.io_processor_count = io_count;
    config.allocator = (ta_allocator){NULL, refusable_alloc, refusable_free};
    CHECK_EQUAL(test_host_new(&config, &e->host), TA_OK);
}

static uint32_t world_add(host_world *e, const char *text) {
    size_t length;
    int i = e->image_count++;
    e->image_bytes[i] = compile_chart("c.scxml", text, NULL, &length, 0);
    CHECK(e->image_bytes[i] != NULL);
    CHECK_EQUAL(ta_image_open((ta_bytes){e->image_bytes[i], length}, &e->image[i]), TA_OK);
    uint32_t segment = 0;
    CHECK_EQUAL(ta_host_add_image(e->host, ta_image_bytes(&e->image[i]), &segment), TA_OK);
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
        if (effect->kind == TA_EFFECT_SEND) e->routed_out_count++;
        if (effect->kind == TA_EFFECT_WAKE) {
            e->wake_count++;
            e->wake_session = effect->session;
            e->wake_migration = effect->migration;
        }
        if (effect->kind == TA_EFFECT_DONE) {
            e->done_count++;
            e->done_session = effect->session;
            e->done_data[0] = 0;
            if (effect->payload) payload_text(e->ecmascript, effect->payload, e->done_data, sizeof e->done_data);
        }
        if (effect->kind == TA_EFFECT_REQUEST) {
            e->request_count++;
            e->request_segment = effect->segment;
            e->request_token = effect->token;
            snprintf(e->request_source, sizeof e->request_source, "%.*s", (int)effect->auxiliary.length,
                     effect->auxiliary.pointer ? effect->auxiliary.pointer : "");
        }
        if (effect->kind == TA_HOST_EFFECT_SEGMENT_FREED) e->freed_count++;
        if (effect->kind == TA_HOST_EFFECT_MIGRATED) e->migrated_count++;
        if (effect->kind == TA_HOST_EFFECT_MIGRATION_DONE) {
            e->last_migration = effect->migration;
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

/* {"method": "trace.watch"}: a rule for chart's "ask" events taken, sampled at
 * `detail` (with values); seconds < 0: no end. Its id, UINT64_MAX when refused. */
static uint64_t watch(ta_host *host, const char *chart, const char *detail, int values,
                      double seconds) {
    static host_ask a;
    ta_cbor_writer *w = ask_begin(&a, "trace.watch", seconds < 0 ? 2 : 2);
    ta_cbor_write_text(w, ta_string("rules"));
    ta_cbor_write_array(w, 1);
    ta_cbor_write_map(w, 6);
    ta_cbor_write_text(w, ta_string("at"));
    ta_cbor_write_text(w, ta_string("take"));
    ta_cbor_write_text(w, ta_string("chart"));
    ta_cbor_write_text(w, ta_string(chart));
    ta_cbor_write_text(w, ta_string("event"));
    ta_cbor_write_text(w, ta_string("ask"));
    ta_cbor_write_text(w, ta_string("sample"));
    ta_cbor_write_text(w, ta_string("sample"));
    ta_cbor_write_text(w, ta_string("detail"));
    ta_cbor_write_text(w, ta_string(detail));
    ta_cbor_write_text(w, ta_string("values"));
    ta_cbor_write_simple(w, values ? TA_CBOR_TRUE : TA_CBOR_FALSE);
    ta_cbor_write_text(w, ta_string("seconds"));
    if (seconds < 0) ta_cbor_write_simple(w, TA_CBOR_NULL);
    else ta_cbor_write_float(w, seconds);
    if (ask_send(host, &a) != TA_OK) return UINT64_MAX;
    return ask_uint(&a, 0, "id");
}

/* {"method": "trace.unwatch"}: whether the reply's text is `expected`. */
static int unwatch(ta_host *host, uint64_t id, const char *expected) {
    static host_ask a;
    ta_cbor_writer *w = ask_begin(&a, "trace.unwatch", 1);
    ta_cbor_write_text(w, ta_string("id"));
    ta_cbor_write_uint(w, id);
    if (ask_send(host, &a) != TA_OK) return 0;
    if (strcmp(a.text, expected) != 0) fprintf(stderr, "  reply: %s\n", a.text);
    return strcmp(a.text, expected) == 0;
}

/* A watched step's class crosses segments with what it sends (docs/adr/0007). */
static void test_class_across_segments(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    world_add(&e, W1);
    world_add(&e, ECHO);
    uint64_t id = watch(e.host, "w", "full", 0, -1.0);
    CHECK(id != UINT64_MAX);
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
    CHECK(unwatch(e.host, id, "{\"id\":1,\"result\":{}}"));
    CHECK(unwatch(e.host, id, "{\"id\":1,\"error\":{\"code\":-32001,\"message\":\"no such watch: it ended or never was\",\"data\":{\"reason\":\"stale\",\"path\":\"params.id\"}}}"));
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK_EQUAL(begin_class(&e, 1, "ping"), TA_TRACE_DETAIL_SAMPLED);
    /* a watch with an end: a deadline of the run, then gone */
    id = watch(e.host, "w", "full", 0, 0.5); /* 500 ticks at 1000 a second */
    ta_run result = ta_host_run(e.host, 1000);
    drain(&e);
    CHECK_EQUAL(result.next_deadline, 500u);
    ta_host_advance_time(e.host, (ta_host_time){500, 0});
    CHECK(unwatch(e.host, id, "{\"id\":1,\"error\":{\"code\":-32001,\"message\":\"no such watch: it ended or never was\",\"data\":{\"reason\":\"stale\",\"path\":\"params.id\"}}}"));
    CHECK_EQUAL(send(&e, w, "ask", message), TA_OK);
    run(&e);
    CHECK_EQUAL(begin_class(&e, 1, "ping"), TA_TRACE_DETAIL_SAMPLED);
    /* more different details than the classes hold: four presets, with
     * values and without, are eight */
    static const char *const presets[] = {"summary", "decisions", "full", "off"};
    for (uint32_t i = 0; i < 6; i++)
        CHECK(watch(e.host, "w", presets[i % 4], i >= 4, -1.0) != UINT64_MAX);
    CHECK(watch(e.host, "w", presets[2], 1, -1.0) == UINT64_MAX);
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
    CHECK_EQUAL(host_stats_fill(e.host, &stats), TA_OK);
    CHECK(stats.routed == 2 && stats.segments == 2 && stats.actors == 2);

    /* nobody has that session: error.communication; another system: a SEND
     * for the host's transport (test_remote.c), which this one ignores */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, w, "ask", "{\"to\": \"" BASE "#_scxml_999\"}"), TA_OK);
    CHECK_EQUAL(send(&e, w, "ask", "{\"to\": \"tinyactors://elsewhere/#_scxml_1\"}"), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "comm;") == 0);
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
    CHECK_EQUAL(host_stats_fill(e.host, &stats), TA_OK);
    CHECK_EQUAL(stats.actors, 42);
    /* enumeration, in chunks */
    uint64_t cursor = 0, out[16];
    uint32_t total = 0, got;
    while ((got = host_actors_page(e.host, &cursor, (ta_uint64_array){out, 16})) != 0)
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
    uint32_t migration;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){0, 0}, 0, 0, 0, 0}, &migration),
        TA_OK);
    CHECK_EQUAL(ta_host_run(e.host, 100000).status, TA_RUN_YIELDED);
    drain(&e);
    CHECK_EQUAL(e.migrated_count, 3); /* one quantum per run */
    run(&e);
    CHECK_EQUAL(e.migrated_count, 5);
    CHECK(e.last_migration == migration && e.last_done.migrated == 5 && e.last_done.pending == 0);
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
    CHECK(segment_info.image == NULL); /* freed with the segment */
    /* the migrated actor gets its events at the same session id, in its new version */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, w[0], "pong", NULL), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "v2 pong;") == 0);
    /* per-chart counts by name: both versions summed, the freed one's kept */
    ta_chart_stats charts[4];
    for (int i = 0; i < 4; i++)
        charts[i].size = sizeof charts[i];
    CHECK_EQUAL(host_chart_stats_fill(e.host, charts, 4), 2u);
    const ta_chart_stats *w_stats = NULL;
    for (int i = 0; i < 2; i++)
        if (charts[i].name.length == 1 && charts[i].name.pointer[0] == 'w') w_stats = &charts[i];
    CHECK(w_stats != NULL);
    CHECK_EQUAL(w_stats->spawned, 6); /* five in version 1, one in version 2 */
    CHECK_EQUAL(w_stats->actors, 6);  /* the migrated ones left and arrived: not ended */
    CHECK_EQUAL(w_stats->ended, 0);
    CHECK(w_stats->macrosteps >= 13); /* version 1: 5 + 5 + 1; version 2: 1 + 1 at least */
    ta_chart_stats before = *w_stats;
    CHECK_EQUAL(host_chart_stats_fill(e.host, charts, 1), 2u); /* room for one: still the count */
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
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_bytes images[3] = {ta_image_bytes(&e.image[0]), ta_image_bytes(&e.image[1]),
                          ta_image_bytes(&e.image[2])};
    CHECK_EQUAL(
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, 3, 0}, &e.host),
        TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    CHECK_EQUAL(host_chart_stats_fill(e.host, charts, 4), 2u);
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
    ta_bytes written; /* the host's, until the ack */
    CHECK_EQUAL(ta_host_hibernate(e.host, a, &written), TA_OK);
    e.blob_length = output_copy(written, e.blob, sizeof e.blob);
    ta_host_effects_ack(e.host); /* the output is copied: its batch ends */
    world_add(&e, W2);
    /* without wake, with a timeout: b moves, a is left behind and reported */
    ta_host_advance_time(e.host, (ta_host_time){1000, 0});
    uint32_t migration;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, 0, 0, 50}, &migration),
        TA_OK);
    ta_run result = ta_host_run(e.host, 100000);
    CHECK(result.next_deadline <= 1050); /* idle, but due at the timeout */
    drain(&e);
    run(&e);
    CHECK_EQUAL(e.last_migration, 0u); /* not over yet: a waits */
    ta_host_advance_time(e.host, (ta_host_time){1060, 0});
    run(&e);
    CHECK_EQUAL(e.last_migration, migration);
    CHECK(e.last_done.migrated == 1 && e.last_done.pending == 1);
    uint64_t left[4];
    CHECK_EQUAL(ta_host_migration_pending(e.host, migration, (ta_uint64_array){left, 4}),
                1); /* until the next run */
    CHECK_EQUAL(left[0], a);
    CHECK_EQUAL(e.wake_count, 0);
    /* with wake: the host is asked for the blob, then a moves */
    CHECK_EQUAL(
        ta_host_migrate(e.host,
                        &(ta_host_migration){(ta_string_view){0, 0}, a, TA_HOST_MIGRATE_WAKE, 0, 0},
                        &migration),
        TA_OK);
    run(&e);
    CHECK(e.wake_count == 1 && e.wake_session == a);
    CHECK_EQUAL(ta_host_wake(e.host, a, (ta_bytes){e.blob, e.blob_length}), TA_OK);
    run(&e);
    CHECK(e.last_migration == migration && e.last_done.migrated == 1 && e.last_done.pending == 0);
    uint32_t at;
    ta_host_location location;
    CHECK_EQUAL(ta_host_locate(e.host, a, &location), TA_OK);
    at = location.segment;
    CHECK_EQUAL(at, 1u);
    ta_actor_info info; /* woken for the move: in memory, not hibernated */
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    CHECK_EQUAL(ta_actor_info_get(ta_host_system(e.host, location.segment), location.actor, &info), TA_OK);
    CHECK(info.state != TA_ACTOR_HIBERNATED && info.state != TA_ACTOR_FREE);
    (void)b;
    /* the sweep reports actors that did not run since the previous sweep */
    uint64_t cold[8];
    ta_host_sweep(e.host, 1000, (ta_uint64_array){cold, 8});
    uint32_t n = ta_host_sweep(e.host, 1000, (ta_uint64_array){cold, 8});
    CHECK(n >= 1);
    world_close(&e);
}

/* a hibernated actor's blob, copied (the host's view ends at the ack) */
typedef struct kept_blob {
    unsigned char bytes[1 << 14];
    size_t length;
} kept_blob;

static void keep(host_world *e, uint64_t session, kept_blob *blob) {
    ta_bytes written;
    CHECK_EQUAL(ta_host_hibernate(e->host, session, &written), TA_OK);
    blob->length = output_copy(written, blob->bytes, sizeof blob->bytes);
    ta_host_effects_ack(e->host);
}

static ta_bytes kept(const kept_blob *blob) {
    return (ta_bytes){blob->bytes, blob->length};
}

/* A migration asks for a hibernated actor with its id, and again when it finds
 * it hibernated after a wake; an actor an event wakes (a WAKE with no migration)
 * joins a migration that waits for it, with or without TA_HOST_MIGRATE_WAKE; an
 * actor exported while the migration waits leaves it. */
static void test_migration_asks_again_and_takes_the_woken(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
    world_add(&e, W1);
    uint64_t a = spawn(&e, "w"), b = spawn(&e, "w"), c = spawn(&e, "w"), d = spawn(&e, "w");
    run(&e);
    static kept_blob blob_a, blob_b, blob_c;
    keep(&e, a, &blob_a);
    keep(&e, b, &blob_b);
    keep(&e, c, &blob_c);
    /* an event's WAKE names no migration */
    CHECK_EQUAL(ta_host_send(e.host, &(ta_event_in){.size = sizeof(ta_event_in), .session = c, .name = {"pong", 4}}), TA_OK);
    run(&e);
    CHECK(e.wake_count == 1 && e.wake_session == c && e.wake_migration == 0u);
    world_add(&e, W2);
    uint32_t migration;
    CHECK_EQUAL(ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, TA_HOST_MIGRATE_WAKE, 0, 0}, &migration),
                TA_OK);
    run(&e); /* d moves; a, b and c are asleep, each asked for (c besides its event's WAKE) */
    CHECK_EQUAL(e.migrated_count, 1);
    CHECK_EQUAL(e.wake_count, 4);
    CHECK_EQUAL(e.wake_migration, migration);
    uint64_t pending[8];
    CHECK_EQUAL(ta_host_migration_pending(e.host, migration, (ta_uint64_array){pending, 8}), 3u);
    /* a woken and put back before the migration tried it: asked for again */
    CHECK_EQUAL(ta_host_wake(e.host, a, kept(&blob_a)), TA_OK);
    keep(&e, a, &blob_a);
    run(&e);
    CHECK(e.wake_count == 5 && e.wake_session == a && e.wake_migration == migration);
    CHECK_EQUAL(ta_host_wake(e.host, a, kept(&blob_a)), TA_OK);
    CHECK_EQUAL(ta_host_wake(e.host, c, kept(&blob_c)), TA_OK); /* the event's actor: moved too */
    run(&e);
    CHECK_EQUAL(e.migrated_count, 3);
    CHECK_EQUAL(e.last_migration, 0u); /* b still asleep */
    CHECK_EQUAL(ta_host_wake(e.host, b, kept(&blob_b)), TA_OK);
    run(&e);
    CHECK(e.last_migration == migration && e.last_done.migrated == 4 && e.last_done.pending == 0 && e.last_done.failed == 0);
    (void)d;
    world_close(&e);

    /* without TA_HOST_MIGRATE_WAKE: an actor an event wakes joins the migration;
     * one exported meanwhile leaves it, and the migration ends with no timeout */
    world_open(&e, 16, NULL, 0);
    world_add(&e, W1);
    uint64_t x = spawn(&e, "w"), y = spawn(&e, "w");
    run(&e);
    static kept_blob blob_x, blob_y;
    keep(&e, x, &blob_x);
    keep(&e, y, &blob_y);
    world_add(&e, W2);
    CHECK_EQUAL(ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, 0, 0, 0}, &migration), TA_OK);
    run(&e);
    CHECK_EQUAL(e.wake_count, 0);
    CHECK_EQUAL(ta_host_send(e.host, &(ta_event_in){.size = sizeof(ta_event_in), .session = x, .name = {"pong", 4}}), TA_OK);
    run(&e);
    CHECK(e.wake_count == 1 && e.wake_session == x && e.wake_migration == 0u);
    CHECK_EQUAL(ta_host_wake(e.host, x, kept(&blob_x)), TA_OK);
    run(&e);
    CHECK_EQUAL(e.migrated_count, 1);
    CHECK_EQUAL(e.last_migration, 0u); /* y asleep */
    CHECK_EQUAL(ta_host_wake(e.host, y, kept(&blob_y)), TA_OK); /* y ends: woken, terminated */
    CHECK_EQUAL(ta_host_terminate(e.host, y), TA_OK);
    run(&e);
    CHECK(e.last_migration == migration && e.last_done.migrated == 1 && e.last_done.pending == 0);
    world_close(&e);
}

/* A migration over many hibernated actors: the walk (a quantum of 3 per run here)
 * asks for each once, and the runs after it look at none of the sleeping
 * ones. */
static void test_migration_of_many_hibernated_actors(void) {
    host_world e;
    world_open(&e, 4096, NULL, 0);
    world_add(&e, W1);
    enum { N = 3000 };
    static uint64_t sessions[N];
    for (int i = 0; i < N; i++)
        sessions[i] = spawn(&e, "w");
    run(&e);
    static kept_blob blob;
    for (int i = 0; i < N; i++)
        keep(&e, sessions[i], &blob); /* the blobs are not needed: nobody wakes them */
    world_add(&e, W2);
    uint32_t migration;
    CHECK_EQUAL(ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"w", 1}, 0, TA_HOST_MIGRATE_WAKE, 0, 0}, &migration),
                TA_OK);
    for (int i = 0; i < 2 * N && e.wake_count < N; i++) {
        ta_host_run(e.host, 100000);
        drain(&e);
    }
    CHECK_EQUAL(e.wake_count, N); /* each asked for once */
    uint64_t one;
    CHECK_EQUAL(ta_host_migration_pending(e.host, migration, (ta_uint64_array){&one, 1}), (uint32_t)N);
    ta_run result = ta_host_run(e.host, 100000); /* waits: idle, nothing asked again */
    drain(&e);
    CHECK_EQUAL(result.status, TA_RUN_IDLE);
    CHECK_EQUAL(e.wake_count, N);
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
    uint32_t migration;
    CHECK_EQUAL(
        ta_host_migrate(e.host, &(ta_host_migration){(ta_string_view){"p", 1}, 0, 0, 0, 0}, &migration),
        TA_OK);
    for (int i = 0; i < 5; i++)
        run(&e);
    CHECK_EQUAL(e.migrated_count, 0); /* waiting: its kid runs */
    CHECK_EQUAL(e.last_migration, 0u);
    uint32_t again;
    CHECK_EQUAL(ta_host_locate(e.host, p, &location), TA_OK);
    again = location.actor;
    CHECK_EQUAL(again, actor); /* left alone meanwhile: not exported and re-adopted */
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, p, "finish", NULL), TA_OK);
    run(&e);
    run(&e);
    CHECK_EQUAL(e.migrated_count, 1);
    CHECK(e.last_migration == migration && e.last_done.migrated == 1 && e.last_done.pending == 0);
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

/* a target that is no absolute URI is invalid
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

/* A send to an I/O processor parks its sender until the batch is acked;
 * the other actors go on, and what the handler gives before the ack (an
 * error, or an event back) comes before what the sender does next: the
 * BasicHTTP cases of W3C 577 and 201 */
#define IO_TYPE "http://example.com/io"
static void test_a_sender_waits_for_its_send(void) {
    ta_io_processor_entry io = {{IO_TYPE, (uint32_t)strlen(IO_TYPE)}, {"http://example.com/", 19}};
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
        world_add(&e, SC("o", "initial='a'><state id='a'><onentry><log expr=\"'other'\"/></onentry></state>"));
        uint64_t session = spawn(&e, "s");
        spawn(&e, "o");
        ta_run result = ta_host_run(e.host, 100000);
        CHECK_EQUAL(result.status, TA_RUN_EFFECTS);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(e.host, &effects);
        int sends = 0, other = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (effects[i].kind == TA_EFFECT_SEND) {
                sends++;
                CHECK_EQUAL(effects[i].io_processor, 1); /* the config's first */
                CHECK_EQUAL(effects[i].session, session);
                CHECK(effects[i].flags & TA_EFFECT_FLAG_WAITING);
            }
            other += effects[i].kind == TA_EFFECT_LOG; /* only the other actor has logged */
        }
        CHECK_EQUAL(sends, 1);
        CHECK_EQUAL(other, 1);
        if (answer == 0) { /* before the ack: the handler's outcome */
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
        drain(&e); /* the ack: the send is decided, the sender goes on */
        run(&e);
        CHECK(strcmp(e.log, answer == 0 ? "other;next;ok:error.communication;" : "other;next;ok:back;") == 0);
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
                          "<final id='done'><onentry><log expr=\"'done'\"/></onentry>"
                          "<donedata><param name='n' expr='n'/></donedata></final>"));
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
    ta_host_advance_time(e.host, (ta_host_time){50, 0});
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
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_bytes images[2] = {ta_image_bytes(&e.image[0]), ta_image_bytes(&e.image[1])};
    CHECK_EQUAL(
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, 1, 0}, &e.host),
        TA_ERROR_INVALID); /* too few */
    ta_bytes swapped[2] = {ta_image_bytes(&e.image[1]), ta_image_bytes(&e.image[0])};
    CHECK_EQUAL(
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, swapped, 2, 0}, &e.host),
        TA_ERROR_INVALID); /* wrong ones */
    CHECK_EQUAL(
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, 2, 0}, &e.host),
        TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}),
                TA_ERROR_STATE); /* not fresh any more */
    e.log[0] = 0;
    run(&e); /* the queued event, with its payload */
    CHECK(strcmp(e.log, "n=5;") == 0);
    ta_host_advance_time(e.host, (ta_host_time){120, 0}); /* the delayed send, armed before the snapshot */
    run(&e);
    if (strcmp(e.log, "n=5;ticked:5;done;")) fprintf(stderr, "  log: %s\n", e.log);
    CHECK(strcmp(e.log, "n=5;ticked:5;done;") == 0); /* routed to the other segment and back */
    /* its <donedata> reaches the host with DONE, from the restored datamodel */
    CHECK_EQUAL(e.done_session, c);
    if (strcmp(e.done_data, "{\"n\":5}")) fprintf(stderr, "  donedata: %s\n", e.done_data);
    CHECK(strcmp(e.done_data, "{\"n\":5}") == 0);
    ta_host_stats stats;
    stats.size = sizeof stats;
    CHECK_EQUAL(host_stats_fill(e.host, &stats), TA_OK);
    CHECK(stats.routed >= 2 && stats.segments == 2);
    free(hs);
    free(ds);
    world_close(&e);
}

/* Two segments running the same image: the ECMAScript datamodel's snapshot
 * names a session's chart by its segment too, so after a restore each
 * session runs its own segment's attachment, and the first segment retiring
 * leaves the second's sessions working. */
static void test_snapshot_of_one_image_in_two_segments(void) {
    host_world e;
    world_open(&e, 16, NULL, 0);
#define TWICE SC("t", "initial='a'><datamodel><data id='n' expr='0'/></datamodel><state id='a'>"                \
                     "<transition event='inc'><assign location='n' expr='n + 1'/>"                            \
                     "<log expr=\"'n=' + n\"/></transition><transition event='stop' target='f'/></state>"   \
                     "<final id='f'/>")
    world_add(&e, TWICE);
    uint64_t first = spawn(&e, "t");
    world_add(&e, TWICE); /* the same content: another segment, the current version */
    uint64_t second = spawn(&e, "t");
    run(&e);
    ta_buffer written = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_ERROR_NO_MEMORY);
    size_t hl = written.length;
    void *hs = malloc(hl);
    written = (ta_buffer){hs, hl, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_OK);
    written = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_ERROR_NO_MEMORY);
    size_t dl = written.length;
    void *ds = malloc(dl);
    written = (ta_buffer){ds, dl, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_OK);
    ta_host_free(e.host);
    ta_datamodel_ecmascript_free(e.ecmascript);
    e.ecmascript = ta_datamodel_ecmascript_new(NULL);
    e.binding = ta_datamodel_ecmascript_binding(e.ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_bytes images[2] = {ta_image_bytes(&e.image[0]), ta_image_bytes(&e.image[1])};
    CHECK_EQUAL(test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, 2, 0}, &e.host), TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(e.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    CHECK_EQUAL(send(&e, first, "stop", "null"), TA_OK);
    run(&e);
    run(&e); /* the first segment has no actors left, and its chart a newer version: it retires */
    CHECK(e.freed_count >= 1);
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, second, "inc", "null"), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "n=1;") == 0);
    free(hs);
    free(ds);
    world_close(&e);
#undef TWICE
}

/* configurations as nested state values: atomic, compound, parallel regions
 * (an atomic region is {}), a missing session */
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
    CHECK_EQUAL(ask_state(e.host, session, value, sizeof value), TA_OK);
    CHECK(strcmp(value, "{\"top\":{\"p\":{\"r1\":\"x\",\"r2\":{},\"r3\":{\"k\":\"k1\"}}}}") == 0);
    CHECK_EQUAL(send(&e, session, "go", NULL), TA_OK);
    run(&e);
    CHECK_EQUAL(ask_state(e.host, session, value, sizeof value), TA_OK);
    CHECK(strcmp(value, "{\"top\":{\"p\":{\"r1\":\"y\",\"r2\":{},\"r3\":{\"k\":\"k1\"}}}}") == 0);
    CHECK_EQUAL(send(&e, session, "flat", NULL), TA_OK);
    run(&e);
    CHECK_EQUAL(ask_state(e.host, session, value, sizeof value), TA_OK);
    CHECK(strcmp(value, "\"flat\"") == 0);
    CHECK_EQUAL(ask_state(e.host, session + 1000, value, sizeof value), TA_OK);
    CHECK(strstr(value, "\"reason\":\"stale\"") != NULL);
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
    ta_bytes image = ta_host_snapshot_image((ta_bytes){pack, length}, 0);
    ta_bytes original = ta_image_bytes(&e.image[0]);
    CHECK(image.pointer && image.length == original.length &&
          !memcmp(image.pointer, original.pointer, original.length));
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
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = contents.system_id;
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.datamodels = &f.binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    ta_bytes images[1] = {image}; /* the pack's bytes: the host copies them */
    CHECK_EQUAL(test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {contents.host, contents.host_length}, images, 1, 0}, &f.host),
                TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(f.ecmascript, (ta_bytes){state, dl}), TA_OK);
    CHECK_EQUAL(ta_host_locate(f.host, session, &(ta_host_location){0, 0}), TA_OK);
    ta_host_free(f.host);
    ta_datamodel_ecmascript_free(f.ecmascript);
    free(state);
    free(pack);
    world_close(&e);
}

/* Whether a session is hibernated (1), in memory (0), or neither (-1). */
static int hibernated(host_world *e, uint64_t session) {
    ta_host_location location;
    if (ta_host_locate(e->host, session, &location) != TA_OK) return -1;
    ta_actor_info info;
    memset(&info, 0, sizeof info);
    info.size = sizeof info;
    if (ta_actor_info_get(ta_host_system(e->host, location.segment), location.actor, &info) != TA_OK) return -1;
    return info.state == TA_ACTOR_HIBERNATED;
}

/* Hibernation through the host, which keeps the blob: an event for the
 * hibernated actor asks for it once (WAKE) and waits; so does the actor's
 * own timer firing; woken, it goes on with its variables and its state. */
#define SLEEPER                                                                                    \
    SC("sleeper", "initial='idle'><datamodel><data id='hits' expr='0'/></datamodel>"               \
                  "<state id='idle'><onentry><send event='alarm' delay='200ms'/></onentry>"        \
                  "<transition event='touch'><assign location='hits' expr='hits + 1'/>"            \
                  "<log expr=\"'touched ' + hits\"/></transition>"                                 \
                  "<transition event='alarm'><log expr=\"'alarm after ' + hits\"/></transition>"   \
                  "</state>")

static void test_hibernate_and_wake(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    world_add(&e, SLEEPER);
    uint64_t s = spawn(&e, "sleeper");
    run(&e);
    CHECK_EQUAL(send(&e, s, "touch", NULL), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "touched 1;") == 0);
    ta_bytes written; /* the host's, until the ack */
    CHECK_EQUAL(ta_host_hibernate(e.host, s, &written), TA_OK);
    e.blob_length = output_copy(written, e.blob, sizeof e.blob);
    ta_host_effects_ack(e.host); /* the output is copied: its batch ends */
    CHECK_EQUAL(hibernated(&e, s), 1);
    CHECK_EQUAL(send(&e, s, "touch", NULL), TA_OK);
    run(&e);
    CHECK(e.wake_count == 1 && e.wake_session == s);
    CHECK(strcmp(e.log, "touched 1;") == 0); /* the event waits for the blob */
    CHECK_EQUAL(hibernated(&e, s), 1);
    CHECK_EQUAL(ta_host_wake(e.host, s, (ta_bytes){e.blob, e.blob_length}), TA_OK);
    run(&e);
    CHECK_EQUAL(hibernated(&e, s), 0);
    CHECK(strcmp(e.log, "touched 1;touched 2;") == 0);
    char value[64];
    CHECK_EQUAL(ask_state(e.host, s, value, sizeof value), TA_OK);
    CHECK(strcmp(value, "\"idle\"") == 0);
    /* again, and the alarm it set before both hibernations wakes it */
    CHECK_EQUAL(ta_host_hibernate(e.host, s, &written), TA_OK);
    e.blob_length = output_copy(written, e.blob, sizeof e.blob);
    ta_host_effects_ack(e.host); /* the output is copied: its batch ends */
    CHECK_EQUAL(hibernated(&e, s), 1);
    ta_host_advance_time(e.host, (ta_host_time){250, 0});
    run(&e);
    CHECK(e.wake_count == 2 && e.wake_session == s);
    CHECK_EQUAL(ta_host_wake(e.host, s, (ta_bytes){e.blob, e.blob_length}), TA_OK);
    run(&e);
    CHECK_EQUAL(hibernated(&e, s), 0);
    if (strcmp(e.log, "touched 1;touched 2;alarm after 2;")) fprintf(stderr, "  log: %s\n", e.log);
    CHECK(strcmp(e.log, "touched 1;touched 2;alarm after 2;") == 0);
    world_close(&e);
}

/* An <invoke srcexpr> the image does not resolve is a REQUEST effect for the
 * host, with the source as written and the segment to answer in. Answered
 * with an error, the invoke is error.execution; the actor ends, and the host
 * gets its DONE. */
#define ASKER                                                                                      \
    SC("asker", "initial='a'><state id='a'><invoke id='ghost' srcexpr=\"'missing.scxml'\"/>"       \
                "<transition event='error.execution' target='end'>"                               \
                "<log expr=\"'cannot invoke'\"/></transition>"                                     \
                "<transition event='done.invoke.ghost' target='bad'/></state>"                     \
                "<final id='end'/><final id='bad'/>")

/* ta_host_add_image copies the image (docs/adr/0011, decision 7): the
 * caller's bytes, at any alignment, may be changed and freed once the call
 * returns; bytes that are no image add nothing. */
static void test_image_copied(void) {
    host_world e;
    world_open(&e, 0, NULL, 0);
    size_t length;
    void *compiled = compile_chart(
        "c.scxml",
        SC("p", "initial='a'><state id='a'><transition event='go' target='b'/></state>"
                "<state id='b'><onentry><log expr=\"'at b'\"/></onentry></state>"),
        NULL, &length, 0);
    CHECK(compiled != NULL);
    unsigned char *odd = (unsigned char *)malloc(length + 1);
    memcpy(odd + 1, compiled, length);
    uint32_t segment = 9;
    CHECK_EQUAL(ta_host_add_image(e.host, (ta_bytes){odd + 1, length}, &segment), TA_OK);
    CHECK_EQUAL(segment, 0u);
    memset(odd, 0xa5, length + 1); /* the caller's bytes change, then go */
    free(odd);
    CHECK_EQUAL(ta_host_add_image(e.host, (ta_bytes){compiled, 16}, &segment), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_host_segment_count(e.host), 1u); /* nothing added */
    free(compiled);
    ta_host_segment_info info;
    info.size = sizeof info;
    CHECK_EQUAL(ta_host_segment_info_get(e.host, 0, &info), TA_OK);
    CHECK(info.image != NULL);
    CHECK_EQUAL((uintptr_t)ta_image_bytes(info.image).pointer & 7u, 0u); /* opened 8-aligned */
    CHECK_EQUAL(ta_image_bytes(info.image).length, length);
    uint64_t p = spawn(&e, "p");
    CHECK_EQUAL(send(&e, p, "go", NULL), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "at b;") == 0);
    world_close(&e);
}

/* A datamodel that ran charts of a host now freed keeps them idle, with the
 * image the host freed; restoring sessions onto another image's charts
 * compares the hashes stored at attach and reads no freed image
 * (ta_datamodel_ecmascript_restore, under ASan). */
static void test_restore_after_images_freed(void) {
    host_world old;
    world_open(&old, 0, NULL, 0);
    world_add(&old, SC("x", "initial='a'><state id='a'/>"));
    spawn(&old, "x");
    run(&old);
    ta_host_free(old.host); /* x's image goes; its chart stays in old's datamodel, idle */
    old.host = NULL;
    host_world e;
    world_open(&e, 0, NULL, 0);
    world_add(&e, SC("y", "initial='a'><datamodel><data id='n' expr='7'/></datamodel>"
                          "<state id='a'><transition event='show'><log expr=\"'n=' + n\"/>"
                          "</transition></state>"));
    uint64_t y = spawn(&e, "y");
    run(&e);
    ta_buffer written = {NULL, 0, 0};
    ta_host_snapshot(e.host, &written);
    void *hs = malloc(written.length);
    written = (ta_buffer){hs, written.length, 0};
    CHECK_EQUAL(ta_host_snapshot(e.host, &written), TA_OK);
    size_t hl = written.length;
    written = (ta_buffer){NULL, 0, 0};
    ta_datamodel_ecmascript_snapshot(e.ecmascript, &written);
    void *ds = malloc(written.length);
    written = (ta_buffer){ds, written.length, 0};
    CHECK_EQUAL(ta_datamodel_ecmascript_snapshot(e.ecmascript, &written), TA_OK);
    size_t dl = written.length;
    ta_bytes images[1] = {ta_image_bytes(&e.image[0])};
    ta_host_free(e.host);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.datamodels = &old.binding; /* the datamodel with x's idle chart */
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(
        test_host_restore(&config, &(ta_host_snapshot_input){{0, 0}, {hs, hl}, images, 1, 0}, &e.host),
        TA_OK);
    CHECK_EQUAL(ta_datamodel_ecmascript_restore(old.ecmascript, (ta_bytes){ds, dl}), TA_OK);
    ta_datamodel_ecmascript_free(e.ecmascript);
    e.ecmascript = old.ecmascript;
    old.ecmascript = NULL;
    e.log[0] = 0;
    CHECK_EQUAL(send(&e, y, "show", NULL), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "n=7;") == 0);
    world_close(&e);
    world_close(&old);
    free(hs);
    free(ds);
}

/* A system manifest declaring three boot actors of a chart that logs on
 * entry, compiled and added to `e`'s host; its segment. */
static uint32_t add_boot_system(host_world *e) {
    static const test_file files[] = {
        {"worker.scxml", SC("worker", "initial='ready'><state id='ready'>"
                                     "<onentry><log expr=\"'boot'\"/></onentry></state>")},
        {NULL, NULL}};
    const char *manifest = "<system><chart name='worker' src='worker.scxml'/>"
                           "<actor name='first' chart='worker'/>"
                           "<actor name='second' chart='worker'/>"
                           "<actor name='third' chart='worker'/></system>";
    ta_compile_options options = {0};
    options.size = sizeof options;
    options.resolve = test_resolve;
    options.resolve_context = (void *)files;
    ta_source source = {{"system.xml", 10}, {manifest, (uint32_t)strlen(manifest)}};
    ta_compile_result *compiled = NULL;
    CHECK_EQUAL(ta_compile_system(&options, &source, &compiled), TA_OK);
    ta_string_view image = ta_compile_image(compiled);
    uint32_t segment = 0;
    CHECK_EQUAL(ta_host_add_image(e->host, (ta_bytes){image.pointer, image.length}, &segment), TA_OK);
    ta_compile_free(compiled);
    return segment;
}

/* Boot is the public path from a compiled system manifest to live actors.
 * Start with room for only one actor: boot must grow before spawning the
 * three declared actors, and leave their entry actions for the host loop. */
static void test_boot_manifest(void) {
    host_world e;
    world_open(&e, 1, NULL, 0);
    uint32_t segment = add_boot_system(&e);

    ta_host_segment_info info = {0};
    info.size = sizeof info;
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 0);
    ta_system_stats stats = {0};
    stats.size = sizeof stats;
    ta_system_stats_get(ta_host_system(e.host, segment), &stats);
    CHECK_EQUAL(stats.actors_capacity, 1);
    CHECK_EQUAL(ta_host_actor_by_name(e.host, (ta_string_view){"first", 5}), 0);
    CHECK_EQUAL(ta_host_boot(e.host, segment + 1), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 0);

    CHECK_EQUAL(ta_host_boot(e.host, segment), TA_OK);
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 3);
    ta_system_stats_get(ta_host_system(e.host, segment), &stats);
    CHECK(stats.actors_capacity >= 3);
    uint64_t first = ta_host_actor_by_name(e.host, (ta_string_view){"first", 5});
    uint64_t second = ta_host_actor_by_name(e.host, (ta_string_view){"second", 6});
    uint64_t third = ta_host_actor_by_name(e.host, (ta_string_view){"third", 5});
    CHECK(first && second && third);
    CHECK(first != second && first != third && second != third);
    drain(&e); /* detect entry logs emitted eagerly by boot, and ack before run */
    CHECK_EQUAL(e.log[0], 0);
    CHECK_EQUAL(run(&e), TA_RUN_IDLE);
    CHECK(strcmp(e.log, "boot;boot;boot;") == 0);
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 3);
    world_close(&e);
}

/* The allocator refuses the growth boot needs: TA_ERROR_NO_MEMORY before any
 * boot actor is spawned, and a boot once it gives room spawns each once. */
static void test_boot_refused(void) {
    host_world e;
    world_open(&e, 1, NULL, 0);
    uint32_t segment = add_boot_system(&e);
    ta_host_segment_info info = {0};
    info.size = sizeof info;
    refusing = 1;
    CHECK_EQUAL(ta_host_boot(e.host, segment), TA_ERROR_NO_MEMORY);
    refusing = 0;
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 0); /* none of the three */
    CHECK_EQUAL(ta_host_actor_by_name(e.host, (ta_string_view){"first", 5}), 0);
    CHECK_EQUAL(ta_host_boot(e.host, segment), TA_OK);
    CHECK_EQUAL(ta_host_segment_info_get(e.host, segment, &info), TA_OK);
    CHECK_EQUAL(info.actors, 3);
    drain(&e);
    CHECK_EQUAL(run(&e), TA_RUN_IDLE);
    CHECK(strcmp(e.log, "boot;boot;boot;") == 0); /* each once */
    world_close(&e);
}

static void test_request_answered_with_an_error(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    world_add(&e, ECHO);
    uint32_t segment = world_add(&e, ASKER);
    uint64_t s = spawn(&e, "asker");
    run(&e);
    CHECK_EQUAL(e.request_count, 1);
    CHECK_EQUAL(e.request_segment, segment);
    CHECK(strcmp(e.request_source, "missing.scxml") == 0);
    CHECK_EQUAL(e.done_count, 0); /* it waits for the answer */
    CHECK_EQUAL(ta_host_resume(e.host, &(ta_resume){segment, e.request_token, TA_ERROR_INVALID, 0, 0}), TA_OK);
    run(&e);
    CHECK(strcmp(e.log, "cannot invoke;") == 0);
    CHECK(e.done_count == 1 && e.done_session == s);
    world_close(&e);
}

/* A segment grows only the pool that ran out: spawning past maxActors
 * grows the actors, by at least a quarter of the region (8 to 512 at once
 * here, beside the default events), and the slab by what they need (a
 * grown segment is the size of one made for that many actors), and the
 * other pools stay; events past maxEvents grow the events alone. The
 * high-water marks are what the pools held at most. */
static ta_system_stats segment_stats(host_world *e, uint32_t segment) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(ta_host_system(e->host, segment), &stats);
    return stats;
}

static void test_growth_grows_what_ran_out(void) {
    host_world sized;
    world_open(&sized, 512, NULL, 0);
    uint32_t sized_segment = world_add(&sized, ECHO);
    ta_system_stats want = segment_stats(&sized, sized_segment);
    world_close(&sized);

    host_world e;
    world_open(&e, 8, NULL, 0);
    uint32_t segment = world_add(&e, ECHO);
    ta_system_stats before = segment_stats(&e, segment);
    uint64_t sessions[40];
    for (int i = 0; i < 40; i++)
        sessions[i] = spawn(&e, "echo");
    ta_system_stats grown = segment_stats(&e, segment);
    CHECK_EQUAL(grown.actors_capacity, 512u); /* one relocation, not 8, 16, 32, 64 */
    CHECK(grown.region_bytes >= before.region_bytes + before.region_bytes / 4);
    CHECK_EQUAL(grown.actors_high_water, 40u);
    CHECK_EQUAL(grown.events_capacity, before.events_capacity);
    CHECK_EQUAL(grown.timers_capacity, before.timers_capacity);
    CHECK_EQUAL(grown.requests_capacity, before.requests_capacity);
    CHECK_EQUAL(grown.workspaces_capacity, before.workspaces_capacity);
    CHECK_EQUAL(grown.trace_contexts_capacity, before.trace_contexts_capacity);
    CHECK_EQUAL(grown.slab_pages_capacity, want.slab_pages_capacity);
    CHECK_EQUAL(grown.region_bytes, want.region_bytes);
    run(&e);

    /* more events than the pool holds, for one actor that has not run */
    uint32_t events = before.events_capacity + before.events_capacity / 2;
    for (uint32_t i = 0; i < events; i++)
        CHECK_EQUAL(send(&e, sessions[0], "noop", NULL), TA_OK);
    ta_system_stats more = segment_stats(&e, segment);
    CHECK_EQUAL(more.events_capacity, before.events_capacity * 2);
    CHECK(more.events_high_water >= events);
    CHECK_EQUAL(more.actors_capacity, 512u);
    CHECK_EQUAL(more.timers_capacity, before.timers_capacity);
    CHECK_EQUAL(more.slab_pages_capacity, grown.slab_pages_capacity);
    run(&e);
    ta_system_stats after = segment_stats(&e, segment);
    CHECK(after.events < events); /* handled (an actor may keep its last one) */
    CHECK_EQUAL(after.events_high_water, more.events_high_water); /* a mark, not a gauge */
    world_close(&e);
}

/* Beside many actors the events are a small share of the region: their
 * pool grows by at least a quarter of the region at once, not by doubling
 * from 256, which would copy the whole region at every doubling. */
static void test_a_small_pool_grows_by_a_share_of_the_region(void) {
    host_world e;
    world_open(&e, 8, NULL, 0);
    uint32_t segment = world_add(&e, ECHO);
    uint64_t first = 0;
    for (int i = 0; i < 4000; i++) {
        uint64_t session = spawn(&e, "echo");
        if (!first) first = session;
        if (i % 256 == 255) run(&e);
    }
    run(&e);
    ta_system_stats before = segment_stats(&e, segment);
    for (uint32_t i = 0; i <= before.events_capacity; i++)
        CHECK_EQUAL(send(&e, first, "noop", NULL), TA_OK);
    ta_system_stats after = segment_stats(&e, segment);
    CHECK(after.events_capacity > before.events_capacity * 2);
    CHECK(after.region_bytes >= before.region_bytes + before.region_bytes / 4);
    CHECK_EQUAL(after.actors_capacity, before.actors_capacity);
    run(&e);
    world_close(&e);
}

/* Senders hold their workspaces until their sends are handled: actors left
 * waiting for one make the host add workspaces (as many as hold and wait),
 * so 256 actors sending at once are handed out in one batch, not four at a
 * time. A segment whose actors never wait keeps its four. */
static void test_workspaces_grow_for_waiting_senders(void) {
    ta_io_processor_entry io = {{IO_TYPE, (uint32_t)strlen(IO_TYPE)}, {"http://example.com/", 19}};
    host_world e;
    world_open(&e, 8, &io, 1);
    uint32_t segment = world_add(&e, SC("s", "initial='a'><state id='a'><transition event='go'>"
                                             "<send type='" IO_TYPE "' target='out' event='x'/>"
                                             "</transition></state>"));
    uint64_t sessions[256];
    for (int i = 0; i < 256; i++)
        sessions[i] = spawn(&e, "s");
    run(&e);
    CHECK_EQUAL(segment_stats(&e, segment).workspaces_capacity, 4u);
    for (int i = 0; i < 256; i++)
        CHECK_EQUAL(send(&e, sessions[i], "go", NULL), TA_OK);
    int cycles = 0, sends = 0;
    while (sends < 256 && cycles < 1000) {
        ta_run result = ta_host_run(e.host, 1000000);
        CHECK(result.status != TA_RUN_FAULT && result.status != TA_RUN_EXHAUSTED);
        const ta_host_effect *effects;
        uint32_t n = ta_host_effects(e.host, &effects);
        for (uint32_t i = 0; i < n; i++)
            sends += effects[i].kind == TA_EFFECT_SEND;
        ta_host_effects_ack(e.host);
        cycles++;
    }
    CHECK_EQUAL(sends, 256);
    CHECK_EQUAL(cycles, 1); /* four a batch before: 64 */
    ta_system_stats stats = segment_stats(&e, segment);
    CHECK_EQUAL(stats.workspaces_capacity, 256u); /* the holders and the waiters */
    CHECK_EQUAL(stats.workspaces_high_water, 256u);
    run(&e);
    world_close(&e);
}

/* Beside a large explicit maxEvents the actors are a small share of the
 * region: spawning past maxActors grows them by at least a quarter of the
 * region at once, not by doubling from 64, which would copy the whole
 * region at every doubling. */
static void test_actors_grow_by_a_share_of_the_region(void) {
    host_world e;
    memset(&e, 0, sizeof e);
    e.ecmascript = ta_datamodel_ecmascript_new(NULL);
    e.binding = ta_datamodel_ecmascript_binding(e.ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_events = 20000;
    config.datamodels = &e.binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, refusable_alloc, refusable_free};
    CHECK_EQUAL(test_host_new(&config, &e.host), TA_OK);
    uint32_t segment = world_add(&e, ECHO);
    ta_system_stats before = segment_stats(&e, segment);
    for (uint32_t i = 0; i <= before.actors_capacity; i++)
        spawn(&e, "echo");
    ta_system_stats after = segment_stats(&e, segment);
    CHECK(after.actors_capacity > before.actors_capacity * 2);
    CHECK(after.region_bytes >= before.region_bytes + before.region_bytes / 4);
    CHECK_EQUAL(after.events_capacity, before.events_capacity);
    run(&e);
    world_close(&e);
}

int main(void) {
    RUN(test_growth_grows_what_ran_out);
    RUN(test_a_small_pool_grows_by_a_share_of_the_region);
    RUN(test_workspaces_grow_for_waiting_senders);
    RUN(test_actors_grow_by_a_share_of_the_region);
    RUN(test_boot_manifest);
    RUN(test_boot_refused);
    RUN(test_image_copied);
    RUN(test_restore_after_images_freed);
    RUN(test_snapshot_pack);
    RUN(test_hibernate_and_wake);
    RUN(test_request_answered_with_an_error);
    RUN(test_state_value);
    RUN(test_snapshot_restore);
    RUN(test_snapshot_of_one_image_in_two_segments);
    RUN(test_bad_targets);
    RUN(test_a_sender_waits_for_its_send);
    RUN(test_fair_segments);
    RUN(test_routing_between_segments);
    RUN(test_class_across_segments);
    RUN(test_versions_and_migration);
    RUN(test_migration_wake_and_timeout);
    RUN(test_migration_asks_again_and_takes_the_woken);
    RUN(test_migration_of_many_hibernated_actors);
    RUN(test_migration_waits_for_invocations);
    TEST_MAIN_END();
}
