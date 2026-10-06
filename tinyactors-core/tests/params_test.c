/*
 * System parameters (ADR 0092): set once at creation and checked there;
 * buffers every run needs are allocated at creation, nested interpreter
 * regions on first use and then kept; content-sized temporaries that do not
 * fit the render area are an execution error; quotas bound what grows.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "test_drive.h"

static int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define SOURCE(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })

/* ------------------------------------------------------------------------
 * A test allocator: how many allocations, how many at least `large`
 * bytes, and one it can refuse
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * A data model whose conditions are true, whose values are empty, and
 * whose params make an empty value
 */

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
    *params->out = true;
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    return TA_OP_OK;
}
static ta_op_status dm_make_params(void *self, const ta_datamodel_make_params_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("params"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .eval_value = dm_eval_value,
    .make_params = dm_make_params,
};

/* ------------------------------------------------------------------------
 * Helpers
 */

static ta_system *create(const ta_param_value *params, uint32_t count) {
    ta_system *system = NULL;
    ta_result result = ta_system_create(
        &(ta_system_config){ .allocator = allocator, .params = params, .param_count = count }, &system);
    return result == TA_OK ? system : NULL;
}

static ta_definition_id define(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    ta_definition_id id = 0;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                             .datamodel = { .v = &dm_vtable },
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    CHECK(definition != NULL && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static ta_actor_id spawn(ta_system *system, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    return actor;
}

static test_drive_status resume(ta_system *system, ta_actor_id actor) {
    test_drive_status status = TEST_DRIVE_ERROR;
    ta_result result = test_drive_run(system, actor, &(test_drive_run_params){ .budget = 1000, .status = &status });
    CHECK(result == TA_OK);
    return status;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

static void deliver(ta_system *system, ta_actor_id actor, const char *name) {
    ta_event event = { .name = event_name(system, name), .type = TA_EVENT_EXTERNAL };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .name = test_event_text(system, &event), .event = &event }) == TA_OK);
}

static bool in_state(ta_system *system, ta_actor_id actor, const char *id) {
    ta_state_id active[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                          .capacity = 8, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1) {
        ta_state_info info = {0};
        ta_actor_inspection inspection = {0};
        CHECK(ta_actor_inspect(system, actor, &inspection) == TA_OK);
        CHECK(ta_definition_state(system, &(ta_definition_state_params){ .definition = inspection.definition_id,
                                                                         .state = active[index],
                                                                         .out = &info }) == TA_OK);
        if (info.id.len == strlen(id) && memcmp(info.id.ptr, id, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

static uint32_t pending_sends(ta_system *system, ta_actor_id actor) {
    ta_actor_inspection inspection = {0};
    CHECK(ta_actor_inspect(system, actor, &inspection) == TA_OK);
    return inspection.delayed_send_count;
}

/* ------------------------------------------------------------------------
 * Validation, read-back and the info table
 */

static void test_validation(void) {
    ta_system *system = create(NULL, 0);
    CHECK(system != NULL);
    for (uint32_t param = 0; param < (uint32_t)TA_PARAM_COUNT; param += 1u) {
        ta_param_info info = {0};
        uint64_t value = 0;
        CHECK(ta_param_info_of((ta_param)param, &info) == TA_OK);
        CHECK(info.name.len != 0 && info.unit.len != 0);
        CHECK(info.name.ptr[0] >= 'a' && info.name.ptr[0] <= 'z'); /* lowerCamelCase */
        CHECK(info.min <= info.default_value && info.default_value <= info.max);
        CHECK(ta_system_param(system, (ta_param)param, &value) == TA_OK && value == info.default_value);
        for (uint32_t other = 0; other < param; other += 1u) {
            ta_param_info earlier = {0};
            CHECK(ta_param_info_of((ta_param)other, &earlier) == TA_OK);
            CHECK(earlier.name.len != info.name.len || memcmp(earlier.name.ptr, info.name.ptr, info.name.len) != 0);
        }
    }
    ta_param_info info = {0};
    CHECK(ta_param_info_of(TA_PARAM_COUNT, &info) == TA_ERR_INVALID);
    CHECK(ta_param_info_of(TA_PARAM_INTERPRETER_BYTES, &info) == TA_OK);
    CHECK(info.name.len == strlen("interpreterBytes") && memcmp(info.name.ptr, "interpreterBytes", info.name.len) == 0);
    CHECK(info.unit.len == 5 && memcmp(info.unit.ptr, "bytes", 5) == 0);
    CHECK(info.default_value == TA_PARAM_DEFAULT_INTERPRETER_BYTES);
    uint64_t value = 0;
    CHECK(ta_system_param(system, TA_PARAM_COUNT, &value) == TA_ERR_INVALID);
    ta_system_destroy(system);

    ta_param_value set[] = { { TA_PARAM_MICROSTEP_LIMIT, 7 }, { TA_PARAM_RENDER_BYTES, 4096 } };
    system = create(set, 2);
    CHECK(system != NULL);
    CHECK(ta_system_param(system, TA_PARAM_MICROSTEP_LIMIT, &value) == TA_OK && value == 7);
    CHECK(ta_system_param(system, TA_PARAM_RENDER_BYTES, &value) == TA_OK && value == 4096);
    ta_system_destroy(system);

    ta_param rejected = TA_PARAM_COUNT;
    ta_system *none = NULL;
    ta_param_value low[] = { { TA_PARAM_INTERPRETER_BYTES, 10 } };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = low, .param_count = 1,
                                                .rejected = &rejected },
                           &none) == TA_ERR_INVALID);
    CHECK(none == NULL && rejected == TA_PARAM_INTERPRETER_BYTES);
    ta_param_value twice[] = { { TA_PARAM_MAX_ACTORS, 5 }, { TA_PARAM_EVENT_NAME_DEPTH, 8 }, { TA_PARAM_MAX_ACTORS, 6 } };
    rejected = TA_PARAM_COUNT;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = twice, .param_count = 3,
                                                .rejected = &rejected },
                           &none) == TA_ERR_INVALID);
    CHECK(rejected == TA_PARAM_MAX_ACTORS);
    ta_param_value deep[] = { { TA_PARAM_EVENT_NAME_DEPTH, 65 } };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = deep, .param_count = 1 },
                           &none) == TA_ERR_INVALID);
    ta_param_value unknown[] = { { TA_PARAM_COUNT, 1 } };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = unknown, .param_count = 1 },
                           &none) == TA_ERR_INVALID);
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = NULL, .param_count = 1 },
                           &none) == TA_ERR_INVALID);
}

