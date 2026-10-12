/* ta_cbor: token lists for random access, the streaming writer and the
 * reader (include/tinyactors/cbor.h), against the examples of RFC 8949
 * Appendix A. What the writer writes is byte for byte what the token
 * encoder it replaced wrote: the shortest form of every head and float. */
#include "../harness/harness.h"
#include <tinyactors/cbor.h>

#include <stdlib.h>
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

/* An item read (a token, or a reader's item) written again. */
static void write_item(ta_cbor_writer *writer, const ta_cbor_item *item) {
    double number;
    switch (item->kind) {
    case TA_CBOR_UINT: ta_cbor_write_uint(writer, item->value); break;
    case TA_CBOR_NEGINT: ta_cbor_write_negative(writer, item->value); break;
    case TA_CBOR_TEXT:
        ta_cbor_write_text(writer, (ta_string_view){item->bytes.pointer, (uint32_t)item->bytes.length});
        break;
    case TA_CBOR_BYTES: ta_cbor_write_bytes(writer, item->bytes); break;
    case TA_CBOR_ARRAY: ta_cbor_write_array(writer, item->count); break;
    case TA_CBOR_MAP: ta_cbor_write_map(writer, item->count); break;
    case TA_CBOR_TAG: ta_cbor_write_tag(writer, item->value); break;
    case TA_CBOR_SIMPLE: ta_cbor_write_simple(writer, (uint32_t)item->value); break;
    default:
        memcpy(&number, &item->value, sizeof number);
        ta_cbor_write_float(writer, number);
        break;
    }
}

/* Read `hex` with the reader and write each item with the writer: the
 * writer's bytes (into `encoded`), or the first error. */
static ta_status rewrite(const char *hex, ta_bytes *out) {
    size_t length = unhex(hex, message);
    ta_cbor_reader reader;
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_item item;
    int32_t read;
    while ((read = ta_cbor_read(&reader, &item)) == 1)
        write_item(&writer, &item);
    if (read < 0) return read;
    return ta_cbor_writer_bytes(&writer, out);
}

/* `hex`, decoded to tokens and written again from them, and read and written
 * again with the reader: both are `expected`. */
