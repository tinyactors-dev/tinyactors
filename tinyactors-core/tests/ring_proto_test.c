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

int main(void) {
    test_table();
    test_every_kind_accepts_its_minimal_record();
    if (failures != 0) {
        fprintf(stderr, "ring_proto_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("ring_proto_test: ok");
    return 0;
}
