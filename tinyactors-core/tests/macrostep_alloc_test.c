/*
 * A macrostep never calls the allocator (ADR 0102): an allocator that
 * refuses every request made while a macrostep runs, and counts every free
 * made then, sees neither, from the first macrostep of a fresh system on. Safe points (spawn, admission,
 * the refill between inputs, setting the time) may allocate.
 *
 * 1. a session's bootstrap, a chain of raised internal events, sends to
 *    the session itself and to #_internal, and a delayed send cancelled in
 *    the same macrostep make no request inside a macrostep;
 * 2. delayed sends that stay pending and fire later make none either;
 * 3. entering and leaving a state with an <invoke> of a host service, the
 *    invocation started and cancelled each time, makes none either;
 * 4. a <raise> beyond `internalQueueEvents` in one macrostep fails as
 *    error.execution.capacity.internal, and the chart goes on, still
 *    without a request inside a macrostep;
 * 5. a macrostep a budget pauses, and continues on later calls, makes
 *    none: its pause storage is the slot's, sized when the input was taken;
 * 6. a session that reaches a top-level final state makes none, and keeps
 *    what a finished session keeps;
 * 7. a local child that a host invoker spawns when its parent's state is
 *    entered, and destroys when it is cancelled, makes none inside the
 *    parent's macrostep: startup runs after the macrostep, and what a
 *    destroyed session held is given back after its input completes.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.c"
#include "test_allocator.h"
#include "test_drive.h"

static int failures = 0;

#define CHECK(condition)                                                                   \
    do {                                                                                   \
        if (!(condition)) {                                                                \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1;                                                                 \
        }                                                                                  \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define LIT(text) { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(text) }

static test_allocator heap;
static ta_system *watched;
static unsigned long inside;

/* Counts and refuses what is asked while `watched` runs a macrostep; counts
 * what is freed then, too. */
static void *watch_alloc(void *self, size_t size, size_t alignment) {
    if (watched != NULL && watched->run.running) {
        inside += 1u;
        return NULL;
    }
    return test_allocator_vtable.alloc(self, size, alignment);
}

static void watch_free(void *self, const ta_allocator_free_params *params) {
    if (watched != NULL && watched->run.running) {
        inside += 1u; /* nothing is given back inside a macrostep either */
    }
    test_allocator_vtable.free(self, params);
}

static const ta_allocator_vtable watch_vtable = { .alloc = watch_alloc, .free = watch_free };

/*
 * `idle --go--> idle`: raises `a`, which raises `b`; sends `self` to
 * itself and `inner` to #_internal; schedules `later` a second ahead and
 * cancels it; and schedules `kept`, which fires on a later time move.
 */
static const ta_scxml_spec *busy_chart(void) {
    static const ta_scxml_action_spec on_go[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("a") } },
        { .kind = TA_SCXML_ACTION_SEND, .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("self") } } },
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("inner") },
                       .target = LIT("#_internal") } },
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("later") },
                       .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("later") }, .delay = LIT("1s") } },
        { .kind = TA_SCXML_ACTION_CANCEL, .as.cancel.sendid = LIT("later") },
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("kept") }, .delay = LIT("1s") } },
        {0},
    };
    static const ta_scxml_action_spec on_a[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("b") } },
        {0},
    };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("go"), .actions = on_go },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("a"), .actions = on_a },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("b self inner kept") },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("busy"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Runs everything queued until core is idle. */
static void drain(ta_system *system) {
    ta_run_status status = TA_RUN_MORE;
    while (status != TA_RUN_IDLE) {
        ta_input_outcome outcomes[8];
        uint32_t count = 0;
        CHECK(ta_run(system, &(ta_run_params){ .budget = 1000u, .outcomes = outcomes, .outcome_capacity = 8,
                                               .outcome_count = &count, .status = &status }) == TA_OK);
        for (uint32_t index = 0; index < count; index += 1u) {
            CHECK(outcomes[index].kind == TA_INPUT_COMPLETED || outcomes[index].kind == TA_INPUT_IGNORED);
        }
    }
}

/* One round: `go` to every actor, run, then a second later fire `kept`. */
static void round_of(ta_system *system, const ta_actor_id *actors, uint32_t count) {
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system),
                                                                   .actor = actors[index], .name = STR("go"),
                                                                   .event = &event }) == TA_OK);
    }
    drain(system);
    CHECK(ta_system_set_time(system, &(ta_set_time_params){ .now = ta_system_time(system) + 1000u,
                                                            .budget = UINT32_MAX }) == TA_OK);
    drain(system);
}

/* Claims 1 and 2. */
static void test_macrosteps_never_allocate(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    watched = system;
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = busy_chart(),
                                                                               .datamodel = ta_null_datamodel(),
                                                                               .allocator = allocator,
                                                                               .out = &compiled }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, compiled, &id) == TA_OK);
    enum { actors = 4 };
    ta_actor_id actor[actors] = { 0 };
    for (uint32_t index = 0; index < actors; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor[index] }) == TA_OK);
    }
    drain(system);
    for (uint32_t round = 0; round < 8u; round += 1u) {
        round_of(system, actor, actors);
    }
    if (inside != 0u) {
        fprintf(stderr, "allocator requests inside a macrostep: %lu\n", inside);
    }
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

