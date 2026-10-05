/* test: standalone asan */
/*
 * Block C2, the engine (src/record.h): ta_record_reader_init and
 * ta_record_check, driven by a hand-built descriptor table with awkward
 * shapes: a message with no spans, one with three spans, an enum and a bool,
 * one with the largest fixed part, and one on another ring. The generated
 * table and the fuzz loop are in ring_proto_test.c.
 *
 * Every refusal is checked with its reason, and after every refusal the
 * reader's next_seq is unchanged. The overflow cases use exact values: a
 * span of (0xFFFFFFF0, 0x20) wraps a u32 sum, and a size of 2^32 does not fit
 * the size field.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

enum { PLAIN = 0x0101, SPANS = 0x0102, BIG = 0x0103, ONC2H = 0x0201 };

/* SPANS: spans at 32, 40, 48; an enum 0..2 at 56; a bool at 57; fixed part 64. */
static const ta_record_span_at spans_spans[] = { { 32 }, { 40 }, { 48 } };
static const ta_record_domain spans_domains[] = { { 56, 0x7u }, { 57, 0x3u } };
/* BIG: a span at the last possible place of a 128-byte fixed part. */
static const ta_record_span_at big_spans[] = { { 120 } };

static const ta_record_descriptor descriptors[] = {
    { .kind = PLAIN, .ring = TA_RING_H2C, .fixed_size = 32, .flags_allowed = 0x0100 },
    { .kind = SPANS, .ring = TA_RING_H2C, .span_count = 3, .domain_count = 2, .fixed_size = 64, .flags_allowed = 0x0101,
      .spans = spans_spans, .domains = spans_domains },
    { .kind = BIG, .ring = TA_RING_H2C, .span_count = 1, .fixed_size = 128, .spans = big_spans },
    { .kind = ONC2H, .ring = TA_RING_C2H, .fixed_size = 32 },
};
static const ta_record_table table = { descriptors, TA_ARRAY_COUNT(descriptors) };

static ta_record_reader reader_make(ta_ring_mode mode) {
    ta_record_reader reader;
    ta_record_reader_params params = { .table = &table, .ring = TA_RING_H2C, .mode = mode, .max_record_bytes = 256 };
    CHECK(ta_record_reader_init(&reader, &params) == TA_OK);
    return reader;
}

/* A record under construction: 8-aligned storage, one past the end for the unaligned runs. */
typedef struct rec {
    _Alignas(8) unsigned char bytes[512];
    uint32_t length;
} rec;

static void put16(rec *r, uint32_t at, uint16_t v) { ta_mem_copy(r->bytes + at, &v, sizeof v); }
static void put32(rec *r, uint32_t at, uint32_t v) { ta_mem_copy(r->bytes + at, &v, sizeof v); }
static void put64(rec *r, uint32_t at, uint64_t v) { ta_mem_copy(r->bytes + at, &v, sizeof v); }

/* rec_make builds a well-formed record of `length` bytes: header filled, the rest zero. */
static void rec_make(rec *r, uint16_t kind, uint32_t length, uint64_t seq) {
    ta_mem_zero(r->bytes, sizeof r->bytes);
    r->length = length;
    put16(r, TA_RECORD_AT_KIND, kind);
    put32(r, TA_RECORD_AT_SIZE, length);
    put64(r, TA_RECORD_AT_SEQ, seq);
    put64(r, TA_RECORD_AT_SESSION, 7);
    put64(r, TA_RECORD_AT_TIME, 9);
}

static void rec_span(rec *r, uint32_t at, uint32_t offset, uint32_t length) {
    put32(r, at, offset);
    put32(r, at + 4, length);
}

static ta_record_fault check(ta_record_reader *reader, const unsigned char *bytes, size_t length, ta_record *out) {
    return ta_record_check(reader, &(ta_record_input){ .bytes = bytes, .length = length }, out);
}

static ta_record_fault check_rec(ta_record_reader *reader, const rec *r) {
    ta_record out;
    return check(reader, r->bytes, r->length, &out);
}

/* Expects a refusal with `fault` and an unmoved next_seq. */
#define REFUSE(reader, r, fault) \
    do { \
        uint64_t before_ = (reader)->next_seq; \
        ta_record_fault got_ = check_rec((reader), (r)); \
        if (got_ != (fault)) fprintf(stderr, "%s:%d: wanted %s, got %s\n", __FILE__, __LINE__, ta_record_fault_name(fault), ta_record_fault_name(got_)); \
        CHECK(got_ == (fault)); \
        CHECK((reader)->next_seq == before_); \
    } while (0)

