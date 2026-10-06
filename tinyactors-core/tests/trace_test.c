/*
 * Tracing (ADR 0067): the empty masks a system starts with, the mask that
 * keeps core from building records nobody wants, sampling, exact record
 * sequences for small charts, step keys and their span IDs, and counters
 * and trace parents that survive a session image. Records are read from the
 * system's tel ring after each call (ADR 0098), as a host reads them.
 *
 * Uses the C library; the core does not.
 */
#include <stdarg.h>
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
#define END {0}

/* ------------------------------------------------------------------------
 * A test allocator, which counts calls and can refuse one of them
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

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

static void put_event(const ta_trace_record *record) {
    if (!record->has_event) {
        put(" -");
        return;
    }
    ta_str name = ta_trace_record_text(record, TA_TRACE_TEXT_EVENT_NAME);
    put(" %.*s", (int)name.len, name.ptr);
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
    "cancelled", "invoked", "uninvoked", "paused", "resumed", "stopped", "ignored", "forwarded",
    "long", "runaway", "definition-registered", "definition-retired", "definition-freed", "abandoned",
    "withdrawn",
};
_Static_assert(sizeof kind_names / sizeof kind_names[0] == (size_t)TA_TRACE_WITHDRAWN + 1u,
               "a name for every trace kind");
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
            put_event(record);
            break;
        case TA_TRACE_MACROSTEP_END:
            put_states("in", record->states, record->state_count);
            break;
        case TA_TRACE_MICROSTEP:
            put_event(record);
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
            put(record->has_fault ? " !%d" : " =%d", record->has_fault ? record->fault_code : (int)record->result);
            break;
        case TA_TRACE_RAISED:
            put_event(record);
            break;
        case TA_TRACE_DATA_INIT:
            put(" %.*s", (int)ta_trace_record_text(record, TA_TRACE_TEXT_NAME).len,
                ta_trace_record_text(record, TA_TRACE_TEXT_NAME).ptr);
            break;
        case TA_TRACE_SCRIPT:
            if (record->has_fault) {
                put(" !%d", record->fault_code);
            }
            break;
        case TA_TRACE_FOREACH:
            put(" i%u", (unsigned)record->iteration);
            break;
        case TA_TRACE_ACTION_FAILED:
            put(" a%u", (unsigned)record->action);
            break;
        case TA_TRACE_SENT:
            put_event(record);
            break;
        case TA_TRACE_FINISHED:
            put(" s%u", (unsigned)record->state);
            break;
        default:
            break;
    }
    put("; ");
}

/* What a test reads tel into, and the masks it sets. */
typedef struct {
    tel_write_fn write;
    ta_trace_masks masks;
} sink;

static sink recording_sink(uint32_t kinds) {
    return (sink){ .write = record_write, .masks = { .kinds = kinds } };
}

/* The sink whose records the run reads, set by use(). */
static sink current = { 0 };

/* Sets the sink's masks and reads its records from here on. */
static ta_result use(ta_system *system, sink chosen) {
    current = chosen;
    return ta_system_set_trace_masks(system, &chosen.masks);
}

/* Reads what the system wrote to tel since the last read. */
static void read_tel(ta_system *system) {
    if (current.write != NULL) {
        tel_drain(system, current.write, NULL);
    } else {
        ta_tel_record record;
        while (ta_system_tel_peek(system, &record) == TA_OK) {
            (void)ta_system_tel_release(system);
        }
    }
}

#define ALL_KINDS UINT32_MAX

static void journal_reset(void) {
    journal_length = 0;
    journal[0] = '\0';
    records = 0;
}

static void expect_journal(const char *expected) {
    if (traced_system != NULL) {
        read_tel(traced_system);
    }
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
    read_tel(r->system);
}

/* One run that answers core's own result: the out-of-memory sweep asserts
 * TA_ERR_NOMEM from it, the code of the fault that stopped the input. A
 * stopped input goes on at the next run. */
static ta_result resume(run *r, uint32_t budget, test_drive_status *status) {
    ta_result result =
        test_drive_run(r->system, r->actor, &(test_drive_run_params){ .budget = budget, .status = status });
    read_tel(r->system);
    return result;
}

/* One run with this budget, and reads what it wrote. */
static ta_result run_for(run *r, uint32_t budget, test_drive_status *status) {
    ta_result result =
        test_drive_run(r->system, r->actor, &(test_drive_run_params){ .budget = budget, .status = status });
    read_tel(r->system);
    return result;
}

