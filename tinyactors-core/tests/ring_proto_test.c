/* test: standalone asan exhaustive */
/*
 * Block C2, the generated codec (src/ring_proto.h, src/ring_proto.c): the
 * descriptor table against ta_record_check, and, as the writers and getters
 * land, round trips, malformed records and fuzzing.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_allocator.h"
#include "ring_proto_cases.h"

static int failures = 0;

/* TA_TEST_FULL=1 asks for the exhaustive sizes (`mise run test --full`); read once at the top of main. */
static bool full_run = false;

/* pick answers the fast size, or the full one when TA_TEST_FULL asked for it. It only chooses sizes, never an asserted value. */
static uint64_t pick(uint64_t fast, uint64_t full) { return full_run ? full : fast; }

/* The test's own buffers come from a strict allocator, with sized frees as the library's code does. The codec itself never allocates. */
static ta_test_allocator heap = { .strict = true };

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

static bool is_zero(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i += 1) if (b[i] != 0) return false;
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

/* ---- every message: write, commit, peek, check, get ---- */

static uint8_t pool[RING_PROTO_CASE_POOL_BYTES];

static unsigned char *heap_alloc(size_t size) {
    unsigned char *p = ta_mem_alloc(ta_test_allocator_get(&heap), size, 8);
    CHECK(p != NULL);
    return p;
}

static void heap_free(unsigned char *p, size_t size) {
    ta_mem_free(ta_test_allocator_get(&heap), p, &(ta_allocator_free_params){ .ptr = p, .size = size, .alignment = 8 });
}

/* Three rings in one region, as a system would build them; the region starts full of 0xA5, like reused memory. */
typedef struct world {
    ta_rings rings;
    ta_rings_plan plan;
    unsigned char *memory;
    ta_record_reader reader[TA_RING_COUNT];
} world;

static bool world_up(world *w) {
    ta_ring_spec spec = { 4096, 1024, TA_RING_LOSSLESS };
    ta_rings_specs specs = { .ring = { spec, spec, spec } };
    specs.ring[TA_RING_TEL].mode = TA_RING_LOSSY;
    if (ta_rings_layout(&specs, &w->plan) != TA_OK) return false;
    w->memory = ta_mem_alloc(ta_test_allocator_get(&heap), w->plan.bytes, w->plan.alignment);
    if (w->memory == NULL) return false;
    memset(w->memory, (int)CANARY, w->plan.bytes);
    if (ta_rings_init(&w->rings, &(ta_rings_init_params){ .memory = w->memory, .memory_bytes = w->plan.bytes, .specs = specs }) != TA_OK) return false;
    for (int ring = 0; ring < TA_RING_COUNT; ring += 1) {
        ta_record_reader_params params = { .table = ta_msg_table(), .ring = (ta_ring_id)ring, .mode = specs.ring[ring].mode,
                                           .max_record_bytes = 1024 };
        if (ta_record_reader_init(&w->reader[ring], &params) != TA_OK) return false;
    }
    return true;
}

static void world_down(world *w) {
    ta_mem_free(ta_test_allocator_get(&heap), w->memory,
                &(ta_allocator_free_params){ .ptr = w->memory, .size = w->plan.bytes, .alignment = w->plan.alignment });
}

static ta_ring_id ring_of(uint16_t kind) { return (ta_ring_id)((kind >> 8) - 1); }

/* The record a case writes for a seed, in a canary-framed arena whose record bytes start as `fill`. */
static bool write_into_arena(const ring_proto_case *c, const void *data, uint32_t size, unsigned char fill, arena *a) {
    arena_reset(a, size);
    memset(arena_record(a), fill, size);
    ta_ring_reservation to = { .bytes = arena_record(a), .size = size, .seq = 5 };
    ta_result result = c->write(data, &to);
    CHECK(result == TA_OK);
    CHECK(canaries_intact(a));
    return result == TA_OK;
}

