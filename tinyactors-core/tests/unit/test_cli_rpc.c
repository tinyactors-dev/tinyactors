/* Unit tests for cli/rpc: lines cut as they come (split across feeds,
 * "\r\n", blank lines, an unended last line, a line past the limit skipped
 * to its newline, the block given back after a large line), envelopes read
 * and refused with core's codes, reasons and paths, batches, the message a
 * forwarded request becomes, responses and notifications as JSON lines, and
 * the allocators refusing at every count until the call succeeds. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "cli/rpc.h"

/* ---- a heap that refuses after a budget ---------------------------------------- */

typedef struct test_heap {
    long budget; /* allocations left; negative: no limit */
    long made;
} test_heap;

static void *test_alloc(void *context, size_t size, size_t align) {
    (void)align;
    test_heap *heap = (test_heap *)context;
    if (heap->budget == 0) return NULL;
    if (heap->budget > 0) heap->budget--;
    heap->made++;
    return malloc(size);
}

static void test_free(void *context, void *pointer, size_t size) {
    (void)context;
    (void)size;
    free(pointer);
}

static ta_bytes bytes_of(const char *text) {
    return (ta_bytes){text, strlen(text)};
}

static int text_is(ta_string_view text, const char *expected) {
    int same = text.length == strlen(expected) && (!text.length || !memcmp(text.pointer, expected, text.length));
    if (!same) fprintf(stderr, "  '%.*s' (expected '%s')\n", (int)text.length, text.pointer ? text.pointer : "", expected);
    return same;
}

/* ---- lines ---------------------------------------------------------------------- */

typedef struct line_reader {
    test_heap heap;
    ta_allocator allocator;
    ta_rpc_lines lines;
} line_reader;

static void line_reader_init(line_reader *reader, size_t limit) {
    reader->heap = (test_heap){-1, 0};
    reader->allocator = (ta_allocator){&reader->heap, test_alloc, test_free};
    ta_rpc_lines_config config = {&reader->allocator, limit};
    ta_rpc_lines_init(&reader->lines, &config);
}

/* The next line is `expected` (NULL: none is whole; "!": a line too long). */
static int next_is(line_reader *reader, const char *expected) {
    ta_rpc_line line;
    int got = ta_rpc_lines_next(&reader->lines, &line);
    if (!expected) {
        if (got) fprintf(stderr, "  a line '%.*s' where none was expected\n", (int)line.text.length, line.text.pointer);
        return !got;
    }
    if (!got) {
        fprintf(stderr, "  no line where '%s' was expected\n", expected);
        return 0;
    }
    if (!strcmp(expected, "!")) return line.too_long && !line.text.length;
    return !line.too_long && text_is(line.text, expected);
}

static void test_lines_as_they_come(void) {
    line_reader reader;
    line_reader_init(&reader, 64);
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("{\"a\"")), TA_OK);
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of(":1}\n\n  \t\r\n{\"b\":2}\r\n{\"c\"")), TA_OK);
    CHECK(next_is(&reader, "{\"a\":1}"));
    CHECK(next_is(&reader, "{\"b\":2}"));
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of(":3}")), TA_OK);
    CHECK(next_is(&reader, NULL));
    /* the end of input: the unended last line is whole, once */
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, (ta_bytes){NULL, 0}), TA_OK);
    CHECK(next_is(&reader, "{\"c\":3}"));
    CHECK(next_is(&reader, NULL));
    ta_rpc_lines_free(&reader.lines);

    /* a "\r" alone at the end of input, and a byte at a time */
    line_reader_init(&reader, 64);
    const char *text = "x\ny\r";
    for (size_t i = 0; text[i]; i++)
        CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, (ta_bytes){text + i, 1}), TA_OK);
    CHECK(next_is(&reader, "x"));
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, (ta_bytes){NULL, 0}), TA_OK);
    CHECK(next_is(&reader, "y"));
    CHECK(next_is(&reader, NULL));
    ta_rpc_lines_free(&reader.lines);
}

