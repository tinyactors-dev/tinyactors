/*
 * Operator transitions and abandoning a macrostep (ADR 0082): a jump runs
 * through the normal engine as the first microstep of its macrostep, in
 * any chart; ta_actor_deliver() refuses a malformed one and retains
 * nothing, and admits a well-formed one, which waits behind the inputs and
 * the macrostep before it (ADR 0099); a pause inside it resumes, and
 * survives capture and reconstruction; one admitted before its session is
 * captured waits in the pool, not in the image, for the session it was
 * admitted for; ta_actor_abandon() leaves an actor idle with a legal
 * configuration, between microsteps only, and has nothing to abandon
 * before an input is taken, when withdrawing it drops it instead; and a
 * run that runs out of memory at any allocation ends as one that did not.
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
 * A data model that writes every script to a log, with the event `_event`
 * names: "@J" for the operator's event, "@G" for "go", "@-" for none.
 */

static char script_log[8192];
static size_t script_length = 0;
static ta_event_name_id jump_name = 0;
static ta_event_name_id go_name = 0;

static void note(const char *what) {
    int written = snprintf(script_log + script_length, sizeof(script_log) - script_length, "%s ", what);
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
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->script);
    const ta_event *event = ta_dm_host_event(params->host);
    const char *seen = event == NULL ? "@-" : event->name == jump_name ? "@J" : event->name == go_name ? "@G" : "@?";
    char line[64];
    snprintf(line, sizeof(line), "%.*s%s", (int)text.len, text.ptr, seen);
    note(line);
    return TA_OP_OK;
}
static ta_op_status dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->array);
    *params->out = (ta_value){ .self = &snapshot_token, .v = &snapshot_vtable };
    live_snapshots += 1;
    *params->count = (uint32_t)(text.ptr[0] - '0');
    return TA_OP_OK;
}
static ta_op_status dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("operator"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .execute_script = dm_execute_script,
    .foreach_snapshot = dm_foreach_snapshot,
    .foreach_bind = dm_foreach_bind,
};

/* ------------------------------------------------------------------------
 * The chart. Nothing in it was written for an operator:
 *
 *   idle  --go-->  work (w1 --next--> w2, wh: deep history, default w1)
 *                  par  (r1: r1a | r1b,  r2: r2a | r2b)
 *   idle  --spin-> loop (l1 <-> l2, eventless, forever; l1 raises "tick")
 *                  end  (final)
 */

#define SCRIPT(text) { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR(text) }
#define END {0}
#define HANDLERS(name, text)                                                 \
    static const ta_scxml_action_spec name##_actions[] = { SCRIPT(text), END }; \
    static const ta_scxml_handler_spec name[] = { { name##_actions }, END }

HANDLERS(idle_entry, "e.idle");
HANDLERS(work_entry, "e.work");
HANDLERS(work_exit, "x.work");
HANDLERS(w2_entry, "e.w2");
HANDLERS(par_entry, "e.par");
HANDLERS(r1a_entry, "e.r1a");
HANDLERS(r1b_entry, "e.r1b");
HANDLERS(r2a_entry, "e.r2a");
HANDLERS(r2a_exit, "x.r2a");
HANDLERS(r2b_entry, "e.r2b");
HANDLERS(end_entry, "e.end");

static const ta_scxml_action_spec w1_body[] = { SCRIPT("w1.body"), END };
static const ta_scxml_action_spec w1_entry_actions[] = {
    SCRIPT("e.w1"),
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("i"), .body = w1_body } },
    SCRIPT("e.w1.end"),
    END,
};
static const ta_scxml_handler_spec w1_entry[] = { { w1_entry_actions }, END };

static const ta_scxml_action_spec l1_entry_actions[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("tick") } },
    END,
};
static const ta_scxml_handler_spec l1_entry[] = { { l1_entry_actions }, END };

static const ta_str to_work[] = { STR("work"), {0} };
static const ta_str to_loop[] = { STR("loop"), {0} };
static const ta_str to_w1[] = { STR("w1"), {0} };
static const ta_str to_w2[] = { STR("w2"), {0} };
static const ta_str to_l1[] = { STR("l1"), {0} };
static const ta_str to_l2[] = { STR("l2"), {0} };

static const ta_scxml_transition_spec idle_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_work },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("spin"), .targets = to_loop },
    END,
};
static const ta_scxml_transition_spec w1_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("next"), .targets = to_w2 },
    END,
};
static const ta_scxml_transition_spec wh_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_w1 },
    END,
};
static const ta_scxml_transition_spec l1_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_l2 },
    END,
};
static const ta_scxml_transition_spec l2_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_l1 },
    END,
};

