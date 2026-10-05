/*
 * The null data model core ships (SCXML B.1, ADR 0057): its name and shape,
 * the In(state) predicate, compile-time refusal of every other expression,
 * and run-time error.execution for operations that need data.
 *
 * Uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
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
#define END {0}

/* ------------------------------------------------------------------------
 * A test allocator, which counts what is outstanding
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * Helpers
 */

typedef struct {
    ta_result result;
    ta_compile_error error;
    ta_arena arena;
    ta_definition *definition;
} compiled;

static void compile(ta_system *system, const ta_scxml_spec *spec, compiled *out) {
    *out = (compiled){0};
    CHECK(ta_arena_init(&out->arena, allocator, 256) == TA_OK);
    out->result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                         .spec = spec,
                                                         .datamodel = ta_null_datamodel(),
                                                         .allocator = allocator,
                                                         .out = &out->definition,
                                                         .error = &out->error,
                                                         .error_arena = &out->arena,
                                                     });
}

static void release(compiled *out) {
    if (out->definition != NULL) {
        ta_definition_destroy(out->definition);
    }
    ta_arena_destroy(&out->arena);
}

static bool str_is(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

/* Spawns an actor of `spec`, runs it to stability, delivers `event` when
 * given, and answers whether the state `expected` is then active. */
static bool lands_in(const ta_scxml_spec *spec, const char *event, const char *expected) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    compiled chart;
    compile(system, spec, &chart);
    CHECK(chart.result == TA_OK);
    bool active = false;
    ta_definition_id id = 0;
    if (chart.result == TA_OK && ta_system_register_definition(system, chart.definition, &id) == TA_OK) {
        chart.definition = NULL; /* the system owns it now */
        ta_actor_id actor = 0;
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
        if (event != NULL) {
            ta_event delivered = { .name = event_name(system, event), .type = TA_EVENT_EXTERNAL,
                                   .invoke_id = TA_INVOKE_ID_NONE };
            CHECK(test_drive_deliver(system,
                                     &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                                 .event = &delivered },
                                     &(test_drive_run_params){ .status = &status }) == TA_OK);
        }
        ta_state_id state = 0;
        CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                                   .definition = id, .id = { .ptr = expected,
                                                                             .len = (uint32_t)strlen(expected) },
                                                   .out = &state }) == TA_OK);
        ta_state_id states[16];
        uint32_t count = 0;
        CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = states,
                                                                              .capacity = 16, .count = &count }) ==
              TA_OK);
        for (uint32_t index = 0; index < count; index += 1u) {
            active = active || states[index] == state;
        }
    }
    release(&chart);
    ta_system_destroy(system);
    return active;
}

/* ------------------------------------------------------------------------
 * The adapter's shape
 */

static void test_shape(void) {
    ta_datamodel model = ta_null_datamodel();
    CHECK(model.self == NULL);
    CHECK(model.v != NULL && str_is(model.v->name, "null"));
    CHECK(model.v->inspect == NULL);
    CHECK(model.v->instance_size(model.self, NULL) == 0);
    CHECK(model.v->instance_alignment(model.self, NULL) == 1);

    /* A chart without expressions keeps nothing per definition. */
    void *shared = (void *)&shared;
    CHECK(model.v->prepare_definition(model.self, &(ta_datamodel_prepare_definition_params){
                                                      .allocator = allocator, .shared = &shared }) == TA_OK);
    CHECK(shared == NULL);

    /* A binding is host data, which the profile cannot hold. */
    static const ta_value_vtable host_values = {0};
    ta_value binding = { .v = &host_values };
    CHECK(model.v->init_instance(model.self, &(ta_datamodel_init_instance_params){ .binding = &binding }) ==
          TA_ERR_UNSUPPORTED);
    CHECK(model.v->init_instance(model.self, &(ta_datamodel_init_instance_params){0}) == TA_OK);
}

/* ------------------------------------------------------------------------
 * In(state)
 */

/* The shape of W3C test436: In() sees the sibling region of a parallel
 * state, is false for an inactive state and for one the chart lacks, and
 * accepts either quote, no quote, and spacing. */
