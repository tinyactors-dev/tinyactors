/*
 * ta_ring: the single-producer, single-consumer byte ring (src/ring.h)
 * behind h2c, c2h and tel.
 *
 * The test includes the amalgamation so it reaches the private unit; it
 * uses the C library, the core does not. Memory comes from a counting
 * allocator local to this test, which can fail after a number of
 * allocations and records the size of every request.
 *
 * Claims proven here, in order:
 *
 * 1. init refuses an unknown mode, a max_record_bytes that is not a
 *    multiple of 8 or below 16, and a capacity that is not a multiple of
 *    8 or below max_record_bytes, with TA_ERR_INVALID, and a refused
 *    allocation with TA_ERR_NOMEM, leaving the ring untouched and nothing
 *    allocated; a valid init allocates one buffer of exactly the capacity
 *    at alignment 8 and gives an empty ring whose first seq is 1, on which
 *    peek and release answer TA_ERR_NOT_FOUND;
 * 2. reserve rounds the size up to 8, stamps kind 0, flags 0, the size
 *    and the seq, and numbers records from 1 without gaps; nothing is
 *    visible until commit; one commit publishes several reservations; the
 *    consumer reads the records in place, in order, as they were filled;
 * 3. reserve of a size below 16 or above max_record_bytes answers
 *    TA_ERR_INVALID and changes nothing;
 * 4. a record wraps at every offset: from every 8-byte offset of the
 *    buffer and for every record size, a record that does not fit before
 *    the end gets a pad (kind 0, size = the bytes to the end) and starts
 *    at offset 0, never splits, and the consumer never sees the pad;
 *    this holds with counters near 2^40 as well;
 * 5. a full lossless ring answers TA_ERR_FULL and changes nothing (the
 *    counters, the seq and every byte); after a release the record fits;
 * 6. a ring of exactly max_record_bytes takes a largest record at every
 *    offset: when the pad and the record could never fit, the pad alone
 *    is committed, the reserve answers TA_ERR_FULL without taking a seq,
 *    and after the pad is released the record goes at offset 0; with a
 *    reservation open the pad is not written and nothing changes;
 * 7. a full lossy ring drops the record, counts it in dropped, consumes
 *    its seq and answers TA_ERR_FULL; take_drops answers the count and the
 *    first lost seq and forgets them, and TA_ERR_NOT_FOUND when none are
 *    left; the reader sees the gap and accepts it;
 * 8. peek and release answer TA_ERR_CORRUPT, change nothing and write
 *    nothing to out, for a record whose size is not a multiple of 8, is
 *    below 16 (8 for a pad), above max_record_bytes, above the committed
 *    bytes or past the end of the buffer, for a pad that ends before the
 *    end of the buffer, for a seq gap or repeat on a lossless ring, and
 *    for a seq that does not grow on a lossy ring;
 * 9. release releases exactly one record and the pads before it, and
 *    answers TA_ERR_NOT_FOUND on an empty ring;
 * 10. reserve, commit, peek, release, take_drops and the counters never
 *     allocate;
 * 11. destroy frees the buffer;
 * 12. 20000 random reserves, commits, peeks, releases and drop takes in
 *     each mode agree with a model of the committed records at every
 *     step, and keep tail <= head <= write <= tail + capacity.
 */
#include "../include/tinyactors.c"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * An allocator that counts and can fail.
 */

#define MAX_BLOCKS 16u

typedef struct {
    void *ptrs[MAX_BLOCKS];
    size_t sizes[MAX_BLOCKS];
    uint32_t live;
    uint32_t attempts;
    /* The attempt, counting from 1, from which every allocation fails; 0
     * never fails. */
    uint32_t fail_from;
    size_t last_size;
    size_t last_alignment;
} counting_allocator;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *heap = self;
    heap->attempts += 1u;
    heap->last_size = size;
    heap->last_alignment = alignment;
    if ((heap->fail_from != 0 && heap->attempts >= heap->fail_from) || heap->live == MAX_BLOCKS) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size_t rounded = (size + alignment - 1u) / alignment * alignment;
    void *ptr = aligned_alloc(alignment, rounded);
    if (ptr != NULL) {
        memset(ptr, 0xA5, rounded);
        heap->sizes[heap->live] = size;
        heap->ptrs[heap->live++] = ptr;
    }
    return ptr;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *heap = self;
    for (uint32_t index = 0; index < heap->live; index += 1u) {
        if (heap->ptrs[index] == params->ptr) {
            if (heap->sizes[index] != params->size) {
                fprintf(stderr, "free of %zu bytes, allocated as %zu\n", params->size, heap->sizes[index]);
                failures += 1;
            }
            heap->live -= 1u;
            heap->ptrs[index] = heap->ptrs[heap->live];
            heap->sizes[index] = heap->sizes[heap->live];
            free(params->ptr);
            return;
        }
    }
    fprintf(stderr, "free of %p, which this allocator never handed out\n", params->ptr);
    failures += 1;
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };

