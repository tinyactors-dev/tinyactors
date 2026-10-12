/* The guard (docs/adr/0010 in the repository root, decision 3): while the
 * host calls into a datamodel outside a run (a request, an envelope,
 * hibernate, the telemetry takes, release in the ack, deliver, free), calls
 * back into the host follow the rules of a run (host.h, "During a run"):
 * send, spawn and terminate are taken, lookups answer, everything else is
 * refused. Each case wraps one entry of the ECMAScript datamodel with a C
 * hook that calls back into the host from inside it. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>
#include "../support/host_request.h"
#include "../support/host_config.h"

/* `a` and `b` are variables for actor.context; `out` sends a value to
 * another system; `self` sends a value to itself (a traced value). */
#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='guarded' "        \
    "initial='s'><datamodel><data id='a' expr='1'/><data id='b' expr='({x: 2})'/></datamodel>"     \
    "<state id='s'>"                                                                               \
    "<transition event='out'><send target='tinyactors://b/@x' event='hello'>"                      \
    "<content expr='({v: 1})'/></send></transition>"                                               \
    "<transition event='self'><send event='back'><content expr='({v: 2})'/></send></transition>"   \
    "<transition event='back'><log expr=\"'back'\"/></transition>"                                 \
    "<transition event='later'><send event='tick' delay='10s'>"                                    \
    "<content expr='({t: 1})'/></send></transition>"                                               \
    "</state></scxml>"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_vtable vtable; /* the ECMAScript one, with every hook below wrapped */
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    uint32_t segment;
    uint64_t session, other;
    void (*hook)(struct world *); /* run by the next wrapped entry, once */
    int hooks;
} world;

static world *current;
static int to_wire_calls, serialize_calls, payload_serialize_calls;

/* The host's allocator, counted: the C library's. */
static size_t allocations;
static void *counting_alloc(void *context, size_t size, size_t align) {
    (void)context;
    allocations++;
    ta_allocator c = ta_runtime_malloc();
    return c.alloc(c.context, size, align);
}
static void counting_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator c = ta_runtime_malloc();
    c.free(c.context, pointer, size);
}

/* {"id": 1, "method": "host.stats"} */
#define STATS "\xa2\x62id\x01\x66method\x6ahost.stats"

static void hook(void) {
    if (!current || !current->hook) return;
    void (*action)(world *) = current->hook;
    current->hook = NULL;
    action(current);
    current->hooks++;
}

static const ta_datamodel_vtable *inner(void) { return ta_datamodel_ecmascript_vtable(); }

static ta_status to_wire(void *datamodel, ta_payload p, ta_bytes *out) {
    to_wire_calls++;
    hook();
    return inner()->to_wire(datamodel, p, out);
}
static ta_status from_wire(void *datamodel, ta_bytes cbor, ta_payload *out) {
    hook();
    return inner()->from_wire(datamodel, cbor, out);
}
static ta_status variable(const ta_datamodel_call *call, ta_string_view name, ta_payload *out) {
    hook();
    return inner()->variable(call, name, out);
}
static ta_status serialize(const ta_datamodel_call *call, ta_bytes *out) {
    serialize_calls++;
    hook();
    return inner()->serialize(call, out);
}
static ta_status payload_serialize(void *datamodel, ta_payload p, ta_bytes *out) {
    payload_serialize_calls++;
    return inner()->payload_serialize(datamodel, p, out);
}
static void release(void *datamodel, ta_payload p) {
    hook();
    inner()->release(datamodel, p);
}
static void destroy(const ta_datamodel_call *call) {
    hook();
    inner()->destroy(call);
}

static void *compile_guarded(size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"guarded.scxml", 13}, {CHART, sizeof CHART - 1}};
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

static ta_run run_once(world *w) {
    ta_run result = ta_host_run(w->host, 100000);
    ta_host_effects_ack(w->host);
    return result;
}

