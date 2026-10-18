/* ta_host_post (docs/requests.md; docs/adr/0011 in the repository root),
 * beyond its protocol vectors (tinyactors-test/suites/protocol): what bytes
 * alone cannot show — spans of real traces, variables after a run,
 * hibernation, a datamodel without variables, actor.send's values read by
 * the target's datamodel, a config
 * nothing points into afterwards. Also ta_host_metrics. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"
#include "../support/host_output.h"

#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='pinged' "         \
    "initial='s'><datamodel><data id='n' expr='1'/><data id='who' expr=\"'bob'\"/>"              \
    "<data id='nothing'/></datamodel><state id='s'><transition event='set'>"                       \
    "<assign location='n' expr='_event.data.n'/><assign location='who' expr='_event.origin'/>"     \
    "</transition><transition event='ping' target='t'>"                                             \
    "<assign location='n' expr='n + 1'/></transition></state><state id='t'>"                      \
    "<transition event='ping' target='u'/></state><state id='u'/></scxml>"

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

/* The C library's allocator, refusing every block while `refusing` is set. */
static int refusing;
static void *refusing_alloc(void *context, size_t size, size_t align) {
    (void)context;
    if (refusing) return NULL;
    ta_allocator heap = ta_runtime_malloc();
    return heap.alloc(heap.context, size, align);
}
static void refusing_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator heap = ta_runtime_malloc();
    heap.free(heap.context, pointer, size);
}

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    uint32_t segment;
    uint64_t session;
} world;

/* encoder: give the host an OTLP encoder; vtable: the ECMAScript datamodel's
 * entries as given (NULL: as they are) */
static void world_open(world *w, int encoder, const ta_datamodel_vtable *vtable) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    if (vtable) w->binding.vtable = vtable;
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, refusing_alloc, refusing_free};
    if (encoder) config.telemetry.otlp.size = sizeof config.telemetry.otlp;
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_pinged(&length);
    CHECK(w->image_bytes != NULL);
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image_bytes, length}, &w->segment), TA_OK);
    free(w->image_bytes); /* the host keeps a copy */
    w->image_bytes = NULL;
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

static int reply_is(const char *got, const char *expected) {
    if (strcmp(got, expected) != 0) fprintf(stderr, "  got:      %s\n  expected: %s\n", got, expected);
    return strcmp(got, expected) == 0;
}

#define RESULT(value) "{\"id\":1,\"result\":" value "}"
#define ERROR_BODY(code, reason, path, message)                                                    \
    "\"error\":{\"code\":" code ",\"message\":\"" message "\",\"data\":{\"reason\":\"" reason         \
    "\",\"path\":\"" path "\"}}}"
#define ERROR(code, reason, path, message) "{\"id\":1," ERROR_BODY(code, reason, path, message)

static void test_spans_and_metrics(void) {
    world w;
    world_open(&w, 1, NULL);
    settle(&w);
    ta_bytes out; /* the host's, until the ack: copied into the request */
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK(out.length > 0);
    /* the request, built by hand: {"method": "trace.spans.decode", "params": {"traces": bytes}} */
#define DECODE "\xa3\x62id\x01\x66method\x72trace.spans.decode\x66params\xa1\x66traces"
    static unsigned char request[(1 << 16) + 64];
    size_t n = sizeof DECODE - 1;
    memcpy(request, DECODE "\x59", n + 1);
    n++;
    request[n++] = (unsigned char)(out.length >> 8);
    request[n++] = (unsigned char)out.length;
    memcpy(request + n, out.pointer, out.length);
    n += out.length;
    ta_host_effects_ack(w.host); /* the output is copied: its batch ends */
    static unsigned char reply[1 << 16];
    ta_buffer answer = {reply, sizeof reply, 0};
    CHECK_EQUAL(host_post_copy(w.host, (ta_bytes){request, n}, &answer), TA_OK);
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
    /* metrics on demand; the second call counts anew */
    ta_bytes metrics;
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK);
    CHECK(metrics.length > 0);
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK);
    CHECK(metrics.length > 0);
    world_close(&w);
    world_open(&w, 0, NULL);
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_ERROR_STATE); /* no encoder */
    world_close(&w);
}

