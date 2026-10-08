/* The otlp codec (tinyactors/otlp.h): a system's trace records to OTLP
 * protobuf and back. Well-formed requests of the right shape, granary's
 * vocabulary span by span for a request actor and a lookup actor, spans
 * open across runs, a drop ends them, a full staging is flushed and a full
 * table grown without losing a record, time by fuel interpolation, and the
 * bytes of a small request are fixed. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/otlp.h>
#include <tinyactors/system.h>
#include "trace/record.h"

#include <stdlib.h>
#include <string.h>

/* ---- a system, an encoder, and what they wrote ------------------------------------------ */

#define REQUESTS 64
#define SPANS    1024

typedef struct world {
    void *bytes;
    ta_image image;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    void *memory;
    ta_system *system;
    void *otlp_memory;
    size_t otlp_length;
    ta_otlp *otlp;
    uint64_t wall;
    unsigned char *requests[REQUESTS], *logs[REQUESTS];
    size_t request_lengths[REQUESTS], log_lengths[REQUESTS];
    uint32_t request_count, log_count, flushes, grows;
    ta_otlp_span spans[SPANS];
    uint32_t span_count;
} world;

static void world_otlp(world *w, const ta_otlp_config *config) {
    CHECK_EQUAL(ta_otlp_layout(config, &w->otlp_length), TA_OK);
    w->otlp_memory = malloc(w->otlp_length);
    CHECK_EQUAL(ta_otlp_init(w->otlp_memory, w->otlp_length, config, &w->otlp), TA_OK);
    ta_otlp_attribute service = {{"service.name", 12}, {"test", 4}};
    CHECK_EQUAL(ta_otlp_set_resource(w->otlp, &service, 1), TA_OK);
}

/* Charts compiled into one image. */
static void *compile_charts(const char *const *charts, uint32_t count, size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source sources[8];
    char names[8][32];
    for (uint32_t i = 0; i < count; i++) {
        snprintf(names[i], sizeof names[i], "chart%u.scxml", i);
        sources[i] = (ta_source){{names[i], (uint32_t)strlen(names[i])},
                                 {charts[i], (uint32_t)strlen(charts[i])}};
    }
    ta_compile_result *result = NULL;
    void *copy = NULL;
    if (ta_compile_charts(&options, sources, count, &result) == TA_OK) {
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

static int world_open(world *w, const char *const *charts, uint32_t count,
                      const ta_otlp_config *config) {
    memset(w, 0, sizeof *w);
    size_t length;
    w->bytes = compile_charts(charts, count, &length);
    if (!w->bytes || ta_image_open(w->bytes, length, &w->image) != TA_OK) return 0;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    ta_system_config system_config;
    memset(&system_config, 0, sizeof system_config);
    system_config.size = sizeof system_config;
    system_config.ticks_per_second = 1000;
    system_config.max_actors = 256;
    system_config.trace_kinds = TA_TRACE_FULL | TA_TRACE_VALUES;
    size_t need;
    ta_system_layout(&w->image, &system_config, &need);
    w->memory = malloc(need);
    if (ta_system_init(w->memory, need, &w->image, &system_config, &w->binding, 1, &w->system) !=
        TA_OK)
        return 0;
    world_otlp(w, config);
    w->wall = 1700000000000000000ull;
    return 1;
}

static void world_flush(world *w) {
    size_t size = ta_otlp_traces_size(w->otlp);
    if (size && w->request_count < REQUESTS) {
        unsigned char *bytes = malloc(size);
        CHECK_EQUAL(ta_otlp_put_traces(w->otlp, bytes, size - 1), 0); /* not its size */
        CHECK_EQUAL(ta_otlp_put_traces(w->otlp, bytes, size), size);
        CHECK_EQUAL(ta_otlp_traces_size(w->otlp), 0);
        w->request_lengths[w->request_count] = size;
        w->requests[w->request_count++] = bytes;
        ta_otlp_cursor cursor;
        memset(&cursor, 0, sizeof cursor);
        int32_t got;
        while (w->span_count < SPANS &&
               (got = ta_otlp_next_span(bytes, size, &cursor, &w->spans[w->span_count])) == 1)
            w->span_count++;
        CHECK(w->span_count == SPANS || got == 0);
    }
    size = ta_otlp_logs_size(w->otlp);
    if (size && w->log_count < REQUESTS) {
        unsigned char *bytes = malloc(size);
        CHECK_EQUAL(ta_otlp_put_logs(w->otlp, bytes, size), size);
        w->log_lengths[w->log_count] = size;
        w->logs[w->log_count++] = bytes;
    }
}

/* Read what the system wrote; flush or grow as the encoder asks. */
static void world_read(world *w) {
    ta_trace_view view;
    while (ta_system_trace(w->system, &view)) {
        ta_otlp_input input;
        memset(&input, 0, sizeof input);
        input.size = sizeof input;
        input.view = view;
        input.image = &w->image;
        input.datamodels = &w->binding;
        input.datamodel_count = 1;
        uint32_t cursor = 0, status;
        while ((status = ta_otlp_read(w->otlp, &input, &cursor)) != TA_OTLP_READ) {
            if (status == TA_OTLP_FLUSH) {
                w->flushes++;
                world_flush(w);
            } else {
                w->grows++;
                ta_otlp_config bigger;
                memset(&bigger, 0, sizeof bigger);
                bigger.size = sizeof bigger;
                ta_otlp_stats stats;
                stats.size = sizeof stats;
                ta_otlp_stats_get(w->otlp, &stats);
                bigger.open_spans = stats.open_capacity * 2;
                bigger.chunks = stats.chunks_capacity * 2;
                bigger.staging_bytes = stats.staging_capacity * 2;
                size_t length;
                CHECK_EQUAL(ta_otlp_layout(&bigger, &length), TA_OK);
                void *memory = malloc(length);
                ta_otlp *moved;
                CHECK_EQUAL(ta_otlp_relocate(w->otlp, memory, length, &bigger, &moved), TA_OK);
                free(w->otlp_memory);
                w->otlp_memory = memory;
                w->otlp = moved;
            }
        }
        CHECK_EQUAL(cursor, view.length);
        ta_system_trace_ack(w->system, view.length);
    }
}

static uint64_t fuel_clock(world *w) {
    ta_system_stats stats;
    stats.size = sizeof stats;
    ta_system_stats_get(w->system, &stats);
    return stats.fuel_clock;
}

/* One run, marked before and after, its effects drained and its records read. */
static ta_run world_run(world *w, uint32_t budget) {
    ta_otlp_mark(w->otlp, fuel_clock(w), w->wall);
    ta_run run = ta_system_run(w->system, budget);
    w->wall += 1000000; /* a millisecond a run */
    ta_otlp_mark(w->otlp, fuel_clock(w), w->wall);
    const ta_effect *effects;
    uint32_t n = ta_system_effects(w->system, &effects);
    ta_system_effects_ack(w->system, n);
    world_read(w);
    w->wall += 1000000; /* and between runs */
    return run;
}

static void world_settle(world *w) {
    for (int i = 0; i < 100 && world_run(w, 100000).status != TA_RUN_IDLE; i++)
        ;
}

static void world_close(world *w) {
    for (uint32_t i = 0; i < w->request_count; i++)
        free(w->requests[i]);
    for (uint32_t i = 0; i < w->log_count; i++)
        free(w->logs[i]);
    ta_system_deinit(w->system);
    free(w->memory);
    free(w->otlp_memory);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->bytes);
}

static uint32_t world_spawn(world *w, const char *name, const char *traceparent) {
    uint32_t actor = 0;
    CHECK_EQUAL(
        ta_system_spawn(
            w->system,
            ta_image_chart_by_name(&w->image, (ta_string_view){name, (uint32_t)strlen(name)}),
            (ta_string_view){name, (uint32_t)strlen(name)}, 0,
            (ta_string_view){traceparent, traceparent ? (uint32_t)strlen(traceparent) : 0}, &actor),
        TA_OK);
    return actor;
}

static void world_send(world *w, uint32_t actor, const char *name, const char *json,
                       const char *traceparent) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.target = actor;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    if (json)
        event.data = ta_datamodel_ecmascript_payload_from_json(
            w->ecmascript, (ta_string_view){json, (uint32_t)strlen(json)});
    if (traceparent)
        event.traceparent = (ta_string_view){traceparent, (uint32_t)strlen(traceparent)};
    CHECK_EQUAL(ta_system_send(w->system, &event), TA_OK);
}

