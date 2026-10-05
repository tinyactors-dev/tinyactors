/*
 * Pausing at a microstep's start (ADR 0086): a resume with
 * `pause_at_microstep_start` selects the next microstep's transitions and
 * pauses before its first unit, before any state is exited. The position
 * names that unit in each kind of block (an <onexit>, transition content,
 * an <onentry>, an initial transition's content, a history default's
 * content, an operator transition's entry), or the first transition of a
 * microstep with none; it equals the PAUSED record's; the pause survives
 * capture and reconstruction; the flag changes nothing a run does; and
 * every allocation of the run can fail.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "tel_reader.h"
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
#define EXPR(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })

/* ------------------------------------------------------------------------
 * A test allocator, which counts what is live and can fail one allocation
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * A data model that writes every script and guard to a log
 */

static char script_log[4096];
static size_t script_length = 0;

static void note(const char *what, ta_str text) {
    int written = snprintf(script_log + script_length, sizeof(script_log) - script_length, "%s%.*s ", what,
                           (int)text.len, text.ptr);
    if (written > 0 && script_length + (size_t)written < sizeof(script_log)) {
        script_length += (size_t)written;
    }
}

static void clear_log(void) {
    script_length = 0;
    script_log[0] = 0;
}

static ta_str source(const void *shared, ta_expr_id expr) {
    return ((const ta_expression *)shared)[expr].as.source;
}

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = (void *)params->expressions;
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
    ta_str text = source(params->shared, params->expr);
    note("?", text);
    *params->out = text.len == 1 && text.ptr[0] == 't';
    return TA_OP_OK;
}
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    note("", source(params->shared, params->script));
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("log"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .execute_script = dm_execute_script,
};

/* ------------------------------------------------------------------------
 * The chart: one microstep whose first unit is in each kind of block, and
 * two with none.
 *
 *   q --enter--> e          <onentry> e1 e2          (e --back--> q: <onexit> ex)
 *   q --init--> p           <initial> content pi     (b --leave--> q: exits b, then p's <onexit> px)
 *   q --hist--> h's history default content hd       (h --back--> q: no content at all)
 *   q --content (guard t)   transition content tc, no targets
 *   q --empty--> r          no content              (r --back--> q)
 */

#define SCRIPT(text) { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR(text) }
#define END {0}

static const ta_scxml_action_spec e_entry_actions[] = { SCRIPT("e1"), SCRIPT("e2"), END };
static const ta_scxml_handler_spec e_entry[] = { { e_entry_actions }, END };
static const ta_scxml_action_spec e_exit_actions[] = { SCRIPT("ex"), END };
static const ta_scxml_handler_spec e_exit[] = { { e_exit_actions }, END };
static const ta_scxml_action_spec p_exit_actions[] = { SCRIPT("px"), END };
static const ta_scxml_handler_spec p_exit[] = { { p_exit_actions }, END };
static const ta_scxml_action_spec p_initial_actions[] = { SCRIPT("pi"), END };
static const ta_scxml_action_spec hd_actions[] = { SCRIPT("hd"), END };
static const ta_scxml_action_spec content_actions[] = { SCRIPT("tc"), END };

static const ta_str to_q[] = { STR("q"), {0} };
static const ta_str to_e[] = { STR("e"), {0} };
static const ta_str to_p[] = { STR("p"), {0} };
static const ta_str to_b[] = { STR("b"), {0} };
static const ta_str to_hh[] = { STR("hh"), {0} };
static const ta_str to_h2[] = { STR("h2"), {0} };
static const ta_str to_r[] = { STR("r"), {0} };

static const ta_scxml_transition_spec q_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("enter"), .targets = to_e },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("init"), .targets = to_p },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("hist"), .targets = to_hh },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("content"), .condition = EXPR("t"),
      .actions = content_actions },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("empty"), .targets = to_r },
    END,
};
static const ta_scxml_transition_spec back_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("back"), .targets = to_q },
    END,
};
static const ta_scxml_transition_spec b_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("leave"), .targets = to_q },
    END,
};
static const ta_scxml_transition_spec hh_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_h2, .actions = hd_actions },
    END,
};
static const ta_scxml_initial_spec p_initial = { .targets = to_b, .actions = p_initial_actions };