static void test_actor_context(void) {
    world w;
    world_open(&w, 0, NULL);
    static host_ask a;
    ta_cbor_writer *writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    /* before its first macrostep the actor's <data> are not initialized */
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":undefined,\"who\":undefined,\"nothing\":undefined}}")));
    settle(&w);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":1,\"who\":\"bob\",\"nothing\":undefined}}")));
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){"ping", 4};
    event.session = w.session;
    CHECK_EQUAL(ta_host_send(w.host, &event), TA_OK);
    settle(&w);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":2,\"who\":\"bob\",\"nothing\":undefined}}")));
    /* hibernated: wake it first */
    static unsigned char blob[1 << 16];
    ta_bytes hibernated; /* the host's, until the ack */
    CHECK_EQUAL(ta_host_hibernate(w.host, w.session, &hibernated), TA_OK);
    size_t length = output_copy(hibernated, blob, sizeof blob);
    ta_host_effects_ack(w.host); /* the output is copied: its batch ends */
    writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32002", "state", "params.session", "the actor is hibernated: wake it first")));
    CHECK_EQUAL(ta_host_wake(w.host, w.session, (ta_bytes){blob, length}), TA_OK);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":2,\"who\":\"bob\",\"nothing\":undefined}}")));
    world_close(&w);

    /* null stays null, apart from undefined */
    world_open(&w, 0, NULL);
    settle(&w);
    event.name = (ta_string_view){"set", 3};
    event.session = w.session;
    event.data = ta_datamodel_ecmascript_payload_eval(w.ecmascript, 0, ta_string("{n: null}"));
    CHECK_EQUAL(ta_host_send(w.host, &event), TA_OK);
    settle(&w);
    writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":null,\"who\":undefined,\"nothing\":undefined}}")));
    world_close(&w);

    /* a datamodel that does not show its variables (no `variable` entry) */
    static ta_datamodel_vtable hidden;
    hidden = *ta_datamodel_ecmascript_vtable();
    hidden.variable = NULL;
    world_open(&w, 0, &hidden);
    settle(&w);
    writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32002", "state", "params.session",
                                 "the actor's datamodel does not show its variables")));
    world_close(&w);
}

/* actor.send with `params` (its members written by `write`) into `a`. */
typedef void (*send_params)(ta_cbor_writer *writer, uint64_t session);

static ta_status send_ask(world *w, host_ask *a, send_params write, uint32_t count) {
    ta_cbor_writer *writer = ask_begin(a, "actor.send", count);
    write(writer, w->session);
    return ask_send(w->host, a);
}

static void set_by_session(ta_cbor_writer *writer, uint64_t session) {
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, session);
    ta_cbor_write_text(writer, ta_string("event"));
    ta_cbor_write_text(writer, ta_string("set"));
    ta_cbor_write_text(writer, ta_string("data"));
    ta_cbor_write_map(writer, 1);
    ta_cbor_write_text(writer, ta_string("n"));
    ta_cbor_write_uint(writer, 7);
    ta_cbor_write_text(writer, ta_string("origin"));
    ta_cbor_write_text(writer, ta_string("tinyactors://b/@x"));
}

static void ping_by_address(ta_cbor_writer *writer, uint64_t session) {
    (void)session;
    ta_cbor_write_text(writer, ta_string("address"));
    ta_cbor_write_text(writer, ta_string("tinyactors://h/#_scxml_1"));
    ta_cbor_write_text(writer, ta_string("event"));
    ta_cbor_write_text(writer, ta_string("ping"));
}

static void elsewhere(ta_cbor_writer *writer, uint64_t session) {
    (void)session;
    ta_cbor_write_text(writer, ta_string("address"));
    ta_cbor_write_text(writer, ta_string("tinyactors://b/#_scxml_1"));
    ta_cbor_write_text(writer, ta_string("event"));
    ta_cbor_write_text(writer, ta_string("ping"));
}

static void nobody(ta_cbor_writer *writer, uint64_t session) {
    (void)session;
    ta_cbor_write_text(writer, ta_string("address"));
    ta_cbor_write_text(writer, ta_string("tinyactors://h/@nobody"));
    ta_cbor_write_text(writer, ta_string("event"));
    ta_cbor_write_text(writer, ta_string("ping"));
}

static void no_event(ta_cbor_writer *writer, uint64_t session) {
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, session);
}

static void not_a_value(ta_cbor_writer *writer, uint64_t session) {
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, session);
    ta_cbor_write_text(writer, ta_string("event"));
    ta_cbor_write_text(writer, ta_string("set"));
    ta_cbor_write_text(writer, ta_string("data"));
    ta_cbor_write_tag(writer, 99); /* no neutral value has tag 99 */
    ta_cbor_write_uint(writer, 1);
}

/* An event from a remote client: the target by session or address, its data
 * read by the target's datamodel, the origin passed through. */
