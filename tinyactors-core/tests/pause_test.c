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

static long live_allocations = 0;
static int refuse_all = 0;
/* Set: the next script `x1` starts refusing, inside the microstep, after its
 * queue slot is reserved (ADR 0047), so saving a pause is what gets refused. */
static int refuse_from_x1 = 0;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (refuse_all) {
        return NULL;
    }
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    void *memory = aligned_alloc(alignment, size == 0 ? alignment : size);
    live_allocations += memory != NULL ? 1 : 0;
    return memory;
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) {
        live_allocations -= 1;
    }
    free(params->ptr);
}

static const ta_allocator_vtable allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator allocator = { .self = NULL, .v = &allocator_vtable };

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
    uint32_t action_budget;
    uint32_t pauses;
    uint32_t microstep_pauses;
} run;

static void start(run *r, uint32_t action_budget) {
    *r = (run){ .action_budget = action_budget };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &r->system) == TA_OK);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(r->system, &(ta_definition_compile_params){ .spec = &chart,
                                                                                 .datamodel = { .v = &dm_vtable },
                                                                                 .allocator = allocator,
                                                                                 .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(r->system, definition, &r->definition) == TA_OK);
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
}

/* One resume call; counts pauses, and pauses inside a microstep. */
static ta_run_status resume_once(run *r) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(r->system, &(ta_actor_resume_params){ .actor = r->actor, .budget = 1000, .status = &status,
                                                               .action_budget = r->action_budget }) == TA_OK);
    if (status == TA_RUN_PAUSED) {
        r->pauses += 1u;
        ta_actor_inspection inspection;
        CHECK(ta_actor_inspect(r->system, r->actor, &inspection) == TA_OK);
        CHECK(inspection.macrostep_in_progress);
        r->microstep_pauses += inspection.microstep_in_progress ? 1u : 0u;
        ta_atom atom = 0;
        CHECK(ta_system_intern_atom(r->system, STR("refused"), &atom) == TA_OK);
        ta_event refused = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &refused.name) == TA_OK);
        CHECK(ta_actor_deliver(r->system, r->actor, &refused) == TA_ERR_BUSY);
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
    CHECK(ta_actor_deliver(r->system, r->actor, &event) == TA_OK);
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

/* A pause that cannot save its position does not pause: the resume runs on
 * and the result is the same. Allocations are refused from inside the
 * microstep on, where saving a pause is the only one left. */
static void test_unsaved_pause_keeps_running(void) {
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
    refuse_all = 0;
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

int main(void) {
    test_every_budget_is_invisible();
    test_capture_inside_foreach();
    test_unsaved_pause_keeps_running();
    CHECK(live_allocations == 0);
    if (failures != 0) {
        fprintf(stderr, "pause test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core pause test passed");
    return 0;
}