static uint64_t session_of(world *w, uint32_t actor) {
    ta_actor_info info;
    info.size = sizeof info;
    return ta_actor_info_get(w->system, actor, &info) == TA_OK ? info.serial : 0;
}

/* ---- reading spans ------------------------------------------------------------------------ */

static int is(ta_string_view view, const char *text) {
    return view.length == strlen(text) && !memcmp(view.pointer, text, view.length);
}

static int attribute(ta_otlp_fields fields, const char *key, ta_otlp_value *out) {
    uint32_t at = 0;
    ta_string_view k;
    ta_otlp_value v;
    int32_t got;
    while ((got = ta_otlp_next_attribute(fields, &at, &k, &v)) == 1)
        if (is(k, key)) {
            *out = v;
            return 1;
        }
    CHECK(got == 0);
    return 0;
}

static int64_t int_of(ta_otlp_fields fields, const char *key) {
    ta_otlp_value v;
    return attribute(fields, key, &v) && v.kind == TA_OTLP_VALUE_INT ? v.integer : -1;
}

static const char *text_of(ta_otlp_fields fields, const char *key) {
    static char buffers[4][512];
    static int next;
    char *buffer = buffers[next++ % 4];
    ta_otlp_value v;
    if (!attribute(fields, key, &v) || v.kind != TA_OTLP_VALUE_STRING) return "<none>";
    snprintf(buffer, 512, "%.*s", (int)v.bytes.length, v.bytes.pointer);
    return buffer;
}

/* An array of strings as "a,b". */
static const char *texts_of(ta_otlp_fields fields, const char *key) {
    static char buffer[1024];
    ta_otlp_value v;
    if (!attribute(fields, key, &v) || v.kind != TA_OTLP_VALUE_ARRAY) return "<none>";
    buffer[0] = 0;
    uint32_t at = 0;
    ta_otlp_value item;
    while (ta_otlp_next_value(v.items, &at, &item) == 1) {
        size_t l = strlen(buffer);
        snprintf(buffer + l, sizeof buffer - l, "%s%.*s", l ? "," : "", (int)item.bytes.length,
                 item.bytes.pointer);
    }
    return buffer;
}

static const ta_otlp_span *span_named(world *w, const char *name, uint64_t session) {
    for (uint32_t i = 0; i < w->span_count; i++)
        if (is(w->spans[i].name, name) &&
            (!session || int_of(w->spans[i].attributes, "scxml.session_id") == (int64_t)session))
            return &w->spans[i];
    return NULL;
}

static uint32_t count_named(world *w, const char *name) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < w->span_count; i++)
        n += is(w->spans[i].name, name);
    return n;
}

static const ta_otlp_span *span_by_id(world *w, ta_span_id id) {
    for (uint32_t i = 0; i < w->span_count; i++)
        if (w->spans[i].span == id) return &w->spans[i];
    return NULL;
}

