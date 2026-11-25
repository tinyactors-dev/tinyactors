#define _POSIX_C_SOURCE 200809L /* nanosleep */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/* The config, as CBOR: {"systemID": system, "ioProcessors": [{"type": io_processor}]} */
static ta_status open_host(host *h, const host_options *options, ta_allocator *allocator) {
    unsigned char room[128];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, allocator);
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, ta_string(options->system));
    ta_cbor_write_text(&writer, ta_string("ioProcessors"));
    ta_cbor_write_array(&writer, 1);
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("type"));
    ta_cbor_write_text(&writer, ta_string(options->io_processor));
    ta_bytes config;
    ta_status status = ta_cbor_writer_bytes(&writer, &config);
    ta_host_environment environment = {.size = sizeof environment, .datamodel_count = 1,
                                       .datamodels = &h->datamodel, .allocator = *allocator};
    if (status == TA_OK) status = ta_host_create(config, &environment, &h->host);
    ta_cbor_writer_free(&writer); /* read during the call */
    return status;
}

/* The chart's file, compiled into an image the host copies. */
static ta_status add_chart(host *h, const char *path, ta_allocator allocator) {
    ta_bytes text;
    ta_status status = ta_runtime_load_file(path, &text);
    if (status != TA_OK) return status;
    ta_compile_options options = {.size = sizeof options, .alloc = allocator};
    ta_source source = {ta_string(path), {(const char *)text.pointer, (uint32_t)text.length}};
    ta_compile_result *compiled = NULL;
    status = ta_compile_charts(&options, (ta_source_list){&source, 1}, &compiled);
    if (status == TA_OK) {
        ta_string_view image = ta_compile_image(compiled);
        uint32_t segment = 0;
        status = ta_host_add_image(h->host, (ta_bytes){(const unsigned char *)image.pointer, image.length}, &segment);
    }
    ta_compile_free(compiled);
    ta_runtime_free_file(text);
    return status;
}

ta_status host_open(host *h, const host_options *options) {
    ta_allocator allocator = ta_runtime_malloc();
    h->ecmascript = ta_datamodel_ecmascript_new(NULL);
    h->datamodel = ta_datamodel_ecmascript_binding(h->ecmascript);
    ta_status status = open_host(h, options, &allocator);
    return status == TA_OK ? add_chart(h, options->chart, allocator) : status;
}

uint64_t host_spawn(host *h, const char *chart) {
    ta_host_chart_ref ref;
    if (ta_host_chart(h->host, ta_string(chart), &ref) != TA_OK) return 0;
    ta_spawn spawn = {.segment = ref.segment, .chart = ref.chart};
    uint64_t session = 0;
    return ta_host_spawn(h->host, &spawn, &session) == TA_OK ? session : 0;
}

ta_status host_send(host *h, host_event e) {
    ta_event_in event = {.size = sizeof event, .session = e.session, .name = ta_string(e.name)};
    if (e.data) event.data = ta_datamodel_ecmascript_payload_eval(h->ecmascript, 0, ta_string(e.data));
    ta_status status = ta_host_send(h->host, &event);
    if (status != TA_OK && event.data) /* a refused event's data is still ours */
        h->datamodel.vtable->release(h->datamodel.datamodel, event.data);
    return status;
}

/* Every effect handed out, each batch acknowledged after its handler saw it. */
static void drain(host *h, host_handler *handle) {
    const ta_host_effect *effects;
    uint32_t count;
    while ((count = ta_host_effects(h->host, &effects)) > 0) {
        for (uint32_t i = 0; i < count; i++) handle(h, i);
        ta_host_effects_ack(h->host); /* what the batch handed out ends here */
    }
}

/* Sleeps until the host's clock, in nanoseconds (ta_runtime_now_ns), reads `deadline`. */
static void sleep_until(ta_time deadline) {
    ta_time now = ta_runtime_now_ns();
    if (deadline <= now) return;
    ta_time wait = deadline - now;
    struct timespec duration = {(time_t)(wait / 1000000000u), (long)(wait % 1000000000u)};
    nanosleep(&duration, NULL);
}

void host_run(host *h, host_handler *handle) {
    for (;;) {
        drain(h, handle);
        ta_host_advance_time(h->host, (ta_system_time){ta_runtime_now_ns(), 0}); /* timers fire as it passes */
        ta_run run = ta_host_run(h->host, 10000);
        drain(h, handle);
        if (run.status == TA_RUN_FAULT) return;
        if (run.status != TA_RUN_IDLE) continue; /* more to run now */
        if (run.next_deadline == UINT64_MAX) return; /* nothing left, not even a timer */
        sleep_until(run.next_deadline); /* a program with an event loop arms a timer instead */
    }
}

ta_status host_answer(host *h, uint32_t effect, const char *name) {
    ta_event_in event = {.size = sizeof event, .name = ta_string(name)};
    event.session = ta_host_effect_number(h->host, effect, TA_EFFECT_FIELD_SESSION);
    event.sendid = ta_host_effect_text(h->host, effect, TA_EFFECT_FIELD_SENDID);
    return ta_host_send(h->host, &event);
}

ta_status host_fail(host *h, uint32_t effect) {
    ta_event_in event = {.size = sizeof event, .flags = TA_EVENT_IN_ERROR_COMMUNICATION};
    event.session = ta_host_effect_number(h->host, effect, TA_EFFECT_FIELD_SESSION);
    event.sendid = ta_host_effect_text(h->host, effect, TA_EFFECT_FIELD_SENDID);
    return ta_host_send(h->host, &event);
}

void host_print(host *h, const char *label, uint32_t effect) {
    ta_payload payload = ta_host_effect_number(h->host, effect, TA_EFFECT_FIELD_PAYLOAD);
    ta_bytes cbor;
    if (ta_datamodel_ecmascript_payload_to_cbor(h->ecmascript, payload, &cbor) != TA_OK) return;
    char room[256];
    ta_buffer json = {room, sizeof room, 0};
    ta_status status = ta_cbor_diagnostic(cbor, &json); /* CBOR as text: JSON for these values */
    if (status == TA_ERROR_NO_MEMORY) { /* json.length is the room it needs */
        json = (ta_buffer){malloc(json.length), json.length, 0};
        status = json.pointer ? ta_cbor_diagnostic(cbor, &json) : TA_ERROR_NO_MEMORY;
    }
    if (status == TA_OK) printf("%s %.*s\n", label, (int)json.length, (const char *)json.pointer);
    if (json.pointer != room) free(json.pointer);
}

void host_close(host *h) {
    ta_host_free(h->host);
    ta_datamodel_ecmascript_free(h->ecmascript); /* after every host that uses it */
}