static void settle(run *r) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(run_for(r, 1000, &status) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE || status == TEST_DRIVE_DONE);
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
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .time = ta_system_time(r->system), .actor = r->actor, .name = test_event_text(r->system, &event), .event = &event,
                                                                  .trace = trace }) == TA_OK);
    read_tel(r->system);
}

static void finish(run *r) {
    read_tel(r->system);
    current = (sink){ 0 };
    ta_system_destroy(r->system);
    traced_system = NULL;
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* A system starts with every mask zero, so it writes nothing; masks without a
 * system or a system without masks are refused. */
static void test_null_sink(void) {
    run r;
    start_system(&r);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    ta_tel_status status;
    CHECK(ta_system_tel_status(r.system, &status) == TA_OK);
    CHECK(status.capacity != 0 && status.used == 0 && status.next_seq == 1); /* nothing was written */
    ta_trace_masks none = { 0 };
    CHECK(ta_system_set_trace_masks(NULL, &none) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_masks(r.system, NULL) == TA_ERR_INVALID);
    CHECK(ta_system_set_trace_masks(r.system, &none) == TA_OK);
    finish(&r);
}

/* The sink that wants nothing and writes nothing. */
static sink null_sink(void) {
    return (sink){ 0 };
}

/* Runs the chart's whole life and returns the allocations it made. */
static long whole_life(sink chosen) {
    run r;
    start_system(&r);
    CHECK(use(r.system, chosen) == TA_OK);
    long before = heap.allocations;
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "stop", (ta_trace_parent){0});
    settle(&r);
    long made = heap.allocations - before;
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    finish(&r);
    return made;
}

/* The mask decides what is built: a sink that wants nothing sees nothing,
 * and costs nothing more than no sink at all. */
