/* test: runtime asan */
/*
 * Pausing inside executable content (ADR 0061): a run that pauses before
 * every unit of executable content is observably identical to one that
 * never pauses, for every action budget; a paused actor captured inside a
 * <foreach> reconstructs and finishes identically; a pause that cannot save
 * keeps running; and nothing leaks, snapshots included.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "tel_reader.h"
#include "test_allocator.h"

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
 * An allocator that counts what is live and can refuse everything
 */

/* Whether the script `x1` has turned refusal on; the allocator itself refuses. */
static int refuse_all = 0;
/* Set: the next script `x1` starts refusing, inside the microstep, after its
 * queue slot is reserved (ADR 0070), so every pause after it must save
 * without allocating. */
static int refuse_from_x1 = 0;

static ta_test_allocator heap;
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * <foreach> snapshots that count their references
 */

static long live_snapshots = 0;

static void snapshot_retain(void *self) {
    (void)self;
    live_snapshots += 1;
}
static void snapshot_release(void *self) {
    (void)self;
    live_snapshots -= 1;
}
static const ta_value_vtable snapshot_vtable = { .retain = snapshot_retain, .release = snapshot_release };
static int snapshot_token;

/* ------------------------------------------------------------------------
 * A data model that writes every operation to a trace
 */

static char trace[8192];
static size_t trace_length = 0;

static void note(const char *what, ta_str text) {
    int written = snprintf(trace + trace_length, sizeof(trace) - trace_length, "%s %.*s;", what, (int)text.len,
                           text.ptr);
    if (written > 0 && trace_length + (size_t)written < sizeof(trace)) {
        trace_length += (size_t)written;
    }
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
    note("if", text);
    *params->out = text.len == 1 && text.ptr[0] == 't';
    return TA_OP_OK;
}
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->script);
    note("script", text);
    if (refuse_from_x1 && text.len == 2 && memcmp(text.ptr, "x1", 2) == 0) {
        refuse_all = 1;
        ta_test_allocator_refuse_all(&heap, true);
    }
    return TA_OP_OK;
}
static ta_op_status dm_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self;
    note("assign", source(params->shared, params->location));
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    note("value", source(params->shared, params->expr));
    return TA_OP_OK;
}
static ta_op_status dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->array);
    note("each", text);
    *params->out = (ta_value){ .self = &snapshot_token, .v = &snapshot_vtable };
    live_snapshots += 1;
    *params->count = (uint32_t)(text.ptr[0] - '0');
    return TA_OP_OK;
}
static ta_op_status dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self;
    char position[2] = { (char)('0' + params->index), 0 };
    note("bind", (ta_str){ .ptr = position, .len = 1 });
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("trace"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .eval_value = dm_eval_value,
    .assign = dm_assign,
    .execute_script = dm_execute_script,
    .foreach_snapshot = dm_foreach_snapshot,
    .foreach_bind = dm_foreach_bind,
};

/* ------------------------------------------------------------------------
 * A trace sink that keeps the last execution-control record (ADR 0083)
 */

static ta_step_key last_control_key;
static ta_trace_position last_control_position;
static long control_records = 0;

static bool same_key(ta_step_key a, ta_step_key b) {
    return a.session == b.session && a.macrostep == b.macrostep && a.microstep == b.microstep;
}
static bool same_position(ta_trace_position a, ta_trace_position b) {
    return a.at == b.at && a.state == b.state && a.transition == b.transition && a.action == b.action &&
           a.iteration == b.iteration;
}
static bool nowhere(ta_trace_position position) {
    return position.at == TA_TRACE_AT_NONE && position.state == TA_STATE_NONE &&
           position.transition == TA_TRANSITION_NONE && position.action == TA_ACTION_NONE && position.iteration == 0;
}

static void control_visit(void *self, const ta_tel_entry *entry) {
    (void)self;
    const ta_tel_trace *record = &entry->trace;
    if (record->kind == TA_TRACE_RESUMED) {
        /* A run goes on from where the last one stopped. */
        CHECK(same_key(record->key, last_control_key));
        CHECK(same_position(record->position, last_control_position));
    }
    last_control_key = record->key;
    last_control_position = record->position;
    control_records += 1;
}
static const ta_trace_masks control_masks = {
    .kinds = TA_TRACE_BIT(TA_TRACE_PAUSED) | TA_TRACE_BIT(TA_TRACE_RESUMED),
};
/* Read after each resume: the records are checked in the order core wrote them. */
static tel_reader control_reader = { .visit = control_visit };

