/* test: standalone asan */
/*
 * Phase 3 (docs/proposals/ring-proto/reference/names.md): the interpreter
 * matches a taken event by lookup.
 *
 * Checks, through a compiled chart and a running actor rather than the
 * resolver alone:
 *
 *   - the worked examples of names.md: which of the descriptors `order`,
 *     `order.*.paid`, `error.communication.invoke.payment` and `*` fire for
 *     each event of the table, including a generated invoke ID with a dot
 *     (s3.7) and the name an invoke failure produces;
 *   - the same table resolved as bytes, for segments that are not atoms
 *     (UNKNOWN), against the live system's atom table;
 *   - taking an event, and resolving random names, leave the atom table and
 *     the trie as they were (counts before and after);
 *   - a take allocates nothing: the test allocator refuses every request
 *     from the first on, and the macrostep still completes;
 *   - an invoke ID with dots is one segment of the failure name, as before
 *     nothing, and the next event is still taken;
 *   - ta_interpreter_scratch_need equals the bytes ta_interpreter_init takes
 *     (eventNameDepth 8 and 64), and a chart that would run short of
 *     interpreterBytes only because of a deep eventNameDepth is refused.
 *
 * Includes the amalgamation, because ta_interpreter_init and the resolver
 * are private to core.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <string.h>

#include "test_allocator.h"

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define EXPR(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })

static ta_test_allocator heap = { .strict = true };
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * A data model that logs every script
 */

static char script_log[64];
static size_t script_length = 0;

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
    *params->out = true;
    return TA_OP_OK;
}
static ta_op_status dm_execute_script(void *self, const ta_datamodel_execute_script_params *params) {
    (void)self;
    ta_str text = ((const ta_expression *)params->shared)[params->script].as.source;
    if (script_length + text.len < sizeof script_log) {
        memcpy(script_log + script_length, text.ptr, text.len);
        script_length += text.len;
        script_log[script_length] = 0;
    }
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
 * The chart: a parallel state of four regions, each with one targetless
 * transition on one descriptor of names.md, logging its letter. Every region
 * selects independently, so the log of one event is the set that matched.
 *
 *   A: order    B: order.*.paid    C: error.communication.invoke.payment    D: *
 */

#define SCRIPT(text) { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = EXPR(text) }
#define END {0}

static const ta_scxml_action_spec log_a[] = { SCRIPT("A"), END };
static const ta_scxml_action_spec log_b[] = { SCRIPT("B"), END };
static const ta_scxml_action_spec log_c[] = { SCRIPT("C"), END };
static const ta_scxml_action_spec log_d[] = { SCRIPT("D"), END };
static const ta_scxml_transition_spec on_a[] = {
    { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("order"), .actions = log_a }, END };
static const ta_scxml_transition_spec on_b[] = {
    { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("order.*.paid"), .actions = log_b }, END };
static const ta_scxml_transition_spec on_c[] = {
    { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("error.communication.invoke.payment"), .actions = log_c },
    END };
static const ta_scxml_transition_spec on_d[] = {
    { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("*"), .actions = log_d }, END };
static const ta_scxml_state_spec regions[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = on_a },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = on_b },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c"), .transitions = on_c },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("d"), .transitions = on_d },
    END,
};
static const ta_scxml_state_spec roots[] = {
    { .kind = TA_SCXML_STATE_PARALLEL, .id = STR("p"), .states = regions }, END };
static const ta_scxml_spec chart = { .name = STR("match"), .states = roots };

/* ------------------------------------------------------------------------
 * Driving it
 */

typedef struct {
    ta_system *system;
    ta_definition_id definition;
    ta_actor_id actor;
} run;

static ta_param_value next_params[4];
static uint32_t next_param_count;
static void next_param(ta_param param, uint64_t value) {
    next_params[next_param_count++] = (ta_param_value){ .param = param, .value = value };
}

