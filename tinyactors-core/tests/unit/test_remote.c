/* Other systems through the host's transport (host.h, "Other systems"): a
 * send to another system's address is a SEND for TA_HOST_TRANSPORT, with the
 * sender's address as ORIGIN, and needs no config; an input names its target
 * by a local address; a reply to _event.origin goes back the same way; a
 * send the embedder fails gives its sender error.communication. */
#include "../harness/harness.h"
#include "../support/compile_util.h"
#include "../support/host_config.h"
#include <tinyactors/datamodel_ecmascript.h>
#include <tinyactors/host.h>
#include <tinyactors/runtime.h>

#define CHART                                                                                      \
    "<scxml xmlns='http://www.w3.org/2005/07/scxml' datamodel='ecmascript' name='talker' "         \
    "initial='s'><state id='s'>"                                                                   \
    "<transition event='call'><send target='tinyactors://b/#_scxml_5' event='hello' id='h1'>"      \
    "<param name='n' expr='41'/></send></transition>"                                              \
    "<transition event='hello'><log label='got' expr=\"_event.origin + ' ' + _event.origintype + "   \
    "' ' + typeof _event.data\"/><send targetexpr='_event.origin' event='thanks'/></transition>"        \
    "<transition event='error.communication'><log label='failed' expr='_event.sendid'/>"          \
    "</transition></state></scxml>"

#define SCXML_TYPE  "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    uint32_t segment;
    uint64_t session;
    char log[512];
} world;

static void world_open(world *w) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    ta_io_processor_descriptor processors[1] = {{{"urn:test:other", 14}, {"", 0}, 0, 0}};
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"a", 1};
    config.system.size = sizeof config.system;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.io_processors = processors;
    config.io_processor_count = 1;
    config.allocator = ta_runtime_malloc();
    CHECK_EQUAL(test_host_new(&config, &w->host), TA_OK);
    size_t length;
    void *image = compile_chart("talker.scxml", CHART, NULL, &length, 0);
    CHECK(image != NULL);
    CHECK_EQUAL(ta_host_add_image(w->host, (ta_bytes){image, length}, &w->segment), TA_OK);
    free(image);
    ta_spawn spawn;
    memset(&spawn, 0, sizeof spawn);
    spawn.segment = w->segment;
    spawn.name = (ta_string_view){"talker", 6};
    CHECK_EQUAL(ta_host_spawn(w->host, &spawn, &w->session), TA_OK);
}

static void world_close(world *w) {
    ta_host_free(w->host);
    ta_datamodel_ecmascript_free(w->ecmascript);
}

static int text_is(ta_string_view text, const char *expected) {
    return text.length == strlen(expected) && memcmp(text.pointer, expected, text.length) == 0;
}

/* Run until idle, keep the log lines, and leave the batch published. */
static uint32_t run(world *w) {
    ta_host_effects_ack(w->host);
    ta_run result = ta_host_run(w->host, 1000000);
    CHECK(result.status == TA_RUN_IDLE || result.status == TA_RUN_EFFECTS);
    const ta_host_effect *effects;
    uint32_t count = ta_host_effects(w->host, &effects);
    for (uint32_t i = 0; i < count; i++)
        if (effects[i].kind == TA_EFFECT_LOG) {
            size_t used = strlen(w->log);
            snprintf(w->log + used, sizeof w->log - used, "%.*s:%.*s;", (int)effects[i].auxiliary.length,
                     effects[i].auxiliary.pointer, (int)effects[i].event.length, effects[i].event.pointer);
        }
    return count;
}

static uint32_t find_send(world *w, uint32_t count) {
    for (uint32_t i = 0; i < count; i++)
        if (ta_host_effect_number(w->host, i, TA_EFFECT_FIELD_KIND) == TA_EFFECT_SEND) return i;
    return TA_NIL;
}

/* What a transport does with what another system sent: resolve the local
 * address, then send with the remote origin (origintype left to core). */
static ta_status input(world *w, const char *address, const char *name, const char *origin) {
    uint64_t session = 0;
    ta_status status = ta_host_resolve(w->host, (ta_string_view){address, (uint32_t)strlen(address)}, &session);
    if (status != TA_OK) return status;
    ta_event_in event;
    memset(&event, 0, sizeof event);
    event.size = sizeof event;
    event.session = session;
    event.name = (ta_string_view){name, (uint32_t)strlen(name)};
    event.origin = (ta_string_view){origin, (uint32_t)strlen(origin)};
    return ta_host_send(w->host, &event);
}

