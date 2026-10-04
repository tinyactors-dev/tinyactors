/* test: standalone asan */
/*
 * Block C1 (src/ring.h): the SPSC byte ring and the control block.
 *
 * Covers the layout and init (validation, failure leaves memory untouched,
 * the control block's contents), the allocation rules (the ring itself never
 * allocates; the one region is allocated by the test and freed with its
 * sized free), the producer, the consumer, the gap-free seq, lossy drops and
 * a seeded model test. Fast, not exhaustive; see the note at the model test.
 *
 * Includes the amalgamation, because the unit is private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

static ta_test_allocator heap = { .strict = true };

#define CANARY 0xA5u

typedef struct region {
    ta_rings_plan plan;
    unsigned char *memory;
} region;

static ta_rings_specs specs_of(uint32_t capacity, uint32_t max, ta_ring_mode mode) {
    ta_ring_spec spec = { capacity, max, mode };
    return (ta_rings_specs){ .ring = { spec, spec, spec } };
}

/* The test's one allocation: what the system will do at creation. */
static bool region_make(region *r, const ta_rings_specs *specs) {
    if (ta_rings_layout(specs, &r->plan) != TA_OK) return false;
    r->memory = ta_mem_alloc(ta_test_allocator_get(&heap), r->plan.bytes, r->plan.alignment);
    return r->memory != NULL;
}

static void region_free(region *r) {
    ta_mem_free(ta_test_allocator_get(&heap), r->memory,
                &(ta_allocator_free_params){ .ptr = r->memory, .size = r->plan.bytes, .alignment = r->plan.alignment });
    r->memory = NULL;
}

static void finish_clean(void) {
    CHECK(ta_test_allocator_finish(&heap) == 0);
    CHECK(heap.stats.bad_frees == 0);
}

static ta_result rings_up(ta_rings *rings, region *r, const ta_rings_specs *specs) {
    return ta_rings_init(rings, &(ta_rings_init_params){ .memory = r->memory, .memory_bytes = r->plan.bytes, .specs = *specs });
}

static bool is_zero(const void *p, size_t n) {
    const unsigned char *b = p;
    for (size_t i = 0; i < n; i += 1) if (b[i] != 0) return false;
    return true;
}

/* ---- layout and init ---- */

static void test_layout(void) {
    ta_rings_specs specs = specs_of(64, 32, TA_RING_LOSSLESS);
    specs.ring[1].capacity = 96;
    specs.ring[2] = (ta_ring_spec){ 200, 16, TA_RING_LOSSY };
    ta_rings_plan plan;
    CHECK(ta_rings_layout(&specs, &plan) == TA_OK);
    CHECK(plan.alignment == 8);
    CHECK(plan.data_offset[0] >= sizeof(ta_ring_control));
    CHECK(plan.data_offset[0] % 8 == 0 && plan.data_offset[1] % 8 == 0 && plan.data_offset[2] % 8 == 0);
    CHECK(plan.data_offset[1] == plan.data_offset[0] + 64);
    CHECK(plan.data_offset[2] == plan.data_offset[1] + 96);
    CHECK(plan.bytes == plan.data_offset[2] + 200);

    /* Bad specs answer INVALID and leave *plan alone. */
    ta_rings_plan untouched = { 1, 2, { 3, 4, 5 } };
    ta_ring_spec bad[] = {
        { 60, 32, TA_RING_LOSSLESS },  /* capacity not a multiple of 8 */
        { 64, 36, TA_RING_LOSSLESS },  /* max not a multiple of 8 */
        { 64, 72, TA_RING_LOSSLESS },  /* max above capacity */
        { 64, 8, TA_RING_LOSSLESS },   /* max below the minimum record */
        { 64, 0, TA_RING_LOSSLESS },
        { 0, 0, TA_RING_LOSSLESS },
        { 64, 32, (ta_ring_mode)7 },
    };
    for (uint32_t i = 0; i < TA_ARRAY_COUNT(bad); i += 1) {
        for (uint32_t r = 0; r < TA_RING_COUNT; r += 1) {
            ta_rings_specs s = specs_of(64, 32, TA_RING_LOSSLESS);
            s.ring[r] = bad[i];
            ta_rings_plan p = untouched;
            CHECK(ta_rings_layout(&s, &p) == TA_ERR_INVALID);
            CHECK(p.bytes == 1 && p.alignment == 2 && p.data_offset[2] == 5);
        }
    }

    /* The largest u32 capacities are a plan, not an allocation: the layout only adds. */
    ta_rings_specs huge = specs_of(0xFFFFFFF8u, 0xFFFFFFF8u, TA_RING_LOSSY);
    ta_rings_plan hp;
    ta_result result = ta_rings_layout(&huge, &hp);
    CHECK(result == TA_OK || result == TA_ERR_NOMEM); /* NOMEM only where size_t is 32 bits */
    if (result == TA_OK) CHECK(hp.bytes > 3ull * 0xFFFFFFF8u);
}

