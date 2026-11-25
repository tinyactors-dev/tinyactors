/* tinyactors run's memory: the server run in this process (tool_main) over
 * a platform whose allocator refuses after a budget, for every budget until
 * the session succeeds. Every refusal is a status: the server answers the
 * message it could not serve with -32004 and goes on, or, at startup or for
 * the line buffer, exits 3; it never crashes, and frees what it took (the
 * sanitizer build checks that nothing leaks). The server takes few blocks
 * (its arenas' chunks, the host's, the tables'), so every budget is tried.
 * The session is the worked example's, with a batch, a forwarded request,
 * an image added and refusals; batches held and acked, a send failed,
 * services invoked, done, failed and cancelled, the lookups, and the end of
 * input with a batch held, fed a few bytes at a time. The server's method
 * table is held to the schema's run.* methods too. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "schema_run.h"
#include "serve.h"

#ifndef TA_TEST_TRANSCRIPTS
#define TA_TEST_TRANSCRIPTS "tests/run"
#endif

static const char session[] =
    "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order-"
    "17\",\"params\":{\"total\":4200}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"checkout\",\""
    "data\":{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"actor.state\",\"params\":{\"session\":1}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"charge.ok\","
    "\"sendid\":\"pay\",\"origintype\":\"payments\",\"data\":{\"charge\":\"ch_81\"}}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":6,\"method\":\"run.turn\"},{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"actor"
    ".state\",\"params\":{\"session\":1}},{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"run.advanceTime\",\"para"
    "ms\":{\"by\":\"30s\"}}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"run.spawn\",\"params\":{\"chart\":\"refund\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":10,\"method\":\"run.addImage\",\"params\":{\"sources\":[{\"name\":\"renewal."
    "scxml\",\"text\":\"<scxml xmlns='http://www.w3.org/2005/07/scxml' name='renewal' datamodel='ecmascript'>"
    "<state id='charging'><invoke type='payment' src='psp:renewals' id='payment'/><transition event='done.inv"
    "oke.payment' target='renewed'/></state><final id='renewed'/></scxml>\"}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"ren"
    "ewal-1\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":12,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":13,\"method\":\"run.send\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@renewal-1\",\"invocation\":16777217,\"done\":true,\"data\":{\"receipt\":\"rcpt_9\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":14,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order"
    "-18\",\"params\":{\"total\":990}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":15,\"method\":\"run.sendMany\",\"params\":{\"events\":[{\"target\":2,\"event"
    "\":\"checkout\",\"data\":{\"card\":\"tok_expired\"}}]}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":16,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":17,\"method\":\"run.fail\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@order-18\",\"sendid\":\"pay\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":18,\"method\":\"run.resolve\",\"params\":{\"address\":\"tinyactors://tinyact"
    "ors-run/@order-18\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":19,\"method\":\"run.ack\"}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":20,\"method\":\"run.chart\",\"params\":{\"name\":\"renewal\"}},{\"jsonrpc\""
    ":\"2.0\",\"id\":21,\"method\":\"run.actorByName\",\"params\":{\"name\":\"renewal-1\"}},{\"jsonrpc\":\"2."
    "0\",\"id\":22,\"method\":\"run.terminate\",\"params\":{\"target\":2}},{\"jsonrpc\":\"2.0\",\"id\":23,\"m"
    "ethod\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":28,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"ren"
    "ewal-2\"}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":29,\"method\":\"run.turn\"}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":30,\"method\":\"run.fail\",\"params\":{\"target\":\"tinyactors://tinyactors-"
    "run/@renewal-2\",\"invocation\":50331649}}\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":31,\"method\":\"run.spawn\",\"params\":{\"chart\":\"renewal\",\"name\":\"re"
    "newal-3\"}},{\"jsonrpc\":\"2.0\",\"id\":32,\"method\":\"run.turn\"}]\n"
    "[{\"jsonrpc\":\"2.0\",\"id\":33,\"method\":\"run.terminate\",\"params\":{\"target\":\"tinyactors://tinya"
    "ctors-run/@renewal-3\"}},{\"jsonrpc\":\"2.0\",\"id\":34,\"method\":\"run.turn\"}]\n"
    "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"run.schema\"}\n"
    "not json\n"
    "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"run.spawn\",\"params\":{\"chart\":\"order\",\"name\":\"order"
    "-19\",\"params\":{\"total\":150}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":26,\"method\":\"run.send\",\"params\":{\"target\":3,\"event\":\"checkout\","
    "\"data\":{\"card\":\"tok_visa\"}}}\n"
    "{\"jsonrpc\":\"2.0\",\"id\":27,\"method\":\"run.turn\",\"params\":{\"hold\":true}}\n";

/* ---- a platform in memory ------------------------------------------------------------ */

