/* The schema checker (tools/tinyactors/schema_check.c): every document of
 * the generator's fixture cases (tools/tinyactors/schema/tests), whose
 * expectations a 2020-12 validator checks too (mise run run:schema), and the
 * run protocol's own schema. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "schema.h"
#include "schema_fixtures.h"
#include "schema_run.h"

static void *test_alloc(void *context, size_t size, size_t align) {
    (void)align;
    int *refuse = (int *)context;
    return *refuse ? NULL : malloc(size);
}

static void test_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

/* A decoded document, its tokens on the heap. */
typedef struct test_document {
    ta_cbor_document document;
    ta_cbor_token tokens[512];
} test_document;

static int document_decode(test_document *decoded, ta_bytes cbor) {
    memset(decoded, 0, sizeof *decoded);
    decoded->document.bytes = cbor;
    decoded->document.tokens = decoded->tokens;
    decoded->document.capacity = sizeof decoded->tokens / sizeof decoded->tokens[0];
    return ta_cbor_decode(&decoded->document) == TA_OK;
}

static int view_is(ta_string_view view, ta_string_view expected) {
    return view.length == expected.length && (!view.length || memcmp(view.pointer, expected.pointer, view.length) == 0);
}

/* An arena over the heap; `refuse` set: it refuses every allocation. */
typedef struct test_arena {
    int refuse;
    ta_allocator allocator;
    ta_growable_arena arena;
} test_arena;

static void arena_begin(test_arena *arena) {
    memset(arena, 0, sizeof *arena);
    arena->allocator.context = &arena->refuse;
    arena->allocator.alloc = test_alloc;
    arena->allocator.free = test_free;
    ta_growable_arena_config config = {sizeof config, 0, 256, &arena->allocator};
    ta_growable_arena_init(&arena->arena, &config);
}

static void test_fixtures(void) {
    test_arena arena;
    arena_begin(&arena);
    CHECK(test_schema_fixture_count > 100);
    for (uint32_t i = 0; i < test_schema_fixture_count; i++) {
        const test_schema_fixture *fixture = &test_schema_fixtures[i];
        test_document *decoded = (test_document *)malloc(sizeof *decoded);
        CHECK(decoded && document_decode(decoded, fixture->cbor));
        if (!decoded) continue;
        tool_schema_input input = {fixture->schema, &decoded->document, 0, 0, {0, 0}, &arena.arena};
        tool_schema_error error;
        ta_status status;
        if (fixture->method == TA_NIL) {
            status = tool_schema_check_message(0, &input, &error);
        } else {
            input.root = ta_string("params");
            status = tool_schema_check_params(fixture->method, &input, &error);
        }
        int good = fixture->accepted ? status == TA_OK
                                     : status == TA_ERROR_INVALID && error.code == fixture->code &&
                                           view_is(error.reason, fixture->reason) && view_is(error.path, fixture->path);
        CHECK(good);
        if (!good)
            fprintf(stderr, "  %.*s %.*s: status %d, code %d, reason %.*s, path \"%.*s\"\n",
                    (int)fixture->case_name.length, fixture->case_name.pointer, (int)fixture->document_name.length,
                    fixture->document_name.pointer, (int)status, error.code, (int)error.reason.length,
                    error.reason.pointer ? error.reason.pointer : "", (int)error.path.length,
                    error.path.pointer ? error.path.pointer : "");
        free(decoded);
    }
    ta_growable_arena_free(&arena.arena);
}

/* Params written with the CBOR writer, checked against a run.* method. */
typedef struct test_params {
    unsigned char room[256];
    ta_cbor_writer writer;
    test_document decoded;
} test_params;

static void params_begin(test_params *params) {
    ta_cbor_writer_init(&params->writer, (ta_buffer){params->room, sizeof params->room, 0}, NULL);
}