static ta_allocator allocator_of(counting_allocator *heap) {
    return (ta_allocator){ .self = heap, .v = &counting_vtable };
}

/* ------------------------------------------------------------------------
 * Helpers.
 */

#define KIND 0x0201u

static ta_ring_params params_of(counting_allocator *heap, uint32_t capacity, uint32_t max_record,
                                ta_ring_mode mode) {
    return (ta_ring_params){
        .allocator = allocator_of(heap), .capacity = capacity, .max_record_bytes = max_record, .mode = mode
    };
}

static ta_ring new_ring(counting_allocator *heap, uint32_t capacity, uint32_t max_record, ta_ring_mode mode) {
    ta_ring ring;
    ta_ring_params params = params_of(heap, capacity, max_record, mode);
    CHECK(ta_ring_init(&ring, &params) == TA_OK);
    return ring;
}

/* An empty ring whose counters stand at `counter`, as after that many
 * bytes were written and released. */
static void start_at(ta_ring *ring, uint64_t counter) {
    CHECK(ring->write == ring->tail);
    ring->head = counter;
    ring->tail = counter;
    ring->write = counter;
}

static uint32_t offset_of(const ta_ring *ring, const unsigned char *bytes) {
    return (uint32_t)(bytes - ring->bytes);
}

static uint8_t payload_byte(uint64_t seq, uint32_t index) { return (uint8_t)(seq * 7u + index * 13u + 1u); }

/* Fills a reserved record: its kind and a payload derived from its seq. */
static void fill(const ta_ring_record *record) {
    uint16_t kind = KIND;
    memcpy(record->bytes, &kind, sizeof(kind));
    for (uint32_t index = TA_RING_RECORD_MIN_BYTES; index < record->size; index += 1u) {
        record->bytes[index] = payload_byte(record->seq, index);
    }
}

static bool filled(const ta_ring_record *record, uint64_t seq, uint32_t size) {
    if (record->seq != seq || record->size != size || record->kind != KIND) {
        return false;
    }
    uint64_t stamped;
    uint32_t stamped_size;
    memcpy(&stamped, record->bytes + 8, sizeof(stamped));
    memcpy(&stamped_size, record->bytes + 4, sizeof(stamped_size));
    if (stamped != seq || stamped_size != size) {
        return false;
    }
    for (uint32_t index = TA_RING_RECORD_MIN_BYTES; index < size; index += 1u) {
        if (record->bytes[index] != payload_byte(seq, index)) {
            return false;
        }
    }
    return true;
}

/* Reserves, fills and commits one record. */
static ta_result put(ta_ring *ring, uint32_t size, ta_ring_record *out) {
    ta_ring_record record;
    ta_result result = ta_ring_reserve(ring, size, &record);
    if (result == TA_OK) {
        fill(&record);
        ta_ring_commit(ring);
        if (out != NULL) {
            *out = record;
        }
    }
    return result;
}

/* Peeks, checks and releases the next record. */
static bool takes(ta_ring *ring, uint64_t seq, uint32_t size) {
    ta_ring_record record;
    if (ta_ring_peek(ring, &record) != TA_OK || !filled(&record, seq, size)) {
        return false;
    }
    return ta_ring_release(ring) == TA_OK && ring->read_seq == seq;
}

static void read_prefix(const unsigned char *at, uint16_t *kind, uint16_t *flags, uint32_t *size) {
    memcpy(kind, at, sizeof(*kind));
    memcpy(flags, at + 2, sizeof(*flags));
    memcpy(size, at + 4, sizeof(*size));
}

static void write_u32(unsigned char *at, uint32_t value) { memcpy(at, &value, sizeof(value)); }
static void write_u64(unsigned char *at, uint64_t value) { memcpy(at, &value, sizeof(value)); }

/* A copy of the ring and its buffer, to show that a call changed nothing. */
typedef struct {
    ta_ring ring;
    unsigned char bytes[1024];
} snapshot;