static const ta_scxml_state_spec p_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = b_transitions },
    END,
};
static const ta_scxml_state_spec h_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("h1") },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("h2") },
    { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = STR("hh"), .transitions = hh_transitions },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("q"), .transitions = q_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("e"), .onentry = e_entry, .onexit = e_exit,
      .transitions = back_transitions },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("p"), .initial = &p_initial, .onexit = p_exit,
      .states = p_children },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("h"), .states = h_children, .transitions = back_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r"), .transitions = back_transitions },
    END,
};
static const ta_scxml_spec chart = { .name = STR("starts"), .states = roots };

/* ------------------------------------------------------------------------
 * A trace sink that keeps the last control record, and counts records
 */

static ta_step_key last_control_key;
static ta_trace_position last_control_position;
static long paused_records = 0;
static long condition_records = 0;
static long microstep_records = 0;

static bool same_key(ta_step_key a, ta_step_key b) {
    return a.session == b.session && a.macrostep == b.macrostep && a.microstep == b.microstep;
}
static bool same_position(ta_trace_position a, ta_trace_position b) {
    return a.at == b.at && a.state == b.state && a.transition == b.transition && a.action == b.action &&
           a.iteration == b.iteration;
}

static void sink_write(void *self, const ta_trace_record *record) {
    (void)self;
    if (record->kind == TA_TRACE_CONDITION) {
        condition_records += 1;
    } else if (record->kind == TA_TRACE_MICROSTEP) {
        microstep_records += 1;
    } else if (record->kind == TA_TRACE_PAUSED || record->kind == TA_TRACE_RESUMED) {
        if (record->kind == TA_TRACE_RESUMED) {
            /* A run goes on from where the last one stopped (ADR 0083). */
            CHECK(same_key(record->key, last_control_key));
            CHECK(same_position(record->position, last_control_position));
        } else {
            paused_records += 1;
        }
        last_control_key = record->key;
        last_control_position = record->position;
    }
}

/* ------------------------------------------------------------------------
 * Driving it
 */

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
} run;

static ta_event_name_id event_name(ta_system *system, const char *name) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &atom) == TA_OK);
    ta_event_name_id id = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &id) == TA_OK);
    return id;
}

static ta_state_id state(run *r, const char *id) {
    ta_state_id out = TA_STATE_NONE;
    CHECK(ta_definition_find_state(r->system, &(ta_definition_find_state_params){
                                                  .definition = r->definition,
                                                  .id = { .ptr = id, .len = (uint32_t)strlen(id) },
                                                  .out = &out,
                                              }) == TA_OK);
    return out;
}


/* Parameters for the next system a helper creates (ADR 0092: they are fixed
 * at creation), then back to the defaults. */
static ta_param_value next_params[4];
static uint32_t next_param_count;
static void next_param(ta_param param, uint64_t value) {
    next_params[next_param_count++] = (ta_param_value){ .param = param, .value = value };
}

/* A system with the chart and the sink; no actor yet. */
static void open_system(run *r) {
    *r = (run){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = next_params,
                                                .param_count = next_param_count },
                           &r->system) == TA_OK);
    next_param_count = 0;
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(r->system, &(ta_definition_compile_params){ .spec = &chart,
                                                                                 .datamodel = { .v = &dm_vtable },
                                                                                 .allocator = allocator,
                                                                                 .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(r->system, definition, &r->definition) == TA_OK);
    CHECK(ta_system_set_trace_masks(r->system, &(ta_trace_masks){ .kinds = UINT32_MAX,
                                                                  .watched_kinds = UINT32_MAX,
                                                                  .unsampled_kinds = UINT32_MAX }) == TA_OK);
}

/* One run that answers core's own result: the out-of-memory sweep asserts
 * TA_ERR_NOMEM from it, the code of the fault that stopped the input. A
 * stopped input goes on at the next run. */
static ta_result resume_with(run *r, uint32_t action_budget, bool at_start, test_drive_status *status) {
    ta_result result = test_drive_run(r->system, r->actor,
                                      &(test_drive_run_params){ .budget = 1000, .status = status,
                                                                .action_budget = action_budget,
                                                                .pause_at_microstep_start = at_start,
                                                                .release = true });
    tel_drain(r->system, sink_write, NULL);
    return result;
}

static test_drive_status resume(run *r, uint32_t action_budget, bool at_start) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(r->system, r->actor,
                         &(test_drive_run_params){ .budget = 1000, .status = &status, .action_budget = action_budget,
                                                   .pause_at_microstep_start = at_start }) == TA_OK);
    tel_drain(r->system, sink_write, NULL);
    return status;
}