/* Creation allocates what every run needs; each allocation it makes can fail
 * and leaves nothing behind. */
static void test_creation_out_of_memory(void) {
    unsigned long before = heap.attempts;
    ta_system *system = create(NULL, 0);
    CHECK(system != NULL);
    unsigned long used = heap.attempts - before;
    ta_system_destroy(system);
    CHECK(used > 0);
    for (unsigned long failing = 1; failing <= used; failing += 1u) {
        unsigned long start_allocations = heap.attempts;
        unsigned long start_frees = heap.frees;
        heap.policy.fail_at = heap.attempts + failing;
        ta_system *failed = create(NULL, 0);
        heap.policy.fail_at = 0;
        CHECK(failed == NULL);
        /* every allocation that succeeded was given back; the refused one never existed */
        CHECK(heap.attempts - start_allocations - 1u == heap.frees - start_frees);
    }
}

/* ------------------------------------------------------------------------
 * Charts
 */

/* a --go--> b --go--> a */
static const ta_str to_b[] = { STR("b"), {0} };
static const ta_str to_a[] = { STR("a"), {0} };
static const ta_scxml_transition_spec a_go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"),
                                                   .targets = to_b },
                                                 {0} };
static const ta_scxml_transition_spec b_go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"),
                                                   .targets = to_a },
                                                 {0} };
static const ta_scxml_state_spec toggle_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = a_go },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = b_go },
    {0},
};
static const ta_scxml_spec toggle = { .name = STR("toggle"), .states = toggle_states };

/* ------------------------------------------------------------------------
 * An ordinary run allocates nothing
 */

