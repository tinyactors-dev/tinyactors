/* test: standalone asan */
/*
 * Block E1 (handle.h): generation handles. Pure functions, so nothing here
 * allocates. Checks the packing at its edges (slot 0, the largest slot,
 * generation 0 and UINT32_MAX), that the bit layout is the documented one
 * independently of the code, that a generation wrap skips 0, that a stale
 * handle stops matching but still unpacks, and that the public
 * ta_actor_id_* functions agree with the block bit for bit.
 */
#include "../include/tinyactors.c"

#include "test_allocator.h"

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

/*
 * A definition slot whose generation is UINT32_MAX gets 1 when freed, as an
 * actor slot does, so no definition ID ever has generation 0.
 */
static void test_definition_generation_wrap(void) {
    static ta_test_allocator heap;
    ta_allocator allocator = (ta_allocator)TA_TEST_ALLOCATOR_STATIC(&heap);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = { .ptr = "idle", .len = 4 } }, {0} };
    ta_definition_identity identity = { .family = { { .ptr = "wrap", .len = 4 } },
                                        .revision = { .ptr = (const uint8_t *)"1", .len = 1 } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system,
                                     &(ta_definition_compile_params){
                                         .spec = &(ta_scxml_spec){ .identity = &identity, .states = states },
                                         .datamodel = ta_null_datamodel(),
                                         .allocator = allocator,
                                         .out = &definition,
                                     }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    CHECK(ta_handle_valid(id));
    ta_definition_slot *entry = ta_system_definition_slot(system, ta_handle_slot(id));
    entry->generation = UINT32_MAX;
    ta_definition_id last = ta_handle_make(ta_handle_slot(id), UINT32_MAX);
    CHECK(ta_system_definition(system, last) == definition);
    CHECK(ta_system_retire_definition(system, last) == TA_OK);
    CHECK(entry->generation == 1u);
    CHECK(ta_system_definition(system, last) == NULL);
    ta_system_destroy(system);
    CHECK(heap.stats.live_blocks == 0 && heap.stats.live_bytes == 0);
}

int main(void) {
    test_round_trip_and_public_agreement();
    test_literal_layout();
    test_valid();
    test_matches();
    test_next_generation();
    test_stale_handle();
    test_definition_generation_wrap();
    if (failures != 0) {
        fprintf(stderr, "handle test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core handle test passed");
    return 0;
}