/* A system with one bootstrapped actor in q. The bootstrap's microstep
 * never pauses at its start, flag or not. */
static void start(run *r) {
    open_system(r);
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
    tel_drain(r->system, sink_write, NULL);
    long paused = paused_records;
    CHECK(resume(r, 0, true) == TEST_DRIVE_STABLE);
    CHECK(paused_records == paused);
}

/* Delivers an event, with an operator transition to `target` if not null.
 * Delivery that runs out of memory changes nothing (ADR 0069), and is
 * retried. */
static void deliver(run *r, const char *name, const char *target) {
    ta_state_id targets[1] = { target == NULL ? TA_STATE_NONE : state(r, target) };
    ta_event event = { .name = event_name(r->system, name), .type = TA_EVENT_EXTERNAL,
                       .invoke_id = TA_INVOKE_ID_NONE };
    ta_operator_transition transition = { .source = TA_STATE_NONE, .targets = targets, .target_count = 1 };
    ta_actor_deliver_params params = { .actor = r->actor, .event = &event, .time = ta_system_time(r->system),
                                       .transition = target == NULL ? NULL : &transition };
    ta_result result = ta_actor_deliver(r->system, &params);
    if (result == TA_ERR_NOMEM) {
        result = ta_actor_deliver(r->system, &params);
    }
    CHECK(result == TA_OK);
    tel_drain(r->system, sink_write, NULL);
}

static void offer(run *r, const char *name) {
    deliver(r, name, NULL);
}

static void jump(run *r, const char *target) {
    deliver(r, "operator.goto", target);
}

static ta_actor_inspection inspect(run *r) {
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(r->system, r->actor, &inspection) == TA_OK);
    return inspection;
}

/* The active configuration's atomic state IDs, as a bit set. */
static uint64_t configuration(run *r) {
    ta_state_id states[16];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(r->system, &(ta_actor_active_states_params){ .actor = r->actor, .states = states,
                                                                             .capacity = 16, .count = &count }) ==
          TA_OK);
    uint64_t bits = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        bits |= (uint64_t)1u << states[index];
    }
    return bits;
}

static int log_is(const char *expected) {
    if (strcmp(script_log, expected) != 0) {
        fprintf(stderr, "log: %s\nexpected: %s\n", script_log, expected);
        return 0;
    }
    return 1;
}

static void finish(run *r) {
    ta_system_destroy(r->system);
    CHECK(heap.live == 0);
}

/* The expected start of a microstep: where it stands, the block its first
 * unit is in (or no unit), and the log that one unit adds. */
typedef struct {
    const char *event;    /* delivered; null: `jump` is the operator's target */
    const char *jump;
    ta_trace_at at;
    const char *state;    /* null: TA_STATE_NONE */
    bool transition;      /* `transition` names one */
    bool unit;            /* an action is named */
    ta_action_block block;
    const char *owner;    /* the state owning the block, for ONENTRY and ONEXIT */
    const char *selected; /* what selection logs before the pause */
    const char *first;    /* what the first unit logs */
} start_case;

/*
 * Delivers the case's event and resumes with the flag: the run pauses at
 * the microstep's start, having selected (its guards logged, its CONDITION
 * and MICROSTEP records written) and nothing else. Then one unit runs.
 */