/* ------------------------------------------------------------------------
 * The chart: nested <if> and <foreach> in exit handlers, transition
 * content and entry handlers, several handlers per state, and a raised
 * event that moves on inside the same macrostep.
 *
 *   a  --go-->  p (b --inner--> c)  --back-->  a
 */

#define SCRIPT(text) { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR(text) }
#define END {0}

static const ta_scxml_action_spec a_exit_else[] = { { .kind = TA_SCXML_ACTION_ASSIGN,
                                                      .as.assign = { .location = EXPR("loc"),
                                                                     .value = { .kind = TA_SCXML_VALUE_EXPR,
                                                                                .as.expr = EXPR("v") } } },
                                                    END };
static const ta_scxml_action_spec a_exit_then[] = { SCRIPT("never"), END };
static const ta_scxml_action_spec a_exit_body[] = {
    SCRIPT("x2"),
    { .kind = TA_SCXML_ACTION_IF,
      .as.conditional = { .condition = EXPR("f"), .then_actions = a_exit_then, .else_actions = a_exit_else } },
    END,
};
static const ta_scxml_action_spec a_exit_if[] = {
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("i"), .body = a_exit_body } },
    END,
};
static const ta_scxml_action_spec a_exit_1[] = {
    SCRIPT("x1"),
    { .kind = TA_SCXML_ACTION_IF, .as.conditional = { .condition = EXPR("t"), .then_actions = a_exit_if } },
    END,
};
static const ta_scxml_action_spec a_exit_2[] = { SCRIPT("x3"), END };
static const ta_scxml_handler_spec a_exit[] = { { a_exit_1 }, { a_exit_2 }, END };

static const ta_scxml_action_spec go_inner[] = { SCRIPT("t2"), END };
static const ta_scxml_action_spec go_body[] = {
    SCRIPT("t1"),
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("k"), .body = go_inner } },
    END,
};
static const ta_scxml_action_spec go_actions[] = {
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("3"), .item = STR("j"), .body = go_body } },
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("inner") } },
    END,
};
static const ta_str to_p[] = { STR("p"), {0} };
static const ta_scxml_transition_spec a_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_p, .actions = go_actions },
    END,
};

static const ta_scxml_action_spec p_body[] = { SCRIPT("p2"), END };
static const ta_scxml_action_spec p_entry_1[] = {
    SCRIPT("p1"),
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("m"), .body = p_body } },
    END,
};
static const ta_scxml_handler_spec p_entry[] = { { p_entry_1 }, END };

static const ta_scxml_action_spec b_entry_1[] = { SCRIPT("b1"), END };
static const ta_scxml_action_spec b_body[] = { SCRIPT("b2"), END };
static const ta_scxml_action_spec b_entry_2[] = {
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("n"), .body = b_body } },
    END,
};
static const ta_scxml_handler_spec b_entry[] = { { b_entry_1 }, { b_entry_2 }, END };
static const ta_scxml_action_spec inner_actions[] = { SCRIPT("tc"), END };
static const ta_str to_c[] = { STR("c"), {0} };
static const ta_scxml_transition_spec b_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("inner"), .targets = to_c, .actions = inner_actions },
    END,
};

static const ta_scxml_action_spec c_entry_1[] = { SCRIPT("c1"), END };
static const ta_scxml_handler_spec c_entry[] = { { c_entry_1 }, END };
static const ta_scxml_action_spec back_actions[] = { SCRIPT("back"), END };
static const ta_str to_a[] = { STR("a"), {0} };
static const ta_scxml_transition_spec c_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("back"), .targets = to_a, .actions = back_actions },
    END,
};

static const ta_scxml_state_spec p_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .onentry = b_entry, .transitions = b_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c"), .onentry = c_entry, .transitions = c_transitions },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .onexit = a_exit, .transitions = a_transitions },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("p"), .onentry = p_entry, .states = p_children },
    END,
};
static const ta_scxml_spec chart = { .name = STR("pausing"), .states = roots };

/* ------------------------------------------------------------------------
 * Driving it
 */

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
    uint32_t budget;
    uint32_t action_budget;
    uint32_t pauses;
    uint32_t microstep_pauses;
    /* Pauses whose inspected position named a unit to run next. */
    uint32_t pauses_before_unit;
} run;

static void start(run *r, uint32_t action_budget) {
    *r = (run){ .budget = 1000, .action_budget = action_budget };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &r->system) == TA_OK);
    CHECK(ta_system_set_trace_masks(r->system, &control_masks) == TA_OK);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(r->system, &(ta_definition_compile_params){ .spec = &chart,
                                                                                 .datamodel = { .v = &dm_vtable },
                                                                                 .allocator = allocator,
                                                                                 .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(r->system, definition, &r->definition) == TA_OK);
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
}

