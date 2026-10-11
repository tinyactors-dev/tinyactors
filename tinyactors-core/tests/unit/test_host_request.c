/* ta_host_request (docs/requests.md; docs/adr/0009 in the repository root,
 * decision 2): every method's reply, every way a request is refused, and
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

#define RAW(w, literal) raw(w, literal, sizeof literal - 1)
#define RESULT(value) "{\"result\":" value "}"
#define ERROR(code, reason, path, message)                                                         \
    "{\"error\":{\"code\":" code ",\"message\":\"" message "\",\"data\":{\"reason\":\"" reason      \
    "\",\"path\":\"" path "\"}}}"

static void test_refused_requests(void) {
    world w;
    world_open(&w, 0);
    CHECK(reply_is(RAW(&w, "\xff"), ERROR("-32700", "malformed", "",
                                         "the request is not one well-formed CBOR item")));
    CHECK(reply_is(RAW(&w, "\x01"), ERROR("-32600", "type", "", "a request is a map")));
    CHECK(reply_is(RAW(&w, "\xa0"),
                   ERROR("-32600", "missing", "method", "method names what the request does")));
    CHECK(reply_is(RAW(&w, "\xa1\x66method\x01"), ERROR("-32600", "type", "method", "method is text")));
    CHECK(reply_is(RAW(&w, "\xa1\x66method\x64nope"),
                   ERROR("-32601", "unknown", "method", "no such method")));
    /* a member the request does not have, params that are not a map, an id that is neither */
    CHECK(reply_is(RAW(&w, "\xa2\x66method\x65stats\x61x\x01"),
                   ERROR("-32600", "unknown", "x", "a request has only method, params and id")));
    CHECK(reply_is(RAW(&w, "\xa2\x66method\x65stats\x66params\x01"),
                   ERROR("-32600", "type", "params", "params is a map")));
    CHECK(reply_is(RAW(&w, "\xa2\x66method\x65stats\x62id\x80"),
                   ERROR("-32600", "type", "id", "id is an unsigned integer or text")));
    CHECK(reply_is(RAW(&w, "\xa2\x66method\x65stats\x66params\xa1\x61x\x01"),
                   ERROR("-32602", "unknown", "params.x", "params has a field this method does not take")));
    /* the id comes back first, as it was: a number or text */
    CHECK(strncmp(RAW(&w, "\xa2\x66method\x65stats\x62id\x07"), "{\"id\":7,\"result\":{\"host\":", 25) == 0);
    CHECK(reply_is(RAW(&w, "\xa2\x66method\x64nope\x62id\x61" "a"),
                   "{\"id\":\"a\",\"error\":{\"code\":-32601,\"message\":\"no such method\","
                   "\"data\":{\"reason\":\"unknown\",\"path\":\"method\"}}}"));
    /* more items than the tokens on the stack hold */
    unsigned char big[32 + 300];
    size_t n = sizeof "\xa2\x66method\x65stats\x66params\xa1\x61x\x99\x01\x2c" - 1;
    memcpy(big, "\xa2\x66method\x65stats\x66params\xa1\x61x\x99\x01\x2c", n); /* "x": [300 items] */
    for (int i = 0; i < 300; i++)
        big[n++] = 0x01;
    CHECK(reply_is(raw(&w, big, n), ERROR("-32003", "limit", "", "a request has at most 256 items")));
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
    CHECK(reply_is(a.text, RESULT("{\"sessions\":[1],\"cursor\":null}")));
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
    CHECK(reply_is(a.text, ERROR("-32602", "range", "params.limit", "limit is between 1 and 512")));
    /* actor and state */
    writer = ask_begin(&a, "actor", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"session\":1,\"segment\":0,\"chart\":\"pinged\",\"parent\":null,"
                                  "\"mailbox\":0,\"invocations\":0}")));
    writer = ask_begin(&a, "actor", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, 99);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32001", "stale", "params.session", "no such session in this host")));
    ask_begin(&a, "state", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "missing", "params.session", "session is the actor's session id")));
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
    CHECK(reply_is(a.text, RESULT("{}")));
    writer = ask_begin(&a, "trace.configure", 1);
    ta_cbor_write_text(writer, ta_string("detail"));
    ta_cbor_write_text(writer, ta_string("loud"));
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "unknown", "params.detail", "detail is one of summary, decisions, full, off")));
    writer = ask_begin(&a, "trace.configure", 1);
    ta_cbor_write_text(writer, ta_string("sample"));
    ta_cbor_write_uint(writer, 2);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "range", "params.sample", "a share is between 0 and 1")));
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
    CHECK(reply_is(a.text, ERROR("-32602", "unknown", "params.rules[1].from",
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
    unsigned char small[4];
    ta_buffer out = {small, sizeof small, 0};
    CHECK_EQUAL(ta_host_request(w.host, (ta_bytes){a.request, request.length}, &out),
                TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 13u); /* {"result": {"id": 1}} */
    CHECK(reply_is(raw(&w, a.request, request.length), RESULT("{\"id\":1}")));
    CHECK(reply_is(raw(&w, a.request, request.length), RESULT("{\"id\":2}")));
    writer = ask_begin(&a, "trace.watch", 0);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "missing", "params.rules", "a watch has at least one rule")));
    writer = ask_begin(&a, "trace.unwatch", 1);
    ta_cbor_write_text(writer, ta_string("id"));
    ta_cbor_write_uint(writer, 1);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{}")));
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32001", "stale", "params.id", "no such watch: it ended or never was")));
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
    /* the request, built by hand: {"method": "trace.spans.decode", "params": {"traces": bytes}} */
#define DECODE "\xa2\x66method\x72trace.spans.decode\x66params\xa1\x66traces"
    static unsigned char request[(1 << 16) + 64];
    size_t n = sizeof DECODE - 1;
    memcpy(request, DECODE "\x59", n + 1);
    n++;
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
    uint32_t result = ta_cbor_member(&document, 0, ta_string("result"));
    uint32_t spans = ta_cbor_member(&document, result, ta_string("spans"));
    CHECK(spans != TA_NIL && tokens[spans].kind == TA_CBOR_ARRAY && tokens[spans].count >= 1);
    uint32_t name = ta_cbor_member(&document, spans + 1, ta_string("name"));
    CHECK(name != TA_NIL && tokens[name].kind == TA_CBOR_TEXT);
    uint32_t trace = ta_cbor_member(&document, spans + 1, ta_string("trace"));
    CHECK(trace != TA_NIL && tokens[trace].count == 32);
    /* not OTLP */
    CHECK(reply_is(RAW(&w, DECODE "\x41\xff"),
                   ERROR("-32006", "malformed", "params.traces", "traces is not an ExportTraceServiceRequest")));
    CHECK(reply_is(RAW(&w, "\xa1\x66method\x72trace.spans.decode"),
                   ERROR("-32602", "missing", "params.traces",
                         "traces is the bytes of an ExportTraceServiceRequest")));
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