/* An event of a span by name, or 0. */
static int event_named(const ta_otlp_span *span, const char *name, ta_otlp_event *out) {
    uint32_t at = 0;
    while (ta_otlp_next_event(span->events, &at, out) == 1)
        if (is(out->name, name)) return 1;
    return 0;
}

static ta_span_id span_of(uint64_t session, uint64_t macrostep, uint32_t microstep) {
    return ta_step_key_span_id((ta_step_key){session, macrostep, microstep, 0});
}

#define SC(name, body)                                                                             \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='" name "' " body  \
    "</scxml>"

static const char *const PARENT = "00-0af7651916cd43dd8448eb211c80319c-b7ad6b7169203331-01";

/* ---- granary's issue and allowlist, span by span -------------------------------------------- */

/* An issue asks the allowlist whether its author may post, and closes or
 * allows on the verdict. One HTTP request: one trace. */
static const char *const GRANARY =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='issue' "
    "initial='idle'><datamodel><data id='login' expr='null'/></datamodel>"
    "<state id='idle'><transition event='issue.opened' target='checking'>"
    "<assign location='login' expr='_event.data.login'/></transition></state>"
    "<state id='checking'><onentry><send event='allowlist.check' targetexpr=\"'#_scxml_1'\">"
    "<param name='login' expr='login'/></send></onentry>"
    "<transition event='allowlist.verdict' cond='_event.data.allowed' target='allowed'/>"
    "<transition event='allowlist.verdict' target='closing'/></state>"
    "<state id='closing'><transition target='closed'/></state>"
    "<final id='allowed'/><final id='closed'/></scxml>";
static const char *const ALLOWLIST =
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='allowlist' "
    "initial='ready'><state id='ready'><transition event='allowlist.check'>"
    "<send event='allowlist.verdict' targetexpr='_event.origin'>"
    "<param name='login' expr='_event.data.login'/>"
    "<param name='allowed' expr=\"_event.data.login === 'alice'\"/></send></transition>"
    "</state></scxml>";

