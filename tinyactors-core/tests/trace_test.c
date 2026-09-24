/*
 * Tracing (ADR 0067): the null sink a system starts with, the mask that
 * keeps core from building records nobody wants, sampling, exact record
 * sequences for small charts, step keys and their span IDs, and counters
 * and trace parents that survive a session image.
 *
 * Uses the C library; the core does not.
 */
#include <stdarg.h>
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
#define END {0}

/* ------------------------------------------------------------------------
 * An allocator that counts calls and can refuse after a number of them
 */

static long allocations = 0;
static long live_allocations = 0;
/* When positive, the allocation that brings `allocations` to it fails. */
static long refuse_at = 0;

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (refuse_at > 0 && allocations + 1 == refuse_at) {
        refuse_at = 0;
        return NULL;
    }
    allocations += 1;
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
 * A data model: conditions "t" and "f", "e" fails, and every value empty
 */

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
    if (text.len == 1 && text.ptr[0] == 'e') {
        *params->fault = (ta_fault){ .code = 7 };
        return TA_OP_ERROR;
    }
    *params->out = text.len == 1 && text.ptr[0] == 't';
    return TA_OP_OK;
}
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = source(params->shared, params->script);
    if (text.len == 1 && text.ptr[0] == 'e') {
        *params->fault = (ta_fault){ .code = 8 };
        return TA_OP_ERROR;
    }
    return TA_OP_OK;
}
static ta_op_status dm_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static ta_op_status dm_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    return TA_OP_OK;
}
static ta_op_status dm_foreach_snapshot(void *self, const ta_datamodel_foreach_snapshot_params *params) {
    (void)self;
    *params->out = (ta_value){0};
    *params->count = (uint32_t)(source(params->shared, params->array).ptr[0] - '0');
    return TA_OP_OK;
}
static ta_op_status dm_foreach_bind(void *self, const ta_datamodel_foreach_bind_params *params) {
    (void)self, (void)params;
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
    .set_named = dm_set_named,
    .execute_script = dm_execute_script,
    .foreach_snapshot = dm_foreach_snapshot,
    .foreach_bind = dm_foreach_bind,
};

/* ------------------------------------------------------------------------
 * A sink that renders every record as one line
 */

static ta_system *traced_system = NULL;
static char journal[16384];
static size_t journal_length = 0;
static long records = 0;
/* Whether the control records (PAUSED, RESUMED, STOPPED) are journaled. */
static int journal_control = 1;

static void put(const char *format, ...) {
    va_list args;
    va_start(args, format);
    int written = vsnprintf(journal + journal_length, sizeof(journal) - journal_length, format, args);
    va_end(args);
    if (written > 0 && journal_length + (size_t)written < sizeof(journal)) {
        journal_length += (size_t)written;
    }
}

static void put_event(const ta_event *event) {
    if (event == NULL) {
        put(" -");
        return;
    }
    char name[64];
    uint32_t needed = 0;
    ta_result result = ta_event_name_format(traced_system, &(ta_event_name_format_params){
                                                               .name = event->name, .buffer = name,
                                                               .capacity = sizeof(name) - 1u, .needed = &needed });
    put(" %.*s", result == TA_OK ? (int)needed : 1, result == TA_OK ? name : "?");
}

static void put_states(const char *label, const ta_state_id *states, uint32_t count) {
    put(" %s[", label);
    for (uint32_t index = 0; index < count; index += 1u) {
        put(index == 0 ? "%u" : ",%u", (unsigned)states[index]);
    }
    put("]");
}

static const char *const kind_names[] = {
    "spawned", "loaded",  "unloaded", "finished", "destroyed", "begin", "end",     "cond",
    "micro",   "raised",  "data",     "assign",   "script",    "foreach", "failed", "sent",
    "cancelled", "invoked", "uninvoked", "paused", "resumed", "stopped",
};
static const char *const at_names[] = { "", "select", "exit", "transition", "enter", "boot", "finalize", "invoke" };