static void test_round_trip_every_message(unsigned seeds) {
    world w;
    CHECK(world_up(&w));
    ta_test_allocator_reset_counts(&heap);
    ta_test_allocator_refuse_all(&heap, true); /* the codec and the rings must not notice */
    static arena a, b, c3;
    _Alignas(8) unsigned char data[RING_PROTO_CASE_DATA_BYTES];
    _Alignas(8) unsigned char out[RING_PROTO_CASE_DATA_BYTES];
    _Alignas(8) unsigned char other[RING_PROTO_CASE_DATA_BYTES];
    uint32_t records = 0;
    for (size_t index = 0; index < TA_ARRAY_COUNT(ring_proto_cases); index += 1) {
        const ring_proto_case *c = &ring_proto_cases[index];
        ta_ring *ring = &w.rings.ring[ring_of(c->kind)];
        for (uint32_t seed = 0; seed < seeds; seed += 1) {
            c->fill(data, seed, pool);
            uint32_t size = 0;
            CHECK(c->size(data, &size) == TA_OK);
            CHECK(size >= TA_RECORD_HEADER_BYTES && size % 8 == 0);

            /* The bytes do not depend on what the buffer held before: zero, the ring's 0xA5, and a third pattern. */
            if (!write_into_arena(c, data, size, 0x00, &a) || !write_into_arena(c, data, size, 0xA5, &b) ||
                !write_into_arena(c, data, size, 0x5A, &c3)) {
                return;
            }
            CHECK(memcmp(arena_record(&a), arena_record(&b), size) == 0 && memcmp(arena_record(&a), arena_record(&c3), size) == 0);

            /* Through a real ring: reserve, write, commit, peek, check, get. */
            ta_ring_reservation to;
            CHECK(ta_ring_reserve(ring, size, &to) == TA_RING_OK);
            if (to.bytes == NULL) return;
            CHECK(c->write(data, &to) == TA_OK);
            ta_ring_commit(ring);
            ta_ring_record raw;
            CHECK(ta_ring_peek(ring, &raw) == TA_RING_OK);
            if (raw.bytes == NULL) return;
            CHECK(raw.size == size && raw.seq == to.seq);
            ta_record record;
            ta_record_fault fault = ta_record_check(&w.reader[ring_of(c->kind)], &(ta_record_input){ .bytes = raw.bytes, .length = raw.size }, &record);
            if (fault != TA_RECORD_OK) {
                fprintf(stderr, "%s seed %u: %s\n", c->id, seed, ta_record_fault_name(fault));
                CHECK(fault == TA_RECORD_OK);
                ta_ring_release(ring);
                continue;
            }
            CHECK(record.kind == c->kind && record.seq == raw.seq && record.size == size);

            /* The writer stored what commit stores, and the other bytes are the arena's. */
            ta_mem_zero(out, sizeof out);
            CHECK(c->get(&record, out) == TA_OK);
            CHECK(c->equal(data, out));
            for (uint32_t i = 0; i < size; i += 1) {
                if (i >= 8 && i < 16) continue; /* the seq: the arenas used 5, the ring its own */
                if (raw.bytes[i] != arena_record(&a)[i]) {
                    CHECK(raw.bytes[i] == arena_record(&a)[i]);
                    break;
                }
            }

            /* A getter of another kind refuses the record and leaves its output alone. */
            const ring_proto_case *wrong = &ring_proto_cases[(index + 1) % TA_ARRAY_COUNT(ring_proto_cases)];
            ta_mem_zero(other, sizeof other);
            CHECK(wrong->get(&record, other) == TA_ERR_INVALID);
            CHECK(is_zero(other, sizeof other));
            ta_ring_release(ring);
            records += 1;
        }
    }
    ta_test_allocator_refuse_all(&heap, false);
    CHECK(heap.stats.attempts == 0 && heap.stats.refused == 0);
    CHECK(records == seeds * TA_ARRAY_COUNT(ring_proto_cases));
    world_down(&w);
}

/* ---- malformed records ---- */

typedef struct trial {
    ta_ring_id ring;
    ta_ring_mode mode;
    uint64_t next_seq;
} trial;

/*
 * trial_check runs ta_record_check on a copy of the bytes in an allocation of
 * exactly `length` bytes, so that AddressSanitizer sees any read past the end.
 * On acceptance it asserts the invariants of the contract: next_seq moved to
 * seq + 1, and every span of the message lies inside the copy. On refusal it
 * asserts next_seq did not move. The returned record's pointers are dead (the
 * copy is freed); only its scalar fields are for the caller to look at.
 */
static ta_record_fault trial_check(const trial *t, const unsigned char *bytes, size_t length, ta_record *out) {
    ta_record_reader reader;
    ta_record_reader_params params = { .table = ta_msg_table(), .ring = t->ring, .mode = t->mode, .max_record_bytes = 1024 };
    CHECK(ta_record_reader_init(&reader, &params) == TA_OK);
    reader.next_seq = t->next_seq;
    unsigned char *copy = heap_alloc(length == 0 ? 1 : length);
    if (copy == NULL) return TA_RECORD_OK;
    if (length > 0) memcpy(copy, bytes, length);
    ta_record_fault fault = ta_record_check(&reader, &(ta_record_input){ .bytes = length == 0 ? NULL : copy, .length = length }, out);
    CHECK((unsigned)fault <= (unsigned)TA_RECORD_VALUE);
    if (fault == TA_RECORD_OK) {
        CHECK(reader.next_seq == out->seq + 1 && out->seq >= t->next_seq);
        CHECK(out->size == length && out->bytes == copy);
        for (uint32_t i = 0; i < out->descriptor->span_count; i += 1) {
            ta_bytes view = ta_record_span(out, out->descriptor->spans[i].at);
            CHECK(view.len == 0 || (view.ptr >= copy && view.len <= length && (size_t)(view.ptr - copy) <= length - view.len));
        }
    } else {
        CHECK(reader.next_seq == t->next_seq);
    }
    heap_free(copy, length == 0 ? 1 : length);
    return fault;
}