static void test_in(void) {
    static const ta_str to_yes[] = { STR("yes"), END };
    static const ta_str to_no[] = { STR("no"), END };
    struct { const char *event; const char *condition; bool expected; } cases[] = {
        { "q", "In('ps1')", true },       { "q", "In(\"ps1\")", true },  { "q", "In(ps1)", true },
        { "q", " In ( \"ps1\" ) ", true }, { "q", "In('s1')", false },   { "q", "In('no_such_state')", false },
        { "q", "In('ps0')", true },
    };
    for (size_t index = 0; index < sizeof(cases) / sizeof(cases[0]); index += 1u) {
        ta_scxml_transition_spec ps0_transitions[] = {
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { .ptr = cases[index].event, .len = 1 },
              .condition = { .kind = TA_EXPRESSION_SOURCE,
                             .as.source = { .ptr = cases[index].condition,
                                            .len = (uint32_t)strlen(cases[index].condition) } },
              .targets = to_yes },
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { .ptr = cases[index].event, .len = 1 }, .targets = to_no },
            END,
        };
        ta_scxml_state_spec regions[] = {
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ps0"), .transitions = ps0_transitions },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ps1") },
            END,
        };
        ta_scxml_state_spec states[] = {
            { .kind = TA_SCXML_STATE_PARALLEL, .id = STR("p"), .states = regions },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s1") },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("yes") },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("no") },
            END,
        };
        ta_scxml_spec spec = { .name = STR("in"), .datamodel = STR("null"), .states = states };
        bool answered = lands_in(&spec, cases[index].event, "yes");
        if (answered != cases[index].expected) {
            fprintf(stderr, "In case %s: expected %d\n", cases[index].condition, cases[index].expected);
            failures += 1;
        }
    }
}

/* ------------------------------------------------------------------------
 * Compile-time refusal
 */

static void test_refuses_other_expressions(void) {
    const char *refused[] = { "1 + 1", "In(", "In('a)", "In()", "In('a') && true", "Inside('a')", "in('a')", "" };
    for (size_t index = 0; index < sizeof(refused) / sizeof(refused[0]); index += 1u) {
        ta_str source = { .ptr = refused[index], .len = (uint32_t)strlen(refused[index]) };
        ta_scxml_transition_spec transitions[] = {
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e"),
              .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = source } },
            END,
        };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = transitions },
                                         END };
        ta_scxml_spec spec = { .name = STR("refused"), .states = states };
        ta_system *system = NULL;
        CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
        compiled chart;
        compile(system, &spec, &chart);
        if (chart.result != TA_ERR_INVALID || chart.error.code != TA_COMPILE_ERROR_EXPRESSION_REJECTED) {
            fprintf(stderr, "expression \"%s\" was not refused (result %d, code %d)\n", refused[index],
                    (int)chart.result, (int)chart.error.code);
            failures += 1;
        }
        CHECK(chart.error.subject.ptr == source.ptr);
        CHECK(str_is(chart.error.message, "the null data model has only the In(state) predicate"));
        CHECK(chart.error.path_length == 3); /* states[0].transitions[0].condition */
        release(&chart);
        ta_system_destroy(system);
    }

    /* A value expression is refused too, even In() used as a value: the
     * profile has no values. Refused when defined, not on first use. */
    ta_scxml_action_spec actions[] = {
        { .kind = TA_SCXML_ACTION_LOG, .as.log = { .expr = { .kind = TA_EXPRESSION_SOURCE, .as.source = STR("42") } } },
        END,
    };
    ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("e"),
                                                 .actions = actions },
                                               END };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = transitions },
                                     END };
    ta_scxml_spec spec = { .name = STR("value"), .states = states };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    compiled chart;
    compile(system, &spec, &chart);
    CHECK(chart.result == TA_ERR_INVALID && chart.error.code == TA_COMPILE_ERROR_EXPRESSION_REJECTED);
    CHECK(chart.error.subject.ptr == actions[0].as.log.expr.as.source.ptr);
    release(&chart);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Run-time errors for operations without expressions
 */

