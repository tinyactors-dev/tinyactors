/* demo.c — the scripted order every language's example runs (demo.h). */
#include "demo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/cbor.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

typedef struct demo {
    ta_host *host;
    ta_datamodel_binding binding;
    int done;
} demo;

static void print_payload(demo *d, const char *what, ta_payload payload) {
    ta_bytes wire;
    char text[2048];
    ta_buffer out = {(unsigned char *)text, sizeof text - 1, 0};
    if (!payload || d->binding.vtable->to_wire(d->binding.datamodel, payload, &wire) != TA_OK ||
        ta_cbor_diagnostic(wire, &out) != TA_OK) {
        printf("  %s: (none)\n", what);
        return;
    }
    printf("  %s: %.*s\n", what, (int)out.length, text);
}

static void drain(demo *d) {
    const ta_host_effect *effects;
    uint32_t count;
    while ((count = ta_host_effects(d->host, &effects)) > 0) {
        for (uint32_t i = 0; i < count; i++) {
            const ta_host_effect *e = &effects[i];
            if (e->kind == TA_EFFECT_LOG)
                printf("  log %.*s: %.*s\n", (int)e->auxiliary.length, e->auxiliary.pointer, (int)e->event.length,
                       e->event.pointer);
            else if (e->kind == TA_EFFECT_DONE) {
                print_payload(d, "done", e->payload);
                d->done = 1;
            } else if (e->kind == TA_EFFECT_ERROR)
                printf("  error: the actor failed (flags %u)\n", e->flags);
        }
        ta_host_effects_ack(d->host);
    }
}

static void run_idle(demo *d) {
    for (;;) {
        drain(d);
        ta_host_advance_time(d->host, (ta_host_time){ta_runtime_now_ns(), 0});
        ta_run run = ta_host_run(d->host, 100000);
        drain(d);
        if (run.status == TA_RUN_IDLE) return;
        if (run.status == TA_RUN_FAULT || run.status == TA_RUN_EXHAUSTED) {
            fprintf(stderr, "run: %s\n", ta_status_name((ta_status)run.detail));
            exit(1);
        }
    }
}

typedef struct field {
    const char *key, *text; /* text, or the number */
    uint64_t number;
} field;

typedef struct demo_event {
    const char *name;
    const field *fields; /* ended by a field without a key */
} demo_event;

static void send(demo *d, uint64_t session, const demo_event *event) {
    const char *name = event->name;
    const field *fields = event->fields;
    unsigned char room[256];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
    uint32_t count = 0;
    while (fields[count].key) count++;
    ta_cbor_write_map(&w, count);
    for (uint32_t i = 0; i < count; i++) {
        ta_cbor_write_text(&w, ta_string(fields[i].key));
        if (fields[i].text) ta_cbor_write_text(&w, ta_string(fields[i].text));
        else ta_cbor_write_uint(&w, fields[i].number);
    }
    ta_bytes data;
    ta_cbor_writer_bytes(&w, &data);
    ta_event_in in;
    memset(&in, 0, sizeof in);
    in.size = sizeof in;
    in.session = session;
    in.name = ta_string(name);
    if (d->binding.vtable->from_wire(d->binding.datamodel, data, &in.data) != TA_OK || ta_host_send(d->host, &in) != TA_OK) {
        fprintf(stderr, "send %s refused\n", name);
        exit(1);
    }
    char text[512];
    ta_buffer out = {(unsigned char *)text, sizeof text, 0};
    ta_cbor_diagnostic(data, &out);
    printf("> %s %.*s\n", name, (int)out.length, text);
    run_idle(d);
}

/* hibernate the actor (its blob copied before the ack), then wake it */
static void hibernate_and_wake(demo *d, uint64_t session) {
    ta_bytes blob;
    ta_status status = ta_host_hibernate(d->host, session, &blob);
    if (status != TA_OK) {
        fprintf(stderr, "hibernate: %s\n", ta_status_name(status));
        exit(1);
    }
    unsigned char *copy = malloc(blob.length);
    memcpy(copy, blob.pointer, blob.length);
    size_t length = blob.length;
    ta_host_effects_ack(d->host); /* the batch the output opened */
    printf("> hibernated: a blob of %zu bytes; the session holds nothing in the language now\n", length);
    status = ta_host_wake(d->host, session, (ta_bytes){copy, length});
    free(copy);
    if (status != TA_OK) {
        fprintf(stderr, "wake: %s\n", ta_status_name(status));
        exit(1);
    }
    printf("> woken from the blob\n");
    run_idle(d);
}

int ta_demo_order(const char *chart_path, ta_datamodel_binding binding) {
    setvbuf(stdout, NULL, _IOLBF, 0); /* a language may exit without flushing C's stdout */
    demo d = {0, binding, 0};
    unsigned char room[128];
    ta_cbor_writer w;
    ta_cbor_writer_init(&w, (ta_buffer){room, sizeof room, 0}, 0);
    ta_cbor_write_map(&w, 1);
    ta_cbor_write_text(&w, ta_string("systemID"));
    ta_cbor_write_text(&w, ta_string("shop"));
    ta_bytes config;
    ta_cbor_writer_bytes(&w, &config);
    ta_host_environment environment;
    memset(&environment, 0, sizeof environment);
    environment.size = sizeof environment;
    environment.datamodel_count = 1;
    environment.datamodels = &d.binding;
    environment.allocator = ta_runtime_malloc();
    if (ta_host_create(config, &environment, &d.host) != TA_OK) return fprintf(stderr, "host refused\n"), 1;
    ta_compile_result *compiled = NULL;
    if (ta_runtime_compile_file(chart_path, 0, &compiled) != TA_OK) return fprintf(stderr, "compile failed\n"), 1;
    ta_string_view image = ta_compile_image(compiled);
    uint32_t segment;
    ta_status status = ta_host_add_image(d.host, (ta_bytes){image.pointer, image.length}, &segment);
    ta_compile_free(compiled);
    if (status != TA_OK) return fprintf(stderr, "add_image: %s\n", ta_status_name(status)), 1;
    ta_host_chart_ref ref;
    if (ta_host_chart(d.host, ta_string("order"), &ref) != TA_OK) return 1;
    ta_spawn spawn = {ref.segment, ref.chart, {NULL, 0}, 0, {NULL, 0}};
    uint64_t session;
    status = ta_host_spawn(d.host, &spawn, &session);
    if (status != TA_OK) return fprintf(stderr, "spawn: %s\n", ta_status_name(status)), 1;
    printf("> spawned order, session %llu\n", (unsigned long long)session);
    run_idle(&d);
    send(&d, session, &(demo_event){"add", (field[]){{"sku", "tea", 0}, {"quantity", 0, 2}, {0, 0, 0}}});
    send(&d, session, &(demo_event){"add", (field[]){{"sku", "mug", 0}, {"quantity", 0, 1}, {0, 0, 0}}});
    send(&d, session, &(demo_event){"add", (field[]){{"sku", "cake", 0}, {"quantity", 0, 1}, {0, 0, 0}}});
    hibernate_and_wake(&d, session);
    send(&d, session, &(demo_event){"place", (field[]){{0, 0, 0}}});
    send(&d, session, &(demo_event){"address", (field[]){{"country", "DE", 0}, {0, 0, 0}}});
    send(&d, session, &(demo_event){"place", (field[]){{0, 0, 0}}});
    ta_host_free(d.host);
    return d.done ? 0 : 1;
}
