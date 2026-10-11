/* ta_host_request (docs/requests.md; docs/adr/0009 in the repository root,
 * decision 2): every operation's reply, every way a request is refused, and
 * TA_ERROR_NO_MEMORY leaving the host as it was. Also ta_host_metrics. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"

#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinged' "         \
    "initial='s'><state id='s'><transition event='ping' target='t'/></state>"                     \
    "<state id='t'/></scxml>"

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
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    ta_image image;
    uint32_t segment;
    uint64_t session;
} world;

/* encoder: give the host an OTLP encoder */
static void world_open(world *w, int encoder) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    ta_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    if (encoder) config.telemetry.otlp.size = sizeof config.telemetry.otlp;
    CHECK_EQUAL(ta_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_pinged(&length);
    CHECK(w->image_bytes != NULL);
    CHECK_EQUAL(ta_image_open((ta_bytes){w->image_bytes, length}, &w->image), TA_OK);
    CHECK_EQUAL(ta_host_add_image(w->host, &w->image, &w->segment), TA_OK);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
}

static void settle(world *w) {
    ta_run result;
    do {
        ta_host_advance_time(w->host, (ta_host_time){0, 1700000000000000000ull});
        result = ta_host_run(w->host, 1000000);
        const ta_host_effect *effects;
        ta_host_effects(w->host, &effects);
        ta_host_effects_ack(w->host);
    } while (result.status != TA_RUN_IDLE);
}

/* Send raw request bytes; the reply's diagnostic text into a static buffer. */
static const char *raw(world *w, const void *bytes, size_t length) {
    static unsigned char reply[4096];
    static char text[4096];
    ta_buffer out = {reply, sizeof reply, 0};
    text[0] = 0;
    if (ta_host_request(w->host, (ta_bytes){bytes, length}, &out) != TA_OK) return "(status)";
    ta_buffer diagnostic = {text, sizeof text - 1, 0};
    if (ta_cbor_diagnostic((ta_bytes){reply, out.length}, &diagnostic) != TA_OK) return "(cbor)";
    text[diagnostic.length] = 0;
    return text;
}

static int reply_is(const char *got, const char *expected) {
    if (strcmp(got, expected) != 0) fprintf(stderr, "  got:      %s\n  expected: %s\n", got, expected);
    return strcmp(got, expected) == 0;
}

#define ERROR(code, path, message)                                                                 \
    "{\"error\":{\"code\":\"" code "\",\"path\":\"" path "\",\"message\":\"" message "\"}}"

static void test_refused_requests(void) {
    world w;
    world_open(&w, 0);
    CHECK(reply_is(raw(&w, "\xff", 1),
                   ERROR("malformed", "", "the request is not one well-formed CBOR item")));
    CHECK(reply_is(raw(&w, "\x01", 1), ERROR("type", "", "a request is a map")));
    CHECK(reply_is(raw(&w, "\xa0", 1), ERROR("missing", "op", "op names the operation")));
    CHECK(reply_is(raw(&w, "\xa1\x62op\x01", 5), ERROR("type", "op", "expected text")));
    CHECK(reply_is(raw(&w, "\xa1\x62op\x64nope", 9), ERROR("unknown", "op", "no such operation")));
    CHECK(reply_is(raw(&w, "\xa2\x62op\x65stats\x61x\x01", 13),
                   ERROR("unknown", "x", "the request has a field this operation does not take")));
    /* more items than the tokens on the stack hold */
    unsigned char big[16 + 300];
    size_t n = 0;
    memcpy(big, "\xa2\x62op\x65stats\x61x\x99\x01\x2c", 15); /* "x": [300 items] */
    n = 15;
    for (int i = 0; i < 300; i++)
        big[n++] = 0x01;
    CHECK(reply_is(raw(&w, big, n), ERROR("limit", "", "a request has at most 256 items")));
    world_close(&w);
}