static void record_write(void *self, const ta_trace_record *record) {
    (void)self;
    records += 1;
    if (!journal_control && (record->kind == TA_TRACE_PAUSED || record->kind == TA_TRACE_RESUMED ||
                             record->kind == TA_TRACE_STOPPED)) {
        return;
    }
    put("%s %llu.%u", kind_names[record->kind], (unsigned long long)record->key.macrostep,
        (unsigned)record->key.microstep);
    if (record->position.at != TA_TRACE_AT_NONE) {
        put(" @%s", at_names[record->position.at]);
        if (record->position.state != TA_STATE_NONE) {
            put(":%u", (unsigned)record->position.state);
        }
    }
    switch (record->kind) {
        case TA_TRACE_MACROSTEP_BEGIN:
            put_event(record->event);
            break;
        case TA_TRACE_MACROSTEP_END:
            put_states("in", record->states, record->state_count);
            break;
        case TA_TRACE_MICROSTEP:
            put_event(record->event);
            put_states("t", record->transitions, record->transition_count);
            put_states("x", record->exited, record->exited_count);
            put_states("e", record->entered, record->entered_count);
            if (record->history_count != 0) {
                put_states("h", record->history, record->history_count);
            }
            break;
        case TA_TRACE_CONDITION:
            if (record->position.transition != TA_TRANSITION_NONE) {
                put(" t%u", (unsigned)record->position.transition);
            }
            put(record->has_fault ? " !%d" : " =%d", record->has_fault ? record->fault.code : (int)record->result);
            break;
        case TA_TRACE_RAISED:
            put_event(record->event);
            break;
        case TA_TRACE_DATA_INIT:
            put(" %.*s", (int)record->name.len, record->name.ptr);
            break;
        case TA_TRACE_SCRIPT:
            if (record->has_fault) {
                put(" !%d", record->fault.code);
            }
            break;
        case TA_TRACE_FOREACH:
            put(" i%u", (unsigned)record->iteration);
            break;
        case TA_TRACE_ACTION_FAILED:
            put(" a%u", (unsigned)record->action);
            break;
        case TA_TRACE_SENT:
            put_event(record->event);
            break;
        case TA_TRACE_FINISHED:
            put(" s%u", (unsigned)record->state);
            break;
        default:
            break;
    }
    put("; ");
}

static const ta_trace_sink_vtable record_vtable = { .write = record_write };

static ta_trace_sink recording_sink(uint32_t kinds) {
    return (ta_trace_sink){ .self = NULL, .v = &record_vtable, .kinds = kinds };
}

#define ALL_KINDS UINT32_MAX

static void journal_reset(void) {
    journal_length = 0;
    journal[0] = '\0';
    records = 0;
}

static void expect_journal(const char *expected) {
    if (strcmp(journal, expected) != 0) {
        fprintf(stderr, "journal differs\n  expected: %s\n  actual:   %s\n", expected, journal);
        failures += 1;
    }
}

/* ------------------------------------------------------------------------
 * The chart
 *
 *   a --go [t]--> b      raise inner; foreach 2 { script }   (a guard [f] never runs)
 *   b --inner--> c       if e (fails) then script
 *   c (compound, initial c1): c1 --eventless--> c2 (final); done.state.c --> d
 *   d --stop--> end (top-level final)
 *
 * Top-level data "x" is initialized; a's <onentry> assigns.
 */

static const ta_scxml_action_spec a_entry_1[] = {
    { .kind = TA_SCXML_ACTION_ASSIGN,
      .as.assign = { .location = EXPR("x"), .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("v") } } },
    END,
};
static const ta_scxml_handler_spec a_entry[] = { { a_entry_1 }, END };
static const ta_scxml_action_spec go_body[] = { { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR("s") },
                                                END };
static const ta_scxml_action_spec go_actions[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("inner") } },
    { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = EXPR("2"), .item = STR("i"), .body = go_body } },
    END,
};
static const ta_str to_b[] = { STR("b"), {0} };
static const ta_str to_c[] = { STR("c"), {0} };
static const ta_str to_c2[] = { STR("c2"), {0} };
static const ta_str to_d[] = { STR("d"), {0} };
static const ta_str to_end[] = { STR("end"), {0} };
static const ta_scxml_transition_spec a_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .condition = EXPR("t"), .targets = to_b,
      .actions = go_actions },
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .condition = EXPR("f"), .targets = to_d },
    END,
};
static const ta_scxml_action_spec never[] = { { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR("n") },
                                              END };