static ta_result open_with(run *r, const ta_scxml_spec *spec) {
    *r = (run){0};
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = next_params,
                                                .param_count = next_param_count },
                           &r->system) == TA_OK);
    next_param_count = 0;
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(r->system, &(ta_definition_compile_params){
                                                                 .spec = spec, .datamodel = { .v = &dm_vtable },
                                                                 .allocator = allocator, .out = &definition });
    if (result == TA_OK) {
        CHECK(ta_system_register_definition(r->system, definition, &r->definition) == TA_OK);
    }
    return result;
}

static void close_run(run *r) {
    ta_system_destroy(r->system);
}

static void start(run *r) {
    CHECK(open_with(r, &chart) == TA_OK);
    CHECK(ta_actor_spawn(r->system, &(ta_actor_spawn_params){ .definition = r->definition, .out = &r->actor }) ==
          TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(r->system, &(ta_actor_resume_params){ .actor = r->actor, .budget = 1000,
                                                                .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
}

static ta_event_name_id intern(run *r, const char *dotted) {
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern_dotted(&r->system->event_names,
                                      (ta_str){ .ptr = dotted, .len = (uint32_t)strlen(dotted) }, &name) == TA_OK);
    return name;
}

/* Delivers `name` (already interned), runs the macrostep, and answers the letters that fired. */
static const char *fire(run *r, ta_event_name_id name) {
    script_length = 0;
    script_log[0] = 0;
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(r->system, &(ta_actor_deliver_params){ .actor = r->actor, .event = &event,
                                                                  .time = ta_system_time(r->system) }) == TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(r->system, &(ta_actor_resume_params){ .actor = r->actor, .budget = 1000,
                                                                .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
    return script_log;
}

/* ------------------------------------------------------------------------
 * The tests
 */

static void test_worked_examples(void) {
    run r;
    start(&r);
    /* The chart registers the atoms of its descriptors; the rows' other segments are interned as the events are. */
    CHECK(strcmp(fire(&r, intern(&r, "order.123.paid")), "ABD") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "order.124")), "AD") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "orders.1")), "D") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "error.communication.invoke.payment.timeout")), "CD") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "error.communication.invoke.s3.7.unavailable")), "D") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "x9f2.qq.zz")), "D") == 0);
    /* The prefix rule: the descriptor's own name, and a name that is only a prefix of it. */
    CHECK(strcmp(fire(&r, intern(&r, "order")), "AD") == 0);
    CHECK(strcmp(fire(&r, intern(&r, "error.communication.invoke")), "D") == 0);
    /* Generated names: an invoke failure builds error.communication.invoke.<id>.<reason> from the table's atom. */
    ta_invoke_id payment = TA_INVOKE_ID_NONE;
    CHECK(ta_system_intern_invoke_id(r.system, STR("payment"), &payment) == TA_OK);
    ta_event_name_id failure = 0;
    CHECK(ta_invoke_error_name(r.system, payment, STR("timeout"), &failure) == TA_OK);
    CHECK(strcmp(fire(&r, failure), "CD") == 0);
    CHECK(strcmp(fire(&r, TA_EVENT_NAME_DONE_INVOKE), "D") == 0);
    CHECK(r.system->event_names.invoke_segment != TA_ATOM_NONE);
    close_run(&r);
}

