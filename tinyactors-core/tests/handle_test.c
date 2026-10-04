/*
 * Block E1 (handle.h): generation handles. Pure functions, so nothing here
 * allocates. Checks the packing at its edges (slot 0, the largest slot,
 * generation 0 and UINT32_MAX), that the bit layout is the documented one
 * independently of the code, that a generation wrap skips 0, that a stale
 * handle stops matching but still unpacks, and that the public
 * ta_actor_id_* functions agree with the block bit for bit.
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

static const uint32_t slots[] = { 0u, 1u, 0x7fffffffu, 0xfffffffeu, UINT32_MAX };
static const uint32_t generations[] = { 0u, 1u, 2u, 0x7fffffffu, UINT32_MAX };
#define COUNT(array) (sizeof(array) / sizeof((array)[0]))

static void test_round_trip_and_public_agreement(void) {
    for (size_t s = 0; s < COUNT(slots); s += 1) {
        for (size_t g = 0; g < COUNT(generations); g += 1) {
            ta_handle handle = ta_handle_make(slots[s], generations[g]);
            CHECK(ta_handle_slot(handle) == slots[s]);
            CHECK(ta_handle_generation(handle) == generations[g]);
            CHECK(handle == (((uint64_t)generations[g] << 32) | slots[s]));
            /* The public functions are bit-identical to the block. */
            CHECK(ta_actor_id_make(slots[s], generations[g]) == handle);
            CHECK(ta_actor_id_slot(handle) == slots[s]);
            CHECK(ta_actor_id_generation(handle) == generations[g]);
        }
    }
}

static void test_literal_layout(void) {
    /* Pinned by value, not through the code under test. */
    CHECK(ta_handle_make(7u, 3u) == 0x0000000300000007ull);
    CHECK(ta_handle_make(UINT32_MAX, UINT32_MAX) == 0xffffffffffffffffull);
    CHECK(ta_handle_make(0u, 0u) == 0u);
    CHECK(ta_handle_make(0u, 1u) == 0x0000000100000000ull);
}

static void test_valid(void) {
    CHECK(!ta_handle_valid(ta_handle_make(0u, 0u)));
    CHECK(!ta_handle_valid(ta_handle_make(UINT32_MAX, 0u)));
    CHECK(ta_handle_valid(ta_handle_make(0u, 1u))); /* slot 0 is a slot */
    CHECK(ta_handle_valid(ta_handle_make(UINT32_MAX - 1u, UINT32_MAX)));
}

static void test_matches(void) {
    ta_handle handle = ta_handle_make(5u, 9u);
    CHECK(ta_handle_matches(handle, 9u));
    CHECK(!ta_handle_matches(handle, 8u));
    CHECK(!ta_handle_matches(handle, 10u));
    CHECK(!ta_handle_matches(handle, 0u));
    /* Generation 0 never matches, even a slot holding 0. */
    CHECK(!ta_handle_matches(ta_handle_make(5u, 0u), 0u));
    CHECK(ta_handle_matches(ta_handle_make(0u, UINT32_MAX), UINT32_MAX));
}

static void test_next_generation(void) {
    CHECK(ta_handle_next_generation(1u) == 2u);
    CHECK(ta_handle_next_generation(UINT32_MAX - 1u) == UINT32_MAX);
    CHECK(ta_handle_next_generation(UINT32_MAX) == 1u);
    CHECK(ta_handle_next_generation(0u) == 1u);
    uint32_t generation = UINT32_MAX - 3u;
    for (int step = 0; step < 8; step += 1) {
        generation = ta_handle_next_generation(generation);
        CHECK(generation != 0u);
    }
}

/* A one-slot table model: free bumps the generation, reissue hands it out. */
static void test_stale_handle(void) {
    uint32_t generation = 1u;
    ta_handle first = ta_handle_make(0u, generation);
    CHECK(ta_handle_matches(first, generation));
    generation = ta_handle_next_generation(generation);
    ta_handle second = ta_handle_make(0u, generation);
    CHECK(!ta_handle_matches(first, generation));
    CHECK(ta_handle_matches(second, generation));
    /* The stale handle is still a value: it unpacks to what it was. */
    CHECK(ta_handle_slot(first) == 0u);
    CHECK(ta_handle_generation(first) == 1u);
    CHECK(first != second);
}

int main(void) {
    test_round_trip_and_public_agreement();
    test_literal_layout();
    test_valid();
    test_matches();
    test_next_generation();
    test_stale_handle();
    if (failures != 0) {
        fprintf(stderr, "handle test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core handle test passed");
    return 0;
}
