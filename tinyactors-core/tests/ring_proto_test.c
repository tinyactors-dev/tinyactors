/* test: standalone asan */
/*
 * Block C2, the generated codec (src/ring_proto.h, src/ring_proto.c): the
 * descriptor table against ta_record_check, and, as the writers and getters
 * land, round trips, malformed records and fuzzing.
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

#define MAX_RECORD 4096u

/* The generated reader for ring `ring`, as the pump would build it. */
static ta_record_reader reader_for(ta_ring_id ring, ta_ring_mode mode) {
    ta_record_reader reader;
    ta_record_reader_params params = { .table = ta_msg_table(), .ring = ring, .mode = mode, .max_record_bytes = MAX_RECORD };
    CHECK(ta_record_reader_init(&reader, &params) == TA_OK);
    return reader;
}

/* The generated table is well formed (reader_init validates it), covers 38 messages and every ring. */
static void test_table(void) {
    const ta_record_table *table = ta_msg_table();
    CHECK(table != NULL && table->count == 38);
    uint32_t per_ring[TA_RING_COUNT] = { 0, 0, 0 };
    for (uint32_t i = 0; i < table->count; i += 1) {
        const ta_record_descriptor *descriptor = &table->descriptor[i];
        per_ring[descriptor->ring] += 1;
        CHECK(descriptor->fixed_size <= TA_RECORD_FIXED_MAX);
        CHECK((descriptor->kind >> 8) == (uint16_t)(descriptor->ring + 1)); /* 0x01xx h2c, 0x02xx c2h, 0x03xx tel */
    }
    CHECK(per_ring[TA_RING_H2C] == 15 && per_ring[TA_RING_C2H] == 21 && per_ring[TA_RING_TEL] == 2);
    (void)reader_for(TA_RING_H2C, TA_RING_LOSSLESS);
    (void)reader_for(TA_RING_C2H, TA_RING_LOSSLESS);
    (void)reader_for(TA_RING_TEL, TA_RING_LOSSY);
}

/*
 * A stand-in for the writers: a record of exactly the fixed size, all spans
 * empty, every enum at its lowest defined value, so that it is valid. The
 * checker must accept it for every kind, and refuse it on the other rings.
 */
static void test_every_kind_accepts_its_minimal_record(void) {
    const ta_record_table *table = ta_msg_table();
    for (uint32_t i = 0; i < table->count; i += 1) {
        const ta_record_descriptor *descriptor = &table->descriptor[i];
        unsigned char bytes[TA_RECORD_FIXED_MAX];
        ta_mem_zero(bytes, sizeof bytes);
        uint16_t kind = descriptor->kind;
        uint32_t size = descriptor->fixed_size;
        uint64_t seq = 1;
        ta_mem_copy(bytes + TA_RECORD_AT_KIND, &kind, sizeof kind);
        ta_mem_copy(bytes + TA_RECORD_AT_SIZE, &size, sizeof size);
        ta_mem_copy(bytes + TA_RECORD_AT_SEQ, &seq, sizeof seq);
        for (uint32_t d = 0; d < descriptor->domain_count; d += 1) {
            uint64_t allowed = descriptor->domains[d].allowed;
            uint8_t lowest = 0;
            while (((allowed >> lowest) & 1u) == 0) lowest += 1;
            bytes[descriptor->domains[d].at] = lowest;
        }
        for (int ring = 0; ring < TA_RING_COUNT; ring += 1) {
            ta_record_reader reader = reader_for((ta_ring_id)ring, TA_RING_LOSSLESS);
            ta_record record;
            ta_record_fault fault = ta_record_check(&reader, &(ta_record_input){ .bytes = bytes, .length = size }, &record);
            if (ring == descriptor->ring) {
                CHECK(fault == TA_RECORD_OK);
                CHECK(record.kind == kind && record.descriptor == descriptor);
            } else {
                CHECK(fault == TA_RECORD_WRONG_RING);
            }
        }
    }
}