static void test_actor_send(void) {
    world w;
    world_open(&w, 0, NULL);
    settle(&w);
    static host_ask a;
    CHECK_EQUAL(send_ask(&w, &a, set_by_session, 4), TA_OK);
    CHECK(reply_is(a.text, RESULT("{}")));
    settle(&w);
    ta_cbor_writer *writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"variables\":{\"n\":7,\"who\":\"tinyactors://b/@x\",\"nothing\":undefined}}")));
    CHECK_EQUAL(send_ask(&w, &a, ping_by_address, 2), TA_OK);
    CHECK(reply_is(a.text, RESULT("{}")));
    settle(&w);
    char state[64];
    CHECK_EQUAL(ask_state(w.host, w.session, state, sizeof state), TA_OK);
    CHECK(strcmp(state, "\"t\"") == 0);
    CHECK_EQUAL(send_ask(&w, &a, elsewhere, 2), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "range", "params.address", "address is not an address of this host's system")));
    CHECK_EQUAL(send_ask(&w, &a, nobody, 2), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32001", "stale", "params.address", "the address names no actor in this host")));
    CHECK_EQUAL(send_ask(&w, &a, no_event, 1), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32602", "missing", "params.event", "event is the event's name")));
    CHECK_EQUAL(send_ask(&w, &a, not_a_value, 3), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32006", "malformed", "params.data", "data is no value the actor's datamodel reads")));
    world_close(&w);

    /* a datamodel that reads no values (no `from_wire` entry) */
    static ta_datamodel_vtable deaf;
    deaf = *ta_datamodel_ecmascript_vtable();
    deaf.from_wire = NULL;
    world_open(&w, 0, &deaf);
    settle(&w);
    CHECK_EQUAL(send_ask(&w, &a, set_by_session, 4), TA_OK);
    CHECK(reply_is(a.text, ERROR("-32002", "state", "params.data", "the actor's datamodel reads no values")));
    CHECK_EQUAL(send_ask(&w, &a, ping_by_address, 2), TA_OK); /* no data: no reading */
    CHECK(reply_is(a.text, RESULT("{}")));
    world_close(&w);
}

/* {"id": id, "method": "trace.watch", "params": {"rules": [{}]}} (id < 0:
 * no "id"), a watch matching everything, in `bytes`; its length. */
static size_t watch_message(unsigned char *bytes, size_t capacity, int64_t id) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){bytes, capacity, 0}, NULL);
    ta_cbor_write_map(&w, id >= 0 ? 3 : 2);
    if (id >= 0) {
        ta_cbor_write_text(&w, ta_string("id"));
        ta_cbor_write_uint(&w, (uint64_t)id);
    }
    ta_cbor_write_text(&w, ta_string("method"));
    ta_cbor_write_text(&w, ta_string("trace.watch"));
    ta_cbor_write_text(&w, ta_string("params"));
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("rules"));
    ta_cbor_write_array(&w, 1);
    ta_cbor_write_map(&w, 0);
    ta_bytes out;
    CHECK_EQUAL(ta_cbor_writer_bytes(&w, &out), TA_OK);
    return out.length;
}

/* {"id": id, "method": "trace.unwatch", "params": {"id": watch}}; its length. */
static size_t unwatch_message(unsigned char *bytes, size_t capacity, int64_t id, uint64_t watch) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){bytes, capacity, 0}, NULL);
    ta_cbor_write_map(&w, 3);
    ta_cbor_write_text(&w, ta_string("id"));
    ta_cbor_write_uint(&w, (uint64_t)id);
    ta_cbor_write_text(&w, ta_string("method"));
    ta_cbor_write_text(&w, ta_string("trace.unwatch"));
    ta_cbor_write_text(&w, ta_string("params"));
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("id"));
    ta_cbor_write_uint(&w, watch);
    ta_bytes out;
    CHECK_EQUAL(ta_cbor_writer_bytes(&w, &out), TA_OK);
    return out.length;
}

/* A message's CBOR from diagnostic-free parts: {"id": 1, "method": method}
 * (id < 0: no "id"), in `bytes`; its length. */
