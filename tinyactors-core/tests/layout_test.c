/*
 * Block A2 (src/layout.h): the layout builder. Arithmetic, alignment, the
 * start offset, zero counts and a failure at every edge of size_t, which is
 * the arithmetic a 32-bit host (WebAssembly) meets with 32-bit counts; the
 * builder never allocates, so no allocator is needed here.
 *
 * Includes the amalgamation, because the builder is private to core.
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

static size_t add(ta_layout *layout, size_t size, size_t alignment, size_t count) {
    return ta_layout_add(layout, &(ta_layout_entry){ .size = size, .alignment = alignment, .count = count });
}

static void test_empty(void) {
    ta_layout layout;
    size_t bytes = 99;
    size_t alignment = 99;
    ta_layout_init(&layout, 0);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 0 && alignment == 1u);
    ta_layout_init(&layout, 24);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 24u && alignment == 1u);
}

static void test_single_and_mixed(void) {
    ta_layout layout;
    size_t bytes = 0;
    size_t alignment = 0;
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 4, 4, 3) == 0);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 12u && alignment == 4u);

    /* 1, 4, 8, 16 in turn: each offset is the end rounded up. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 1, 1, 3) == 0);   /* end 3 */
    CHECK(add(&layout, 4, 4, 1) == 4u);  /* end 8 */
    CHECK(add(&layout, 1, 1, 1) == 8u);  /* end 9 */
    CHECK(add(&layout, 8, 8, 2) == 16u); /* end 32 */
    CHECK(add(&layout, 1, 1, 1) == 32u); /* end 33 */
    CHECK(add(&layout, 16, 16, 1) == 48u); /* end 64 */
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 64u && alignment == 16u);

    /* The total is the unpadded end: it is not rounded to the alignment. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 8, 8, 1) == 0);
    CHECK(add(&layout, 1, 1, 1) == 8u);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 9u && alignment == 8u);
    CHECK(ta_layout_bytes(&layout) == 9u);
}

static void test_start_offset(void) {
    ta_layout layout;
    size_t bytes = 0;
    ta_layout_init(&layout, 5);
    CHECK(add(&layout, 8, 8, 1) == 8u); /* the first byte is at 5, an 8-aligned entry goes to 8 */
    CHECK(add(&layout, 2, 2, 1) == 16u);
    CHECK(ta_layout_finish(&layout, &bytes, NULL) == TA_OK);
    CHECK(bytes == 18u);
}

static void test_zero_count(void) {
    ta_layout layout;
    size_t bytes = 0;
    size_t alignment = 0;
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 1, 1, 3) == 0);
    /* An empty array still has an aligned place, and still counts toward the alignment. */
    CHECK(add(&layout, 8, 8, 0) == 8u);
    CHECK(add(&layout, 1, 1, 1) == 8u);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_OK);
    CHECK(bytes == 9u && alignment == 8u);
    /* A zero-size item takes no bytes however many there are, even SIZE_MAX of them. */
    ta_layout_init(&layout, 0);
    CHECK(add(&layout, 0, 1, SIZE_MAX) == 0);
    CHECK(ta_layout_finish(&layout, &bytes, NULL) == TA_OK && bytes == 0);
}

static void test_overflow(void) {
    ta_layout layout;
    size_t bytes = 77;
    size_t alignment = 77;

    /* count * size wraps. */
    ta_layout_init(&layout, 0);
    add(&layout, SIZE_MAX / 2u + 1u, 1, 2);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);
    CHECK(bytes == 77u && alignment == 77u); /* outputs untouched */
    CHECK(ta_layout_bytes(&layout) == 0);

    /* The product fits but the sum with the end does not. */
    ta_layout_init(&layout, 0);
    add(&layout, 1, 1, SIZE_MAX);
    CHECK(!layout.overflowed && layout.end == SIZE_MAX);
    add(&layout, 1, 1, 1);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);

    /* Rounding up wraps: the end is SIZE_MAX - 1 and the next alignment is 16. */
    ta_layout_init(&layout, SIZE_MAX - 1u);
    add(&layout, 0, 16, 0);
    CHECK(layout.overflowed);

    /* An end that is already aligned near SIZE_MAX is not an overflow. */
    ta_layout_init(&layout, SIZE_MAX - (SIZE_MAX % 16u));
    CHECK(add(&layout, 0, 16, 1) == SIZE_MAX - (SIZE_MAX % 16u));
    CHECK(!layout.overflowed);

    /* An alignment of zero is refused, not divided by. */
    ta_layout_init(&layout, 0);
    add(&layout, 4, 0, 1);
    CHECK(layout.overflowed);

    /* The failure is sticky: a later small entry does not clear it. */
    ta_layout_init(&layout, 0);
    add(&layout, SIZE_MAX, 1, 2);
    CHECK(add(&layout, 1, 1, 1) == 0);
    CHECK(layout.overflowed);
    CHECK(ta_layout_finish(&layout, &bytes, &alignment) == TA_ERR_NOMEM);
    CHECK(bytes == 77u && alignment == 77u);
}