typedef struct memory_platform {
    long budget; /* allocations left; negative: no limit */
    long made, live;
    size_t at;  /* of `session`: what receive gave so far */
    char *output;
    size_t output_length;
} memory_platform;

static void *memory_alloc(void *context, size_t size, size_t align) {
    (void)align;
    memory_platform *platform = (memory_platform *)context;
    if (platform->budget == 0) return NULL;
    if (platform->budget > 0) platform->budget--;
    platform->made++;
    platform->live++;
    return malloc(size);
}

static void memory_free(void *context, void *pointer, size_t size) {
    (void)size;
    memory_platform *platform = (memory_platform *)context;
    if (pointer) platform->live--;
    free(pointer);
}

static ta_status memory_read(void *context, const tool_read *request, ta_bytes *out) {
    (void)context;
    char path[1024];
    snprintf(path, sizeof path, "%s/order/%.*s", TA_TEST_TRANSCRIPTS, (int)request->path.length, request->path.pointer);
    FILE *file = fopen(path, "rb");
    if (!file) return TA_ERROR_INVALID;
    fseek(file, 0, SEEK_END);
    long length = ftell(file);
    fseek(file, 0, SEEK_SET);
    void *bytes = ta_growable_arena_alloc(request->arena, (size_t)length + 1, 8);
    size_t read = bytes ? fread(bytes, 1, (size_t)length, file) : 0;
    fclose(file);
    if (!bytes) return TA_ERROR_NO_MEMORY;
    *out = (ta_bytes){bytes, read};
    return TA_OK;
}

static ta_status memory_write(void *context, const tool_write *request) {
    memory_platform *platform = (memory_platform *)context;
    if (request->stream != TOOL_STANDARD_OUTPUT || request->path.length) return TA_OK; /* complaints */
    char *grown = (char *)realloc(platform->output, platform->output_length + request->bytes.length + 1);
    if (!grown) return TA_ERROR_INVALID;
    memcpy(grown + platform->output_length, request->bytes.pointer, request->bytes.length);
    platform->output = grown;
    platform->output_length += request->bytes.length;
    platform->output[platform->output_length] = 0;
    return TA_OK;
}

/* A few bytes at a time, so lines arrive split. */
static ta_status memory_receive(void *context, ta_buffer *room) {
    memory_platform *platform = (memory_platform *)context;
    size_t left = sizeof session - 1 - platform->at, take = left < 97 ? left : 97;
    if (take > room->capacity) take = room->capacity;
    memcpy(room->pointer, session + platform->at, take);
    platform->at += take;
    room->length = take;
    return TA_OK;
}

typedef struct outcome {
    int exit;
    long made, live;
    char *output;
} outcome;

static outcome serve(long budget) {
    static const char *const words[] = {"run", "--config", "config.json", "order.scxml"};
    memory_platform state;
    memset(&state, 0, sizeof state);
    state.budget = budget;
    tool_platform platform;
    memset(&platform, 0, sizeof platform);
    platform.context = &state;
    platform.allocator = (ta_allocator){&state, memory_alloc, memory_free};
    platform.read = memory_read;
    platform.write = memory_write;
    platform.receive = memory_receive;
    platform.version = ta_string("");
    int exit = tool_main(&platform, (tool_command_line){4, words});
    return (outcome){exit, state.made, state.live, state.output};
}