/*
 * One resume call; counts pauses, and pauses inside a microstep. A paused
 * actor's inspection reports the key and position of the PAUSED record the
 * call wrote (ADR 0083), or, when its macrostep had not begun and so wrote
 * none, the session alone and nowhere.
 */
static ta_run_status resume_once(run *r) {
    ta_run_status status = TA_RUN_ERROR;
    long records = control_records;
    CHECK(ta_actor_resume(r->system, &(ta_actor_resume_params){ .actor = r->actor, .budget = r->budget,
                                                               .status = &status,
                                                               .action_budget = r->action_budget }) == TA_OK);
    tel_read(r->system, &control_reader);
    CHECK(control_reader.bad == 0);
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(r->system, r->actor, &inspection) == TA_OK);
    CHECK(inspection.step.session == inspection.session_id);
    if (status != TA_RUN_PAUSED) {
        /* Outside a macrostep: the session alone, and nowhere. */
        CHECK(!inspection.macrostep_in_progress);
        CHECK(same_key(inspection.step, (ta_step_key){ .session = inspection.session_id }));
        CHECK(nowhere(inspection.position));
    }
    if (status == TA_RUN_PAUSED) {
        r->pauses += 1u;
        CHECK(inspection.macrostep_in_progress);
        r->microstep_pauses += inspection.microstep_in_progress ? 1u : 0u;
        if (control_records != records) {
            CHECK(same_key(inspection.step, last_control_key));
            CHECK(same_position(inspection.position, last_control_position));
        } else {
            CHECK(inspection.step.macrostep == 0 && nowhere(inspection.position));
        }
        /* Inside a microstep it stands before a unit; between microsteps,
         * nowhere. */
        CHECK(inspection.microstep_in_progress == (inspection.position.action != TA_ACTION_NONE));
        CHECK(inspection.microstep_in_progress || nowhere(inspection.position));
        r->pauses_before_unit += inspection.position.action != TA_ACTION_NONE ? 1u : 0u;
        ta_atom atom = 0;
        CHECK(ta_system_intern_atom(r->system, STR("refused"), &atom) == TA_OK);
        ta_event refused = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &refused.name) == TA_OK);
        CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system), .actor = r->actor, .event = &refused }) == TA_ERR_BUSY);
    }
    return status;
}

static void settle(run *r) {
    ta_run_status status;
    uint32_t calls = 0;
    while ((status = resume_once(r)) == TA_RUN_PAUSED && calls < 10000u) {
        calls += 1u;
    }
    CHECK(status == TA_RUN_STABLE);
}

static void deliver(run *r, const char *name) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(r->system, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &atom) == TA_OK);
    ta_event_name_id event_name = 0;
    CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &event_name) == TA_OK);
    ta_event event = { .name = event_name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system), .actor = r->actor, .event = &event }) == TA_OK);
}