static size_t message(unsigned char *bytes, size_t capacity, const char *method, int64_t id,
                      uint64_t session) {
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){bytes, capacity, 0}, NULL);
    ta_cbor_write_map(&w, TA_CBOR_COUNT_LATER);
    if (id >= 0) {
        ta_cbor_write_text(&w, ta_string("id"));
        ta_cbor_write_uint(&w, (uint64_t)id);
    }
    ta_cbor_write_text(&w, ta_string("method"));
    ta_cbor_write_text(&w, ta_string(method));
    if (session) {
        ta_cbor_write_text(&w, ta_string("params"));
        ta_cbor_write_map(&w, 1);
        ta_cbor_write_text(&w, ta_string("session"));
        ta_cbor_write_uint(&w, session);
    }
    ta_cbor_write_end(&w);
    ta_bytes out;
    CHECK_EQUAL(ta_cbor_writer_bytes(&w, &out), TA_OK);
    return out.length;
}

/* Effect `index`'s WIRE in diagnostic notation. */
static const char *wire_text(ta_host *host, uint32_t index) {
    static char text[4096];
    ta_bytes wire = ta_host_effect_bytes(host, index, TA_EFFECT_FIELD_WIRE);
    ta_buffer out = {text, sizeof text - 1, 0};
    text[0] = 0;
    if (ta_cbor_diagnostic(wire, &out) == TA_OK) text[out.length] = 0;
    return text;
}

/* ta_host_post: the reply is the
 * last effect the post appends, published without a run, after the
 * method's own effects; a message without an id gets none; refusals. */
static void test_post(void) {
    world w;
    world_open(&w, 1, NULL);
    settle(&w);
    unsigned char bytes[256];
    const ta_host_effect *effects;
    /* one REPLY, the batch's last, valid until the ack */
    size_t length = message(bytes, sizeof bytes, "actor.get", 1, w.session);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK_EQUAL(ta_host_effect_number(w.host, 0, TA_EFFECT_FIELD_KIND), TA_HOST_EFFECT_REPLY);
    CHECK_EQUAL(effects[0].kind, TA_HOST_EFFECT_REPLY);
    CHECK_EQUAL(ta_host_effect_number(w.host, 0, TA_EFFECT_FIELD_SESSION), 0u);
    CHECK(reply_is(wire_text(w.host, 0), RESULT("{\"session\":1,\"segment\":0,\"chart\":\"pinged\","
                                                "\"parent\":null,\"mailbox\":0,\"invocations\":0}")));
    /* A post during dispatch leaves its reply for the next batch. */
    length = unwatch_message(bytes, sizeof bytes, 2, 1);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK_EQUAL(ta_host_effect_bytes(w.host, 1, TA_EFFECT_FIELD_WIRE).length, 0);
    CHECK(reply_is(wire_text(w.host, 0), RESULT("{\"session\":1,\"segment\":0,\"chart\":\"pinged\","
                                                "\"parent\":null,\"mailbox\":0,\"invocations\":0}")));
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK(reply_is(wire_text(w.host, 0),
                   "{\"id\":2," ERROR_BODY("-32001", "stale", "params.id", "no such watch: it ended or never was")));
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 0u);
    /* without an id: applied, no REPLY */
    length = watch_message(bytes, sizeof bytes, -1);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 0u);
    length = unwatch_message(bytes, sizeof bytes, 3, 1);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK(reply_is(wire_text(w.host, 0), "{\"id\":3,\"result\":{}}")); /* the watch was there */
    ta_host_effects_ack(w.host);
    /* not a request (a batch): answered with a null id */
    static const unsigned char batch[] = {0x81, 0xa0};
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){batch, sizeof batch}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK(reply_is(wire_text(w.host, 0),
                   "{\"id\":null," ERROR_BODY("-32600", "type", "", "a request is a map")));
    ta_host_effects_ack(w.host);
    /* trace.end: its TRACES before its REPLY, published without a run */
    length = message(bytes, sizeof bytes, "trace.end", 4, 0);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    uint32_t count = ta_host_effects(w.host, &effects), traces = 0;
    CHECK(count >= 2);
    for (uint32_t i = 0; i + 1 < count; i++)
        traces += effects[i].kind == TA_HOST_EFFECT_TRACES;
    CHECK_EQUAL(traces, 1u);
    CHECK_EQUAL(effects[count - 1].kind, TA_HOST_EFFECT_REPLY);
    CHECK(reply_is(wire_text(w.host, count - 1), "{\"id\":4,\"result\":{}}"));
    ta_host_effects_ack(w.host);
    world_close(&w);

    /* refused room: nothing applied, no REPLY (the pool is not there yet) */
    world_open(&w, 1, NULL);
    length = watch_message(bytes, sizeof bytes, 5);
    refusing = 1;
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_ERROR_NO_MEMORY);
    refusing = 0;
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 0u);
    length = unwatch_message(bytes, sizeof bytes, 6, 1);
    CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){bytes, length}), TA_OK);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    CHECK(reply_is(wire_text(w.host, 0),
                   "{\"id\":6," ERROR_BODY("-32001", "stale", "params.id", "no such watch: it ended or never was")));
    world_close(&w);
}

