/*
 * handle.h: generation handles, a slot in the low 32 bits and the slot's
 * generation in the high 32 bits. Actor and definition IDs are handles, and
 * they leave core in the public API, session images and traces, so the
 * packing is pinned here.
 *
 * Claims proven here, in order:
 *
 * 1. make, slot and generation round-trip at the boundaries: slot 0, slot
 *    and generation at UINT32_MAX, and generation 0;
 * 2. the packed values are pinned: `(generation << 32) | slot`, the values
 *    core has always issued;
 * 3. a handle is valid exactly when its generation is not 0: 0 and every
 *    handle made with generation 0 are not, a handle with generation 1 or
 *    UINT32_MAX is, whatever its slot;
 * 4. the public actor ID helpers (`ta_actor_id_make`, `ta_actor_id_slot`,
 *    `ta_actor_id_generation`) agree with the handle functions;
 * 5. a system issues the pinned IDs: the first definitions and actors get
 *    generation 1 in slots 0 and 1, a reused slot's next ID has generation
 *    2, and retiring a definition ID treats an older generation of a slot
 *    as freed but a handle with generation 0 as never issued.
 *
 * The test includes the amalgamation so it can reach the private units; it
 * uses the C library, the core does not.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* ------------------------------------------------------------------------
 * An allocator over the C library, for the system in claim 5.
 */

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    return aligned_alloc(alignment, size == 0 ? alignment : size);
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* A slot and a generation, with the handle core issued for them. */
typedef struct {
    uint32_t slot;
    uint32_t generation;
    uint64_t packed;
} pinned_handle;

/* Recorded from the actor ID helper before handle.h existed. */
static const pinned_handle pinned[] = {
    { 0u, 0u, 0x0000000000000000ull },
    { 0u, 1u, 0x0000000100000000ull },
    { 1u, 1u, 0x0000000100000001ull },
    { 7u, 3u, 0x0000000300000007ull },
    { 0x12345678u, 0x9abcdef0u, 0x9abcdef012345678ull },
    { UINT32_MAX, 0u, 0x00000000ffffffffull },
    { UINT32_MAX, 1u, 0x00000001ffffffffull },
    { 0u, UINT32_MAX, 0xffffffff00000000ull },
    { UINT32_MAX, UINT32_MAX, 0xffffffffffffffffull },
};

#define PINNED_COUNT (sizeof(pinned) / sizeof(pinned[0]))

/* Claim 1. */
static void test_round_trip(void) {
    for (size_t index = 0; index < PINNED_COUNT; index += 1) {
        ta_handle handle = ta_handle_make(pinned[index].slot, pinned[index].generation);
        CHECK(ta_handle_slot(handle) == pinned[index].slot);
        CHECK(ta_handle_generation(handle) == pinned[index].generation);
        CHECK(ta_handle_make(ta_handle_slot(handle), ta_handle_generation(handle)) == handle);
    }
    /* Unpacking any 64-bit value and packing it again gives it back. */
    uint64_t values[] = { 0u, 1u, 0x8000000000000000ull, 0x00000000ffffffffull, 0xfedcba9876543210ull, UINT64_MAX };
    for (size_t index = 0; index < sizeof(values) / sizeof(values[0]); index += 1) {
        CHECK(ta_handle_make(ta_handle_slot(values[index]), ta_handle_generation(values[index])) == values[index]);
    }
}

/* Claim 2. */
static void test_packing_is_pinned(void) {
    for (size_t index = 0; index < PINNED_COUNT; index += 1) {
        CHECK(ta_handle_make(pinned[index].slot, pinned[index].generation) == pinned[index].packed);
        CHECK(ta_handle_slot(pinned[index].packed) == pinned[index].slot);
        CHECK(ta_handle_generation(pinned[index].packed) == pinned[index].generation);
    }
}

/* Claim 3. */
static void test_valid(void) {
    CHECK(!ta_handle_valid(0u));
    CHECK(!ta_handle_valid(ta_handle_make(0u, 0u)));
    CHECK(!ta_handle_valid(ta_handle_make(1u, 0u)));
    CHECK(!ta_handle_valid(ta_handle_make(UINT32_MAX, 0u)));
    CHECK(ta_handle_valid(ta_handle_make(0u, 1u)));
    CHECK(ta_handle_valid(ta_handle_make(UINT32_MAX, 1u)));
    CHECK(ta_handle_valid(ta_handle_make(0u, UINT32_MAX)));
    CHECK(ta_handle_valid(ta_handle_make(UINT32_MAX, UINT32_MAX)));
    for (size_t index = 0; index < PINNED_COUNT; index += 1) {
        CHECK(ta_handle_valid(pinned[index].packed) == (pinned[index].generation != 0u));
    }
}

/* Claim 4. */
static void test_public_actor_ids_agree(void) {
    for (size_t index = 0; index < PINNED_COUNT; index += 1) {
        ta_actor_id id = ta_actor_id_make(pinned[index].slot, pinned[index].generation);
        CHECK(id == ta_handle_make(pinned[index].slot, pinned[index].generation));
        CHECK(ta_actor_id_slot(id) == ta_handle_slot(id));
        CHECK(ta_actor_id_generation(id) == ta_handle_generation(id));
    }
}

static ta_definition_id register_chart(ta_system *system, ta_str name) {
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec spec = { .name = name, .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &spec,
                                                                           .datamodel = ta_null_datamodel(),
                                                                           .allocator = test_allocator,
                                                                           .out = &definition }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static ta_actor_id spawn(ta_system *system, ta_definition_id definition) {
    ta_actor_id id = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &id }) == TA_OK);
    return id;
}

/* Claim 5. */
static void test_system_issues_pinned_ids(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) == TA_OK);

    ta_definition_id first = register_chart(system, S("first"));
    ta_definition_id second = register_chart(system, S("second"));
    CHECK(first == 0x0000000100000000ull);
    CHECK(second == 0x0000000100000001ull);
    CHECK(ta_system_release_definition(system, first) == TA_OK);
    ta_definition_id third = register_chart(system, S("third"));
    CHECK(third == 0x0000000200000000ull);
    CHECK(ta_handle_slot(third) == ta_handle_slot(first));

    /* An older generation of slot 0 was freed; generation 0 never existed. */
    CHECK(ta_system_retire_definition(system, first) == TA_OK);
    CHECK(ta_system_retire_definition(system, ta_handle_make(0u, 0u)) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_retire_definition(system, ta_handle_make(0u, 5u)) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_retire_definition(system, ta_handle_make(9u, 1u)) == TA_ERR_NOT_FOUND);

    ta_actor_id one = spawn(system, second);
    ta_actor_id two = spawn(system, second);
    CHECK(one == 0x0000000100000000ull);
    CHECK(two == 0x0000000100000001ull);
    CHECK(ta_actor_destroy(system, one) == TA_OK);
    ta_actor_id three = spawn(system, second);
    CHECK(three == 0x0000000200000000ull);

    ta_system_destroy(system);
}

int main(void) {
    test_round_trip();
    test_packing_is_pinned();
    test_valid();
    test_public_actor_ids_agree();
    test_system_issues_pinned_ids();
    if (failures != 0) {
        fprintf(stderr, "handle_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("handle_test: ok");
    return 0;
}
