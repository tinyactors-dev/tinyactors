/*
 * No recursion in core (ADR 0060): charts nested 10,000 levels deep, in
 * states and in executable content, compile, export, register and run on a
 * thread with a 64 KiB stack. And a shortage of memory for the executor's
 * frames is TA_ERR_NOMEM before anything runs, and a retry succeeds.
 *
 * Uses the C library and pthreads; the core uses neither.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

enum { DEPTH = 10000 };

/* ------------------------------------------------------------------------
 * An allocator that can refuse large allocations
 */

static size_t refuse_from = 0; /* refuse allocations of at least this size; 0: none */

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (refuse_from != 0 && size >= refuse_from) {
        return NULL;
    }
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

static const ta_allocator_vtable allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator allocator = { .self = NULL, .v = &allocator_vtable };

/* ------------------------------------------------------------------------
 * A data model whose conditions are true and whose arrays have one item,
 * counting what it is asked
 */

static uint32_t conditions = 0;
static uint32_t bindings = 0;

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}
static void dm_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self, (void)shared, (void)unused;
}
static uint32_t dm_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t dm_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}
static ta_op_status dm_eval_condition(void *self, const ta_datamodel_eval_condition_params *params) {
    (void)self;
    conditions += 1;
    *params->out = true;
    return TA_OP_OK;
}
static ta_op_status dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    *params->count = 1;
    return TA_OP_OK;
}
static ta_op_status dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self, (void)params;
    bindings += 1;
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("null"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .foreach_snapshot = dm_foreach_snapshot,
    .foreach_bind = dm_foreach_bind,
};

/* ------------------------------------------------------------------------
 * Helpers
 */

static ta_definition_id compile_register(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                              .spec = spec,
                                                              .datamodel = { .v = &dm_vtable },
                                                              .allocator = allocator,
                                                              .out = &definition,
                                                          });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

/* Export works, and gives the chart back as deep as it was. */
static void check_export(ta_system *system, ta_definition_id id, uint32_t expected_depth) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, allocator, 1u << 16) == TA_OK);
    const ta_scxml_spec *exported = NULL;
    CHECK(ta_definition_export(system, &(ta_definition_export_params){ .definition = id, .arena = &arena,
                                                                      .out = &exported }) == TA_OK);
    uint32_t depth = 0;
    for (const ta_scxml_state_spec *states = exported != NULL ? exported->states : NULL; states != NULL;
         states = states[0].states) {
        depth += 1u;
    }
    CHECK(depth == expected_depth);
    ta_arena_destroy(&arena);
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

static ta_result resume(ta_system *system, ta_actor_id actor, ta_run_status *status) {
    return ta_actor_resume(system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = status });
}

/* ------------------------------------------------------------------------
 * 10,000 levels of states
 */

static void deep_states(void) {
    /* levels[d] is the list of states at depth d: one compound state, or at
     * the bottom two atomic states, `a` going to `b` on `go`. */
    ta_scxml_state_spec (*levels)[3] = calloc(DEPTH, sizeof(*levels));
    char (*ids)[8] = calloc(DEPTH, sizeof(*ids));
    CHECK(levels != NULL && ids != NULL);
    if (levels == NULL || ids == NULL) {
        free(levels);
        free(ids);
        return;
    }
    static const ta_str to_b[] = { STR("b"), {0} };
    static ta_scxml_transition_spec go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"),
                                               .targets = to_b },
                                             {0} };
    for (uint32_t depth = 0; depth + 1u < DEPTH; depth += 1u) {
        int length = snprintf(ids[depth], sizeof(ids[depth]), "s%u", depth);
        levels[depth][0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_COMPOUND,
                                                  .id = { .ptr = ids[depth], .len = (uint32_t)length },
                                                  .states = levels[depth + 1u] };
    }
    levels[DEPTH - 1u][0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = go };
    levels[DEPTH - 1u][1] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b") };
    ta_scxml_spec spec = { .name = STR("deep_states"), .states = levels[0] };

    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition_id id = compile_register(system, &spec);
    check_export(system, id, DEPTH);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(resume(system, actor, &status) == TA_OK && status == TA_RUN_STABLE);
    ta_event event = { .name = event_name(system, "go"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, actor, &event) == TA_OK);
    CHECK(resume(system, actor, &status) == TA_OK && status == TA_RUN_STABLE);
    ta_state_id b = 0;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = id, .id = STR("b"),
                                                                              .out = &b }) == TA_OK);
    /* Every level is active, ancestors first, and the bottom one is b. */
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .count = &count }) ==
          TA_ERR_NOMEM);
    ta_state_id *active = calloc(count, sizeof(ta_state_id));
    CHECK(active != NULL);
    if (active != NULL) {
        CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                              .capacity = count,
                                                                              .count = &count }) == TA_OK);
        CHECK(count == DEPTH && active[count - 1u] == b);
    }
    free(active);
    ta_system_destroy(system);
    free(ids);
    free(levels);
}