static void test_lines_past_the_limit(void) {
    line_reader reader;
    line_reader_init(&reader, 8);
    /* whole in one chunk: reported, and the next line read */
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("12345678\n123456789\nok\n")), TA_OK);
    CHECK(next_is(&reader, "12345678"));
    CHECK(next_is(&reader, "!"));
    CHECK(next_is(&reader, "ok"));
    /* past the limit before its newline: reported at once, the rest dropped as it comes */
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("aaaaaaaaaa")), TA_OK);
    CHECK(next_is(&reader, "!"));
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("aaaaaaaaaaaaaaaaaaaa")), TA_OK);
    CHECK(next_is(&reader, NULL));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("aaa\nnext\n")), TA_OK);
    CHECK(next_is(&reader, "next"));
    CHECK(next_is(&reader, NULL));
    /* a "\r" does not count against the limit */
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("12345678\r\n")), TA_OK);
    CHECK(next_is(&reader, "12345678"));
    ta_rpc_lines_free(&reader.lines);
}

/* After a line past TA_RPC_LINES_KEEP, the block is given back at the next
 * feed and what is left moves to a small one. */
static void test_lines_give_the_block_back(void) {
    line_reader reader;
    size_t large = (size_t)TA_RPC_LINES_KEEP * 2;
    line_reader_init(&reader, large * 2);
    char *line = (char *)malloc(large + 1);
    memset(line, 'x', large);
    line[large] = '\n';
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, (ta_bytes){line, large + 1}), TA_OK);
    CHECK(reader.lines.pending.capacity > (size_t)TA_RPC_LINES_KEEP);
    ta_rpc_line out;
    CHECK(ta_rpc_lines_next(&reader.lines, &out) && out.text.length == large);
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("small\n")), TA_OK);
    CHECK(reader.lines.pending.capacity <= (size_t)TA_RPC_LINES_KEEP);
    CHECK(next_is(&reader, "small"));
    ta_rpc_lines_free(&reader.lines);

    /* the same after a line too long, dropped */
    line_reader_init(&reader, 16);
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, (ta_bytes){line, large}), TA_OK);
    CHECK(next_is(&reader, "!"));
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("x\nsmall\n")), TA_OK);
    CHECK(reader.lines.pending.capacity <= (size_t)TA_RPC_LINES_KEEP);
    CHECK(next_is(&reader, "small"));
    ta_rpc_lines_free(&reader.lines);
    free(line);
}

/* A feed the allocator refuses takes nothing; the next one goes on. */
static void test_lines_out_of_memory(void) {
    line_reader reader;
    line_reader_init(&reader, 1024);
    reader.heap.budget = 0;
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("lost\n")), TA_ERROR_NO_MEMORY);
    CHECK(next_is(&reader, NULL));
    reader.heap.budget = -1;
    CHECK_EQUAL(ta_rpc_lines_feed(&reader.lines, bytes_of("kept\n")), TA_OK);
    CHECK(next_is(&reader, "kept"));
    ta_rpc_lines_free(&reader.lines);
}

/* ---- messages ------------------------------------------------------------------- */

typedef struct message_reader {
    test_heap heap;
    ta_allocator allocator;
    ta_growable_arena arena;
} message_reader;

static void message_reader_init(message_reader *reader, long budget) {
    reader->heap = (test_heap){budget, 0};
    reader->allocator = (ta_allocator){&reader->heap, test_alloc, test_free};
    ta_growable_arena_config config = {sizeof config, 0, 64, &reader->allocator};
    ta_growable_arena_init(&reader->arena, &config);
}

static ta_status parse(message_reader *reader, const char *line, ta_rpc_message *out) {
    ta_rpc_parsing parsing = {ta_string(line), &reader->arena};
    return ta_rpc_parse(&parsing, out);
}