static int round_trip(const char *hex, const char *expected) {
    unsigned char want[256];
    size_t length = unhex(expected, want);
    ta_cbor_document document;
    if (decode(hex, &document) != TA_OK) {
        fprintf(stderr, "  decode failed: %s\n", hex);
        return 0;
    }
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    for (uint32_t i = 0; i < document.count; i++) {
        ta_cbor_item item = {document.tokens[i].kind, document.tokens[i].count, document.tokens[i].value,
                             {NULL, 0}, 0, 0};
        ta_string_view text = ta_cbor_string(&document, i);
        item.bytes = (ta_bytes){text.pointer, text.length};
        write_item(&writer, &item);
    }
    ta_bytes out;
    if (ta_cbor_writer_bytes(&writer, &out) != TA_OK) {
        fprintf(stderr, "  writing the tokens failed: %s\n", hex);
        return 0;
    }
    if (out.length != length || memcmp(out.pointer, want, length) != 0) {
        fprintf(stderr, "  %s written as %zu bytes, not %s\n", hex, out.length, expected);
        return 0;
    }
    if (rewrite(hex, &out) != TA_OK || out.length != length || memcmp(out.pointer, want, length) != 0) {
        fprintf(stderr, "  %s read and written again is not %s\n", hex, expected);
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
    ta_bytes out;
    for (size_t k = 0; k < sizeof examples / sizeof examples[0]; k++) {
        CHECK(decode(examples[k], &document) == TA_ERROR_UNSUPPORTED);
        CHECK(rewrite(examples[k], &out) == TA_ERROR_UNSUPPORTED); /* the reader too */
    }
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
    ta_bytes out;
    for (size_t k = 0; k < sizeof examples / sizeof examples[0]; k++) {
        ta_status status = decode(examples[k], &document);
        if (status != TA_ERROR_INVALID) fprintf(stderr, "  %s: %d\n", examples[k], status);
        CHECK(status == TA_ERROR_INVALID);
        status = rewrite(examples[k], &out); /* the reader refuses it too */
        if (status != TA_ERROR_INVALID) fprintf(stderr, "  read: %s: %d\n", examples[k], status);
        CHECK(status == TA_ERROR_INVALID);
    }
}

static void test_depth(void) {
    char hex[2 * (TA_CBOR_DEPTH_MAX + 2) + 1];
    ta_cbor_document document;
    ta_bytes out;
    /* TA_CBOR_DEPTH_MAX arrays around a 0 */
    size_t at = 0;
    for (int k = 0; k < TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "81", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_OK);
    CHECK_EQUAL(document.count, TA_CBOR_DEPTH_MAX + 1);
    CHECK(round_trip(hex, hex));
    /* the reader says how deep each item is */
    ta_cbor_reader reader;
    ta_cbor_reader_init(&reader, (ta_bytes){message, unhex(hex, message)});
    ta_cbor_item item;
    uint32_t deepest = 0;
    while (ta_cbor_read(&reader, &item) == 1)
        deepest = item.depth;
    CHECK_EQUAL(deepest, TA_CBOR_DEPTH_MAX);
    /* one more: refused by the decoder, the reader and the writer */
    at = 0;
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "81", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_ERROR_INVALID);
    CHECK(rewrite(hex, &out) == TA_ERROR_INVALID);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++)
        ta_cbor_write_array(&writer, k == TA_CBOR_DEPTH_MAX ? 0 : 1); /* the last is whole: [] */
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK); /* 64 open, the innermost empty */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++)
        ta_cbor_write_array(&writer, 1);
    CHECK(writer.status == TA_ERROR_INVALID);
    ta_cbor_write_uint(&writer, 0);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* tags count as nesting too */
    at = 0;
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++, at += 2)
        memcpy(hex + at, "c1", 2);
    memcpy(hex + at, "00", 3);
    CHECK(decode(hex, &document) == TA_ERROR_INVALID);
    CHECK(rewrite(hex, &out) == TA_ERROR_INVALID);
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    for (int k = 0; k <= TA_CBOR_DEPTH_MAX; k++)
        ta_cbor_write_tag(&writer, 1);
    CHECK(writer.status == TA_ERROR_INVALID);
}

/* The decoder's tokens: TA_ERROR_NO_MEMORY says how many it needs. */
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
}

/* An allocator over the C library's that counts what it gives and takes,
 * and refuses once `refuse` reaches 0. */
typedef struct counted_heap {
    size_t live, blocks;
    int refuse; /* blocks still given (-1: every one) */
} counted_heap;

static void *counted_alloc(void *context, size_t size, size_t align) {
    counted_heap *heap = (counted_heap *)context;
    (void)align;
    if (heap->refuse == 0) return NULL;
    if (heap->refuse > 0) heap->refuse--;
    heap->live += size;
    heap->blocks++;
    return malloc(size);
}

static void counted_free(void *context, void *pointer, size_t size) {
    counted_heap *heap = (counted_heap *)context;
    heap->live -= size;
    heap->blocks--;
    free(pointer);
}

/* [1, [2, 3], "<text>"] into `writer` */
static void write_sample(ta_cbor_writer *writer, ta_string_view text) {
    ta_cbor_write_array(writer, 3);
    ta_cbor_write_uint(writer, 1);
    ta_cbor_write_array(writer, 2);
    ta_cbor_write_uint(writer, 2);
    ta_cbor_write_uint(writer, 3);
    ta_cbor_write_text(writer, text);
}