static void take_snapshot(const ta_ring *ring, snapshot *shot) {
    memcpy(&shot->ring, ring, sizeof(*ring));
    memcpy(shot->bytes, ring->bytes, ring->capacity < sizeof(shot->bytes) ? ring->capacity : sizeof(shot->bytes));
}

static bool unchanged(const ta_ring *ring, const snapshot *shot) {
    size_t size = ring->capacity < sizeof(shot->bytes) ? ring->capacity : sizeof(shot->bytes);
    return memcmp(ring, &shot->ring, sizeof(*ring)) == 0 && memcmp(ring->bytes, shot->bytes, size) == 0;
}

static bool counters_hold(const ta_ring *ring) {
    return ring->tail <= ring->head && ring->head <= ring->write &&
           ring->write - ring->tail <= ring->capacity && ring->head % 8u == 0 && ring->tail % 8u == 0 &&
           ring->write % 8u == 0;
}

/* ------------------------------------------------------------------------
 * The claims.
 */

/* 1 */
static void test_init(void) {
    counting_allocator heap = {0};
    ta_ring_params good = params_of(&heap, 128, 64, TA_RING_LOSSLESS);
    ta_ring_params bad[] = { good, good, good, good, good, good };
    bad[0].mode = (ta_ring_mode)2;
    bad[1].max_record_bytes = 60;
    bad[2].max_record_bytes = 8;
    bad[3].capacity = 124;
    bad[4].capacity = 56;
    bad[5].capacity = 0;
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(bad); index += 1u) {
        ta_ring ring;
        memset(&ring, 0x5A, sizeof(ring));
        ta_ring before;
        memcpy(&before, &ring, sizeof(ring));
        CHECK(ta_ring_init(&ring, &bad[index]) == TA_ERR_INVALID);
        CHECK(memcmp(&ring, &before, sizeof(ring)) == 0);
    }
    CHECK(heap.attempts == 0);

    heap.fail_from = 1;
    ta_ring ring;
    memset(&ring, 0x5A, sizeof(ring));
    ta_ring before;
    memcpy(&before, &ring, sizeof(ring));
    CHECK(ta_ring_init(&ring, &good) == TA_ERR_NOMEM);
    CHECK(memcmp(&ring, &before, sizeof(ring)) == 0 && heap.live == 0);
    heap.fail_from = 0;

    heap.attempts = 0;
    CHECK(ta_ring_init(&ring, &good) == TA_OK);
    CHECK(heap.attempts == 1 && heap.live == 1 && heap.last_size == 128 && heap.last_alignment == 8);
    CHECK((uintptr_t)ring.bytes % 8u == 0);
    CHECK(ta_ring_capacity(&ring) == 128 && ta_ring_used(&ring) == 0 && ta_ring_available(&ring) == 128);
    CHECK(ring.next_seq == 1 && ring.read_seq == 0 && ring.dropped == 0);
    ta_ring_record record = { 0 };
    CHECK(ta_ring_peek(&ring, &record) == TA_ERR_NOT_FOUND && record.bytes == NULL);
    CHECK(ta_ring_release(&ring) == TA_ERR_NOT_FOUND);
    ta_ring_drops drops;
    CHECK(ta_ring_take_drops(&ring, &drops) == TA_ERR_NOT_FOUND);

    /* The smallest valid ring: one record of 16 bytes. */
    ta_ring small = new_ring(&heap, 16, 16, TA_RING_LOSSY);
    ta_ring_destroy(&small);
    ta_ring_destroy(&ring);
    CHECK(heap.live == 0);
}

