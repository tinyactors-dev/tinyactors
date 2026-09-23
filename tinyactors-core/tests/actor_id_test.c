/*
 * ADR 0059: an actor ID is a public slot and generation. The split and join
 * functions round-trip, a released slot's generation moves on so the old ID
 * goes stale, and a generation that wraps skips 0: no slot is ever issued
 * generation 0.
 *
 * Includes the amalgamation, so it can set a free slot's generation to
 * UINT32_MAX directly instead of reusing one slot four billion times.
 */
#include "../include/tinyactors.c"

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

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

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

static void test_split_and_join(void) {
    ta_actor_id id = ta_actor_id_make(7u, 3u);
    CHECK(ta_actor_id_slot(id) == 7u);
    CHECK(ta_actor_id_generation(id) == 3u);
    CHECK(id == (((ta_actor_id)3u << 32) | 7u)); /* the documented layout */
    ta_actor_id extremes = ta_actor_id_make(UINT32_MAX - 1u, UINT32_MAX);
    CHECK(ta_actor_id_slot(extremes) == UINT32_MAX - 1u);
    CHECK(ta_actor_id_generation(extremes) == UINT32_MAX);
    CHECK(ta_actor_id_make(0u, 0u) == 0u); /* zero: never a valid ID */
}

static ta_actor_id spawn(ta_system *system, ta_definition_id definition) {
    ta_actor_id id = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &id }) == TA_OK);
    return id;
}

static bool resident(ta_system *system, ta_actor_id id) {
    ta_actor_inspection view;
    return ta_actor_inspect(system, id, &view) == TA_OK;
}

static void test_generation_skips_zero(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) == TA_OK);
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") }, { 0 } };
    ta_scxml_spec spec = { .name = S("ids"), .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &spec,
                                                                           .datamodel = ta_null_datamodel(),
                                                                           .allocator = test_allocator,
                                                                           .out = &definition }) == TA_OK);
    ta_definition_id chart = 0;
    CHECK(ta_system_register_definition(system, definition, &chart) == TA_OK);

    /* A fresh slot starts at generation 1. */
    ta_actor_id first = spawn(system, chart);
    uint32_t slot = ta_actor_id_slot(first);
    CHECK(ta_actor_id_generation(first) == 1u);

    /* Destroyed, the ID goes stale, and the slot's next occupant is newer. */
    CHECK(ta_actor_destroy(system, first) == TA_OK);
    CHECK(!resident(system, first));
    ta_actor_id second = spawn(system, chart);
    CHECK(ta_actor_id_slot(second) == slot);
    CHECK(ta_actor_id_generation(second) == 2u);
    CHECK(!resident(system, first));

    /* The slot's generation at its last value before the wrap. */
    CHECK(ta_actor_destroy(system, second) == TA_OK);
    ta_actor_slot *entry = ta_vec_at(&system->actors, slot);
    entry->generation = UINT32_MAX;
    ta_actor_id last = spawn(system, chart);
    CHECK(ta_actor_id_slot(last) == slot);
    CHECK(ta_actor_id_generation(last) == UINT32_MAX);
    CHECK(resident(system, last));

    /* Released, it wraps to 1, not 0. */
    CHECK(ta_actor_destroy(system, last) == TA_OK);
    CHECK(entry == ta_vec_at(&system->actors, slot) && entry->generation == 1u);
    ta_actor_id wrapped = spawn(system, chart);
    CHECK(ta_actor_id_slot(wrapped) == slot);
    CHECK(ta_actor_id_generation(wrapped) == 1u);
    CHECK(!resident(system, last));
    CHECK(!resident(system, ta_actor_id_make(slot, 0u)));
    /*
     * After 2^32 - 1 reuses the first ID names the slot again: the aliasing
     * ADR 0059 accepts, at full 32-bit strength.
     */
    CHECK(resident(system, first));

    ta_system_destroy(system);
}

int main(void) {
    test_split_and_join();
    test_generation_skips_zero();
    if (failures != 0) {
        fprintf(stderr, "actor id test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core actor id test passed");
    return 0;
}