static ta_op_status svc_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    *params->service_handle = 1u;
    return TA_OP_OK;
}

static ta_op_status svc_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)self;
    (void)service_handle;
    (void)fault;
    return TA_OP_OK;
}

static ta_op_status svc_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    (void)params;
    return TA_OP_OK;
}

static const ta_invoker_vtable svc_vtable = { .start = svc_start, .cancel = svc_cancel, .forward = svc_forward };

/* `idle --go--> busy`, which invokes a host service, and `busy --go-->
 * idle`, which cancels it. */
static const ta_scxml_spec *invoking_chart(void) {
    static const ta_str to_busy[] = { STR("busy"), {0} };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_busy },
        {0},
    };
    static const ta_scxml_transition_spec busy[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_idle },
        {0},
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .type = LIT("svc"), .src = LIT("service") },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy"), .transitions = busy, .invokes = invokes },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("invoking"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Claim 3. */
static void test_invocations_never_allocate(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    watched = system;
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(system, type, (ta_invoker){ .v = &svc_vtable }) == TA_OK);
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = invoking_chart(),
                                                                               .datamodel = ta_null_datamodel(),
                                                                               .allocator = allocator,
                                                                               .out = &compiled }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, compiled, &id) == TA_OK);
    enum { actors = 3 };
    ta_actor_id actor[actors] = { 0 };
    for (uint32_t index = 0; index < actors; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor[index] }) == TA_OK);
    }
    drain(system);
    for (uint32_t round = 0; round < 6u; round += 1u) {
        round_of(system, actor, actors);
    }
    if (inside != 0u) {
        fprintf(stderr, "allocator requests inside a macrostep: %lu\n", inside);
    }
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* `idle --go-->` raises six events; `error.execution.capacity.internal`
 * leads to `full`. */
static const ta_scxml_spec *raising_chart(void) {
    static const ta_scxml_action_spec on_go[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } },
        {0},
    };
    static const ta_str to_full[] = { STR("full"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("go"), .actions = on_go },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution.capacity.internal"), .targets = to_full },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("full") },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("raising"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Claim 4. */
static void test_raise_beyond_the_quota_fails(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    ta_param_value quota = { .param = TA_PARAM_INTERNAL_QUEUE_EVENTS, .value = 4 };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = &quota, .param_count = 1 },
                           &system) == TA_OK);
    watched = system;
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = raising_chart(),
                                                                               .datamodel = ta_null_datamodel(),
                                                                               .allocator = allocator,
                                                                               .out = &compiled }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, compiled, &id) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    drain(system);
    round_of(system, &actor, 1);
    ta_state_id full = 0;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = id,
                                                                               .id = STR("full"),
                                                                               .out = &full }) == TA_OK);
    CHECK(ta_actor_state_active(ta_actor_lookup(system, actor), full));
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Compiles and registers `spec` on the watched system. */
static ta_definition_id define(ta_system *system, ta_allocator allocator, const ta_scxml_spec *spec) {
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = spec,
                                                                               .datamodel = ta_null_datamodel(),
                                                                               .allocator = allocator,
                                                                               .out = &compiled }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, compiled, &id) == TA_OK);
    return id;
}