/* 2 */
static void test_reserve_commit_read(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 256, 64, TA_RING_LOSSLESS);

    ta_ring_record first;
    CHECK(ta_ring_reserve(&ring, 17, &first) == TA_OK);
    CHECK(first.size == 24 && first.seq == 1 && first.kind == 0 && first.bytes == ring.bytes);
    uint16_t kind;
    uint16_t flags;
    uint32_t size;
    read_prefix(first.bytes, &kind, &flags, &size);
    CHECK(kind == 0 && flags == 0 && size == 24);
    uint64_t seq;
    memcpy(&seq, first.bytes + 8, sizeof(seq));
    CHECK(seq == 1);
    fill(&first);

    /* Not visible before commit. */
    ta_ring_record seen;
    CHECK(ta_ring_peek(&ring, &seen) == TA_ERR_NOT_FOUND);
    CHECK(ta_ring_used(&ring) == 0 && ta_ring_available(&ring) == 256 - 24);

    ta_ring_record second;
    ta_ring_record third;
    CHECK(ta_ring_reserve(&ring, 16, &second) == TA_OK);
    CHECK(ta_ring_reserve(&ring, 64, &third) == TA_OK);
    CHECK(second.seq == 2 && third.seq == 3 && second.size == 16 && third.size == 64);
    CHECK(offset_of(&ring, second.bytes) == 24 && offset_of(&ring, third.bytes) == 40);
    fill(&second);
    fill(&third);
    ta_ring_commit(&ring);
    CHECK(ta_ring_used(&ring) == 104 && ring.head == ring.write);

    /* Read in place and in order. */
    CHECK(ta_ring_peek(&ring, &seen) == TA_OK && seen.bytes == first.bytes && filled(&seen, 1, 24));
    CHECK(ta_ring_peek(&ring, &seen) == TA_OK && seen.seq == 1);
    CHECK(takes(&ring, 1, 24));
    CHECK(takes(&ring, 2, 16));
    CHECK(takes(&ring, 3, 64));
    CHECK(ta_ring_peek(&ring, &seen) == TA_ERR_NOT_FOUND);
    CHECK(ring.next_seq == 4 && ring.read_seq == 3);
    ta_ring_destroy(&ring);
}

/* 3 */
static void test_reserve_size_bounds(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 128, 64, TA_RING_LOSSY);
    CHECK(put(&ring, 16, NULL) == TA_OK);
    uint32_t sizes[] = { 0, 1, 8, 15, 65, 72, UINT32_MAX };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(sizes); index += 1u) {
        snapshot shot;
        take_snapshot(&ring, &shot);
        ta_ring_record record = { 0 };
        CHECK(ta_ring_reserve(&ring, sizes[index], &record) == TA_ERR_INVALID);
        CHECK(unchanged(&ring, &shot) && record.bytes == NULL);
    }
    CHECK(put(&ring, 57, NULL) == TA_OK); /* rounds to 64, the largest */
    ta_ring_destroy(&ring);
}

/* 4 */
static void test_wrap_at_every_offset(void) {
    counting_allocator heap = {0};
    const uint32_t capacity = 128;
    uint64_t bases[] = { 0, (uint64_t)1 << 40 };
    for (uint32_t base = 0; base < TA_ARRAY_COUNT(bases); base += 1u) {
        for (uint32_t start = 0; start < capacity; start += 8u) {
            for (uint32_t size = 16; size <= 64; size += 8u) {
                ta_ring ring = new_ring(&heap, capacity, 64, TA_RING_LOSSLESS);
                start_at(&ring, bases[base] + start);
                uint64_t seq = 1;
                /* Where a record, a pad and another record fit, keep a
                 * second record in the ring, so each reserve sees another
                 * one beside it; go through two full turns. */
                bool companion = 3u * size - 8u <= capacity;
                if (companion) {
                    CHECK(put(&ring, size, NULL) == TA_OK);
                }
                for (uint32_t step = 0; step < 2u * capacity / size + 2u; step += 1u) {
                    uint32_t at = (uint32_t)(ring.write % capacity);
                    ta_ring_record record;
                    CHECK(put(&ring, size, &record) == TA_OK);
                    uint32_t where = offset_of(&ring, record.bytes);
                    CHECK(where + record.size <= capacity);
                    if (size > capacity - at) {
                        uint16_t kind;
                        uint16_t flags;
                        uint32_t pad;
                        read_prefix(ring.bytes + at, &kind, &flags, &pad);
                        CHECK(where == 0 && kind == 0 && flags == 0 && pad == capacity - at);
                    } else {
                        CHECK(where == at);
                    }
                    CHECK(takes(&ring, seq, size));
                    seq += 1u;
                    CHECK(counters_hold(&ring));
                }
                if (companion) {
                    CHECK(takes(&ring, seq, size));
                }
                ta_ring_record none;
                CHECK(ta_ring_peek(&ring, &none) == TA_ERR_NOT_FOUND);
                CHECK(ring.head == ring.tail);
                ta_ring_destroy(&ring);
            }
        }
    }
    CHECK(heap.live == 0);
}

