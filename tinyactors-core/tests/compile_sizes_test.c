/*
 * The sizes the compiler derives from a chart's counts, and the scratch
 * the interpreter takes for it: none of the arithmetic wraps, and a chart
 * whose sizes would not fit is refused instead of compiled with a size too
 * small for what is later written into it.
 *
 * Claims proven here, in order:
 *
 * 1. the words of a bitset hold every bit, for every count up to
 *    UINT32_MAX: 0, 1, 63, 64 and 65 bits, and the counts just below
 *    UINT32_MAX, where `bits + 63` would wrap;
 * 2. an actor's history storage is sized exactly while the session it
 *    belongs to (the configuration, its late-binding bits and the history
 *    bitmaps) fits in 32 bits, the boundary included, and is refused one
 *    slot past it, with the output untouched; counts whose 32-bit product
 *    wraps are refused, not sized from the wrapped value;
 * 3. the entry and executor frame capacities are the true counts at depths
 *    where 32-bit arithmetic wrapped, up to UINT32_MAX, and the scratch
 *    need they give is at least those frames' bytes (on a 64-bit build), or
 *    TA_ERR_NOMEM where a size_t cannot hold the need;
 * 4. the compiler refuses a definition whose frame capacity is that large:
 *    TA_ERR_UNSUPPORTED with a TA_COMPILE_ERROR_LIMIT error (TA_ERR_NOMEM
 *    on a 32-bit build), no definition, nothing registered and nothing
 *    leaked;
 * 5. an ordinary chart (73 states, three history states, guards and
 *    nested <if>) compiles to the sizes core computed before this
 *    arithmetic was checked: its history slots, storage and session bytes,
 *    its frame and guard capacities, its scratch need, its largest
 *    continuation and its actor block.
 *
 * Claims 3 and 4 set the counts on a definition directly: a chart deep
 * enough to reach them cannot be built in a test.
 *
 * The test includes the amalgamation so it can reach the private units; it
 * uses the C library, the core does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* The struct sizes the pinned scratch, continuation and actor-block bytes
 * were recorded with, on a 64-bit build. */
static bool pinned_sizes(void) {
    return sizeof(void *) == 8u && sizeof(ta_actor) == 232u && _Alignof(ta_actor) == 8u &&
           sizeof(ta_continuation) == 48u && sizeof(ta_exec_frame) == 40u && _Alignof(ta_exec_frame) == 8u &&
           sizeof(ta_arena_block) == 40u && sizeof(struct ta_entry_frame) == 48u &&
           sizeof(struct ta_trace_condition) == 24u;
}

/* ------------------------------------------------------------------------
 * An ordinary chart: a compound `top` holding three regions of 22 atomic
 * states and a history state each (the middle one deep), so 73 states and
 * two words a set. The first state of each region has a guarded
 * transition and an <onentry> with an <if> nested in an <if>.
 */

#define REGIONS 3u
#define LEAVES 22u

static char names[REGIONS][LEAVES + 2u][16];
static ta_str first_target[REGIONS][2];
static ta_scxml_transition_spec history_default[REGIONS][2];
static ta_str guarded_target[2];
static ta_scxml_transition_spec guarded[2];
static ta_scxml_state_spec leaves[REGIONS][LEAVES + 2u];
static ta_scxml_state_spec regions[REGIONS + 1u];
static ta_scxml_state_spec top[2];
static const ta_scxml_action_spec innermost[] = { { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = S("in") } },
                                                  { 0 } };
static ta_scxml_action_spec inner[2];
static ta_scxml_action_spec outer[2];
static ta_scxml_handler_spec onentry[2];
static ta_scxml_spec chart_spec;

/* Writes `<prefix><region>` into `buffer`, with `_<leaf>` after it for a leaf. */
static ta_str name_of(char *buffer, char prefix, uint32_t region, int32_t leaf) {
    int length = leaf < 0 ? snprintf(buffer, 16, "%c%u", prefix, region)
                          : snprintf(buffer, 16, "%c%u_%d", prefix, region, leaf);
    return (ta_str){ .ptr = buffer, .len = (uint32_t)length };
}