static const ta_scxml_action_spec inner_actions[] = {
    { .kind = TA_SCXML_ACTION_IF, .as.conditional = { .condition = EXPR("e"), .then_actions = never } },
    END,
};
static const ta_scxml_transition_spec b_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("inner"), .targets = to_c, .actions = inner_actions },
    END,
};
static const ta_scxml_transition_spec c_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("done.state.c"), .targets = to_d },
    END,
};
static const ta_scxml_transition_spec c1_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_c2 },
    END,
};
static const ta_scxml_transition_spec d_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = to_end },
    END,
};
static const ta_scxml_state_spec c_children[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c1"), .transitions = c1_transitions },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("c2") },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .onentry = a_entry, .transitions = a_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = b_transitions },
    { .kind = TA_SCXML_STATE_COMPOUND, .id = STR("c"), .states = c_children, .transitions = c_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("d"), .transitions = d_transitions },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};
static const ta_scxml_data_spec data[] = {
    { .name = STR("x"), .initializer = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("v") } },
    END,
};
static const ta_scxml_spec chart = { .name = STR("traced"), .states = roots, .data = data };

/* ------------------------------------------------------------------------
 * Driving it
 */

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
} run;

static void start_system(run *r) {
    *r = (run){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &r->system) == TA_OK);
    traced_system = r->system;
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(r->system, &(ta_definition_compile_params){ .spec = &chart,
                                                                                 .datamodel = { .v = &dm_vtable },
                                                                                 .allocator = allocator,
                                                                                 .out = &definition }) == TA_OK);
    CHECK(ta_system_register_definition(r->system, definition, &r->definition) == TA_OK);
}

static void spawn(run *r, ta_trace_parent trace) {
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor,
                                                              .trace = trace }) == TA_OK);
}

static ta_result resume(run *r, uint32_t budget, ta_run_status *status) {
    return ta_actor_resume(r->system,
                           &(ta_actor_resume_params){ .actor = r->actor, .budget = budget, .status = status });
}

static void settle(run *r) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(resume(r, 1000, &status) == TA_OK);
    CHECK(status == TA_RUN_STABLE || status == TA_RUN_DONE);
}

static ta_event event_named(run *r, const char *text) {
    ta_atom atoms[4];
    uint32_t count = 0;
    const char *start = text;
    for (const char *at = text;; at += 1) {
        if (*at == '.' || *at == '\0') {
            CHECK(ta_system_intern_atom(r->system, (ta_str){ .ptr = start, .len = (uint32_t)(at - start) },
                                        &atoms[count]) == TA_OK);
            count += 1u;
            start = at + 1;
            if (*at == '\0') {
                break;
            }
        }
    }
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_event_name_intern(r->system, &(ta_atom_list){ .atoms = atoms, .count = count }, &event.name) ==
          TA_OK);
    return event;
}

static void deliver(run *r, const char *name, ta_trace_parent trace) {
    ta_event event = event_named(r, name);
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .actor = r->actor, .event = &event,
                                                                  .trace = trace }) == TA_OK);
}

static void finish(run *r) {
    ta_system_destroy(r->system);
    traced_system = NULL;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* A system starts with the null sink; a sink without operations is refused. */
static void test_null_sink(void) {
    ta_trace_sink null = ta_null_trace_sink();
    CHECK(null.self == NULL && null.v != NULL && null.v->write != NULL);
    CHECK(null.kinds == 0 && !null.unsampled);
    null.v->write(NULL, NULL); /* writes nothing, reads nothing */

    run r;
    start_system(&r);
    CHECK(ta_system_set_trace_sink(r.system, (ta_trace_sink){ .v = NULL, .kinds = ALL_KINDS }) == TA_ERR_INVALID);
    static const ta_trace_sink_vtable no_write = {0};
    CHECK(ta_system_set_trace_sink(r.system, (ta_trace_sink){ .v = &no_write, .kinds = ALL_KINDS }) ==
          TA_ERR_INVALID);
    CHECK(ta_system_set_trace_sink(NULL, ta_null_trace_sink()) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_sink(r.system, ta_null_trace_sink()) == TA_OK);
    finish(&r);
}

/* Runs the chart's whole life and returns the allocations it made. */
static long whole_life(ta_trace_sink sink) {
    run r;
    start_system(&r);
    CHECK(ta_system_set_trace_sink(r.system, sink) == TA_OK);
    long before = allocations;
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "stop", (ta_trace_parent){0});
    settle(&r);
    long made = allocations - before;
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    finish(&r);
    return made;
}