static void test_init(void) {
    ta_record_reader reader = { 0 };
    ta_record_reader_params good = { .table = &table, .ring = TA_RING_H2C, .mode = TA_RING_LOSSLESS, .max_record_bytes = 256 };
    CHECK(ta_record_reader_init(&reader, &good) == TA_OK);
    CHECK(reader.next_seq == 1 && reader.table == &table && reader.max_record_bytes == 256);

    ta_record_reader untouched = { 0 };
    untouched.next_seq = 77;
    ta_record_reader_params bad;
    bad = good; bad.table = NULL;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
    bad = good; bad.ring = TA_RING_COUNT;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
    bad = good; bad.mode = (ta_ring_mode)2;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
    bad = good; bad.max_record_bytes = 24;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
    bad = good; bad.max_record_bytes = 252;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
    CHECK(untouched.next_seq == 77 && untouched.table == NULL);

    /* An empty table is a reader that refuses every kind. */
    ta_record_table empty = { NULL, 0 };
    bad = good; bad.table = &empty;
    CHECK(ta_record_reader_init(&reader, &bad) == TA_OK);
    rec r;
    rec_make(&r, PLAIN, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_KIND);
    ta_record_table null_with_count = { NULL, 1 };
    bad = good; bad.table = &null_with_count;
    CHECK(ta_record_reader_init(&untouched, &bad) == TA_ERR_INVALID);
}

/* Each malformed descriptor is refused by init, because check trusts the table. */
static void test_init_validates_table(void) {
    ta_record_reader reader;
    ta_record_reader_params params = { .ring = TA_RING_H2C, .mode = TA_RING_LOSSLESS, .max_record_bytes = 256 };
    ta_record_table bad_table;

    ta_record_descriptor unsorted[] = { descriptors[1], descriptors[0] };
    bad_table = (ta_record_table){ unsorted, 2 };
    params.table = &bad_table;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);

    ta_record_descriptor duplicate[] = { descriptors[0], descriptors[0] };
    bad_table = (ta_record_table){ duplicate, 2 };
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);

    ta_record_descriptor one;
    one = descriptors[0]; one.kind = 0;
    bad_table = (ta_record_table){ &one, 1 };
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    one = descriptors[0]; one.ring = TA_RING_COUNT;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    one = descriptors[0]; one.fixed_size = 24;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    one = descriptors[0]; one.fixed_size = 36;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    one = descriptors[0]; one.fixed_size = TA_RECORD_FIXED_MAX + 8;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    one = descriptors[1]; one.spans = NULL;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);

    static const ta_record_span_at in_header[] = { { 24 } };
    one = descriptors[1]; one.span_count = 1; one.spans = in_header;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    static const ta_record_span_at past_end[] = { { 60 } };
    one = descriptors[1]; one.span_count = 1; one.spans = past_end;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    static const ta_record_span_at last_fits[] = { { 56 } };
    one = descriptors[1]; one.span_count = 1; one.spans = last_fits; one.domain_count = 0;
    CHECK(ta_record_reader_init(&reader, &params) == TA_OK);
    static const ta_record_domain domain_out[] = { { 64, 1 } };
    one = descriptors[1]; one.domain_count = 1; one.domains = domain_out;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
    static const ta_record_domain domain_in_header[] = { { 31, 1 } };
    one = descriptors[1]; one.domain_count = 1; one.domains = domain_in_header;
    CHECK(ta_record_reader_init(&reader, &params) == TA_ERR_INVALID);
}

