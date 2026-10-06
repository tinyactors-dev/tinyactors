/*
 * A macrostep never allocates (ADR 0102): an allocator that refuses every
 * request made while a macrostep runs sees no such request, from the
 * first macrostep of a fresh system on. Safe points (spawn, admission,
 * the refill between inputs, setting the time) may allocate.
 *
 * 1. a session's bootstrap, a chain of raised internal events, sends to
 *    the session itself and to #_internal, and a delayed send cancelled in
 *    the same macrostep make no request inside a macrostep;
 * 2. delayed sends that stay pending and fire later make none either;
 * 3. a <raise> beyond `internalQueueEvents` in one macrostep fails as
 *    error.execution.capacity.internal, and the chart goes on, still
 *    without a request inside a macrostep.
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

/* Counts and refuses what is asked while `watched` runs a macrostep. */
static void *watch_alloc(void *self, size_t size, size_t alignment) {
    if (watched != NULL && watched->run.running) {
        inside += 1u;
        return NULL;
    }
    return test_allocator_vtable.alloc(self, size, alignment);
}

static void watch_free(void *self, const ta_allocator_free_params *params) {
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

/* Claim 3. */
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

int main(void) {
    test_macrosteps_never_allocate();
    test_raise_beyond_the_quota_fails();
    if (failures != 0) {
        fprintf(stderr, "macrostep_alloc_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("macrostep_alloc_test: ok");
    return 0;
}