/* The writer's room: grown through an allocator, or full without one. */
static void test_writer_growth(void) {
    static char long_text[1000];
    memset(long_text, 'x', sizeof long_text);
    ta_string_view text = {long_text, sizeof long_text};
    size_t whole = 1 + 1 + 3 + 3 + sizeof long_text; /* 83 01 82 02 03 79 03e8 x... */
    /* no allocator: the write that does not fit is TA_ERROR_NO_MEMORY, nothing
     * is written past the room, and length counts what the item needs */
    unsigned char small[8];
    memset(small, 0xEE, sizeof small);
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){small, 6, 0}, NULL);
    write_sample(&writer, text);
    CHECK(writer.status == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(writer.length, whole);
    CHECK(small[6] == 0xEE && small[7] == 0xEE);
    CHECK(memcmp(small, "\x83\x01\x82\x02\x03", 5) == 0); /* what fitted stays */
    ta_bytes out = {small, 1};
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_NO_MEMORY);
    CHECK(out.pointer == NULL && out.length == 0);
    /* no room at all: counted only */
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, NULL);
    write_sample(&writer, text);
    CHECK(writer.status == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(writer.length, whole);

    /* an allocator: the room grows, the room is not given back, the grown
     * block is, and the bytes are those of a room big enough */
    counted_heap heap = {0, 0, -1};
    ta_allocator allocator = {&heap, counted_alloc, counted_free};
    ta_cbor_writer_init(&writer, (ta_buffer){small, sizeof small, 0}, &allocator);
    write_sample(&writer, text);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    CHECK_EQUAL(out.length, whole);
    CHECK(out.pointer != small);
    CHECK_EQUAL(heap.blocks, 1u); /* blocks it outgrew went back */
    static unsigned char big[2048];
    ta_cbor_writer room;
    ta_cbor_writer_init(&room, (ta_buffer){big, sizeof big, 0}, NULL);
    write_sample(&room, text);
    ta_bytes expected;
    CHECK(ta_cbor_writer_bytes(&room, &expected) == TA_OK);
    CHECK(out.length == expected.length && memcmp(out.pointer, expected.pointer, out.length) == 0);
    ta_cbor_writer_free(&writer);
    CHECK_EQUAL(heap.blocks, 0u);
    CHECK_EQUAL(heap.live, 0u);
    /* no room, an allocator: everything in blocks it grows */
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &allocator);
    write_sample(&writer, text);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    CHECK(out.length == expected.length && memcmp(out.pointer, expected.pointer, out.length) == 0);
    ta_cbor_writer_free(&writer);
    CHECK_EQUAL(heap.live, 0u);
    /* the allocator refuses: TA_ERROR_NO_MEMORY, nothing leaks */
    heap.refuse = 0;
    ta_cbor_writer_init(&writer, (ta_buffer){small, sizeof small, 0}, &allocator);
    write_sample(&writer, text);
    CHECK(writer.status == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(writer.length, whole);
    ta_cbor_writer_free(&writer);
    CHECK_EQUAL(heap.live, 0u);
}