static ta_status params_check(test_params *params, uint32_t method, ta_growable_arena *arena, tool_schema_error *error) {
    ta_bytes bytes;
    if (ta_cbor_writer_bytes(&params->writer, &bytes) != TA_OK || !document_decode(&params->decoded, bytes))
        return TA_ERROR_FATAL;
    tool_schema_input input = {&tool_schema_run, &params->decoded.document, 0, 0, ta_string("params"), arena};
    return tool_schema_check_params(method, &input, error);
}

static void test_run_schema(void) {
    test_arena arena;
    arena_begin(&arena);
    tool_schema_error error;
    static test_params params;

    CHECK_EQUAL(tool_schema_method_find(&tool_schema_run, ta_string("run.spawn")), TOOL_SCHEMA_RUN_METHOD_RUN_SPAWN);
    CHECK_EQUAL(tool_schema_method_find(&tool_schema_run, ta_string("actor.state")), TOOL_SCHEMA_RUN_METHOD_ACTOR_STATE);
    CHECK_EQUAL(tool_schema_method_find(&tool_schema_run, ta_string("host.future")), TOOL_SCHEMA_RUN_METHOD_COUNT);
    CHECK_EQUAL(tool_schema_run.method_count, TOOL_SCHEMA_RUN_METHOD_COUNT);
    for (uint32_t i = 0; i < tool_schema_run.method_count; i++) {
        CHECK(tool_schema_run.methods[i].params != TA_NIL);
        CHECK(tool_schema_run.methods[i].result != TA_NIL);
    }
    ta_bytes text = tool_schema_text();
    CHECK(text.length > 1000 && ((const char *)text.pointer)[0] == '{');

    /* {"chart": "order", "params": {"total": 4200}} */
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 2);
    ta_cbor_write_text(&params.writer, ta_string("chart"));
    ta_cbor_write_text(&params.writer, ta_string("order"));
    ta_cbor_write_text(&params.writer, ta_string("params"));
    ta_cbor_write_map(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("total"));
    ta_cbor_write_uint(&params.writer, 4200);
    CHECK_EQUAL(params_check(&params, TOOL_SCHEMA_RUN_METHOD_RUN_SPAWN, &arena.arena, &error), TA_OK);

    /* {"target": 1, "event": "checkout", "data": 1, "cbor": "AQ"}: not both */
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 4);
    ta_cbor_write_text(&params.writer, ta_string("target"));
    ta_cbor_write_uint(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("event"));
    ta_cbor_write_text(&params.writer, ta_string("checkout"));
    ta_cbor_write_text(&params.writer, ta_string("data"));
    ta_cbor_write_uint(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("cbor"));
    ta_cbor_write_text(&params.writer, ta_string("AQ"));
    CHECK_EQUAL(params_check(&params, TOOL_SCHEMA_RUN_METHOD_RUN_SEND, &arena.arena, &error), TA_ERROR_INVALID);
    CHECK(error.code == -32602 && view_is(error.reason, ta_string("range")) &&
          view_is(error.path, ta_string("params.cbor")));

    /* {"events": [{"target": 0, "event": "x"}]}: a session from 1 */
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("events"));
    ta_cbor_write_array(&params.writer, 1);
    ta_cbor_write_map(&params.writer, 2);
    ta_cbor_write_text(&params.writer, ta_string("target"));
    ta_cbor_write_uint(&params.writer, 0);
    ta_cbor_write_text(&params.writer, ta_string("event"));
    ta_cbor_write_text(&params.writer, ta_string("x"));
    CHECK_EQUAL(params_check(&params, TOOL_SCHEMA_RUN_METHOD_RUN_SEND_MANY, &arena.arena, &error), TA_ERROR_INVALID);
    CHECK(view_is(error.reason, ta_string("range")) && view_is(error.path, ta_string("params.events[0].target")));

    /* {"by": "30s"} and {"by": "30"} */
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("by"));
    ta_cbor_write_text(&params.writer, ta_string("30s"));
    CHECK_EQUAL(params_check(&params, TOOL_SCHEMA_RUN_METHOD_RUN_ADVANCE_TIME, &arena.arena, &error), TA_OK);
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("by"));
    ta_cbor_write_text(&params.writer, ta_string("30"));
    CHECK_EQUAL(params_check(&params, TOOL_SCHEMA_RUN_METHOD_RUN_ADVANCE_TIME, &arena.arena, &error),
                TA_ERROR_INVALID);
    CHECK(view_is(error.path, ta_string("params.by")));

    /* params absent: checked as an empty object */
    tool_schema_input absent = {&tool_schema_run, 0, TA_NIL, 0, ta_string("params"), &arena.arena};
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_RUN_TURN, &absent, &error), TA_OK);
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_RUN_SPAWN, &absent, &error), TA_ERROR_INVALID);
    CHECK(view_is(error.reason, ta_string("missing")) && view_is(error.path, ta_string("params.chart")));
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_COUNT, &absent, &error), TA_ERROR_INVALID);

    /* a notification's params against its definition */
    params_begin(&params);
    ta_cbor_write_map(&params.writer, 3);
    ta_cbor_write_text(&params.writer, ta_string("batch"));
    ta_cbor_write_uint(&params.writer, 1);
    ta_cbor_write_text(&params.writer, ta_string("index"));
    ta_cbor_write_uint(&params.writer, 0);
    ta_cbor_write_text(&params.writer, ta_string("session"));
    ta_cbor_write_uint(&params.writer, 1);
    ta_bytes bytes;
    CHECK(ta_cbor_writer_bytes(&params.writer, &bytes) == TA_OK && document_decode(&params.decoded, bytes));
    tool_schema_input notification = {&tool_schema_run, &params.decoded.document, 0, 0, ta_string("params"),
                                      &arena.arena};
    CHECK_EQUAL(tool_schema_check_message(TOOL_SCHEMA_RUN_NODE_EFFECT_LOG_PARAMS, &notification, &error), TA_OK);
    CHECK_EQUAL(tool_schema_check_message(TOOL_SCHEMA_RUN_NODE_EFFECT_ERROR_PARAMS, &notification, &error),
                TA_ERROR_INVALID);
    CHECK(view_is(error.reason, ta_string("missing")) && view_is(error.path, ta_string("params.status")));
    CHECK_EQUAL(tool_schema_check_message(tool_schema_run.node_count, &notification, &error), TA_ERROR_INVALID);
    ta_growable_arena_free(&arena.arena);
}