static void test_ordinary_run_allocates_nothing(void) {
    ta_system *system = create(NULL, 0);
    CHECK(system != NULL);
    ta_definition_id id = define(system, &toggle);
    ta_actor_id actor = spawn(system, id);
    CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    (void)event_name(system, "go"); /* interned once, outside the measure */
    /* The first event's text tops the name area's reserve up, once
     * (ADR 0099), and sizes the run's name buffer, once (ADR 0100):
     * measured apart, at most one page of the area and the buffer. */
    unsigned long warm = heap.attempts;
    deliver(system, actor, "go");
    CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    CHECK(heap.attempts - warm <= 2u);
    unsigned long before = heap.attempts;
    for (int round = 0; round < 4; round += 1) {
        deliver(system, actor, "go");
        CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    }
    CHECK(heap.attempts == before);
    CHECK(in_state(system, actor, "b"));
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Taking an input allocates nothing once the pool is warm (ADR 0069, ADR
 * 0099): the taken event stays in its envelope, which the pool already
 * holds, so however many sessions hold an input at once, nothing is
 * allocated for them, and creation makes nothing for events either.
 */

enum { HELD_ACTORS = 5 };

/* Delivers "go" to the first `count` actors, then takes it in each with a
 * budget of one microstep, where its macrostep pauses holding its input
 * (the session filter lets each pause beside the others): `count` actors
 * hold an input at once. Then each finishes. */
static void go_round(ta_system *system, const ta_actor_id *actors, uint32_t count) {
    for (uint32_t index = 0; index < count; index += 1u) {
        deliver(system, actors[index], "go");
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(test_drive_run(system, actors[index], &(test_drive_run_params){ .budget = 1, .status = &status }) ==
              TA_OK);
        CHECK(status == TEST_DRIVE_PAUSED);
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        CHECK(resume(system, actors[index]) == TEST_DRIVE_STABLE);
    }
}

static void test_taking_allocates_nothing(void) {
    ta_system *system = create(NULL, 0);
    CHECK(system != NULL);
    ta_definition_id id = define(system, &toggle);
    ta_actor_id actors[HELD_ACTORS];
    for (uint32_t index = 0; index < HELD_ACTORS; index += 1u) {
        actors[index] = spawn(system, id);
        CHECK(resume(system, actors[index]) == TEST_DRIVE_STABLE);
    }
    (void)event_name(system, "go");
    /* Warm-up: the first round's text tops the name area's reserve up. */
    go_round(system, actors, HELD_ACTORS);

    /* One session, two, or all of them holding an input at once: none of
     * it allocates, and nothing is freed. */
    unsigned long before = heap.attempts;
    unsigned long frees_before = heap.frees;
    go_round(system, actors, 1);
    go_round(system, actors, 2);
    go_round(system, actors, HELD_ACTORS);
    CHECK(heap.attempts == before);
    CHECK(heap.frees == frees_before);

    /* With the allocator refusing everything, a warm system still takes
     * an input and runs it to its end. */
    deliver(system, actors[0], "go");
    bool was_a = in_state(system, actors[0], "a");
    heap.policy.refuse = true;
    test_drive_status status = TEST_DRIVE_ERROR;
    ta_result result = test_drive_run(system, actors[0], &(test_drive_run_params){ .budget = 1000, .status = &status });
    heap.policy.refuse = false;
    CHECK(result == TA_OK && status == TEST_DRIVE_STABLE);
    CHECK(in_state(system, actors[0], was_a ? "b" : "a"));
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * The render area
 */

enum { MANY_PARAMS = 40 };

static void test_render_overflow(void) {
    static ta_scxml_param_spec params[MANY_PARAMS + 1];
    static char names[MANY_PARAMS][8];
    for (uint32_t index = 0; index < MANY_PARAMS; index += 1u) {
        int length = snprintf(names[index], sizeof names[index], "p%u", index);
        params[index] = (ta_scxml_param_spec){ .name = { .ptr = names[index], .len = (uint32_t)length },
                                               .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SOURCE("v") } };
    }
    params[MANY_PARAMS] = (ta_scxml_param_spec){0};
    ta_scxml_action_spec send[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("later") },
                       .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("10s") },
                       .params = params } },
        {0},
    };
    ta_scxml_handler_spec onentry[] = { { .actions = send }, {0} };
    static const ta_str to_failed[] = { STR("failed"), {0} };
    ta_scxml_transition_spec failing[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"),
                                             .targets = to_failed },
                                           {0} };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .onentry = onentry, .transitions = failing },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
        {0},
    };
    ta_scxml_spec spec = { .name = STR("many_params"), .states = states };

    /* With room, the send is made and waits. */
    ta_system *system = create(NULL, 0);
    CHECK(system != NULL);
    ta_actor_id actor = spawn(system, define(system, &spec));
    CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    CHECK(in_state(system, actor, "a") && pending_sends(system, actor) == 1);
    ta_system_destroy(system);

    /* Without, the same content fails the same way each time. */
    ta_param_value small[] = { { TA_PARAM_RENDER_BYTES, 256 } };
    for (int round = 0; round < 2; round += 1) {
        system = create(small, 1);
        CHECK(system != NULL);
        actor = spawn(system, define(system, &spec));
        CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
        CHECK(in_state(system, actor, "failed") && pending_sends(system, actor) == 0);
        ta_system_destroy(system);
    }
}

