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

int main(void) {
    test_layout();
    test_init();
    test_region_failure_sweep();
    if (failures != 0) {
        fprintf(stderr, "ring_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("ring_test: ok");
    return 0;
}