/* `idle --go--> idle` raises a chain of eight internal events. */
static const ta_scxml_spec *chain_chart(void) {
    static const ta_scxml_action_spec on_go[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("c") } },
        {0},
    };
    static const ta_scxml_action_spec on_c[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("d") } },
        {0},
    };
    static const ta_scxml_action_spec on_d[] = {
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("e") } },
        {0},
    };
    static const ta_str to_one[] = { STR("one"), {0} };
    static const ta_str to_two[] = { STR("two"), {0} };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_one, .actions = on_go },
        {0},
    };
    static const ta_scxml_transition_spec one[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("c"), .targets = to_two, .actions = on_c },
        {0},
    };
    static const ta_scxml_transition_spec two[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("d"), .targets = to_idle, .actions = on_d },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("one"), .transitions = one },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("two"), .transitions = two },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("chain"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Claim 5. */
static void test_paused_macrosteps_never_allocate(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    watched = system;
    ta_definition_id id = define(system, allocator, chain_chart());
    enum { actors = 3 };
    ta_actor_id actor[actors] = { 0 };
    for (uint32_t index = 0; index < actors; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor[index] }) == TA_OK);
    }
    drain(system);
    uint32_t pauses = 0;
    for (uint32_t round = 0; round < 4u; round += 1u) {
        for (uint32_t index = 0; index < actors; index += 1u) {
            ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
            CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system),
                                                                       .actor = actor[index], .name = STR("go"),
                                                                       .event = &event }) == TA_OK);
        }
        ta_run_status status = TA_RUN_MORE;
        while (status != TA_RUN_IDLE) {
            ta_input_outcome outcomes[8];
            uint32_t count = 0;
            CHECK(ta_run(system, &(ta_run_params){ .budget = 1u, .outcomes = outcomes, .outcome_capacity = 8,
                                                   .outcome_count = &count, .status = &status }) == TA_OK);
            pauses += status == TA_RUN_PAUSED ? 1u : 0u;
        }
    }
    CHECK(pauses > 0u);
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* `idle --go--> done`, a top-level <final>. */
static const ta_scxml_spec *finishing_chart(void) {
    static const ta_str to_done[] = { STR("done"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_done },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_FINAL, .id = STR("done") },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("finishing"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Claim 6. */
static void test_finishing_never_allocates(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    watched = system;
    ta_definition_id id = define(system, allocator, finishing_chart());
    enum { actors = 4 };
    ta_actor_id actor[actors] = { 0 };
    for (uint32_t index = 0; index < actors; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor[index] }) == TA_OK);
    }
    drain(system);
    for (uint32_t index = 0; index < actors; index += 1u) {
        ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system),
                                                                   .actor = actor[index], .name = STR("go"),
                                                                   .event = &event }) == TA_OK);
    }
    ta_run_status status = TA_RUN_MORE;
    while (status != TA_RUN_IDLE) {
        ta_input_outcome outcomes[8];
        uint32_t count = 0;
        CHECK(ta_run(system, &(ta_run_params){ .budget = 1000u, .outcomes = outcomes, .outcome_capacity = 8,
                                               .outcome_count = &count, .status = &status }) == TA_OK);
    }
    ta_state_id done = 0;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = id,
                                                                               .id = STR("done"),
                                                                               .out = &done }) == TA_OK);
    for (uint32_t index = 0; index < actors; index += 1u) {
        ta_actor_inspection view = {0};
        CHECK(ta_actor_inspect(system, actor[index], &view) == TA_OK);
        CHECK(view.final_state == done);
    }
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* A host invoker that spawns a local child of `child_definition` when an
 * invocation starts, and destroys it when the invocation is cancelled. */
static ta_definition_id child_definition;
static ta_actor_id last_child;
static uint32_t children_spawned;

static ta_op_status child_start(void *self, const ta_invoker_start_params *params) {
    ta_system *system = self;
    const ta_invoke_request *request = params->request;
    ta_actor_id child = 0;
    ta_result result = ta_actor_spawn(system, &(ta_actor_spawn_params){
                                                  .definition = child_definition,
                                                  .parent = &(ta_actor_parent){ .actor = request->parent,
                                                                                .invoke_id = request->invoke_id },
                                                  .out = &child });
    if (result != TA_OK) {
        return result == TA_ERR_NOMEM ? TA_OP_NOMEM : TA_OP_ERROR;
    }
    *params->service_handle = child;
    last_child = child;
    children_spawned += 1u;
    return TA_OP_OK;
}

static ta_op_status child_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)fault;
    (void)ta_actor_destroy(self, (ta_actor_id)service_handle);
    return TA_OP_OK;
}

static const ta_invoker_vtable child_vtable = { .start = child_start, .cancel = child_cancel, .forward = svc_forward };

/* `idle --go--> busy`, which invokes a local child, and `busy --go-->
 * idle`, which cancels it. */
static const ta_scxml_spec *parent_chart(void) {
    static const ta_str to_busy[] = { STR("busy"), {0} };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_busy },
        {0},
    };
    static const ta_scxml_transition_spec busy[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_idle },
        {0},
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .type = LIT("child") },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy"), .transitions = busy, .invokes = invokes },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("parent"), .datamodel = STR("null"), .states = states };
    return &spec;
}

/* Claim 7. */
static void test_local_children_never_allocate_in_the_parent(void) {
    heap = (test_allocator){ 0 };
    inside = 0;
    children_spawned = 0;
    ta_allocator allocator = { .self = &heap, .v = &watch_vtable };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    watched = system;
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(system, STR("child"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(system, type, (ta_invoker){ .self = system, .v = &child_vtable }) == TA_OK);
    child_definition = define(system, allocator, chain_chart());
    ta_definition_id id = define(system, allocator, parent_chart());
    enum { actors = 3 };
    ta_actor_id actor[actors] = { 0 };
    for (uint32_t index = 0; index < actors; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor[index] }) == TA_OK);
    }
    drain(system);
    for (uint32_t round = 0; round < 6u; round += 1u) {
        round_of(system, actor, actors);
    }
    CHECK(children_spawned == 3u * actors);
    if (inside != 0u) {
        fprintf(stderr, "allocator calls inside a parent's macrostep: %lu\n", inside);
    }
    CHECK(inside == 0u);
    watched = NULL;
    ta_system_destroy(system);
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
}

int main(void) {
    test_macrosteps_never_allocate();
    test_invocations_never_allocate();
    test_raise_beyond_the_quota_fails();
    test_paused_macrosteps_never_allocate();
    test_finishing_never_allocates();
    test_local_children_never_allocate_in_the_parent();
    if (failures != 0) {
        fprintf(stderr, "macrostep_alloc_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("macrostep_alloc_test: ok");
    return 0;
}
