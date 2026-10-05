/* test: standalone asan */
/*
 * Block tel (src/tel.h, once it exists) and the rings a system owns (ADR
 * 0098): the control block and the telemetry ring made at ta_system_create,
 * the public reader (ta_system_control_block, ta_ring_peek, ta_ring_release),
 * and, in later sections of this file, the producer.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition)                                                                 \
    do {                                                                                 \
        if (!(condition)) {                                                              \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                               \
        }                                                                                \
    } while (0)

static ta_test_allocator heap = { .strict = true };

static void finish_clean(void) {
    CHECK(ta_test_allocator_finish(&heap) == 0);
    CHECK(heap.stats.bad_frees == 0);
}

static ta_system *create_with(const ta_param_value *params, uint32_t count) {
    ta_system *system = NULL;
    ta_result result = ta_system_create(&(ta_system_config){ .allocator = ta_test_allocator_get(&heap),
                                                             .params = params, .param_count = count },
                                        &system);
    return result == TA_OK ? system : NULL;
}

/* ------------------------------------------------------------------------
 * The rings a system owns
 */

static void test_control_block(void) {
    ta_system *system = create_with(NULL, 0);
    CHECK(system != NULL);
    const ta_ring_control *control = ta_system_control_block(system);
    CHECK(control != NULL && ta_system_control_block(NULL) == NULL);
    CHECK(control->magic == TA_RING_MAGIC && control->version == TA_RING_VERSION &&
          control->ring_count == TA_RING_COUNT);
    /* h2c and c2h do not exist until ring-proto 5.1: they are entries with no bytes. */
    CHECK(control->h2c.capacity == 0 && control->h2c.max_record_bytes == 0);
    CHECK(control->c2h.capacity == 0 && control->c2h.max_record_bytes == 0);
    /* tel is telBytes with records of at most maxRecordBytes, and lossy. */
    CHECK(control->tel.capacity == TA_PARAM_DEFAULT_TEL_BYTES);
    CHECK(control->tel.max_record_bytes == TA_PARAM_DEFAULT_MAX_RECORD_BYTES);
    CHECK(control->tel.mode == (uint32_t)TA_RING_LOSSY && control->h2c.mode == (uint32_t)TA_RING_LOSSLESS);
    CHECK(control->tel.offset >= sizeof(ta_ring_control) && control->tel.offset % 8 == 0);
    ta_ring_shared cells[3] = { control->h2c, control->c2h, control->tel };
    for (uint32_t id = 0; id < 3; id += 1) {
        CHECK(cells[id].head == 0 && cells[id].tail == 0 && cells[id].next_seq == 1 && cells[id].dropped == 0);
    }
    /* The staging area sits behind the ring areas, inside the same allocation, and is maxRecordBytes long. */
    CHECK(system->tel_staging_bytes == TA_PARAM_DEFAULT_MAX_RECORD_BYTES);
    CHECK(system->tel_staging >= (unsigned char *)system->rings_memory + control->tel.offset + control->tel.capacity);
    CHECK(system->tel_staging + system->tel_staging_bytes <= (unsigned char *)system->rings_memory + system->rings_bytes);
    ta_system_destroy(system);
    finish_clean();
}

static void test_reading_empty_rings(void) {
    ta_system *system = create_with(NULL, 0);
    ta_ring_record record = { (const uint8_t *)1, 99, 99, 99 };
    for (uint32_t id = 0; id < TA_RING_COUNT; id += 1) {
        CHECK(ta_ring_peek(system, (ta_ring_id)id, &record) == TA_ERR_NOT_FOUND);
        CHECK(record.bytes == (const uint8_t *)1 && record.size == 99 && record.seq == 99); /* untouched */
        ta_ring_release(system, (ta_ring_id)id, 0);
        ta_ring_release(system, (ta_ring_id)id, 16);
    }
    const ta_ring_control *control = ta_system_control_block(system);
    CHECK(control->tel.tail == 0 && control->tel.head == 0);
    CHECK(ta_ring_peek(NULL, TA_RING_TEL, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, TA_RING_COUNT, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, (ta_ring_id)-1, &record) == TA_ERR_INVALID);
    CHECK(ta_ring_peek(system, TA_RING_TEL, NULL) == TA_ERR_INVALID);
    ta_ring_release(NULL, TA_RING_TEL, 16);
    ta_ring_release(system, TA_RING_COUNT, 16);
    ta_system_destroy(system);
    finish_clean();
}