/* 5 */
static void test_lossless_full(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 128, 64, TA_RING_LOSSLESS);
    CHECK(put(&ring, 64, NULL) == TA_OK);
    CHECK(put(&ring, 48, NULL) == TA_OK);
    /* 16 bytes left; a record of 24 does not fit. */
    snapshot shot;
    take_snapshot(&ring, &shot);
    ta_ring_record record = { 0 };
    CHECK(ta_ring_reserve(&ring, 24, &record) == TA_ERR_FULL);
    CHECK(unchanged(&ring, &shot) && record.bytes == NULL);
    CHECK(put(&ring, 16, NULL) == TA_OK);
    CHECK(ta_ring_available(&ring) == 0);
    take_snapshot(&ring, &shot);
    CHECK(ta_ring_reserve(&ring, 16, &record) == TA_ERR_FULL);
    CHECK(unchanged(&ring, &shot));
    /* A release makes room; the record goes at offset 0. */
    CHECK(takes(&ring, 1, 64));
    CHECK(put(&ring, 64, &record) == TA_OK && record.seq == 4 && record.bytes == ring.bytes);
    CHECK(takes(&ring, 2, 48));
    CHECK(takes(&ring, 3, 16));
    CHECK(takes(&ring, 4, 64));
    ta_ring_destroy(&ring);
}

/* 6 */
static void test_ring_of_one_record(void) {
    counting_allocator heap = {0};
    for (uint32_t mode = 0; mode < 2u; mode += 1u) {
        for (uint32_t start = 0; start < 64; start += 8u) {
            ta_ring ring = new_ring(&heap, 64, 64, (ta_ring_mode)mode);
            start_at(&ring, 1000u * 64u + start);
            ta_ring_record record;
            if (start == 0) {
                CHECK(put(&ring, 64, &record) == TA_OK && record.bytes == ring.bytes);
            } else {
                CHECK(ta_ring_reserve(&ring, 64, &record) == TA_ERR_FULL);
                /* The pad alone is committed; on a lossy ring the record
                 * is dropped, and only then does it take a seq. */
                CHECK(ta_ring_used(&ring) == 64 - start && ring.head == ring.write);
                uint16_t kind;
                uint16_t flags;
                uint32_t pad;
                read_prefix(ring.bytes + start, &kind, &flags, &pad);
                CHECK(kind == 0 && pad == 64 - start);
                CHECK(ring.next_seq == (mode == TA_RING_LOSSY ? 2u : 1u));
                CHECK(ring.dropped == (mode == TA_RING_LOSSY ? 1u : 0u));
                CHECK(ta_ring_peek(&ring, &record) == TA_ERR_NOT_FOUND);
                CHECK(ta_ring_used(&ring) == 0);
                CHECK(put(&ring, 64, &record) == TA_OK && record.bytes == ring.bytes);
            }
            uint64_t seq = record.seq;
            CHECK(takes(&ring, seq, 64));
            /* Every later record of the largest size fits. */
            for (uint32_t step = 0; step < 4u; step += 1u) {
                CHECK(put(&ring, 64, NULL) == TA_OK);
                CHECK(takes(&ring, seq + 1u + step, 64));
            }
            ta_ring_destroy(&ring);
        }
    }

    /* With a reservation open, the pad alone would publish it: nothing is
     * written. */
    ta_ring ring = new_ring(&heap, 64, 64, TA_RING_LOSSLESS);
    start_at(&ring, 40);
    ta_ring_record open;
    CHECK(ta_ring_reserve(&ring, 16, &open) == TA_OK);
    snapshot shot;
    take_snapshot(&ring, &shot);
    ta_ring_record record;
    CHECK(ta_ring_reserve(&ring, 64, &record) == TA_ERR_FULL);
    CHECK(unchanged(&ring, &shot));
    ta_ring_destroy(&ring);
    CHECK(heap.live == 0);
}

/* 7 */
static void test_lossy(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 64, 32, TA_RING_LOSSY);
    CHECK(put(&ring, 32, NULL) == TA_OK);
    CHECK(put(&ring, 32, NULL) == TA_OK);
    ta_ring_record record;
    CHECK(ta_ring_reserve(&ring, 16, &record) == TA_ERR_FULL);
    CHECK(ta_ring_reserve(&ring, 32, &record) == TA_ERR_FULL);
    CHECK(ring.dropped == 2 && ring.next_seq == 5 && ta_ring_used(&ring) == 64);

    ta_ring_drops drops;
    CHECK(ta_ring_take_drops(&ring, &drops) == TA_OK && drops.count == 2 && drops.first_seq == 3);
    CHECK(ta_ring_take_drops(&ring, &drops) == TA_ERR_NOT_FOUND);
    CHECK(ring.dropped == 2);

    CHECK(takes(&ring, 1, 32));
    CHECK(put(&ring, 24, &record) == TA_OK && record.seq == 5);
    CHECK(ta_ring_reserve(&ring, 16, &record) == TA_ERR_FULL);
    CHECK(ta_ring_take_drops(&ring, &drops) == TA_OK && drops.count == 1 && drops.first_seq == 6);
    CHECK(ring.dropped == 3);
    /* The reader sees seq jump from 2 to 5 and accepts it. */
    CHECK(takes(&ring, 2, 32));
    CHECK(takes(&ring, 5, 24));
    CHECK(put(&ring, 16, &record) == TA_OK && record.seq == 7);
    CHECK(takes(&ring, 7, 16));
    ta_ring_destroy(&ring);
}