/* The mask decides what is built: a sink that wants nothing sees nothing,
 * and costs nothing more than no sink at all. */
static void test_mask_suppresses_construction(void) {
    journal_reset();
    long untraced = whole_life(ta_null_trace_sink());
    long masked = whole_life(recording_sink(0));
    CHECK(records == 0);
    CHECK(masked == untraced);
    long lifecycle = whole_life(recording_sink(TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_DESTROYED)));
    CHECK(lifecycle == untraced);
    expect_journal("spawned 0.0; destroyed 0.0; ");
}

/*
 * The exact records of the chart's life: the bootstrap as macrostep 1, a
 * delivered event whose guards are written after its selection completes,
 * an internal event, a failing <if> in transition content and its error,
 * which enables no transition and so is no microstep, an eventless
 * microstep, done.state, and a top-level final state, whose FINISHED is a
 * lifecycle record outside the macrostep.
 */
static void test_exact_sequence(void) {
    journal_reset();
    run r;
    start_system(&r);
    CHECK(ta_system_set_trace_sink(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    expect_journal("spawned 0.0; begin 1.0 -; data 1.0 @boot x; micro 1.1 - t[] x[] e[0]; "
                   "assign 1.1 @enter:0; end 1.1 in[0]; ");

    journal_reset();
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 2.0 go; cond 2.0 @select:0 t0 =1; micro 2.1 go t[0] x[0] e[1]; "
                   "raised 2.1 @transition:0 inner; foreach 2.1 @transition:0 i0; script 2.1 @transition:0; "
                   "foreach 2.1 @transition:0 i1; script 2.1 @transition:0; "
                   "micro 2.2 inner t[2] x[1] e[2,3]; cond 2.2 @transition:1 t2 !7; "
                   "raised 2.2 @transition:1 error.execution; micro 2.3 - t[4] x[3] e[4]; "
                   "raised 2.3 @enter:4 done.state.c; micro 2.4 done.state.c t[3] x[4,2] e[5]; end 2.4 in[5]; ");

    journal_reset();
    deliver(&r, "stop", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 3.0 stop; micro 3.1 stop t[5] x[5] e[6]; end 3.1 in[]; finished 0.0 s6; ");

    journal_reset();
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    expect_journal("destroyed 0.0; ");
    finish(&r);
}

/* A macrostep whose event enables no transition is BEGIN, then END. */
static void test_ignored_event(void) {
    run r;
    start_system(&r);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    CHECK(ta_system_set_trace_sink(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    journal_reset();
    deliver(&r, "unknown", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 2.0 unknown; end 2.0 in[0]; ");
    /* Resuming an idle actor runs nothing, so it writes nothing. */
    journal_reset();
    settle(&r);
    expect_journal("");
    finish(&r);
}

/* A macrostep paused by its budget writes PAUSED, and RESUMED when it goes on. */
static void test_pause_and_resume(void) {
    run r;
    start_system(&r);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    CHECK(ta_system_set_trace_sink(r.system, recording_sink(TA_TRACE_BIT(TA_TRACE_PAUSED) |
                                                            TA_TRACE_BIT(TA_TRACE_RESUMED) |
                                                            TA_TRACE_BIT(TA_TRACE_MICROSTEP))) == TA_OK);
    journal_reset();
    deliver(&r, "go", (ta_trace_parent){0});
    ta_run_status status = TA_RUN_ERROR;
    CHECK(resume(&r, 1, &status) == TA_OK && status == TA_RUN_PAUSED);
    CHECK(resume(&r, 1000, &status) == TA_OK && status == TA_RUN_STABLE);
    expect_journal("micro 2.1 go t[0] x[0] e[1]; paused 2.1; resumed 2.1; micro 2.2 inner t[2] x[1] e[2,3]; "
                   "micro 2.3 - t[4] x[3] e[4]; micro 2.4 done.state.c t[3] x[4,2] e[5]; ");
    finish(&r);
}

static ta_trace_parent parent_of(uint64_t high, uint64_t low, uint8_t flags, uint64_t parent_span) {
    return (ta_trace_parent){ .context = { .trace_id_high = high, .trace_id_low = low, .flags = flags },
                              .cause = { .session = 0, .macrostep = parent_span } };
}

/* Records that carry the context of the macrostep they were written in. */
static ta_trace_context seen_context;
static ta_step_key seen_cause;
static long seen_records = 0;
static void context_write(void *self, const ta_trace_record *record) {
    (void)self;
    seen_records += 1;
    if (record->kind == TA_TRACE_MACROSTEP_BEGIN) {
        seen_context = record->context;
        seen_cause = record->cause;
    }
}
static const ta_trace_sink_vtable context_vtable = { .write = context_write };

/* Sampling: an unsampled context builds only lifecycle records, unless the
 * sink asks for unsampled ones; a sampled one, or none, builds all. */
static void test_sampling(void) {
    run r;
    start_system(&r);
    ta_trace_sink sink = { .v = &context_vtable, .kinds = ALL_KINDS };
    CHECK(ta_system_set_trace_sink(r.system, sink) == TA_OK);
    spawn(&r, parent_of(1, 2, 0, 99));
    seen_records = 0;
    settle(&r);
    CHECK(seen_records == 0); /* the bootstrap ran unsampled */

    seen_records = 0;
    deliver(&r, "go", parent_of(3, 4, TA_TRACE_FLAG_SAMPLED, 77));
    settle(&r);
    CHECK(seen_records > 0);
    CHECK(seen_context.trace_id_high == 3 && seen_context.trace_id_low == 4);
    CHECK(seen_cause.session == 0 && seen_cause.macrostep == 77);

    seen_records = 0;
    sink.unsampled = true;
    CHECK(ta_system_set_trace_sink(r.system, sink) == TA_OK);
    deliver(&r, "stop", parent_of(5, 6, 0, 55));
    settle(&r);
    CHECK(seen_records > 0);
    CHECK(seen_context.trace_id_high == 5 && seen_cause.macrostep == 55);
    finish(&r);
}

/* Span IDs are a fixed mix of the key, the same in every host. */
static void test_span_ids(void) {
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 0, .macrostep = 0x1234 }) == 0x1234);
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 0, .macrostep = 0 }) == 0);
    uint64_t a = ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 1, .microstep = 0 });
    uint64_t b = ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 1, .microstep = 1 });
    uint64_t c = ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 2, .microstep = 0 });
    uint64_t d = ta_step_key_span_id((ta_step_key){ .session = 2, .macrostep = 1, .microstep = 0 });
    CHECK(a != 0 && b != 0 && c != 0 && d != 0);
    CHECK(a != b && a != c && a != d && b != c && b != d && c != d);
    /* Golden values: a host that derives span IDs itself must match these. */
    CHECK(a == 0x049e7f1d71e53363ULL);
    CHECK(b == 0x304917c348f35bf4ULL);
    CHECK(c == 0x79368ebf22c90f5bULL);
    CHECK(d == 0x6412a7d0f1c7345aULL);
}