static void test_granary_vocabulary(void) {
    const char *charts[] = {ALLOWLIST, GRANARY};
    world w;
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.flags = TA_OTLP_VALUES;
    CHECK(world_open(&w, charts, 2, &config));
    uint32_t allowlist = world_spawn(&w, "allowlist", NULL), issue = world_spawn(&w, "issue", NULL);
    uint64_t sa = session_of(&w, allowlist), si = session_of(&w, issue);
    CHECK(sa == 1 && si == 2);
    world_settle(&w);
    world_send(&w, issue, "issue.opened", "{\"login\": \"mallory\"}", PARENT);
    world_settle(&w);
    world_flush(&w);

    /* lifecycle: spawned, with the actor's and the chart's names */
    const ta_otlp_span *spawned = span_named(&w, "scxml.spawned", si);
    CHECK(spawned != NULL);
    if (spawned) {
        CHECK(!strcmp(text_of(spawned->attributes, "tinyactors.actor.name"), "issue"));
        CHECK(!strcmp(text_of(spawned->attributes, "tinyactors.chart.name"), "issue"));
        CHECK(!strcmp(text_of(spawned->attributes, "tinyactors.span.kind"), "spawned"));
        CHECK(!strcmp(text_of(spawned->attributes, "scxml.actor"), "1:1"));
        CHECK_EQUAL(int_of(spawned->attributes, "scxml.definition"), 1);
        CHECK(spawned->end > spawned->start);
    }
    CHECK_EQUAL(count_named(&w, "scxml.macrostep bootstrap"), 2);
    const ta_otlp_span *boot = span_named(&w, "scxml.macrostep bootstrap", sa);
    CHECK(boot && !strcmp(texts_of(boot->attributes, "scxml.state"), "ready"));
    if (boot) CHECK_EQUAL(boot->parent, 0); /* a trace of its own */

    /* the request: issue.opened, under the caller's span */
    const ta_otlp_span *opened = span_named(&w, "scxml.macrostep issue.opened", si),
                       *opened_micro = span_named(&w, "scxml.microstep issue.opened", si),
                       *check = span_named(&w, "scxml.macrostep allowlist.check", sa),
                       *check_micro = span_named(&w, "scxml.microstep allowlist.check", sa),
                       *verdict = span_named(&w, "scxml.macrostep allowlist.verdict", si),
                       *verdict_micro = span_named(&w, "scxml.microstep allowlist.verdict", si),
                       *eventless = span_by_id(&w, span_of(si, 3, 2)),
                       *finished = span_named(&w, "scxml.finished", si),
                       *destroyed = span_named(&w, "scxml.destroyed", si);
    CHECK(opened && opened_micro && check && check_micro && verdict && verdict_micro && eventless &&
          finished && destroyed);
    if (!(opened && opened_micro && check && check_micro && verdict && verdict_micro && eventless &&
          finished && destroyed)) {
        for (uint32_t i = 0; i < w.span_count; i++)
            fprintf(stderr, "  span %.*s\n", (int)w.spans[i].name.length, w.spans[i].name.pointer);
        world_close(&w);
        return;
    }
    const ta_otlp_span *traced[] = {opened,  opened_micro,  check,    check_micro,
                                    verdict, verdict_micro, eventless};
    for (uint32_t i = 0; i < sizeof traced / sizeof traced[0]; i++) {
        CHECK(traced[i]->trace.high == 0x0af7651916cd43ddull &&
              traced[i]->trace.low == 0x8448eb211c80319cull);
        CHECK_EQUAL(traced[i]->kind, TA_OTLP_SPAN_INTERNAL);
        CHECK(traced[i]->end > traced[i]->start);
        CHECK(is(traced[i]->scope_name, "tinyactors"));
        CHECK(!strcmp(text_of(traced[i]->resource, "service.name"), "test"));
        CHECK(!strcmp(text_of(traced[i]->scope_attributes, "tinyactors.chart.name"),
                      text_of(traced[i]->attributes, "tinyactors.chart.name")));
    }
    /* the macrostep: its event, its cause, the configuration after it */
    CHECK_EQUAL(opened->parent, 0xb7ad6b7169203331ull);
    CHECK_EQUAL(opened->span, span_of(si, 2, 0));
    CHECK(!strcmp(text_of(opened->attributes, "scxml.event.name"), "issue.opened"));
    CHECK(!strcmp(text_of(opened->attributes, "scxml.event.type"), "external"));
    CHECK(!strcmp(text_of(opened->attributes, "scxml.event.data"), "{\"login\":\"mallory\"}"));
    CHECK(!strcmp(texts_of(opened->attributes, "scxml.state"), "checking"));
    CHECK_EQUAL(int_of(opened->attributes, "scxml.macrostep"), 2);
    CHECK_EQUAL(int_of(opened->attributes, "scxml.session_id"), (int64_t)si);
    CHECK(int_of(opened->attributes, "scxml.time") >= 0);
    CHECK(!strcmp(text_of(opened->attributes, "tinyactors.span.kind"), "macrostep"));
    CHECK(!strcmp(text_of(opened->attributes, "tinyactors.actor.name"), "issue"));
    /* its microstep: the transition, the states, what happened in it */
    CHECK_EQUAL(opened_micro->parent, opened->span);
    CHECK_EQUAL(opened_micro->span, span_of(si, 2, 1));
    CHECK(!strcmp(texts_of(opened_micro->attributes, "scxml.transition"),
                  "idle \xe2\x86\x92 checking on issue.opened"));
    CHECK(!strcmp(texts_of(opened_micro->attributes, "scxml.state.exited"), "idle"));
    CHECK(!strcmp(texts_of(opened_micro->attributes, "scxml.state.entered"), "checking"));
    CHECK_EQUAL(int_of(opened_micro->attributes, "scxml.microstep"), 1);
    CHECK(!strcmp(text_of(opened_micro->attributes, "tinyactors.span.kind"), "microstep"));
    CHECK(opened_micro->start >= opened->start && opened_micro->end <= opened->end);
    ta_otlp_event event;
    CHECK(event_named(opened_micro, "scxml.sent", &event));
    CHECK(!strcmp(text_of(event.attributes, "scxml.event.name"), "allowlist.check"));
    CHECK(event_named(opened_micro, "scxml.assign", &event));
    CHECK(!strcmp(text_of(event.attributes, "scxml.location"), "login"));
    CHECK(!strcmp(text_of(event.attributes, "scxml.value"), "\"mallory\""));
    /* the lookup: a child of the sending microstep, in the other actor */
    CHECK_EQUAL(check->parent, opened_micro->span);
    CHECK_EQUAL(int_of(check->attributes, "scxml.cause.session_id"), (int64_t)si);
    CHECK_EQUAL(int_of(check->attributes, "scxml.cause.macrostep"), 2);
    CHECK_EQUAL(int_of(check->attributes, "scxml.cause.microstep"), 1);
    CHECK(!strcmp(text_of(check->attributes, "scxml.event.data"), "{\"login\":\"mallory\"}"));
    CHECK(!strcmp(texts_of(check->attributes, "scxml.state"), "ready"));
    CHECK(!strcmp(text_of(check->attributes, "tinyactors.actor.name"), "allowlist"));
    CHECK_EQUAL(check_micro->parent, check->span);
    CHECK(!strcmp(texts_of(check_micro->attributes, "scxml.transition"),
                  "ready on allowlist.check")); /* targetless */
    CHECK(!strcmp(texts_of(check_micro->attributes, "scxml.state.entered"), ""));
    /* the reply: caused by the lookup's microstep */
    CHECK_EQUAL(verdict->parent, check_micro->span);
    CHECK_EQUAL(int_of(verdict->attributes, "scxml.cause.session_id"), (int64_t)sa);
    CHECK(!strcmp(text_of(verdict->attributes, "scxml.event.data"),
                  "{\"login\":\"mallory\",\"allowed\":false}"));
    CHECK(!strcmp(texts_of(verdict->attributes, "scxml.state"), "")); /* final: empty */
    CHECK(event_named(verdict, "scxml.condition", &event));           /* on the macrostep */
    CHECK(!strcmp(text_of(event.attributes, "scxml.transition"),
                  "checking \xe2\x86\x92 allowed on allowlist.verdict"));
    ta_otlp_value result;
    CHECK(attribute(event.attributes, "scxml.result", &result) &&
          result.kind == TA_OTLP_VALUE_BOOL && result.integer == 0);
    CHECK_EQUAL(verdict_micro->parent, verdict->span);
    CHECK(!strcmp(texts_of(verdict_micro->attributes, "scxml.transition"),
                  "checking \xe2\x86\x92 closing on allowlist.verdict"));
    CHECK_EQUAL(eventless->parent, verdict->span);
    CHECK(!strcmp(texts_of(eventless->attributes, "scxml.transition"),
                  "closing \xe2\x86\x92 closed"));
    CHECK(!strcmp(texts_of(eventless->attributes, "scxml.state.entered"), "closed"));
    CHECK(eventless->start >= verdict_micro->end);
    /* the end of the session */
    CHECK(!strcmp(text_of(finished->attributes, "scxml.final_state"), "closed"));
    CHECK(!strcmp(text_of(finished->attributes, "tinyactors.span.kind"), "finished"));
    CHECK(finished->trace.low == 0x8448eb211c80319cull);
    CHECK_EQUAL(finished->parent, 0);
    CHECK(!strcmp(text_of(destroyed->attributes, "tinyactors.span.kind"), "destroyed"));
    CHECK(finished->start >= verdict->end);
    /* nothing is left open, and every record was read */
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(w.otlp, &stats);
    CHECK_EQUAL(stats.open, 0);
    CHECK_EQUAL(stats.chunks, 0);
    CHECK_EQUAL(stats.truncated, 0);
    world_close(&w);
}