static void test_requests(void) {
    message_reader reader;
    message_reader_init(&reader, -1);
    ta_rpc_message message;
    CHECK_EQUAL(parse(&reader, "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"run.turn\",\"params\":{\"runs\":2}}", &message),
                TA_OK);
    CHECK_EQUAL(message.error.code, 0);
    CHECK_EQUAL(message.kind, TA_RPC_REQUEST);
    CHECK(text_is(ta_cbor_string(&message.document, message.method), "run.turn"));
    CHECK_EQUAL(message.document.tokens[message.params].kind, TA_CBOR_MAP);
    CHECK_EQUAL(message.document.tokens[message.id].value, 7);

    /* every kind of id; a notification without one; no params */
    static const char *const requests[] = {
        "{\"jsonrpc\":\"2.0\",\"id\":\"a\",\"method\":\"m\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"m\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":-3,\"method\":\"m\"}",
        "{\"jsonrpc\":\"2.0\",\"id\":3.0,\"method\":\"m\"}",
    };
    for (size_t i = 0; i < sizeof requests / sizeof requests[0]; i++) {
        CHECK_EQUAL(parse(&reader, requests[i], &message), TA_OK);
        CHECK_EQUAL(message.error.code, 0);
        CHECK_EQUAL(message.kind, TA_RPC_REQUEST);
        CHECK_EQUAL(message.params, TA_NIL);
    }
    CHECK_EQUAL(parse(&reader, "{\"method\":\"m\",\"jsonrpc\":\"2.0\",\"params\":{}}", &message), TA_OK);
    CHECK_EQUAL(message.error.code, 0);
    CHECK_EQUAL(message.kind, TA_RPC_NOTIFICATION);
    CHECK_EQUAL(message.id, TA_NIL);
    ta_growable_arena_free(&reader.arena);
}

/* What is not a request, with core's code, reason and path. */
static void test_refusals(void) {
    static const struct {
        const char *line;
        int32_t code;
        const char *reason, *path;
    } cases[] = {
        {"{", TA_RPC_PARSE_ERROR, "malformed", ""},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"method\":\"n\"}", TA_RPC_PARSE_ERROR, "malformed", ""},
        {"\"\\ud800\"", TA_RPC_PARSE_ERROR, "malformed", ""},
        {"1", TA_RPC_INVALID_REQUEST, "type", ""},
        {"null", TA_RPC_INVALID_REQUEST, "type", ""},
        {"[]", TA_RPC_INVALID_REQUEST, "missing", ""},
        {"{\"method\":\"m\"}", TA_RPC_INVALID_REQUEST, "missing", "jsonrpc"},
        {"{\"jsonrpc\":\"1.0\",\"method\":\"m\"}", TA_RPC_INVALID_REQUEST, "type", "jsonrpc"},
        {"{\"jsonrpc\":2,\"method\":\"m\"}", TA_RPC_INVALID_REQUEST, "type", "jsonrpc"},
        {"{\"jsonrpc\":\"2.0\"}", TA_RPC_INVALID_REQUEST, "missing", "method"},
        {"{\"jsonrpc\":\"2.0\",\"method\":1}", TA_RPC_INVALID_REQUEST, "type", "method"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"params\":[1]}", TA_RPC_INVALID_REQUEST, "type", "params"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"id\":1.5}", TA_RPC_INVALID_REQUEST, "type", "id"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"id\":true}", TA_RPC_INVALID_REQUEST, "type", "id"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"id\":{}}", TA_RPC_INVALID_REQUEST, "type", "id"},
        {"{\"jsonrpc\":\"2.0\",\"method\":\"m\",\"result\":1}", TA_RPC_INVALID_REQUEST, "unknown", "result"},
    };
    message_reader reader;
    message_reader_init(&reader, -1);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        ta_rpc_message message;
        CHECK_EQUAL(parse(&reader, cases[i].line, &message), TA_OK);
        int same = message.error.code == cases[i].code && text_is(message.error.reason, cases[i].reason) &&
                   text_is(message.error.path, cases[i].path) && message.error.message.length > 0;
        if (!same) fprintf(stderr, "  %s: code %d\n", cases[i].line, message.error.code);
        CHECK(same);
    }
    /* nested past the reader's depth */
    char deep[200];
    size_t at = 0;
    for (int i = 0; i < 70; i++)
        deep[at++] = '[';
    for (int i = 0; i < 70; i++)
        deep[at++] = ']';
    deep[at] = 0;
    ta_rpc_message message;
    CHECK_EQUAL(parse(&reader, deep, &message), TA_OK);
    CHECK_EQUAL(message.error.code, TA_RPC_INVALID_REQUEST);
    CHECK(text_is(message.error.reason, "limit"));
    ta_growable_arena_free(&reader.arena);
}

