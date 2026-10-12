/* ta_host_effect_number / ta_host_effect_text (host.h): every field an effect
 * has, read without the struct's layout, and a send to an invoked service
 * carrying the token of the invoke that started it. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "../support/host_config.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='caller' "         \
    "initial='s'><state id='s'>"                                                                   \
    "<invoke id='svc' type='http://example.com/service' src='svc:x'/>"                            \
    "<transition event='go'><log label='said' expr=\"'hello'\"/>"                                  \
    "<send target='#_svc' event='poke' id='p1'/></transition>"                                     \
    "<transition event='end' target='f'/></state><final id='f'/></scxml>"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    void *image_bytes;
    uint32_t segment;
    uint64_t session;
} world;

static void *compile_caller(size_t *length) {
    ta_compile_options options;
    memset(&options, 0, sizeof options);
    options.size = sizeof options;
    ta_source source = {{"caller.scxml", 12}, {CHART, sizeof CHART - 1}};
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

static void world_open(world *w) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"h", 1};
    config.system.size = sizeof config.system;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    w->image_bytes = compile_caller(&length);
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

/* Run until idle; the published batch stays for the caller to read. */
static uint32_t run_batch(world *w, const ta_host_effect **effects) {
    ta_host_effects_ack(w->host);
    ta_run result = ta_host_run(w->host, 1000000);
    CHECK(result.status == TA_RUN_IDLE || result.status == TA_RUN_EFFECTS);
    return ta_host_effects(w->host, effects);
}

static void send_event(world *w, const char *name) {
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.session = w->session;
    CHECK_EQUAL(ta_host_send(w->host, &event), TA_OK);
}

static int text_is(ta_string_view text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.pointer, expected, text.length) == 0;
}

/* The index of the first effect of `kind` in the batch, TA_NIL if none. */
static uint32_t find(ta_host *host, uint32_t count, uint32_t kind) {
    for (uint32_t i = 0; i < count; i++)
        if (ta_host_effect_number(host, i, TA_EFFECT_FIELD_KIND) == kind) return i;
    return TA_NIL;
}

static void test_invoked_service_is_keyed_by_token(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    uint32_t count = run_batch(&w, &effects);
    uint32_t invoke = find(w.host, count, TA_EFFECT_INVOKE);
    CHECK(invoke != TA_NIL);
    uint64_t token = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_TOKEN);
    uint64_t segment = ta_host_effect_number(w.host, invoke, TA_EFFECT_FIELD_SEGMENT);
    CHECK(token != 0);
    CHECK_EQUAL(segment, w.segment);
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_TARGET), "svc:x"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_EVENT),
                  "http://example.com/service"));
    CHECK(text_is(ta_host_effect_text(w.host, invoke, TA_EFFECT_FIELD_SENDID), "svc"));
    send_event(&w, "go");
    count = run_batch(&w, &effects);
    uint32_t send = find(w.host, count, TA_EFFECT_SEND);
    CHECK(send != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_TOKEN), token);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_SEGMENT), segment);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_IO_PROCESSOR), (uint64_t)TA_NIL);
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_EVENT), "poke"));
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_SESSION), w.session);
    /* the log of the same step */
    uint32_t log = find(w.host, count, TA_EFFECT_LOG);
    CHECK(log != TA_NIL);
    CHECK(text_is(ta_host_effect_text(w.host, log, TA_EFFECT_FIELD_AUXILIARY), "said"));
    world_close(&w);
}

/* Every number and text field matches the struct; unknown fields, the wrong
 * kind of field and an index past the batch answer nothing. */
static void test_fields_match_the_struct(void) {
    world w;
    world_open(&w);
    const ta_host_effect *effects;
    run_batch(&w, &effects);
    send_event(&w, "go");
    run_batch(&w, &effects);
    send_event(&w, "end");
    uint32_t count = run_batch(&w, &effects);
    CHECK(find(w.host, count, TA_EFFECT_DONE) != TA_NIL);
    CHECK(find(w.host, count, TA_EFFECT_UNINVOKE) != TA_NIL);
    for (uint32_t i = 0; i < count; i++) {
        const ta_host_effect *e = &effects[i];
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_KIND), e->kind);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_SEGMENT), e->segment);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_ACTOR), e->actor);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_SESSION), e->session);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_ROOT), e->root);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_TOKEN), e->token);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_IO_PROCESSOR), e->io_processor);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_FLAGS), e->flags);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_JOB), e->job);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_PAYLOAD), e->payload);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_DATAMODEL), e->datamodel);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_DELAY_HINT), e->delay_hint);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_CHART), e->chart);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_MIGRATED), e->migrated);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_FAILED), e->failed);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_PENDING), e->pending);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_PEER), e->peer);
        ta_string_view t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_TARGET);
        CHECK(t.length == e->target.length && memcmp(t.pointer, e->target.pointer, t.length) == 0);
        t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_EVENT);
        CHECK(t.length == e->event.length && memcmp(t.pointer, e->event.pointer, t.length) == 0);
        t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_SENDID);
        CHECK(t.length == e->sendid.length && memcmp(t.pointer, e->sendid.pointer, t.length) == 0);
        t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_AUXILIARY);
        CHECK(t.length == e->auxiliary.length &&
              memcmp(t.pointer, e->auxiliary.pointer, t.length) == 0);
        t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_MSGID);
        CHECK(t.length == e->msgid.length && memcmp(t.pointer, e->msgid.pointer, t.length) == 0);
        char expected[TA_TRACEPARENT_LENGTH];
        ta_buffer out = {expected, sizeof expected, 0};
        int traced = ta_traceparent_format(&e->trace, &out) == TA_OK;
        t = ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_TRACEPARENT);
        CHECK_EQUAL(t.length, traced ? TA_TRACEPARENT_LENGTH : 0u);
        CHECK(!traced || memcmp(t.pointer, expected, t.length) == 0);
        /* unknown fields and the wrong kind */
        CHECK_EQUAL(ta_host_effect_number(w.host, i, 0), 0u);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, 999), 0u);
        CHECK_EQUAL(ta_host_effect_number(w.host, i, TA_EFFECT_FIELD_TARGET), 0u);
        CHECK_EQUAL(ta_host_effect_text(w.host, i, TA_EFFECT_FIELD_KIND).length, 0u);
    }
    CHECK_EQUAL(ta_host_effect_number(w.host, count, TA_EFFECT_FIELD_KIND), 0u);
    CHECK_EQUAL(ta_host_effect_text(w.host, count, TA_EFFECT_FIELD_EVENT).length, 0u);
    /* after the ack the batch is gone */
    ta_host_effects_ack(w.host);
    CHECK_EQUAL(ta_host_effect_number(w.host, 0, TA_EFFECT_FIELD_KIND), 0u);
    world_close(&w);
}

int main(void) {
    RUN(test_invoked_service_is_keyed_by_token);
    RUN(test_fields_match_the_struct);
    TEST_MAIN_END();
}