static void test_mask_suppresses_construction(void) {
    journal_reset();
    long untraced = whole_life(null_sink());
    long masked = whole_life(recording_sink(0));
    CHECK(records == 0);
    CHECK(masked == untraced);
    long lifecycle = whole_life(recording_sink(TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_DESTROYED)));
    CHECK(lifecycle == untraced);
    expect_journal("spawned 0.1; destroyed 0.3; ");
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
    CHECK(use(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    expect_journal("spawned 0.1; begin 1.0 -; data 1.0 @boot x; micro 1.1 - t[] x[] e[0]; "
                   "assign 1.1 @enter:0; end 1.1 in[0]; ");

    journal_reset();
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 2.0 go; cond 2.0 @select:0 t0 =1; micro 2.1 go t[0] x[0] e[1]; "
                   "raised 2.1 @transition:0 inner; foreach 2.1 @transition:0 i0; script 2.1 @transition:0; "
                   "foreach 2.1 @transition:0 i1; script 2.1 @transition:0; "
                   "micro 2.2 inner t[2] x[1] e[2,3]; cond 2.2 @transition:1 t2 !7; "
                   "raised 2.2 @transition:1 error.execution; micro 2.3 - t[4] x[3] e[4]; "
                   "raised 2.3 @enter:4 done.state.c; ignored 2.3; micro 2.4 done.state.c t[3] x[4,2] e[5]; end 2.4 in[5]; ");

    journal_reset();
    deliver(&r, "stop", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 3.0 stop; micro 3.1 stop t[5] x[5] e[6]; end 3.1 in[]; finished 0.2 s6; ");

    journal_reset();
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    expect_journal("destroyed 0.3; ");
    finish(&r);
}

/* A macrostep whose event enables no transition is BEGIN, then END. */
static void test_ignored_event(void) {
    run r;
    start_system(&r);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    CHECK(use(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    journal_reset();
    deliver(&r, "unknown", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 2.0 unknown; ignored 2.0; end 2.0 in[0]; ");
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
    CHECK(use(r.system, recording_sink(TA_TRACE_BIT(TA_TRACE_PAUSED) | TA_TRACE_BIT(TA_TRACE_RESUMED) |
                                       TA_TRACE_BIT(TA_TRACE_MICROSTEP))) == TA_OK);
    journal_reset();
    deliver(&r, "go", (ta_trace_parent){0});
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(run_for(&r, 1, &status) == TA_OK && status == TEST_DRIVE_PAUSED);
    CHECK(run_for(&r, 1000, &status) == TA_OK && status == TEST_DRIVE_STABLE);
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

/* Sampling: an unsampled context builds only lifecycle records, unless the
 * sink asks for unsampled ones; a sampled one, or none, builds all. */
static void test_sampling(void) {
    run r;
    start_system(&r);
    sink chosen = { .write = context_write, .masks = { .kinds = ALL_KINDS } };
    CHECK(use(r.system, chosen) == TA_OK);
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
    chosen.masks.unsampled_kinds = chosen.masks.kinds;
    CHECK(use(r.system, chosen) == TA_OK);
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

/* What the last MACROSTEP_BEGIN ran under: its trace context and cause. */
static ta_trace_context begin_context;
static ta_step_key begin_cause;
static void begin_write(void *self, const ta_trace_record *record) {
    if (record->kind == TA_TRACE_MACROSTEP_BEGIN) {
        begin_context = record->context;
        begin_cause = record->cause;
    }
    record_write(self, record);
}

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

    /* The pending send kept the parent it was scheduled under: it reaches the
     * session in core, not the router, and the macrostep it starts runs in
     * the request's trace, caused by the step that scheduled it. The
     * counters go on: that macrostep is the session's third. */
    CHECK(use(system, (sink){ .write = begin_write,
                              .masks = { .kinds = TA_TRACE_BIT(TA_TRACE_MACROSTEP_BEGIN) } }) == TA_OK);
    journal_reset();
    CHECK(ta_system_set_time(system, 20) == TA_OK);
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    ta_load_request request;
    CHECK(dispatched == 1 && ta_system_load_needed(system, &request) == TA_ERR_NOT_FOUND);
    r.actor = restored;
    settle(&r);
    CHECK(begin_context.trace_id_high == 0xaa && begin_context.trace_id_low == 0xbb);
    CHECK(begin_context.flags == TA_TRACE_FLAG_SAMPLED);
    CHECK(begin_cause.session == before.session_id);
    CHECK(begin_cause.macrostep == 2 && begin_cause.microstep == 1);
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    expect_journal("begin 3.0 later; begin 4.0 go; ");
    current = (sink){ 0 };
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
    CHECK(use(r.system, recording_sink(ALL_KINDS)) == TA_OK);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    journal_reset();
    long before = heap.allocations;
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    long made = heap.allocations - before;
    char baseline[sizeof(journal)];
    memcpy(baseline, journal, journal_length + 1u);
    finish(&r);
    CHECK(made > 0);

    for (long failing = 1; failing <= made; failing += 1) {
        start_system(&r);
        CHECK(use(r.system, recording_sink(ALL_KINDS)) == TA_OK);
        spawn(&r, (ta_trace_parent){0});
        settle(&r);
        journal_reset();
        ta_event event = event_named(&r, "go");
        heap.policy.fail_at = heap.attempts + failing;
        ta_result delivered = ta_actor_deliver(r.system,
                                               &(ta_actor_deliver_params){ .time = ta_system_time(r.system), .actor = r.actor, .name = test_event_text(r.system, &event), .event = &event });
        read_tel(r.system);
        test_drive_status status = TEST_DRIVE_ERROR;
        uint32_t attempts = 0;
        if (delivered == TA_ERR_NOMEM) {
            delivered = ta_actor_deliver(r.system, &(ta_actor_deliver_params){ .time = ta_system_time(r.system), .actor = r.actor, .name = test_event_text(r.system, &event), .event = &event });
            read_tel(r.system);
        }
        CHECK(delivered == TA_OK);
        while (resume(&r, 1000, &status) == TA_ERR_NOMEM && attempts < 4u) {
            attempts += 1u;
        }
        heap.policy.fail_at = 0;
        CHECK(status == TEST_DRIVE_STABLE);
        read_tel(r.system);
        if (strcmp(journal, baseline) != 0) {
            fprintf(stderr, "allocation %ld failing changed the records\n  expected: %s\n  actual:   %s\n",
                    failing, baseline, journal);
            failures += 1;
        }
        finish(&r);
    }
    journal_control = 1;
}

/* ------------------------------------------------------------------------
 * Lifecycle records have keys of their own (napi ADR 0016): OTLP needs a
 * distinct span ID for each, including two LOADED records of one session
 * on either side of an image.
 */

static uint64_t lifecycle_spans[16];
static uint32_t lifecycle_count = 0;
static void lifecycle_write(void *self, const ta_trace_record *record) {
    record_write(self, record);
    if (lifecycle_count < 16u) {
        lifecycle_spans[lifecycle_count++] = ta_step_key_span_id(record->key);
    }
}

static void test_lifecycle_keys(void) {
    run r;
    start_system(&r);
    uint32_t kinds = TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_LOADED) |
                     TA_TRACE_BIT(TA_TRACE_UNLOADED) | TA_TRACE_BIT(TA_TRACE_DESTROYED);
    CHECK(use(r.system, (sink){ .write = lifecycle_write, .masks = { .kinds = kinds } }) == TA_OK);
    journal_reset();
    lifecycle_count = 0;
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    for (int round = 0; round < 2; round += 1) {
        ta_session_image *image = NULL;
        CHECK(ta_actor_capture(r.system, &(ta_actor_capture_params){ .actor = r.actor, .out = &image }) == TA_OK);
        CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
        CHECK(ta_actor_reconstruct(r.system, &(ta_actor_reconstruct_params){ .image = image, .out = &r.actor }) ==
              TA_OK);
        ta_session_image_destroy(image);
    }
    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    /* Each image holds the count at capture; restoring skips the ordinal an
     * UNLOADED would take next, which the DESTROYED of the source used. */
    expect_journal("spawned 0.1; destroyed 0.2; loaded 0.3; destroyed 0.4; loaded 0.5; destroyed 0.6; ");
    CHECK(lifecycle_count == 6);
    for (uint32_t a = 0; a < lifecycle_count; a += 1u) {
        CHECK(lifecycle_spans[a] != 0);
        for (uint32_t b = a + 1u; b < lifecycle_count; b += 1u) {
            CHECK(lifecycle_spans[a] != lifecycle_spans[b]);
        }
    }
    /* Golden values: lifecycle keys go through the same derivation. */
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 0, .microstep = 1 }) == 0x65c672b9ddbfb556ULL);
    CHECK(ta_step_key_span_id((ta_step_key){ .session = 1, .macrostep = 0, .microstep = 3 }) == 0xb4ed65a25541f516ULL);
    finish(&r);
}

/* ------------------------------------------------------------------------
 * The tracing module (ADR 0073)
 */

static ta_result parse(const char *text, ta_trace_parent *out) {
    return ta_traceparent_parse(&(ta_traceparent_parse_params){ .text = { text, (uint32_t)strlen(text) }, .out = out });
}

/* W3C's example round trips, and malformed text is refused. */
static void test_traceparent(void) {
    const char *example = "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01";
    ta_trace_parent parent;
    CHECK(parse(example, &parent) == TA_OK);
    CHECK(parent.context.trace_id_high == 0x4bf92f3577b34da6ull);
    CHECK(parent.context.trace_id_low == 0xa3ce929d0e0e4736ull);
    CHECK(parent.context.flags == TA_TRACE_FLAG_SAMPLED && parent.context.detail == 0);
    CHECK(parent.cause.session == 0 && parent.cause.macrostep == 0x00f067aa0ba902b7ull &&
          parent.cause.microstep == 0);
    char text[56];
    uint32_t needed = 0;
    CHECK(ta_traceparent_format(&(ta_traceparent_format_params){ .parent = parent, .buffer = text,
                                                                 .capacity = sizeof text, .needed = &needed }) ==
          TA_OK);
    CHECK(needed == 56 && strcmp(text, example) == 0);
    CHECK(ta_traceparent_format(&(ta_traceparent_format_params){ .parent = parent, .buffer = text, .capacity = 10,
                                                                 .needed = &needed }) == TA_ERR_NOMEM);
    CHECK(needed == 56);
    CHECK(ta_traceparent_format(&(ta_traceparent_format_params){ .parent = {0}, .buffer = text,
                                                                 .capacity = sizeof text, .needed = &needed }) ==
          TA_ERR_INVALID);

    /* Optional whitespace, and a future version with more after a dash. */
    CHECK(parse(" \t00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-00 ", &parent) == TA_OK &&
          parent.context.flags == 0);
    CHECK(parse("cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-09-extra", &parent) == TA_OK &&
          parent.context.flags == 9);

    static const char *const invalid[] = {
        "",
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-0",       /* short */
        "00-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01-x",    /* version 00 is 55 */
        "cc-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01x",     /* no dash after 55 */
        "ff-4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",      /* version ff */
        "00-4BF92F3577B34DA6A3CE929D0E0E4736-00f067aa0ba902b7-01",      /* uppercase */
        "00-00000000000000000000000000000000-00f067aa0ba902b7-01",      /* zero trace ID */
        "00-4bf92f3577b34da6a3ce929d0e0e4736-0000000000000000-01",      /* zero parent ID */
        "00_4bf92f3577b34da6a3ce929d0e0e4736-00f067aa0ba902b7-01",      /* separator */
        "00-4bf92f3577b34da6a3ce929d0e0e473g-00f067aa0ba902b7-01",      /* not hex */
    };
    for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index += 1) {
        CHECK(parse(invalid[index], &parent) == TA_ERR_INVALID);
    }

    /* The IDs are read eight digits at a time: every byte value, at every
     * position of both IDs, is taken exactly when it is a lowercase hex
     * digit, and lands in the right nibble. */
    for (uint32_t at = 3; at < 52; at += 1u) {
        if (at == 35) {
            continue; /* the dash between the IDs */
        }
        for (int byte = 1; byte < 256; byte += 1) {
            char changed[56];
            memcpy(changed, example, sizeof changed);
            changed[at] = (char)byte;
            int value = byte >= '0' && byte <= '9' ? byte - '0' : byte >= 'a' && byte <= 'f' ? byte - 'a' + 10 : -1;
            ta_result result = ta_traceparent_parse(
                &(ta_traceparent_parse_params){ .text = { changed, 55 }, .out = &parent });
            CHECK(value < 0 ? result == TA_ERR_INVALID : result == TA_OK);
            if (value >= 0) {
                uint32_t field = at < 19 ? 0 : at < 35 ? 1 : 2;
                uint32_t start = field == 0 ? 3 : field == 1 ? 19 : 36;
                uint64_t read = field == 0 ? parent.context.trace_id_high
                                : field == 1 ? parent.context.trace_id_low : parent.cause.macrostep;
                CHECK(((read >> (4u * (15u - (at - start)))) & 0xfu) == (uint64_t)value);
            }
        }
    }

    char id[33];
    CHECK(ta_trace_id_format(&(ta_trace_id_format_params){ .context = { 1, 0xabcu, 0, 0 }, .buffer = id,
                                                           .capacity = sizeof id, .needed = &needed }) == TA_OK);
    CHECK(needed == 33 && strcmp(id, "0000000000000001" "0000000000000abc") == 0);
    CHECK(ta_span_id_format(&(ta_span_id_format_params){ .span_id = 0x00f067aa0ba902b7ull, .buffer = id,
                                                         .capacity = sizeof id, .needed = &needed }) == TA_OK);
    CHECK(needed == 17 && strcmp(id, "00f067aa0ba902b7") == 0);
}