static const ta_scxml_state_spec work_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("w1"), .onentry = w1_entry, .transitions = w1_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("w2"), .onentry = w2_entry },
    { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = STR("wh"), .transitions = wh_transitions },
    END,
};
static const ta_scxml_state_spec r1_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r1a"), .onentry = r1a_entry },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r1b"), .onentry = r1b_entry },
    END,
};
static const ta_scxml_state_spec r2_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r2a"), .onentry = r2a_entry, .onexit = r2a_exit },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("r2b"), .onentry = r2b_entry },
    END,
};
static const ta_scxml_state_spec par_children[] = {
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("r1"), .states = r1_children },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("r2"), .states = r2_children },
    END,
};
static const ta_scxml_state_spec loop_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("l1"), .onentry = l1_entry, .transitions = l1_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("l2"), .transitions = l2_transitions },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .onentry = idle_entry, .transitions = idle_transitions },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("work"), .onentry = work_entry, .onexit = work_exit,
      .states = work_children },
    { .kind = TA_SCXML_STATE_PARALLEL, .id = STR("par"), .onentry = par_entry, .states = par_children },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("loop"), .states = loop_children },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end"), .onentry = end_entry },
    END,
};
static const ta_scxml_spec chart = { .name = STR("operator"), .states = roots };

/* ------------------------------------------------------------------------
 * A trace sink that keeps the records a test looks at
 */

typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    bool has_event;
    char event[48];
    uint32_t transition_count;
    ta_state_id state;
    uint32_t state_count;
    ta_state_id states[8];
    uint32_t exited_count;
    uint32_t entered_count;
    ta_state_id entered[8];
    uint32_t dropped;
} kept_record;

static kept_record records[512];
static uint32_t record_count = 0;

static void sink_write(void *self, const ta_trace_record *record) {
    (void)self;
    if (record_count == sizeof(records) / sizeof(records[0])) {
        return;
    }
    kept_record *kept = &records[record_count++];
    *kept = (kept_record){ .kind = record->kind, .key = record->key, .has_event = record->has_event,
                           .transition_count = record->transition_count, .state = record->state,
                           .state_count = record->state_count, .exited_count = record->exited_count,
                           .entered_count = record->entered_count, .dropped = record->dropped };
    ta_str event_name = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME);
    if (record->has_event && event_name.len < sizeof kept->event) {
        memcpy(kept->event, event_name.ptr, event_name.len);
    }
    for (uint32_t index = 0; index < record->state_count && index < 8u; index += 1u) {
        kept->states[index] = record->states[index];
    }
    for (uint32_t index = 0; index < record->entered_count && index < 8u; index += 1u) {
        kept->entered[index] = record->entered[index];
    }
}

/* The system whose records `records` keeps. */
static ta_system *recorded_system = NULL;

/* Whether a kept record's event is `name`, read as text. */
static bool kept_event_is(const kept_record *kept, ta_event_name_id name) {
    char text[48];
    uint32_t needed = 0;
    return ta_event_name_format(recorded_system, &(ta_event_name_format_params){ .name = name, .buffer = text,
                                                                                 .capacity = sizeof text,
                                                                                 .needed = &needed }) == TA_OK &&
           strcmp(kept->event, text) == 0;
}

/* The index of the first record of `kind` from `from` on, or record_count. */
static uint32_t find_record(ta_trace_kind kind, uint32_t from) {
    while (from < record_count && records[from].kind != kind) {
        from += 1u;
    }
    return from;
}

/* The records as text, without PAUSED, RESUMED and STOPPED (ADR 0067). */
static void records_text(char *out, size_t size) {
    size_t length = 0;
    out[0] = 0;
    for (uint32_t index = 0; index < record_count; index += 1u) {
        const kept_record *r = &records[index];
        if (r->kind == TA_TRACE_PAUSED || r->kind == TA_TRACE_RESUMED || r->kind == TA_TRACE_STOPPED) {
            continue;
        }
        int written = snprintf(out + length, size - length, "%u:%llu.%u/%u/%u/%u/%u/%u;", (unsigned)r->kind,
                               (unsigned long long)r->key.macrostep, r->key.microstep, r->transition_count,
                               r->state_count, r->exited_count, r->entered_count, r->dropped);
        if (written > 0 && length + (size_t)written < size) {
            length += (size_t)written;
        }
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

static ta_event_name_id event_name(run *r, const char *name) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(r->system, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &atom) == TA_OK);
    ta_event_name_id id = 0;
    CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &id) == TA_OK);
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

/* A system with the chart, a recording trace sink, and one actor, not yet
 * bootstrapped. */
static void start(run *r) {
    *r = (run){0};
    /* Room on tel for every record of a runaway's fifty microsteps. */
    next_param(TA_PARAM_TEL_BYTES, 1u << 20);
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
    recorded_system = r->system;
    jump_name = event_name(r, "operator.goto");
    go_name = event_name(r, "go");
    record_count = 0;
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
    tel_drain(r->system, sink_write, NULL);
}