static void test_writer_invalid(void) {
    ta_cbor_writer writer;
    ta_bytes out;
    /* an array short of its items */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_array(&writer, 2);
    ta_cbor_write_uint(&writer, 1);
    CHECK(writer.status == TA_OK);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* two items */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_uint(&writer, 1);
    ta_cbor_write_uint(&writer, 2);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* nothing */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* text that is not UTF-8; the same bytes as bytes are fine */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_text(&writer, (ta_string_view){"\xc3\x28", 2});
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_bytes(&writer, (ta_bytes){"\xc3\x28", 2});
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    /* simple values 24-31 */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_simple(&writer, 24);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* an end that closes nothing, or a container counted up front */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_end(&writer);
    CHECK(writer.status == TA_ERROR_INVALID);
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_array(&writer, 1);
    ta_cbor_write_end(&writer);
    CHECK(writer.status == TA_ERROR_INVALID);
    /* a map counted later with a key and no value */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_map(&writer, TA_CBOR_COUNT_LATER);
    ta_cbor_write_text(&writer, ta_string("a"));
    ta_cbor_write_end(&writer);
    CHECK(writer.status == TA_ERROR_INVALID);
    /* a container counted later and never closed */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_array(&writer, TA_CBOR_COUNT_LATER);
    ta_cbor_write_uint(&writer, 1);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
    /* invalid wins over a full room */
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, NULL);
    ta_cbor_write_text(&writer, ta_string("ok"));
    ta_cbor_write_simple(&writer, 25);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_ERROR_INVALID);
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
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_map(&writer, 3);
    ta_cbor_write_text(&writer, ta_string("op"));
    ta_cbor_write_text(&writer, ta_string("trace.configure"));
    ta_cbor_write_text(&writer, ta_string("detail"));
    ta_cbor_write_text(&writer, ta_string("summary"));
    ta_cbor_write_text(&writer, ta_string("sample"));
    ta_cbor_write_float(&writer, 0.01);
    ta_bytes out;
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    CHECK(out.pointer == encoded); /* in the room */
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
    ta_cbor_document document = {.bytes = out, .tokens = tokens, .capacity = 256};
    CHECK(ta_cbor_decode(&document) == TA_OK);
    ta_string_view operation = ta_cbor_string(&document, ta_cbor_member(&document, 0, ta_string("op")));
    CHECK(operation.length == 15 && memcmp(operation.pointer, "trace.configure", 15) == 0);
    uint32_t sample = ta_cbor_member(&document, 0, ta_string("sample"));
    CHECK(as_double(document.tokens[sample].value) == 0.01);

    /* every kind of item */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_array(&writer, 8);
    ta_cbor_write_int(&writer, -1000);
    ta_cbor_write_int(&writer, INT64_MIN);
    ta_cbor_write_uint(&writer, UINT64_MAX);
    ta_cbor_write_negative(&writer, UINT64_MAX); /* -2^64 */
    ta_cbor_write_bytes(&writer, (ta_bytes){"\x01\x02", 2});
    ta_cbor_write_simple(&writer, TA_CBOR_NULL);
    ta_cbor_write_tag(&writer, TA_CBOR_TAG_URI);
    ta_cbor_write_text(&writer, ta_string("tinyactors://a/@b"));
    ta_cbor_write_text(&writer, ta_string(""));
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    length = unhex("88"
                   "3903e7"
                   "3b7fffffffffffffff"
                   "1bffffffffffffffff"
                   "3bffffffffffffffff"
                   "420102"
                   "f6"
                   "d820" "7174696e796163746f72733a2f2f612f4062"
                   "60",
                   want);
    CHECK_EQUAL(out.length, length);
    CHECK(memcmp(encoded, want, length) == 0);
}

/* A count fixed when the container closes: the head in its shortest form,
 * the items moved up when it is longer than a byte. */