/* A seed gives the same sequence on every host: xoshiro256** over SplitMix64. */
static void test_generator(void) {
    run r;
    start_system(&r);
    ta_trace_context first = ta_system_new_trace(r.system); /* unseeded: seed 0 */
    CHECK(first.trace_id_high == 0x99ec5f36cb75f2b4ull && first.trace_id_low == 0xbf6e1f784956452aull);
    CHECK(first.flags == (TA_TRACE_FLAG_RANDOM | TA_TRACE_FLAG_SAMPLED) && first.detail == 0);
    CHECK(ta_system_new_span_id(r.system) == 0x1a5f849d4933e6e0ull);
    ta_system_seed_trace_ids(r.system, 42);
    ta_trace_context again = ta_system_new_trace(r.system);
    CHECK(again.trace_id_high == 0x15780b2e0c2ec716ull && again.trace_id_low == 0x6104d9866d113a7eull);
    CHECK(ta_system_new_span_id(r.system) == 0xae17533239e499a1ull);
    ta_system_seed_trace_ids(r.system, 42);
    CHECK(ta_system_new_trace(r.system).trace_id_low == 0x6104d9866d113a7eull);
    finish(&r);
}

/* Consistent probability sampling on the trace ID's rightmost 56 bits. */
static void test_sampler(void) {
    const uint64_t all = (uint64_t)1u << 56;
    CHECK(ta_trace_ratio_threshold(1, 1) == 0 && ta_trace_ratio_threshold(3, 2) == 0);
    CHECK(ta_trace_ratio_threshold(0, 1) == all && ta_trace_ratio_threshold(1, 0) == all);
    CHECK(ta_trace_ratio_threshold(1, 2) == all / 2u);
    CHECK(ta_trace_ratio_threshold(250000000u, 1000000000u) == all / 4u * 3u);
    CHECK(ta_trace_ratio_threshold(1, 3) == 48038396025285291ull); /* round(2/3 * 2^56) */
    uint64_t half = ta_trace_ratio_threshold(1, 2);
    /* The high byte of the low word is not randomness, and is ignored. */
    ta_trace_context below = { .trace_id_low = 0xff00000000000000ull | (half - 1u) };
    ta_trace_context at = { .trace_id_low = half };
    CHECK(!ta_trace_sampled_at(&below, half) && ta_trace_sampled_at(&at, half));
    CHECK(ta_trace_sampled_at(&below, 0) && !ta_trace_sampled_at(&at, all));

    run r;
    start_system(&r);
    ta_system_set_trace_ratio(r.system, 1, 4);
    int sampled = 0;
    for (int index = 0; index < 20000; index += 1) {
        sampled += (ta_system_new_trace(r.system).flags & TA_TRACE_FLAG_SAMPLED) != 0 ? 1 : 0;
    }
    CHECK(sampled > 4600 && sampled < 5400);
    ta_system_set_trace_ratio(r.system, 0, 1);
    CHECK((ta_system_new_trace(r.system).flags & TA_TRACE_FLAG_SAMPLED) == 0);
    finish(&r);
}