static void build_chart(void) {
    inner[0] = (ta_scxml_action_spec){
        .kind = TA_SCXML_ACTION_IF,
        .as.conditional = { .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("In('r0')") },
                            .then_actions = innermost },
    };
    outer[0] = (ta_scxml_action_spec){
        .kind = TA_SCXML_ACTION_IF,
        .as.conditional = { .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("In('r1')") },
                            .then_actions = inner },
    };
    onentry[0] = (ta_scxml_handler_spec){ .actions = outer };
    guarded_target[0] = S("r1");
    guarded[0] = (ta_scxml_transition_spec){
        .kind = TA_SCXML_TRANSITION_EXTERNAL,
        .event = S("go"),
        .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("In('r2')") },
        .targets = guarded_target,
    };
    for (uint32_t region = 0; region < REGIONS; region += 1u) {
        for (uint32_t leaf = 0; leaf < LEAVES; leaf += 1u) {
            leaves[region][leaf] = (ta_scxml_state_spec){
                .kind = TA_SCXML_STATE_ATOMIC,
                .id = name_of(names[region][leaf], 's', region, (int32_t)leaf),
            };
        }
        leaves[region][0].transitions = guarded;
        leaves[region][0].onentry = onentry;
        first_target[region][0] = leaves[region][0].id;
        history_default[region][0] =
            (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = first_target[region] };
        leaves[region][LEAVES] = (ta_scxml_state_spec){
            .kind = region == 1u ? TA_SCXML_STATE_HISTORY_DEEP : TA_SCXML_STATE_HISTORY_SHALLOW,
            .id = name_of(names[region][LEAVES], 'h', region, -1),
            .transitions = history_default[region],
        };
        regions[region] = (ta_scxml_state_spec){
            .kind = TA_SCXML_STATE_COMPOUND,
            .id = name_of(names[region][LEAVES + 1u], 'r', region, -1),
            .states = leaves[region],
        };
    }
    top[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_COMPOUND, .id = S("top"), .states = regions };
    chart_spec = (ta_scxml_spec){ .name = S("sizes"), .states = top };
}

static ta_definition *compile_chart(ta_system *system, ta_allocator allocator) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &chart_spec,
                                                                             .datamodel = ta_null_datamodel(),
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    return definition;
}

/* Claim 1. */
static void test_bits_words(void) {
    CHECK(ta_bits_words(0u) == 0u);
    CHECK(ta_bits_words(1u) == 1u);
    CHECK(ta_bits_words(63u) == 1u);
    CHECK(ta_bits_words(64u) == 1u);
    CHECK(ta_bits_words(65u) == 2u);
    CHECK(ta_bits_words(73u) == 2u);
    CHECK(ta_bits_words(UINT32_MAX - 63u) == 67108863u); /* the last count where bits + 63 fits */
    CHECK(ta_bits_words(UINT32_MAX - 62u) == 67108864u); /* the first where it would wrap */
    CHECK(ta_bits_words(UINT32_MAX - 1u) == 67108864u);
    CHECK(ta_bits_words(UINT32_MAX) == 67108864u);
    /* The words hold every bit and no whole word more, near the top too. */
    for (uint64_t bits = (uint64_t)UINT32_MAX - 200u; bits <= UINT32_MAX; bits += 1u) {
        uint64_t words = ta_bits_words((uint32_t)bits);
        CHECK(words * 64u >= bits && (words == 0u || (words - 1u) * 64u < bits));
    }
    for (uint32_t bits = 0; bits <= 1000u; bits += 1u) {
        CHECK(ta_bits_words(bits) == (bits + 63u) / 64u);
    }
}

/* Claim 2. */
static void test_history_storage(void) {
    uint32_t bytes = 7u;
    CHECK(ta_history_storage_bytes(0u, 16u, &bytes) && bytes == 0u);
    CHECK(ta_history_storage_bytes(3u, 16u, &bytes) && bytes == 48u);
    CHECK(ta_history_storage_bytes(1u, 8u, &bytes) && bytes == 8u);

    /* UINT32_MAX is 65535 * 65537: the session of 65535 slots of 65535
     * bytes is exactly UINT32_MAX, which fits; one slot more does not. */
    CHECK(ta_history_storage_bytes(65535u, 65535u, &bytes) && bytes == 65535u * 65535u);
    bytes = 7u;
    CHECK(!ta_history_storage_bytes(65536u, 65535u, &bytes));
    CHECK(bytes == 7u);

    /* The largest configuration (8191 words): 65542 slots fit, 65543 do not. */
    uint32_t largest = 8191u * 8u;
    CHECK(((uint64_t)65542u + 2u) * largest <= UINT32_MAX);
    CHECK(((uint64_t)65543u + 2u) * largest > UINT32_MAX);
    CHECK(ta_history_storage_bytes(65542u, largest, &bytes) && bytes == 65542u * largest);
    bytes = 7u;
    CHECK(!ta_history_storage_bytes(65543u, largest, &bytes));
    CHECK(bytes == 7u);

    /* Counts whose 32-bit product wraps to a small size are refused. */
    CHECK((uint32_t)(65537u * 65536u) == 65536u);
    CHECK(!ta_history_storage_bytes(65537u, 65536u, &bytes));
    CHECK(!ta_history_storage_bytes(UINT32_MAX, 8u, &bytes));
    CHECK(!ta_history_storage_bytes(UINT32_MAX - 1u, 1u, &bytes));
    CHECK(bytes == 7u);
}