static void test_checked_primitives(void) {
    size_t out = 5;
    CHECK(ta_checked_mul(0, SIZE_MAX, &out) && out == 0);
    CHECK(ta_checked_mul(SIZE_MAX, 1, &out) && out == SIZE_MAX);
    out = 5;
    CHECK(!ta_checked_mul(SIZE_MAX, 2, &out) && out == 5u);
    CHECK(!ta_checked_mul(SIZE_MAX / 2u + 1u, 2, &out) && out == 5u);
    CHECK(ta_checked_mul(SIZE_MAX / 2u, 2, &out) && out == SIZE_MAX - 1u);
    CHECK(ta_checked_add(SIZE_MAX - 1u, 1, &out) && out == SIZE_MAX);
    out = 5;
    CHECK(!ta_checked_add(SIZE_MAX, 1, &out) && out == 5u);
    CHECK(!ta_checked_add(1, SIZE_MAX, &out) && out == 5u);
}

/* ------------------------------------------------------------------------
 * Golden figures, recorded from the hand-written calculators before unit 1.4
 * replaced them (x86_64 Linux). The layout must reproduce them exactly: a
 * block that grew would cost every actor, and a continuation or scratch
 * figure that changed would break a sized free or let a chart register with
 * too little scratch.
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

typedef struct {
    uint32_t states;
    size_t block_bytes;
    size_t instance_offset;
    size_t continuation_bytes;
    size_t scratch_need;
} golden;

/* A chart of `states` atomic states with one transition, compiled for the null data model. */
static void check_chart(ta_system *system, golden expected) {
    uint32_t n = expected.states;
    ta_scxml_state_spec *states = calloc(n + 1u, sizeof *states);
    ta_str *ids = calloc(n + 1u, sizeof *ids);
    char(*names)[16] = calloc(n + 1u, 16);
    ta_scxml_transition_spec transitions[2] = { 0 };
    ta_str targets[2] = { 0 };
    for (uint32_t index = 0; index < n; index += 1u) {
        snprintf(names[index], 16, "s%u", index);
        ids[index] = (ta_str){ .ptr = names[index], .len = (uint32_t)strlen(names[index]) };
        states[index] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = ids[index] };
    }
    targets[0] = ids[n - 1u];
    transitions[0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                 .event = { .ptr = "go", .len = 2 },
                                                 .targets = targets };
    states[0].transitions = transitions;
    ta_scxml_spec spec = { .name = { .ptr = "golden", .len = 6 }, .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &spec,
                                                                              .datamodel = ta_null_datamodel(),
                                                                              .allocator = test_allocator,
                                                                              .out = &definition }) == TA_OK);
    if (definition != NULL) {
        ta_continuation shape = { .words = ta_bits_words(n), .decl_words = 2, .state_count = n,
                                  .enabled_count = n + 1u, .frame_count = 3 };
        size_t need = 0;
        CHECK(definition->actor_block_bytes == expected.block_bytes);
        CHECK(definition->actor_session_offset == 232u);
        CHECK(definition->actor_instance_offset == expected.instance_offset);
        CHECK(definition->actor_block_alignment == 8u);
        CHECK(ta_continuation_bytes(&shape) == expected.continuation_bytes);
        CHECK(ta_interpreter_scratch_need(definition, &need) == TA_OK && need == expected.scratch_need);
        ta_definition_destroy(definition);
    }
    free(states);
    free(ids);
    free(names);
}

static void test_golden_figures(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) == TA_OK);
    if (system == NULL) {
        return;
    }
    check_chart(system, (golden){ 2, 248, 248, 240, 652 });
    check_chart(system, (golden){ 7, 248, 248, 280, 776 });
    check_chart(system, (golden){ 70, 264, 264, 816, 2444 });
    check_chart(system, (golden){ 700, 408, 408, 6144, 18988 });
    /* The default system's arena: one region (interpreterBytes and renderBytes) and the nested table. */
    CHECK(system->arena_bytes == 82048u);
    /* The header is 12 bytes since the jump stores its own size (it was 8). */
    uint32_t jump = 77u;
    CHECK(ta_operator_jump_bytes(3, &jump) == TA_OK && jump == 24u);
    CHECK(ta_operator_jump_bytes(0, &jump) == TA_OK && jump == 12u);
    /* A total above a uint32_t is refused on every host, and the output is left alone. */
    jump = 77u;
    CHECK(ta_operator_jump_bytes(UINT32_MAX, &jump) == TA_ERR_NOMEM && jump == 77u);
    ta_system_destroy(system);
}

int main(void) {
    test_golden_figures();
    test_empty();
    test_single_and_mixed();
    test_start_offset();
    test_zero_count();
    test_overflow();
    test_checked_primitives();
    if (failures != 0) {
        fprintf(stderr, "layout_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("layout_test: ok");
    return 0;
}