/* A watched context builds the sink's watched kinds, and its macrostep keeps the mark. */
static long watched_run(uint8_t detail) {
    run r;
    start_system(&r);
    sink chosen = { .write = context_write,
                    .masks = { .kinds = TA_TRACE_BIT(TA_TRACE_MACROSTEP_BEGIN), .watched_kinds = ALL_KINDS } };
    CHECK(use(r.system, chosen) == TA_OK);
    spawn(&r, parent_of(1, 2, TA_TRACE_FLAG_SAMPLED, 9));
    settle(&r);
    ta_trace_parent parent = parent_of(3, 4, TA_TRACE_FLAG_SAMPLED, 7);
    parent.context.detail = detail;
    seen_records = 0;
    deliver(&r, "go", parent);
    settle(&r);
    CHECK(seen_context.detail == detail);
    long seen = seen_records;
    finish(&r);
    return seen;
}

static void test_watched_detail(void) {
    CHECK(watched_run(0) == 1);
    CHECK(watched_run(TA_TRACE_DETAIL_WATCHED) > 1);
}

/* ------------------------------------------------------------------------
 * Counters and occupancy (ADR 0072)
 */

static uint32_t occupied(run *r) {
    uint32_t states[64];
    uint32_t count = 0;
    CHECK(ta_definition_occupancy(r->system, &(ta_definition_occupancy_params){ .definition = r->definition,
                                                                                .states = states,
                                                                                .capacity = 64,
                                                                                .count = &count }) == TA_OK);
    uint32_t total = 0;
    for (uint32_t state = 0; state < count; state += 1u) {
        total += states[state];
    }
    return total;
}