static void test_accept(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    ta_record out;

    rec_make(&r, PLAIN, 32, 1);
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
    CHECK(out.kind == PLAIN && out.size == 32 && out.seq == 1 && out.gap == 0 && out.bytes == r.bytes);
    CHECK(out.descriptor == &descriptors[0] && reader.next_seq == 2);

    /* A 3-span record: one span in the tail, two empty, an enum and a bool in range, both defined flags. */
    rec_make(&r, SPANS, 80, 2);
    put16(&r, TA_RECORD_AT_FLAGS, 0x0101);
    rec_span(&r, 32, 64, 11);
    r.bytes[56] = 2;
    r.bytes[57] = 1;
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
    ta_bytes first = ta_record_span(&out, 32);
    CHECK(first.ptr == r.bytes + 64 && first.len == 11);
    CHECK(ta_record_span(&out, 40).len == 0 && ta_record_span(&out, 40).ptr == NULL);
    CHECK(ta_record_u64(&out, TA_RECORD_AT_SESSION) == 7 && ta_record_u64(&out, TA_RECORD_AT_TIME) == 9);
    CHECK(ta_record_u16(&out, TA_RECORD_AT_FLAGS) == 0x0101 && ta_record_u32(&out, TA_RECORD_AT_SIZE) == 80);
    CHECK(ta_record_u8(&out, 56) == 2 && ta_record_u8(&out, 57) == 1);
    CHECK(reader.next_seq == 3);

    /* A span that ends exactly at the end of the record, and one that starts at the fixed size. */
    rec_make(&r, SPANS, 80, 3);
    rec_span(&r, 32, 64, 16);
    rec_span(&r, 40, 79, 1);
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
    /* Overlapping spans are allowed. */
    rec_make(&r, SPANS, 80, 4);
    rec_span(&r, 32, 64, 16);
    rec_span(&r, 40, 64, 16);
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);

    /* The largest fixed part, with its span in the last slot; and the same record at an odd address. */
    rec_make(&r, BIG, 136, 5);
    rec_span(&r, 120, 128, 8);
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
    CHECK(ta_record_span(&out, 120).len == 8);
    rec_make(&r, BIG, 136, 6);
    rec_span(&r, 120, 128, 8);
    unsigned char shifted[520];
    ta_mem_copy(shifted + 1, r.bytes, r.length);
    CHECK(check(&reader, shifted + 1, r.length, &out) == TA_RECORD_OK);
    CHECK(reader.next_seq == 7);
}

static void test_truncated_and_size(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    ta_record out;
    rec_make(&r, PLAIN, 32, 1);

    /* Lengths below the header never read the header. */
    for (size_t length = 0; length < 32; length += 1) {
        CHECK(check(&reader, r.bytes, length, &out) == TA_RECORD_TRUNCATED);
    }
    CHECK(check(&reader, NULL, 32, &out) == TA_RECORD_TRUNCATED);
    CHECK(reader.next_seq == 1);

    /* Length passed differs from the size field, both ways. */
    CHECK(check(&reader, r.bytes, 40, &out) == TA_RECORD_SIZE);
    rec_make(&r, PLAIN, 64, 1);
    CHECK(check(&reader, r.bytes, 32, &out) == TA_RECORD_SIZE);
    /* Not a multiple of 8, even when the length agrees. */
    rec_make(&r, PLAIN, 36, 1);
    REFUSE(&reader, &r, TA_RECORD_SIZE);
    /* Above the reader's maximum (256), a multiple of 8, equal to the length. */
    rec_make(&r, PLAIN, 264, 1);
    r.length = 264;
    REFUSE(&reader, &r, TA_RECORD_SIZE);
    /* A length that does not fit a u32 can only be a size fault. A 64-bit size_t makes the value, a 32-bit one never gets here. */
    if (sizeof(size_t) > 4) {
        rec_make(&r, PLAIN, 32, 1);
        CHECK(check(&reader, r.bytes, (size_t)0x100000020ull, &out) == TA_RECORD_SIZE);
        put32(&r, TA_RECORD_AT_SIZE, 0x20u);
        CHECK(check(&reader, r.bytes, (size_t)0x100000000ull, &out) == TA_RECORD_SIZE);
    }
    /* The fixed part is longer than the record. */
    rec_make(&r, SPANS, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_TRUNCATED);
    rec_make(&r, SPANS, 56, 1);
    REFUSE(&reader, &r, TA_RECORD_TRUNCATED);
    rec_make(&r, BIG, 120, 1);
    REFUSE(&reader, &r, TA_RECORD_TRUNCATED);
    /* Exactly the fixed size is whole. */
    rec_make(&r, SPANS, 64, 1);
    CHECK(check_rec(&reader, &r) == TA_RECORD_OK);
}

static void test_kind(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    rec_make(&r, 0, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_KIND);
    rec_make(&r, 0x7777, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_KIND);
    rec_make(&r, 0xFFFF, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_KIND);
    /* Known, but travels on c2h and this reader reads h2c. */
    rec_make(&r, ONC2H, 32, 1);
    REFUSE(&reader, &r, TA_RECORD_WRONG_RING);
    /* Kind is judged before the fixed size, which is judged before seq. */
    rec_make(&r, SPANS, 32, 99);
    REFUSE(&reader, &r, TA_RECORD_TRUNCATED);
}

