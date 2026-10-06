/*
 * The test driver (tests/test_drive.h): the one way core's other test
 * programs run an actor, alone or after delivering an event to it. Their
 * verdicts rest on it running exactly what they ask for, so its own
 * contract is proven here, on a small chart:
 *
 *   idle --go--> a  (raising r1 and r2)   a --r1--> b   b --r2--> idle
 *   idle --stop--> end (final)
 *
 * `go` makes a macrostep of three microsteps, the first of two units.
 *
 * Uses the C library; the core does not.
 *
 * Claims proven here, in order:
 *
 * 1. test_drive_run with null parameters, or zeroed ones, runs the
 *    bootstrap to stable: the budget 0 is TEST_DRIVE_BUDGET, not a refused
 *    budget, and a null status is allowed;
 * 2. test_drive_deliver delivers the event and runs the actor once: the
 *    whole macrostep, reported stable, with `ignored` cleared; an event
 *    that enables no transition is reported ignored; a final state is
 *    reported done;
 * 3. a refused delivery answers the delivery's own result (TA_ERR_DONE for
 *    a finished actor, TA_ERR_NOT_FOUND for no actor) and runs nothing:
 *    the status and `ignored` are untouched;
 * 4. the limits reach the run: a budget of one microstep pauses between
 *    microsteps, an action budget of one pauses inside the first
 *    microstep, a pause at the start of every microstep pauses before its
 *    first unit; each later run continues where the last one stopped, and
 *    test_drive_deliver passes its limits on in the same way;
 * 5. one call is one run: the helper never runs again after a pause;
 * 6. the helper keeps nothing: every allocation is given back when the
 *    system is destroyed;
 * 7. release is asked for: a session a fault stopped (a runaway) answers
 *    the fault's code and stays stopped through a run without `release`;
 *    a run with `release` lets it go on to the end of its macrostep.
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

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * The chart and a system that runs it
 */

static const ta_str to_a[] = { STR("a"), { 0 } };
static const ta_str to_b[] = { STR("b"), { 0 } };
static const ta_str to_idle[] = { STR("idle"), { 0 } };
static const ta_str to_end[] = { STR("end"), { 0 } };
static const ta_scxml_action_spec raise_both[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r1") } },
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r2") } },
    { 0 },
};
static const ta_scxml_transition_spec from_idle[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_a, .actions = raise_both },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = to_end },
    { 0 },
};
static const ta_scxml_transition_spec from_a[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("r1"), .targets = to_b },
    { 0 },
};
static const ta_scxml_transition_spec from_b[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("r2"), .targets = to_idle },
    { 0 },
};
static const ta_scxml_state_spec states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = from_idle },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = from_a },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = from_b },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    { 0 },
};

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
    ta_actor_deliver_params delivery;
} fixture;

static fixture open_fixture_with(const ta_param_value *params, uint32_t count) {
    fixture f = { 0 };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &f.system) == TA_OK);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(f.system, &(ta_definition_compile_params){
                                                   .spec = &(ta_scxml_spec){ .name = STR("driven"), .states = states },
                                                   .datamodel = ta_null_datamodel(),
                                                   .allocator = allocator,
                                                   .out = &definition,
                                               }) == TA_OK);
    CHECK(ta_system_register_definition(f.system, definition, &f.definition) == TA_OK);
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.definition, .out = &f.actor }) ==
          TA_OK);
    return f;
}

static fixture open_fixture(void) {
    return open_fixture_with(NULL, 0);
}

static void close_fixture(fixture *f) {
    ta_system_destroy(f->system);
    CHECK(test_allocator_finish(&heap) == 0);
}

static ta_event event(fixture *f, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(f->system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) ==
          TA_OK);
    ta_event made = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_event_name_intern(f->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &made.name) == TA_OK);
    return made;
}

/* The delivery of `offered` to `actor` at the system's time, kept in the
 * fixture so the test can pass its address. */
static const ta_actor_deliver_params *deliver_to(fixture *f, ta_actor_id actor, const ta_event *offered) {
    f->delivery = (ta_actor_deliver_params){ .name = test_event_text(f->system, offered), .time = ta_system_time(f->system), .actor = actor, .event = offered };
    return &f->delivery;
}

/* True when the actor finished in the final state `end`. */
static bool finished(fixture *f) {
    ta_state_id end = 0;
    CHECK(ta_definition_find_state(f->system, &(ta_definition_find_state_params){
                                                  .definition = f->definition, .id = STR("end"), .out = &end }) ==
          TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f->system, f->actor, &view) == TA_OK);
    return view.residency == TA_ACTOR_DONE && view.final_state == end;
}

/* True when `id` is the actor's one active state. */
static bool only_in(fixture *f, const char *id) {
    ta_state_id wanted = 0;
    CHECK(ta_definition_find_state(f->system, &(ta_definition_find_state_params){
                                                  .definition = f->definition,
                                                  .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                                  .out = &wanted }) == TA_OK);
    ta_state_id active[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(f->system, &(ta_actor_active_states_params){ .actor = f->actor, .states = active,
                                                                             .capacity = 4, .count = &count }) ==
          TA_OK);
    return count == 1 && active[0] == wanted;
}

static ta_actor_inspection inspect(fixture *f) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(f->system, f->actor, &view) == TA_OK);
    return view;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Claim 1. */
static void test_defaults_run_the_bootstrap(void) {
    fixture f = open_fixture();
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    CHECK(only_in(&f, "idle") && !inspect(&f).macrostep_in_progress);
    close_fixture(&f);

    f = open_fixture();
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, f.actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && only_in(&f, "idle"));
    close_fixture(&f);
}