/* ------------------------------------------------------------------------
 * Event name depth
 */

static void test_event_name_depth(void) {
    ta_param_value shallow[] = { { TA_PARAM_EVENT_NAME_DEPTH, 8 } };
    ta_system *system = create(shallow, 1);
    CHECK(system != NULL);
    ta_atom segment = 0;
    CHECK(ta_system_intern_atom(system, STR("s"), &segment) == TA_OK);
    ta_atom segments[9] = { segment, segment, segment, segment, segment, segment, segment, segment, segment };
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = segments, .count = 8 }, &name) == TA_OK);
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = segments, .count = 9 }, &name) == TA_ERR_INVALID);
    ta_system_destroy(system);
    system = create(NULL, 0);
    CHECK(system != NULL);
    CHECK(ta_system_intern_atom(system, STR("s"), &segment) == TA_OK);
    for (uint32_t index = 0; index < 9; index += 1u) {
        segments[index] = segment;
    }
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = segments, .count = 9 }, &name) == TA_OK);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Nested interpreter regions: a parent invokes a child that invokes a
 * grandchild; cancelling exits both, each in a nested region
 */

static ta_definition_id child_definition = 0;
static ta_definition_id grandchild_definition = 0;
static ta_actor_id spawned[8];
static uint32_t spawned_count = 0;
static ta_result last_spawn = TA_OK;

static ta_op_status svc_start(void *self, const ta_invoker_start_params *params) {
    ta_definition_id definition = self == NULL ? child_definition : grandchild_definition;
    ta_actor_id child = 0;
    last_spawn = ta_actor_spawn(params->request->system,
                                &(ta_actor_spawn_params){
                                    .definition = definition,
                                    .parent = &(ta_actor_parent){ .actor = params->request->parent,
                                                                  .invoke_id = params->request->invoke_id },
                                    .out = &child });
    if (last_spawn != TA_OK) {
        static const char text[] = "no child";
        *params->fault = (ta_fault){ .code = last_spawn, .message = { .ptr = text, .len = sizeof text - 1u } };
        return TA_OP_ERROR;
    }
    spawned[spawned_count++ % 8u] = child;
    *params->service_handle = child;
    return TA_OP_OK;
}
static ta_op_status svc_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self, (void)service, (void)fault;
    return TA_OP_OK;
}
static ta_op_status svc_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static const ta_invoker_vtable svc_vtable = { .start = svc_start, .cancel = svc_cancel, .forward = svc_forward };
static char grand_marker;

static void resume_spawned(ta_system *system, uint32_t from) {
    for (uint32_t index = from; index < spawned_count; index += 1u) {
        (void)resume(system, spawned[index % 8u]);
    }
}