/* One run that answers core's own result: the tests of runaways and of
 * running out of memory assert it, the code of the fault that stopped the
 * input. A stopped input goes on at the next run. */
static ta_result resume_with(run *r, uint32_t budget, uint32_t action_budget, test_drive_status *status) {
    ta_result result = test_drive_run(r->system, r->actor,
                                      &(test_drive_run_params){ .budget = budget, .status = status,
                                                                .action_budget = action_budget });
    tel_drain(r->system, sink_write, NULL);
    return result;
}

/* Abandons the actor's macrostep (ADR 0082) and reads what it wrote. */
static ta_result abandon(run *r) {
    ta_result result = ta_actor_abandon(r->system, r->actor);
    tel_drain(r->system, sink_write, NULL);
    return result;
}

/* Destroys the actor and reads what it wrote. */
static ta_result destroy(run *r) {
    ta_result result = ta_actor_destroy(r->system, r->actor);
    tel_drain(r->system, sink_write, NULL);
    return result;
}

/* One run with these budgets, and reads what it wrote. */
static ta_result run_for(run *r, uint32_t budget, uint32_t action_budget, test_drive_status *status) {
    ta_result result = test_drive_run(r->system, r->actor,
                                      &(test_drive_run_params){ .budget = budget, .status = status,
                                                                .action_budget = action_budget });
    tel_drain(r->system, sink_write, NULL);
    return result;
}

static test_drive_status resume(run *r, uint32_t action_budget) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(run_for(r, 1000, action_budget, &status) == TA_OK);
    return status;
}

/* Resumes until the actor is stable or done, pausing as the budget says. */
static test_drive_status settle(run *r, uint32_t action_budget) {
    test_drive_status status = TEST_DRIVE_ERROR;
    for (uint32_t calls = 0; calls < 10000u; calls += 1u) {
        status = resume(r, action_budget);
        if (status != TEST_DRIVE_PAUSED) {
            break;
        }
    }
    return status;
}

/* Admits an event, with the operator transition if not null, under the
 * host's `correlation`, which ta_system_withdraw_input() names it by. */
static ta_result offer_as(run *r, const char *name, const ta_operator_transition *transition, uint64_t correlation) {
    ta_event event = { .name = event_name(r, name), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    ta_result result = ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system),
                                                                               .actor = r->actor, .event = &event,
                                                                               .transition = transition,
                                                                               .correlation = correlation });
    tel_drain(r->system, sink_write, NULL);
    return result;
}

static ta_result offer(run *r, const char *name, const ta_operator_transition *transition) {
    return offer_as(r, name, transition, 0);
}

/* Delivers the operator's event with a jump from `source` to `targets`. */
static ta_result jump(run *r, ta_state_id source, const ta_state_id *targets, uint32_t count) {
    return offer(r, "operator.goto",
                 &(ta_operator_transition){ .source = source, .targets = targets, .target_count = count });
}

/* The active configuration as author IDs, space separated. */
static void active_text(run *r, char *out, size_t size) {
    ta_state_id states[32];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(r->system, &(ta_actor_active_states_params){ .actor = r->actor, .states = states,
                                                                             .capacity = 32, .count = &count }) ==
          TA_OK);
    size_t length = 0;
    out[0] = 0;
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_state_info info = {0};
        CHECK(ta_definition_state(r->system, &(ta_definition_state_params){ .definition = r->definition,
                                                                           .state = states[index],
                                                                           .out = &info }) == TA_OK);
        int written = snprintf(out + length, size - length, "%s%.*s", index == 0 ? "" : " ", (int)info.id.len,
                               info.id.ptr);
        if (written > 0 && length + (size_t)written < size) {
            length += (size_t)written;
        }
    }
}

static int active_is(run *r, const char *expected) {
    char text[256];
    active_text(r, text, sizeof(text));
    if (strcmp(text, expected) != 0) {
        fprintf(stderr, "active: %s\nexpected: %s\n", text, expected);
        return 0;
    }
    return 1;
}

static int log_is(const char *expected) {
    if (strcmp(script_log, expected) != 0) {
        fprintf(stderr, "scripts: %s\nexpected: %s\n", script_log, expected);
        return 0;
    }
    return 1;
}

static ta_actor_inspection inspect(run *r) {
    ta_actor_inspection inspection;
    CHECK(ta_actor_inspect(r->system, r->actor, &inspection) == TA_OK);
    return inspection;
}

static void finish(run *r) {
    ta_system_destroy(r->system);
    CHECK(heap.live == 0);
    CHECK(live_snapshots == 0);
}

/* ------------------------------------------------------------------------
 * Jumps
 */