static void test_runtime_errors(void) {
    static const ta_str to_faulted[] = { STR("faulted"), END };
    /* Literal content has no expression, so it compiles, and importing it
     * needs data the profile does not have. */
    ta_scxml_action_spec send[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("out") },
                       .data = { .kind = TA_SCXML_VALUE_CONTENT,
                                 .as.content = { .bytes = { .ptr = (const uint8_t *)"literal", .len = 7 } } } } },
        END,
    };
    ta_scxml_transition_spec start_transitions[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("send"), .actions = send },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"), .targets = to_faulted },
        END,
    };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .transitions = start_transitions },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("faulted") },
        END,
    };
    ta_scxml_spec spec = { .name = STR("content"), .states = states };
    CHECK(lands_in(&spec, "send", "faulted"));

    /* A data declaration without an initializer has no expression either;
     * declaring it fails when the actor starts. */
    ta_scxml_transition_spec wait[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"),
                                          .targets = to_faulted },
                                        END };
    ta_scxml_state_spec with_data[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .transitions = wait },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("faulted") },
        END,
    };
    ta_scxml_data_spec data[] = { { .name = STR("x") }, END };
    ta_scxml_spec data_spec = { .name = STR("data"), .states = with_data, .data = data };
    CHECK(lands_in(&data_spec, NULL, "faulted"));
}

/* ------------------------------------------------------------------------
 * A literal <invoke src> reaches the invoker as text: it is never imported
 * into the data model, which the null data model could not do.
 */

typedef struct {
    int starts;
    char src_text[32];
    bool has_src_value;
} invoke_record;

static invoke_record invoked;

static ta_op_status record_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    const ta_invoke_request *request = params->request;
    invoked.starts += 1;
    invoked.has_src_value = request->src.v != NULL;
    if (request->src_text.ptr != NULL && request->src_text.len < sizeof invoked.src_text) {
        memcpy(invoked.src_text, request->src_text.ptr, request->src_text.len);
        invoked.src_text[request->src_text.len] = '\0';
    }
    *params->service_handle = 1;
    return TA_OP_OK;
}

static ta_op_status ignore_cancel(void *self, uint64_t service_handle, ta_fault *fault) {
    (void)self;
    (void)service_handle;
    (void)fault;
    return TA_OP_OK;
}

static ta_op_status ignore_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self;
    (void)params;
    return TA_OP_OK;
}

static void test_literal_invoke_src(void) {
    static const ta_invoker_vtable recorder = { .start = record_start, .cancel = ignore_cancel,
                                                .forward = ignore_forward };
    static const ta_str to_faulted[] = { STR("faulted"), END };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(system, STR("svc"), &type) == TA_OK);
    CHECK(ta_system_register_invoker(system, type, (ta_invoker){ .v = &recorder }) == TA_OK);
    ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE,
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") },
          .src = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("lit-src") } },
        END,
    };
    ta_scxml_transition_spec failed[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"), .targets = to_faulted },
        END,
    };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("start"), .transitions = failed, .invokes = invokes },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("faulted") },
        END,
    };
    ta_scxml_spec spec = { .name = STR("invoking"), .states = states };
    invoked = (invoke_record){0};
    compiled chart;
    compile(system, &spec, &chart);
    CHECK(chart.result == TA_OK);
    ta_definition_id id = 0;
    if (chart.result == TA_OK && ta_system_register_definition(system, chart.definition, &id) == TA_OK) {
        chart.definition = NULL; /* the system owns it now */
        ta_actor_id actor = 0;
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
        CHECK(invoked.starts == 1);
        CHECK(strcmp(invoked.src_text, "lit-src") == 0);
        CHECK(!invoked.has_src_value);
        ta_state_id faulted = 0;
        CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                                   .definition = id, .id = STR("faulted"), .out = &faulted }) ==
              TA_OK);
        ta_state_id active[4];
        uint32_t count = 0;
        CHECK(ta_actor_active_states(system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                              .capacity = 4, .count = &count }) ==
              TA_OK);
        CHECK(count == 1 && active[0] != faulted);
    }
    release(&chart);
    ta_system_destroy(system);
}

int main(void) {
    test_shape();
    test_in();
    test_refuses_other_expressions();
    test_runtime_errors();
    test_literal_invoke_src();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "null data model test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core null data model test passed");
    return 0;
}