static ta_system *nesting_system(uint32_t nesting, ta_definition_id *parent) {
    ta_param_value params[] = { { TA_PARAM_INTERPRETER_NESTING, nesting } };
    ta_system *system = create(params, 1);
    CHECK(system != NULL);
    ta_atom child_type = 0;
    ta_atom grand_type = 0;
    CHECK(ta_system_intern_atom(system, STR("child"), &child_type) == TA_OK);
    CHECK(ta_system_intern_atom(system, STR("grand"), &grand_type) == TA_OK);
    CHECK(ta_system_register_invoker(system, child_type, (ta_invoker){ .self = NULL, .v = &svc_vtable }) == TA_OK);
    CHECK(ta_system_register_invoker(system, grand_type, (ta_invoker){ .self = &grand_marker, .v = &svc_vtable }) ==
          TA_OK);

    static const ta_scxml_invoke_spec grand_invoke[] = {
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("grand") } }, {0}
    };
    static const ta_scxml_state_spec child_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c"), .invokes = grand_invoke }, {0}
    };
    static const ta_scxml_spec child = { .name = STR("child"), .states = child_states };
    static const ta_scxml_state_spec grand_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("g") }, {0} };
    static const ta_scxml_spec grand = { .name = STR("grand"), .states = grand_states };
    static const ta_scxml_invoke_spec child_invoke[] = {
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("child") } }, {0}
    };
    static const ta_str stop_to[] = { STR("idle"), {0} };
    static const ta_str start_to[] = { STR("working"), {0} };
    static const ta_scxml_transition_spec working_go[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = stop_to }, {0}
    };
    static const ta_scxml_transition_spec idle_go[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("start"), .targets = start_to }, {0}
    };
    static const ta_scxml_state_spec parent_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"), .invokes = child_invoke, .transitions = working_go },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle_go },
        {0},
    };
    static const ta_scxml_spec parent_spec = { .name = STR("parent"), .states = parent_states };
    child_definition = define(system, &child);
    grandchild_definition = define(system, &grand);
    *parent = define(system, &parent_spec);
    return system;
}

