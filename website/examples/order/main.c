/* cc main.c -ltinyactors_datamodel_ecmascript -ltinyactors_runtime -ltinyactors -lm */
#include <stdio.h>
#include <string.h>
#include <tinyactors.h>
#include <tinyactors_datamodel_ecmascript.h>
#include <tinyactors_runtime.h>

typedef struct program {
    ta_host *host;
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding datamodel; /* its release() gives back a payload we still own */
} program;

/* A number in an object the chart sent ({"amount": 42}): `key` in, `value` out. */
typedef struct member {
    ta_payload payload;
    const char *key;
    uint64_t value;
} member;

static ta_status read_member(program *p, member *m) {
    ta_bytes cbor;
    ta_status status = ta_datamodel_ecmascript_payload_to_cbor(p->ecmascript, m->payload, &cbor);
    if (status != TA_OK) return status;
    ta_cbor_token tokens[16];
    ta_cbor_document document = {cbor, tokens, 16, 0};
    status = ta_cbor_decode(&document); /* TA_ERROR_NO_MEMORY for an object of more tokens */
    if (status != TA_OK) return status;
    uint32_t at = ta_cbor_member(&document, 0, ta_string(m->key));
    if (at == TA_NIL || tokens[at].kind != TA_CBOR_UINT) return TA_ERROR_INVALID;
    m->value = tokens[at].value;
    return TA_OK;
}

static void handle(program *p, uint32_t i) {
    ta_host *host = p->host;
    ta_payload payload = ta_host_effect_number(host, i, TA_EFFECT_FIELD_PAYLOAD);
    switch (ta_host_effect_number(host, i, TA_EFFECT_FIELD_KIND)) {
    case TA_EFFECT_SEND: { /* the chart's <send type="payments"> */
        member amount = {payload, "amount", 0};
        if (read_member(p, &amount) != TA_OK) break;
        printf("charge %llu\n", (unsigned long long)amount.value);
        ta_event_in charged = {.size = sizeof charged, .name = ta_string("charged")};
        charged.session = ta_host_effect_number(host, i, TA_EFFECT_FIELD_SESSION);
        charged.sendid = ta_host_effect_text(host, i, TA_EFFECT_FIELD_SENDID);
        ta_host_send(host, &charged); /* an event back to the order */
        break;
    }
    case TA_EFFECT_DONE: /* the order reached <final id="paid"> */
    {
        member total = {payload, "total", 0};
        if (read_member(p, &total) == TA_OK) printf("paid %llu\n", (unsigned long long)total.value);
        break;
    }
    default:
        break;
    }
}

static void drain(program *p) {
    const ta_host_effect *effects;
    uint32_t count;
    while ((count = ta_host_effects(p->host, &effects)) > 0) {
        for (uint32_t i = 0; i < count; i++) handle(p, i);
        ta_host_effects_ack(p->host); /* what the batch handed out ends here */
    }
}

/* An event for the order, its data an ECMAScript expression (or none). */
typedef struct event_for {
    uint64_t session;
    const char *name;
    const char *data;
} event_for;

static ta_status deliver(program *p, event_for e) {
    ta_event_in event = {.size = sizeof event, .session = e.session, .name = ta_string(e.name)};
    if (e.data) event.data = ta_datamodel_ecmascript_payload_eval(p->ecmascript, 0, ta_string(e.data));
    ta_status status = ta_host_send(p->host, &event);
    if (status != TA_OK && event.data) /* a refused event's data is still ours */
        p->datamodel.vtable->release(p->datamodel.datamodel, event.data);
    return status;
}

int main(void) {
    ta_allocator allocator = ta_runtime_malloc();
    ta_bytes chart;
    if (ta_runtime_load_file("order.scxml", &chart) != TA_OK) return 1;
    ta_compile_options options = {.size = sizeof options, .alloc = allocator};
    ta_source source = {ta_string("order.scxml"), {(const char *)chart.pointer, (uint32_t)chart.length}};
    ta_compile_result *compiled = NULL;
    if (ta_compile_charts(&options, (ta_source_list){&source, 1}, &compiled) != TA_OK) return 1;

    /* the config, as CBOR: {"systemID": "shop", "ioProcessors": [{"type": "payments"}]} */
    unsigned char room[128];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, &allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, ta_string("shop"));
    ta_cbor_write_text(&writer, ta_string("ioProcessors"));
    ta_cbor_write_array(&writer, 1);
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("type"));
    ta_cbor_write_text(&writer, ta_string("payments"));
    ta_bytes config;
    if (ta_cbor_writer_bytes(&writer, &config) != TA_OK) return 1;

    program p = {NULL, ta_datamodel_ecmascript_new(NULL), {0}};
    p.datamodel = ta_datamodel_ecmascript_binding(p.ecmascript);
    ta_host_environment environment = {.size = sizeof environment, .datamodel_count = 1,
                                       .datamodels = &p.datamodel, .allocator = allocator};
    if (ta_host_create(config, &environment, &p.host) != TA_OK) return 1;
    ta_cbor_writer_free(&writer); /* read during the call */
    ta_string_view image = ta_compile_image(compiled);
    ta_host_add_image(p.host, (ta_bytes){(const unsigned char *)image.pointer, image.length}, NULL);
    ta_compile_free(compiled);
    ta_runtime_free_file(chart);

    ta_host_chart_ref order;
    ta_host_chart(p.host, ta_string("order"), &order);
    ta_spawn spawn = {.segment = order.segment, .chart = order.chart};
    uint64_t session = 0;
    ta_host_spawn(p.host, &spawn, &session);
    deliver(&p, (event_for){session, "add", "({price: 30})"});
    deliver(&p, (event_for){session, "add", "({price: 12})"});
    deliver(&p, (event_for){session, "checkout", NULL});

    for (;;) { /* run until idle; a long-lived program arms a timer at next_deadline */
        drain(&p);
        ta_run run = ta_host_run(p.host, 10000);
        drain(&p);
        if (run.status == TA_RUN_IDLE || run.status == TA_RUN_FAULT) break;
    }
    ta_host_free(p.host);
    ta_datamodel_ecmascript_free(p.ecmascript);
    return 0;
}
