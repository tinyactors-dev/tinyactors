/* ta_cbor: decoding to and encoding from token lists (include/tinyactors/cbor.h),
 * against the examples of RFC 8949 Appendix A. */
#include "../harness/harness.h"
#include <tinyactors/cbor.h>

#include <string.h>

/* `hex` as bytes into `out`; returns the length. */
static size_t unhex(const char *hex, unsigned char *out) {
    size_t length = 0;
    for (; hex[0] && hex[1]; hex += 2) {
        unsigned value = 0;
        for (int k = 0; k < 2; k++) {
            char c = hex[k];
            value = value * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        out[length++] = (unsigned char)value;
    }
    return length;
}

static ta_cbor_token tokens[256];
static unsigned char message[256], encoded[256];

/* Decode `hex` into the shared tokens. */
static ta_status decode(const char *hex, ta_cbor_document *document) {
    size_t length = unhex(hex, message);
    *document = (ta_cbor_document){.bytes = {message, length}, .tokens = tokens, .capacity = 256};
    return ta_cbor_decode(document);
}

/* Decode `hex`, encode it again, and compare with `expected`. */
static int round_trip(const char *hex, const char *expected) {
    ta_cbor_document document;
    if (decode(hex, &document) != TA_OK) {
        fprintf(stderr, "  decode failed: %s\n", hex);
        return 0;
    }
    ta_buffer out = {encoded, sizeof encoded, 0};
    if (ta_cbor_encode(&document, &out) != TA_OK) {
        fprintf(stderr, "  encode failed: %s\n", hex);
        return 0;
    }
    unsigned char want[256];
    size_t length = unhex(expected, want);
    if (out.length != length || memcmp(encoded, want, length) != 0) {
        fprintf(stderr, "  %s encoded as %zu bytes, not %s\n", hex, out.length, expected);
        return 0;
    }
    return 1;
}

static double as_double(uint64_t bits) {
    double value;
    memcpy(&value, &bits, sizeof value);
    return value;
}

/* Appendix A's definite-length examples that are already deterministic: they
 * decode and encode to the same bytes. */
static void test_appendix_a_round_trips(void) {
    static const char *const examples[] = {
        "00", "01", "0a", "17", "1818", "1819", "1864", "1903e8", "1a000f4240", "1b000000e8d4a51000",
        "1bffffffffffffffff", "c249010000000000000000", "3bffffffffffffffff", "c349010000000000000000",
        "20", "29", "3863", "3903e7",
        "f90000", "f98000", "f93c00", "fb3ff199999999999a", "f93e00", "f97bff", "fa47c35000", "fa7f7fffff",
        "fb7e37e43c8800759c", "f90001", "f90400", "f9c400", "fbc010666666666666", "f97c00", "f97e00", "f9fc00",
        "f4", "f5", "f6", "f7", "f0", "f8ff",
        "c074323031332d30332d32315432303a30343a30305a", "c11a514b67b0", "c1fb41d452d9ec200000",
        "d74401020304", "d818456449455446", "d82076687474703a2f2f7777772e6578616d706c652e636f6d",
        "40", "4401020304", "60", "6161", "6449455446", "62225c", "62c3bc", "63e6b0b4", "64f0908591",
        "80", "83010203", "8301820203820405",
        "98190102030405060708090a0b0c0d0e0f101112131415161718181819",
        "a0", "a201020304", "a26161016162820203", "826161a161626163",
        "a56161614161626142616361436164614461656145",
    };
    for (size_t k = 0; k < sizeof examples / sizeof examples[0]; k++)
        CHECK(round_trip(examples[k], examples[k]));
}

/* Appendix A's examples that are not in their shortest form decode to the
 * same values and encode in the shortest form. */
static void test_appendix_a_values(void) {
    ta_cbor_document document;
    static const struct {
        const char *hex, *shortest;
    } floats[] = {
        {"fa7f800000", "f97c00"},         {"fa7fc00000", "f97e00"},         {"faff800000", "f9fc00"},
        {"fb7ff0000000000000", "f97c00"}, {"fb7ff8000000000000", "f97e00"}, {"fbfff0000000000000", "f9fc00"},
    };
    for (size_t k = 0; k < sizeof floats / sizeof floats[0]; k++)
        CHECK(round_trip(floats[k].hex, floats[k].shortest));
    CHECK(decode("fa7f800000", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_FLOAT);
    CHECK(as_double(document.tokens[0].value) > 1e308);

    /* integers and lengths wider than they need be */
    CHECK(round_trip("1817", "17"));
    CHECK(round_trip("1b0000000000000001", "01"));
    CHECK(round_trip("3800", "20"));
    CHECK(round_trip("780161", "6161"));
    CHECK(round_trip("9900020102", "820102"));

    /* the values of a few examples */
    CHECK(decode("1bffffffffffffffff", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_UINT && document.tokens[0].value == UINT64_MAX);
    CHECK(decode("3903e7", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_NEGINT && document.tokens[0].value == 999);
    CHECK(decode("f90001", &document) == TA_OK);
    CHECK(as_double(document.tokens[0].value) == 5.960464477539063e-8);
    CHECK(decode("f97bff", &document) == TA_OK);
    CHECK(as_double(document.tokens[0].value) == 65504.0);
    CHECK(decode("fa47c35000", &document) == TA_OK);
    CHECK(as_double(document.tokens[0].value) == 100000.0);
    CHECK(decode("f8ff", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_SIMPLE && document.tokens[0].value == 255);
    CHECK(decode("f5", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_SIMPLE && document.tokens[0].value == TA_CBOR_TRUE);
    CHECK(decode("d82076687474703a2f2f7777772e6578616d706c652e636f6d", &document) == TA_OK);
    CHECK(document.count == 2 && document.tokens[0].kind == TA_CBOR_TAG &&
          document.tokens[0].value == TA_CBOR_TAG_URI);
    ta_string_view uri = ta_cbor_string(&document, 1);
    CHECK(uri.length == 22 && memcmp(uri.pointer, "http://www.example.com", 22) == 0);
    CHECK(decode("64f0908591", &document) == TA_OK);
    CHECK(document.tokens[0].kind == TA_CBOR_TEXT && document.tokens[0].count == 4);
}

static void test_indefinite_lengths_are_unsupported(void) {
    static const char *const examples[] = {
        "5f42010243030405ff", "7f657374726561646d696e67ff", "9fff", "9f018202039f0405ffff",
        "9f01820203820405ff", "83018202039f0405ff", "83019f0203ff820405",
        "9f0102030405060708090a0b0c0d0e0f101112131415161718181819ff", "bf61610161629f0203ffff",
        "826161bf61626163ff", "bf6346756ef563416d7421ff",
    };
    ta_cbor_document document;
    for (size_t k = 0; k < sizeof examples / sizeof examples[0]; k++)
        CHECK(decode(examples[k], &document) == TA_ERROR_UNSUPPORTED);
}

static void test_malformed(void) {
    static const char *const examples[] = {
        "",                   /* nothing */
        "18",                 /* truncated argument */
        "1a0001",             /* truncated argument */
        "6261",               /* truncated text */
        "8201",               /* truncated array */
        "a101",               /* a key without its value */
        "c0",                 /* a tag without its item */
        "62c328",             /* not UTF-8 */
        "61ff",               /* not UTF-8 */
        "62c080",             /* overlong */
        "63eda080",           /* a surrogate */
        "64f4908080",         /* past U+10FFFF */
        "1c", "1d", "1e",     /* reserved additional information */
        "3c", "5d", "7e", "9c", "bd", "de", "fc", "fd", "fe",
        "ff",                 /* break outside an indefinite length */
        "f810",               /* a two-byte simple value below 32 */
        "0000",               /* bytes after the item */
        "8100ff",             /* bytes after the item */
        "9bffffffffffffffff00", /* a count past the message */
    };
    ta_cbor_document document;
    for (size_t k = 0; k < sizeof examples / sizeof examples[0]; k++) {
        ta_status status = decode(examples[k], &document);
        if (status != TA_ERROR_INVALID) fprintf(stderr, "  %s: %d\n", examples[k], status);
        CHECK(status == TA_ERROR_INVALID);
    }
}

static void test_depth(void) {
    char hex[2 * (TA_CBOR_DEPTH_MAX + 2) + 1];
    ta_cbor_document document;
    /* TA_CBOR_DEPTH_MAX arrays around a 0 */
    size_t at = 0;
    for (int k = 0; k < TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "81", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_OK);
    CHECK_EQUAL(document.count, TA_CBOR_DEPTH_MAX + 1);
    ta_buffer out = {encoded, sizeof encoded, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_OK);
    /* one more */
    at = 0;
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "81", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_ERROR_INVALID);
    /* tags count as nesting too */
    at = 0;
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "c1", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_ERROR_INVALID);
}

static void test_no_memory(void) {
    unsigned char bytes[16];
    size_t length = unhex("8201820203", bytes); /* [1, [2, 3]]: five tokens */
    ta_cbor_token few[2];
    ta_cbor_document document = {.bytes = {bytes, length}, .tokens = few, .capacity = 2};
    CHECK(ta_cbor_decode(&document) == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(document.count, 5);
    ta_cbor_token enough[5];
    document.tokens = enough;
    document.capacity = 5;
    CHECK(ta_cbor_decode(&document) == TA_OK);
    CHECK_EQUAL(document.count, 5);
    /* malformed wins over too few tokens */
    length = unhex("820182020361", bytes);
    document = (ta_cbor_document){.bytes = {bytes, length}, .tokens = few, .capacity = 2};
    CHECK(ta_cbor_decode(&document) == TA_ERROR_INVALID);

    /* encoding: the length it needs */
    length = unhex("8201820203", bytes);
    document = (ta_cbor_document){.bytes = {bytes, length}, .tokens = enough, .capacity = 5};
    CHECK(ta_cbor_decode(&document) == TA_OK);
    unsigned char small[3];
    ta_buffer out = {small, sizeof small, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 5);
    out = (ta_buffer){encoded, 5, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_OK);
    CHECK_EQUAL(out.length, 5);
    CHECK(memcmp(encoded, bytes, 5) == 0);
    /* no buffer at all: measured only */
    out = (ta_buffer){NULL, 0, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 5);
}

static void test_encode_invalid(void) {
    ta_buffer out = {encoded, sizeof encoded, 0};
    ta_cbor_token list[3];
    ta_cbor_document document = {.bytes = {"ab", 2}, .tokens = list, .capacity = 3};
    /* an array short of its items */
    list[0] = (ta_cbor_token){TA_CBOR_ARRAY, 2, 0};
    list[1] = (ta_cbor_token){TA_CBOR_UINT, 0, 1};
    document.count = 2;
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    /* two items */
    list[0] = (ta_cbor_token){TA_CBOR_UINT, 0, 1};
    document.count = 2;
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    /* text outside the bytes */
    list[0] = (ta_cbor_token){TA_CBOR_TEXT, 3, 0};
    document.count = 1;
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    /* text that is not UTF-8 */
    document.bytes = (ta_bytes){"\xc3\x28", 2};
    list[0] = (ta_cbor_token){TA_CBOR_TEXT, 2, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    list[0] = (ta_cbor_token){TA_CBOR_BYTES, 2, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_OK);
    /* simple values 24-31 and unknown kinds */
    list[0] = (ta_cbor_token){TA_CBOR_SIMPLE, 0, 24};
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    list[0] = (ta_cbor_token){99, 0, 0};
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
    /* nothing */
    document.count = 0;
    CHECK(ta_cbor_encode(&document, &out) == TA_ERROR_INVALID);
}

static void test_member_and_next(void) {
    ta_cbor_document document;
    /* {"a": 1, "b": [2, 3], 7: 8, "c": {"d": "e"}} */
    CHECK(decode("a4616101616282020307086163a161646165", &document) == TA_OK);
    CHECK_EQUAL(document.count, 13);
    CHECK_EQUAL(ta_cbor_next(&document, 0), 13);
    uint32_t b = ta_cbor_member(&document, 0, ta_string("b"));
    CHECK_EQUAL(b, 4);
    CHECK_EQUAL(ta_cbor_next(&document, b), 7);
    uint32_t c = ta_cbor_member(&document, 0, ta_string("c"));
    CHECK(c != TA_NIL && document.tokens[c].kind == TA_CBOR_MAP);
    uint32_t d = ta_cbor_member(&document, c, ta_string("d"));
    ta_string_view e = ta_cbor_string(&document, d);
    CHECK(e.length == 1 && e.pointer[0] == 'e');
    CHECK_EQUAL(ta_cbor_member(&document, 0, ta_string("z")), TA_NIL);
    CHECK_EQUAL(ta_cbor_member(&document, 0, ta_string("")), TA_NIL);
    CHECK_EQUAL(ta_cbor_member(&document, b, ta_string("a")), TA_NIL); /* not a map */
    CHECK_EQUAL(ta_cbor_next(&document, 13), TA_NIL);
    CHECK_EQUAL(ta_cbor_string(&document, 1).length, 1);
    CHECK_EQUAL(ta_cbor_string(&document, 2).length, 0); /* not text */
    /* a tagged item is one item */
    CHECK(decode("82c1016161", &document) == TA_OK);
    CHECK_EQUAL(ta_cbor_next(&document, 1), 3);
}

/* The proposal's request, built from C. */
static void test_writer(void) {
    ta_cbor_token list[16];
    char strings[64];
    ta_cbor_writer writer = {
        .document = {.tokens = list, .capacity = 16},
        .strings = {.pointer = strings, .capacity = sizeof strings},
    };
    ta_cbor_write_map(&writer, 3);
    ta_cbor_write_text(&writer, ta_string("op"));
    ta_cbor_write_text(&writer, ta_string("trace.configure"));
    ta_cbor_write_text(&writer, ta_string("detail"));
    ta_cbor_write_text(&writer, ta_string("summary"));
    ta_cbor_write_text(&writer, ta_string("sample"));
    ta_cbor_write_float(&writer, 0.01);
    ta_buffer out = {encoded, sizeof encoded, 0};
    CHECK(ta_cbor_finish(&writer, &out) == TA_OK);
    unsigned char want[64];
    size_t length = unhex("a3"
                          "626f70"
                          "6f74726163652e636f6e666967757265"
                          "6664657461696c"
                          "6773756d6d617279"
                          "6673616d706c65"
                          "fb3f847ae147ae147b",
                          want);
    CHECK_EQUAL(out.length, length);
    CHECK(memcmp(encoded, want, length) == 0);

    /* and read back */
    ta_cbor_document document = {.bytes = {encoded, out.length}, .tokens = tokens, .capacity = 256};
    CHECK(ta_cbor_decode(&document) == TA_OK);
    ta_string_view operation = ta_cbor_string(&document, ta_cbor_member(&document, 0, ta_string("op")));
    CHECK(operation.length == 15 && memcmp(operation.pointer, "trace.configure", 15) == 0);
    uint32_t sample = ta_cbor_member(&document, 0, ta_string("sample"));
    CHECK(as_double(document.tokens[sample].value) == 0.01);

    /* every kind of token */
    writer = (ta_cbor_writer){.document = {.tokens = list, .capacity = 16},
                              .strings = {.pointer = strings, .capacity = sizeof strings}};
    ta_cbor_write_array(&writer, 7);
    ta_cbor_write_int(&writer, -1000);
    ta_cbor_write_int(&writer, INT64_MIN);
    ta_cbor_write_uint(&writer, UINT64_MAX);
    ta_cbor_write_bytes(&writer, (ta_bytes){"\x01\x02", 2});
    ta_cbor_write_simple(&writer, TA_CBOR_NULL);
    ta_cbor_write_tag(&writer, TA_CBOR_TAG_URI);
    ta_cbor_write_text(&writer, ta_string("tinyactors://a/@b"));
    ta_cbor_write_text(&writer, ta_string(""));
    out = (ta_buffer){encoded, sizeof encoded, 0};
    CHECK(ta_cbor_finish(&writer, &out) == TA_OK);
    length = unhex("87"
                   "3903e7"
                   "3b7fffffffffffffff"
                   "1bffffffffffffffff"
                   "420102"
                   "f6"
                   "d820" "7174696e796163746f72733a2f2f612f4062"
                   "60",
                   want);
    CHECK_EQUAL(out.length, length);
    CHECK(memcmp(encoded, want, length) == 0);

    /* too few tokens or too little room for strings: what it needs */
    ta_cbor_token two[2];
    char four[4];
    writer = (ta_cbor_writer){.document = {.tokens = two, .capacity = 2},
                              .strings = {.pointer = four, .capacity = sizeof four}};
    ta_cbor_write_map(&writer, 2);
    ta_cbor_write_text(&writer, ta_string("op"));
    ta_cbor_write_text(&writer, ta_string("stats"));
    ta_cbor_write_text(&writer, ta_string("x"));
    ta_cbor_write_uint(&writer, 1);
    CHECK(writer.status == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(writer.document.count, 5);
    CHECK_EQUAL(writer.strings.length, 8);
    out = (ta_buffer){encoded, sizeof encoded, 99};
    CHECK(ta_cbor_finish(&writer, &out) == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 0);

    /* a container short of its items is refused at the end */
    writer = (ta_cbor_writer){.document = {.tokens = list, .capacity = 16},
                              .strings = {.pointer = strings, .capacity = sizeof strings}};
    ta_cbor_write_map(&writer, 1);
    ta_cbor_write_text(&writer, ta_string("op"));
    CHECK(writer.status == TA_OK);
    out = (ta_buffer){encoded, sizeof encoded, 0};
    CHECK(ta_cbor_finish(&writer, &out) == TA_ERROR_INVALID);
}

/* Each float in the shortest width that keeps it exactly. */
static void test_float_widths(void) {
    static const struct {
        double value;
        const char *hex;
    } floats[] = {
        {0.0, "f90000"},
        {-0.0, "f98000"},
        {1.5, "f93e00"},
        {65504.0, "f97bff"},
        {65505.0, "fa477fe100"},
        {1e300, "fb7e37e43c8800759c"},
        {0.1, "fb3fb999999999999a"},
        {100000.0, "fa47c35000"},
        {5.960464477539063e-8, "f90001"},  /* the smallest half subnormal */
        {6.097555160522461e-5, "f903ff"},  /* the largest half subnormal */
        {2.9802322387695312e-8, "fa33000000"}, /* below a half's range */
        {1.401298464324817e-45, "fa00000001"}, /* the smallest single subnormal */
        {3.4028234663852886e+38, "fa7f7fffff"},
        {-4.0, "f9c400"},
    };
    ta_cbor_token list[1];
    char none[1];
    for (size_t k = 0; k < sizeof floats / sizeof floats[0]; k++) {
        ta_cbor_writer writer = {.document = {.tokens = list, .capacity = 1},
                                 .strings = {.pointer = none, .capacity = 0}};
        ta_cbor_write_float(&writer, floats[k].value);
        ta_buffer out = {encoded, sizeof encoded, 0};
        CHECK(ta_cbor_finish(&writer, &out) == TA_OK);
        unsigned char want[16];
        size_t length = unhex(floats[k].hex, want);
        int same = out.length == length && memcmp(encoded, want, length) == 0;
        if (!same) fprintf(stderr, "  %g is not %s\n", floats[k].value, floats[k].hex);
        CHECK(same);
        /* and back to the same double */
        ta_cbor_document document;
        CHECK(decode(floats[k].hex, &document) == TA_OK);
        double back = as_double(document.tokens[0].value);
        CHECK(memcmp(&back, &floats[k].value, sizeof back) == 0);
    }
    /* infinities and NaN, whatever their bits */
    uint64_t nans[] = {UINT64_C(0x7FF8000000000000), UINT64_C(0xFFF8000000000001), UINT64_C(0x7FF0000000000001)};
    for (size_t k = 0; k < sizeof nans / sizeof nans[0]; k++) {
        ta_cbor_token token = {TA_CBOR_FLOAT, 0, nans[k]};
        ta_cbor_document document = {.tokens = &token, .capacity = 1, .count = 1};
        ta_buffer out = {encoded, sizeof encoded, 0};
        CHECK(ta_cbor_encode(&document, &out) == TA_OK);
        CHECK(out.length == 3 && encoded[0] == 0xf9 && encoded[1] == 0x7e && encoded[2] == 0);
    }
    CHECK(round_trip("f97c00", "f97c00"));
    CHECK(round_trip("fbfff0000000000000", "f9fc00"));
}

int main(void) {
    RUN(test_appendix_a_round_trips);
    RUN(test_appendix_a_values);
    RUN(test_indefinite_lengths_are_unsupported);
    RUN(test_malformed);
    RUN(test_depth);
    RUN(test_no_memory);
    RUN(test_encode_invalid);
    RUN(test_member_and_next);
    RUN(test_writer);
    RUN(test_float_widths);
    TEST_MAIN_END();
}