static void test_stats_actors_actor_state(void) {
    world w;
    world_open(&w, 0);
    settle(&w);
    static host_ask a;
    ask_begin(&a, "stats", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK_EQUAL(ask_uint(&a, ask_field(&a, 0, "host"), "segments"), 1u);
    CHECK_EQUAL(ask_uint(&a, ask_field(&a, 0, "host"), "actors"), 1u);
    CHECK(strstr(a.text, "\"charts\":[{\"name\":\"pinged\",\"actors\":1,\"hibernated\":0,"
                         "\"spawned\":1,\"ended\":0,") != NULL);
    /* actors: one page, then a second actor and pages of one */
    ta_cbor_writer *writer = ask_begin(&a, "actors", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, "{\"sessions\":[1],\"cursor\":null}"));
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w.segment;
    uint64_t second;
    CHECK_EQUAL(ta_host_spawn(w.host, &spawn, &second), TA_OK);
    settle(&w);
    uint64_t cursor = 0, sessions[1];
    uint32_t total = 0, got;
    while ((got = host_actors_page(w.host, &cursor, (ta_uint64_array){sessions, 1})) != 0)
        total += got;
    CHECK_EQUAL(total, 2u);
    writer = ask_begin(&a, "actors", 1);
    ta_cbor_write_text(writer, ta_string("limit"));
    ta_cbor_write_uint(writer, 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("range", "limit", "limit is between 1 and 512")));
    /* actor and state */
    writer = ask_begin(&a, "actor", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, "{\"session\":1,\"segment\":0,\"chart\":\"pinged\",\"parent\":null,"
                           "\"mailbox\":0,\"invocations\":0}"));
    writer = ask_begin(&a, "actor", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, 99);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("stale", "session", "no such session in this host")));
    ask_begin(&a, "state", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("missing", "session", "session is the actor's session id")));
    char value[64];
    CHECK_EQUAL(ask_state(w.host, w.session, value, sizeof value), TA_OK);
    CHECK(reply_is(value, "\"s\""));
    world_close(&w);
}

static void test_trace_configure_and_watch(void) {
    world w;
    world_open(&w, 0);
    static host_ask a;
    ta_cbor_writer *writer = ask_begin(&a, "trace.configure", 2);
    ta_cbor_write_text(writer, ta_string("detail"));
    ta_cbor_write_text(writer, ta_string("decisions"));
    ta_cbor_write_text(writer, ta_string("sample"));
    ta_cbor_write_float(writer, 0.25);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, "{}"));
    writer = ask_begin(&a, "trace.configure", 1);
    ta_cbor_write_text(writer, ta_string("detail"));
    ta_cbor_write_text(writer, ta_string("loud"));
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("unknown", "detail", "detail is one of summary, decisions, full, off")));
    writer = ask_begin(&a, "trace.configure", 1);
    ta_cbor_write_text(writer, ta_string("sample"));
    ta_cbor_write_uint(writer, 2);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("range", "sample", "a share is between 0 and 1")));
    writer = ask_begin(&a, "trace.configure", 1);
    ta_cbor_write_text(writer, ta_string("rules"));
    ta_cbor_write_array(writer, 2);
    ta_cbor_write_map(writer, 1);
    ta_cbor_write_text(writer, ta_string("chart"));
    ta_cbor_write_text(writer, ta_string("pinged"));
    ta_cbor_write_map(writer, 1);
    ta_cbor_write_text(writer, ta_string("from"));
    ta_cbor_write_array(writer, 1);
    ta_cbor_write_text(writer, ta_string("elsewhere"));
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("unknown", "rules[1].from",
                                 "from is a list of host, actor, timer, spawn, sampled, unsampled")));
    /* a watch: its reply does not fit, so it is not applied (the next one gets id 1) */
    writer = ask_begin(&a, "trace.watch", 1);
    ta_cbor_write_text(writer, ta_string("rules"));
    ta_cbor_write_array(writer, 1);
    ta_cbor_write_map(writer, 2);
    ta_cbor_write_text(writer, ta_string("chart"));
    ta_cbor_write_text(writer, ta_string("pinged"));
    ta_cbor_write_text(writer, ta_string("sample"));
    ta_cbor_write_text(writer, ta_string("sample"));
    ta_buffer request = {a.request, sizeof a.request, 0};
    CHECK_EQUAL(ta_cbor_finish(writer, &request), TA_OK);
    unsigned char small[2];
    ta_buffer out = {small, sizeof small, 0};
    CHECK_EQUAL(ta_host_request(w.host, (ta_bytes){a.request, request.length}, &out),
                TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 5u); /* {"id": 1} */
    CHECK(reply_is(raw(&w, a.request, request.length), "{\"id\":1}"));
    CHECK(reply_is(raw(&w, a.request, request.length), "{\"id\":2}"));
    writer = ask_begin(&a, "trace.watch", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("missing", "rules", "a watch has at least one rule")));
    writer = ask_begin(&a, "trace.unwatch", 1);
    ta_cbor_write_text(writer, ta_string("id"));
    ta_cbor_write_uint(writer, 1);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, "{}"));
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("stale", "id", "no such watch: it ended or never was")));
    world_close(&w);
}

