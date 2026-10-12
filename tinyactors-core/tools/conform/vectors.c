/* The methods the protocol vectors use (driver.md, from host.create on):
 * one host and its datamodel between calls, each method one or two core
 * calls. The runner builds the vectors' setups with them
 * (../tinyactors-test/lib/runner.rb, `setup`). */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/trace.h>

void conform_vectors_close(conform_vectors *vectors) {
    if (vectors->host) ta_host_free(vectors->host);
    vectors->host = NULL;
    conform_datamodel_close(&vectors->datamodel);
    for (uint32_t i = 0; i < vectors->image_count; i++)
        conform_image_free(&vectors->images[i]);
    vectors->image_count = 0;
}

/* The methods after host.create need its host (-32002 state). */
static ta_host *host_of(conform *state, conform_call *call) {
    if (!state->vectors.host)
        conform_fail(call, &(conform_failure){CONFORM_STATE, "state", ""}, "no host: call host.create first");
    return state->vectors.host;
}

/* The param `name` (a neutral value) as a payload of the host's datamodel:
 * 0 when it is absent. */
typedef struct payload_reading {
    ta_status status;
    ta_payload payload;
} payload_reading;

static payload_reading payload_read(conform *state, conform_call *call, const char *name) {
    payload_reading reading = {TA_OK, 0};
    uint32_t at = conform_param(call, name);
    if (at == TA_NIL) return reading;
    ta_buffer bytes = {NULL, 0, 0};
    reading.status = conform_item_bytes(call, &(conform_item){at, NULL}, &bytes);
    if (reading.status == TA_OK)
        reading.status = conform_datamodel_read(&state->vectors.datamodel, (ta_bytes){bytes.pointer, bytes.length},
                                                &reading.payload);
    conform_buffer_free(&bytes);
    return reading;
}

/* ---- host.create, host.free, host.request ---------------------------------------- */

static void host_create(conform *state, conform_call *call, conform_result *result) {
    conform_vectors *vectors = &state->vectors;
    ta_bytes config = {NULL, 0};
    if (vectors->host) {
        conform_fail(call, &(conform_failure){CONFORM_STATE, "state", ""}, "a host already: call host.free first");
        return;
    }
    if (!conform_required(call, "config") || !conform_bytes(call, "config", &config) ||
        !conform_datamodel_open(&vectors->datamodel, call))
        return;
    static unsigned char error[8192];
    ta_buffer refusal = {error, sizeof error, 0};
    ta_host_environment environment = conform_datamodel_environment(&vectors->datamodel, &refusal);
    ta_status status = ta_host_create(config, &environment, &vectors->host);
    conform_result_integer(result, "status", status);
    if (status == TA_OK) {
        conform_result_text(result, "datamodel", ta_string(vectors->datamodel.binding.vtable->name));
        return;
    }
    vectors->host = NULL;
    conform_datamodel_close(&vectors->datamodel);
    if (refusal.length) conform_result_bytes(result, "error", conform_keep(state, (ta_bytes){error, refusal.length}));
}

static void host_free(conform *state, conform_call *call, conform_result *result) {
    (void)result;
    if (host_of(state, call)) conform_vectors_close(&state->vectors);
}

typedef struct request_writing {
    ta_host *host;
    ta_bytes request;
} request_writing;

static ta_status request_write(void *context, ta_buffer *out) {
    request_writing *writing = (request_writing *)context;
    return ta_host_request(writing->host, writing->request, out);
}

/* Into a buffer of `capacity` bytes when the call gives one, else into one
 * grown until the reply fits. */
static void host_request(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    request_writing writing = {host, {NULL, 0}};
    int64_t capacity = -1;
    if (!host || !conform_required(call, "request") || !conform_bytes(call, "request", &writing.request) ||
        !conform_integer(call, "capacity", &capacity))
        return;
    ta_buffer reply = {NULL, 0, 0};
    ta_status status;
    if (capacity >= 0) {
        status = conform_buffer_reserve(&reply, (size_t)capacity) ? TA_OK : TA_ERROR_NO_MEMORY;
        reply.capacity = (size_t)capacity;
        if (status == TA_OK) status = ta_host_request(host, writing.request, &reply);
    } else {
        status = conform_write_growing(&(conform_writing){request_write, &writing}, &reply);
    }
    conform_result_integer(result, "status", status);
    conform_result_integer(result, "length", (int64_t)reply.length);
    if (status == TA_OK) conform_result_bytes(result, "reply", conform_keep(state, (ta_bytes){reply.pointer, reply.length}));
    conform_buffer_free(&reply);
}

/* ---- image.add, actor.spawn, actor.send, clock.advance, host.run ------------------ */