static void test_seq(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    rec_make(&r, PLAIN, 32, 0);
    REFUSE(&reader, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, 2);
    REFUSE(&reader, &r, TA_RECORD_SEQ); /* a gap on a lossless ring */
    rec_make(&r, PLAIN, 32, 1);
    CHECK(check_rec(&reader, &r) == TA_RECORD_OK);
    REFUSE(&reader, &r, TA_RECORD_SEQ); /* the same record again */
    rec_make(&r, PLAIN, 32, UINT64_MAX);
    REFUSE(&reader, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, 2);
    CHECK(check_rec(&reader, &r) == TA_RECORD_OK);

    /* Lossy: any jump forward is accepted and reported; going back is not. */
    ta_record_reader lossy = reader_make(TA_RING_LOSSY);
    ta_record out;
    rec_make(&r, PLAIN, 32, 1);
    CHECK(check(&lossy, r.bytes, r.length, &out) == TA_RECORD_OK && out.gap == 0);
    rec_make(&r, PLAIN, 32, 5);
    CHECK(check(&lossy, r.bytes, r.length, &out) == TA_RECORD_OK && out.gap == 3 && lossy.next_seq == 6);
    rec_make(&r, PLAIN, 32, 6);
    CHECK(check(&lossy, r.bytes, r.length, &out) == TA_RECORD_OK && out.gap == 0);
    rec_make(&r, PLAIN, 32, 6);
    REFUSE(&lossy, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, 3);
    REFUSE(&lossy, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, 0);
    REFUSE(&lossy, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, UINT64_MAX);
    REFUSE(&lossy, &r, TA_RECORD_SEQ);
    rec_make(&r, PLAIN, 32, UINT64_MAX - 1);
    CHECK(check(&lossy, r.bytes, r.length, &out) == TA_RECORD_OK && lossy.next_seq == UINT64_MAX);
    rec_make(&r, PLAIN, 32, UINT64_MAX - 1);
    REFUSE(&lossy, &r, TA_RECORD_SEQ);
}

static void test_spans(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;

    /* Each of the three span slots is checked, not only the first. */
    for (uint32_t slot = 32; slot <= 48; slot += 8) {
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 32, 4); /* inside the fixed part */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 63, 1); /* one below the fixed size */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 81, 0); /* empty, non-canonical, and past the end */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 80, 0); /* empty at the end: non-canonical */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 64, 0); /* empty with an offset */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 80, 1); /* starts at the end */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 64, 17); /* one byte too long */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 0xFFFFFFF0u, 0x20u); /* the u32 sum wraps to 16 */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 64, 0xFFFFFFFFu);
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 0xFFFFFFFFu, 1);
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 0xFFFFFFFFu, 0xFFFFFFFFu);
        REFUSE(&reader, &r, TA_RECORD_SPAN);
        rec_make(&r, SPANS, 80, 1);
        rec_span(&r, slot, 64, 0xFFFFFFC0u); /* offset + length == 2^32 exactly */
        REFUSE(&reader, &r, TA_RECORD_SPAN);
    }
    /* A record whose span fault is in the last slot, with the other two good: the last is still seen. */
    rec_make(&r, SPANS, 80, 1);
    rec_span(&r, 32, 64, 8);
    rec_span(&r, 40, 72, 8);
    rec_span(&r, 48, 72, 9);
    REFUSE(&reader, &r, TA_RECORD_SPAN);

    /* A span in the 128-byte fixed part. */
    rec_make(&r, BIG, 136, 1);
    rec_span(&r, 120, 128, 9);
    REFUSE(&reader, &r, TA_RECORD_SPAN);
    rec_span(&r, 120, 120, 8);
    REFUSE(&reader, &r, TA_RECORD_SPAN);
}

static void test_values(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    ta_record out;

    /* Flag bits: only 0 and 8 are defined for SPANS. */
    for (uint32_t bit = 0; bit < 16; bit += 1) {
        rec_make(&r, SPANS, 64, 1);
        put16(&r, TA_RECORD_AT_FLAGS, (uint16_t)(1u << bit));
        if (bit == 0 || bit == 8) {
            CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
            reader.next_seq = 1;
        } else {
            REFUSE(&reader, &r, TA_RECORD_VALUE);
        }
    }
    rec_make(&r, PLAIN, 32, 1);
    put16(&r, TA_RECORD_AT_FLAGS, 0x0001); /* PLAIN defines only bit 8 */
    REFUSE(&reader, &r, TA_RECORD_VALUE);

    /* The enum allows 0..2 and the bool 0..1. */
    for (uint32_t value = 0; value < 256; value += 1) {
        rec_make(&r, SPANS, 64, 1);
        r.bytes[56] = (unsigned char)value;
        if (value <= 2) {
            CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
            reader.next_seq = 1;
        } else {
            REFUSE(&reader, &r, TA_RECORD_VALUE);
        }
        rec_make(&r, SPANS, 64, 1);
        r.bytes[57] = (unsigned char)value;
        if (value <= 1) {
            CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
            reader.next_seq = 1;
        } else {
            REFUSE(&reader, &r, TA_RECORD_VALUE);
        }
    }
    CHECK(ta_record_in_domain(0x7u, 2) && !ta_record_in_domain(0x7u, 3) && !ta_record_in_domain(~0ull, 64));
    CHECK(ta_record_in_domain(1ull << 63, 63) && !ta_record_in_domain(~0ull, 0xFFFFFFFFu));

    /* Spans are judged before values, and seq before both. */
    rec_make(&r, SPANS, 80, 1);
    r.bytes[56] = 9;
    rec_span(&r, 32, 0xFFFFFFF0u, 0x20u);
    REFUSE(&reader, &r, TA_RECORD_SPAN);
    rec_make(&r, SPANS, 80, 5);
    r.bytes[56] = 9;
    rec_span(&r, 32, 0xFFFFFFF0u, 0x20u);
    REFUSE(&reader, &r, TA_RECORD_SEQ);
}