/* ta_host_create with raw config bytes: the status, and the error reply's
 * diagnostic text in `text` ("" when none was written). */
static ta_status create(const void *bytes, size_t length, char *text, ta_host **host) {
    static unsigned char error[512];
    ta_buffer reply = {error, sizeof error, 0};
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.allocator = ta_runtime_malloc();
    environment.error = &reply;
    ta_status status = ta_host_create((ta_bytes){bytes, length}, &environment, host);
    text[0] = 0;
    ta_buffer diagnostic = {text, 1023, 0};
    if (reply.length && ta_cbor_diagnostic((ta_bytes){error, reply.length}, &diagnostic) == TA_OK)
        text[diagnostic.length] = 0;
    return status;
}


static void test_create(void) {
    char text[1024];
    ta_host *host = NULL;
    /* everything the config holds */
    static unsigned char bytes[1024];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){bytes, sizeof bytes, 0}, NULL);
    ta_cbor_write_map(&w, 5);
    ta_cbor_write_text(&w, ta_string("systemID"));
    ta_cbor_write_text(&w, ta_string("orders"));
    ta_cbor_write_text(&w, ta_string("system"));
    ta_cbor_write_map(&w, 3);
    ta_cbor_write_text(&w, ta_string("maxActors"));
    ta_cbor_write_uint(&w, 8);
    ta_cbor_write_text(&w, ta_string("ticksPerSecond"));
    ta_cbor_write_uint(&w, 1000);
    ta_cbor_write_text(&w, ta_string("tracing"));
    ta_cbor_write_simple(&w, TA_CBOR_FALSE);
    ta_cbor_write_text(&w, ta_string("ioProcessors"));
    ta_cbor_write_array(&w, 1);
    ta_cbor_write_map(&w, 2);
    ta_cbor_write_text(&w, ta_string("type"));
    ta_cbor_write_text(&w, ta_string("http://www.w3.org/TR/scxml/#BasicHTTPEventProcessor"));
    ta_cbor_write_text(&w, ta_string("location"));
    ta_cbor_write_text(&w, ta_string("http://localhost:8080/"));
    ta_cbor_write_text(&w, ta_string("migrateQuantum"));
    ta_cbor_write_uint(&w, 16);
    ta_cbor_write_text(&w, ta_string("telemetry"));
    ta_cbor_write_map(&w, 3);
    ta_cbor_write_text(&w, ta_string("stagingBytes"));
    ta_cbor_write_uint(&w, TA_OTLP_STAGING_BYTES_MIN);
    ta_cbor_write_text(&w, ta_string("logs"));
    ta_cbor_write_simple(&w, TA_CBOR_FALSE);
    ta_cbor_write_text(&w, ta_string("resource"));
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("service.name"));
    ta_cbor_write_text(&w, ta_string("orders"));
    ta_bytes config;
    CHECK_EQUAL(ta_cbor_writer_bytes(&w, &config), TA_OK);
    CHECK_EQUAL(create(bytes, config.length, text, &host), TA_OK);
    CHECK(host != NULL && text[0] == 0);
    memset(bytes, 0, sizeof bytes); /* nothing points into the config afterwards */
    static host_ask a;
    ask_begin(&a, "host.stats", 0);
    CHECK_EQUAL(ask_send(host, &a), TA_OK);
    CHECK_EQUAL(ask_uint(&a, ask_field(&a, 0, "host"), "segments"), 0u);
    ta_bytes traces;
    CHECK_EQUAL(ta_host_traces(host, &traces), TA_OK); /* an encoder: nothing staged */
    CHECK_EQUAL(traces.length, 0u);
    ta_host_free(host);
    /* without an error buffer: refused all the same */
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.allocator = ta_runtime_malloc();
    CHECK_EQUAL(ta_host_create((ta_bytes){"\xa0", 1}, &environment, &host), TA_ERROR_INVALID);
}

/* host.config answers the effective config: the system's defaults where the
 * config has none (ticksPerSecond 1e9), its own values where it has them,
 * and a watch's seconds are counted in the effective rate. The reply is a
 * config ta_host_create takes, and a host made from it answers the same. */
