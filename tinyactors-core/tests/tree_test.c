/* test: runtime asan */
/*
 * ta_tree / ta_tree_list: the public storage an importer uses to author a
 * specification tree at run time (include/tinyactors.h, "Storage for
 * dynamically authored specification trees"; docs/internal/builder.md: "The
 * compiler derives all of those, so an importer needs no live ta_system to
 * build a tree").
 *
 * This test was written before core implemented these seven functions: it sat
 * in PENDING_TESTS in tinyactors-core/mise/tasks/build, which reports an
 * unlinkable test instead of failing the build. That is the C-side equivalent
 * of the expected failures in §5 of docs/internal/testing-and-api-change.md,
 * which the scenario suite cannot express here because tinyactors-test skips
 * the tree API by policy (a JSON specification decodes straight into the spec
 * tree, see bindings.rb). src/tree.c implements them, so it is an ordinary
 * test now and every check below has to pass.
 *
 * Claims proven here, in order:
 *
 * 1. a closed list is the contiguous array of what was put into it, followed
 *    by the zero-initialized terminator every child list needs;
 * 2. lists nest: an inner list closed while an outer one is open does not
 *    disturb it, which is the shape every importer builds;
 * 3. ta_tree_alloc honours size and alignment and the blocks it returns do
 *    not overlap;
 * 4. the first allocation failure is retained, reported by ta_tree_result,
 *    and later writes are ignored, so an importer reports one result at its
 *    outer boundary;
 * 5. ta_tree_destroy returns every block to the allocator, with the size and
 *    alignment it was given;
 * 6. a tree-built spec compiles to the same definition revision as the same
 *    chart written as C literals, and the compiler copies what it retains:
 *    the definition still reads back the authored state IDs after the tree
 *    that held their text is destroyed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"

static int failures;

#define CHECK(condition)                                                     \
    do {                                                                     \
        if (!(condition)) {                                                  \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                             \
            failures += 1;                                                   \
        }                                                                    \
    } while (0)

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

/* ------------------------------------------------------------------------
 * The shared test allocator (tests/test_allocator.h), in strict mode: it
 * remembers every block it handed out, so the test can see that a destroyed
 * tree returned all of them, with the size and alignment they were allocated
 * with, and nothing else.
 */

/* Every block is back, and none was returned with the wrong facts. */
static void check_drained(ta_test_allocator *heap, const char *what) {
    if (heap->stats.live_blocks != 0 || heap->stats.live_bytes != 0) {
        fprintf(stderr, "%s: %u blocks (%zu bytes) still live\n", what, (unsigned)heap->stats.live_blocks,
                heap->stats.live_bytes);
        failures += 1;
    }
    if (heap->stats.bad_frees != 0) {
        fprintf(stderr, "%s: %u bad frees\n", what, (unsigned)heap->stats.bad_frees);
        failures += 1;
    }
}

/* ------------------------------------------------------------------------
 * A data model that evaluates nothing: the charts below hold no expressions.
 */

static ta_result model_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}

static void model_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self;
    (void)shared;
    (void)allocator;
}

static uint32_t model_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 8;
}

static uint32_t model_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 8;
}

static ta_result model_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static void model_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static const ta_datamodel_vtable model_vtable = {
    .name = STR("inert"),
    .prepare_definition = model_prepare,
    .destroy_definition = model_destroy_definition,
    .instance_size = model_instance_size,
    .instance_alignment = model_instance_alignment,
    .init_instance = model_init_instance,
    .destroy_instance = model_destroy_instance,
};

static bool str_is(ta_str text, const char *expected) {
    size_t length = strlen(expected);
    return text.len == length && (length == 0 || memcmp(text.ptr, expected, length) == 0);
}

static bool bytes_equal(ta_bytes left, ta_bytes right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

/* ------------------------------------------------------------------------
 * 1 and 2: what a closed list contains, and lists that nest
 */

static void test_a_closed_list_is_terminated(void) {
    ta_test_allocator heap = { .strict = true };
    ta_tree tree;
    ta_tree_init(&tree, ta_test_allocator_get(&heap));
    CHECK(ta_tree_result(&tree) == TA_OK);

    ta_tree_list states;
    ta_tree_list_init(&states, &tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_state_spec),
                                              .element_alignment = _Alignof(ta_scxml_state_spec) });

    static const char *const ids[] = { "idle", "busy", "done" };
    for (uint32_t index = 0; index < 3; index += 1) {
        ta_scxml_state_spec state = {
            .kind = index == 2u ? TA_SCXML_STATE_FINAL : TA_SCXML_STATE_ATOMIC,
            .id = { .ptr = ids[index], .len = (uint32_t)strlen(ids[index]) },
        };
        ta_tree_put(&states, &state);
    }

    const ta_scxml_state_spec *list = ta_tree_close(&states);
    CHECK(ta_tree_result(&tree) == TA_OK);
    CHECK(list != NULL);
    if (list != NULL) {
        for (uint32_t index = 0; index < 3; index += 1) {
            CHECK(str_is(list[index].id, ids[index]));
        }
        CHECK(list[0].kind == TA_SCXML_STATE_ATOMIC);
        CHECK(list[1].kind == TA_SCXML_STATE_ATOMIC);
        CHECK(list[2].kind == TA_SCXML_STATE_FINAL);

        /* The terminator every child list needs: a zero-initialized element. */
        static const ta_scxml_state_spec zero;
        CHECK(list[3].kind == TA_SCXML_STATE_NONE);
        CHECK(memcmp(&list[3], &zero, sizeof(zero)) == 0);
    }

    ta_tree_destroy(&tree);
    check_drained(&heap, "a closed list");
}