static uint32_t round8(uint32_t n) { return (n + 7u) & ~7u; }

/* A ta_bytes of `length` bytes that the size function only counts: the pointer is never read. */
static ta_bytes claim(uint32_t length) { return (ta_bytes){ (const uint8_t *)"x", length }; }

static void test_size(void) {
    uint32_t size = 7;
    ta_msg_h2c_time time = { .head = { 0 } };
    CHECK(ta_msg_h2c_time_size(&time, &size) == TA_OK && size == 32);

    /* One span: the fixed part, the bytes, a round up to 8. */
    const uint8_t text[] = "abcdefghijklmnopqrstuvwxyz";
    for (uint32_t length = 0; length < 20; length += 1) {
        ta_msg_c2h_log log = { .label = { text, length } };
        CHECK(ta_msg_c2h_log_size(&log, &size) == TA_OK);
        CHECK(size == round8((uint32_t)sizeof(ta_msg_c2h_log_wire) + length) && size % 8 == 0);
    }

    /* Six spans add up; empty ones take nothing. */
    ta_msg_h2c_event event = { .target = claim(3), .name = claim(9), .invokeid = claim(1) };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_OK);
    CHECK(size == round8((uint32_t)sizeof(ta_msg_h2c_event_wire) + 13));

    /* Lengths that wrap a u32 sum are an overflow, not a small size, and *size is left alone. */
    size = 12345;
    event = (ta_msg_h2c_event){ .target = claim(0x40000000u), .name = claim(0x40000000u), .sendid = claim(0x40000000u),
                                .origin = claim(0x40000000u) };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_ERR_NOMEM && size == 12345);
    event = (ta_msg_h2c_event){ .target = claim(0xFFFFFFFFu) };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_ERR_NOMEM && size == 12345);
    event = (ta_msg_h2c_event){ .target = claim(0xFFFFFFFFu), .name = claim(0xFFFFFFFFu), .sendid = claim(0xFFFFFFFFu),
                                .origin = claim(0xFFFFFFFFu), .origin_type = claim(0xFFFFFFFFu), .invokeid = claim(0xFFFFFFFFu) };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_ERR_NOMEM && size == 12345);
    /* The largest record that still fits a u32 and is a multiple of 8, and one byte more. */
    uint32_t fixed = (uint32_t)sizeof(ta_msg_h2c_event_wire);
    event = (ta_msg_h2c_event){ .target = claim(0xFFFFFFF8u - fixed) };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_OK && size == 0xFFFFFFF8u);
    event = (ta_msg_h2c_event){ .target = claim(0xFFFFFFF8u - fixed + 1u) };
    size = 12345;
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_ERR_NOMEM && size == 12345);

    /* A view with a length and no pointer cannot be measured, wherever it is. */
    event = (ta_msg_h2c_event){ .invokeid = { NULL, 1 } };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_ERR_INVALID && size == 12345);
    event = (ta_msg_h2c_event){ .target = { NULL, 0 } };
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_OK && size == fixed);
}

/* ---- writers ---- */

#define CANARY 0xA5u
#define SLACK 16u

/* A plain buffer for one record: `size` bytes in the middle, SLACK canary bytes on each side, all 0xA5 to begin with. */
typedef struct arena {
    _Alignas(8) unsigned char bytes[MAX_RECORD + 2 * SLACK];
    uint32_t size;
} arena;

static void arena_reset(arena *a, uint32_t size) {
    for (size_t i = 0; i < sizeof a->bytes; i += 1) a->bytes[i] = CANARY;
    a->size = size;
}

static unsigned char *arena_record(arena *a) { return a->bytes + SLACK; }

/* No byte outside the record was written. */
static bool canaries_intact(const arena *a) {
    for (uint32_t i = 0; i < SLACK; i += 1) {
        if (a->bytes[i] != CANARY || a->bytes[SLACK + a->size + i] != CANARY) return false;
    }
    return true;
}