/* A macrostep that does not finish in one run stays open across runs and is
 * written once it ends; parent: link makes it a root linked to its cause. */
static void test_open_across_runs(void) {
    const char *charts[] = {SC("looper",
                               "initial='a'><datamodel><data id='n' expr='0'/></datamodel>"
                               "<state id='a'><transition event='go' target='b'>"
                               "<foreach array='Array.from({length: 300}, (_, i) => i)' item='i'>"
                               "<assign location='n' expr='n + i'/></foreach></transition></state>"
                               "<state id='b'/>")};
    world w;
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.flags = TA_OTLP_LINK_PARENTS;
    CHECK(world_open(&w, charts, 1, &config));
    uint32_t a = world_spawn(&w, "looper", NULL);
    uint64_t s = session_of(&w, a);
    world_settle(&w);
    world_flush(&w);
    world_send(&w, a, "go", NULL, PARENT);
    world_run(&w, 50); /* not enough for 300 assignments */
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(w.otlp, &stats);
    CHECK_EQUAL(stats.open, 1);
    CHECK(stats.chunks > 0); /* its assignments wait in chunks */
    world_flush(&w);
    CHECK(span_named(&w, "scxml.macrostep go", s) == NULL);
    world_settle(&w);
    world_flush(&w);
    const ta_otlp_span *go = span_named(&w, "scxml.macrostep go", s),
                       *micro = span_named(&w, "scxml.microstep go", s);
    CHECK(go && micro);
    if (go && micro) {
        CHECK_EQUAL(go->parent, 0); /* a root */
        ta_otlp_link link;
        uint32_t at = 0;
        CHECK_EQUAL(ta_otlp_next_link(go->links, &at, &link), 1);
        CHECK_EQUAL(link.span, 0xb7ad6b7169203331ull);
        CHECK(go->end - go->start > 2000000); /* across several runs (2 ms each) */
        uint32_t events = 0;
        at = 0;
        ta_otlp_event event;
        while (ta_otlp_next_event(micro->events, &at, &event) == 1)
            events++;
        CHECK_EQUAL(events, 300);
    }
    ta_otlp_stats_get(w.otlp, &stats);
    CHECK(stats.open == 0 && stats.chunks == 0);
    world_close(&w);
}

/* A staging too small for a burst is flushed as often as needed, and a
 * table too small is grown: no record is lost. */
static void test_flush_and_grow(void) {
    const char *charts[] = {SC("cell",
                               "initial='a'><state id='a'>"
                               "<transition event='poke' target='b'/></state><state id='b'/>")};
    world w;
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    config.open_spans = 1;
    config.chunks = 2;
    CHECK(world_open(&w, charts, 1, &config));
    for (int i = 0; i < 150; i++)
        world_spawn(&w, "cell", NULL);
    world_settle(&w);
    world_flush(&w);
    CHECK(w.flushes > 0);
    CHECK_EQUAL(count_named(&w, "scxml.spawned"), 150);
    CHECK_EQUAL(count_named(&w, "scxml.macrostep bootstrap"), 150);
    CHECK_EQUAL(count_named(&w, "scxml.microstep (eventless)"), 150);
    world_close(&w);
}

/* ---- synthetic records: drops, and fixed bytes ---------------------------------------------- */

typedef struct synthetic {
    ta_journal journal;
    uint64_t storage[4096];
    void *image_bytes;
    ta_image image;
    ta_otlp *otlp;
    void *memory;
} synthetic;

static void synthetic_open(synthetic *t, uint32_t open_spans) {
    memset(t, 0, sizeof *t);
    ta_journal_init(&t->journal, t->storage, sizeof t->storage);
    const char *charts[] = {SC("c", "initial='a'><state id='a'><transition event='e' target='b'/>"
                                    "</state><state id='b'/>")};
    size_t length;
    t->image_bytes = compile_charts(charts, 1, &length);
    CHECK(t->image_bytes && ta_image_open(t->image_bytes, length, &t->image) == TA_OK);
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.open_spans = open_spans;
    config.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    size_t bytes;
    CHECK_EQUAL(ta_otlp_layout(&config, &bytes), TA_OK);
    t->memory = malloc(bytes);
    CHECK_EQUAL(ta_otlp_init(t->memory, bytes, &config, &t->otlp), TA_OK);
    ta_otlp_mark(t->otlp, 0, 1000000000ull);
    ta_otlp_mark(t->otlp, 1000, 1000001000ull); /* a nanosecond a unit of fuel */
}

static ta_trace_record *synthetic_record(synthetic *t, uint32_t kind, uint32_t extra,
                                         uint64_t session, uint64_t macrostep, uint32_t microstep,
                                         uint64_t fuel) {
    uint32_t size = (uint32_t)sizeof(ta_trace_record) + ta_trace_body_size(kind) + 4u * extra;
    ta_trace_record *record = ta_trace_record_reserve(&t->journal, kind, size, fuel, 7);
    memset(ta_trace_record_body(record), 0, size - sizeof *record);
    record->actor = (1u << 24) | (uint32_t)session;
    record->chart = 0;
    record->key = (ta_step_key){session, macrostep, microstep, 0};
    record->trace = (ta_trace_id){0x1122334455667788ull, 0x99aabbccddeeff00ull};
    record->flags = TA_TRACE_FLAG_SAMPLED;
    record->span = ta_step_key_span_id(record->key);
    record->parent = microstep ? ta_step_key_span_id((ta_step_key){session, macrostep, 0, 0}) : 0;
    record->fuel = fuel;
    record->now = 7;
    return record;
}