static void test_nested_regions(void) {
    ta_definition_id parent_definition = 0;
    ta_system *system = nesting_system(3, &parent_definition);
    uint64_t interpreter_bytes = 0;
    CHECK(ta_system_param(system, TA_PARAM_INTERPRETER_BYTES, &interpreter_bytes) == TA_OK);
    heap.policy.large = (size_t)interpreter_bytes;
    spawned_count = 0;
    /* The parent's first take, a safe point, allocates regions 1 and 2 and
     * the frames that exit sessions in them, once (ADR 0102); a cancel
     * inside a macrostep allocates none. */
    unsigned long before = heap.large_allocations;
    ta_actor_id parent = spawn(system, parent_definition);
    CHECK(resume(system, parent) == TEST_DRIVE_STABLE); /* starts the child */
    CHECK(heap.large_allocations - before == 3u);
    resume_spawned(system, 0);                      /* the child starts the grandchild */
    resume_spawned(system, 1);
    CHECK(spawned_count == 2 && last_spawn == TA_OK);

    /* The first cancel exits the child in region 1 and the grandchild in
     * region 2, which are there already. */
    before = heap.large_allocations;
    deliver(system, parent, "stop");
    CHECK(resume(system, parent) == TEST_DRIVE_STABLE);
    CHECK(heap.large_allocations == before);

    /* The second finds both regions kept. */
    uint32_t mark = spawned_count;
    deliver(system, parent, "start");
    CHECK(resume(system, parent) == TEST_DRIVE_STABLE);
    resume_spawned(system, mark);
    resume_spawned(system, mark + 1u);
    before = heap.large_allocations;
    deliver(system, parent, "stop");
    CHECK(resume(system, parent) == TEST_DRIVE_STABLE);
    CHECK(heap.large_allocations == before);
    heap.policy.large = 0;
    ta_system_destroy(system);

    /* With two interpreters, a grandchild would need a third: its spawn is
     * refused, and nothing is allocated for it. */
    system = nesting_system(2, &parent_definition);
    spawned_count = 0;
    parent = spawn(system, parent_definition);
    CHECK(resume(system, parent) == TEST_DRIVE_STABLE);
    resume_spawned(system, 0);
    CHECK(spawned_count == 1 && last_spawn == TA_ERR_UNSUPPORTED);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Quotas
 */

static void test_quotas(void) {
    ta_param_value two[] = { { TA_PARAM_MAX_ACTORS, 2 }, { TA_PARAM_MAX_DEFINITIONS, 1 } };
    ta_system *system = create(two, 2);
    CHECK(system != NULL);
    ta_definition_id id = define(system, &toggle);
    ta_actor_id first = spawn(system, id);
    (void)spawn(system, id);
    ta_actor_id third = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &third }) == TA_ERR_NOMEM);
    ta_counters counters = {0};
    ta_system_counters(system, &counters);
    CHECK(counters.quota_refusals == 1u);
    CHECK(ta_actor_destroy(system, first) == TA_OK);
    (void)spawn(system, id);

    static const ta_scxml_state_spec other_states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("x") }, {0} };
    static const ta_scxml_spec other = { .name = STR("other"), .states = other_states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &other,
                                                                             .datamodel = { .v = &dm_vtable },
                                                                             .allocator = allocator,
                                                                             .out = &definition }) == TA_OK);
    ta_definition_id other_id = 0;
    CHECK(ta_system_register_definition(system, definition, &other_id) == TA_ERR_NOMEM);
    ta_definition_destroy(definition);
    ta_system_counters(system, &counters);
    CHECK(counters.quota_refusals == 2u);
    ta_system_destroy(system);

    /* maxPendingSends: the second delayed send is an execution error. */
    ta_scxml_action_spec sends[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("one") },
                       .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("10s") } } },
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("two") },
                       .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("10s") } } },
        {0},
    };
    ta_scxml_handler_spec onentry[] = { { .actions = sends }, {0} };
    static const ta_str to_failed[] = { STR("failed"), {0} };
    ta_scxml_transition_spec failing[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"),
                                             .targets = to_failed },
                                           {0} };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .onentry = onentry, .transitions = failing },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
        {0},
    };
    ta_scxml_spec spec = { .name = STR("two_sends"), .states = states };
    ta_param_value one[] = { { TA_PARAM_MAX_PENDING_SENDS, 1 } };
    system = create(one, 1);
    CHECK(system != NULL);
    ta_actor_id actor = spawn(system, define(system, &spec));
    CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    CHECK(in_state(system, actor, "failed") && pending_sends(system, actor) == 1);
    ta_system_counters(system, &counters);
    CHECK(counters.quota_refusals == 1u);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * The need the compiler reports is exact: a system with exactly that many
 * interpreterBytes runs the chart, every trace kind included
 */

enum { REGIONS = 3, PER_REGION = 20 };