static void test_batches(void) {
    message_reader reader;
    message_reader_init(&reader, -1);
    ta_rpc_message batch;
    CHECK_EQUAL(parse(&reader,
                      "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"a\",\"params\":{\"x\":[1,{\"y\":2}]}},"
                      "{\"jsonrpc\":\"2.0\",\"method\":\"b\"},7,{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"c\"}]",
                      &batch),
                TA_OK);
    CHECK_EQUAL(batch.error.code, 0);
    CHECK_EQUAL(batch.kind, TA_RPC_BATCH);
    CHECK_EQUAL(batch.count, 4);
    static const struct {
        uint32_t kind;
        int32_t code;
        const char *method;
    } expected[] = {{TA_RPC_REQUEST, 0, "a"},
                    {TA_RPC_NOTIFICATION, 0, "b"},
                    {TA_RPC_NOTIFICATION, TA_RPC_INVALID_REQUEST, ""},
                    {TA_RPC_REQUEST, 0, "c"}};
    uint32_t at = batch.at + 1;
    for (uint32_t i = 0; i < batch.count; i++) {
        ta_rpc_message element;
        ta_rpc_element_read(&(ta_rpc_element){&batch, at, 0}, &element);
        CHECK_EQUAL(element.error.code, expected[i].code);
        if (!expected[i].code) {
            CHECK_EQUAL(element.kind, expected[i].kind);
            CHECK(text_is(ta_cbor_string(&element.document, element.method), expected[i].method));
        }
        at = ta_cbor_next(&batch.document, at);
    }
    CHECK_EQUAL(at, batch.document.count);
    ta_growable_arena_free(&reader.arena);
}

/* ---- writing -------------------------------------------------------------------- */

/* A CBOR item written by `writer` as its JSON line. */
static int line_is(ta_cbor_writer *writer, const char *expected) {
    ta_bytes cbor;
    if (ta_cbor_writer_bytes(writer, &cbor) != TA_OK) {
        fprintf(stderr, "  the writer failed (expected %s)\n", expected);
        return 0;
    }
    unsigned char room[1024];
    ta_output out = ta_output_over((ta_buffer){room, sizeof room, 0}, NULL);
    if (ta_rpc_line_write(cbor, &out) != TA_OK) return 0;
    return text_is((ta_string_view){(const char *)out.bytes, (uint32_t)out.length}, expected);
}

static void test_forward(void) {
    message_reader reader;
    message_reader_init(&reader, -1);
    ta_rpc_message message;
    CHECK_EQUAL(parse(&reader,
                      "{\"jsonrpc\":\"2.0\",\"id\":\"mine\",\"method\":\"actor.state\",\"params\":{\"session\":"
                      "18446744073709551615,\"x\":[-1,1.5,\"t\",null,true]}}",
                      &message),
                TA_OK);
    ta_allocator allocator = ta_growable_arena_allocator(&reader.arena);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    ta_rpc_forward_write(&(ta_rpc_forward){&message, 42}, &writer);
    CHECK(line_is(&writer, "{\"method\":\"actor.state\",\"params\":{\"session\":18446744073709551615,"
                           "\"x\":[-1,1.5,\"t\",null,true]},\"id\":42}\n"));
    /* a notification stays one */
    CHECK_EQUAL(parse(&reader, "{\"jsonrpc\":\"2.0\",\"method\":\"trace.end\"}", &message), TA_OK);
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    ta_rpc_forward_write(&(ta_rpc_forward){&message, 42}, &writer);
    CHECK(line_is(&writer, "{\"method\":\"trace.end\"}\n"));
    ta_growable_arena_free(&reader.arena);
}