/*
 * Catches: an inner list that writes into the outer list's storage, so a
 * nested spec comes out interleaved or short. Every importer builds this
 * shape: the transitions of a state are closed while the state list is open.
 */
static void test_lists_nest(void) {
    ta_test_allocator heap = { .strict = true };
    ta_tree tree;
    ta_tree_init(&tree, ta_test_allocator_get(&heap));

    ta_tree_list states;
    ta_tree_list_init(&states, &tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_state_spec),
                                              .element_alignment = _Alignof(ta_scxml_state_spec) });

    ta_scxml_state_spec idle = { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle") };
    ta_tree_put(&states, &idle);

    /* One state's transitions, opened and closed inside the open state list. */
    ta_tree_list transitions;
    ta_tree_list_init(&transitions, &tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_transition_spec),
                                              .element_alignment = _Alignof(ta_scxml_transition_spec) });
    ta_tree_list targets;
    ta_tree_list_init(&targets, &tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_str),
                                              .element_alignment = _Alignof(ta_str) });
    ta_str idle_target = STR("idle");
    ta_tree_put(&targets, &idle_target);
    const ta_str *target_list = ta_tree_close(&targets);

    ta_scxml_transition_spec back = {
        .kind = TA_SCXML_TRANSITION_EXTERNAL,
        .event = STR("stop"),
        .targets = target_list,
    };
    ta_tree_put(&transitions, &back);
    const ta_scxml_transition_spec *transition_list = ta_tree_close(&transitions);

    ta_scxml_state_spec busy = {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("busy"),
        .transitions = transition_list,
    };
    ta_tree_put(&states, &busy);
    const ta_scxml_state_spec *state_list = ta_tree_close(&states);

    CHECK(ta_tree_result(&tree) == TA_OK);
    CHECK(state_list != NULL && transition_list != NULL && target_list != NULL);
    if (state_list != NULL && transition_list != NULL && target_list != NULL) {
        CHECK(str_is(state_list[0].id, "idle"));
        CHECK(state_list[0].transitions == NULL);
        CHECK(str_is(state_list[1].id, "busy"));
        CHECK(state_list[1].transitions == transition_list);
        CHECK(state_list[2].kind == TA_SCXML_STATE_NONE);

        CHECK(str_is(transition_list[0].event, "stop"));
        CHECK(transition_list[0].targets == target_list);
        CHECK(transition_list[1].kind == TA_SCXML_TRANSITION_NONE);

        CHECK(str_is(target_list[0], "idle"));
        CHECK(target_list[1].ptr == NULL);
    }

    ta_tree_destroy(&tree);
    check_drained(&heap, "nested lists");
}

/* ------------------------------------------------------------------------
 * 3: ta_tree_alloc
 *
 * Catches: a bump allocator that ignores the requested alignment, or hands
 * out overlapping blocks, which an importer discovers only as corrupted
 * authored text much later.
 */

static void test_alloc_is_aligned_and_distinct(void) {
    ta_test_allocator heap = { .strict = true };
    ta_tree tree;
    ta_tree_init(&tree, ta_test_allocator_get(&heap));

    static const size_t sizes[] = { 1, 3, 64, 7, 4096 };
    static const size_t alignments[] = { 1, 8, 16, 4, 64 };
    unsigned char *blocks[5];

    for (uint32_t index = 0; index < 5; index += 1) {
        blocks[index] = ta_tree_alloc(&tree, sizes[index], alignments[index]);
        CHECK(blocks[index] != NULL);
        if (blocks[index] == NULL) {
            continue;
        }
        CHECK((uintptr_t)blocks[index] % alignments[index] == 0);
        memset(blocks[index], (int)(index + 1u), sizes[index]);
    }

    /* Nothing written later reached an earlier block. */
    for (uint32_t index = 0; index < 5; index += 1) {
        if (blocks[index] == NULL) {
            continue;
        }
        for (size_t at = 0; at < sizes[index]; at += 1) {
            CHECK(blocks[index][at] == (unsigned char)(index + 1u));
        }
    }
    CHECK(ta_tree_result(&tree) == TA_OK);

    ta_tree_destroy(&tree);
    check_drained(&heap, "tree allocations");
}