static void synthetic_begin(synthetic *t, uint64_t session, uint64_t macrostep, uint64_t fuel) {
    ta_trace_record *record =
        synthetic_record(t, TA_TRACE_MACROSTEP_BEGIN, 0, session, macrostep, 0, fuel);
    ta_trace_macrostep_begin *begin = ta_trace_record_body(record);
    begin->name = TA_NIL; /* the bootstrap */
    begin->sendid = begin->origin = begin->datamodel = begin->actor_name = TA_NIL;
    ta_trace_record_commit(&t->journal, record);
}

static void synthetic_microstep(synthetic *t, uint64_t session, uint64_t macrostep,
                                uint32_t microstep, uint64_t fuel) {
    ta_trace_record *record =
        synthetic_record(t, TA_TRACE_MICROSTEP, 2, session, macrostep, microstep, fuel);
    ta_trace_microstep *micro = ta_trace_record_body(record);
    micro->begin_fuel = fuel - 1;
    micro->transition_count = 1;
    micro->entered_count = 1;
    micro->name = TA_NIL;
    uint32_t *list = (uint32_t *)(micro + 1);
    list[0] = 0; /* a -> b on e */
    list[1] = 2; /* b */
    ta_trace_record_commit(&t->journal, record);
}

static void synthetic_end(synthetic *t, uint64_t session, uint64_t macrostep, uint64_t fuel) {
    ta_trace_record *record =
        synthetic_record(t, TA_TRACE_MACROSTEP_END, 1, session, macrostep, 0, fuel);
    ((ta_trace_macrostep_end *)ta_trace_record_body(record))->state_count = 1;
    ((uint32_t *)((ta_trace_macrostep_end *)ta_trace_record_body(record) + 1))[0] = 2;
    ta_trace_record_commit(&t->journal, record);
}

static void synthetic_read(synthetic *t, world *w) {
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&t->journal, &bytes), cursor = 0;
    ta_otlp_input input;
    memset(&input, 0, sizeof input);
    input.size = sizeof input;
    input.view = (ta_trace_view){bytes, length, 0, NULL};
    input.image = &t->image;
    CHECK_EQUAL(ta_otlp_read(t->otlp, &input, &cursor), TA_OTLP_READ);
    CHECK_EQUAL(cursor, length);
    ta_journal_release(&t->journal, length);
    w->otlp = t->otlp;
    world_flush(w);
}

static void synthetic_close(synthetic *t, world *w) {
    for (uint32_t i = 0; i < w->request_count; i++)
        free(w->requests[i]);
    for (uint32_t i = 0; i < w->log_count; i++)
        free(w->logs[i]);
    free(t->memory);
    free(t->image_bytes);
}

/* A drop ends the open spans as errors marked with the count; the rest of
 * their macrosteps is skipped. */
static void test_drop_ends_spans(void) {
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 4);
    synthetic_begin(&t, 1, 1, 10);
    synthetic_begin(&t, 2, 1, 11);
    synthetic_microstep(&t, 2, 1, 1, 12);
    /* a microstep of session 2 opened from its events, then lost */
    ta_trace_record *raised = synthetic_record(&t, TA_TRACE_RAISED, 0, 2, 1, 2, 13);
    ((ta_trace_raised *)ta_trace_record_body(raised))->name = TA_NIL;
    ta_trace_record_commit(&t.journal, raised);
    ta_trace_record *dropped = ta_trace_record_reserve(
        &t.journal, TA_TRACE_DROPPED, sizeof(ta_trace_record) + sizeof(ta_trace_dropped), 20, 7);
    memset(ta_trace_record_body(dropped), 0, sizeof(ta_trace_dropped));
    dropped->fuel = 20;
    ((ta_trace_dropped *)ta_trace_record_body(dropped))->count = 5;
    ta_trace_record_commit(&t.journal, dropped);
    synthetic_end(&t, 1, 1, 30);          /* skipped: its macrostep ended with the drop */
    synthetic_microstep(&t, 2, 1, 3, 31); /* skipped */
    synthetic_begin(&t, 1, 2, 40);        /* a new macrostep: read again */
    synthetic_end(&t, 1, 2, 41);
    synthetic_read(&t, &w);
    ta_otlp_stats stats;
    stats.size = sizeof stats;
    ta_otlp_stats_get(t.otlp, &stats);
    CHECK_EQUAL(stats.dropped, 5);
    CHECK_EQUAL(stats.skipped, 2);
    CHECK_EQUAL(stats.open, 0);
    uint32_t errors = 0, marked = 0;
    for (uint32_t i = 0; i < w.span_count; i++) {
        const ta_otlp_span *span = &w.spans[i];
        if (span->status == TA_OTLP_STATUS_ERROR) errors++;
        if (int_of(span->attributes, "tinyactors.telemetry.dropped") == 5) marked++;
    }
    CHECK_EQUAL(errors, 3); /* session 1's macrostep, session 2's macrostep and microstep 2 */
    CHECK_EQUAL(marked, 3);
    CHECK_EQUAL(w.span_count, 5); /* those, session 2's microstep 1, and session 1's next */
    const ta_otlp_span *next = span_by_id(&w, span_of(1, 2, 0));
    CHECK(next && next->status == TA_OTLP_STATUS_UNSET);
    if (next) {
        CHECK_EQUAL(next->start, 1000000040ull); /* fuel 40, a nanosecond each, from 1 s */
        CHECK_EQUAL(next->end, 1000000041ull);
    }
    synthetic_close(&t, &w);
}