/* Counters move whether or not a sink is installed; occupancy follows residency. */
static void test_counters_and_occupancy(void) {
    run r;
    start_system(&r);
    spawn(&r, (ta_trace_parent){0});
    settle(&r);
    CHECK(occupied(&r) == 1);
    deliver(&r, "unknown", (ta_trace_parent){0});
    settle(&r);
    deliver(&r, "go", (ta_trace_parent){0});
    settle(&r);
    ta_counters counters;
    CHECK(ta_definition_counters(r.system, r.definition, &counters) == TA_OK);
    CHECK(counters.spawned == 1 && counters.destroyed == 0);
    CHECK(counters.macrosteps == 3); /* bootstrap, unknown, go */
    CHECK(counters.events_taken == 2 && counters.events_ignored == 1);
    CHECK(counters.internal_ignored == 1); /* the error.execution nothing handles */
    CHECK(counters.errors_execution == 1);
    CHECK(counters.microsteps >= 5);
    CHECK(occupied(&r) >= 1);

    ta_counters total;
    ta_system_counters(r.system, &total);
    CHECK(total.macrosteps == counters.macrosteps && total.errors_execution == 1);
    CHECK(ta_definition_counters(r.system, r.definition + 1u, &counters) == TA_ERR_NOT_FOUND);
    uint32_t count = 0;
    CHECK(ta_definition_occupancy(r.system, &(ta_definition_occupancy_params){ .definition = r.definition,
                                                                               .count = &count }) ==
          TA_ERR_NOMEM);
    CHECK(count != 0);

    CHECK(ta_actor_destroy(r.system, r.actor) == TA_OK);
    CHECK(occupied(&r) == 0);
    CHECK(ta_definition_counters(r.system, r.definition, &counters) == TA_OK && counters.destroyed == 1);
    finish(&r);
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
    test_lifecycle_keys();
    test_records_are_written_once();
    test_traceparent();
    test_generator();
    test_sampler();
    test_watched_detail();
    test_counters_and_occupancy();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "trace_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("trace_test passed");
    return 0;
}