/* ------------------------------------------------------------------------
 * 4 and 5: the first allocation failure is the one that is reported
 *
 * Catches: a tree that forgets the failure once a later, smaller allocation
 * succeeds, or that keeps writing into storage it did not get — an importer
 * would then build a truncated chart and compile it as if it were whole.
 */

static void test_the_first_failure_is_retained(void) {
    ta_test_allocator heap = { .strict = true };
    ta_test_allocator_fail_from_nth(&heap, 2); /* the first allocation succeeds, then it runs dry */
    ta_tree tree;
    ta_tree_init(&tree, ta_test_allocator_get(&heap));

    ta_tree_list states;
    ta_tree_list_init(&states, &tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_state_spec),
                                              .element_alignment = _Alignof(ta_scxml_state_spec) });

    bool failed = false;
    for (uint32_t index = 0; index < 100000u && !failed; index += 1) {
        ta_scxml_state_spec state = { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s") };
        ta_tree_put(&states, &state);
        failed = ta_tree_result(&tree) != TA_OK;
    }
    CHECK(failed);
    CHECK(ta_tree_result(&tree) == TA_ERR_NOMEM);

    /* Later writes are ignored, and the retained result does not change. */
    ta_scxml_state_spec more = { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("late") };
    ta_tree_put(&states, &more);
    CHECK(ta_tree_alloc(&tree, 8, 8) == NULL);
    CHECK(ta_tree_result(&tree) == TA_ERR_NOMEM);
    (void)ta_tree_close(&states);
    CHECK(ta_tree_result(&tree) == TA_ERR_NOMEM);

    /* A failed tree still owns what it did get, and destroy returns it. */
    ta_tree_destroy(&tree);
    check_drained(&heap, "a tree that ran out of memory");
}

/* ------------------------------------------------------------------------
 * 6: a tree-built spec is the same chart as the literal one
 */

/* <state id="idle"><transition event="go" target="busy"/></state>
 * <state id="busy"><onentry><raise event="entered"/></onentry>
 *                  <transition event="stop" target="idle"/></state> */
static const ta_scxml_state_spec literal_states[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
              .event = STR("go"),
              .targets = (const ta_str[]){ STR("busy"), {0} } },
            {0},
        },
    },
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("busy"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
              .event = STR("stop"),
              .targets = (const ta_str[]){ STR("idle"), {0} } },
            {0},
        },
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("entered") } },
                  {0},
              } },
            {0},
        },
    },
    {0},
};

/* The same chart, authored through the tree. Its text lives in the tree. */
static const ta_scxml_state_spec *build_states(ta_tree *tree) {
    ta_tree_list states;
    ta_tree_list_init(&states, tree,
                      &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_state_spec),
                                              .element_alignment = _Alignof(ta_scxml_state_spec) });

    static const char *const names[] = { "idle", "busy" };
    static const char *const events[] = { "go", "stop" };

    for (uint32_t index = 0; index < 2u; index += 1) {
        /* The authored text is tree storage, not a literal: after the tree is
         * destroyed the definition may not read from it any more. */
        const char *name = names[index];
        const char *target_name = names[1u - index];
        size_t name_len = strlen(name);
        char *owned_name = ta_tree_alloc(tree, name_len, 1);
        if (owned_name != NULL) {
            memcpy(owned_name, name, name_len);
        }

        ta_tree_list targets;
        ta_tree_list_init(&targets, tree,
                          &(ta_tree_list_layout){ .element_size = sizeof(ta_str),
                                                  .element_alignment = _Alignof(ta_str) });
        ta_str target = { .ptr = target_name, .len = (uint32_t)strlen(target_name) };
        ta_tree_put(&targets, &target);

        ta_tree_list transitions;
        ta_tree_list_init(&transitions, tree,
                          &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_transition_spec),
                                                  .element_alignment = _Alignof(ta_scxml_transition_spec) });
        ta_scxml_transition_spec transition = {
            .kind = TA_SCXML_TRANSITION_EXTERNAL,
            .event = { .ptr = events[index], .len = (uint32_t)strlen(events[index]) },
            .targets = ta_tree_close(&targets),
        };
        ta_tree_put(&transitions, &transition);

        ta_scxml_state_spec state = {
            .kind = TA_SCXML_STATE_ATOMIC,
            .id = { .ptr = owned_name, .len = (uint32_t)name_len },
            .transitions = ta_tree_close(&transitions),
        };

        if (index == 1u) {
            ta_tree_list actions;
            ta_tree_list_init(&actions, tree,
                              &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_action_spec),
                                                      .element_alignment = _Alignof(ta_scxml_action_spec) });
            ta_scxml_action_spec raise = { .kind = TA_SCXML_ACTION_RAISE,
                                           .as.raise = { .event = STR("entered") } };
            ta_tree_put(&actions, &raise);

            ta_tree_list handlers;
            ta_tree_list_init(&handlers, tree,
                              &(ta_tree_list_layout){ .element_size = sizeof(ta_scxml_handler_spec),
                                                      .element_alignment = _Alignof(ta_scxml_handler_spec) });
            ta_scxml_handler_spec onentry = { .actions = ta_tree_close(&actions) };
            ta_tree_put(&handlers, &onentry);
            state.onentry = ta_tree_close(&handlers);
        }

        ta_tree_put(&states, &state);
    }

    return ta_tree_close(&states);
}

