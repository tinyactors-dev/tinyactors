/* w3c.run (driver.md, "The W3C environment"): one W3C IRP test on a host
 * of its own, with one I/O processor, BasicHTTP, which ta-conform serves
 * as a loopback: a send to its location is written as an HTTP POST
 * (tinyactors/basic_http.h), read back as an incoming request and
 * delivered to the sender; a send it cannot write is answered with
 * error.communication. Time moves to the next deadline whenever the host
 * is idle. The test's charts log the outcome. */
#include "conform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <tinyactors/basic_http.h>

#define LOCATION "http://localhost:8080/scxml"

/* A send to the BasicHTTP processor, written while its effect was
 * published and delivered after the acknowledgement. */
typedef struct w3c_post {
    uint64_t session;
    int refused; /* answered with error.communication */
    char sendid[128];
    char message[8192];
    size_t length;
} w3c_post;

typedef struct w3c_test {
    ta_host *host;
    conform_datamodel datamodel;
    w3c_post posts[16];
    uint32_t post_count;
} w3c_test;

/* The host's config: docs/host-config.md, with BasicHTTP as processor 0. */
static ta_status config_write(void *context, ta_buffer *out) {
    (void)context;
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, *out, NULL);
    ta_cbor_write_map(&writer, 3);
    ta_cbor_write_text(&writer, ta_string("systemID"));
    ta_cbor_write_text(&writer, ta_string("w3c"));
    ta_cbor_write_text(&writer, ta_string("system"));
    ta_cbor_write_map(&writer, 3);
    ta_cbor_write_text(&writer, ta_string("ticksPerSecond"));
    ta_cbor_write_uint(&writer, 1000);
    ta_cbor_write_text(&writer, ta_string("maxEvents"));
    ta_cbor_write_uint(&writer, 1024);
    ta_cbor_write_text(&writer, ta_string("maxActors"));
    ta_cbor_write_uint(&writer, 64);
    ta_cbor_write_text(&writer, ta_string("ioProcessors"));
    ta_cbor_write_array(&writer, 1);
    ta_cbor_write_map(&writer, 3);
    ta_cbor_write_text(&writer, ta_string("type"));
    ta_cbor_write_text(&writer, ta_string(TA_BASIC_HTTP_TYPE));
    ta_cbor_write_text(&writer, ta_string("location"));
    ta_cbor_write_text(&writer, ta_string(LOCATION));
    ta_cbor_write_text(&writer, ta_string("sync"));
    ta_cbor_write_simple(&writer, TA_CBOR_TRUE);
    return conform_writer_end(&writer, out);
}

/* ---- the loopback ----------------------------------------------------------------------- */

/* The form fields of a payload that is a map (each value as text: text as
 * it is, anything else in diagnostic notation), or its body when it is
 * text: what the POST carries (SCXML 1.0, C.2.1). */
typedef struct w3c_form {
    ta_http_field fields[32];
    uint32_t field_count;
    ta_string_view body;
    char store[4096];
    size_t used;
} w3c_form;

static ta_string_view form_keep(w3c_form *form, ta_string_view text) {
    if (form->used + text.length > sizeof form->store) text.length = (uint32_t)(sizeof form->store - form->used);
    memcpy(form->store + form->used, text.pointer, text.length);
    ta_string_view kept = {form->store + form->used, text.length};
    form->used += text.length;
    return kept;
}

/* The item at `at` of `document`, as diagnostic notation. */
static ta_string_view form_diagnostic(w3c_form *form, const ta_cbor_document *document, uint32_t at) {
    static unsigned char item[4096];
    static char text[4096];
    uint32_t end = ta_cbor_next(document, at);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){item, sizeof item, 0}, NULL);
    for (uint32_t i = at; i < end; i++)
        conform_write_token(&writer, document, i);
    ta_bytes bytes;
    ta_buffer out = {text, sizeof text, 0};
    if (ta_cbor_writer_bytes(&writer, &bytes) != TA_OK || ta_cbor_diagnostic(bytes, &out) != TA_OK)
        return (ta_string_view){"", 0};
    return form_keep(form, (ta_string_view){text, (uint32_t)out.length});
}