static void test_init(void) {
    ta_rings_specs specs = specs_of(64, 32, TA_RING_LOSSLESS);
    specs.ring[TA_RING_TEL] = (ta_ring_spec){ 128, 48, TA_RING_LOSSY };
    region r;
    ta_test_allocator_reset_counts(&heap);
    CHECK(region_make(&r, &specs));
    CHECK(heap.stats.allocations == 1);
    memset(r.memory, CANARY, r.plan.bytes);

    ta_rings rings;
    uint64_t attempts = heap.stats.attempts;
    CHECK(rings_up(&rings, &r, &specs) == TA_OK);
    CHECK(heap.stats.attempts == attempts); /* init allocates nothing */
    const ta_ring_control *c = rings.control;
    CHECK((void *)c == (void *)r.memory);
    CHECK(c->magic == TA_RING_MAGIC && c->version == TA_RING_VERSION && c->ring_count == TA_RING_COUNT);
    for (uint32_t i = 0; i < TA_RING_COUNT; i += 1) {
        CHECK(c->ring[i].capacity == specs.ring[i].capacity);
        CHECK(c->ring[i].offset == r.plan.data_offset[i]);
        CHECK(c->ring[i].max_record_bytes == specs.ring[i].max_record_bytes);
        CHECK(c->ring[i].mode == (uint32_t)specs.ring[i].mode);
        CHECK(c->ring[i].head == 0 && c->ring[i].tail == 0 && c->ring[i].dropped == 0 && c->ring[i].next_seq == 1);
        CHECK(rings.ring[i].data == r.memory + c->ring[i].offset);
    }
    /* Data areas are not scrubbed: init writes the control block only. */
    CHECK(r.memory[r.plan.data_offset[0]] == CANARY && r.memory[r.plan.bytes - 1] == CANARY);

    /* Failures leave the handles zeroed and the memory untouched. */
    unsigned char *copy = ta_mem_alloc(ta_test_allocator_get(&heap), r.plan.bytes, 8);
    memset(r.memory, CANARY, r.plan.bytes);
    memcpy(copy, r.memory, r.plan.bytes);
    ta_rings_init_params p = { .memory = r.memory, .memory_bytes = r.plan.bytes, .specs = specs };
    p.memory_bytes = r.plan.bytes - 1;
    CHECK(ta_rings_init(&rings, &p) == TA_ERR_INVALID && is_zero(&rings, sizeof rings));
    p.memory_bytes = r.plan.bytes;
    p.memory = r.memory + 1;
    CHECK(ta_rings_init(&rings, &p) == TA_ERR_INVALID && is_zero(&rings, sizeof rings));
    p.memory = NULL;
    CHECK(ta_rings_init(&rings, &p) == TA_ERR_INVALID && is_zero(&rings, sizeof rings));
    p.memory = r.memory;
    p.specs.ring[1].capacity = 63;
    CHECK(ta_rings_init(&rings, &p) == TA_ERR_INVALID && is_zero(&rings, sizeof rings));
    CHECK(memcmp(copy, r.memory, r.plan.bytes) == 0);
    ta_mem_free(ta_test_allocator_get(&heap), copy,
                &(ta_allocator_free_params){ .ptr = copy, .size = r.plan.bytes, .alignment = 8 });

    region_free(&r);
    finish_clean();
}