static const ta_record_descriptor *descriptor_of(uint16_t kind) {
    const ta_record_table *table = ta_msg_table();
    for (uint32_t i = 0; i < table->count; i += 1) {
        if (table->descriptor[i].kind == kind) return &table->descriptor[i];
    }
    return NULL;
}

static void poke16(unsigned char *bytes, uint32_t at, uint16_t v) { memcpy(bytes + at, &v, sizeof v); }
static void poke32(unsigned char *bytes, uint32_t at, uint32_t v) { memcpy(bytes + at, &v, sizeof v); }
static void poke64(unsigned char *bytes, uint32_t at, uint64_t v) { memcpy(bytes + at, &v, sizeof v); }

#define EXPECT(got, want) \
    do { \
        ta_record_fault got_ = (got); \
        if (got_ != (want)) fprintf(stderr, "%s:%d: %s: wanted %s, got %s\n", __FILE__, __LINE__, c->id, ta_record_fault_name(want), ta_record_fault_name(got_)); \
        CHECK(got_ == (want)); \
    } while (0)

/* The span rule of record.h, restated in 64-bit arithmetic so that it cannot share a wrap with the code under test. */
static bool span_valid(uint32_t offset, uint32_t length, uint32_t fixed, uint32_t size) {
    if (length == 0) return offset == 0;
    return offset >= fixed && (uint64_t)offset + length <= size;
}