/* ------------------------------------------------------------------------
 * Trace parents on what an actor sends, through a session image
 */

static ta_trace_parent delivered_trace;
static long deliveries = 0;
static ta_external_event_submit_status record_submit(void *self, const ta_external_event_submit_params *params) {
    (void)self;
    deliveries += 1;
    delivered_trace = params->delivery->trace;
    return TA_EXTERNAL_EVENT_ACCEPTED;
}
static const ta_external_event_router_vtable router_vtable = { .submit = record_submit };

/* a --go--> b, which sends "later" to itself after 10 ms. */
static const ta_scxml_action_spec later_actions[] = {
    { .kind = TA_SCXML_ACTION_SEND,
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("later") },
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("10ms") } } },
    END,
};
static const ta_str to_sb[] = { STR("sb"), {0} };
static const ta_scxml_transition_spec sa_transitions[] = {
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .targets = to_sb, .actions = later_actions },
    END,
};
static const ta_scxml_state_spec sender_roots[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("sa"), .transitions = sa_transitions },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("sb") },
    END,
};
static const ta_scxml_spec sender_chart = { .name = STR("sender"), .states = sender_roots };

static void test_counters_and_parents_survive_an_image(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    traced_system = system;
    CHECK(ta_system_set_external_event_router(system, (ta_external_event_router){ .v = &router_vtable }) == TA_OK);
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){ .spec = &sender_chart,
                                                                              .datamodel = { .v = &dm_vtable },
                                                                              .allocator = allocator,
                                                                              .out = &definition }) == TA_OK);
    ta_definition_id definition_id = 0;
    CHECK(ta_system_register_definition(system, definition, &definition_id) == TA_OK);
    run r = { .system = system, .definition = definition_id };
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "go", parent_of(0xaa, 0xbb, TA_TRACE_FLAG_SAMPLED, 42));
    settle(&r);

    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = r.actor, .out = &image }) == TA_OK);
    ta_actor_inspection before;
    CHECK(ta_actor_inspect(system, r.actor, &before) == TA_OK);
    CHECK(ta_actor_destroy(system, r.actor) == TA_OK);
    ta_actor_id restored = 0;
    CHECK(ta_actor_reconstruct(system, &(ta_actor_reconstruct_params){ .image = image, .out = &restored }) ==
          TA_OK);
    ta_session_image_destroy(image);

    /* The pending send kept the parent it was scheduled under. */
    CHECK(ta_system_set_time(system, 20) == TA_OK);
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 1 && deliveries == 1);
    CHECK(delivered_trace.context.trace_id_high == 0xaa && delivered_trace.context.trace_id_low == 0xbb);
    CHECK(delivered_trace.context.flags == TA_TRACE_FLAG_SAMPLED);
    CHECK(delivered_trace.cause.session == before.session_id);
    CHECK(delivered_trace.cause.macrostep == 2 && delivered_trace.cause.microstep == 1);

    /* The counters go on: the next macrostep of the session is 3. */
    CHECK(ta_system_set_trace_sink(system, recording_sink(TA_TRACE_BIT(TA_TRACE_MACROSTEP_BEGIN))) == TA_OK);
    journal_reset();
    r.actor = restored;
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 3.0 go; ");
    ta_system_destroy(system);
    traced_system = NULL;
}