/* A host with the guarded chart, two actors of it and a peer for system b. */
static void world_open(world *w, int telemetry) {
    memset(w, 0, sizeof *w);
    current = w;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    w->vtable = *w->binding.vtable;
    w->vtable.to_wire = to_wire;
    w->vtable.from_wire = from_wire;
    w->vtable.variable = variable;
    w->vtable.serialize = serialize;
    w->vtable.payload_serialize = payload_serialize;
    w->vtable.release = release;
    w->vtable.destroy = destroy;
    w->binding.vtable = &w->vtable;
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"a", 1};
    config.system.size = sizeof config.system;
    config.system.ticks_per_second = 1000;
    config.system.max_actors = 8;
    static ta_trace_policy policy;
    if (telemetry) {
        ta_trace_policy_default(&policy);
        policy.kinds[TA_TRACE_DETAIL_SAMPLED] = TA_TRACE_FULL | TA_TRACE_VALUES;
        config.system.trace_policy = &policy;
        config.telemetry.otlp.size = sizeof config.telemetry.otlp;
        config.telemetry.otlp.flags = TA_OTLP_VALUES;
    }
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = (ta_allocator){NULL, counting_alloc, counting_free};
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_guarded(&length);
    CHECK(w->image_bytes != NULL);
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){w->image_bytes, length}, &w->segment), TA_OK);
    free(w->image_bytes); /* the host keeps a copy */
    w->image_bytes = NULL;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    spawn.name = (ta_string_view){"g", 1};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
    spawn.name = (ta_string_view){"h", 1};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->other), TA_OK);
    ta_peer_config peer;
    memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    peer.flags = TA_PEER_TRUSTED;
    peer.id = (ta_string_view){"b", 1};
    uint32_t handle;
    CHECK_EQUAL(ta_host_peer_add(w->host, &peer, &handle), TA_OK);
    ta_host_advance_time(w->host, (ta_host_time){1, 1700000000000000000ull});
    run_once(w);
}

static void world_close(world *w) {
    w->hook = NULL;
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
    free(w->image_bytes);
    current = NULL;
}

static void send_event(world *w, uint64_t session, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = session;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    CHECK_EQUAL(ta_host_send(w->host, &event), TA_OK);
}

static int alive(world *w, uint64_t session) {
    ta_host_location where;
    return ta_host_locate(w->host, session, &where) == TA_OK;
}

/* Everything a run refuses, refused here too; the inputs are taken. */
static void refused(world *w) {
    unsigned char bytes[256];
    ta_buffer room = {bytes, sizeof bytes, 0};
    ta_bytes out;
    CHECK_EQUAL(ta_host_post(w->host, (ta_bytes){STATS, sizeof STATS - 1}), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_hibernate(w->host, w->other, &out), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_export_actor(w->host, w->other, &out), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_snapshot(w->host, &room), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_traces(w->host, &out), TA_ERROR_STATE);
    ta_run nested = ta_host_run(w->host, 10);
    CHECK_EQUAL(nested.status, TA_RUN_FAULT);
    CHECK_EQUAL(nested.detail, (uint32_t)TA_ERROR_STATE);
    const ta_host_effect *effects;
    CHECK_EQUAL(ta_host_effects(w->host, &effects), 0u);
    ta_host_effects_ack(w->host); /* nothing */
    ta_host_location where; /* lookups answer */
    CHECK_EQUAL(ta_host_locate(w->host, w->other, &where), TA_OK);
    CHECK_EQUAL(ta_host_actor_by_name(w->host, (ta_string_view){"h", 1}), w->other);
}

/* The inputs: an event for the other actor, and this actor terminated. */
static void inputs(world *w) {
    refused(w);
    send_event(w, w->other, "self");
    CHECK_EQUAL(ta_host_terminate(w->host, w->session), TA_OK);
    CHECK(alive(w, w->session)); /* it exits at its next microstep boundary */
}

/* ---- actor.context: variable() and to_wire() of a live instance -------------------------- */