static bool untouched(const arena *a) {
    for (size_t i = 0; i < sizeof a->bytes; i += 1) {
        if (a->bytes[i] != CANARY) return false;
    }
    return true;
}

static ta_record_fault check_arena(ta_record_reader *reader, arena *a, ta_record *out) {
    return ta_record_check(reader, &(ta_record_input){ .bytes = arena_record(a), .length = a->size }, out);
}

static bool bytes_are(ta_bytes view, const char *expected, uint32_t length) {
    if (view.len != length) return false;
    for (uint32_t i = 0; i < length; i += 1) {
        if (view.ptr[i] != (uint8_t)expected[i]) return false;
    }
    return true;
}

static void test_write_event(void) {
    ta_msg_h2c_event event = { .head = { .flags = TA_MSG_FLAG_RECORDED, .session = 0x1122334455667788ull, .at = 99 },
                               .target = { (const uint8_t *)"ab/c", 4 },
                               .name = { (const uint8_t *)"go.now", 6 },
                               .origin = { (const uint8_t *)"o", 1 },
                               .invocation_seq = 42, .data = 0xDEADBEEFull, .type = TA_MSG_EVENT_TYPE_EXTERNAL };
    uint32_t size = 0;
    CHECK(ta_msg_h2c_event_size(&event, &size) == TA_OK);
    arena a;
    arena_reset(&a, size);
    ta_ring_reservation to = { .bytes = arena_record(&a), .size = size, .seq = 1 };
    CHECK(ta_msg_h2c_event_write(&event, &to) == TA_OK);
    CHECK(canaries_intact(&a));

    /* The reader accepts it and the header says what the writer was given. */
    ta_record_reader reader = reader_for(TA_RING_H2C, TA_RING_LOSSLESS);
    ta_record record;
    CHECK(check_arena(&reader, &a, &record) == TA_RECORD_OK);
    CHECK(record.kind == TA_MSG_H2C_EVENT && record.size == size && record.seq == 1);
    CHECK(ta_record_u16(&record, TA_RECORD_AT_FLAGS) == TA_MSG_FLAG_RECORDED);
    CHECK(ta_record_u64(&record, TA_RECORD_AT_SESSION) == 0x1122334455667788ull && ta_record_u64(&record, TA_RECORD_AT_TIME) == 99);
    CHECK(bytes_are(ta_record_span(&record, offsetof(ta_msg_h2c_event_wire, target)), "ab/c", 4));
    CHECK(bytes_are(ta_record_span(&record, offsetof(ta_msg_h2c_event_wire, name)), "go.now", 6));
    CHECK(ta_record_span(&record, offsetof(ta_msg_h2c_event_wire, sendid)).len == 0);
    CHECK(bytes_are(ta_record_span(&record, offsetof(ta_msg_h2c_event_wire, origin)), "o", 1));
    CHECK(ta_record_u64(&record, offsetof(ta_msg_h2c_event_wire, invocation_seq)) == 42);
    CHECK(ta_record_u64(&record, offsetof(ta_msg_h2c_event_wire, data)) == 0xDEADBEEFull);
    CHECK(ta_record_u8(&record, offsetof(ta_msg_h2c_event_wire, type)) == TA_MSG_EVENT_TYPE_EXTERNAL);

    /* Every pad and rounding byte is zero although the buffer was full of 0xA5: the fixed part's pad_end, and the tail's round up. */
    const unsigned char *bytes = arena_record(&a);
    for (uint32_t i = (uint32_t)offsetof(ta_msg_h2c_event_wire, type) + 1; i < sizeof(ta_msg_h2c_event_wire); i += 1) CHECK(bytes[i] == 0);
    for (uint32_t i = (uint32_t)sizeof(ta_msg_h2c_event_wire) + 11; i < size; i += 1) CHECK(bytes[i] == 0);

    /* The same data into a different buffer gives the same bytes (replay determinism), whatever was there. */
    arena b;
    arena_reset(&b, size);
    for (uint32_t i = 0; i < size; i += 1) arena_record(&b)[i] = (unsigned char)(i * 37u + 1u);
    ta_ring_reservation to_b = { .bytes = arena_record(&b), .size = size, .seq = 1 };
    CHECK(ta_msg_h2c_event_write(&event, &to_b) == TA_OK);
    for (uint32_t i = 0; i < size; i += 1) CHECK(arena_record(&a)[i] == arena_record(&b)[i]);
}