static void check_start(run *r, const start_case *c) {
    if (c->event != NULL) {
        offer(r, c->event);
    } else {
        jump(r, c->jump);
    }
    uint64_t before = configuration(r);
    long microsteps = microstep_records;
    long paused = paused_records;
    clear_log();
    CHECK(resume(r, 1, true) == TEST_DRIVE_PAUSED);
    CHECK(paused_records == paused + 1);
    CHECK(microstep_records == microsteps + 1);
    CHECK(log_is(c->selected));
    CHECK(configuration(r) == before);

    ta_actor_inspection inspection = inspect(r);
    CHECK(inspection.macrostep_in_progress && inspection.microstep_in_progress);
    CHECK(same_key(inspection.step, last_control_key));
    CHECK(same_position(inspection.position, last_control_position));
    ta_trace_position position = inspection.position;
    CHECK(position.at == c->at);
    CHECK(position.state == (c->state == NULL ? TA_STATE_NONE : state(r, c->state)));
    CHECK((position.transition != TA_TRANSITION_NONE) == c->transition);
    CHECK((position.action != TA_ACTION_NONE) == c->unit);
    CHECK(position.iteration == 0);
    if (c->transition) {
        ta_transition_info transition = {0};
        CHECK(ta_definition_transition(r->system, &(ta_definition_transition_params){
                                                      .definition = r->definition,
                                                      .transition = position.transition,
                                                      .out = &transition }) == TA_OK);
        CHECK(transition.source == position.state);
    }
    if (c->unit) {
        ta_action_info info = {0};
        CHECK(ta_definition_action(r->system, &(ta_definition_action_params){ .definition = r->definition,
                                                                             .action = position.action,
                                                                             .out = &info }) == TA_OK);
        CHECK(info.block == c->block && info.index == 0);
        if (c->block == TA_ACTION_IN_ONENTRY || c->block == TA_ACTION_IN_ONEXIT) {
            CHECK(info.owner == state(r, c->owner));
        } else if (c->block == TA_ACTION_IN_TRANSITION && c->transition) {
            CHECK(info.owner == position.transition);
        }
    }
    /* Paused inside a microstep: an event admitted now waits for the
     * macrostep to end (ADR 0099), and it is not to be abandoned (ADR 0082). */
    ta_event waiting = { .name = event_name(r->system, "waiting"), .type = TA_EVENT_EXTERNAL,
                         .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system), .actor = r->actor,
                                                                  .event = &waiting, .correlation = 7 }) == TA_OK);
    CHECK(ta_actor_abandon(r->system, r->actor) == TA_ERR_BUSY);

    /* A single step runs the first unit, and only it. */
    test_drive_status status = resume(r, 1, true);
    char expected[128];
    snprintf(expected, sizeof(expected), "%s%s", c->selected, c->first);
    CHECK(log_is(expected));
    if (status == TEST_DRIVE_PAUSED) {
        CHECK(inspect(r).position.action != position.action);
    }
    while (status == TEST_DRIVE_PAUSED) {
        status = resume(r, 1, true);
    }
    CHECK(status == TEST_DRIVE_STABLE);
    /* The event admitted during the pause was never taken: it can still be
     * withdrawn, and runs nothing. */
    CHECK(ta_system_withdraw_input(r->system, 7) == TA_OK);
}

/* ------------------------------------------------------------------------
 * Tests
 */

static const start_case cases[] = {
    { .event = "enter", .at = TA_TRACE_AT_ENTER, .state = "e", .unit = true, .block = TA_ACTION_IN_ONENTRY,
      .owner = "e", .selected = "", .first = "e1 " },
    { .event = "back", .at = TA_TRACE_AT_EXIT, .state = "e", .unit = true, .block = TA_ACTION_IN_ONEXIT,
      .owner = "e", .selected = "", .first = "ex " },
    { .event = "init", .at = TA_TRACE_AT_ENTER, .state = "p", .unit = true, .block = TA_ACTION_IN_TRANSITION,
      .selected = "", .first = "pi " },
    /* b has no <onexit>: the first unit is its parent's, and b is still active. */
    { .event = "leave", .at = TA_TRACE_AT_EXIT, .state = "p", .unit = true, .block = TA_ACTION_IN_ONEXIT,
      .owner = "p", .selected = "", .first = "px " },
    { .event = "hist", .at = TA_TRACE_AT_ENTER, .state = "h", .unit = true, .block = TA_ACTION_IN_TRANSITION,
      .selected = "", .first = "hd " },
    /* Exits h2 and h, enters q: no unit, so it stands at its transition. */
    { .event = "back", .at = TA_TRACE_AT_TRANSITION, .state = "h", .transition = true, .selected = "",
      .first = "" },
    { .event = "content", .at = TA_TRACE_AT_TRANSITION, .state = "q", .transition = true, .unit = true,
      .block = TA_ACTION_IN_TRANSITION, .selected = "?t ", .first = "tc " },
    { .event = "empty", .at = TA_TRACE_AT_TRANSITION, .state = "q", .transition = true, .selected = "",
      .first = "" },
    { .event = "back", .at = TA_TRACE_AT_TRANSITION, .state = "r", .transition = true, .selected = "",
      .first = "" },
    /* An operator transition with no unit stands at a transition that is
     * not the chart's. */
    { .jump = "r", .at = TA_TRACE_AT_TRANSITION, .selected = "", .first = "" },
    /* One whose first unit is e's <onentry>. */
    { .jump = "e", .at = TA_TRACE_AT_ENTER, .state = "e", .unit = true, .block = TA_ACTION_IN_ONENTRY,
      .owner = "e", .selected = "", .first = "e1 " },
};
#define CASE_COUNT (sizeof(cases) / sizeof(cases[0]))