static void image_add(conform *state, conform_call *call, conform_result *result) {
    conform_vectors *vectors = &state->vectors;
    conform_sources sources;
    if (!host_of(state, call) || !conform_required(call, "charts") || !conform_sources_read(call, "charts", &sources))
        return;
    if (vectors->image_count == sizeof vectors->images / sizeof vectors->images[0]) {
        conform_fail(call, &(conform_failure){CONFORM_INVALID_PARAMS, "range", "params.charts"}, "too many images");
        return;
    }
    conform_image *image = &vectors->images[vectors->image_count];
    uint32_t segment = 0;
    ta_status status = conform_image_compile(
        &(conform_documents){{sources.items, sources.count}, {sources.items, sources.count}}, image);
    if (status == TA_OK) {
        vectors->image_count++;
        status = ta_host_add_image(vectors->host, &image->image, &segment);
    }
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_integer(result, "segment", segment);
}

static void actor_spawn(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    ta_string_view name = {"", 0};
    if (!host || !conform_required(call, "chart") || !conform_text(call, "chart", &name)) return;
    ta_host_chart_ref chart;
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    uint64_t session = 0;
    ta_status status = ta_host_chart(host, name, &chart);
    if (status == TA_OK) {
        payload_reading params = payload_read(state, call, "params");
        status = params.status;
        spawn.params = params.payload;
    }
    if (status == TA_OK) {
        spawn.segment = chart.segment;
        spawn.chart = chart.chart;
        status = ta_host_spawn(host, &spawn, &session);
        if (status != TA_OK) conform_datamodel_release(&state->vectors.datamodel, spawn.params);
    }
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_integer(result, "session", (int64_t)session);
}

static void actor_send(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    int64_t session = 0;
    if (!host || !conform_required(call, "session") || !conform_required(call, "event") ||
        !conform_integer(call, "session", &session) || !conform_text(call, "event", &event.name))
        return;
    event.session = (uint64_t)session;
    payload_reading data = payload_read(state, call, "data");
    ta_status status = data.status;
    event.data = data.payload;
    if (status == TA_OK) {
        status = ta_host_send(host, &event);
        if (status != TA_OK) conform_datamodel_release(&state->vectors.datamodel, event.data);
    }
    conform_result_integer(result, "status", status);
}

static void clock_advance(conform *state, conform_call *call, conform_result *result) {
    (void)result;
    ta_host *host = host_of(state, call);
    int64_t monotonic = 0, wall = 0;
    if (!host || !conform_integer(call, "monotonic", &monotonic) || !conform_integer(call, "wallNs", &wall)) return;
    ta_host_advance_time(host, (ta_host_time){(ta_time)monotonic, (uint64_t)wall});
}

/* Runs while the host yields or its effects are full; each PEER and
 * PEER_ERROR effect written as an envelope, in order. */
static void host_run(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    if (!host) return;
    static ta_bytes envelopes[64];
    uint32_t count = 0;
    ta_run run;
    int rounds = 0;
    do {
        run = ta_host_run(host, 1000000);
        const ta_host_effect *effects;
        uint32_t effect_count = ta_host_effects(host, &effects);
        for (uint32_t i = 0; i < effect_count; i++) {
            if (effects[i].kind != TA_HOST_EFFECT_PEER && effects[i].kind != TA_HOST_EFFECT_PEER_ERROR) continue;
            static unsigned char bytes[1 << 16];
            ta_buffer out = {bytes, sizeof bytes, 0};
            ta_status status = ta_host_envelope(host, &effects[i], &out);
            if (status != TA_OK) fprintf(stderr, "ta-conform: ta_host_envelope refused an effect: %d\n", (int)status);
            else if (count < sizeof envelopes / sizeof envelopes[0])
                envelopes[count++] = conform_keep(state, (ta_bytes){bytes, out.length});
        }
        ta_host_effects_ack(host);
    } while ((run.status == TA_RUN_YIELDED || run.status == TA_RUN_EFFECTS_FULL) && ++rounds < 1000);
    conform_result_integer(result, "status", run.status);
    conform_result_list(result, "envelopes", (conform_bytes_list){envelopes, count});
}

/* ---- peer.add, envelope.read, envelope.deliver ------------------------------------- */

static void peer_add(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    ta_peer_config peer;
    memset(&peer, 0, sizeof peer);
    peer.size = sizeof peer;
    int trusted = 0;
    if (!host || !conform_required(call, "id") || !conform_text(call, "id", &peer.id) ||
        !conform_boolean(call, "trusted", &trusted))
        return;
    peer.flags = trusted ? TA_PEER_TRUSTED : 0;
    uint32_t handle = 0;
    ta_status status = ta_host_peer_add(host, &peer, &handle);
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_integer(result, "peer", handle);
}

/* An envelope (or an error envelope) read back into a PEER (PEER_ERROR)
 * effect, so that ta_host_envelope writes it again. */
typedef struct envelope_writing {
    ta_host *host;
    const ta_envelope *envelope; /* or */
    const ta_peer_error *error;
} envelope_writing;