/* The same rows as bytes whose segments are not atoms, against the live atom table: UNKNOWN, never interned. */
static void test_unknown_segments(void) {
    run r;
    start(&r);
    static const struct {
        const char *name;
        uint32_t count;
        uint32_t unknown_mask; /* bit i set: segment i is UNKNOWN */
    } rows[] = {
        { "order.123.paid", 3, 1u << 1 },
        { "order.124", 2, 1u << 1 },
        { "orders.1", 2, 3u },
        { "error.communication.invoke.payment.timeout", 5, 1u << 4 },
        { "error.communication.invoke.s3.7.unavailable", 6, 7u << 3 },
        { "x9f2.qq.zz", 3, 7u },
    };
    for (uint32_t row = 0; row < TA_ARRAY_COUNT(rows); row += 1) {
        ta_atom out[8];
        uint32_t count = 0;
        CHECK(ta_name_resolve(&(ta_name_resolve_params){ .atoms = &r.system->atoms,
                                                         .name = { rows[row].name, (uint32_t)strlen(rows[row].name) },
                                                         .out = out, .capacity = 8, .count = &count }) == TA_OK);
        CHECK(count == rows[row].count);
        for (uint32_t index = 0; index < count && index < 8; index += 1) {
            CHECK((out[index] == TA_NAME_UNKNOWN) == (((rows[row].unknown_mask >> index) & 1u) != 0));
        }
        /* The matcher over these atoms answers the table's columns. */
        const ta_definition *definition = ta_definition_lookup(r.system, r.definition);
        CHECK(definition != NULL && definition->event_pattern_count == 4);
        bool fired[4];
        for (uint32_t pattern = 0; pattern < 4; pattern += 1) {
            fired[pattern] = ta_event_pattern_matches(definition, &definition->event_patterns[pattern],
                                                      (ta_event_atoms){ .atoms = out, .count = count });
        }
        CHECK(fired[3]);
        CHECK(fired[0] == (row <= 1));
        CHECK(fired[1] == (row == 0));
        CHECK(fired[2] == (row == 3));
    }
    close_run(&r);
}

typedef struct {
    size_t atoms;
    uint32_t index;
    size_t text;
    size_t nodes;
    uint32_t trie_index;
} tables;

static tables snapshot(const ta_system *system) {
    return (tables){ .atoms = system->atoms.entries.count, .index = ta_table_count(&system->atoms.index),
                     .text = ta_arena_allocated_bytes(&system->atoms.strings),
                     .nodes = system->event_names.trie.nodes.count,
                     .trie_index = ta_table_count(&system->event_names.trie.index) };
}

static bool same_tables(tables a, tables b) {
    return a.atoms == b.atoms && a.index == b.index && a.text == b.text && a.nodes == b.nodes &&
           a.trie_index == b.trie_index;
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t random32(void) {
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return (uint32_t)(rng_state >> 16);
}

/* Taking events and resolving random names grow neither the atom table nor the trie, and a take allocates nothing. */
static void test_tables_unchanged_and_no_allocation(void) {
    run r;
    start(&r);
    ta_event_name_id names[6];
    static const char *const text[6] = { "order.123.paid", "order.124", "orders.1",
                                         "error.communication.invoke.payment.timeout", "x9f2.qq.zz", "order" };
    for (uint32_t index = 0; index < 6; index += 1) {
        names[index] = intern(&r, text[index]); /* outside the measured region */
    }
    tables before = snapshot(r.system);
    for (uint32_t round = 0; round < 60; round += 1) {
        CHECK(fire(&r, names[round % 6])[0] != 0);
    }
    /* The take's own step allocates nothing: with every request refused, resolution still gives the atoms. */
    ta_actor *actor = ta_actor_lookup(r.system, r.actor);
    ta_interpreter it;
    CHECK(actor != NULL && ta_interpreter_init(&it, r.system, actor) == TA_OK);
    static const uint32_t segments_of[6] = { 3, 2, 2, 5, 3, 1 };
    ta_test_allocator_reset_counts(&heap);
    ta_test_allocator_fail_from_nth(&heap, 1);
    for (uint32_t index = 0; index < 6; index += 1) {
        ta_interpreter_resolve_event(&it, &(ta_event){ .name = names[index] });
        CHECK(it.event_atom_count == segments_of[index]);
    }
    /* Random names through the resolver, on the live system. */
    for (uint32_t round = 0; round < 2000; round += 1) {
        char buffer[48];
        uint32_t length = 0;
        uint32_t segments = 1 + random32() % 6;
        for (uint32_t segment = 0; segment < segments; segment += 1) {
            if (segment != 0) {
                buffer[length++] = '.';
            }
            uint32_t letters = 1 + random32() % 4;
            for (uint32_t letter = 0; letter < letters; letter += 1) {
                buffer[length++] = (char)('a' + random32() % 26);
            }
        }
        ta_atom out[8];
        uint32_t count = 0;
        (void)ta_name_resolve(&(ta_name_resolve_params){ .atoms = &r.system->atoms, .name = { buffer, length },
                                                         .out = out, .capacity = 8, .count = &count });
    }
    CHECK(heap.stats.attempts == 0);
    ta_test_allocator_fail_from_nth(&heap, 0);
    CHECK(same_tables(before, snapshot(r.system)));
    close_run(&r);
}

/*
 * An invoke ID is one atom even when its text has dots (generated IDs are
 * <state>.<serial>). The name an invoke failure builds keeps it one segment:
 * "payment.7" is not "payment", so the descriptor for invoke.payment does not
 * match, and a many-dotted ID does not make the name deeper than
 * eventNameDepth (8), which would have left it with no atoms at all.
 */
static void test_dotted_invoke_id_is_one_segment(void) {
    next_param(TA_PARAM_EVENT_NAME_DEPTH, 8);
    run r;
    start(&r);
    static const char *const ids[] = { "payment.7", "p.q.r.s.t.u.v" };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(ids); index += 1) {
        ta_invoke_id id = TA_INVOKE_ID_NONE;
        CHECK(ta_system_intern_invoke_id(r.system, (ta_str){ ids[index], (uint32_t)strlen(ids[index]) }, &id) == TA_OK);
        ta_event_name_id failure = 0;
        CHECK(ta_invoke_error_name(r.system, id, STR("timeout"), &failure) == TA_OK);
        ta_atom path[TA_TRIE_MAX_DEPTH];
        CHECK(ta_trie_segments(&r.system->event_names.trie, failure, path) == 5u);
        CHECK(strcmp(fire(&r, failure), "D") == 0);
    }
    close_run(&r);
}