static void test_request(void) {
    world w;
    world_open(&w, 0);
    w.hook = inputs; /* in the first variable() */
    static host_ask a;
    ta_cbor_writer *writer = ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(writer, ta_string("session"));
    ta_cbor_write_uint(writer, w.session);
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK_EQUAL(w.hooks, 1);
    /* both variables read from the instance the hook could not destroy */
    CHECK(strstr(a.text, "\"a\":1") != NULL);
    CHECK(strstr(a.text, "\"b\":{\"x\":2}") != NULL);
    run_once(&w);
    CHECK(!alive(&w, w.session)); /* the terminate took effect in the next run */
    CHECK(alive(&w, w.other));
    /* and a to_wire calling back is guarded the same way */
    w.hook = refused;
    ask_begin(&a, "actor.context", 1);
    ta_cbor_write_text(&a.writer, ta_string("session"));
    ta_cbor_write_uint(&a.writer, w.other);
    w.vtable.variable = inner()->variable;
    CHECK_EQUAL(ask_send(w.host, &a), TA_OK);
    CHECK_EQUAL(w.hooks, 2);
    world_close(&w);
}

/* ---- ta_host_envelope: to_wire, and release in the ack -------------------------------- */

static void test_envelope_and_ack(void) {
    world w;
    world_open(&w, 0);
    send_event(&w, w.session, "out");
    ta_host_run(w.host, 100000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w.host, &effects), peer = n;
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_HOST_EFFECT_PEER) peer = i;
    CHECK(peer < n);
    if (peer < n) {
        w.hook = refused;
        ta_bytes out;
        CHECK_EQUAL(ta_host_envelope(w.host, &effects[peer], &out), TA_OK);
        CHECK_EQUAL(w.hooks, 1);
        CHECK(out.length > 0);
    }
    w.hook = inputs; /* in the release of the PEER effect's payload */
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(w.hooks, 2);
    run_once(&w);
    CHECK(!alive(&w, w.session));
    world_close(&w);
}

/* ---- deliver: from_wire ------------------------------------------------------------------ */

static void deliver_refused(world *w) {
    refused(w);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://a/@h", 17};
    envelope.name = (ta_string_view){"self", 4};
    CHECK_EQUAL(ta_host_deliver(w->host, 0, &envelope), TA_ERROR_STATE);
    send_event(w, w->other, "self");
}

static void test_deliver(void) {
    world w;
    world_open(&w, 0);
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    envelope.size = sizeof envelope;
    envelope.target = (ta_string_view){"tinyactors://a/@g", 17};
    envelope.name = (ta_string_view){"back", 4};
    envelope.origin = (ta_string_view){"tinyactors://b/@x", 17};
    envelope.msgid = (ta_string_view){"m1", 2};
    envelope.wire = (ta_bytes){"\xa1\x61v\x01", 4}; /* {"v": 1} */
    w.hook = deliver_refused;
    CHECK_EQUAL(ta_host_deliver(w.host, 0, &envelope), TA_ROUTE_DELIVERED);
    CHECK_EQUAL(w.hooks, 1);
    world_close(&w);
}

/* ---- hibernate: serialize ---------------------------------------------------------------- */

static void test_hibernate(void) {
    world w;
    world_open(&w, 0);
    w.hook = inputs;
    static unsigned char blob[1 << 16];
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w.host, w.session, &out), TA_OK);
    CHECK(out.length <= sizeof blob);
    memcpy(blob, out.pointer, out.length); /* the host's view, until the next call into it */
    CHECK_EQUAL(w.hooks, 1);
    /* terminated while it was written: it exits once woken */
    CHECK_EQUAL(ta_host_wake(w.host, w.session, (ta_bytes){blob, out.length}), TA_OK);
    run_once(&w);
    CHECK(!alive(&w, w.session));
    CHECK(alive(&w, w.other));
    world_close(&w);
}

/* ---- the telemetry takes: to_wire of a traced value --------------------------------------- */

static void test_traces(void) {
    world w;
    world_open(&w, 1);
    run_once(&w);
    send_event(&w, w.session, "self");
    ta_host_run(w.host, 100000); /* its records are read by the next take */
    ta_host_effects_ack(w.host);
    w.hook = refused;
    ta_bytes out;
    CHECK_EQUAL(ta_host_traces(w.host, &out), TA_OK);
    CHECK_EQUAL(w.hooks, 1);
    world_close(&w);
}

/* ---- free: destroy ---------------------------------------------------------------------- */