static void test_jump_runs_the_engine(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    ta_state_id w2 = state(&r, "w2");
    clear_log();
    uint32_t first = record_count;
    CHECK(jump(&r, TA_STATE_NONE, &w2, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    /* Entry handlers ran and saw the operator's event as _event. */
    CHECK(log_is("e.work@J e.w2@J "));

    uint32_t begin = find_record(TA_TRACE_MACROSTEP_BEGIN, first);
    CHECK(begin < record_count && records[begin].has_event && kept_event_is(&records[begin], jump_name));
    uint32_t micro = find_record(TA_TRACE_MICROSTEP, first);
    CHECK(micro < record_count);
    if (micro < record_count) {
        const kept_record *m = &records[micro];
        CHECK(m->has_event && kept_event_is(m, jump_name));
        CHECK(m->transition_count == 0);
        CHECK(m->state == TA_STATE_NONE);
        CHECK(m->state_count == 1 && m->states[0] == w2);
        CHECK(m->exited_count == 1);
        CHECK(m->entered_count == 2 && m->entered[0] == state(&r, "work") && m->entered[1] == w2);
        CHECK(m->key.microstep == 1);
    }
    CHECK(find_record(TA_TRACE_MICROSTEP, micro + 1u) == record_count); /* nothing else ran */
    CHECK(find_record(TA_TRACE_MACROSTEP_END, first) < record_count);

    /* The chart goes on from there as if it had got there itself. */
    clear_log();
    CHECK(offer(&r, "go", NULL) == TA_OK); /* idle's transition: not active, ignored */
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    finish(&r);
}

static void test_history_target(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer(&r, "go", NULL) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer(&r, "next", NULL) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));

    /* Out: leaving work records its deep history. */
    ta_state_id idle = state(&r, "idle");
    clear_log();
    CHECK(jump(&r, TA_STATE_NONE, &idle, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    CHECK(log_is("x.work@J e.idle@J "));

    /* Back through the history state: w2 again, not the default w1. */
    ta_state_id wh = state(&r, "wh");
    clear_log();
    uint32_t first = record_count;
    CHECK(jump(&r, TA_STATE_NONE, &wh, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    CHECK(log_is("e.work@J e.w2@J "));
    uint32_t micro = find_record(TA_TRACE_MICROSTEP, first);
    CHECK(micro < record_count && records[micro].state_count == 1 && records[micro].states[0] == wh);
    finish(&r);
}

static void test_parallel_region(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    ta_state_id both[] = { state(&r, "r1b"), state(&r, "r2a") };
    clear_log();
    CHECK(jump(&r, TA_STATE_NONE, both, 2) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "par r1 r1b r2 r2a"));
    CHECK(log_is("e.par@J e.r1b@J e.r2a@J "));

    /* A region as the source moves that region only: r2 is not exited. */
    ta_state_id r1a = state(&r, "r1a");
    clear_log();
    uint32_t first = record_count;
    CHECK(jump(&r, state(&r, "r1"), &r1a, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "par r1 r1a r2 r2a"));
    CHECK(log_is("e.r1a@J "));
    uint32_t micro = find_record(TA_TRACE_MICROSTEP, first);
    CHECK(micro < record_count && records[micro].state == state(&r, "r1") && records[micro].exited_count == 1);

    /* One target in a region: the other region enters its default. */
    ta_state_id r2b = state(&r, "r2b");
    clear_log();
    CHECK(jump(&r, TA_STATE_NONE, &r2b, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "par r1 r1a r2 r2b"));
    CHECK(log_is("x.r2a@J e.par@J e.r1a@J e.r2b@J "));
    finish(&r);
}

static void test_final_target(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    ta_state_id end = state(&r, "end");
    clear_log();
    CHECK(jump(&r, TA_STATE_NONE, &end, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_DONE);
    CHECK(log_is("e.end@J "));
    ta_actor_inspection inspection = inspect(&r);
    CHECK(inspection.residency == TA_ACTOR_DONE);
    CHECK(inspection.final_state == end);
    CHECK(find_record(TA_TRACE_FINISHED, 0) < record_count);

    /* A finished actor: done, after a malformed jump is refused as one. */
    CHECK(jump(&r, TA_STATE_NONE, &end, 1) == TA_ERR_DONE);
    CHECK(jump(&r, TA_STATE_NONE, &end, 0) == TA_ERR_INVALID);
    CHECK(abandon(&r) == TA_ERR_DONE);
    finish(&r);
}

static void test_refusals(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    long live = heap.live;
    ta_state_id w1 = state(&r, "w1");
    ta_state_id w2 = state(&r, "w2");
    ta_state_id wh = state(&r, "wh");
    ta_state_id work = state(&r, "work");
    ta_state_id idle = state(&r, "idle");
    ta_state_id r1a = state(&r, "r1a");
    ta_state_id r1b = state(&r, "r1b");
    ta_state_id unknown = 1000u;

    CHECK(jump(&r, TA_STATE_NONE, &w1, 0) == TA_ERR_INVALID);                  /* no targets */
    CHECK(jump(&r, TA_STATE_NONE, NULL, 1) == TA_ERR_INVALID);                 /* no list */
    CHECK(jump(&r, TA_STATE_NONE, &unknown, 1) == TA_ERR_INVALID);             /* unknown target */
    CHECK(jump(&r, unknown, &w1, 1) == TA_ERR_INVALID);                        /* unknown source */
    CHECK(jump(&r, w1, &w2, 1) == TA_ERR_INVALID);                             /* source not active */
    CHECK(jump(&r, wh, &w2, 1) == TA_ERR_INVALID);                             /* a history state is never active */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ w1, w2 }, 2) == TA_ERR_INVALID);    /* one compound */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ work, w2 }, 2) == TA_ERR_INVALID);  /* an ancestor */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ w1, w1 }, 2) == TA_ERR_INVALID);    /* twice */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ wh, w1 }, 2) == TA_ERR_INVALID);    /* history's region */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ idle, r1a }, 2) == TA_ERR_INVALID); /* the root is compound */
    CHECK(jump(&r, TA_STATE_NONE, (ta_state_id[]){ r1a, r1b }, 2) == TA_ERR_INVALID);  /* one region */

    /* Nothing was retained: the actor is idle and takes the next event. */
    CHECK(heap.live == live);
    ta_actor_inspection inspection = inspect(&r);
    CHECK(!inspection.macrostep_in_progress);
    CHECK(inspection.current_event == NULL);
    CHECK(offer(&r, "go", NULL) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w1"));
    /* The source is the root or an active state; the targets the chart's. */
    CHECK(jump(&r, work, &w2, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    finish(&r);
}

/* A jump admitted while the actor is busy waits for what is before it, in
 * the order it was admitted (ADR 0099), and runs from the configuration
 * it then finds; each run takes one input. */
static void test_busy(void) {
    run r;
    start(&r);
    ta_state_id w2 = state(&r, "w2");
    ta_state_id idle = state(&r, "idle");
    /* Spawned and not yet bootstrapped: it waits behind the bootstrap. */
    CHECK(jump(&r, TA_STATE_NONE, &w2, 1) == TA_OK);
    CHECK(resume(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    CHECK(resume(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    CHECK(jump(&r, TA_STATE_NONE, &idle, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    /* Behind an admitted event, and behind that event's macrostep paused
     * inside a microstep. */
    CHECK(offer(&r, "go", NULL) == TA_OK);
    CHECK(jump(&r, TA_STATE_NONE, &w2, 1) == TA_OK);
    CHECK(resume(&r, 1) == TEST_DRIVE_PAUSED);
    CHECK(inspect(&r).microstep_in_progress);
    CHECK(jump(&r, TA_STATE_NONE, &idle, 1) == TA_OK);
    /* Malformed is invalid whatever the actor's state. */
    CHECK(jump(&r, TA_STATE_NONE, &w2, 0) == TA_ERR_INVALID);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w1"));
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    finish(&r);
}

/* The log and configuration of a jump into w1, whose <onentry> has four
 * units, with a pause before each of them when `action_budget` is 1. */
static void jump_into_w1(run *r, uint32_t action_budget, int capture_at) {
    start(r);
    CHECK(settle(r, 0) == TEST_DRIVE_STABLE);
    ta_state_id w1 = state(r, "w1");
    clear_log();
    CHECK(jump(r, TA_STATE_NONE, &w1, 1) == TA_OK);
    if (capture_at == 0) {
        /* Before it is taken the jump waits in the pool, not in the session
         * (ADR 0099): the image holds no input, and the jump, admitted for
         * the session that is gone, is refused when it is reached. The
         * reconstructed session takes it again. */
        ta_session_image *image = NULL;
        CHECK(ta_actor_capture(r->system, &(ta_actor_capture_params){ .actor = r->actor, .out = &image }) == TA_OK);
        ta_actor_id gone = r->actor;
        CHECK(destroy(r) == TA_OK);
        CHECK(ta_actor_reconstruct(r->system, &(ta_actor_reconstruct_params){ .image = image, .out = &r->actor }) ==
              TA_OK);
        ta_session_image_destroy(image);
        ta_input_outcome refused = { .kind = TA_INPUT_TAKEN };
        uint32_t count = 0;
        ta_run_status ran = TA_RUN_MORE;
        CHECK(ta_run(r->system, &(ta_run_params){ .budget = 1000, .outcomes = &refused, .outcome_capacity = 1,
                                                  .outcome_count = &count, .status = &ran }) == TA_OK);
        CHECK(count == 1 && refused.kind == TA_INPUT_REFUSED && refused.code == TA_ERR_NOT_FOUND &&
              refused.actor == gone && ran == TA_RUN_IDLE);
        CHECK(active_is(r, "idle"));
        CHECK(jump(r, TA_STATE_NONE, &w1, 1) == TA_OK);
    }
    uint32_t pauses = 0;
    test_drive_status status = TEST_DRIVE_ERROR;
    while ((status = resume(r, action_budget)) == TEST_DRIVE_PAUSED) {
        pauses += 1u;
        ta_actor_inspection inspection = inspect(r);
        CHECK(inspection.microstep_in_progress);
        /* The configuration is the one before the jump until it completes. */
        CHECK(active_is(r, "idle"));
        CHECK(abandon(r) == TA_ERR_BUSY);
        /* An event admitted now waits for the jump's macrostep to end. */
        CHECK(offer_as(r, "go", NULL, 100u + pauses) == TA_OK);
        if ((int)pauses == capture_at) {
            ta_session_image *image = NULL;
            CHECK(ta_actor_capture(r->system, &(ta_actor_capture_params){ .actor = r->actor, .out = &image }) ==
                  TA_OK);
            CHECK(destroy(r) == TA_OK);
            CHECK(ta_actor_reconstruct(r->system, &(ta_actor_reconstruct_params){ .image = image,
                                                                                 .out = &r->actor }) == TA_OK);
            ta_session_image_destroy(image);
            CHECK(inspect(r).microstep_in_progress);
        }
    }
    CHECK(status == TEST_DRIVE_STABLE);
    CHECK(action_budget == 0 || pauses >= 4u);
    CHECK(active_is(r, "work w1"));
    /* The events admitted during the pauses were never taken. */
    for (uint32_t pause = 1; pause <= pauses; pause += 1u) {
        CHECK(ta_system_withdraw_input(r->system, 100u + pause) == TA_OK);
    }
}

static void test_pause_inside_the_jump(void) {
    run r;
    jump_into_w1(&r, 0, -1);
    char baseline[sizeof(script_log)];
    memcpy(baseline, script_log, script_length + 1u);
    CHECK(log_is("e.work@J e.w1@J w1.body@J w1.body@J e.w1.end@J "));
    finish(&r);

    jump_into_w1(&r, 1, -1);
    CHECK(log_is(baseline));
    finish(&r);

    /* Captured inside the jump's <foreach>, destroyed, reconstructed. */
    for (int at = 1; at <= 4; at += 1) {
        jump_into_w1(&r, 1, at);
        CHECK(log_is(baseline));
        finish(&r);
    }
    /* Captured after delivery, before the jump was taken. */
    jump_into_w1(&r, 0, 0);
    CHECK(log_is(baseline));
    finish(&r);
}

/* ------------------------------------------------------------------------
 * Abandoning
 */

static void test_abandon_a_macrostep_that_never_ends(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(abandon(&r) == TA_ERR_INVALID); /* idle: nothing to abandon */
    CHECK(offer(&r, "spin", NULL) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(run_for(&r, 21, 0, &status) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    CHECK(run_for(&r, 20, 0, &status) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    ta_actor_inspection inspection = inspect(&r);
    CHECK(inspection.macrostep_in_progress && !inspection.microstep_in_progress);
    uint32_t waiting = inspection.internal_count;
    CHECK(waiting >= 10u);
    ta_state_id jump_to = state(&r, "w2");
    /* Admitted, it waits for a macrostep that never ends. */
    CHECK(jump(&r, TA_STATE_NONE, &jump_to, 1) == TA_OK);

    uint32_t first = record_count;
    CHECK(abandon(&r) == TA_OK);
    uint32_t abandoned = find_record(TA_TRACE_ABANDONED, first);
    CHECK(abandoned < record_count);
    if (abandoned < record_count) {
        CHECK(records[abandoned].dropped == waiting);
        CHECK(records[abandoned].has_event && kept_event_is(&records[abandoned], event_name(&r, "spin")));
        CHECK(abandoned + 1u < record_count && records[abandoned + 1u].kind == TA_TRACE_MACROSTEP_END);
        CHECK(records[abandoned + 1u].key.macrostep == records[abandoned].key.macrostep);
        CHECK(records[abandoned + 1u].state_count == 2);
    }
    inspection = inspect(&r);
    CHECK(inspection.residency == TA_ACTOR_IDLE);
    CHECK(!inspection.macrostep_in_progress);
    CHECK(inspection.internal_count == 0);
    CHECK(inspection.current_event == NULL);
    char active[64];
    active_text(&r, active, sizeof(active));
    CHECK(strcmp(active, "loop l1") == 0 || strcmp(active, "loop l2") == 0);
    CHECK(abandon(&r) == TA_ERR_INVALID);

    /* Then the jump that waited runs as usual, and the actor is out of the loop. */
    clear_log();
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));
    CHECK(log_is("e.work@J e.w2@J "));

    CHECK(destroy(&r) == TA_OK);
    CHECK(abandon(&r) == TA_ERR_NOT_FOUND);
    finish(&r);
}

static void test_abandon_only_between_microsteps(void) {
    run r;
    start(&r);
    /* Its bootstrap has not completed a microstep: no configuration yet. */
    CHECK(abandon(&r) == TA_ERR_BUSY);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer(&r, "spin", NULL) == TA_OK);
    /* Paused inside l1's <onentry>, which raises. */
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(run_for(&r, 1000, 1, &status) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    CHECK(inspect(&r).microstep_in_progress);
    CHECK(abandon(&r) == TA_ERR_BUSY);
    /* A budget of one microstep and no action budget finishes it. */
    CHECK(run_for(&r, 1, 0, &status) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    CHECK(!inspect(&r).microstep_in_progress);
    CHECK(abandon(&r) == TA_OK);
    CHECK(active_is(&r, "loop l1"));
    finish(&r);
}

/*
 * An input admitted and not yet taken waits in the pool, not in the
 * session (ADR 0099): there is no macrostep to abandon, nothing is written,
 * and withdrawing it drops it instead (decision 11), with its operator
 * transition.
 */
static void test_abandon_before_the_event_is_taken(void) {
    run r;
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer_as(&r, "go", NULL, 1) == TA_OK);
    uint32_t first = record_count;
    CHECK(abandon(&r) == TA_ERR_INVALID);
    CHECK(record_count == first);
    CHECK(!inspect(&r).macrostep_in_progress);
    CHECK(ta_system_withdraw_input(r.system, 1) == TA_OK);
    CHECK(ta_system_withdraw_input(r.system, 1) == TA_ERR_NOT_FOUND);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    CHECK(record_count == first);
    /* A jump: its operator transition goes with the event. */
    ta_state_id w2 = state(&r, "w2");
    CHECK(offer_as(&r, "operator.goto", &(ta_operator_transition){ .source = TA_STATE_NONE, .targets = &w2,
                                                                    .target_count = 1 },
                   2) == TA_OK);
    CHECK(abandon(&r) == TA_ERR_INVALID);
    CHECK(ta_system_withdraw_input(r.system, 2) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "idle"));
    finish(&r); /* checks nothing leaked */
}

/* ------------------------------------------------------------------------
 * Out of memory (ADR 0070): every allocation of a jump fails in turn
 */

/* Delivers a jump into w1 with a pause before every unit, and settles,
 * retrying each TA_ERR_NOMEM: from deliver with nothing retained, from a
 * run with the actor still in its macrostep, or with the jump still
 * waiting, untaken, when the run could not take it. */
static void jump_retrying(run *r) {
    ta_state_id w1 = state(r, "w1");
    ta_result result;
    while ((result = jump(r, TA_STATE_NONE, &w1, 1)) == TA_ERR_NOMEM) {
        CHECK(!inspect(r).macrostep_in_progress);
    }
    CHECK(result == TA_OK);
    for (uint32_t calls = 0; calls < 1000u; calls += 1u) {
        (void)ta_actor_release(r->system, r->actor); /* a fault stopped it: it goes on */
        ta_input_outcome outcome = { .kind = TA_INPUT_TAKEN };
        uint32_t count = 0;
        ta_run_status status = TA_RUN_IDLE;
        result = ta_run(r->system, &(ta_run_params){ .budget = 1000, .action_budget = 1, .session = r->actor,
                                                     .outcomes = &outcome, .outcome_capacity = 1,
                                                     .outcome_count = &count, .status = &status });
        tel_drain(r->system, sink_write, NULL);
        if (result == TA_ERR_NOMEM && count == 0) {
            /* The jump could not be taken: it waits, untaken, and the
             * actor is between macrosteps. */
            ta_actor_inspection view = inspect(r);
            CHECK(!view.macrostep_in_progress && view.current_event == NULL);
            continue;
        }
        CHECK(result == TA_OK);
        if (count == 1 && outcome.kind == TA_INPUT_FAULTED) {
            /* Out of memory inside the macrostep: stopped where it stands. */
            CHECK(outcome.code == TA_ERR_NOMEM);
            CHECK(inspect(r).macrostep_in_progress);
            continue;
        }
        if (status != TA_RUN_PAUSED) {
            CHECK(count == 1 && outcome.kind == TA_INPUT_COMPLETED);
            break;
        }
    }
}

static void test_out_of_memory(void) {
    run r;
    /* No cached event blocks: the jump's event takes a new one, which grows
     * inside the jump's microstep, before its first unit (ADR 0070). */
    next_param(TA_PARAM_EVENT_BLOCK_CACHE, 0);
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    clear_log();
    uint32_t first = record_count;
    unsigned long before = heap.attempts;
    jump_retrying(&r);
    unsigned long used = heap.attempts - before;
    char baseline_log[sizeof(script_log)];
    memcpy(baseline_log, script_log, script_length + 1u);
    char baseline_records[4096];
    record_count -= first;
    memmove(records, records + first, record_count * sizeof(records[0]));
    records_text(baseline_records, sizeof(baseline_records));
    CHECK(active_is(&r, "work w1"));
    finish(&r);
    CHECK(used >= 3u); /* the operator transition, the event block, and its growth */

    char text[4096];
    for (unsigned long failing = 1; failing <= used; failing += 1u) {
        next_param(TA_PARAM_EVENT_BLOCK_CACHE, 0);
        start(&r);
        CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
        clear_log();
        first = record_count;
        heap.policy.fail_at = heap.attempts + failing;
        jump_retrying(&r);
        heap.policy.fail_at = 0;
        CHECK(log_is(baseline_log));
        CHECK(active_is(&r, "work w1"));
        record_count -= first;
        memmove(records, records + first, record_count * sizeof(records[0]));
        records_text(text, sizeof(text));
        if (strcmp(text, baseline_records) != 0) {
            fprintf(stderr, "allocation %lu failing: records\n  %s\nexpected\n  %s\n", failing, text,
                    baseline_records);
            failures += 1;
        }
        finish(&r);
    }

    /* Abandoning needs scratch memory; without it, nothing changes. */
    for (unsigned long failing = 1; failing <= 4u; failing += 1u) {
        start(&r);
        CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
        CHECK(offer(&r, "spin", NULL) == TA_OK);
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(run_for(&r, 7, 0, &status) == TA_OK && status == TEST_DRIVE_PAUSED);
        uint32_t waiting = inspect(&r).internal_count;
        heap.policy.fail_at = heap.attempts + failing;
        ta_result result = abandon(&r);
        heap.policy.fail_at = 0;
        if (result == TA_ERR_NOMEM) {
            ta_actor_inspection inspection = inspect(&r);
            CHECK(inspection.macrostep_in_progress && inspection.internal_count == waiting);
            result = abandon(&r);
        }
        CHECK(result == TA_OK);
        CHECK(!inspect(&r).macrostep_in_progress);
        finish(&r);
    }
    printf("operator test: %lu allocations of a paused jump each failed in turn\n", used);
}

/*
 * A macrostep stopped as a runaway (ADR 0075) stands between two microsteps,
 * as after a budget pause, so it can be abandoned; then a jump moves it out
 * of the loop, and the next macrostep has the limit again.
 */
static void test_abandon_a_runaway(void) {
    run r;
    next_param(TA_PARAM_MICROSTEP_LIMIT, 50);
    start(&r);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer(&r, "spin", NULL) == TA_OK);
    test_drive_status status = TEST_DRIVE_PAUSED;
    CHECK(resume_with(&r, 1000, 0, &status) == TA_ERR_RUNAWAY);
    CHECK(status == TEST_DRIVE_ERROR);
    CHECK(find_record(TA_TRACE_RUNAWAY, 0) < record_count);
    ta_actor_inspection inspection = inspect(&r);
    CHECK(inspection.macrostep_in_progress && !inspection.microstep_in_progress);
    ta_state_id jump_to = state(&r, "w2");
    /* Admitted, it waits: a fault stopped the macrostep, which keeps its input. */
    CHECK(jump(&r, TA_STATE_NONE, &jump_to, 1) == TA_OK);

    uint32_t first = record_count;
    CHECK(abandon(&r) == TA_OK);
    uint32_t abandoned = find_record(TA_TRACE_ABANDONED, first);
    CHECK(abandoned < record_count && kept_event_is(&records[abandoned], event_name(&r, "spin")));
    CHECK(abandoned + 1u < record_count && records[abandoned + 1u].kind == TA_TRACE_MACROSTEP_END);
    CHECK(inspect(&r).residency == TA_ACTOR_IDLE);

    /* Then the jump that waited runs. */
    clear_log();
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(active_is(&r, "work w2"));

    /* Back in idle, the next loop is stopped at the limit again. */
    ta_state_id idle = state(&r, "idle");
    CHECK(jump(&r, TA_STATE_NONE, &idle, 1) == TA_OK);
    CHECK(settle(&r, 0) == TEST_DRIVE_STABLE);
    CHECK(offer(&r, "spin", NULL) == TA_OK);
    CHECK(resume_with(&r, 1000, 0, &status) == TA_ERR_RUNAWAY);
    CHECK(abandon(&r) == TA_OK);
    finish(&r);
}

int main(void) {
    test_jump_runs_the_engine();
    test_history_target();
    test_parallel_region();
    test_final_target();
    test_refusals();
    test_busy();
    test_pause_inside_the_jump();
    test_abandon_a_macrostep_that_never_ends();
    test_abandon_only_between_microsteps();
    test_abandon_before_the_event_is_taken();
    test_abandon_a_runaway();
    test_out_of_memory();
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "operator test: %d failures\n", failures);
        return 1;
    }
    printf("operator test: ok\n");
    return 0;
}