/* One refused write: the verdict, and the whole buffer, canaries included, as it was. */
static void refused(const ta_msg_h2c_event *data, arena *a, uint32_t buffer_size, ta_result expected) {
    arena_reset(a, buffer_size);
    ta_ring_reservation to = { .bytes = arena_record(a), .size = buffer_size, .seq = 1 };
    CHECK(ta_msg_h2c_event_write(data, &to) == expected);
    CHECK(untouched(a));
}

static void test_write_refuses(void) {
    ta_msg_h2c_event good = { .target = { (const uint8_t *)"ab", 2 }, .type = TA_MSG_EVENT_TYPE_EXTERNAL };
    uint32_t size = 0;
    CHECK(ta_msg_h2c_event_size(&good, &size) == TA_OK);
    arena a;

    refused(&good, &a, size - 1, TA_ERR_INVALID);
    refused(&good, &a, size + 8, TA_ERR_INVALID);
    arena_reset(&a, size);
    ta_ring_reservation none = { .bytes = NULL, .size = size, .seq = 1 };
    CHECK(ta_msg_h2c_event_write(&good, &none) == TA_ERR_INVALID);
    CHECK(untouched(&a));

    ta_msg_h2c_event bad = good;
    bad.type = 3; /* the set is 0..2 */
    refused(&bad, &a, size, TA_ERR_INVALID);
    bad = good;
    bad.head.flags = 0x0001; /* h2c.event defines no message flag bits */
    refused(&bad, &a, size, TA_ERR_INVALID);
    bad = good;
    bad.head.flags = 0x0400; /* a common bit nobody defined */
    refused(&bad, &a, size, TA_ERR_INVALID);
    bad = good;
    bad.name = (ta_bytes){ NULL, 5 };
    refused(&bad, &a, size, TA_ERR_INVALID);
    bad = good;
    bad.target = (ta_bytes){ (const uint8_t *)"x", 0xFFFFFFFFu };
    refused(&bad, &a, size, TA_ERR_NOMEM);

    /* The same data is fine when the reservation is right. */
    arena_reset(&a, size);
    ta_ring_reservation to = { .bytes = arena_record(&a), .size = size, .seq = 1 };
    CHECK(ta_msg_h2c_event_write(&good, &to) == TA_OK);
    CHECK(canaries_intact(&a));
}