/* The scratch figure is the bytes init takes, with the trace buffers wanted, at two depths. */
static void test_scratch_need(void) {
    static const uint64_t depths[] = { 8, 64 };
    for (uint32_t index = 0; index < TA_ARRAY_COUNT(depths); index += 1) {
        next_param(TA_PARAM_EVENT_NAME_DEPTH, depths[index]);
        run r;
        start(&r);
        CHECK(ta_system_set_trace_masks(r.system, &(ta_trace_masks){ .kinds = UINT32_MAX, .watched_kinds = UINT32_MAX,
                                                                     .unsampled_kinds = UINT32_MAX }) == TA_OK);
        const ta_definition *definition = ta_definition_lookup(r.system, r.definition);
        ta_actor *actor = ta_actor_lookup(r.system, r.actor);
        CHECK(definition != NULL && actor != NULL);
        if (definition != NULL && actor != NULL) {
            size_t need = 0;
            CHECK(ta_interpreter_scratch_need(definition, &need) == TA_OK);
            ta_interpreter it;
            CHECK(ta_interpreter_init(&it, r.system, actor) == TA_OK);
            CHECK(it.event_atom_capacity == depths[index]);
        CHECK(r.system->scratch.head->used == need);
        }
        close_run(&r);
    }
}

/* The chart needs 904 bytes of scratch at eventNameDepth 8 and 1128 at 64: with interpreterBytes 1024 only the
 * shallow system registers it. */
static void test_scratch_refused_by_deep_names(void) {
    run r;
    next_param(TA_PARAM_INTERPRETER_BYTES, 1024);
    next_param(TA_PARAM_EVENT_NAME_DEPTH, 8);
    CHECK(open_with(&r, &chart) == TA_OK);
    close_run(&r);
    next_param(TA_PARAM_INTERPRETER_BYTES, 1024);
    next_param(TA_PARAM_EVENT_NAME_DEPTH, 64);
    CHECK(open_with(&r, &chart) != TA_OK);
    close_run(&r);
}

int main(void) {
    test_worked_examples();
    test_unknown_segments();
    test_tables_unchanged_and_no_allocation();
    test_dotted_invoke_id_is_one_segment();
    test_scratch_need();
    test_scratch_refused_by_deep_names();
    CHECK(ta_test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "event_match_test: %d failures\n", failures);
        return 1;
    }
    puts("event_match_test: ok");
    return 0;
}