static void test_scratch_need_exact(void) {
    static ta_scxml_state_spec leaves[REGIONS][PER_REGION + 2];
    static ta_scxml_state_spec regions[REGIONS + 1];
    static char ids[REGIONS][PER_REGION + 1][12];
    static ta_scxml_transition_spec steps[REGIONS][PER_REGION][2];
    static ta_str targets[REGIONS][PER_REGION][2];
    for (uint32_t region = 0; region < REGIONS; region += 1u) {
        for (uint32_t leaf = 0; leaf < PER_REGION; leaf += 1u) {
            int length = snprintf(ids[region][leaf], sizeof ids[region][leaf], "r%us%u", region, leaf);
            uint32_t next = (leaf + 1u) % PER_REGION;
            int next_length = snprintf(ids[region][PER_REGION], sizeof ids[region][PER_REGION], "r%us%u", region, next);
            (void)next_length;
            targets[region][leaf][0] = (ta_str){ .ptr = ids[region][next], .len = 0 };
            targets[region][leaf][1] = (ta_str){0};
            steps[region][leaf][0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                                 .event = STR("go"), .condition = SOURCE("c"),
                                                                 .targets = targets[region][leaf] };
            steps[region][leaf][1] = (ta_scxml_transition_spec){0};
            leaves[region][leaf] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC,
                                                          .id = { .ptr = ids[region][leaf], .len = (uint32_t)length },
                                                          .transitions = steps[region][leaf] };
        }
        static ta_str defaults[REGIONS][2];
        static ta_scxml_transition_spec default_step[REGIONS][2];
        defaults[region][0] = (ta_str){ .ptr = ids[region][0], .len = (uint32_t)strlen(ids[region][0]) };
        default_step[region][0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                              .targets = defaults[region] };
        leaves[region][PER_REGION] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_HISTORY_DEEP,
                                                            .transitions = default_step[region] };
        leaves[region][PER_REGION + 1] = (ta_scxml_state_spec){0};
    }
    for (uint32_t region = 0; region < REGIONS; region += 1u) {
        for (uint32_t leaf = 0; leaf < PER_REGION; leaf += 1u) {
            uint32_t next = (leaf + 1u) % PER_REGION;
            targets[region][leaf][0].len = (uint32_t)strlen(ids[region][next]);
        }
    }
    static char region_ids[REGIONS][8];
    static char history_ids[REGIONS][8];
    for (uint32_t region = 0; region < REGIONS; region += 1u) {
        int length = snprintf(region_ids[region], sizeof region_ids[region], "r%u", region);
        int history_length = snprintf(history_ids[region], sizeof history_ids[region], "h%u", region);
        leaves[region][PER_REGION].id = (ta_str){ .ptr = history_ids[region], .len = (uint32_t)history_length };
        regions[region] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_COMPOUND,
                                                 .id = { .ptr = region_ids[region], .len = (uint32_t)length },
                                                 .states = leaves[region] };
    }
    regions[REGIONS] = (ta_scxml_state_spec){0};
    ta_scxml_state_spec top[] = { { .kind = TA_SCXML_STATE_PARALLEL, .id = STR("p"), .states = regions }, {0} };
    ta_scxml_spec spec = { .name = STR("wide"), .states = top };

    ta_param_value tiny[] = { { TA_PARAM_INTERPRETER_BYTES, 1024 } };
    ta_system *system = create(tiny, 1);
    CHECK(system != NULL);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, allocator, 1024) == TA_OK);
    ta_definition *definition = NULL;
    ta_compile_error error = {0};
    ta_result compiled = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &spec, .datamodel = { .v = &dm_vtable },
                                                 .allocator = allocator, .out = &definition,
                                                 .error = &error, .error_arena = &arena });
    if (compiled != TA_ERR_UNSUPPORTED) {
        fprintf(stderr, "wide: %d, code %d, %.*s\n", (int)compiled, (int)error.code, (int)error.message.len,
                error.message.ptr);
    }
    CHECK(compiled == TA_ERR_UNSUPPORTED);
    CHECK(error.code == TA_COMPILE_ERROR_LIMIT && error.path_length == 0);
    uint64_t need = 0;
    static const char prefix[] = "the chart needs ";
    CHECK(error.message.len > sizeof prefix && memcmp(error.message.ptr, prefix, sizeof prefix - 1u) == 0);
    for (const char *digit = error.message.ptr + sizeof prefix - 1u; *digit >= '0' && *digit <= '9'; digit += 1) {
        need = need * 10u + (uint64_t)(*digit - '0');
    }
    CHECK(need > 1024u);
    ta_arena_destroy(&arena);
    ta_system_destroy(system);

    ta_param_value exact[] = { { TA_PARAM_INTERPRETER_BYTES, need } };
    system = create(exact, 1);
    CHECK(system != NULL);
    CHECK(ta_system_set_trace_masks(system, &(ta_trace_masks){ .kinds = UINT32_MAX,
                                                               .watched_kinds = UINT32_MAX,
                                                               .unsampled_kinds = UINT32_MAX }) == TA_OK);
    ta_actor_id actor = spawn(system, define(system, &spec));
    CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    for (int round = 0; round < 3; round += 1) {
        deliver(system, actor, "go");
        CHECK(resume(system, actor) == TEST_DRIVE_STABLE);
    }
    CHECK(in_state(system, actor, "r0s3"));
    ta_system_destroy(system);
}

int main(void) {
    test_validation();
    test_creation_out_of_memory();
    test_ordinary_run_allocates_nothing();
    test_taking_allocates_nothing();
    test_render_overflow();
    test_event_name_depth();
    test_nested_regions();
    test_quotas();
    test_scratch_need_exact();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "params test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("tinyactors-core params test passed\n");
    return 0;
}