/* ------------------------------------------------------------------------
 * 10,000 levels of <if> and <foreach>
 */

static ta_scxml_action_spec (*deep_blocks(void))[2] {
    /* blocks[d] is the block at depth d: an <if> at even depths, a <foreach>
     * at odd ones, each holding the next block. */
    ta_scxml_action_spec (*blocks)[2] = calloc(DEPTH + 1u, sizeof(*blocks));
    if (blocks == NULL) {
        return NULL;
    }
    for (uint32_t depth = 0; depth < DEPTH; depth += 1u) {
        if (depth % 2u == 0) {
            blocks[depth][0] = (ta_scxml_action_spec){
                .kind = TA_SCXML_ACTION_IF,
                .as.conditional = { .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = STR("c") },
                                    .then_actions = blocks[depth + 1u] },
            };
        } else {
            blocks[depth][0] = (ta_scxml_action_spec){
                .kind = TA_SCXML_ACTION_FOREACH,
                .as.foreach = { .array = { .kind = TA_EXPRESSION_SOURCE, .as.source = STR("xs") },
                                .item = STR("x"), .body = blocks[depth + 1u] },
            };
        }
    }
    return blocks;
}

static void deep_actions(void) {
    ta_scxml_action_spec (*blocks)[2] = deep_blocks();
    CHECK(blocks != NULL);
    if (blocks == NULL) {
        return;
    }
    ta_scxml_handler_spec onentry[] = { { .actions = blocks[0] }, {0} };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s"), .onentry = onentry }, {0} };
    ta_scxml_spec spec = { .name = STR("deep_actions"), .states = states };

    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition_id id = compile_register(system, &spec);
    check_export(system, id, 1);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    conditions = bindings = 0;
    ta_run_status status = TA_RUN_ERROR;
    CHECK(resume(system, actor, &status) == TA_OK && status == TA_RUN_STABLE);
    CHECK(conditions == DEPTH / 2u);
    CHECK(bindings == DEPTH / 2u);
    ta_system_destroy(system);
    free(blocks);
}

/*
 * The executor's frames come from the interpreter's scratch when it starts.
 * Without memory for them the resume is TA_ERR_NOMEM before any action
 * runs, and a retry once memory is back runs the whole block once.
 */
static void frames_out_of_memory(void) {
    ta_scxml_action_spec (*blocks)[2] = deep_blocks();
    CHECK(blocks != NULL);
    if (blocks == NULL) {
        return;
    }
    ta_scxml_handler_spec onentry[] = { { .actions = blocks[0] }, {0} };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s"), .onentry = onentry }, {0} };
    ta_scxml_spec spec = { .name = STR("deep_actions"), .states = states };

    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_definition_id id = compile_register(system, &spec);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    conditions = bindings = 0;
    ta_run_status status = TA_RUN_ERROR;
    refuse_from = 64u * 1024u; /* the frames for 10,000 levels are far larger */
    CHECK(resume(system, actor, &status) == TA_ERR_NOMEM);
    CHECK(conditions == 0 && bindings == 0);
    refuse_from = 0;
    CHECK(resume(system, actor, &status) == TA_OK && status == TA_RUN_STABLE);
    CHECK(conditions == DEPTH / 2u);
    CHECK(bindings == DEPTH / 2u);
    ta_system_destroy(system);
    free(blocks);
}

static void *run_all(void *unused) {
    (void)unused;
    deep_states();
    deep_actions();
    frames_out_of_memory();
    return NULL;
}

int main(void) {
    pthread_attr_t attributes;
    pthread_t thread;
    CHECK(pthread_attr_init(&attributes) == 0);
    CHECK(pthread_attr_setstacksize(&attributes, 64u * 1024u) == 0);
    CHECK(pthread_create(&thread, &attributes, run_all, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    pthread_attr_destroy(&attributes);
    if (failures != 0) {
        fprintf(stderr, "deep test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core deep test passed: 10,000 levels of states and of actions on a 64 KiB stack");
    return 0;
}