/* The region is the only allocation: failing it ends as a clean failure with nothing held. */
static void test_region_failure_sweep(void) {
    ta_rings_specs specs = specs_of(64, 32, TA_RING_LOSSLESS);
    ta_test_allocator_reset_counts(&heap);
    ta_test_allocator_fail_nth(&heap, 1);
    region r;
    CHECK(!region_make(&r, &specs));
    ta_test_allocator_disarm(&heap);
    finish_clean();
}

/* ---- producer ---- */

/*
 * A fixture: one region, one set of rings, torn down with the sized free.
 * `at` puts one ring's counters at byte count `at` with nothing unreleased,
 * which is a state the ring can reach only by committing and releasing, but
 * not at every offset (records are at least 16 bytes), so the sweep sets it.
 */
typedef struct fixture {
    region r;
    ta_rings rings;
} fixture;

static void fixture_up(fixture *f, const ta_rings_specs *specs) {
    CHECK(region_make(&f->r, specs));
    CHECK(rings_up(&f->rings, &f->r, specs) == TA_OK);
    memset(f->r.memory + f->r.plan.data_offset[0], CANARY, f->r.plan.bytes - f->r.plan.data_offset[0]);
}

static void fixture_down(fixture *f) {
    region_free(&f->r);
    finish_clean();
}

static void ring_at(ta_ring *ring, uint64_t at) {
    ring->head = ring->peer_tail = ring->tail = ring->peer_head = at;
    ring->shared->head = ring->shared->tail = at;
}

/* What a host consumer does with a ring it drains: store the tail. */
static void host_release(ta_ring *ring, uint64_t bytes) { ring->shared->tail += bytes; }

static void fill_record(unsigned char *bytes, uint32_t size, uint32_t tag) {
    for (uint32_t i = 16; i < size; i += 1) bytes[i] = (unsigned char)(tag * 13u + i);
}

static bool record_ok(const unsigned char *bytes, uint32_t size, uint32_t tag) {
    for (uint32_t i = 16; i < size; i += 1) if (bytes[i] != (unsigned char)(tag * 13u + i)) return false;
    return true;
}

static uint32_t read32(const unsigned char *at) { uint32_t v; memcpy(&v, at, 4); return v; }
static uint64_t read64(const unsigned char *at) { uint64_t v; memcpy(&v, at, 8); return v; }

static void test_wrap_at_every_offset(void) {
    static const uint32_t capacities[] = { 64, 72, 256 };
    static const uint32_t sizes[] = { 16, 24, 32, 40, 56, 64 };
    for (uint32_t ci = 0; ci < TA_ARRAY_COUNT(capacities); ci += 1) {
        for (uint32_t si = 0; si < TA_ARRAY_COUNT(sizes); si += 1) {
            uint32_t capacity = capacities[ci], size = sizes[si];
            for (uint64_t off = 0; off < capacity; off += 8) {
                fixture f;
                ta_rings_specs specs = specs_of(capacity, size, TA_RING_LOSSLESS);
                fixture_up(&f, &specs);
                ta_ring *ring = &f.rings.ring[TA_RING_C2H];
                ring_at(ring, off);
                uint64_t pad = size > capacity - off ? capacity - off : 0;
                uint64_t before = ring->shared->head;
                uint64_t attempts = heap.stats.attempts;
                ta_ring_reservation res;
                ta_ring_status status = ta_ring_reserve(ring, size, &res);
                if (pad + size > capacity) {
                    /* An empty ring whose record cannot fit after its pad: the lone pad. */
                    CHECK(off != 0 && status == TA_RING_FULL && res.bytes == NULL);
                    CHECK(ring->shared->head == before + pad && ring->shared->next_seq == 1);
                    CHECK(read32(ring->data + off) == 0 && read32(ring->data + off + 4) == pad);
                    host_release(ring, pad);
                    status = ta_ring_reserve(ring, size, &res);
                    pad = 0;
                    before = ring->shared->head;
                }
                CHECK(status == TA_RING_OK && res.size == size && res.seq == 1);
                uint64_t start = (before + pad) % capacity;
                CHECK(res.bytes == ring->data + start && start + size <= capacity);
                CHECK(res.bytes - ring->data < (ptrdiff_t)capacity && (uintptr_t)res.bytes % 8 == 0);
                CHECK(ring->shared->head == before); /* reserve publishes nothing */
                fill_record(res.bytes, size, (uint32_t)off);
                ta_ring_commit(ring);
                CHECK(ring->shared->head == before + pad + size && ring->shared->next_seq == 2);
                if (pad != 0) {
                    CHECK(start == 0);
                    CHECK(read32(ring->data + (before % capacity)) == 0 && read32(ring->data + (before % capacity) + 4) == pad);
                }
                CHECK(read32(ring->data + start + 4) == size && read64(ring->data + start + 8) == 1);
                CHECK(record_ok(ring->data + start, size, (uint32_t)off));
                host_release(ring, pad + size);
                /* The next record continues where the last ended, wrapping to 0 exactly at the end. */
                uint64_t next = (before + pad + size) % capacity;
                CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_OK);
                CHECK(res.bytes == ring->data + (16 > capacity - next ? 0 : next) && res.seq == 2);
                CHECK(heap.stats.attempts == attempts); /* the ring never allocates */
                fixture_down(&f);
            }
        }
    }
}