static void form_read(w3c_form *form, ta_bytes cbor) {
    static ta_cbor_token tokens[512];
    ta_cbor_document document = {cbor, tokens, 512, 0};
    if (ta_cbor_decode(&document) != TA_OK) return;
    if (tokens[0].kind == TA_CBOR_TEXT) {
        form->body = ta_cbor_string(&document, 0);
        return;
    }
    if (tokens[0].kind != TA_CBOR_MAP) return;
    uint32_t at = 1;
    for (uint32_t i = 0; i < tokens[0].count && form->field_count < 32; i++) {
        uint32_t value = ta_cbor_next(&document, at);
        ta_http_field *field = &form->fields[form->field_count++];
        field->name = form_keep(form, ta_cbor_string(&document, at));
        field->value = tokens[value].kind == TA_CBOR_TEXT ? form_keep(form, ta_cbor_string(&document, value))
                                                          : form_diagnostic(form, &document, value);
        at = ta_cbor_next(&document, value);
    }
}

/* A SEND effect to the BasicHTTP processor, written as a POST. */
static void post_write(w3c_test *test, const ta_host_effect *effect) {
    if (test->post_count == sizeof test->posts / sizeof test->posts[0]) return;
    w3c_post *post = &test->posts[test->post_count++];
    memset(post, 0, sizeof *post);
    post->session = effect->session;
    snprintf(post->sendid, sizeof post->sendid, "%.*s", (int)effect->sendid.length,
             effect->sendid.pointer ? effect->sendid.pointer : "");
    size_t location = strlen(LOCATION);
    if (effect->target.length < location || memcmp(effect->target.pointer, LOCATION, location)) {
        post->refused = 1; /* no target, or one that is not ours */
        return;
    }
    static w3c_form form;
    memset(&form, 0, sizeof form);
    ta_bytes cbor = {NULL, 0};
    if (effect->payload && conform_datamodel_write(&test->datamodel, effect->payload, &cbor) == TA_OK)
        form_read(&form, cbor);
    ta_http_post request = {effect->target, effect->event, form.fields, form.field_count, 0, form.body};
    ta_buffer out = {post->message, sizeof post->message, 0};
    if (ta_http_encode_post(&request, &out) != TA_OK) post->refused = 1;
    post->length = out.length;
}

/* The request's form fields, as a map of text to text: the event's data. */
static ta_status fields_write(const ta_http_request *request, ta_buffer *out) {
    ta_cbor_writer writer;
    conform_writer_begin(&writer, out);
    ta_cbor_write_map(&writer, request->field_count);
    for (uint32_t i = 0; i < request->field_count; i++) {
        ta_cbor_write_text(&writer, request->fields[i].name);
        ta_cbor_write_text(&writer, request->fields[i].value);
    }
    return conform_writer_end(&writer, out);
}

/* A POST read back as an incoming request and delivered to its sender. */
static void post_deliver(w3c_test *test, const w3c_post *post) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = post->session;
    if (post->refused) {
        event.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
        event.sendid = ta_string(post->sendid);
        ta_host_send(test->host, &event);
        return;
    }
    static char scratch[8192];
    static ta_http_request request;
    if (ta_http_parse_request((ta_bytes){post->message, post->length}, &(ta_buffer){scratch, sizeof scratch, 0},
                              &request) != TA_OK)
        return;
    ta_buffer data = {NULL, 0, 0};
    if (fields_write(&request, &data) == TA_OK)
        conform_datamodel_read(&test->datamodel, (ta_bytes){data.pointer, data.length}, &event.data);
    conform_buffer_free(&data);
    event.name = request.event;
    event.origin = ta_string(LOCATION);
    event.origintype = ta_string(TA_BASIC_HTTP_TYPE);
    event.raw = (ta_string_view){post->message, (uint32_t)post->length};
    if (ta_host_send(test->host, &event) != TA_OK) conform_datamodel_release(&test->datamodel, event.data);
}