static ta_status envelope_write(void *context, ta_buffer *out) {
    const envelope_writing *writing = (const envelope_writing *)context;
    ta_host_effect effect;
    memset(&effect, 0, sizeof effect);
    effect.size = sizeof effect;
    if (writing->error) {
        effect.kind = TA_HOST_EFFECT_PEER_ERROR;
        effect.token = writing->error->reason;
        effect.auxiliary = writing->error->origin;
        effect.msgid = writing->error->msgid;
        effect.sendid = writing->error->sendid;
        return ta_host_envelope(writing->host, &effect, out);
    }
    const ta_envelope *envelope = writing->envelope;
    effect.kind = TA_HOST_EFFECT_PEER;
    effect.target = envelope->target;
    effect.event = envelope->name;
    effect.sendid = envelope->sendid;
    effect.auxiliary = envelope->origin;
    effect.origintype = envelope->origintype;
    effect.msgid = envelope->msgid;
    effect.deduplication_key = envelope->deduplication_key;
    effect.flags = envelope->hops;
    effect.wire = envelope->wire;
    effect.datamodel = TA_NIL;
    if (envelope->traceparent.length && ta_traceparent_parse(envelope->traceparent, &effect.trace) != TA_OK)
        return TA_ERROR_INVALID;
    return ta_host_envelope(writing->host, &effect, out);
}

static void envelope_read(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    ta_bytes cbor = {NULL, 0};
    int is_error = 0;
    if (!host || !conform_required(call, "cbor") || !conform_bytes(call, "cbor", &cbor) ||
        !conform_boolean(call, "error", &is_error))
        return;
    ta_envelope envelope;
    ta_peer_error error;
    ta_status status = is_error ? ta_host_peer_error_read(cbor, &error) : ta_host_envelope_read(host, cbor, &envelope);
    ta_buffer back = {NULL, 0, 0};
    envelope_writing writing = {host, is_error ? NULL : &envelope, is_error ? &error : NULL};
    if (status == TA_OK) status = conform_write_growing(&(conform_writing){envelope_write, &writing}, &back);
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_bytes(result, "back", conform_keep(state, (ta_bytes){back.pointer, back.length}));
    conform_buffer_free(&back);
}

static void envelope_deliver(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    ta_bytes cbor = {NULL, 0};
    int64_t peer = 0;
    if (!host || !conform_required(call, "peer") || !conform_required(call, "cbor") ||
        !conform_integer(call, "peer", &peer) || !conform_bytes(call, "cbor", &cbor))
        return;
    ta_envelope envelope;
    memset(&envelope, 0, sizeof envelope);
    ta_status status = ta_host_envelope_read(host, cbor, &envelope);
    /* A read that failed with envelope.size set is an envelope whose data is
     * not one item: ta_host_deliver refuses it as malformed and answers its
     * sender (host.h, ta_host_envelope_read). With size 0 there is nobody to
     * answer. */
    int32_t route = status == TA_OK || envelope.size ? ta_host_deliver(host, (uint32_t)peer, &envelope) : status;
    conform_result_integer(result, "status", route < 0 ? route : TA_OK);
    if (route >= 0) conform_result_integer(result, "route", route);
}

/* ---- value.encode ------------------------------------------------------------------- */

/* quickjs only: an expression that throws is TA_ERROR_EXECUTION. */
static void value_encode(conform *state, conform_call *call, conform_result *result) {
    ta_string_view expression = {"", 0};
    if (!host_of(state, call) || !conform_required(call, "expression") ||
        !conform_text(call, "expression", &expression))
        return;
    conform_datamodel *datamodel = &state->vectors.datamodel;
    if (!datamodel->ecmascript) {
        conform_result_integer(result, "status", TA_ERROR_UNSUPPORTED);
        return;
    }
    ta_payload payload = ta_datamodel_ecmascript_payload_eval(datamodel->ecmascript, 0, expression);
    ta_buffer cbor = {NULL, 0, 0};
    ta_status status = payload ? conform_datamodel_write(datamodel, payload, &cbor) : TA_ERROR_EXECUTION;
    conform_datamodel_release(datamodel, payload);
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_bytes(result, "cbor", conform_keep(state, (ta_bytes){cbor.pointer, cbor.length}));
    conform_buffer_free(&cbor);
}

static const char *const create_params[] = {"config", "datamodel", "realm", NULL};
static const char *const no_params[] = {NULL};
static const char *const request_params[] = {"request", "capacity", NULL};
static const char *const image_params[] = {"charts", NULL};
static const char *const spawn_params[] = {"chart", "params", NULL};
static const char *const send_params[] = {"session", "event", "data", NULL};
static const char *const clock_params[] = {"monotonic", "wallNs", NULL};
static const char *const peer_params[] = {"id", "trusted", NULL};
static const char *const read_params[] = {"cbor", "error", NULL};
static const char *const deliver_params[] = {"peer", "cbor", NULL};
static const char *const encode_params[] = {"expression", NULL};

const conform_method conform_vector_methods[] = {
    {"host.create", host_create, create_params},
    {"host.free", host_free, no_params},
    {"host.request", host_request, request_params},
    {"image.add", image_add, image_params},
    {"actor.spawn", actor_spawn, spawn_params},
    {"actor.send", actor_send, send_params},
    {"clock.advance", clock_advance, clock_params},
    {"host.run", host_run, no_params},
    {"peer.add", peer_add, peer_params},
    {"envelope.read", envelope_read, read_params},
    {"envelope.deliver", envelope_deliver, deliver_params},
    {"value.encode", value_encode, encode_params},
    {NULL, NULL, NULL},
};