static void test_each_kind_of_first_unit(void) {
    run r;
    start(&r);
    long conditions = condition_records;
    for (uint32_t index = 0; index < CASE_COUNT; index += 1u) {
        check_start(&r, &cases[index]);
    }
    CHECK(condition_records == conditions + 1); /* the guard, written before the pause */
    finish(&r);
}

/*
 * Every case's event in one run, resumed until stable after each: with the
 * flag and any action budget the log and configurations equal a run
 * without the flag, and at budget 0 the flag pauses once per microstep.
 */
static void run_all(run *r, uint32_t action_budget, bool at_start, uint32_t *pauses, char *configurations) {
    start(r);
    clear_log();
    size_t length = 0;
    for (uint32_t index = 0; index < CASE_COUNT; index += 1u) {
        if (cases[index].event != NULL) {
            offer(r, cases[index].event);
        } else {
            jump(r, cases[index].jump);
        }
        test_drive_status status;
        while ((status = resume(r, action_budget, at_start)) == TEST_DRIVE_PAUSED) {
            *pauses += 1u;
        }
        CHECK(status == TEST_DRIVE_STABLE);
        length += (size_t)snprintf(configurations + length, 256u - length, "%llx.",
                                   (unsigned long long)configuration(r));
    }
    finish(r);
}

static void test_the_flag_changes_nothing_a_run_does(void) {
    run r;
    uint32_t pauses = 0;
    char baseline_configurations[256];
    run_all(&r, 0, false, &pauses, baseline_configurations);
    CHECK(pauses == 0);
    char baseline[sizeof(script_log)];
    memcpy(baseline, script_log, script_length + 1u);
    for (uint32_t action_budget = 0; action_budget <= 4u; action_budget += 1u) {
        pauses = 0;
        char configurations[256];
        run_all(&r, action_budget, true, &pauses, configurations);
        CHECK(log_is(baseline));
        CHECK(strcmp(configurations, baseline_configurations) == 0);
        CHECK(pauses >= CASE_COUNT);
        if (action_budget == 0) {
            CHECK(pauses == CASE_COUNT);
        }
    }
    /* Without the flag, action budget 1 never stops before a microstep's
     * first unit: the old behaviour. */
    pauses = 0;
    char configurations[256];
    run_all(&r, 1, false, &pauses, configurations);
    CHECK(log_is(baseline));
    CHECK(pauses == 2u); /* before e2, each time e is entered */
    printf("microstep start test: %u pauses at action budget 1 without the flag\n", pauses);
}

/* A microstep budget still counts completed microsteps: from a start, a
 * budget of 1 finishes the microstep and pauses between microsteps, where
 * the position is nowhere and abandoning is allowed (ADR 0082). */
static void test_microstep_budget(void) {
    run r;
    start(&r);
    offer(&r, "init");
    CHECK(resume(&r, 0, true) == TEST_DRIVE_PAUSED);
    CHECK(inspect(&r).microstep_in_progress);
    clear_log();
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(r.system, r.actor,
                         &(test_drive_run_params){ .budget = 1, .status = &status, .pause_at_microstep_start = true }) ==
          TA_OK);
    tel_drain(r.system, sink_write, NULL);
    CHECK(status == TEST_DRIVE_PAUSED);
    CHECK(log_is("pi "));
    ta_actor_inspection inspection = inspect(&r);
    CHECK(inspection.macrostep_in_progress && !inspection.microstep_in_progress);
    CHECK(inspection.position.at == TA_TRACE_AT_NONE && inspection.position.action == TA_ACTION_NONE);
    CHECK(inspection.step.microstep == 1);
    CHECK(resume(&r, 0, true) == TEST_DRIVE_STABLE);
    CHECK(configuration(&r) == (((uint64_t)1u << state(&r, "p")) | ((uint64_t)1u << state(&r, "b"))));
    finish(&r);
}

/*
 * Captured at a microstep's start, destroyed, and reconstructed: it stands
 * where the captured actor stood and finishes as it would have.
 */