static void test_exact_max_record(void) {
    static const uint32_t capacities[] = { 64, 4096 };
    for (uint32_t ci = 0; ci < TA_ARRAY_COUNT(capacities); ci += 1) {
        uint32_t capacity = capacities[ci];
        for (uint64_t off = 0; off < capacity; off += 8) {
            fixture f;
            ta_rings_specs specs = specs_of(capacity, capacity, TA_RING_LOSSLESS);
            fixture_up(&f, &specs);
            ta_ring *ring = &f.rings.ring[TA_RING_C2H];
            ring_at(ring, off);
            ta_ring_reservation res;
            if (off != 0) {
                /* The pad alone is committed; the consumer releases it; the retry succeeds. */
                CHECK(ta_ring_reserve(ring, capacity, &res) == TA_RING_FULL);
                CHECK(ring->shared->head - ring->shared->tail == capacity - off);
                CHECK(ring->shared->next_seq == 1); /* a pad takes no seq */
                CHECK(ta_ring_reserve(ring, capacity, &res) == TA_RING_FULL); /* still unreleased */
                CHECK(ring->shared->head - ring->shared->tail == capacity - off);
                host_release(ring, capacity - off);
            }
            CHECK(ta_ring_reserve(ring, capacity, &res) == TA_RING_OK);
            CHECK(res.bytes == ring->data && res.size == capacity && res.seq == 1);
            fill_record(res.bytes, capacity, 7);
            ta_ring_commit(ring);
            CHECK(ring->shared->head - ring->shared->tail == capacity);
            CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_FULL && res.seq == 2);
            fixture_down(&f);
        }
    }
}