/* 8: the record at the tail is tampered with in each way, and peek and
 * release refuse it without changing the ring. */
static void expect_corrupt(ta_ring *ring) {
    snapshot shot;
    take_snapshot(ring, &shot);
    ta_ring_record record = { 0 };
    CHECK(ta_ring_peek(ring, &record) == TA_ERR_CORRUPT);
    CHECK(record.bytes == NULL);
    CHECK(ta_ring_release(ring) == TA_ERR_CORRUPT);
    CHECK(unchanged(ring, &shot));
}

static void test_corrupt(void) {
    counting_allocator heap = {0};
    for (uint32_t mode = 0; mode < 2u; mode += 1u) {
        /* Two records of 64 in a ring of 256 with max 64; the first is
         * tampered with. */
        uint32_t sizes[] = { 12, 0, 4, 8, 72, 136, 200 };
        for (uint32_t index = 0; index < TA_ARRAY_COUNT(sizes); index += 1u) {
            ta_ring ring = new_ring(&heap, 256, 64, (ta_ring_mode)mode);
            ta_ring_record first;
            CHECK(put(&ring, 64, &first) == TA_OK);
            CHECK(put(&ring, 64, NULL) == TA_OK);
            write_u32(first.bytes + 4, sizes[index]);
            expect_corrupt(&ring);
            ta_ring_destroy(&ring);
        }
        /* A pad of 4 bytes, or of a size that is not a multiple of 8. */
        uint32_t pads[] = { 0, 4, 12 };
        for (uint32_t index = 0; index < TA_ARRAY_COUNT(pads); index += 1u) {
            ta_ring ring = new_ring(&heap, 256, 64, (ta_ring_mode)mode);
            ta_ring_record first;
            CHECK(put(&ring, 64, &first) == TA_OK);
            memset(first.bytes, 0, 4);
            write_u32(first.bytes + 4, pads[index]);
            expect_corrupt(&ring);
            ta_ring_destroy(&ring);
        }
        /* A well-formed pad that ends before the end of the buffer. */
        uint32_t short_pads[] = { 8, 64, 128 };
        for (uint32_t index = 0; index < TA_ARRAY_COUNT(short_pads); index += 1u) {
            ta_ring ring = new_ring(&heap, 256, 64, (ta_ring_mode)mode);
            ta_ring_record first;
            CHECK(put(&ring, 64, &first) == TA_OK);
            CHECK(put(&ring, 64, NULL) == TA_OK);
            memset(first.bytes, 0, 4);
            write_u32(first.bytes + 4, short_pads[index]);
            expect_corrupt(&ring);
            ta_ring_destroy(&ring);
        }
        /* A record that would run past the end of the buffer, though the
         * committed bytes hold that many. */
        ta_ring ring = new_ring(&heap, 128, 64, (ta_ring_mode)mode);
        start_at(&ring, 112);
        ta_ring_record first;
        CHECK(put(&ring, 16, &first) == TA_OK && offset_of(&ring, first.bytes) == 112);
        CHECK(put(&ring, 16, NULL) == TA_OK);
        CHECK(ta_ring_used(&ring) == 32);
        write_u32(first.bytes + 4, 32);
        expect_corrupt(&ring);
        ta_ring_destroy(&ring);
    }

    /* Lossless: a gap or a repeat. */
    uint64_t seqs[] = { 0, 2, 3, UINT64_MAX };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(seqs); index += 1u) {
        ta_ring ring = new_ring(&heap, 128, 32, TA_RING_LOSSLESS);
        ta_ring_record first;
        CHECK(put(&ring, 16, &first) == TA_OK);
        write_u64(first.bytes + 8, seqs[index]);
        expect_corrupt(&ring);
        ta_ring_destroy(&ring);
    }
    {
        ta_ring ring = new_ring(&heap, 128, 32, TA_RING_LOSSLESS);
        ta_ring_record second;
        CHECK(put(&ring, 16, NULL) == TA_OK);
        CHECK(put(&ring, 16, &second) == TA_OK);
        CHECK(takes(&ring, 1, 16));
        write_u64(second.bytes + 8, 1);
        expect_corrupt(&ring);
        write_u64(second.bytes + 8, 3);
        expect_corrupt(&ring);
        ta_ring_destroy(&ring);
    }
    /* Lossy: a seq that does not grow. */
    {
        ta_ring ring = new_ring(&heap, 128, 32, TA_RING_LOSSY);
        ta_ring_record second;
        CHECK(put(&ring, 16, NULL) == TA_OK);
        CHECK(put(&ring, 16, &second) == TA_OK);
        CHECK(takes(&ring, 1, 16));
        write_u64(second.bytes + 8, 1);
        expect_corrupt(&ring);
        write_u64(second.bytes + 8, 0);
        expect_corrupt(&ring);
        write_u64(second.bytes + 8, 9);
        ta_ring_record record;
        CHECK(ta_ring_peek(&ring, &record) == TA_OK && record.seq == 9);
        CHECK(ta_ring_release(&ring) == TA_OK && ring.read_seq == 9);
        ta_ring_destroy(&ring);
    }
    CHECK(heap.live == 0);
}

