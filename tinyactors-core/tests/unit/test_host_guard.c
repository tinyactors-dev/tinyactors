/* The guard (docs/adr/0010 in the repository root, decision 3): while the
 * host calls into a datamodel outside a run (a request, hibernate, the
 * telemetry takes, release in the ack and in an input, free), calls
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
 * another system (a SEND for the remote I/O processor); `self` sends a value
 * to itself (a traced value). */
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

/* A host with the guarded chart, two actors of it and a remote I/O
 * processor, which takes the sends to system b. */
static void world_open(world *w, int telemetry) {
    memset(w, 0, sizeof *w);
    current = w;
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    w->vtable = *w->binding.vtable;
    w->vtable.to_wire = to_wire;
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
    static const ta_io_processor_descriptor remote = {
        {"urn:test:remote", 15}, {"", 0}, TA_IO_PROCESSOR_REMOTE, 0};
    config.io_processors = &remote;
    config.io_processor_count = 1;
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
    uint64_t resolved = 0;
    CHECK_EQUAL(ta_host_resolve(w->host, (ta_string_view){"tinyactors://a/@h", 17}, &resolved), TA_OK);
    CHECK_EQUAL(resolved, w->other);
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

/* ---- a SEND for the remote I/O processor: release in the ack ------------------------- */

static void test_send_and_ack(void) {
    world w;
    world_open(&w, 0);
    send_event(&w, w.session, "out");
    ta_host_run(w.host, 100000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w.host, &effects), send = n;
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_EFFECT_SEND && effects[i].payload) send = i;
    CHECK(send < n);
    w.hook = inputs; /* in the release of the SEND effect's payload */
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(w.hooks, 1);
    run_once(&w);
    CHECK(!alive(&w, w.session));
    world_close(&w);
}

/* ---- an input: release of an error.communication's payload --------------------------- */

static void test_input(void) {
    world w;
    world_open(&w, 0);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = w.session;
    event.flags = TA_EVENT_IN_ERROR_COMMUNICATION; /* a transport failing a send */
    event.sendid = (ta_string_view){"m1", 2};
    CHECK_EQUAL(ta_datamodel_ecmascript_payload_from_cbor(w.ecmascript, (ta_bytes){"\xa1\x61v\x01", 4},
                                                          &event.data), /* {"v": 1} */
                TA_OK);
    w.hook = inputs; /* the event carries no value: its payload is released */
    CHECK_EQUAL(ta_host_send(w.host, &event), TA_OK);
    CHECK_EQUAL(w.hooks, 1);
    run_once(&w);
    CHECK(!alive(&w, w.session));
    CHECK(alive(&w, w.other));
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
    memcpy(blob, out.pointer, out.length); /* the host's view, until the ack */
    ta_host_effects_ack(w.host); /* the output is copied: its batch ends */
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
    to_wire_calls = 0;
    send_event(&w, w.session, "later"); /* a delayed send with a value */
    send_event(&w, w.other, "out");     /* a SEND for the remote processor with a value */
    ta_host_run(w.host, 100000);
    const ta_host_effect *effects;
    uint32_t n = ta_host_effects(w.host, &effects), send = n;
    for (uint32_t i = 0; i < n; i++)
        if (effects[i].kind == TA_EFFECT_SEND && effects[i].payload) send = i;
    CHECK(send < n);
    CHECK_EQUAL(to_wire_calls, 0); /* the SEND's value stays a payload: the transport writes it */
    ta_host_effects_ack(w.host);
    serialize_calls = payload_serialize_calls = 0;
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w.host, w.session, &out), TA_OK);
    CHECK_EQUAL(serialize_calls, 1);         /* the session's state */
    CHECK_EQUAL(payload_serialize_calls, 0); /* the delayed send stays in the host, its value with it */
    world_close(&w);
}

/* ---- outputs until the ack (ADR 0012, decision 1) ------------------------------------ */

static int same_bytes(ta_bytes view, const unsigned char *copy, size_t length) {
    return view.length == length && memcmp(view.pointer, copy, length) == 0;
}

/* Traces, a metrics request and a hibernation blob taken in one batch, with
 * calls into the host between them, all read after the last one; an output
 * taken with no batch open opens one: the run refuses and a take gives
 * nothing until the ack. */