static void test_sizes_busy_abandon(void) {
    for (int lossy = 0; lossy < 2; lossy += 1) {
        fixture f;
        ta_rings_specs specs = specs_of(64, 32, lossy ? TA_RING_LOSSY : TA_RING_LOSSLESS);
        fixture_up(&f, &specs);
        ta_ring *ring = &f.rings.ring[TA_RING_C2H];
        ta_ring_reservation res = { (unsigned char *)1, 99, 99 };
        static const uint32_t bad[] = { 0, 1, 8, 40, 64, 72, UINT32_MAX - 7, UINT32_MAX - 6, UINT32_MAX };
        unsigned char control[sizeof(ta_ring_control)];
        memcpy(control, f.rings.control, sizeof control);
        for (uint32_t i = 0; i < TA_ARRAY_COUNT(bad); i += 1) {
            CHECK(ta_ring_reserve(ring, bad[i], &res) == TA_RING_TOO_BIG);
            CHECK(res.bytes == (unsigned char *)1 && res.size == 99 && res.seq == 99); /* *out not written */
            CHECK(memcmp(control, f.rings.control, sizeof control) == 0);
            CHECK(ring->pending_size == 0);
        }
        /* Rounding: 12 and 17 round up to 16 and 24. */
        CHECK(ta_ring_reserve(ring, 12, &res) == TA_RING_OK && res.size == 16);
        ta_ring_abandon(ring);
        CHECK(ta_ring_reserve(ring, 17, &res) == TA_RING_OK && res.size == 24);
        unsigned char *first = res.bytes;
        /* A second reserve while one is open is BUSY and keeps the first. */
        ta_ring_reservation other = { 0 };
        CHECK(ta_ring_reserve(ring, 16, &other) == TA_RING_BUSY && other.bytes == NULL);
        CHECK(ring->pending_size == 24);
        /* Abandon leaves the ring as it was, and the next reserve is the same place and seq. */
        ta_ring_abandon(ring);
        CHECK(memcmp(control, f.rings.control, sizeof control) == 0);
        CHECK(ta_ring_reserve(ring, 24, &res) == TA_RING_OK && res.bytes == first && res.seq == 1);
        ta_ring_commit(ring);
        CHECK(ring->shared->next_seq == 2 && ring->shared->dropped == 0);
        fixture_down(&f);
    }
}

static void test_lossless_full(void) {
    fixture f;
    ta_rings_specs specs = specs_of(64, 32, TA_RING_LOSSLESS);
    fixture_up(&f, &specs);
    ta_ring *ring = &f.rings.ring[TA_RING_C2H];
    ta_ring_reservation res;
    for (uint32_t i = 1; i <= 2; i += 1) { /* two records of 32 fill the 64 bytes to the last */
        CHECK(ta_ring_reserve(ring, 32, &res) == TA_RING_OK && res.seq == i);
        fill_record(res.bytes, 32, i);
        ta_ring_commit(ring);
    }
    unsigned char control[sizeof(ta_ring_control)], data[64];
    memcpy(control, f.rings.control, sizeof control);
    memcpy(data, ring->data, sizeof data);
    for (uint32_t i = 0; i < 3; i += 1) {
        CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_FULL && res.bytes == NULL && res.seq == 3);
        CHECK(memcmp(control, f.rings.control, sizeof control) == 0 && memcmp(data, ring->data, sizeof data) == 0);
    }
    host_release(ring, 32);
    CHECK(ta_ring_reserve(ring, 32, &res) == TA_RING_OK && res.seq == 3 && res.bytes == ring->data);
    ta_ring_commit(ring);
    CHECK(record_ok(ring->data + 32, 32, 2) && read64(ring->data + 8) == 3);
    fixture_down(&f);
}

/* A corrupt peer tail is a protocol failure for the producer too: it moved back or past head. */
static void test_producer_corrupt_tail(void) {
    fixture f;
    ta_rings_specs specs = specs_of(64, 32, TA_RING_LOSSLESS);
    fixture_up(&f, &specs);
    ta_ring *ring = &f.rings.ring[TA_RING_C2H];
    ta_ring_reservation res;
    CHECK(ta_ring_reserve(ring, 32, &res) == TA_RING_OK);
    ta_ring_commit(ring);
    ring->shared->tail = 40; /* past head (32) */
    CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_CORRUPT);
    ring->shared->tail = 32;
    CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_OK);
    ta_ring_commit(ring);
    ring->shared->tail = 8; /* back from 32 */
    CHECK(ta_ring_reserve(ring, 16, &res) == TA_RING_CORRUPT);
    fixture_down(&f);
}

int main(void) {
    test_layout();
    test_init();
    test_region_failure_sweep();
    test_wrap_at_every_offset();
    test_exact_max_record();
    test_sizes_busy_abandon();
    test_lossless_full();
    test_producer_corrupt_tail();
    if (failures != 0) {
        fprintf(stderr, "ring_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("ring_test: ok");
    return 0;
}
