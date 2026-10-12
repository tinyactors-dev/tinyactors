/* The host's OTLP encoder (the config's "telemetry", docs/host-config.md; docs/adr/0009 in the
 * repository root, decision 6): records are read inside ta_host_run, a
 * TA_HOST_EFFECT_TRACES effect says a request is ready, and ta_host_traces
 * writes it into the caller's buffer. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"

/* Every `ping` is a macrostep with a log record. */
#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinged' "         \
    "initial='s'><datamodel><data id='n' expr='0'/></datamodel>"                                   \
    "<state id='s'><transition event='ping'><assign location='n' expr='n + 1'/>"                   \
    "<log expr=\"'ping ' + n\"/></transition></state></scxml>"

static void *compile_pinged(size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"pinged.scxml", 12}, {CHART, sizeof CHART - 1}};
    ta_compile_result *result = NULL;
    void *copy = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &result) == TA_OK) {
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

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_vtable vtable; /* the ECMAScript one, with `assign` wrapped */
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    ta_image image;
    uint32_t segment;
    uint64_t session;
    ta_status during_run; /* what ta_host_traces answered from inside a run */
    int probe;            /* call ta_host_traces from the next assign */
    uint32_t traces, logs; /* TA_HOST_EFFECT_TRACES / _LOGS seen */
} world;

static world *current;

static ta_status assign(const ta_datamodel_call *call, ta_expression location,
                        ta_expression value) {
    if (current->probe) {
        current->probe = 0;
        unsigned char bytes[64];
        ta_buffer out = {bytes, sizeof bytes, 0};
        current->during_run = ta_host_traces(current->host, &out);
    }
    return ta_datamodel_ecmascript_vtable()->assign(call, location, value);
}

/* telemetry: NULL for a host without an encoder */
static void world_open(world *w, const test_host_telemetry *telemetry) {
    memset(w, 0, sizeof *w);
    current = w;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    w->vtable = *w->binding.vtable;
    w->vtable.assign = assign;
    w->binding.vtable = &w->vtable;
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    if (telemetry) config.telemetry = *telemetry;
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_pinged(&length);
    CHECK(w->image_bytes != NULL);
    CHECK_EQUAL(ta_image_open((ta_bytes){w->image_bytes, length}, &w->image), TA_OK);
    CHECK_EQUAL(ta_host_add_image(w->host, &w->image, &w->segment), TA_OK);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    spawn.chart = 0;
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
    current = NULL;
}

static ta_run run_once(world *w) {
    ta_run result = ta_host_run(w->host, 1000000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w->host, &effects);
    for (uint32_t i = 0; i < n; i++) {
        w->traces += effects[i].kind == TA_HOST_EFFECT_TRACES;
        w->logs += effects[i].kind == TA_HOST_EFFECT_LOGS;
    }
    ta_host_effects_ack(w->host);
    return result;
}

static void ping(world *w, uint32_t count) {
    for (uint32_t i = 0; i < count; i++) {
        ta_event_in event;
        memset(&event, 0, sizeof event);
        event.session = w->session;
        event.name = (ta_string_view){"ping", 4};
        CHECK_EQUAL(ta_host_send(w->host, &event), TA_OK);
    }
}

static void advance(world *w, ta_time monotonic) {
    ta_host_advance_time(w->host, (ta_host_time){monotonic, 1700000000000000000ull + monotonic});
}

static uint32_t span_count(const void *bytes, size_t length) {
    ta_otlp_cursor cursor;
    memset(&cursor, 0, sizeof cursor);
    ta_otlp_span span;
    uint32_t n = 0;
    while (ta_otlp_next_span((ta_bytes){bytes, length}, &cursor, &span) > 0)
        n++;
    return n;
}

static test_host_telemetry encoder(ta_time flush_interval) {
    test_host_telemetry telemetry;
    memset(&telemetry, 0, sizeof telemetry);
    telemetry.otlp.size = sizeof telemetry.otlp;
    telemetry.flush_interval = flush_interval;
    return telemetry;
}

/* Records are read inside the run; ta_host_traces takes the spans once. */
static void test_drain_and_take(void) {
    test_host_telemetry telemetry = encoder(0);
    world w;
    world_open(&w, &telemetry);
    advance(&w, 10);
    run_once(&w);
    ping(&w, 3);
    run_once(&w);
    advance(&w, 20);
    run_once(&w); /* reads the pings' records */
    CHECK_EQUAL(w.traces, 0u); /* no interval, staging not full */
    static unsigned char bytes[1 << 16];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK(span_count(bytes, out.length) >= 3);
    out = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK_EQUAL(out.length, 0u); /* taken */
    out = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_OK);
    CHECK(out.length > 0); /* the <log>s */
    world_close(&w);
}