static uint64_t config_ticks(ta_host *host) {
    static host_ask a;
    ask_begin(&a, "host.config", 0);
    if (ask_send(host, &a) != TA_OK) return UINT64_MAX;
    return ask_uint(&a, ask_field(&a, 0, "system"), "ticksPerSecond");
}

/* trace.watch for `seconds` at time 0: the run's next deadline, its end. */
static ta_time watch_end(ta_host *host, uint64_t seconds) {
    static host_ask a;
    ta_cbor_writer *w = ask_begin(&a, "trace.watch", 2);
    ta_cbor_write_text(w, ta_string("rules"));
    ta_cbor_write_array(w, 1);
    ta_cbor_write_map(w, 0);
    ta_cbor_write_text(w, ta_string("seconds"));
    ta_cbor_write_uint(w, seconds);
    if (ask_send(host, &a) != TA_OK || a.result == TA_NIL) return 0;
    return ta_host_run(host, 1000).next_deadline;
}

static void test_config_effective(void) {
    char text[1024];
    ta_host *host = NULL;
    static host_ask a, b;
    /* {"systemID": "h"}: every default */
    CHECK_EQUAL(create("\xa1\x68systemID\x61h", 12, text, &host), TA_OK);
    CHECK_EQUAL(config_ticks(host), 1000000000u);
    ask_begin(&a, "host.config", 0);
    CHECK_EQUAL(ask_send(host, &a), TA_OK);
    CHECK(reply_is(a.text, RESULT("{\"systemID\":\"h\",\"system\":{\"maxActors\":64,\"maxEvents\":256,"
                                  "\"maxTimers\":64,\"workspaces\":4,\"maxRequests\":16,\"outbox\":64,"
                                  "\"quantum\":1000,\"instanceBytes\":0,\"stringBytes\":17408,"
                                  "\"ticksPerSecond\":1000000000,\"traceBytes\":131072,"
                                  "\"traceContexts\":256,\"traceRules\":16,\"traceSeed\":0,"
                                  "\"tracing\":true},\"ioProcessors\":[],\"migrateQuantum\":256}")));
    CHECK_EQUAL(watch_end(host, 2), 2000000000u);
    /* the reply is a config: a host made from it answers the same */
    ta_host *again = NULL;
    size_t prefix = 12; /* {"id": 1, "result": ... */
    CHECK_EQUAL(create(a.reply + prefix, a.answer.length - prefix, text, &again), TA_OK);
    ask_begin(&b, "host.config", 0);
    CHECK_EQUAL(ask_send(again, &b), TA_OK);
    CHECK(reply_is(b.text, a.text));
    ta_host_free(again);
    /* params: none, or an empty map; anything else is refused */
    ask_begin(&a, "host.config", 1);
    ta_cbor_write_text(&a.writer, ta_string("x"));
    ta_cbor_write_uint(&a.writer, 1);
    CHECK_EQUAL(ask_send(host, &a), TA_OK);
    CHECK(a.result == TA_NIL);
    CHECK(strstr(a.text, "\"code\":-32602") != NULL && strstr(a.text, "\"path\":\"params.x\"") != NULL);
    ta_host_free(host);
    /* {"systemID": "h", "system": {"ticksPerSecond": 1000}, "telemetry": {}} */
    CHECK_EQUAL(create("\xa3\x68systemID\x61h\x66system\xa1\x6eticksPerSecond\x19\x03\xe8"
                       "\x69telemetry\xa0",
                       49, text, &host),
                TA_OK);
    CHECK_EQUAL(config_ticks(host), 1000u);
    CHECK_EQUAL(watch_end(host, 2), 2000u);
    ask_begin(&a, "host.config", 0);
    CHECK_EQUAL(ask_send(host, &a), TA_OK);
    uint32_t telemetry = ask_field(&a, 0, "telemetry");
    CHECK_EQUAL(ask_uint(&a, telemetry, "stagingBytes"), TA_OTLP_STAGING_BYTES_DEFAULT);
    CHECK_EQUAL(ask_uint(&a, telemetry, "logBytes"), TA_OTLP_STAGING_BYTES_DEFAULT / 4u);
    CHECK_EQUAL(ask_uint(&a, telemetry, "openSpans"), 1024u);
    CHECK_EQUAL(ask_uint(&a, telemetry, "chunks"), 8192u);
    ta_host_free(host);
}

int main(void) {
    RUN(test_post);
    RUN(test_spans_and_metrics);
    RUN(test_actor_context);
    RUN(test_actor_send);
    RUN(test_create);
    RUN(test_config_effective);
    TEST_MAIN_END();
}