/* ---- the test ------------------------------------------------------------------------------ */

/* Runs the host: at most 200 runs of 20,000 fuel; the first Outcome log
 * decides. */
static const char *test_run(w3c_test *test) {
    const char *outcome = NULL;
    for (int round = 0; round < 200 && !outcome; round++) {
        ta_run run = ta_host_run(test->host, 20000);
        const ta_host_effect *effects;
        uint32_t count = ta_host_effects(test->host, &effects);
        test->post_count = 0;
        for (uint32_t i = 0; i < count; i++) {
            if (!outcome) outcome = conform_outcome(&effects[i]);
            if (effects[i].kind == TA_EFFECT_SEND && effects[i].io_processor == 0) post_write(test, &effects[i]);
        }
        ta_host_effects_ack(test->host);
        for (uint32_t i = 0; i < test->post_count; i++)
            post_deliver(test, &test->posts[i]);
        if (test->post_count && run.status == TA_RUN_IDLE) continue;
        if (run.status == TA_RUN_EXHAUSTED || run.status == TA_RUN_FAULT) {
            fprintf(stderr, "ta-conform: the run ended with status %u (detail %u)\n", run.status, run.detail);
            break;
        }
        if (run.status == TA_RUN_IDLE) {
            if (run.next_deadline == UINT64_MAX) break;
            ta_host_advance_time(test->host, (ta_host_time){run.next_deadline, 0});
        }
    }
    return outcome ? outcome : "timeout";
}

static void w3c_run(conform *state, conform_call *call, conform_result *result) {
    (void)state;
    static w3c_test test;
    memset(&test, 0, sizeof test);
    conform_sources sources;
    if (!conform_required(call, "documents") || !conform_sources_read(call, "documents", &sources) ||
        !conform_datamodel_open(&test.datamodel, call))
        return;
    const char *outcome = "compile";
    conform_image image;
    static unsigned char config[1024];
    ta_buffer config_bytes = {config, sizeof config, 0};
    uint32_t segment = 0;
    ta_host_environment environment = conform_datamodel_environment(&test.datamodel, NULL);
    /* the charts: the start document and the dependencies that are SCXML */
    static ta_source charts[32];
    uint32_t chart_count = 0;
    for (uint32_t i = 0; i < sources.count; i++) {
        ta_string_view name = sources.items[i].name;
        if (i == 0 || (name.length > 6 && !memcmp(name.pointer + name.length - 6, ".scxml", 6)))
            charts[chart_count++] = sources.items[i];
    }
    conform_documents documents = {{charts, chart_count}, {sources.items, sources.count}};
    if (sources.count && conform_image_compile(&documents, &image) == TA_OK) {
        outcome = "fail";
        ta_status status = config_write(NULL, &config_bytes);
        if (status == TA_OK) status = ta_host_create((ta_bytes){config, config_bytes.length}, &environment, &test.host);
        if (status == TA_OK) status = ta_host_add_image(test.host, image.bytes, &segment);
        if (status == TA_OK) status = ta_host_boot(test.host, segment);
        if (status == TA_OK) outcome = test_run(&test);
        else fprintf(stderr, "ta-conform: the test's host refused it: %d\n", (int)status);
        if (test.host) ta_host_free(test.host);
        conform_image_free(&image);
    }
    conform_datamodel_close(&test.datamodel);
    conform_result_text(result, "outcome", ta_string(outcome));
}

static const char *const run_params[] = {"documents", "datamodel", "realm", NULL};

const conform_method conform_w3c_methods[] = {
    {"w3c.run", w3c_run, run_params},
    {NULL, NULL, NULL},
};