/* 9 */
static void test_release(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 128, 64, TA_RING_LOSSLESS);
    CHECK(ta_ring_release(&ring) == TA_ERR_NOT_FOUND);
    start_at(&ring, 96);
    CHECK(put(&ring, 24, NULL) == TA_OK); /* 96..120 */
    CHECK(put(&ring, 24, NULL) == TA_OK); /* pad 120..128, then 0..24 */
    CHECK(put(&ring, 16, NULL) == TA_OK); /* 24..40 */
    CHECK(ta_ring_used(&ring) == 72);
    CHECK(ta_ring_release(&ring) == TA_OK && ring.tail == 120 && ta_ring_used(&ring) == 48);
    /* The second release takes the pad and the record behind it. */
    CHECK(ta_ring_release(&ring) == TA_OK && ring.tail == 152 && ta_ring_used(&ring) == 16);
    CHECK(ring.read_seq == 2);
    CHECK(ta_ring_release(&ring) == TA_OK && ta_ring_used(&ring) == 0 && ring.read_seq == 3);
    CHECK(ta_ring_release(&ring) == TA_ERR_NOT_FOUND);
    ta_ring_destroy(&ring);
}

/* 10 */
static void test_never_allocates(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 64, 32, TA_RING_LOSSY);
    uint32_t attempts = heap.attempts;
    ta_ring_record record;
    ta_ring_drops drops;
    for (uint32_t step = 0; step < 50u; step += 1u) {
        (void)put(&ring, 16u + (step % 3u) * 8u, NULL);
        if (step % 3u == 0) {
            (void)ta_ring_peek(&ring, &record);
            (void)ta_ring_release(&ring);
        }
        (void)ta_ring_take_drops(&ring, &drops);
        (void)ta_ring_used(&ring);
        (void)ta_ring_available(&ring);
        (void)ta_ring_capacity(&ring);
    }
    CHECK(ring.dropped != 0);
    CHECK(heap.attempts == attempts);
    ta_ring_destroy(&ring);
}

/* 11 */
static void test_destroy(void) {
    counting_allocator heap = {0};
    ta_ring ring = new_ring(&heap, 64, 32, TA_RING_LOSSLESS);
    CHECK(put(&ring, 16, NULL) == TA_OK);
    CHECK(heap.live == 1);
    ta_ring_destroy(&ring);
    CHECK(heap.live == 0 && ring.bytes == NULL && ring.capacity == 0);
}

/* 12 */
static uint64_t random_state = 0x9E3779B97F4A7C15u;

static uint32_t random_below(uint32_t bound) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (uint32_t)(random_state % bound);
}

#define MODEL_MAX 256u

typedef struct {
    uint64_t seq;
    uint32_t size;
} model_record;

