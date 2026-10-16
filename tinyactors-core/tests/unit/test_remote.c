/* Other systems through the I/O processor the config marks remote (host.h,
 * "Other systems"): a send to another system's address is a SEND for it,
 * with the sender's address as ORIGIN; an input names its target by a local
 * address; a reply to _event.origin goes back the same way; without a remote
 * processor the sender gets error.communication. */
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
    "</transition><transition event='direct'><send type='urn:test:remote' "                       \
    "target='tinyactors://a/@talker' event='hello'/></transition></state></scxml>"

#define REMOTE_TYPE "urn:test:remote"
#define SCXML_TYPE  "http://www.w3.org/TR/scxml/#SCXMLEventProcessor"

typedef struct world {
    ta_datamodel_ecmascript *ecmascript;
    ta_datamodel_binding binding;
    ta_host *host;
    uint32_t segment;
    uint64_t session;
    char log[512];
} world;

static void world_open(world *w, int remote) {
    memset(w, 0, sizeof *w);
    w->ecmascript = ta_datamodel_ecmascript_new(NULL);
    w->binding = ta_datamodel_ecmascript_binding(w->ecmascript);
    ta_io_processor_descriptor processors[2] = {
        {{"urn:test:other", 14}, {"", 0}, 0, 0},
        {{REMOTE_TYPE, sizeof REMOTE_TYPE - 1}, {"", 0},
         TA_IO_PROCESSOR_REMOTE, 0},
    };
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"a", 1};
    config.system.size = sizeof config.system;
    config.system.max_actors = 8;
    config.datamodels = &w->binding;
    config.datamodel_count = 1;
    config.io_processors = processors;
    config.io_processor_count = remote ? 2 : 1;
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

static void test_a_send_to_another_system_is_a_send_for_the_remote_processor(void) {
    world w;
    world_open(&w, 1);
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
    CHECK_EQUAL(ta_host_effect_number(w.host, send, TA_EFFECT_FIELD_IO_PROCESSOR), 1u); /* the config's index */
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
    world_open(&w, 1);
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
    world_open(&w, 1);
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

/* A chart that names the remote processor's type gets the same decision: a
 * local target is delivered here, not handed to the transport. */
static void test_the_remote_type_in_a_chart_is_decided_the_same_way(void) {
    world w;
    world_open(&w, 1);
    run(&w);
    ta_event_in direct;
    memset(&direct, 0, sizeof direct);
    direct.size = sizeof direct;
    direct.session = w.session;
    direct.name = (ta_string_view){"direct", 6};
    CHECK_EQUAL(ta_host_send(w.host, &direct), TA_OK);
    run(&w);
    run(&w);
    CHECK(strcmp(w.log, "got:tinyactors://a/#_scxml_1 " SCXML_TYPE " undefined;") == 0);
    world_close(&w);
}

static void test_without_a_remote_processor_the_sender_is_told(void) {
    world w;
    world_open(&w, 0);
    run(&w);
    ta_event_in call;
    memset(&call, 0, sizeof call);
    call.size = sizeof call;
    call.session = w.session;
    call.name = (ta_string_view){"call", 4};
    CHECK_EQUAL(ta_host_send(w.host, &call), TA_OK);
    uint32_t count = run(&w);
    CHECK_EQUAL(find_send(&w, count), TA_NIL);
    run(&w);
    CHECK(strcmp(w.log, "failed:h1;") == 0);
    world_close(&w);
}

static void test_at_most_one_remote_processor(void) {
    ta_datamodel_ecmascript *ecmascript = ta_datamodel_ecmascript_new(NULL);
    ta_datamodel_binding binding = ta_datamodel_ecmascript_binding(ecmascript);
    ta_io_processor_descriptor processors[2] = {
        {{"urn:a", 5}, {"", 0}, TA_IO_PROCESSOR_REMOTE, 0},
        {{"urn:b", 5}, {"", 0}, TA_IO_PROCESSOR_REMOTE, 0},
    };
    test_host_config config;
    memset(&config, 0, sizeof config);
    config.size = sizeof config;
    config.system_id = (ta_string_view){"a", 1};
    config.system.size = sizeof config.system;
    config.datamodels = &binding;
    config.datamodel_count = 1;
    config.io_processors = processors;
    config.io_processor_count = 2;
    config.allocator = ta_runtime_malloc();
    ta_host *host = NULL;
    CHECK(test_host_new(&config, &host) != TA_OK);
    CHECK(host == NULL);
    ta_datamodel_ecmascript_free(ecmascript);
}

int main(void) {
    RUN(test_a_send_to_another_system_is_a_send_for_the_remote_processor);
    RUN(test_an_input_by_address_and_the_reply_to_its_origin);
    RUN(test_a_sender_waits_for_the_transport);
    RUN(test_the_remote_type_in_a_chart_is_decided_the_same_way);
    RUN(test_without_a_remote_processor_the_sender_is_told);
    RUN(test_at_most_one_remote_processor);
    TEST_MAIN_END();
}