static int freeing_hooks;

static void while_freeing(world *w) {
    CHECK_EQUAL(ta_host_post(w->host, (ta_bytes){STATS, sizeof STATS - 1}), TA_ERROR_STATE);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    uint64_t session;
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &session), TA_ERROR_STATE); /* a host going takes nothing */
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = w->other;
    event.name = (ta_string_view){"self", 4};
    CHECK_EQUAL(ta_host_send(w->host, &event), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_terminate(w->host, w->other), TA_ERROR_STATE);
    freeing_hooks++;
}

static void test_free(void) {
    world w;
    world_open(&w, 0);
    w.hook = while_freeing;
    ta_host_free(w.host);
    CHECK_EQUAL(freeing_hooks, 1);
    ta_datamodel_ecmascript_free(w.ecmascript);
    free(w.image_bytes);
    current = NULL;
}

/* ---- each value written once (docs/adr/0010, decision 1) ------------------------------- */

static void test_each_output_once(void) {
    world w;
    world_open(&w, 0);
    send_event(&w, w.session, "later"); /* a delayed send with a value */
    send_event(&w, w.other, "out");     /* a PEER effect with a value */
    ta_host_run(w.host, 100000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w.host, &effects);
    to_wire_calls = 0;
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_HOST_EFFECT_PEER) {
            ta_bytes out;
            CHECK_EQUAL(ta_host_envelope(w.host, &effects[i], &out), TA_OK);
        }
    CHECK_EQUAL(to_wire_calls, 1); /* the envelope's value, once */
    ta_host_effects_ack(w.host);
    serialize_calls = payload_serialize_calls = 0;
    ta_bytes out;
    CHECK_EQUAL(ta_host_export_actor(w.host, w.session, &out), TA_OK);
    CHECK_EQUAL(serialize_calls, 1);         /* the session's state */
    CHECK_EQUAL(payload_serialize_calls, 1); /* the delayed send's value */
    world_close(&w);
}

/* ---- the host's outputs grow between runs only (tinyactors-core/AGENTS.md) -------------- */

/* In a datamodel call inside a run: hibernate and export are refused before
 * anything is written, so the host's output does not grow. */
static void in_a_run(world *w) {
    size_t before = allocations;
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w->host, w->other, &out), TA_ERROR_STATE);
    CHECK_EQUAL(ta_host_export_actor(w->host, w->other, &out), TA_ERROR_STATE);
    CHECK_EQUAL(allocations, before);
}

static void test_outputs_grow_between_runs(void) {
    world w;
    world_open(&w, 0);
    CHECK_EQUAL(ta_host_terminate(w.host, w.session), TA_OK);
    w.hook = in_a_run; /* in its destroy, when it exits during the run */
    ta_host_run(w.host, 100000);
    CHECK_EQUAL(w.hooks, 1);
    ta_host_effects_ack(w.host);
    /* outside a run, the system's writer grows the host's output through the
     * host's allocator, in one pass: serialize once */
    serialize_calls = 0;
    size_t before = allocations;
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w.host, w.other, &out), TA_OK);
    CHECK(allocations > before);
    CHECK_EQUAL(serialize_calls, 1);
    static unsigned char blob[1 << 16];
    CHECK(out.length <= sizeof blob);
    memcpy(blob, out.pointer, out.length);
    size_t length = out.length;
    CHECK_EQUAL(ta_host_wake(w.host, w.other, (ta_bytes){blob, length}), TA_OK);
    run_once(&w);
    /* the block is kept at its largest: the same blob again needs none */
    before = allocations;
    CHECK_EQUAL(ta_host_hibernate(w.host, w.other, &out), TA_OK);
    CHECK_EQUAL(out.length, length);
    CHECK_EQUAL(allocations, before);
    world_close(&w);
}

int main(void) {
    RUN(test_request);
    RUN(test_envelope_and_ack);
    RUN(test_deliver);
    RUN(test_hibernate);
    RUN(test_traces);
    RUN(test_free);
    RUN(test_each_output_once);
    RUN(test_outputs_grow_between_runs);
    TEST_MAIN_END();
}