/* The active states, by ID, appended to the trace. */
static void note_configuration(run *r) {
    ta_state_id states[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(r->system, &(ta_actor_active_states_params){ .actor = r->actor, .states = states,
                                                                             .capacity = 8, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1u) {
        char digit[2] = { (char)('0' + states[index]), 0 };
        note("active", (ta_str){ .ptr = digit, .len = 1 });
    }
}

static void script(run *r) {
    settle(r);
    note_configuration(r);
    deliver(r, "go");
    settle(r);
    note_configuration(r);
    deliver(r, "back");
    settle(r);
    note_configuration(r);
    deliver(r, "go");
    settle(r);
    note_configuration(r);
}

static void finish(run *r) {
    ta_system_destroy(r->system);
}

/* ------------------------------------------------------------------------
 * Tests
 */

static void test_every_budget_is_invisible(void) {
    trace_length = 0;
    run r;
    start(&r, 0);
    script(&r);
    CHECK(r.pauses == 0);
    finish(&r);
    char baseline[sizeof(trace)];
    memcpy(baseline, trace, trace_length + 1u);
    size_t baseline_length = trace_length;

    uint32_t budgets_that_paused = 0;
    for (uint32_t budget = 1; budget <= 80u; budget += 1u) {
        trace_length = 0;
        trace[0] = 0;
        start(&r, budget);
        script(&r);
        finish(&r);
        if (trace_length != baseline_length || memcmp(trace, baseline, baseline_length) != 0) {
            fprintf(stderr, "action budget %u differs:\n  %s\nexpected:\n  %s\n", budget, trace, baseline);
            failures += 1;
        }
        CHECK(budget > 45u || r.microstep_pauses > 0u);
        budgets_that_paused += r.microstep_pauses > 0u ? 1u : 0u;
    }
    CHECK(budgets_that_paused >= 40u);
    CHECK(live_snapshots == 0);

    /* A budget of one pauses before every unit. */
    trace_length = 0;
    start(&r, 1);
    script(&r);
    finish(&r);
    printf("pause test: %zu-byte trace, %u pauses at action budget 1, identical for budgets 1..80\n", baseline_length,
           r.pauses);
    CHECK(r.pauses > 40u);
}

/* Paused inside the transition's nested <foreach>: capture, destroy the
 * original, reconstruct, and finish the same way. */
static void test_capture_inside_foreach(void) {
    trace_length = 0;
    run r;
    start(&r, 0);
    script(&r);
    finish(&r);
    char baseline[sizeof(trace)];
    memcpy(baseline, trace, trace_length + 1u);
    size_t baseline_length = trace_length;

    trace_length = 0;
    start(&r, 1);
    settle(&r);
    note_configuration(&r);
    deliver(&r, "go");
    /* Run into the transition's content: the exit handlers take 12 units. */
    for (uint32_t step = 0; step < 16u; step += 1u) {
        CHECK(resume_once(&r) == TA_RUN_PAUSED);
    }
    CHECK(live_snapshots >= 2);
    ta_actor_inspection captured;
    CHECK(ta_actor_inspect(r.system, r.actor, &captured) == TA_OK);
    CHECK(captured.position.at == TA_TRACE_AT_TRANSITION && captured.position.transition != TA_TRANSITION_NONE);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(r.system, &(ta_actor_capture_params){ .actor = r.actor, .out = &image }) == TA_OK);
    long held = live_snapshots;
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    CHECK(live_snapshots < held); /* the original's frames were released */
    ta_actor_id restored = 0;
    CHECK(ta_actor_reconstruct(r.system, &(ta_actor_reconstruct_params){ .image = image, .out = &restored }) == TA_OK);
    ta_session_image_destroy(image);
    r.actor = restored;
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(r.system, r.actor, &inspection) == TA_OK);
    CHECK(inspection.microstep_in_progress);
    /* The image keeps where it stood (ADR 0083). */
    CHECK(same_key(inspection.step, captured.step));
    CHECK(same_position(inspection.position, captured.position));
    settle(&r);
    note_configuration(&r);
    deliver(&r, "back");
    settle(&r);
    note_configuration(&r);
    deliver(&r, "go");
    settle(&r);
    note_configuration(&r);
    finish(&r);
    if (trace_length != baseline_length || memcmp(trace, baseline, baseline_length) != 0) {
        fprintf(stderr, "reconstructed run differs:\n  %s\nexpected:\n  %s\n", trace, baseline);
        failures += 1;
    }
    CHECK(live_snapshots == 0);
}

/* A pause needs no allocation (ADR 0070): the run stops into a continuation
 * reserved before it started, or into the one it resumed from. With every
 * allocation refused from inside the microstep on, it still pauses after
 * each unit, and the macrostep ends as the first "go" did. */
static void test_pause_needs_no_allocation(void) {
    trace_length = 0;
    run r;
    start(&r, 1);
    settle(&r);
    deliver(&r, "go");
    settle(&r);
    deliver(&r, "back");
    settle(&r);
    size_t warmed = trace_length;
    deliver(&r, "go");
    refuse_from_x1 = 1;
    ta_run_status status = resume_once(&r);
    refuse_from_x1 = 0;
    CHECK(refuse_all == 1);
    CHECK(status == TA_RUN_PAUSED);
    uint32_t pauses = 0;
    while ((status = resume_once(&r)) == TA_RUN_PAUSED && pauses < 10000u) {
        pauses += 1u;
    }
    refuse_all = 0;
    ta_test_allocator_refuse_all(&heap, false);
    CHECK(pauses > 10u);
    CHECK(status == TA_RUN_STABLE);
    note_configuration(&r);
    finish(&r);
    /* The refused run did what the first "go" did. */
    char first_go[4096];
    size_t go_start = (size_t)(strstr(trace, "script x1") - trace);
    size_t go_length = (size_t)(strstr(trace + go_start, "script back") - (trace + go_start));
    memcpy(first_go, trace + go_start, go_length);
    CHECK(trace_length - warmed >= go_length);
    CHECK(memcmp(trace + warmed, first_go, go_length) == 0);
    CHECK(live_snapshots == 0);
}

/* The block an action's innermost enclosing handler or transition is:
 * follows <if> and <foreach> owners out to it. */