/* TA_ERROR_NO_MEMORY says what the request needs and leaves the spans. */
static void test_take_sizing(void) {
    test_host_telemetry telemetry = encoder(0);
    world w;
    world_open(&w, &telemetry);
    advance(&w, 10);
    ping(&w, 2);
    run_once(&w);
    advance(&w, 20);
    unsigned char small[8];
    ta_buffer out = {small, sizeof small, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_ERROR_NO_MEMORY);
    size_t need = out.length;
    CHECK(need > sizeof small);
    out = (ta_buffer){NULL, 0, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, need); /* nothing changed */
    unsigned char *bytes = malloc(need);
    out = (ta_buffer){bytes, need, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK_EQUAL(out.length, need);
    CHECK(span_count(bytes, out.length) >= 2);
    free(bytes);
    world_close(&w);
}

/* The flush interval is a deadline of the run; when it passes, one effect. */
static void test_flush_deadline(void) {
    test_host_telemetry telemetry = encoder(100);
    world w;
    world_open(&w, &telemetry);
    advance(&w, 10);
    run_once(&w);
    ping(&w, 1);
    ta_run result = run_once(&w); /* writes records: staged from now (10) */
    CHECK_EQUAL(result.next_deadline, (ta_time)110);
    CHECK_EQUAL(w.traces, 0u);
    advance(&w, 60);
    run_once(&w); /* read, not due yet */
    CHECK_EQUAL(w.traces, 0u);
    advance(&w, 110);
    result = run_once(&w);
    CHECK_EQUAL(w.traces, 1u);
    CHECK_EQUAL(w.logs, 1u);
    ping(&w, 1);
    run_once(&w);
    advance(&w, 300);
    run_once(&w);
    CHECK_EQUAL(w.traces, 1u); /* one outstanding until taken */
    static unsigned char bytes[1 << 16];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK(out.length > 0);
    out = (ta_buffer){bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_OK);
    result = run_once(&w);
    CHECK_EQUAL(result.next_deadline, UINT64_MAX); /* nothing staged */
    ping(&w, 1);
    result = run_once(&w); /* staged from now (300) */
    CHECK_EQUAL(result.next_deadline, (ta_time)400);
    advance(&w, 350);
    run_once(&w);
    CHECK_EQUAL(w.traces, 1u);
    advance(&w, 400);
    run_once(&w);
    CHECK_EQUAL(w.traces, 2u); /* taken, so the next one comes */
    world_close(&w);
}

/* A full staging is a TRACES effect without an interval. */
static void test_staging_full(void) {
    test_host_telemetry telemetry = encoder(0);
    telemetry.otlp.staging_bytes = TA_OTLP_STAGING_BYTES_MIN;
    telemetry.otlp.flags = TA_OTLP_NO_LOGS;
    world w;
    world_open(&w, &telemetry);
    ta_time now = 10;
    for (int round = 0; round < 200 && !w.traces; round++) {
        advance(&w, now++);
        ping(&w, 20);
        run_once(&w);
    }
    CHECK_EQUAL(w.traces, 1u);
    CHECK_EQUAL(w.logs, 0u);
    static unsigned char bytes[TA_OTLP_STAGING_BYTES_MIN * 2];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK(out.length > TA_OTLP_STAGING_BYTES_MIN / 2);
    world_close(&w);
}

/* The bytes are what a caller reading the journals into its own encoder,
 * with the same marks, would write. */
static void test_same_bytes(void) {
    test_host_telemetry telemetry = encoder(0);
    world a, b;
    world_open(&b, NULL);
    world_open(&a, &telemetry);
    ta_otlp_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    size_t length = 0;
    CHECK_EQUAL(ta_otlp_layout(&config, &length), TA_OK);
    void *memory = malloc(length);
    ta_otlp *otlp = NULL;
    CHECK_EQUAL(ta_otlp_init((ta_memory){memory, length}, &config, &otlp), TA_OK);
    world *both[2] = {&a, &b};
    for (ta_time t = 10; t <= 40; t += 10) {
        for (int k = 0; k < 2; k++) {
            current = both[k];
            advance(both[k], t);
            if (k == 1) {
                ta_host_stats stats;
                stats.size = sizeof stats;
                host_stats_fill(b.host, &stats);
                ta_otlp_mark(otlp, stats.fuel_clock, 1700000000000000000ull + t);
                ta_system *system = ta_host_system(b.host, b.segment);
                ta_otlp_input input;
                memset(&input, 0, sizeof input);
                input.size = sizeof input;
                input.source = b.segment;
                input.image = &b.image;
                input.datamodels = &b.binding;
                input.datamodel_count = 1;
                while (ta_system_trace(system, &input.view)) {
                    uint32_t cursor = 0;
                    CHECK_EQUAL(ta_otlp_read(otlp, &input, &cursor), TA_OTLP_READ);
                    ta_system_trace_ack(system, input.view.length);
                }
            }
            ping(both[k], 2);
            run_once(both[k]);
        }
    }
    current = &a;
    advance(&a, 50);
    static unsigned char ours[1 << 16], theirs[1 << 16];
    ta_buffer x = {ours, sizeof ours, 0}, y = {theirs, sizeof theirs, 0};
    CHECK_EQUAL(ta_host_traces(a.host, &x), TA_OK);
    current = &b;
    advance(&b, 50);
    {
        ta_host_stats stats;
        stats.size = sizeof stats;
        host_stats_fill(b.host, &stats);
        ta_otlp_mark(otlp, stats.fuel_clock, 1700000000000000000ull + 50);
        ta_system *system = ta_host_system(b.host, b.segment);
        ta_otlp_input input;
        memset(&input, 0, sizeof input);
        input.size = sizeof input;
        input.source = b.segment;
        input.image = &b.image;
        input.datamodels = &b.binding;
        input.datamodel_count = 1;
        while (ta_system_trace(system, &input.view)) {
            uint32_t cursor = 0;
            CHECK_EQUAL(ta_otlp_read(otlp, &input, &cursor), TA_OTLP_READ);
            ta_system_trace_ack(system, input.view.length);
        }
    }
    CHECK_EQUAL(ta_otlp_put_traces(otlp, &y), TA_OK);
    CHECK(x.length > 0);
    CHECK_EQUAL(x.length, y.length);
    CHECK(x.length == y.length && memcmp(ours, theirs, x.length) == 0);
    free(memory);
    current = &a;
    world_close(&a);
    current = &b;
    world_close(&b);
}

/* Refused inside a run; without an encoder, refused and the journals are the caller's. */
/* Spans an ended span count: those marked scxml.incomplete, and the end
 * time of the last of them. */
static uint32_t incomplete_spans(const void *bytes, size_t length, uint64_t *end) {
    ta_otlp_cursor cursor;
    memset(&cursor, 0, sizeof cursor);
    ta_otlp_span span;
    uint32_t n = 0;
    while (ta_otlp_next_span((ta_bytes){bytes, length}, &cursor, &span) > 0) {
        uint32_t at = 0;
        ta_otlp_key_value attribute;
        while (ta_otlp_next_attribute(span.attributes, &at, &attribute) > 0)
            if (attribute.key.length == 16 && !memcmp(attribute.key.pointer, "scxml.incomplete", 16)) {
                n++;
                *end = span.end;
            }
    }
    return n;
}

/* trace.end ends the open spans at the latest wall time, marked
 * scxml.incomplete; a TRACES effect follows and the take holds them. */
static void test_end_open_spans(void) {
    test_host_telemetry telemetry = encoder(0);
    world w;
    world_open(&w, &telemetry);
    advance(&w, 10);
    run_once(&w);
    ping(&w, 1);
    ta_host_run(w.host, 1); /* a budget too small to finish the macrostep */
    ta_host_effects_ack(w.host);
    advance(&w, 30);
    static host_ask a;
    ask_begin(&a, "trace.end", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(strcmp(a.text, "{\"id\":1,\"result\":{}}") == 0);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w.host, &effects), traces = 0;
    for (uint32_t i = 0; i < n; i++)
        traces += effects[i].kind == TA_HOST_EFFECT_TRACES;
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(traces, 1u); /* due at once */
    static unsigned char bytes[1 << 16];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    uint64_t end = 0;
    CHECK(incomplete_spans(bytes, out.length, &end) >= 1);
    CHECK_EQUAL(end, 1700000000000000030ull);
    world_close(&w);

    world_open(&w, NULL); /* the null encoder has no spans to end */
    ask_begin(&a, "trace.end", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(strstr(a.text, "\"reason\":\"state\"") != NULL);
    world_close(&w);
}

/* A host freed with spans open ends them and frees everything (ASan). */
static void test_free_with_open_spans(void) {
    test_host_telemetry telemetry = encoder(0);
    world w;
    world_open(&w, &telemetry);
    advance(&w, 10);
    run_once(&w);
    ping(&w, 1);
    ta_host_run(w.host, 1);
    ta_host_effects_ack(w.host);
    advance(&w, 20);
    run_once(&w);
    world_close(&w);
}

static void test_refusals(void) {
    test_host_telemetry telemetry = encoder(0);
    world w;
    world_open(&w, &telemetry);
    w.during_run = TA_OK;
    w.probe = 1;
    ping(&w, 1);
    run_once(&w);
    CHECK_EQUAL(w.during_run, TA_ERROR_STATE);
    world_close(&w);

    world_open(&w, NULL);
    unsigned char bytes[64];
    ta_buffer out = {bytes, sizeof bytes, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_logs(w.host, &out), TA_ERROR_STATE);
    ping(&w, 1);
    ta_run result = run_once(&w);
    CHECK_EQUAL(result.next_deadline, UINT64_MAX);
    ta_trace_view view;
    CHECK(ta_system_trace(ta_host_system(w.host, w.segment), &view) > 0); /* left for the caller */
    world_close(&w);
}

int main(void) {
    RUN(test_drain_and_take);
    RUN(test_take_sizing);
    RUN(test_flush_deadline);
    RUN(test_staging_full);
    RUN(test_same_bytes);
    RUN(test_end_open_spans);
    RUN(test_free_with_open_spans);
    RUN(test_refusals);
    TEST_MAIN_END();
}