static void test_count_later(void) {
    static unsigned char room[1 << 18], expected_room[1 << 18];
    const uint32_t sizes[] = {0, 1, 23, 24, 255, 256, 65535, 65536};
    for (size_t k = 0; k < sizeof sizes / sizeof sizes[0]; k++) {
        uint32_t n = sizes[k];
        /* [[n items counted later], "end"] against the same counted up front */
        ta_cbor_writer later, upfront;
        ta_cbor_writer_init(&later, (ta_buffer){room, sizeof room, 0}, NULL);
        ta_cbor_writer_init(&upfront, (ta_buffer){expected_room, sizeof expected_room, 0}, NULL);
        ta_cbor_write_array(&later, 2);
        ta_cbor_write_array(&upfront, 2);
        ta_cbor_write_array(&later, TA_CBOR_COUNT_LATER);
        ta_cbor_write_array(&upfront, n);
        for (uint32_t i = 0; i < n; i++) {
            ta_cbor_write_uint(&later, i % 30);
            ta_cbor_write_uint(&upfront, i % 30);
        }
        ta_cbor_write_end(&later);
        ta_cbor_write_text(&later, ta_string("end"));
        ta_cbor_write_text(&upfront, ta_string("end"));
        ta_bytes a, b;
        CHECK(ta_cbor_writer_bytes(&later, &a) == TA_OK);
        CHECK(ta_cbor_writer_bytes(&upfront, &b) == TA_OK);
        int same = a.length == b.length && memcmp(a.pointer, b.pointer, a.length) == 0;
        if (!same) fprintf(stderr, "  %u items counted later: %zu bytes, not %zu\n", n, a.length, b.length);
        CHECK(same);
    }
    /* a map whose writer drops some members: {"a": 1, "c": 3} */
    ta_cbor_writer writer;
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
    ta_cbor_write_map(&writer, TA_CBOR_COUNT_LATER);
    const char *keys[] = {"a", "b", "c"};
    for (int i = 0; i < 3; i++) {
        if (i == 1) continue; /* dropped, its key with it */
        ta_cbor_write_text(&writer, ta_string(keys[i]));
        ta_cbor_write_uint(&writer, (uint64_t)i + 1);
    }
    ta_cbor_write_end(&writer);
    ta_bytes out;
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    unsigned char want[16];
    size_t length = unhex("a2616101616303", want);
    CHECK(out.length == length && memcmp(out.pointer, want, length) == 0);
    /* nested counted later, the outer longer than a byte, growing as it goes */
    counted_heap heap = {0, 0, -1};
    ta_allocator allocator = {&heap, counted_alloc, counted_free};
    ta_cbor_writer_init(&writer, (ta_buffer){NULL, 0, 0}, &allocator);
    ta_cbor_write_array(&writer, TA_CBOR_COUNT_LATER);
    for (int i = 0; i < 30; i++) {
        ta_cbor_write_map(&writer, TA_CBOR_COUNT_LATER);
        for (int j = 0; j < 30; j++) {
            ta_cbor_write_uint(&writer, (uint64_t)j);
            ta_cbor_write_text(&writer, ta_string("value"));
        }
        ta_cbor_write_end(&writer);
    }
    ta_cbor_write_end(&writer);
    CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
    ta_cbor_document document = {.bytes = out, .tokens = NULL, .capacity = 0};
    CHECK(ta_cbor_decode(&document) == TA_ERROR_NO_MEMORY); /* well-formed: only tokens lacked */
    CHECK_EQUAL(document.count, 1u + 30u * (1u + 60u));
    CHECK(((const unsigned char *)out.pointer)[0] == 0x98 && ((const unsigned char *)out.pointer)[1] == 30);
    CHECK(((const unsigned char *)out.pointer)[2] == 0xb8 && ((const unsigned char *)out.pointer)[3] == 30);
    ta_cbor_writer_free(&writer);
    CHECK_EQUAL(heap.live, 0u);
    /* counted later past a full room: the length is still what it needs */
    ta_cbor_writer_init(&writer, (ta_buffer){encoded, 4, 0}, NULL);
    ta_cbor_write_array(&writer, TA_CBOR_COUNT_LATER);
    for (int i = 0; i < 30; i++)
        ta_cbor_write_uint(&writer, 1);
    ta_cbor_write_end(&writer);
    CHECK(writer.status == TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(writer.length, 2u + 30u);
}

/* The reader: each item with its kind, value, length, view and depth. */
static void test_reader(void) {
    /* {"a": [1, -2, h'0102'], "b": 32("x"), "c": 1.5, "d": true} */
    size_t length = unhex("a46161830121420102" "6162d8206178" "6163f93e00" "6164f5", message);
    ta_cbor_reader reader;
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    ta_cbor_item item;
    static const struct {
        uint32_t kind, count;
        uint64_t value;
        const char *bytes;
        uint32_t depth;
    } want[] = {
        {TA_CBOR_MAP, 4, 0, NULL, 0},   {TA_CBOR_TEXT, 1, 0, "a", 1}, {TA_CBOR_ARRAY, 3, 0, NULL, 1},
        {TA_CBOR_UINT, 0, 1, NULL, 2},  {TA_CBOR_NEGINT, 0, 1, NULL, 2},
        {TA_CBOR_BYTES, 2, 0, "\x01\x02", 2},
        {TA_CBOR_TEXT, 1, 0, "b", 1},   {TA_CBOR_TAG, 0, 32, NULL, 1}, {TA_CBOR_TEXT, 1, 0, "x", 2},
        {TA_CBOR_TEXT, 1, 0, "c", 1},   {TA_CBOR_FLOAT, 0, UINT64_C(0x3FF8000000000000), NULL, 1},
        {TA_CBOR_TEXT, 1, 0, "d", 1},   {TA_CBOR_SIMPLE, 0, TA_CBOR_TRUE, NULL, 1},
    };
    for (size_t k = 0; k < sizeof want / sizeof want[0]; k++) {
        CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
        int same = item.kind == want[k].kind && item.count == want[k].count && item.value == want[k].value &&
                   item.depth == want[k].depth &&
                   (!want[k].bytes || (item.bytes.length == item.count &&
                                       !memcmp(item.bytes.pointer, want[k].bytes, item.count)));
        if (!same) fprintf(stderr, "  item %zu: kind %u count %u value %llu depth %u\n", k, item.kind,
                           item.count, (unsigned long long)item.value, item.depth);
        CHECK(same);
    }
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 0); /* read whole */
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 0);
    /* a view points into the message: nothing is copied */
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    ta_cbor_read(&reader, &item);
    ta_cbor_read(&reader, &item);
    CHECK((const unsigned char *)item.bytes.pointer == message + 2);
    /* one scalar, then the end; bytes after it are refused, and stay refused */
    length = unhex("0102", message);
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), TA_ERROR_INVALID);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), TA_ERROR_INVALID);
    /* a truncated container: refused at the item that cannot be whole (a
     * count past the bytes left), the items before it read */
    length = unhex("82018201", message);
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), TA_ERROR_INVALID);
    length = unhex("820161", message); /* text past the end */
    ta_cbor_reader_init(&reader, (ta_bytes){message, length});
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), 1);
    CHECK_EQUAL(ta_cbor_read(&reader, &item), TA_ERROR_INVALID);
    /* an empty message */
    ta_cbor_reader_init(&reader, (ta_bytes){NULL, 0});
    CHECK_EQUAL(ta_cbor_read(&reader, &item), TA_ERROR_INVALID);
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
    for (size_t k = 0; k < sizeof floats / sizeof floats[0]; k++) {
        ta_cbor_writer writer;
        ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
        ta_cbor_write_float(&writer, floats[k].value);
        ta_bytes out;
        CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
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
        ta_cbor_writer writer;
        ta_cbor_writer_init(&writer, (ta_buffer){encoded, sizeof encoded, 0}, NULL);
        ta_cbor_write_float(&writer, as_double(nans[k]));
        ta_bytes out;
        CHECK(ta_cbor_writer_bytes(&writer, &out) == TA_OK);
        CHECK(out.length == 3 && encoded[0] == 0xf9 && encoded[1] == 0x7e && encoded[2] == 0);
    }
    CHECK(round_trip("f97c00", "f97c00"));
    CHECK(round_trip("fbfff0000000000000", "f9fc00"));
}

