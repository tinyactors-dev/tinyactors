/* Unit tests for cli/json: JSON text to one CBOR item (RFC 8949, section
 * 6.2) and back (6.1). Cases by kind, refusals with their offsets, the
 * suite's values.txt vectors written as JSON (tests/json/values.txt holds
 * what each writes), and the allocators refusing at every count until the
 * call succeeds. */
#include <stdlib.h>
#include <string.h>

#include "../harness/harness.h"
#include "cli/json.h"

#ifndef TA_VALUES
#define TA_VALUES "../tinyactors-test/suites/protocol/values.txt"
#endif
#ifndef TA_JSON_VALUES
#define TA_JSON_VALUES "tests/json/values.txt"
#endif

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

/* ---- hexadecimal -------------------------------------------------------------- */

static size_t hex_decode(const char *hex, unsigned char *out) {
    size_t length = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned int byte;
        sscanf(hex, "%2x", &byte);
        out[length++] = (unsigned char)byte;
    }
    return length;
}

static void hex_print(const unsigned char *bytes, size_t length) {
    for (size_t i = 0; i < length; i++)
        fprintf(stderr, "%02x", bytes[i]);
}

/* ---- reading ------------------------------------------------------------------ */

/* A reading's outcome: its status, its CBOR (copied, malloc'd) and failure. */
typedef struct read_outcome {
    ta_status status;
    unsigned char *cbor;
    size_t length;
    ta_json_failure failure;
} read_outcome;

typedef struct read_heaps {
    test_heap writer, arena;
} read_heaps;

static read_outcome read_json(ta_string_view text, read_heaps *heaps) {
    ta_allocator writer_allocator = {&heaps->writer, test_alloc, test_free};
    ta_allocator arena_allocator = {&heaps->arena, test_alloc, test_free};
    ta_growable_arena arena;
    ta_growable_arena_config config = {sizeof config, 0, 64, &arena_allocator};
    ta_growable_arena_init(&arena, &config);
    unsigned char room[16];
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){room, sizeof room, 0}, &writer_allocator);
    ta_json_reading reading = {text, &writer, &arena, {0, {"", 0}}};
    read_outcome outcome = {ta_json_read(&reading), NULL, 0, reading.failure};
    ta_bytes bytes;
    if (outcome.status == TA_OK && ta_cbor_writer_bytes(&writer, &bytes) == TA_OK) {
        outcome.cbor = (unsigned char *)malloc(bytes.length + 1);
        memcpy(outcome.cbor, bytes.pointer, bytes.length);
        outcome.length = bytes.length;
    }
    ta_cbor_writer_free(&writer);
    ta_growable_arena_free(&arena);
    return outcome;
}

static read_outcome read_plain(ta_string_view text) {
    read_heaps heaps = {{-1, 0}, {-1, 0}};
    return read_json(text, &heaps);
}

/* A literal as a view, NUL bytes inside it included. */
#define TEXT(literal) ((ta_string_view){(literal), (uint32_t)(sizeof(literal) - 1)})

static int reads_as(ta_string_view text, const char *hex) {
    unsigned char expected[256];
    size_t length = hex_decode(hex, expected);
    read_outcome outcome = read_plain(text);
    int same = outcome.status == TA_OK && outcome.length == length && memcmp(outcome.cbor, expected, length) == 0;
    if (!same) {
        fprintf(stderr, "  '%.*s': status %d, ", (int)text.length, text.pointer, (int)outcome.status);
        if (outcome.cbor) hex_print(outcome.cbor, outcome.length);
        fprintf(stderr, " (expected %s)\n", hex);
    }
    free(outcome.cbor);
    return same;
}