static void test_outputs_last_until_the_ack(void) {
    world w;
    world_open(&w, 1);
    send_event(&w, w.session, "self");
    ta_host_run(w.host, 100000);
    const ta_host_effect *effects;
    CHECK(ta_host_effects(w.host, &effects) > 0); /* the batch is open */
    static unsigned char traces_copy[1 << 16], metrics_copy[1 << 16];
    ta_bytes traces, metrics, blob;
    CHECK_EQUAL(ta_host_traces(w.host, &traces), TA_OK);
    CHECK(traces.length > 0 && traces.length <= sizeof traces_copy);
    memcpy(traces_copy, traces.pointer, traces.length);
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK);
    CHECK(metrics.length > 0 && metrics.length <= sizeof metrics_copy);
    memcpy(metrics_copy, metrics.pointer, metrics.length);
    host_ask a; /* a post writes its reply through the host's output too */
    ask_begin(&a, "host.stats", 0);
    ta_bytes request;
    CHECK_EQUAL(ta_cbor_writer_bytes(&a.writer, &request), TA_OK);
    CHECK_EQUAL(ta_host_post(w.host, request), TA_OK);
    CHECK_EQUAL(ta_host_hibernate(w.host, w.other, &blob), TA_OK);
    CHECK(blob.length > 0);
    CHECK(same_bytes(traces, traces_copy, traces.length));
    CHECK(same_bytes(metrics, metrics_copy, metrics.length));
    CHECK(blob.pointer != traces.pointer && blob.pointer != metrics.pointer);
    ta_host_effects_ack(w.host);
    /* the post's REPLY waited for the next take */
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    ta_host_effects_ack(w.host);
    /* with no batch open, an output opens one of its own */
    CHECK_EQUAL(ta_host_metrics(w.host, &metrics), TA_OK);
    CHECK_EQUAL(ta_host_post(w.host, request), TA_OK); /* its REPLY is pending */
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 0u);
    ta_run refused = ta_host_run(w.host, 100000);
    CHECK_EQUAL(refused.status, TA_RUN_FAULT);
    CHECK_EQUAL(refused.detail, (uint32_t)TA_ERROR_STATE);
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effects(w.host, &effects), 1u);
    ta_host_effects_ack(w.host);
    CHECK(ta_host_run(w.host, 100000).status != TA_RUN_FAULT);
    ta_cbor_writer_free(&a.writer);
    world_close(&w);
}

/* ---- the host's outputs grow between runs only (tinyactors-core/AGENTS.md) -------------- */

/* In a datamodel call inside a run: hibernate is refused before
 * anything is written, so the host's output does not grow. */
static void in_a_run(world *w) {
    size_t before = allocations;
    ta_bytes out;
    CHECK_EQUAL(ta_host_hibernate(w->host, w->other, &out), TA_ERROR_STATE);
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

static void batch_closed(world *w) {
    CHECK_EQUAL(ta_host_effect_number(w->host, 0, TA_EFFECT_FIELD_KIND), 0);
    CHECK_EQUAL(ta_host_effect_number(w->host, 19, TA_EFFECT_FIELD_KIND), 0);
    CHECK_EQUAL(ta_host_effect_text(w->host, 0, TA_EFFECT_FIELD_EVENT).length, 0);
    CHECK_EQUAL(ta_host_effect_bytes(w->host, 0, TA_EFFECT_FIELD_WIRE).length, 0);
}

static void test_closed_batch_during_release(void) {
    for (int closing = 0; closing < 2; closing++) {
        world w;
        world_open(&w, 0);
        send_event(&w, w.session, "out");
        CHECK_EQUAL(ta_host_run(w.host, 100000).status, TA_RUN_EFFECTS); /* its sender waits for the batch */
        const ta_host_effect *effects;
        uint32_t count = ta_host_effects(w.host, &effects);
        int payload = 0;
        for (uint32_t i = 0; i < count; i++) payload |= effects[i].payload != 0;
        CHECK(payload);
        size_t before = allocations;
        for (int i = 0; i < 20; i++)
            CHECK_EQUAL(ta_host_post(w.host, (ta_bytes){STATS, sizeof STATS - 1}), TA_OK);
        CHECK(allocations > before); /* the borrowed array was replaced and held */
        w.hook = batch_closed;
        if (closing) {
            ta_host_free(w.host);
            w.host = NULL;
        } else {
            ta_host_effects_ack(w.host);
        }
        CHECK_EQUAL(w.hooks, 1);
        world_close(&w);
    }
}

int main(void) {
    RUN(test_closed_batch_during_release);
    RUN(test_request);
    RUN(test_send_and_ack);
    RUN(test_input);
    RUN(test_hibernate);
    RUN(test_traces);
    RUN(test_free);
    RUN(test_each_output_once);
    RUN(test_outputs_last_until_the_ack);
    RUN(test_outputs_grow_between_runs);
    TEST_MAIN_END();
}