static ta_result compile_and_register(ta_system *system, ta_allocator allocator,
                                      const ta_scxml_spec *spec, ta_definition_id *out) {
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system, &(ta_definition_compile_params){ .spec = spec,
                                                 .datamodel = (ta_datamodel){ .self = NULL, .v = &model_vtable },
                                                 .allocator = allocator,
                                                 .out = &definition });
    if (result != TA_OK) {
        return result;
    }
    return ta_system_register_definition(system, definition, out);
}

/*
 * Catches: a tree whose lists are mis-terminated or mis-aligned, so the
 * compiler sees a different chart than the author wrote — the revision
 * fingerprint moves although the two specs are the same chart. Also catches a
 * compiler that keeps pointing into the tree: the authored state IDs are read
 * back after the tree that held them is gone.
 */
static void test_a_tree_built_spec_is_the_literal_chart(void) {
    ta_test_allocator heap = { .strict = true };
    ta_allocator allocator = ta_test_allocator_get(&heap);
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = allocator, .initial_time = 0 }, &system) != TA_OK) {
        fprintf(stderr, "tree test: could not create a system\n");
        failures += 1;
        return;
    }

    ta_definition_id literal = 0;
    CHECK(compile_and_register(system, allocator,
                               &(ta_scxml_spec){ .name = STR("light"), .states = literal_states },
                               &literal) == TA_OK);

    ta_test_allocator tree_heap = { .strict = true };
    ta_tree tree;
    ta_tree_init(&tree, ta_test_allocator_get(&tree_heap));
    const ta_scxml_state_spec *states = build_states(&tree);
    CHECK(ta_tree_result(&tree) == TA_OK);

    ta_definition_id built = 0;
    CHECK(compile_and_register(system, allocator,
                               &(ta_scxml_spec){ .name = STR("light"), .states = states },
                               &built) == TA_OK);

    /* The compiler consumed the tree synchronously and copied what it keeps. */
    ta_tree_destroy(&tree);
    check_drained(&tree_heap, "the tree of a compiled spec");

    ta_definition_inspection from_literal;
    ta_definition_inspection from_tree;
    CHECK(ta_definition_inspect(system, literal, &from_literal) == TA_OK);
    CHECK(ta_definition_inspect(system, built, &from_tree) == TA_OK);
    CHECK(from_tree.state_count == from_literal.state_count);
    CHECK(str_is(from_tree.identity.family.name, "light"));
    CHECK(bytes_equal(from_tree.identity.revision, from_literal.identity.revision));

    /* The authored state IDs, read after the tree that held their text. */
    bool saw_idle = false;
    bool saw_busy = false;
    for (uint32_t index = 0; index < from_tree.state_count; index += 1) {
        ta_state_info info;
        if (ta_definition_state(system, &(ta_definition_state_params){ .definition = built,
                                                                       .state = index,
                                                                       .out = &info }) != TA_OK) {
            failures += 1;
            continue;
        }
        saw_idle = saw_idle || str_is(info.id, "idle");
        saw_busy = saw_busy || str_is(info.id, "busy");
    }
    CHECK(saw_idle);
    CHECK(saw_busy);

    ta_system_destroy(system);
    check_drained(&heap, "the system of a tree-built definition");
}

int main(void) {
    test_a_closed_list_is_terminated();
    test_lists_nest();
    test_alloc_is_aligned_and_distinct();
    test_the_first_failure_is_retained();
    test_a_tree_built_spec_is_the_literal_chart();

    if (failures != 0) {
        fprintf(stderr, "tree test: %d checks failed\n", failures);
        return 1;
    }
    printf("tree test: ok\n");
    return 0;
}
