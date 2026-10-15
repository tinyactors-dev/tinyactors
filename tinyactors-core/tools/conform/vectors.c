/* The methods the protocol vectors use (driver.md, from host.create on):
 * one host and its datamodel between calls, each method one or two core
 * calls. The runner builds the vectors' setups with them
 * (../tinyactors-test/lib/runner.rb, `setup`). */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/runtime.h>
#include <tinyactors/trace.h>

void conform_vectors_close(conform_vectors *vectors) {
    if (vectors->host) ta_host_free(vectors->host);
    vectors->host = NULL;
    conform_datamodel_close(&vectors->datamodel);
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

/* ---- host.create, host.free, host.post ------------------------------------------- */

/* The C library's allocator, refusing every block while `refusing` is set. */
static void *vectors_alloc(void *context, size_t size, size_t align) {
    const conform_vectors *vectors = (const conform_vectors *)context;
    if (vectors->refusing) return NULL;
    ta_allocator heap = ta_runtime_malloc();
    return heap.alloc(heap.context, size, align);
}

static void vectors_free(void *context, void *pointer, size_t size) {
    (void)context;
    ta_allocator heap = ta_runtime_malloc();
    heap.free(heap.context, pointer, size);
}

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
    environment.allocator = (ta_allocator){vectors, vectors_alloc, vectors_free};
    vectors->refusing = 0;
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

/* The batch's NOTIFICATION bytes, copied in order into `list` (up to 64). */
typedef struct kept_list {
    ta_bytes items[64];
    uint32_t count;
} kept_list;

static void notifications_keep(conform *state, kept_list *list) {
    ta_host *host = state->vectors.host;
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(host, &effects);
    for (uint32_t i = 0; i < count; i++)
        if (effects[i].kind == TA_HOST_EFFECT_NOTIFICATION &&
            list->count < sizeof list->items / sizeof list->items[0])
            list->items[list->count++] = conform_keep(state, ta_host_effect_bytes(host, i, TA_EFFECT_FIELD_WIRE));
}

/* Post before taking: refusal leaves pending effects untouched. Success
 * copies the reply and notifications and acknowledges the entire batch. With `refuse`, only the host allocator
 * refuses blocks during the post. */
static void host_post(conform *state, conform_call *call, conform_result *result) {
    conform_vectors *vectors = &state->vectors;
    ta_host *host = host_of(state, call);
    ta_bytes message = {NULL, 0};
    int refuse = 0;
    if (!host || !conform_required(call, "message") || !conform_bytes(call, "message", &message) ||
        !conform_boolean(call, "refuse", &refuse))
        return;
    const ta_host_effect *effects;
    vectors->refusing = refuse;
    ta_status status = ta_host_post(host, message);
    vectors->refusing = 0;
    conform_result_integer(result, "status", status);
    if (status != TA_OK) return;
    static kept_list notifications;
    notifications.count = 0;
    uint32_t count;
    while ((count = ta_host_effects(host, &effects)) != 0) {
        notifications_keep(state, &notifications);
        if (effects[count - 1].kind == TA_HOST_EFFECT_REPLY)
            conform_result_bytes(result, "reply",
                                 conform_keep(state, ta_host_effect_bytes(host, count - 1, TA_EFFECT_FIELD_WIRE)));
        ta_host_effects_ack(host);
    }
    conform_result_list(result, "notifications", (conform_bytes_list){notifications.items, notifications.count});
}

/* ---- image.add, actor.spawn, actor.send, clock.advance, host.run ------------------ */

/* The host copies the image: it is freed once added. */
static void image_add(conform *state, conform_call *call, conform_result *result) {
    conform_vectors *vectors = &state->vectors;
    conform_sources sources;
    if (!host_of(state, call) || !conform_required(call, "charts") || !conform_sources_read(call, "charts", &sources))
        return;
    conform_image image;
    uint32_t segment = 0;
    ta_status status = conform_image_compile(
        &(conform_documents){{sources.items, sources.count}, {sources.items, sources.count}}, &image);
    if (status == TA_OK) {
        status = ta_host_add_image(vectors->host, image.bytes, &segment);
        conform_image_free(&image);
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
    if (!conform_text(call, "origin", &event.origin)) return;
    payload_reading data = payload_read(state, call, "data");
    ta_bytes cbor = {NULL, 0}; /* the data as the value table's CBOR, read by the datamodel */
    if (conform_param(call, "cbor") != TA_NIL) {
        if (!conform_bytes(call, "cbor", &cbor)) return;
        data.status = conform_datamodel_read(&state->vectors.datamodel, cbor, &data.payload);
    }
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

/* The sends to the remote I/O processor of the batch ta_host_effects
 * publishes, each kept (its data written by the sender's datamodel), and
 * its NOTIFICATIONs; then the batch is acknowledged. */
static ta_string_view text_of(ta_bytes bytes) {
    return (ta_string_view){(const char *)bytes.pointer, (uint32_t)bytes.length};
}

typedef struct send_list {
    conform_send items[64];
    uint32_t count;
} send_list;

static void batch_take(conform *state, send_list *sends, kept_list *notifications) {
    ta_host *host = state->vectors.host;
    const ta_host_effect *effects;
    uint32_t effect_count = ta_host_effects(host, &effects);
    for (uint32_t i = 0; i < effect_count; i++) {
        if (effects[i].kind != TA_EFFECT_SEND || effects[i].io_processor != 0 ||
            sends->count == sizeof sends->items / sizeof sends->items[0])
            continue; /* the vectors' configs have one I/O processor, the remote one */
        conform_send *send = &sends->items[sends->count++];
        send->target = text_of(conform_keep(state, (ta_bytes){effects[i].target.pointer, effects[i].target.length}));
        send->event = text_of(conform_keep(state, (ta_bytes){effects[i].event.pointer, effects[i].event.length}));
        ta_string_view origin = ta_host_effect_text(host, i, TA_EFFECT_FIELD_ORIGIN);
        send->origin = text_of(conform_keep(state, (ta_bytes){origin.pointer, origin.length}));
        send->data = (ta_bytes){NULL, 0};
        ta_bytes data = {NULL, 0}; /* the datamodel's view, until its next call: kept */
        if (effects[i].payload && conform_datamodel_write(&state->vectors.datamodel, effects[i].payload, &data) == TA_OK)
            send->data = conform_keep(state, data);
    }
    notifications_keep(state, notifications);
    ta_host_effects_ack(host);
}

/* Pending effects first, then runs, acknowledging each batch; sends and
 * notifications kept in order. */
static void host_run(conform *state, conform_call *call, conform_result *result) {
    ta_host *host = host_of(state, call);
    if (!host) return;
    static send_list sends;
    static kept_list notifications;
    sends.count = notifications.count = 0;
    batch_take(state, &sends, &notifications);
    ta_run run;
    int rounds = 0;
    do {
        run = ta_host_run(host, 1000000);
        batch_take(state, &sends, &notifications);
    } while ((run.status == TA_RUN_YIELDED || run.status == TA_RUN_EFFECTS_FULL) && ++rounds < 1000);
    conform_result_integer(result, "status", run.status);
    conform_result_sends(result, "sends", (conform_sends){sends.items, sends.count});
    conform_result_list(result, "notifications", (conform_bytes_list){notifications.items, notifications.count});
}

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
    ta_bytes cbor = {NULL, 0};
    ta_status status = payload ? conform_datamodel_write(datamodel, payload, &cbor) : TA_ERROR_EXECUTION;
    if (status == TA_OK) cbor = conform_keep(state, cbor); /* before the next call on the datamodel */
    conform_datamodel_release(datamodel, payload);
    conform_result_integer(result, "status", status);
    if (status == TA_OK) conform_result_bytes(result, "cbor", cbor);
}

static const char *const create_params[] = {"config", "datamodel", "realm", NULL};
static const char *const no_params[] = {NULL};
static const char *const post_params[] = {"message", "refuse", NULL};
static const char *const image_params[] = {"charts", NULL};
static const char *const spawn_params[] = {"chart", "params", NULL};
static const char *const send_params[] = {"session", "event", "data", "cbor", "origin", NULL};
static const char *const clock_params[] = {"monotonic", "wallNs", NULL};
static const char *const encode_params[] = {"expression", NULL};

const conform_method conform_vector_methods[] = {
    {"host.create", host_create, create_params},
    {"host.free", host_free, no_params},
    {"host.post", host_post, post_params},
    {"image.add", image_add, image_params},
    {"actor.spawn", actor_spawn, spawn_params},
    {"actor.send", actor_send, send_params},
    {"clock.advance", clock_advance, clock_params},
    {"host.run", host_run, no_params},
    {"value.encode", value_encode, encode_params},
    {NULL, NULL, NULL},
};