static ta_action_info root_block(run *r, ta_action_id action) {
    ta_action_info info = {0};
    for (uint32_t depth = 0; depth < 16u; depth += 1u) {
        CHECK(ta_definition_action(r->system, &(ta_definition_action_params){ .definition = r->definition,
                                                                               .action = action, .out = &info }) ==
              TA_OK);
        if (info.block != TA_ACTION_IN_THEN && info.block != TA_ACTION_IN_ELSE && info.block != TA_ACTION_IN_FOREACH) {
            break;
        }
        action = info.owner;
    }
    return info;
}

/*
 * What the position names (ADR 0083), at action budget 1, where the run
 * pauses before every unit: the next unit's action sits in the block the
 * phase and state say, and a <foreach> whose next iteration is next is
 * named with that iteration. resume_once() checks that each equals the
 * PAUSED record.
 */
static void test_position_names_the_next_unit(void) {
    trace_length = 0;
    run r;
    start(&r, 1);
    settle(&r);
    deliver(&r, "go");
    ta_run_status status = resume_once(&r);
    /* The first unit, `x1`, ran: next is the <if> after it in a's first <onexit>. */
    CHECK(status == TA_RUN_PAUSED);
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(r.system, r.actor, &inspection) == TA_OK);
    CHECK(inspection.position.at == TA_TRACE_AT_EXIT && inspection.position.state == 0);
    CHECK(inspection.step.macrostep == 2 && inspection.step.microstep == 1);
    ta_action_info info = {0};
    CHECK(ta_definition_action(r.system, &(ta_definition_action_params){ .definition = r.definition,
                                                                         .action = inspection.position.action,
                                                                         .out = &info }) == TA_OK);
    CHECK(info.kind == TA_SCXML_ACTION_IF && info.block == TA_ACTION_IN_ONEXIT && info.owner == 0 &&
          info.block_index == 0 && info.index == 1);

    uint32_t next_iterations = 0;
    uint32_t seen[4] = {0}; /* pauses in EXIT, TRANSITION, ENTER, elsewhere */
    for (uint32_t calls = 0; status == TA_RUN_PAUSED && calls < 1000u; calls += 1u) {
        CHECK(ta_actor_inspect(r.system, r.actor, &inspection) == TA_OK);
        ta_trace_position at = inspection.position;
        ta_action_info root = root_block(&r, at.action);
        if (at.at == TA_TRACE_AT_EXIT) {
            seen[0] += 1u;
            CHECK(root.block == TA_ACTION_IN_ONEXIT && root.owner == at.state);
        } else if (at.at == TA_TRACE_AT_TRANSITION) {
            seen[1] += 1u;
            CHECK(root.block == TA_ACTION_IN_TRANSITION && root.owner == at.transition);
            ta_transition_info transition = {0};
            CHECK(ta_definition_transition(r.system, &(ta_definition_transition_params){
                                                         .definition = r.definition,
                                                         .transition = at.transition,
                                                         .out = &transition }) == TA_OK);
            CHECK(transition.source == at.state);
        } else if (at.at == TA_TRACE_AT_ENTER) {
            seen[2] += 1u;
            CHECK(root.block == TA_ACTION_IN_ONENTRY && root.owner == at.state);
        } else {
            seen[3] += 1u;
        }
        CHECK(ta_definition_action(r.system, &(ta_definition_action_params){ .definition = r.definition,
                                                                             .action = at.action,
                                                                             .out = &info }) == TA_OK);
        next_iterations += info.kind == TA_SCXML_ACTION_FOREACH && at.iteration > 0 ? 1u : 0u;
        CHECK(at.iteration < 3u);
        status = resume_once(&r);
    }
    CHECK(status == TA_RUN_STABLE);
    CHECK(seen[0] > 5u && seen[1] > 5u && seen[2] > 5u && seen[3] == 0);
    CHECK(next_iterations > 3u);
    CHECK(r.pauses == r.pauses_before_unit);
    finish(&r);
    CHECK(live_snapshots == 0);
}

/* A microstep budget pauses between microsteps: the key counts the
 * microsteps done, and the position is nowhere. */
static void test_position_between_microsteps(void) {
    trace_length = 0;
    run r;
    start(&r, 0);
    r.budget = 1;
    script(&r);
    CHECK(r.pauses >= 4u);
    CHECK(r.microstep_pauses == 0 && r.pauses_before_unit == 0);
    finish(&r);
}

int main(void) {
    test_every_budget_is_invisible();
    test_capture_inside_foreach();
    test_pause_needs_no_allocation();
    test_position_names_the_next_unit();
    test_position_between_microsteps();
    CHECK(heap.stats.live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "pause test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core pause test passed");
    return 0;
}