static void test_capture_at_the_start(void) {
    run r;
    start(&r);
    offer(&r, "init");
    CHECK(resume(&r, 0, true) == TEST_DRIVE_PAUSED);
    ta_actor_inspection captured = inspect(&r);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(r.system, &(ta_actor_capture_params){ .actor = r.actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    CHECK(ta_actor_reconstruct(r.system, &(ta_actor_reconstruct_params){ .image = image, .out = &r.actor }) ==
          TA_OK);
    ta_session_image_destroy(image);
    ta_actor_inspection restored = inspect(&r);
    CHECK(restored.microstep_in_progress);
    CHECK(same_key(restored.step, captured.step));
    CHECK(same_position(restored.position, captured.position));
    CHECK(restored.position.at == TA_TRACE_AT_ENTER && restored.position.state == state(&r, "p"));
    clear_log();
    CHECK(resume(&r, 0, true) == TEST_DRIVE_STABLE);
    CHECK(log_is("pi "));
    CHECK(configuration(&r) == (((uint64_t)1u << state(&r, "p")) | ((uint64_t)1u << state(&r, "b"))));
    offer(&r, "leave");
    CHECK(resume(&r, 0, false) == TEST_DRIVE_STABLE);
    CHECK(log_is("pi px "));
    finish(&r);
}

/*
 * Every allocation of a run that pauses at each start and before each
 * unit fails in turn (ADR 0070): a resume that runs out of memory is
 * retried, and the run ends with the log and configurations of the run
 * that failed nothing.
 */
static unsigned long resumes_out_of_memory = 0;
static unsigned long pauses_out_of_memory = 0;

static void run_retrying(run *r, uint32_t first, char *configurations) {
    size_t length = 0;
    for (uint32_t index = first; index < CASE_COUNT; index += 1u) {
        if (cases[index].event != NULL) {
            offer(r, cases[index].event);
        } else {
            jump(r, cases[index].jump);
        }
        test_drive_status status = TEST_DRIVE_PAUSED;
        for (uint32_t calls = 0; status != TEST_DRIVE_STABLE && calls < 1000u; calls += 1u) {
            ta_actor_inspection before = inspect(r);
            ta_result result = resume_with(r, 1, true, &status);
            CHECK(result == TA_OK || result == TA_ERR_NOMEM);
            if (result == TA_ERR_NOMEM) {
                resumes_out_of_memory += 1u;
                status = TEST_DRIVE_PAUSED;
                if (before.microstep_in_progress && before.step.microstep != 0 &&
                    same_position(inspect(r).position, before.position) &&
                    same_key(inspect(r).step, before.step)) {
                    /* It stays where it stood: at a start, or before a unit. */
                    pauses_out_of_memory += 1u;
                }
            }
        }
        CHECK(status == TEST_DRIVE_STABLE);
        length += (size_t)snprintf(configurations + length, 256u - length, "%llx.",
                                   (unsigned long long)configuration(r));
    }
}

static void test_out_of_memory(void) {
    run r;
    next_param(TA_PARAM_EVENT_BLOCK_CACHE, 0);
    start(&r);
    clear_log();
    char baseline_configurations[256];
    unsigned long before = heap.attempts;
    run_retrying(&r, 0, baseline_configurations);
    unsigned long used = heap.attempts - before;
    char baseline[sizeof(script_log)];
    memcpy(baseline, script_log, script_length + 1u);
    finish(&r);
    CHECK(used >= CASE_COUNT);

    for (unsigned long failing = 1; failing <= used; failing += 1u) {
        next_param(TA_PARAM_EVENT_BLOCK_CACHE, 0);
        start(&r);
        clear_log();
        char configurations[256];
        heap.policy.fail_at = heap.attempts + failing;
        run_retrying(&r, 0, configurations);
        heap.policy.fail_at = 0;
        if (!log_is(baseline) || strcmp(configurations, baseline_configurations) != 0) {
            fprintf(stderr, "allocation %lu failing\n", failing);
            failures += 1;
        }
        finish(&r);
    }
    CHECK(resumes_out_of_memory > 0 && pauses_out_of_memory > 0);
    printf("microstep start test: %lu allocations each failed in turn; %lu resumes ran out of memory, %lu of them "
           "from a pause\n",
           used, resumes_out_of_memory, pauses_out_of_memory);
}

int main(void) {
    test_each_kind_of_first_unit();
    test_the_flag_changes_nothing_a_run_does();
    test_microstep_budget();
    test_capture_at_the_start();
    test_out_of_memory();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "microstep start test: %d failures\n", failures);
        return 1;
    }
    puts("tinyactors-core microstep start test passed");
    return 0;
}