/* The token holds a copy: bytes changed after the check do not move what the getters see. */
static void test_snapshot(void) {
    ta_record_reader reader = reader_make(TA_RING_LOSSLESS);
    rec r;
    ta_record out;
    rec_make(&r, SPANS, 80, 1);
    rec_span(&r, 32, 64, 8);
    rec_span(&r, 40, 72, 8);
    CHECK(check(&reader, r.bytes, r.length, &out) == TA_RECORD_OK);
    /* The host now rewrites the whole fixed part, including a span, to point far outside. */
    rec_span(&r, 32, 0xFFFFFFF0u, 0x20u);
    put64(&r, TA_RECORD_AT_SESSION, 12345);
    ta_bytes span = ta_record_span(&out, 32);
    CHECK(span.ptr == r.bytes + 64 && span.len == 8);
    CHECK(ta_record_u64(&out, TA_RECORD_AT_SESSION) == 7);
    /* The getters' own guards: outside the fixed part is zeros or empty, never a read. */
    CHECK(ta_record_u64(&out, 60) == 0 && ta_record_u32(&out, 62) == 0 && ta_record_u16(&out, 63) == 0);
    CHECK(ta_record_u8(&out, 64) == 0 && ta_record_u8(&out, 0xFFFFFFFFu) == 0 && ta_record_u32(&out, 0xFFFFFFFEu) == 0);
    CHECK(ta_record_u32(&out, 60) == 0 /* the last four bytes of the fixed part, never written */);
    CHECK(ta_record_span(&out, 60).len == 0 && ta_record_span(&out, 0xFFFFFFFFu).len == 0);
    ta_record forged = out;
    forged.size = 70; /* a hand-built token whose span does not fit: the view is empty, not outside */
    CHECK(ta_record_span(&forged, 40).len == 0);
    ta_record blank = { 0 };
    CHECK(ta_record_span(&blank, 32).len == 0 && ta_record_u64(&blank, 8) == 0);

    /* The writer's half: a span and its bytes, empty or not, and the well-formedness test. */
    unsigned char into[32];
    ta_mem_zero(into, sizeof into);
    ta_span32 put = { 99, 99 };
    const unsigned char text[3] = { 'a', 'b', 'c' };
    uint32_t tail = ta_record_put_span(&put, into, 8, (ta_bytes){ text, 3 });
    CHECK(tail == 11 && put.offset == 8 && put.length == 3 && into[8] == 'a' && into[10] == 'c' && into[11] == 0 && into[7] == 0);
    tail = ta_record_put_span(&put, into, tail, (ta_bytes){ NULL, 0 });
    CHECK(tail == 11 && put.offset == 0 && put.length == 0);
    CHECK(ta_record_bytes_ok((ta_bytes){ NULL, 0 }) && ta_record_bytes_ok((ta_bytes){ text, 3 }) && !ta_record_bytes_ok((ta_bytes){ NULL, 1 }));
}

static const char *const fault_names[] = { "ok", "truncated", "size", "kind", "wrong ring", "seq", "span", "value" };

static void test_names(void) {
    for (int fault = TA_RECORD_OK; fault <= TA_RECORD_VALUE; fault += 1) {
        const char *name = ta_record_fault_name((ta_record_fault)fault);
        CHECK(name != NULL && ta_str_equal(ta_str_from_cstr(name), ta_str_from_cstr(fault_names[fault])));
    }
    CHECK(ta_record_fault_name((ta_record_fault)99) != NULL);
}

int main(void) {
    test_init();
    test_init_validates_table();
    test_accept();
    test_truncated_and_size();
    test_kind();
    test_seq();
    test_spans();
    test_values();
    test_snapshot();
    test_names();
    if (failures != 0) {
        fprintf(stderr, "record_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("record_test: ok");
    return 0;
}