/* Claim 3. */
static void test_frame_capacities(void) {
    ta_definition definition = { 0 };
    CHECK(ta_entry_capacity(&definition) == 4u);
    CHECK(ta_exec_capacity(&definition) == 1u);

    /* Where 3 * (depth + slots + 1) + 1 wrapped to 3 in 32 bits. */
    definition.max_state_depth = 0x55555555u;
    CHECK(ta_entry_capacity(&definition) == 0x100000003ull);
    definition.max_state_depth = 0x2aaaaaaau;
    definition.history.slot_count = 0x2aaaaaabu;
    CHECK(ta_entry_capacity(&definition) == 0x100000003ull);
    definition.max_state_depth = UINT32_MAX;
    definition.history.slot_count = UINT32_MAX;
    CHECK(ta_entry_capacity(&definition) == 3u * (2u * (uint64_t)UINT32_MAX + 1u) + 1u);

    /* Where 2 * depth + 1 wrapped to 1 in 32 bits. */
    definition.max_action_depth = 0x80000000u;
    CHECK(ta_exec_capacity(&definition) == 0x100000001ull);
    definition.max_action_depth = UINT32_MAX;
    CHECK(ta_exec_capacity(&definition) == 2u * (uint64_t)UINT32_MAX + 1u);

    /* The need counts those frames, so it cannot fit in interpreterBytes. */
    ta_definition deep = { 0 };
    deep.max_state_depth = 0x55555555u;
    size_t need = 0;
#if SIZE_MAX > UINT32_MAX
    CHECK(ta_interpreter_scratch_need(&deep, &need) == TA_OK);
    CHECK(need >= 0x100000003ull * sizeof(struct ta_entry_frame));
    CHECK(need > UINT32_MAX);
    ta_definition nested = { 0 };
    nested.max_action_depth = 0x80000000u;
    CHECK(ta_interpreter_scratch_need(&nested, &need) == TA_OK);
    CHECK(need >= 0x100000001ull * sizeof(ta_exec_frame));
    CHECK(need > UINT32_MAX);
#else
    CHECK(ta_interpreter_scratch_need(&deep, &need) == TA_ERR_NOMEM);
#endif
}

/* Claim 4: compiles the ordinary chart, then sets one count so its frames
 * cannot fit, and asks the compiler's scratch check about it. */
static void refuse_with(void (*make_deep)(ta_definition *)) {
    test_allocator heap = { 0 };
    ta_allocator allocator = test_allocator_of(&heap);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition *definition = compile_chart(system, allocator);
    if (definition != NULL) {
        make_deep(definition);
        ta_compile_error error = { 0 };
        ta_definition_compile_params params = {
            .spec = &chart_spec,
            .datamodel = ta_null_datamodel(),
            .allocator = allocator,
            .out = &definition,
            .error = &error,
        };
#if SIZE_MAX > UINT32_MAX
        CHECK(ta_compile_check_scratch(system, &params) == TA_ERR_UNSUPPORTED);
        CHECK(error.code == TA_COMPILE_ERROR_LIMIT);
        CHECK(error.message.len != 0);
#else
        CHECK(ta_compile_check_scratch(system, &params) == TA_ERR_NOMEM);
#endif
        CHECK(definition == NULL);
        CHECK(system->live_definitions == 0u && system->definitions.count == 0u);
    }
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

static void deep_states(ta_definition *definition) {
    definition->max_state_depth = 0x55555555u;
}

static void deep_actions(ta_definition *definition) {
    definition->max_action_depth = 0x80000000u;
}

static void test_compiler_refuses(void) {
    refuse_with(deep_states);
    refuse_with(deep_actions);
}

/* Claim 5. */
static void test_ordinary_chart(void) {
    test_allocator heap = { 0 };
    ta_allocator allocator = test_allocator_of(&heap);
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition *definition = compile_chart(system, allocator);
    if (definition != NULL) {
        CHECK(definition->state_count == 73u);
        CHECK(definition->max_state_depth == 3u && definition->max_action_depth == 3u);
        CHECK(definition->configuration.bytes == 16u);
        CHECK(definition->history.slot_count == 3u);
        for (uint32_t slot = 0; slot < definition->history.slot_count; slot += 1u) {
            CHECK(definition->history.slots[slot].offset == slot * 16u);
            CHECK(definition->history.slots[slot].bytes == 16u);
        }
        CHECK(definition->history.storage_bytes == 48u);
        CHECK(ta_definition_session_bytes(definition) == 80u);
        CHECK(ta_entry_capacity(definition) == 22u);
        CHECK(ta_exec_capacity(definition) == 7u);
        CHECK(ta_trace_guard_capacity(definition) == 3u);
        CHECK(ta_decl_words(definition) == 1u);
        size_t need = 0;
        size_t continuation = 0;
        CHECK(ta_interpreter_scratch_need(definition, &need) == TA_OK);
        CHECK(ta_continuation_capacity(definition, &continuation) == TA_OK);
        if (pinned_sizes()) {
            CHECK(need == 3480u);
            CHECK(continuation == 992u);
            CHECK(definition->actor_block_bytes == 312u);
        }
        ta_definition_destroy(definition);
    }
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    build_chart();
    test_bits_words();       /* claim 1 */
    test_history_storage();  /* claim 2 */
    test_frame_capacities(); /* claim 3 */
    test_compiler_refuses(); /* claim 4 */
    test_ordinary_chart();   /* claim 5 */
    if (!pinned_sizes()) {
        fprintf(stderr, "compile_sizes_test: struct sizes differ from the pinned ones; scratch bytes not pinned\n");
    }
    if (failures != 0) {
        fprintf(stderr, "compile_sizes_test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("compile_sizes_test: ok");
    return 0;
}