/* Diagnostic notation: compact, so JSON-shaped values read as their JSON. */
static int diagnostic_is(const char *hex, const char *expected) {
    size_t length = unhex(hex, message);
    char text[256];
    ta_buffer out = {text, sizeof text - 1, 0};
    ta_status status = ta_cbor_diagnostic((ta_bytes){message, length}, &out);
    if (status != TA_OK) {
        fprintf(stderr, "  %s: status %d\n", hex, (int)status);
        return 0;
    }
    text[out.length] = 0;
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "  %s: %s, expected %s\n", hex, text, expected);
        return 0;
    }
    return 1;
}

static void test_diagnostic(void) {
    static const char *const cases[][2] = {
        {"00", "0"},
        {"1903e8", "1000"},
        {"1bffffffffffffffff", "18446744073709551615"},
        {"3bffffffffffffffff", "-18446744073709551616"},
        {"20", "-1"},
        {"3903e7", "-1000"},
        {"f90000", "0.0"},
        {"f98000", "-0.0"},
        {"f93c00", "1.0"},
        {"fb3ff199999999999a", "1.1"},
        {"f93e00", "1.5"},
        {"f97bff", "65504.0"},
        {"fa47c35000", "100000.0"},
        {"fa7f7fffff", "3.4028234663852886e+38"},
        {"fb7e37e43c8800759c", "1.0e+300"},
        {"f90001", "5.960464477539063e-8"},
        {"f90400", "0.00006103515625"},
        {"f9c400", "-4.0"},
        {"fbc010666666666666", "-4.1"},
        {"fb3fb999999999999a", "0.1"},
        {"fb3ddb7cdfd9d7bdbb", "1.0e-10"},
        {"fb444b1ae4d6e2ef50", "1.0e+21"},
        {"fb4415af1d78b58c40", "100000000000000000000.0"},
        {"fb4340000000000001", "9007199254740994.0"},
        {"fb409f607e6b74df04", "2008.1234567891233"},
        {"fb3fd5555555555555", "0.3333333333333333"},
        {"fb7fefffffffffffff", "1.7976931348623157e+308"},
        {"fb0010000000000000", "2.2250738585072014e-308"},
        {"fb3ff0000000000001", "1.0000000000000002"},
        {"fb4000000000000000", "2.0"},
        {"fa3f800001", "1.0000001192092896"},
        {"fb0000000000000001", "5.0e-324"},
        {"f97c00", "Infinity"},
        {"f97e00", "NaN"},
        {"f9fc00", "-Infinity"},
        {"f4", "false"},
        {"f5", "true"},
        {"f6", "null"},
        {"f7", "undefined"},
        {"f0", "simple(16)"},
        {"f8ff", "simple(255)"},
        {"c074323031332d30332d32315432303a30343a30305a", "0(\"2013-03-21T20:04:00Z\")"},
        {"d82063746131", "32(\"ta1\")"},
        {"d74401020304", "23(h'01020304')"},
        {"40", "h''"},
        {"4401020304", "h'01020304'"},
        {"60", "\"\""},
        {"6161", "\"a\""},
        {"62225c", "\"\\\"\\\\\""},
        {"62c3bc", "\"\xc3\xbc\""},
        {"63010a09", "\"\\u0001\\n\\t\""},
        {"80", "[]"},
        {"83010203", "[1,2,3]"},
        {"8301820203820405", "[1,[2,3],[4,5]]"},
        {"a0", "{}"},
        {"a201020304", "{1:2,3:4}"},
        {"a26161016162820203", "{\"a\":1,\"b\":[2,3]}"},
        {"826161a161626163", "[\"a\",{\"b\":\"c\"}]"},
        {"81a0", "[{}]"},
        {"a1616180", "{\"a\":[]}"},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        CHECK(diagnostic_is(cases[i][0], cases[i][1]));

    /* too small: the first bytes, and the length it needs */
    size_t length = unhex("83010203", message);
    char text[4];
    ta_buffer out = {text, 3, 0};
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &out), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(out.length, 7);
    CHECK(memcmp(text, "[1,", 3) == 0);
    ta_buffer measure = {NULL, 0, 0};
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &measure), TA_ERROR_NO_MEMORY);
    CHECK_EQUAL(measure.length, 7);

    /* what decoding refuses */
    ta_buffer any = {text, sizeof text, 0};
    length = unhex("8201", message);
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &any), TA_ERROR_INVALID);
    length = unhex("9f01ff", message);
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &any), TA_ERROR_UNSUPPORTED);
    length = unhex("62c328", message);
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &any), TA_ERROR_INVALID);
    length = unhex("0101", message);
    CHECK_EQUAL(ta_cbor_diagnostic((ta_bytes){message, length}, &any), TA_ERROR_INVALID);
}

int main(void) {
    RUN(test_appendix_a_round_trips);
    RUN(test_appendix_a_values);
    RUN(test_indefinite_lengths_are_unsupported);
    RUN(test_malformed);
    RUN(test_depth);
    RUN(test_no_memory);
    RUN(test_writer_growth);
    RUN(test_writer_invalid);
    RUN(test_member_and_next);
    RUN(test_writer);
    RUN(test_count_later);
    RUN(test_reader);
    RUN(test_float_widths);
    RUN(test_diagnostic);
    TEST_MAIN_END();
}