static void test_responses_and_notifications(void) {
    message_reader reader;
    message_reader_init(&reader, -1);
    ta_allocator allocator = ta_growable_arena_allocator(&reader.arena);
    ta_rpc_message message;
    CHECK_EQUAL(parse(&reader, "{\"jsonrpc\":\"2.0\",\"id\":\"q\\\"1\",\"method\":\"m\",\"params\":{\"a\":[1]}}",
                      &message),
                TA_OK);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    ta_rpc_response response = {{&message.document, message.id, 0}, {&message.document, message.params, 0}, NULL};
    ta_rpc_response_write(&response, &writer);
    CHECK(line_is(&writer, "{\"jsonrpc\":\"2.0\",\"id\":\"q\\\"1\",\"result\":{\"a\":[1]}}\n"));

    ta_rpc_error error = {TA_RPC_STATE, 0, ta_string("state"), ta_string("params.session"), ta_string("not now"),
                          ta_string("TA_ERROR_STATE")};
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    response = (ta_rpc_response){{NULL, TA_NIL, 0}, {NULL, 0, 0}, &error};
    ta_rpc_response_write(&response, &writer);
    CHECK(line_is(&writer, "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32002,\"message\":\"not now\","
                           "\"data\":{\"reason\":\"state\",\"path\":\"params.session\",\"status\":\"TA_ERROR_STATE\"}}}\n"));
    error.status = ta_string("");
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    response = (ta_rpc_response){{&message.document, message.id, 0}, {NULL, 0, 0}, &error};
    ta_rpc_response_write(&response, &writer);
    CHECK(line_is(&writer, "{\"jsonrpc\":\"2.0\",\"id\":\"q\\\"1\",\"error\":{\"code\":-32002,\"message\":\"not now\","
                           "\"data\":{\"reason\":\"state\",\"path\":\"params.session\"}}}\n"));
    /* an empty result */
    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    response = (ta_rpc_response){{&message.document, message.id, 0}, {NULL, 0, 0}, NULL};
    ta_rpc_response_write(&response, &writer);
    CHECK(line_is(&writer, "{\"jsonrpc\":\"2.0\",\"id\":\"q\\\"1\",\"result\":{}}\n"));

    ta_cbor_writer_init(&writer, (ta_buffer){0, 0, 0}, &allocator);
    ta_rpc_notification_begin(&writer, ta_string("effect.log"));
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("session"));
    ta_cbor_write_uint(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("text"));
    ta_cbor_write_text(&writer, ta_string("line\none"));
    CHECK(line_is(&writer, "{\"jsonrpc\":\"2.0\",\"method\":\"effect.log\",\"params\":{\"session\":1,"
                           "\"text\":\"line\\none\"}}\n"));

    /* the line's output refusing to grow */
    unsigned char room[8];
    ta_output small = ta_output_over((ta_buffer){room, sizeof room, 0}, NULL);
    ta_bytes cbor;
    CHECK_EQUAL(ta_cbor_writer_bytes(&writer, &cbor), TA_OK);
    CHECK_EQUAL(ta_rpc_line_write(cbor, &small), TA_ERROR_NO_MEMORY);
    ta_growable_arena_free(&reader.arena);
}

/* The arena refusing at each allocation until the parse succeeds: every
 * refusal is TA_ERROR_NO_MEMORY, and the parse that succeeds reads the same. */
static void test_parse_out_of_memory(void) {
    static const char line[] =
        "[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"run.send\",\"params\":{\"target\":1,\"event\":\"e\\u00e9\","
        "\"data\":{\"items\":[1,2,3,{\"sku\":\"ab\\ncd\"}]}}},{\"jsonrpc\":\"2.0\",\"method\":\"run.turn\"}]";
    int succeeded = 0;
    for (long budget = 0; budget < 64 && !succeeded; budget++) {
        message_reader reader;
        message_reader_init(&reader, budget);
        ta_rpc_message message;
        ta_status status = parse(&reader, line, &message);
        if (status == TA_OK) {
            succeeded = 1;
            CHECK_EQUAL(message.error.code, 0);
            CHECK_EQUAL(message.kind, TA_RPC_BATCH);
            CHECK_EQUAL(message.count, 2);
        } else {
            CHECK_EQUAL(status, TA_ERROR_NO_MEMORY);
        }
        ta_growable_arena_free(&reader.arena);
    }
    CHECK(succeeded);
}

int main(void) {
    RUN(test_lines_as_they_come);
    RUN(test_lines_past_the_limit);
    RUN(test_lines_give_the_block_back);
    RUN(test_lines_out_of_memory);
    RUN(test_requests);
    RUN(test_refusals);
    RUN(test_batches);
    RUN(test_forward);
    RUN(test_responses_and_notifications);
    RUN(test_parse_out_of_memory);
    TEST_MAIN_END();
}