static void test_every_refusal_is_a_status(void) {
    outcome whole = serve(-1);
    CHECK_EQUAL(whole.exit, 0);
    CHECK_EQUAL(whole.live, 0);
    CHECK(whole.output && strstr(whole.output, "\"name\":\"order-17\",\"final\":\"paid\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":3,\"result\":{\"status\":\"held\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":4,\"error\":{\"code\":-32002"));
    CHECK(whole.output && strstr(whole.output, "\"name\":\"renewal-1\",\"final\":\"renewed\""));
    CHECK(whole.output && strstr(whole.output, "\"method\":\"effect.uninvoke\""));
    CHECK(whole.output && strstr(whole.output, "\"id\":24,\"result\":{\"schema\":{"));
    CHECK(whole.output && strstr(whole.output, "\"id\":27,\"result\":{\"status\":\"held\""));
    long tried = 0, answered_memory = 0;
    for (long budget = 0; budget < whole.made; budget++) {
        outcome refused = serve(budget);
        tried++;
        CHECK(refused.exit == 0 || refused.exit == 3);
        CHECK_EQUAL(refused.live, 0); /* everything taken was given back */
        if (refused.output && strstr(refused.output, "-32004")) answered_memory++;
        free(refused.output);
    }
    fprintf(stderr, "  %ld allocations; %ld budgets tried, %ld answered -32004 and went on\n", whole.made, tried,
            answered_memory);
    CHECK(answered_memory > 0);
    free(whole.output);
}

/* The run.* methods of the schema that later steps of the protocol build:
 * the server answers them -32601 until then. */
static const char *const unbuilt[] = {"run.hibernate", "run.wake",   "run.sweep",   "run.migrate", "run.snapshot",
                                      "run.restore",   "run.traces", "run.metrics", "run.post"};

static int is_unbuilt(ta_string_view name) {
    for (size_t i = 0; i < sizeof unbuilt / sizeof unbuilt[0]; i++)
        if (name.length == strlen(unbuilt[i]) && memcmp(name.pointer, unbuilt[i], name.length) == 0) return 1;
    return 0;
}

static int is_served(tool_method_list served, ta_string_view name) {
    for (uint32_t i = 0; i < served.count; i++)
        if (served.methods[i].name.length == name.length &&
            memcmp(served.methods[i].name.pointer, name.pointer, name.length) == 0)
            return 1;
    return 0;
}

/* Both ways: every method the server answers is a run.* method of the
 * schema, and every run.* method of the schema is the server's or listed
 * as unbuilt (and not both). */
static void test_methods_are_the_schemas(void) {
    tool_method_list served = tool_serve_methods();
    CHECK(served.count > 0);
    for (uint32_t i = 0; i < served.count; i++) {
        ta_string_view name = served.methods[i].name;
        int found = tool_schema_method_find(&tool_schema_run, name) < tool_schema_run.method_count;
        if (!found) fprintf(stderr, "  the server has %.*s, the schema has not\n", (int)name.length, name.pointer);
        CHECK(found);
        CHECK(name.length > 4 && memcmp(name.pointer, "run.", 4) == 0);
        CHECK(!is_unbuilt(name));
    }
    uint32_t described = 0;
    for (uint32_t i = 0; i < tool_schema_run.method_count; i++) {
        ta_string_view name = tool_schema_run.methods[i].name;
        if (name.length < 4 || memcmp(name.pointer, "run.", 4) != 0) continue;
        described++;
        int known = is_served(served, name) || is_unbuilt(name);
        if (!known) fprintf(stderr, "  the schema has %.*s, the server has not\n", (int)name.length, name.pointer);
        CHECK(known);
    }
    for (size_t i = 0; i < sizeof unbuilt / sizeof unbuilt[0]; i++)
        CHECK(tool_schema_method_find(&tool_schema_run, ta_string(unbuilt[i])) < tool_schema_run.method_count);
    CHECK_EQUAL(described, served.count + (uint32_t)(sizeof unbuilt / sizeof unbuilt[0]));
}

int main(void) {
    RUN(test_methods_are_the_schemas);
    RUN(test_every_refusal_is_a_status);
    TEST_MAIN_END();
}