/* Claim 2. */
static void test_deliver_runs_the_macrostep(void) {
    fixture f = open_fixture();
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    bool ignored = true;
    ta_event go = event(&f, "go");
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go),
                             &(test_drive_run_params){ .status = &status, .ignored = &ignored }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && !ignored && only_in(&f, "idle"));

    ta_event noise = event(&f, "noise");
    status = TEST_DRIVE_ERROR;
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &noise),
                             &(test_drive_run_params){ .status = &status, .ignored = &ignored }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && ignored && only_in(&f, "idle"));

    ta_event stop = event(&f, "stop");
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &stop),
                             &(test_drive_run_params){ .status = &status, .ignored = &ignored }) == TA_OK);
    CHECK(status == TEST_DRIVE_DONE && !ignored && finished(&f));
    close_fixture(&f);
}

/* Claim 3. */
static void test_refused_delivery_runs_nothing(void) {
    fixture f = open_fixture();
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    ta_event stop = event(&f, "stop");
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &stop), NULL) == TA_OK);
    CHECK(finished(&f));

    test_drive_status status = TEST_DRIVE_PAUSED;
    bool ignored = true;
    ta_event go = event(&f, "go");
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go),
                             &(test_drive_run_params){ .status = &status, .ignored = &ignored }) == TA_ERR_DONE);
    CHECK(status == TEST_DRIVE_PAUSED && ignored);
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor + 1000u, &go),
                             &(test_drive_run_params){ .status = &status, .ignored = &ignored }) ==
          TA_ERR_NOT_FOUND);
    CHECK(status == TEST_DRIVE_PAUSED && ignored && finished(&f));
    close_fixture(&f);
}

/* Claims 4 and 5: a budget of one microstep. */
static void test_microstep_budget_pauses_between_microsteps(void) {
    fixture f = open_fixture();
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    ta_event go = event(&f, "go");
    test_drive_status status = TEST_DRIVE_ERROR;
    test_drive_run_params one = { .budget = 1, .status = &status };
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go), &one) == TA_OK);
    ta_actor_inspection view = inspect(&f);
    CHECK(status == TEST_DRIVE_PAUSED && only_in(&f, "a"));
    CHECK(view.macrostep_in_progress && !view.microstep_in_progress && view.step.microstep == 1u);

    CHECK(test_drive_run(f.system, f.actor, &one) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED && only_in(&f, "b"));
    CHECK(test_drive_run(f.system, f.actor, &one) == TA_OK);
    /* The budget is spent before the macrostep finds itself complete. */
    CHECK(status == TEST_DRIVE_PAUSED && only_in(&f, "idle") && inspect(&f).macrostep_in_progress);
    CHECK(test_drive_run(f.system, f.actor, &one) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && only_in(&f, "idle") && !inspect(&f).macrostep_in_progress);
    close_fixture(&f);
}

/* Claims 4 and 5: an action budget of one unit, and a pause at every
 * microstep's start. */
static void test_action_limits_pause_inside_a_microstep(void) {
    fixture f = open_fixture();
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    ta_event go = event(&f, "go");
    test_drive_status status = TEST_DRIVE_ERROR;
    test_drive_run_params unit = { .action_budget = 1, .status = &status };
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go), &unit) == TA_OK);
    ta_actor_inspection view = inspect(&f);
    CHECK(status == TEST_DRIVE_PAUSED && view.microstep_in_progress);
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    CHECK(only_in(&f, "idle") && !inspect(&f).macrostep_in_progress);

    test_drive_run_params at_start = { .pause_at_microstep_start = true, .status = &status };
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go), &at_start) == TA_OK);
    view = inspect(&f);
    CHECK(status == TEST_DRIVE_PAUSED && view.microstep_in_progress && view.position.action != TA_ACTION_NONE);
    CHECK(only_in(&f, "idle")); /* nothing exited yet */
    status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, f.actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && only_in(&f, "idle"));
    close_fixture(&f);
}

/* Claim 7. */
static void test_release_is_asked_for(void) {
    /* `go` takes three microsteps: the limit of two stops it. */
    ta_param_value limit[] = { { TA_PARAM_MICROSTEP_LIMIT, 2 } };
    fixture f = open_fixture_with(limit, 1);
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    ta_event go = event(&f, "go");
    test_drive_status status = TEST_DRIVE_STABLE;
    CHECK(test_drive_deliver(f.system, deliver_to(&f, f.actor, &go), &(test_drive_run_params){ .status = &status }) ==
          TA_ERR_RUNAWAY);
    CHECK(status == TEST_DRIVE_ERROR && inspect(&f).macrostep_in_progress);
    /* Not released: the run leaves it stopped where it stands. */
    CHECK(test_drive_run(f.system, f.actor, NULL) == TA_OK);
    CHECK(inspect(&f).macrostep_in_progress && !only_in(&f, "idle"));
    CHECK(test_drive_run(f.system, f.actor, &(test_drive_run_params){ .status = &status, .release = true }) ==
          TA_OK);
    CHECK(status == TEST_DRIVE_STABLE && only_in(&f, "idle") && !inspect(&f).macrostep_in_progress);
    close_fixture(&f);
}

int main(void) {
    test_defaults_run_the_bootstrap();
    test_deliver_runs_the_macrostep();
    test_refused_delivery_runs_nothing();
    test_microstep_budget_pauses_between_microsteps();
    test_action_limits_pause_inside_a_microstep();
    test_release_is_asked_for();
    if (failures != 0) {
        fprintf(stderr, "test_drive_test: %d failures\n", failures);
        return 1;
    }
    printf("test_drive_test: ok\n");
    return 0;
}