/* The bytes of a small request are fixed: any change to them is a change
 * of what every OTLP receiver reads. */
static void test_fixed_bytes(void) {
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 4);
    ta_otlp_attribute service = {{"service.name", 12}, {"fixed", 5}};
    CHECK_EQUAL(ta_otlp_set_resource(t.otlp, &service, 1), TA_OK);
    synthetic_begin(&t, 1, 1, 10);
    synthetic_microstep(&t, 1, 1, 1, 12);
    synthetic_end(&t, 1, 1, 14);
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&t.journal, &bytes), cursor = 0;
    ta_otlp_input input;
    memset(&input, 0, sizeof input);
    input.size = sizeof input;
    input.view = (ta_trace_view){bytes, length, 0, NULL};
    input.image = &t.image;
    CHECK_EQUAL(ta_otlp_read(t.otlp, &input, &cursor), TA_OTLP_READ);
    size_t size = ta_otlp_traces_size(t.otlp);
    unsigned char *request = malloc(size);
    CHECK_EQUAL(ta_otlp_put_traces(t.otlp, request, size), size);
    uint64_t hash = 0xcbf29ce484222325ull; /* FNV-1a of the request */
    for (size_t i = 0; i < size; i++)
        hash = (hash ^ request[i]) * 0x100000001b3ull;
    if (hash != 0x5fd589c3f7712327ull)
        fprintf(stderr, "  request: %zu bytes, FNV-1a %016llx\n", size, (unsigned long long)hash);
    CHECK_EQUAL(size, 984);
    CHECK_EQUAL(hash, 0x5fd589c3f7712327ull);
    free(request);
    w.otlp = t.otlp;
    synthetic_close(&t, &w);
}

/* Two sessions open at once in a table of one: the encoder asks to grow,
 * and goes on where it stopped once relocated; nothing is read twice. */
static void test_grow(void) {
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 1);
    synthetic_begin(&t, 1, 1, 10);
    synthetic_begin(&t, 2, 1, 11);
    synthetic_end(&t, 2, 1, 12);
    synthetic_end(&t, 1, 1, 13);
    const unsigned char *bytes;
    uint32_t length = ta_journal_peek(&t.journal, &bytes), cursor = 0;
    ta_otlp_input input;
    memset(&input, 0, sizeof input);
    input.size = sizeof input;
    input.view = (ta_trace_view){bytes, length, 0, NULL};
    input.image = &t.image;
    CHECK_EQUAL(ta_otlp_read(t.otlp, &input, &cursor), TA_OTLP_GROW);
    uint32_t stopped = cursor;
    CHECK(stopped > 0 && stopped < length); /* at the second beginning */
    ta_otlp_config bigger;
    memset(&bigger, 0, sizeof bigger);
    bigger.size = sizeof bigger;
    bigger.open_spans = 2;
    bigger.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    static uint64_t small[2];
    ta_otlp *refused;
    CHECK_EQUAL(ta_otlp_relocate(t.otlp, small, sizeof small, &bigger, &refused),
                TA_ERROR_NO_MEMORY);
    size_t need;
    CHECK_EQUAL(ta_otlp_layout(&bigger, &need), TA_OK);
    void *memory = malloc(need);
    ta_otlp *moved;
    CHECK_EQUAL(ta_otlp_relocate(t.otlp, memory, need, &bigger, &moved), TA_OK);
    free(t.memory);
    t.memory = memory;
    t.otlp = moved;
    CHECK_EQUAL(ta_otlp_read(t.otlp, &input, &cursor), TA_OTLP_READ);
    CHECK_EQUAL(cursor, length);
    w.otlp = t.otlp;
    world_flush(&w);
    CHECK_EQUAL(w.span_count, 2);
    CHECK(span_by_id(&w, span_of(1, 1, 0)) && span_by_id(&w, span_of(2, 1, 0)));
    synthetic_close(&t, &w);
}

/* A <log> is a span event and a log record; a failed action marks its
 * microstep as an error, with an event and a WARN log record. Values are
 * cut at value_limit. */
static void test_logs_failures_values(void) {
    const char *charts[] = {SC("noisy",
                               "initial='a'><datamodel><data id='x' expr='0'/></datamodel>"
                               "<state id='a'><transition event='go' target='b'>"
                               "<log label='note' expr=\"'hello'\"/>"
                               "<assign location='x' expr='undefined.y'/></transition></state>"
                               "<state id='b'/>")};
    world w;
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.flags = TA_OTLP_VALUES;
    config.value_limit = 16;
    CHECK(world_open(&w, charts, 1, &config));
    uint32_t a = world_spawn(&w, "noisy", NULL);
    uint64_t s = session_of(&w, a);
    world_settle(&w);
    world_send(&w, a, "go", "{\"text\": \"a value longer than sixteen bytes\"}", NULL);
    world_settle(&w);
    world_flush(&w);
    const ta_otlp_span *go = span_named(&w, "scxml.macrostep go", s),
                       *micro = span_named(&w, "scxml.microstep go", s);
    CHECK(go && micro);
    if (go && micro) {
        const char *data = text_of(go->attributes, "scxml.event.data");
        CHECK(strlen(data) == 16 && !strncmp(data, "{\"text\":\"a value", 16));
        ta_otlp_value cut;
        CHECK(attribute(go->attributes, "scxml.event.data.truncated", &cut) && cut.integer == 1);
        CHECK_EQUAL(micro->status, TA_OTLP_STATUS_ERROR);
        ta_otlp_event event;
        CHECK(event_named(micro, "scxml.log", &event));
        CHECK(!strcmp(text_of(event.attributes, "scxml.log.label"), "note"));
        CHECK(!strcmp(text_of(event.attributes, "scxml.log.text"), "hello"));
        CHECK(event_named(micro, "scxml.action_failed", &event));
    }
    CHECK(w.log_count > 0);
    int hello = 0, failed = 0;
    for (uint32_t i = 0; i < w.log_count; i++)
        for (size_t k = 0; k + 24 <= w.log_lengths[i]; k++) {
            hello |= !memcmp(w.logs[i] + k, "hello", 5);
            failed |= !memcmp(w.logs[i] + k, "tinyactors.action.failed", 24);
        }
    CHECK(hello && failed);
    world_close(&w);
}