static void test_spans_and_metrics(void) {
    world w;
    world_open(&w, 1);
    settle(&w);
    static unsigned char traces[1 << 16];
    ta_buffer out = {traces, sizeof traces, 0};
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK(out.length > 0);
    /* the request, built by hand: {"op": "spans.decode", "traces": bytes} */
    static unsigned char request[(1 << 16) + 64];
    size_t n = 0;
    memcpy(request, "\xa2\x62op\x6cspans.decode\x66traces\x59", 25);
    n = 25;
    request[n++] = (unsigned char)(out.length >> 8);
    request[n++] = (unsigned char)out.length;
    memcpy(request + n, traces, out.length);
    n += out.length;
    static unsigned char reply[1 << 16];
    ta_buffer answer = {reply, sizeof reply, 0};
    CHECK_EQUAL(ta_host_request(w.host, (ta_bytes){request, n}, &answer), TA_OK);
    static ta_cbor_token tokens[4096];
    ta_cbor_document document = {{reply, answer.length}, tokens, 4096, 0};
    CHECK_EQUAL(ta_cbor_decode(&document), TA_OK);
    uint32_t spans = ta_cbor_member(&document, 0, ta_string("spans"));
    CHECK(spans != TA_NIL && tokens[spans].kind == TA_CBOR_ARRAY && tokens[spans].count >= 1);
    uint32_t name = ta_cbor_member(&document, spans + 1, ta_string("name"));
    CHECK(name != TA_NIL && tokens[name].kind == TA_CBOR_TEXT);
    uint32_t trace = ta_cbor_member(&document, spans + 1, ta_string("trace"));
    CHECK(trace != TA_NIL && tokens[trace].count == 32);
    /* not OTLP */
    CHECK(reply_is(raw(&w, "\xa2\x62op\x6cspans.decode\x66traces\x41\xff", 26),
                   ERROR("malformed", "traces", "traces is not an ExportTraceServiceRequest")));
    CHECK(reply_is(raw(&w, "\xa1\x62op\x6cspans.decode", 17),
                   ERROR("missing", "traces", "traces is the bytes of an ExportTraceServiceRequest")));
    /* metrics on demand: measured, then written; the second call puts the same */
    ta_buffer metrics = {NULL, 0, 0};
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_ERROR_NO_MEMORY);
    size_t needed = metrics.length;
    CHECK(needed > 0);
    static unsigned char metric_bytes[1 << 16];
    metrics = (ta_buffer){metric_bytes, sizeof metric_bytes, 0};
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK);
    CHECK_EQUAL(metrics.length, needed);
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK); /* again: counted anew */
    CHECK(metrics.length > 0);
    world_close(&w);
    world_open(&w, 0);
    metrics = (ta_buffer){metric_bytes, sizeof metric_bytes, 0};
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_ERROR_STATE); /* no encoder */
    world_close(&w);
}

int main(void) {
    RUN(test_refused_requests);
    RUN(test_stats_actors_actor_state);
    RUN(test_trace_configure_and_watch);
    RUN(test_spans_and_metrics);
    TEST_MAIN_END();
}