static void test_a_send_to_another_system_is_a_send_for_the_transport(void) {
    world w;
    world_open(&w);
    run(&w);
    ta_event_in call;
    memset(&call, 0, sizeof call);
    call.size = sizeof call;
    call.session = w.session;
    call.name = (ta_string_view){"call", 4};
    CHECK_EQUAL(ta_host_send(w.host, &call), TA_OK);
    uint32_t count = run(&w);
    uint32_t send = find_send(&w, count);
    CHECK(send != TA_NIL);
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_IO_PROCESSOR), TA_HOST_TRANSPORT);
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_TARGET), "tinyactors://b/#_scxml_5"));
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_EVENT), "hello"));
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_SENDID), "h1"));
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_ORIGIN), "tinyactors://a/#_scxml_1"));
    CHECK(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_PAYLOAD) != 0);
    CHECK(strcmp(w.log, "") == 0);
    world_close(&w);
}

static void test_an_input_by_address_and_the_reply_to_its_origin(void) {
    world w;
    world_open(&w);
    run(&w);
    /* the transport delivers what system b sent: by session address and by name */
    CHECK_EQUAL(input(&w, "tinyactors://a/#_scxml_1", "hello", "tinyactors://b/#_scxml_5"), TA_OK);
    uint32_t count = run(&w);
    CHECK(strcmp(w.log, "got:tinyactors://b/#_scxml_5 " SCXML_TYPE " undefined;") == 0);
    uint32_t send = find_send(&w, count);
    CHECK(send != TA_NIL); /* the reply to _event.origin: system b again */
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_TARGET), "tinyactors://b/#_scxml_5"));
    CHECK(text_is(ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_EVENT), "thanks"));
    w.log[0] = 0;
    CHECK_EQUAL(input(&w, "tinyactors://a/@talker", "hello", "tinyactors://c/@x"), TA_OK);
    run(&w);
    CHECK(strcmp(w.log, "got:tinyactors://c/@x " SCXML_TYPE " undefined;") == 0);
    /* another system's address, no such name, no such session, not an address */
    CHECK_EQUAL(input(&w, "tinyactors://b/#_scxml_1", "hello", ""), TA_ERROR_INVALID);
    CHECK_EQUAL(input(&w, "tinyactors://a/@nobody", "hello", ""), TA_ERROR_STALE);
    CHECK_EQUAL(input(&w, "tinyactors://a/#_scxml_99", "hello", ""), TA_ERROR_STALE);
    CHECK_EQUAL(input(&w, "#_scxml_1", "hello", ""), TA_ERROR_INVALID);
    world_close(&w);
}

/* A send to another system parks its sender until the transport's batch is
 * acked, so the transport can fail it within the sender's macrostep. */
static void test_a_sender_waits_for_the_transport(void) {
    world w;
    world_open(&w);
    run(&w);
    ta_host_effects_ack(w.host);
    ta_event_in call;
    memset(&call, 0, sizeof call);
    call.size = sizeof call;
    call.session = w.session;
    call.name = (ta_string_view){"call", 4};
    CHECK_EQUAL(ta_host_send(w.host, &call), TA_OK);
    ta_run result = ta_host_run(w.host, 1000000);
    CHECK_EQUAL(result.status, TA_RUN_EFFECTS);
    world_close(&w);
}

/* The embedder fails a send it cannot carry: the sender gets
 * error.communication with its sendid, before it goes on. */
static void test_a_send_the_transport_fails(void) {
    world w;
    world_open(&w);
    run(&w);
    ta_event_in call;
    memset(&call, 0, sizeof call);
    call.size = sizeof call;
    call.session = w.session;
    call.name = (ta_string_view){"call", 4};
    CHECK_EQUAL(ta_host_send(w.host, &call), TA_OK);
    uint32_t count = run(&w);
    uint32_t send = find_send(&w, count);
    CHECK(send != TA_NIL);
    ta_event_in failed;
    memset(&failed, 0, sizeof failed);
    failed.size = sizeof failed;
    failed.session = w.session;
    failed.flags = TA_EVENT_IN_ERROR_COMMUNICATION;
    failed.sendid = ta_host_effect_text(w.host, send, TA_EFFECT_FIELD_SENDID);
    CHECK_EQUAL(ta_host_send(w.host, &failed), TA_OK);
    run(&w);
    CHECK(strcmp(w.log, "failed:h1;") == 0);
    world_close(&w);
}

int main(void) {
    RUN(test_a_send_to_another_system_is_a_send_for_the_transport);
    RUN(test_an_input_by_address_and_the_reply_to_its_origin);
    RUN(test_a_sender_waits_for_the_transport);
    RUN(test_a_send_the_transport_fails);
    TEST_MAIN_END();
}