/* One valid record per message, then every single-field corruption of it, each refused with its reason. */
static void test_malformed_every_message(void) {
    static const uint32_t offsets[] = { 0, 1, 31, 0xFFFFFFFFu, 0xFFFFFFF0u, 0x80000000u };
    static const uint32_t lengths[] = { 0, 1, 7, 0x20u, 0xFFFFFFFFu, 0x80000000u };
    _Alignas(8) unsigned char data[RING_PROTO_CASE_DATA_BYTES];
    static arena a;
    for (size_t index = 0; index < TA_ARRAY_COUNT(ring_proto_cases); index += 1) {
        const ring_proto_case *c = &ring_proto_cases[index];
        const ta_record_descriptor *d = descriptor_of(c->kind);
        CHECK(d != NULL);
        if (d == NULL) continue;
        ta_ring_mode mode = d->ring == TA_RING_TEL ? TA_RING_LOSSY : TA_RING_LOSSLESS;
        trial t = { (ta_ring_id)d->ring, mode, 5 };
        c->fill(data, 11, pool);
        uint32_t size = 0;
        CHECK(c->size(data, &size) == TA_OK);
        arena_reset(&a, size);
        ta_ring_reservation to = { .bytes = arena_record(&a), .size = size, .seq = 5 };
        CHECK(c->write(data, &to) == TA_OK);
        unsigned char good[1024];
        memcpy(good, arena_record(&a), size);
        unsigned char bad[1024];
        ta_record record;
        uint32_t fixed = d->fixed_size;

        EXPECT(trial_check(&t, good, size, &record), TA_RECORD_OK);
        CHECK(record.kind == c->kind && record.seq == 5 && record.gap == 0);

        /* Every truncation, and two extensions. */
        for (uint32_t length = 0; length < size; length += 1) {
            EXPECT(trial_check(&t, good, length, &record), length < TA_RECORD_HEADER_BYTES ? TA_RECORD_TRUNCATED : TA_RECORD_SIZE);
        }
        memcpy(bad, good, size);
        memset(bad + size, 0, 16);
        EXPECT(trial_check(&t, bad, size + 1, &record), TA_RECORD_SIZE);
        EXPECT(trial_check(&t, bad, size + 8, &record), TA_RECORD_SIZE);

        /* The size field. */
        static const uint32_t size_deltas[] = { 1, 8, 0x7FFFFFF8u };
        for (size_t k = 0; k < TA_ARRAY_COUNT(size_deltas); k += 1) {
            memcpy(bad, good, size);
            poke32(bad, TA_RECORD_AT_SIZE, size + size_deltas[k]);
            EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_SIZE);
            poke32(bad, TA_RECORD_AT_SIZE, size - size_deltas[k]);
            EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_SIZE);
        }
        memcpy(bad, good, size);
        poke32(bad, TA_RECORD_AT_SIZE, 0);
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_SIZE);
        poke32(bad, TA_RECORD_AT_SIZE, 0xFFFFFFFFu);
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_SIZE);

        /* The kind: the pad, an unknown one, and one that belongs to another ring. */
        memcpy(bad, good, size);
        poke16(bad, TA_RECORD_AT_KIND, 0);
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_KIND);
        poke16(bad, TA_RECORD_AT_KIND, 0xFFFF);
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_KIND);
        poke16(bad, TA_RECORD_AT_KIND, (uint16_t)(c->kind ^ 0x00FFu)); /* same ring, no such message */
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_KIND);
        poke16(bad, TA_RECORD_AT_KIND, (uint16_t)(d->ring == TA_RING_H2C ? TA_MSG_C2H_LOG : TA_MSG_H2C_TIME));
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_WRONG_RING);

        /* The seq: lossless wants exactly 5; lossy accepts a jump forward and no step back. */
        static const uint64_t seqs[] = { 0, 1, 4, 6, 0x100000000ull, UINT64_MAX };
        for (size_t k = 0; k < TA_ARRAY_COUNT(seqs); k += 1) {
            memcpy(bad, good, size);
            poke64(bad, TA_RECORD_AT_SEQ, seqs[k]);
            bool accepted = mode == TA_RING_LOSSY && seqs[k] >= 5 && seqs[k] != UINT64_MAX;
            EXPECT(trial_check(&t, bad, size, &record), accepted ? TA_RECORD_OK : TA_RECORD_SEQ);
            if (accepted) CHECK(record.gap == seqs[k] - 5);
        }

        /* The flag bits: only the ones the message defines. */
        for (uint32_t bit = 0; bit < 16; bit += 1) {
            memcpy(bad, good, size);
            poke16(bad, TA_RECORD_AT_FLAGS, (uint16_t)(1u << bit));
            EXPECT(trial_check(&t, bad, size, &record), (d->flags_allowed >> bit) & 1u ? TA_RECORD_OK : TA_RECORD_VALUE);
        }
        memcpy(bad, good, size);
        poke16(bad, TA_RECORD_AT_FLAGS, 0xFFFF);
        EXPECT(trial_check(&t, bad, size, &record), TA_RECORD_VALUE);

        /* Each span, replaced by every combination of awkward offsets and lengths. */
        uint32_t picks_offset[16];
        uint32_t picks_length[16];
        size_t n_offset = 0;
        size_t n_length = 0;
        for (size_t k = 0; k < TA_ARRAY_COUNT(offsets); k += 1) picks_offset[n_offset++] = offsets[k];
        picks_offset[n_offset++] = fixed - 1;
        picks_offset[n_offset++] = fixed;
        picks_offset[n_offset++] = fixed + 1;
        picks_offset[n_offset++] = size - 1;
        picks_offset[n_offset++] = size;
        picks_offset[n_offset++] = size + 1;
        for (size_t k = 0; k < TA_ARRAY_COUNT(lengths); k += 1) picks_length[n_length++] = lengths[k];
        picks_length[n_length++] = size - fixed;
        picks_length[n_length++] = size;
        for (uint32_t s = 0; s < d->span_count; s += 1) {
            for (size_t i = 0; i < n_offset; i += 1) {
                for (size_t j = 0; j < n_length; j += 1) {
                    memcpy(bad, good, size);
                    poke32(bad, d->spans[s].at, picks_offset[i]);
                    poke32(bad, d->spans[s].at + 4u, picks_length[j]);
                    EXPECT(trial_check(&t, bad, size, &record), span_valid(picks_offset[i], picks_length[j], fixed, size) ? TA_RECORD_OK : TA_RECORD_SPAN);
                }
            }
        }

        /* Each enum and bool, over every byte value. */
        for (uint32_t k = 0; k < d->domain_count; k += 1) {
            for (uint32_t value = 0; value < 256; value += 1) {
                memcpy(bad, good, size);
                bad[d->domains[k].at] = (unsigned char)value;
                EXPECT(trial_check(&t, bad, size, &record), ta_record_in_domain(d->domains[k].allowed, value) ? TA_RECORD_OK : TA_RECORD_VALUE);
            }
        }
    }
}

int main(void) {
    const char *requested = getenv("TA_TEST_FULL");
    full_run = requested != NULL && requested[0] != '\0' && strcmp(requested, "0") != 0;
    for (uint32_t i = 0; i < sizeof pool; i += 1) pool[i] = (uint8_t)(i * 7u + 3u);
    test_table();
    test_every_kind_accepts_its_minimal_record();
    test_size();
    test_write_event();
    test_write_refuses();
    test_write_scalars_and_flags();
    test_round_trip_every_message((unsigned)pick(24, 400));
    test_malformed_every_message();
    if (failures != 0) {
        fprintf(stderr, "ring_proto_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("ring_proto_test: ok");
    return 0;
}