/* Spans still open when the encoder is told to end them: marked incomplete. */
static void test_end_all(void) {
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 4);
    synthetic_begin(&t, 1, 1, 10);
    ta_trace_record *raised = synthetic_record(&t, TA_TRACE_RAISED, 0, 1, 1, 1, 11);
    ((ta_trace_raised *)ta_trace_record_body(raised))->name = TA_NIL;
    ta_trace_record_commit(&t.journal, raised);
    synthetic_read(&t, &w);
    CHECK_EQUAL(w.span_count, 0);
    ta_otlp_end_all(t.otlp, 1000000500ull);
    world_flush(&w);
    CHECK_EQUAL(w.span_count, 2);
    for (uint32_t i = 0; i < w.span_count; i++) {
        ta_otlp_value incomplete;
        CHECK(attribute(w.spans[i].attributes, "scxml.incomplete", &incomplete));
        CHECK_EQUAL(w.spans[i].end, 1000000500ull);
    }
    synthetic_close(&t, &w);
}

/* Fuel maps to wall time within the run that holds it; between runs the
 * time stands; a stamp at a run's start is that run's. */
static void test_time(void) {
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 4);                     /* marks: fuel 0 at 1 s, 1000 at 1 s + 1000 ns */
    ta_otlp_mark(t.otlp, 1000, 2000000000ull); /* idle until 2 s */
    ta_otlp_mark(t.otlp, 1100, 2000001000ull); /* 100 fuel in 1000 ns */
    synthetic_begin(&t, 1, 1, 500);            /* the first run: 1 s + 500 ns */
    synthetic_end(&t, 1, 1, 1000);             /* its last record */
    synthetic_begin(&t, 2, 1, 1001); /* the second run's first (a run starts the clock on) */
    synthetic_end(&t, 2, 1, 1050);   /* halfway through it */
    synthetic_begin(&t, 3, 1, 5000); /* past the last mark: the last mark's time */
    synthetic_end(&t, 3, 1, 5000);
    synthetic_read(&t, &w);
    const ta_otlp_span *one = span_by_id(&w, span_of(1, 1, 0)),
                       *two = span_by_id(&w, span_of(2, 1, 0)),
                       *three = span_by_id(&w, span_of(3, 1, 0));
    CHECK(one && two && three);
    if (one && two && three) {
        CHECK_EQUAL(one->start, 1000000500ull);
        CHECK_EQUAL(one->end, 1000001000ull);
        CHECK_EQUAL(two->start, 2000000010ull);
        CHECK_EQUAL(two->end, 2000000500ull);
        CHECK_EQUAL(three->start, 2000001000ull);
        CHECK_EQUAL(three->end, 2000001001ull); /* at least a nanosecond */
    }
    synthetic_close(&t, &w);
}

/* The decoder refuses what is not a request, and never reads past it. */
static void test_decode_malformed(void) {
    unsigned char garbage[256];
    uint32_t seed = 7;
    for (int round = 0; round < 2000; round++) {
        size_t length = (size_t)(seed % sizeof garbage);
        for (size_t i = 0; i < length; i++) {
            seed = seed * 1103515245u + 12345u;
            garbage[i] = (unsigned char)(seed >> 16);
        }
        unsigned char *copy = malloc(length ? length : 1); /* ASan sees reads past it */
        memcpy(copy, garbage, length);
        ta_otlp_cursor cursor;
        memset(&cursor, 0, sizeof cursor);
        ta_otlp_span span;
        for (int k = 0; k < 64 && ta_otlp_next_span(copy, length, &cursor, &span) == 1; k++) {
            uint32_t at = 0;
            ta_string_view key;
            ta_otlp_value value;
            while (ta_otlp_next_attribute(span.attributes, &at, &key, &value) == 1)
                ;
            ta_otlp_event event;
            at = 0;
            while (ta_otlp_next_event(span.events, &at, &event) == 1)
                ;
        }
        free(copy);
    }
    /* a request cut short is refused */
    synthetic t;
    world w;
    memset(&w, 0, sizeof w);
    synthetic_open(&t, 4);
    synthetic_begin(&t, 1, 1, 10);
    synthetic_end(&t, 1, 1, 11);
    synthetic_read(&t, &w);
    CHECK_EQUAL(w.request_count, 1);
    ta_otlp_cursor cursor;
    memset(&cursor, 0, sizeof cursor);
    ta_otlp_span span;
    CHECK_EQUAL(ta_otlp_next_span(w.requests[0], w.request_lengths[0] - 3, &cursor, &span),
                TA_ERROR_INVALID);
    synthetic_close(&t, &w);
}

int main(void) {
    RUN(test_granary_vocabulary);
    RUN(test_open_across_runs);
    RUN(test_flush_and_grow);
    RUN(test_drop_ends_spans);
    RUN(test_grow);
    RUN(test_fixed_bytes);
    RUN(test_logs_failures_values);
    RUN(test_end_all);
    RUN(test_time);
    RUN(test_decode_malformed);
    TEST_MAIN_END();
}