static void test_random(ta_ring_mode mode) {
    counting_allocator heap = {0};
    const uint32_t max_record = 64;
    ta_ring ring = new_ring(&heap, 128, max_record, mode);
    /* Committed and not released, then reserved and not committed. */
    model_record model[MODEL_MAX];
    uint32_t committed = 0;
    uint32_t length = 0;
    ta_ring_record open[MODEL_MAX];
    uint64_t next_seq = 1;
    uint64_t dropped = 0;
    uint64_t unreported = 0;
    for (uint32_t step = 0; step < 20000u; step += 1u) {
        uint32_t choice = random_below(100);
        if (choice < 40) {
            uint32_t size = random_below(max_record + 16u);
            ta_ring_record record;
            bool empty_before = ring.write == ring.tail;
            uint32_t at = (uint32_t)(ring.write % ring.capacity);
            uint32_t aligned = (size + 7u) & ~7u;
            uint32_t need = aligned + (aligned > ring.capacity - at ? ring.capacity - at : 0u);
            uint32_t available = ta_ring_available(&ring);
            ta_result result = ta_ring_reserve(&ring, size, &record);
            if (size < 16 || size > max_record) {
                CHECK(result == TA_ERR_INVALID);
            } else if (result == TA_OK) {
                CHECK(record.seq == next_seq && record.size == aligned && need <= available);
                CHECK(offset_of(&ring, record.bytes) + record.size <= ring.capacity);
                fill(&record);
                record.kind = KIND;
                open[length - committed] = record;
                model[length++] = (model_record){ record.seq, record.size };
                next_seq += 1u;
            } else {
                CHECK(result == TA_ERR_FULL);
                /* Full only when the record and its pad exceed the room. */
                CHECK(need > available);
                if (mode == TA_RING_LOSSY) {
                    if (unreported == 0) {
                        CHECK(ring.unreported.first_seq == next_seq);
                    }
                    next_seq += 1u;
                    dropped += 1u;
                    unreported += 1u;
                }
                if (empty_before) {
                    /* An empty ring refuses only with the pad alone
                     * committed; after it is skipped the record fits. */
                    CHECK(ring.head == ring.write && ring.head != ring.tail);
                }
            }
        } else if (choice < 55) {
            ta_ring_commit(&ring);
            committed = length;
        } else if (choice < 85) {
            ta_ring_record record;
            if (random_below(2) == 0) {
                ta_result result = ta_ring_peek(&ring, &record);
                if (committed == 0) {
                    CHECK(result == TA_ERR_NOT_FOUND);
                } else {
                    CHECK(result == TA_OK && filled(&record, model[0].seq, model[0].size));
                }
            } else if (committed == 0) {
                CHECK(ta_ring_release(&ring) == TA_ERR_NOT_FOUND);
            } else {
                CHECK(takes(&ring, model[0].seq, model[0].size));
                memmove(model, model + 1, (length - 1u) * sizeof(model[0]));
                length -= 1u;
                committed -= 1u;
            }
        } else {
            ta_ring_drops drops;
            ta_result result = ta_ring_take_drops(&ring, &drops);
            if (unreported == 0) {
                CHECK(result == TA_ERR_NOT_FOUND);
            } else {
                CHECK(result == TA_OK && drops.count == unreported);
                unreported = 0;
            }
        }
        CHECK(counters_hold(&ring));
        CHECK(ring.next_seq == next_seq && ring.dropped == dropped);
        CHECK(heap.live == 1);
        /* Reserved records the producer has not committed keep their
         * bytes. */
        for (uint32_t index = 0; index < length - committed; index += 1u) {
            CHECK(filled(&open[index], open[index].seq, open[index].size));
        }
        if (failures > 20) {
            break;
        }
    }
    ta_ring_commit(&ring);
    for (uint32_t index = 0; index < length; index += 1u) {
        CHECK(takes(&ring, model[index].seq, model[index].size));
    }
    ta_ring_record none;
    CHECK(ta_ring_peek(&ring, &none) == TA_ERR_NOT_FOUND);
    ta_ring_destroy(&ring);
    CHECK(heap.live == 0);
}

int main(void) {
    test_init();
    test_reserve_commit_read();
    test_reserve_size_bounds();
    test_wrap_at_every_offset();
    test_lossless_full();
    test_ring_of_one_record();
    test_lossy();
    test_corrupt();
    test_release();
    test_never_allocates();
    test_destroy();
    test_random(TA_RING_LOSSLESS);
    test_random(TA_RING_LOSSY);
    if (failures != 0) {
        fprintf(stderr, "ring_test: %d checks failed\n", failures);
        return 1;
    }
    printf("ring_test: ok\n");
    return 0;
}