/* A record written by hand through the ring handle is seen, and released only with its own size. */
static void test_peek_release_through_the_system(void) {
    ta_system *system = create_with(NULL, 0);
    ta_ring *tel = &system->rings.ring[TA_RING_TEL];
    ta_ring_reservation reservation;
    CHECK(ta_ring_reserve(tel, 24, &reservation) == TA_RING_OK);
    reservation.bytes[0] = 0x01;
    reservation.bytes[1] = 0x03;
    ta_ring_commit(tel);
    ta_ring_record record;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &record) == TA_OK);
    CHECK(record.size == 24 && record.seq == 1 && record.gap == 0 && record.bytes[1] == 0x03);
    ta_ring_record again;
    CHECK(ta_ring_peek(system, TA_RING_TEL, &again) == TA_OK && again.bytes == record.bytes);
    ta_ring_release(system, TA_RING_TEL, 16); /* not the size peeked: nothing happens */
    CHECK(ta_system_control_block(system)->tel.tail == 0);
    ta_ring_release(system, TA_RING_C2H, 24); /* another ring: nothing happens */
    CHECK(ta_system_control_block(system)->tel.tail == 0);
    ta_ring_release(system, TA_RING_TEL, 24);
    CHECK(ta_system_control_block(system)->tel.tail == 24);
    CHECK(ta_ring_peek(system, TA_RING_TEL, &record) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
    finish_clean();
}

/* ------------------------------------------------------------------------
 * One allocation, sized by the parameters, freed with the same size
 */

static void test_one_allocation_sized_by_parameters(void) {
    ta_param_value small[] = { { TA_PARAM_TEL_BYTES, 8192 } };
    ta_test_allocator_reset_counts(&heap);
    ta_system *base = create_with(small, 1);
    uint64_t base_allocations = heap.stats.allocations;
    size_t base_bytes = heap.stats.live_bytes;
    ta_system_destroy(base);
    finish_clean();

    ta_param_value large[] = { { TA_PARAM_TEL_BYTES, 8192 + 24576 }, { TA_PARAM_MAX_RECORD_BYTES, 4096 + 1024 } };
    ta_test_allocator_reset_counts(&heap);
    ta_system *bigger = create_with(large, 2);
    /* The rings are one allocation however big: the count does not move, the bytes move by telBytes and by the staging area. */
    CHECK(heap.stats.allocations == base_allocations);
    CHECK(heap.stats.live_bytes - base_bytes == 24576u + 1024u);
    CHECK(bigger->tel_staging_bytes == 5120);
    CHECK(ta_system_control_block(bigger)->tel.capacity == 8192 + 24576);
    CHECK(ta_system_control_block(bigger)->tel.max_record_bytes == 5120);
    ta_system_destroy(bigger);
    finish_clean();

    /* The smallest system: one record of 512 bytes is the whole ring. */
    ta_param_value tight[] = { { TA_PARAM_TEL_BYTES, 512 }, { TA_PARAM_MAX_RECORD_BYTES, 512 },
                               { TA_PARAM_TELEMETRY_VALUE_BYTES, 0 } };
    ta_system *least = create_with(tight, 3);
    CHECK(least != NULL && ta_system_control_block(least)->tel.capacity == 512);
    ta_system_destroy(least);
    finish_clean();
}

/* Every allocation a creation makes can fail: a clean failure, nothing held, every free sized as its allocation. */
static void test_creation_allocation_sweep(void) {
    ta_test_allocator_reset_counts(&heap);
    ta_system *counted = create_with(NULL, 0);
    uint64_t allocations = heap.stats.allocations;
    ta_system_destroy(counted);
    finish_clean();
    CHECK(allocations > 1);
    for (uint64_t nth = 1; nth <= allocations; nth += 1) {
        ta_test_allocator_reset_counts(&heap);
        ta_test_allocator_fail_nth(&heap, nth);
        ta_system *system = create_with(NULL, 0);
        ta_test_allocator_disarm(&heap);
        CHECK(system == NULL);
        finish_clean();
    }
}

int main(void) {
    test_control_block();
    test_reading_empty_rings();
    test_peek_release_through_the_system();
    test_one_allocation_sized_by_parameters();
    test_creation_allocation_sweep();
    if (failures != 0) {
        fprintf(stderr, "tel_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tel_test: ok");
    return 0;
}