static void test_write_scalars_and_flags(void) {
    arena a;
    uint32_t size = 0;
    ta_record record;

    /* No fixed fields and no spans: the 32-byte header alone. */
    ta_msg_h2c_time time = { .head = { .flags = TA_MSG_FLAG_REPLAYING, .at = 5000 } };
    CHECK(ta_msg_h2c_time_size(&time, &size) == TA_OK && size == 32);
    arena_reset(&a, size);
    ta_ring_reservation to_time = { .bytes = arena_record(&a), .size = size, .seq = 7 };
    CHECK(ta_msg_h2c_time_write(&time, &to_time) == TA_OK && canaries_intact(&a));
    ta_record_reader reader = reader_for(TA_RING_H2C, TA_RING_LOSSLESS);
    reader.next_seq = 7;
    CHECK(check_arena(&reader, &a, &record) == TA_RECORD_OK && record.seq == 7 && ta_record_u64(&record, TA_RECORD_AT_TIME) == 5000);

    /* Only scalars, with the extremes of the integer types. */
    ta_msg_c2h_macrostep_ended ended = { .macrostep = 3, .source = TA_MSG_INPUT_SOURCE_INVOKE_REPORT, .source_seq = 0xFFFFFFFFFFFFFFFFull,
                                         .effects = 0xFFFFFFFFu, .mail = 17, .status = TA_MSG_MACROSTEP_STATUS_ABANDONED };
    CHECK(ta_msg_c2h_macrostep_ended_size(&ended, &size) == TA_OK && size == sizeof(ta_msg_c2h_macrostep_ended_wire));
    arena_reset(&a, size);
    ta_ring_reservation to_ended = { .bytes = arena_record(&a), .size = size, .seq = 1 };
    CHECK(ta_msg_c2h_macrostep_ended_write(&ended, &to_ended) == TA_OK && canaries_intact(&a));
    ta_record_reader c2h = reader_for(TA_RING_C2H, TA_RING_LOSSLESS);
    CHECK(check_arena(&c2h, &a, &record) == TA_RECORD_OK);
    CHECK(ta_record_u64(&record, offsetof(ta_msg_c2h_macrostep_ended_wire, source_seq)) == 0xFFFFFFFFFFFFFFFFull);
    CHECK(ta_record_u32(&record, offsetof(ta_msg_c2h_macrostep_ended_wire, effects)) == 0xFFFFFFFFu);
    CHECK(ta_record_u8(&record, offsetof(ta_msg_c2h_macrostep_ended_wire, status)) == TA_MSG_MACROSTEP_STATUS_ABANDONED);

    /* tel.trace: two message flags of its own, three spans, on the lossy ring. */
    const char states[] = "s1 s2";
    ta_msg_tel_trace trace = { .head = { .flags = 0x0003 }, .microstep = 9, .trace_kind = 0x1234, .position = 2,
                               .states = { (const uint8_t *)states, 5 } };
    CHECK(ta_msg_tel_trace_size(&trace, &size) == TA_OK);
    arena_reset(&a, size);
    ta_ring_reservation to_trace = { .bytes = arena_record(&a), .size = size, .seq = 4 };
    CHECK(ta_msg_tel_trace_write(&trace, &to_trace) == TA_OK && canaries_intact(&a));
    ta_record_reader tel = reader_for(TA_RING_TEL, TA_RING_LOSSY);
    CHECK(check_arena(&tel, &a, &record) == TA_RECORD_OK && record.seq == 4 && record.gap == 3);
    CHECK(ta_record_u16(&record, TA_RECORD_AT_FLAGS) == 0x0003);
    CHECK(ta_record_u16(&record, offsetof(ta_msg_tel_trace_wire, trace_kind)) == 0x1234);
    CHECK(bytes_are(ta_record_span(&record, offsetof(ta_msg_tel_trace_wire, states)), states, 5));

    /* h2c.invoke_report: a bool field refuses 2. */
    ta_msg_h2c_invoke_report report = { .invokeid = { (const uint8_t *)"i", 1 }, .kind = TA_MSG_INVOKE_REPORT_KIND_ERROR, .ends = 2 };
    CHECK(ta_msg_h2c_invoke_report_size(&report, &size) == TA_OK);
    arena_reset(&a, size);
    ta_ring_reservation to_report = { .bytes = arena_record(&a), .size = size, .seq = 1 };
    CHECK(ta_msg_h2c_invoke_report_write(&report, &to_report) == TA_ERR_INVALID && untouched(&a));
    report.ends = 1;
    CHECK(ta_msg_h2c_invoke_report_write(&report, &to_report) == TA_OK && canaries_intact(&a));
}

int main(void) {
    test_table();
    test_every_kind_accepts_its_minimal_record();
    test_size();
    test_write_event();
    test_write_refuses();
    test_write_scalars_and_flags();
    if (failures != 0) {
        fprintf(stderr, "ring_proto_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("ring_proto_test: ok");
    return 0;
}