/*
 * Every record is written once: a run that runs out of memory at any one
 * allocation, and is resumed, writes the records of the run that did not,
 * once PAUSED, RESUMED and STOPPED are left out.
 */
static void test_records_are_written_once(void) {
    journal_control = 0;
    /* The run that fails nothing, and how many allocations it makes. */
    run r;
    start_system(&r);
    CHECK(ta_system_set_trace_sink(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    journal_reset();
    long before = allocations;
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    long made = allocations - before;
    char baseline[sizeof(journal)];
    memcpy(baseline, journal, journal_length + 1u);
    finish(&r);
    CHECK(made > 0);

    for (long failing = 1; failing <= made; failing += 1) {
        start_system(&r);
        CHECK(ta_system_set_trace_sink(r.system, recording_sink(ALL_KINDS)) == TA_OK);
        spawn(&r, (ta_trace_parent){0});
        settle(&r);
        journal_reset();
        ta_event event = event_named(&r, "go");
        refuse_at = allocations + failing;
        ta_result delivered = ta_actor_deliver(r.system,
                                               &(ta_actor_deliver_params){ .actor = r.actor, .event = &event });
        ta_run_status status = TA_RUN_ERROR;
        uint32_t attempts = 0;
        if (delivered == TA_ERR_NOMEM) {
            delivered = ta_actor_deliver(r.system, &(ta_actor_deliver_params){ .actor = r.actor, .event = &event });
        }
        CHECK(delivered == TA_OK);
        while (resume(&r, 1000, &status) == TA_ERR_NOMEM && attempts < 4u) {
            attempts += 1u;
        }
        refuse_at = 0;
        CHECK(status == TA_RUN_STABLE);
        if (strcmp(journal, baseline) != 0) {
            fprintf(stderr, "allocation %ld failing changed the records\n  expected: %s\n  actual:   %s\n",
                    failing, baseline, journal);
            failures += 1;
        }
        finish(&r);
    }
    journal_control = 1;
}

int main(void) {
    test_null_sink();
    test_mask_suppresses_construction();
    test_exact_sequence();
    test_ignored_event();
    test_pause_and_resume();
    test_sampling();
    test_span_ids();
    test_counters_and_parents_survive_an_image();
    test_records_are_written_once();
    CHECK(live_allocations == 0);
    if (failures != 0) {
        fprintf(stderr, "trace_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("trace_test passed");
    return 0;
}