/* The path is the only allocation: an arena that refuses it gives
 * TA_ERROR_NO_MEMORY, and a refusal at the root, whose path is empty,
 * needs none. */
static void test_out_of_memory(void) {
    test_arena arena;
    arena_begin(&arena);
    arena.refuse = 1;
    tool_schema_error error;
    tool_schema_input absent = {&tool_schema_run, 0, TA_NIL, 0, ta_string("params"), &arena.arena};
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_RUN_SPAWN, &absent, &error), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_RUN_TURN, &absent, &error), TA_OK);
    static const unsigned char text[] = {0x61, 0x78}; /* "x": not an object */
    test_document *decoded = (test_document *)malloc(sizeof *decoded);
    CHECK(decoded && document_decode(decoded, (ta_bytes){text, sizeof text}));
    if (decoded) {
        tool_schema_input root = {&tool_schema_run, &decoded->document, 0, 0, {0, 0}, &arena.arena};
        CHECK_EQUAL(tool_schema_check_message(TOOL_SCHEMA_RUN_NODE_RUN_TURN_PARAMS, &root, &error), TA_ERROR_INVALID);
        CHECK(view_is(error.reason, ta_string("type")) && error.path.length == 0);
        free(decoded);
    }
    arena.refuse = 0;
    CHECK_EQUAL(tool_schema_check_params(TOOL_SCHEMA_RUN_METHOD_RUN_SPAWN, &absent, &error), TA_ERROR_INVALID);
    CHECK(view_is(error.path, ta_string("params.chart")));
    ta_growable_arena_free(&arena.arena);
}

int main(void) {
    RUN(test_fixtures);
    RUN(test_run_schema);
    RUN(test_out_of_memory);
    TEST_MAIN_END();
}