/* Every kind of value, and the numbers at the edges of the integers. */
static void test_read_values(void) {
    static const struct {
        const char *text, *hex;
    } cases[] = {
        {"0", "00"},
        {"7", "07"},
        {"7.0", "07"},
        {"7e0", "07"},
        {"70e-1", "07"},
        {"0.7e1", "07"},
        {"1e3", "1903e8"},
        {"1E+3", "1903e8"},
        {"0.0", "00"},
        {"0e999999", "00"},
        {"-1", "20"},
        {"-1.0", "20"},
        {"-0", "f98000"},
        {"-0.0", "f98000"},
        {"-0e5", "f98000"},
        {"1.5", "f93e00"},
        {"0.1", "fb3fb999999999999a"},
        {"-2.5", "f9c100"},
        {"1.0e+300", "fb7e37e43c8800759c"},
        {"9007199254740993", "1b0020000000000001"},
        {"18446744073709551615", "1bffffffffffffffff"},
        {"1.8446744073709551615e19", "1bffffffffffffffff"},
        {"18446744073709551616", "fa5f800000"},
        {"-18446744073709551616", "3bffffffffffffffff"},
        {"-18446744073709551617", "fadf800000"},
        {"-9223372036854775809", "3b8000000000000000"},
        {"100000000000000000000", "fb4415af1d78b58c40"},
        {"12345678901234567890e-10", "fb41d26580b487e6b7"},
        {"true", "f5"},
        {"false", "f4"},
        {"null", "f6"},
        {"\"\"", "60"},
        {"\"text\"", "6474657874"},
        {"\"\xc3\xbc" "n\xc3\xaf" "c\xc3\xb8" "d\xc3\xa9 \xe2\x9c\x93\"", "6fc3bc6ec3af63c3b864c3a920e29c93"},
        {"\"a\\u00e9\\ud83d\\ude00\\n\"", "6861c3a9f09f98800a"},
        {"\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"", "68225c2f080c0a0d09"},
        {"\"\\u0000\"", "6100"},
        {"\"\\uFFFF\"", "63efbfbf"},
        {"\"\xf4\x8f\xbf\xbf\"", "64f48fbfbf"},
        {"[]", "80"},
        {"{}", "a0"},
        {"[1,[2,{}]]", "8201820" "2a0"},
        {" { \"a\" : [ ] , \"b\":null }\r\n", "a261618061" "62f6"},
        {"{\"$bytes\":\"AA==\"}", "a1662462797465736441413d3d"},
        {"[1,-2.5,0,true,false,null,\"\",\"text\"]", "8801f9c10000f5f4f660647465787" "4"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(reads_as(ta_string(cases[i].text), cases[i].hex));
}

/* What is refused, where, and why. */
static void test_read_refusals(void) {
    static const struct {
        const char *text;
        ta_status status;
        size_t offset;
    } cases[] = {
        {"", TA_ERROR_INVALID, 0},
        {"   ", TA_ERROR_INVALID, 3},
        {"[", TA_ERROR_INVALID, 1},
        {"[1,]", TA_ERROR_INVALID, 3},
        {"[1 2]", TA_ERROR_INVALID, 3},
        {"{\"a\" 1}", TA_ERROR_INVALID, 5},
        {"{1:2}", TA_ERROR_INVALID, 1},
        {"{\"a\":1,}", TA_ERROR_INVALID, 7},
        {"{\"a\":1 \"b\":2}", TA_ERROR_INVALID, 7},
        {"{\"a\":1,\"a\":2}", TA_ERROR_INVALID, 7},
        {"{\"a\":1,\"\\u0061\":2}", TA_ERROR_INVALID, 7},
        {"{\"a\":{\"a\":1,\"b\":2},\"b\":{\"b\":3,\"b\":4}}", TA_ERROR_INVALID, 30},
        {"1 2", TA_ERROR_INVALID, 2},
        {"01", TA_ERROR_INVALID, 1},
        {"-", TA_ERROR_INVALID, 1},
        {"1.", TA_ERROR_INVALID, 2},
        {"1e", TA_ERROR_INVALID, 2},
        {".5", TA_ERROR_INVALID, 0},
        {"+1", TA_ERROR_INVALID, 0},
        {"1e400", TA_ERROR_INVALID, 0},
        {"[-1e400]", TA_ERROR_INVALID, 1},
        {"NaN", TA_ERROR_INVALID, 0},
        {"tru", TA_ERROR_INVALID, 0},
        {"nul", TA_ERROR_INVALID, 0},
        {"\"abc", TA_ERROR_INVALID, 0},
        {"\"a\nb\"", TA_ERROR_INVALID, 2},
        {"\"a\x7f\"", TA_OK, 0},
        {"\"\\x\"", TA_ERROR_INVALID, 1},
        {"\"\\u12\"", TA_ERROR_INVALID, 1},
        {"\"\\ud800\"", TA_ERROR_INVALID, 1},
        {"\"\\udc00\"", TA_ERROR_INVALID, 1},
        {"\"\\ud800\\u0041\"", TA_ERROR_INVALID, 1},
        {"\"\\ud800\\ud800\"", TA_ERROR_INVALID, 1},
        {"\"x\\ude00\\ud83d\"", TA_ERROR_INVALID, 2},
        {"\"\xff\"", TA_ERROR_INVALID, 1},
        {"\"\xc0\xaf\"", TA_ERROR_INVALID, 1},
        {"\"\xe0\x80\xaf\"", TA_ERROR_INVALID, 1},
        {"\"\xed\xa0\x80\"", TA_ERROR_INVALID, 1},
        {"\"\xf4\x90\x80\x80\"", TA_ERROR_INVALID, 1},
        {"\"\xe2\x9c\"", TA_ERROR_INVALID, 1},
        {"[\"\xc3\"]", TA_ERROR_INVALID, 2},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        read_outcome outcome = read_plain(ta_string(cases[i].text));
        ta_status expected = cases[i].status;
        int same = outcome.status == expected && (expected == TA_OK || outcome.failure.offset == cases[i].offset);
        if (!same)
            fprintf(stderr, "  '%s': status %d at %zu (%.*s); expected %d at %zu\n", cases[i].text,
                    (int)outcome.status, outcome.failure.offset, (int)outcome.failure.reason.length,
                    outcome.failure.reason.pointer, (int)expected, cases[i].offset);
        CHECK(same);
        CHECK(expected == TA_OK || outcome.failure.reason.length > 0);
        free(outcome.cbor);
    }
    read_outcome outcome = read_plain(TEXT("\"a\0b\""));
    CHECK(outcome.status == TA_ERROR_INVALID && outcome.failure.offset == 2);
    free(outcome.cbor);
}

/* 64 containers open at once are read; the 65th is TA_ERROR_LIMIT, at its
 * bracket. */
static void test_read_depth(void) {
    char text[2 * (TA_JSON_MAX_DEPTH + 1) + 1];
    for (uint32_t depth = TA_JSON_MAX_DEPTH; depth <= TA_JSON_MAX_DEPTH + 1; depth++) {
        for (uint32_t i = 0; i < depth; i++) {
            text[i] = '[';
            text[2 * depth - 1 - i] = ']';
        }
        read_outcome outcome = read_plain((ta_string_view){text, 2 * depth});
        if (depth == TA_JSON_MAX_DEPTH) {
            CHECK_EQUAL(outcome.status, TA_OK);
            CHECK_EQUAL(outcome.length, TA_JSON_MAX_DEPTH);
        } else {
            CHECK_EQUAL(outcome.status, TA_ERROR_LIMIT);
            CHECK_EQUAL(outcome.failure.offset, TA_JSON_MAX_DEPTH);
        }
        free(outcome.cbor);
    }
}

/* An object of many keys (its set grows) is read in order; the same key
 * last, spelled with an escape, is refused. */
static void test_read_many_keys(void) {
    enum { KEYS = 2000 };
    char *text = (char *)malloc(KEYS * 24 + 64);
    size_t length = 0;
    text[length++] = '{';
    for (int i = 0; i < KEYS; i++)
        length += (size_t)sprintf(text + length, "%s\"k%d\":%d", i ? "," : "", i, i);
    memcpy(text + length, "}", 1);
    read_outcome outcome = read_plain((ta_string_view){text, (uint32_t)length + 1});
    CHECK_EQUAL(outcome.status, TA_OK);
    CHECK(outcome.length > 3 && outcome.cbor[0] == 0xB9 && outcome.cbor[1] == 0x07 && outcome.cbor[2] == 0xD0);
    free(outcome.cbor);
    length += (size_t)sprintf(text + length, ",\"\\u006b1999\":0}");
    outcome = read_plain((ta_string_view){text, (uint32_t)length});
    CHECK_EQUAL(outcome.status, TA_ERROR_INVALID);
    CHECK(outcome.failure.reason.length && memcmp(outcome.failure.reason.pointer, "a key repeated", 14) == 0);
    free(outcome.cbor);
    free(text);
}

/* ---- writing ------------------------------------------------------------------ */

typedef struct write_outcome {
    ta_status status;
    char text[512];
    size_t length;
    uint32_t changes;
} write_outcome;

static write_outcome write_cbor(const unsigned char *cbor, size_t length) {
    write_outcome outcome;
    test_heap heap = {-1, 0};
    ta_allocator allocator = {&heap, test_alloc, test_free};
    char room[8];
    ta_output out = ta_output_over((ta_buffer){room, sizeof room, 0}, &allocator);
    ta_json_writing writing = {{cbor, length}, &out, 0};
    outcome.status = ta_json_write(&writing);
    outcome.changes = writing.changes;
    outcome.length = out.length < sizeof outcome.text ? out.length : sizeof outcome.text;
    if (ta_output_fits(&out)) memcpy(outcome.text, out.bytes, outcome.length);
    ta_output_free(&out);
    return outcome;
}

static int writes_as(const char *hex, const char *json, uint32_t changes) {
    unsigned char cbor[256];
    size_t length = hex_decode(hex, cbor);
    write_outcome outcome = write_cbor(cbor, length);
    int same = outcome.status == TA_OK && outcome.changes == changes && outcome.length == strlen(json) &&
               memcmp(outcome.text, json, outcome.length) == 0;
    if (!same)
        fprintf(stderr, "  %s: status %d, changes %u, '%.*s' (expected %u, '%s')\n", hex, (int)outcome.status,
                outcome.changes, (int)outcome.length, outcome.text, changes, json);
    return same;
}

static void test_write_values(void) {
    CHECK(writes_as("00", "0", 0));
    CHECK(writes_as("1bffffffffffffffff", "18446744073709551615", 0));
    CHECK(writes_as("20", "-1", 0));
    CHECK(writes_as("3bfffffffffffffffe", "-18446744073709551615", 0));
    CHECK(writes_as("3bffffffffffffffff", "-18446744073709551616", 0));
    CHECK(writes_as("f93c00", "1.0", 0));
    CHECK(writes_as("f98000", "-0.0", 0));
    CHECK(writes_as("fb3fb999999999999a", "0.1", 0));
    CHECK(writes_as("fb7e37e43c8800759c", "1.0e+300", 0));
    CHECK(writes_as("f97e00", "null", TA_JSON_CHANGED_FLOAT));
    CHECK(writes_as("f97c00", "null", TA_JSON_CHANGED_FLOAT));
    CHECK(writes_as("fbfff0000000000000", "null", TA_JSON_CHANGED_FLOAT));
    CHECK(writes_as("f5", "true", 0));
    CHECK(writes_as("f4", "false", 0));
    CHECK(writes_as("f6", "null", 0));
    CHECK(writes_as("f7", "null", TA_JSON_CHANGED_UNDEFINED));
    CHECK(writes_as("f0", "null", TA_JSON_CHANGED_UNDEFINED));
    CHECK(writes_as("40", "\"\"", TA_JSON_CHANGED_BYTES));
    CHECK(writes_as("41ff", "\"_w\"", TA_JSON_CHANGED_BYTES));
    CHECK(writes_as("42fbff", "\"-_8\"", TA_JSON_CHANGED_BYTES));
    CHECK(writes_as("43010203", "\"AQID\"", TA_JSON_CHANGED_BYTES));
    CHECK(writes_as("4401020304", "\"AQIDBA\"", TA_JSON_CHANGED_BYTES));
    CHECK(writes_as("d8206161", "\"a\"", TA_JSON_CHANGED_TAG));
    CHECK(writes_as("c11a514b67b0", "1363896240", TA_JSON_CHANGED_TAG));
    CHECK(writes_as("d820d8206161", "\"a\"", TA_JSON_CHANGED_TAG));
    CHECK(writes_as("6461220a5c", "\"a\\\"\\n\\\\\"", 0));
    CHECK(writes_as("80", "[]", 0));
    CHECK(writes_as("a0", "{}", 0));
    CHECK(writes_as("82a0d82080", "[{},[]]", TA_JSON_CHANGED_TAG));
    CHECK(writes_as("a26161a161628201a16163806164a0", "{\"a\":{\"b\":[1,{\"c\":[]}]},\"d\":{}}", 0));
    CHECK(writes_as("83f7a16161f7f6", "[null,{\"a\":null},null]", TA_JSON_CHANGED_UNDEFINED));
    CHECK(writes_as("8340f7f97e00", "[\"\",null,null]",
                    TA_JSON_CHANGED_BYTES | TA_JSON_CHANGED_UNDEFINED | TA_JSON_CHANGED_FLOAT));
}

static void test_write_refusals(void) {
    static const struct {
        const char *hex;
        ta_status status;
    } cases[] = {
        {"a10102", TA_ERROR_UNSUPPORTED}, /* a key that is not text */
        {"a1d8206161f6", TA_ERROR_UNSUPPORTED},
        {"8201", TA_ERROR_INVALID},       /* truncated */
        {"62c328", TA_ERROR_INVALID},     /* not UTF-8 */
        {"0101", TA_ERROR_INVALID},       /* bytes after the item */
        {"9fff", TA_ERROR_UNSUPPORTED},   /* an indefinite length */
        {"", TA_ERROR_INVALID},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned char cbor[64];
        size_t length = hex_decode(cases[i].hex, cbor);
        write_outcome outcome = write_cbor(cbor, length);
        if (outcome.status != cases[i].status)
            fprintf(stderr, "  %s: status %d, expected %d\n", cases[i].hex, (int)outcome.status, (int)cases[i].status);
        CHECK_EQUAL(outcome.status, cases[i].status);
    }
}

/* Every control character and the characters JSON escapes go out escaped
 * and come back as the same text. */
static void test_escapes_round_trip(void) {
    unsigned char cbor[2 + 0x22];
    cbor[0] = 0x78;
    cbor[1] = 0x22;
    for (uint32_t i = 0; i < 0x20; i++)
        cbor[2 + i] = (unsigned char)i;
    cbor[2 + 0x20] = '"';
    cbor[2 + 0x21] = '\\';
    write_outcome written = write_cbor(cbor, sizeof cbor);
    CHECK_EQUAL(written.status, TA_OK);
    read_outcome back = read_plain((ta_string_view){written.text, (uint32_t)written.length});
    CHECK(back.status == TA_OK && back.length == sizeof cbor && memcmp(back.cbor, cbor, sizeof cbor) == 0);
    free(back.cbor);
}

/* ---- the suite's vectors ------------------------------------------------------- */

static char *file_read(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return NULL;
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    char *bytes = (char *)malloc((size_t)size + 1);
    if (bytes && fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        free(bytes);
        bytes = NULL;
    }
    fclose(file);
    if (bytes) bytes[size] = 0;
    return bytes;
}

/* In tests/json/values.txt, the line for `hex`: "<hex> <changes> <json>".
 * Returns the JSON expected and its changes, or NULL. */
static const char *expected_for(const char *fixture, const char *hex, uint32_t *changes) {
    size_t length = strlen(hex);
    for (const char *line = fixture; *line;) {
        const char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (line[0] != '#' && (size_t)(end - line) > length && strncmp(line, hex, length) == 0 && line[length] == ' ') {
            char *after;
            *changes = (uint32_t)strtoul(line + length + 1, &after, 10);
            return after + 1;
        }
        line = *end ? end + 1 : end;
    }
    return NULL;
}

/* Each `cbor` line of values.txt in a roundtrip or encode case: written as
 * the fixture says, and the JSON written reads back and writes again
 * changing nothing. */
static void test_values_vectors(void) {
    char *values = file_read(TA_VALUES), *fixture = file_read(TA_JSON_VALUES);
    CHECK(values != NULL && fixture != NULL);
    if (!values || !fixture) {
        free(values);
        free(fixture);
        return;
    }
    int writable = 0;
    uint32_t vectors = 0;
    for (char *line = values; *line;) {
        char *end = strchr(line, '\n');
        if (!end) end = line + strlen(line);
        if (strncmp(line, "roundtrip ", 10) == 0 || strncmp(line, "encode ", 7) == 0) writable = 1;
        else if (strncmp(line, "malformed ", 10) == 0 || strncmp(line, "invalid ", 8) == 0) writable = 0;
        if (writable && strncmp(line, "cbor ", 5) == 0) {
            char hex[512];
            size_t hex_length = (size_t)(end - line - 5);
            memcpy(hex, line + 5, hex_length);
            hex[hex_length] = 0;
            unsigned char cbor[256];
            size_t length = hex_decode(hex, cbor);
            write_outcome outcome = write_cbor(cbor, length);
            uint32_t changes = 0;
            const char *json = expected_for(fixture, hex, &changes);
            const char *json_end = json ? strchr(json, '\n') : NULL;
            size_t json_length = json ? (json_end ? (size_t)(json_end - json) : strlen(json)) : 0;
            int same = json && outcome.status == TA_OK && outcome.changes == changes &&
                       outcome.length == json_length && memcmp(outcome.text, json, json_length) == 0;
            if (!same)
                fprintf(stderr, "  values.txt %s: %s %u %.*s\n", json ? "differs" : "not in tests/json/values.txt",
                        hex, outcome.changes, (int)outcome.length, outcome.text);
            CHECK(same);
            read_outcome back = read_plain((ta_string_view){outcome.text, (uint32_t)outcome.length});
            CHECK_EQUAL(back.status, TA_OK);
            write_outcome again = write_cbor(back.cbor, back.length);
            CHECK(again.status == TA_OK && again.changes == 0);
            free(back.cbor);
            vectors++;
        }
        line = *end ? end + 1 : end;
    }
    CHECK(vectors >= 20);
    free(values);
    free(fixture);
}

/* ---- running out of memory ------------------------------------------------------ */

/* The writer's allocator, then the arena's, refuse at every count until the
 * reading succeeds: each refusal is TA_ERROR_NO_MEMORY, and the success
 * writes what an unlimited one does. The text grows the writer, a key set
 * and the scratch of escaped strings. */
static void test_read_out_of_memory(void) {
    static const char text[] =
        "{\"k\\u0030\":\"v\\u00e9\\u00e9\\u00e9\",\"k1\":[1,2,3,4,5,6,7,8,9,10],\"k2\":\"a longer text value\","
        "\"k3\":{\"x\":1},\"k4\":4,\"k5\":5,\"k6\":6,\"k7\":7,\"k\\u0038\":\"\\u00e9 more text to unescape here\","
        "\"k9\":9,\"k10\":10}";
    read_outcome reference = read_plain(TEXT(text));
    CHECK_EQUAL(reference.status, TA_OK);
    for (int which = 0; which < 2; which++) {
        int succeeded = 0;
        for (long budget = 0; budget < 64 && !succeeded; budget++) {
            read_heaps heaps = {{which == 0 ? budget : -1, 0}, {which == 1 ? budget : -1, 0}};
            read_outcome outcome = read_json(TEXT(text), &heaps);
            if (outcome.status == TA_OK) {
                succeeded = 1;
                CHECK(outcome.length == reference.length && memcmp(outcome.cbor, reference.cbor, reference.length) == 0);
                CHECK(budget > 0);
            } else {
                CHECK_EQUAL(outcome.status, TA_ERROR_NO_MEMORY);
            }
            free(outcome.cbor);
        }
        CHECK(succeeded);
    }
    free(reference.cbor);
}

/* The output's allocator refuses at every count until the writing fits:
 * each refusal is TA_ERROR_NO_MEMORY with the length the text needs. */
static void test_write_out_of_memory(void) {
    unsigned char cbor[256];
    size_t length = hex_decode("a36161781e74696e796163746f72733a2f2f7461622d3766332f235f7363786d6c5f3461628a"
                               "0102030405060708090a6163fb3fb999999999999a",
                               cbor);
    write_outcome reference = write_cbor(cbor, length);
    CHECK_EQUAL(reference.status, TA_OK);
    int succeeded = 0;
    for (long budget = 0; budget < 16 && !succeeded; budget++) {
        test_heap heap = {budget, 0};
        ta_allocator allocator = {&heap, test_alloc, test_free};
        char room[8];
        ta_output out = ta_output_over((ta_buffer){room, sizeof room, 0}, &allocator);
        ta_json_writing writing = {{cbor, length}, &out, 0};
        ta_status status = ta_json_write(&writing);
        if (status == TA_OK) {
            succeeded = 1;
            CHECK(out.length == reference.length && memcmp(out.bytes, reference.text, out.length) == 0);
        } else {
            CHECK_EQUAL(status, TA_ERROR_NO_MEMORY);
            CHECK_EQUAL(out.length, reference.length);
        }
        ta_output_free(&out);
    }
    CHECK(succeeded);
}

/* Base64url read back: every length 0 to 40 of bytes as the writer writes
 * them, and what is refused. */
static void test_base64url_read(void) {
    unsigned char bytes[40], cbor[48], room[64];
    for (size_t length = 0; length <= sizeof bytes; length++) {
        for (size_t i = 0; i < length; i++)
            bytes[i] = (unsigned char)(i * 37u + length * 11u + 200u);
        cbor[0] = (unsigned char)(0x58);
        cbor[1] = (unsigned char)length;
        memcpy(cbor + 2, bytes, length);
        write_outcome outcome = write_cbor(cbor, length + 2);
        CHECK(outcome.status == TA_OK && outcome.length >= 2);
        ta_string_view text = {outcome.text + 1, (uint32_t)(outcome.length - 2)}; /* without its quotes */
        ta_buffer out = {room, sizeof room, 0};
        CHECK_EQUAL(ta_json_base64url_read(text, &out), TA_OK);
        CHECK(out.length == length && memcmp(room, bytes, length) == 0);
        if (length) { /* one byte short: the length it needs */
            out = (ta_buffer){room, length - 1, 0};
            CHECK_EQUAL(ta_json_base64url_read(text, &out), TA_ERROR_NO_MEMORY);
            CHECK_EQUAL(out.length, length);
        }
    }
    static const char *const refused[] = {"A", "AAAAA", "AA==", "AA+/", "AA A", "\xc3\xa9\x41\x41"};
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        ta_buffer out = {room, sizeof room, 0};
        CHECK_EQUAL(ta_json_base64url_read(ta_string(refused[i]), &out), TA_ERROR_INVALID);
    }
}

int main(void) {
    RUN(test_base64url_read);
    RUN(test_read_values);
    RUN(test_read_refusals);
    RUN(test_read_depth);
    RUN(test_read_many_keys);
    RUN(test_write_values);
    RUN(test_write_refusals);
    RUN(test_escapes_round_trip);
    RUN(test_values_vectors);
    RUN(test_read_out_of_memory);
    RUN(test_write_out_of_memory);
    TEST_MAIN_END();
}
