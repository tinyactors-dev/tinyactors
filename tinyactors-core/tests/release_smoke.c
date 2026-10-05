/*
 * The release's smoke test (root ADR 0001,
 * docs/adr/0001-packaging-and-releases.md): a program that uses only the
 * public header, linked against a packed static or shared library and run
 * on its target (under qemu for another architecture). It compiles a
 * two-state chart against the null data model, spawns an actor, delivers
 * `go`, checks that the actor moved from `idle` to `done`, destroys the
 * system, and checks that every allocation was freed.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tinyactors.h"

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

static long outstanding = 0;

static void *counting_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    void *memory = aligned_alloc(alignment, size == 0 ? alignment : size);
    outstanding += memory != NULL ? 1 : 0;
    return memory;
}

static void counting_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    outstanding -= params->ptr != NULL ? 1 : 0;
    free(params->ptr);
}

static const ta_allocator_vtable counting_vtable = { .alloc = counting_alloc, .free = counting_free };
static const ta_allocator counting = { .self = NULL, .v = &counting_vtable };

static int fail(const char *what) {
    fprintf(stderr, "release smoke: %s\n", what);
    return 1;
}

/* Whether the state named `id` of `definition` is active in `actor`. */
static int in_state(ta_system *system, ta_definition_id definition, ta_actor_id actor, ta_str id) {
    ta_state_id state = 0;
    if (ta_definition_find_state(system, &(ta_definition_find_state_params){ .definition = definition, .id = id,
                                                                           .out = &state }) != TA_OK) {
        return 0;
    }
    ta_state_id active[8];
    uint32_t count = 0;
    if (ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                         .capacity = 8, .count = &count }) != TA_OK) {
        return 0;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        if (active[index] == state) {
            return 1;
        }
    }
    return 0;
}

int main(void) {
    ta_str to_done[] = { STR("done"), { 0 } };
    ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_done },
        { 0 },
    };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = transitions },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("done") },
        { 0 },
    };
    ta_scxml_spec spec = { .name = STR("smoke"), .datamodel = STR("null"), .states = states };

    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = counting }, &system) != TA_OK) {
        return fail("ta_system_create failed");
    }
    ta_definition *definition = NULL;
    if (ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &spec,
                                                                           .datamodel = ta_null_datamodel(),
                                                                           .allocator = counting,
                                                                           .out = &definition }) != TA_OK) {
        return fail("the chart does not compile");
    }
    ta_definition_id id = 0;
    if (ta_system_register_definition(system, definition, &id) != TA_OK) {
        return fail("ta_system_register_definition failed");
    }
    ta_actor_id actor = 0;
    ta_run_status status = TA_RUN_MORE;
    ta_input_outcome outcome = { .kind = TA_INPUT_TAKEN };
    uint32_t outcomes = 0;
    ta_run_params run = { .budget = 100, .outcomes = &outcome, .outcome_capacity = 1, .outcome_count = &outcomes,
                          .status = &status };
    if (ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) != TA_OK ||
        ta_run(system, &run) != TA_OK || outcomes != 1 || outcome.kind != TA_INPUT_COMPLETED) {
        return fail("the actor does not start");
    }
    if (!in_state(system, id, actor, STR("idle"))) {
        return fail("the actor did not start in idle");
    }

    ta_atom atom = 0;
    ta_event_name_id go = 0;
    if (ta_system_intern_atom(system, STR("go"), &atom) != TA_OK ||
        ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &go) != TA_OK) {
        return fail("the event name cannot be interned");
    }
    ta_event event = { .name = go, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    if (ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                            .event = &event, .correlation = 1 }) != TA_OK ||
        ta_run(system, &run) != TA_OK) {
        return fail("the event is not delivered");
    }
    if (outcomes != 1 || outcome.kind != TA_INPUT_COMPLETED || outcome.correlation != 1 || status != TA_RUN_IDLE ||
        in_state(system, id, actor, STR("idle")) || !in_state(system, id, actor, STR("done"))) {
        return fail("the actor did not reach done");
    }

    ta_system_destroy(system);
    if (outstanding != 0) {
        fprintf(stderr, "release smoke: %ld allocations outstanding after destroy\n", outstanding);
        return 1;
    }
    uint32_t version = ta_version();
    printf("tinyactors %u.%u.%u: idle -> done on go, every allocation freed\n", (unsigned)(version >> 16),
           (unsigned)((version >> 8) & 0xffu), (unsigned)(version & 0xffu));
    return 0;
}
