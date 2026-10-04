/* test: standalone */
/*
 * Compile errors (ADR 0058): every code a chart can reach, the path to a
 * fault under every list member of the spec, the subject and related text
 * as the spec's own pointers, a data model naming the expression it refused,
 * the order in which faults win, formatting, and no cost on success.
 *
 * Includes the amalgamation, as export_test does; it uses the C library,
 * the core does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define END {0}

/* ------------------------------------------------------------------------
 * Fixtures
 */

static ta_test_allocator heap;
static const ta_allocator test_allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* A data model that accepts everything, or refuses `refuse` when set. */
typedef struct {
    bool refuse;
    ta_expr_id expression;
    uint32_t offset;
    uint32_t length;
    char message[32];
    /* When set, the message instead of "unexpected token". */
    ta_str long_message;
} test_model;

static ta_result model_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    test_model *model = self;
    *params->shared = NULL;
    if (model == NULL || !model->refuse) {
        return TA_OK;
    }
    if (params->rejected != NULL) {
        /* The adapter's own buffer, which it reuses: core must copy. */
        memcpy(model->message, "unexpected token", 16);
        *params->rejected = (ta_expression_rejection){ .expression = model->expression,
                                                       .offset = model->offset,
                                                       .length = model->length,
                                                       .message = model->long_message.len != 0
                                                                      ? model->long_message
                                                                      : (ta_str){ .ptr = model->message, .len = 16 } };
    }
    return TA_ERR_INVALID;
}
static void model_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self, (void)shared, (void)allocator;
}
static uint32_t model_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t model_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result model_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void model_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}

static const ta_datamodel_vtable model_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = model_prepare,
    .destroy_definition = model_destroy_definition,
    .instance_size = model_instance_size,
    .instance_alignment = model_instance_alignment,
    .init_instance = model_init_instance,
    .destroy_instance = model_destroy_instance,
};

/* A host expression whose retain refuses. */
static ta_result refusing_retain(void *self, void *value, ta_allocator allocator, void **out) {
    (void)self, (void)value, (void)allocator, (void)out;
    return TA_ERR_INVALID;
}

static ta_system *system_under_test;

typedef struct {
    ta_result result;
    ta_compile_error error;
    ta_arena arena;
} outcome;

static void compile_with(const ta_scxml_spec *spec, test_model *model, bool with_arena, outcome *out) {
    *out = (outcome){0};
    if (with_arena) {
        CHECK(ta_arena_init(&out->arena, test_allocator, 256) == TA_OK);
    }
    ta_definition *definition = NULL;
    out->result = ta_definition_compile_spec(system_under_test,
                                             &(ta_definition_compile_params){
                                                 .spec = spec,
                                                 .datamodel = { .self = model, .v = &model_vtable },
                                                 .allocator = test_allocator,
                                                 .out = &definition,
                                                 .error = &out->error,
                                                 .error_arena = with_arena ? &out->arena : NULL,
                                             });
    if (out->result == TA_OK) {
        ta_definition_destroy(definition);
    }
}

static void compile(const ta_scxml_spec *spec, outcome *out) {
    compile_with(spec, NULL, true, out);
}

static void release(outcome *out) {
    ta_arena_destroy(&out->arena);
}

/* The path as "states[0].transitions[1]...", for comparing. */
static const char *path_text(const ta_compile_error *error) {
    static char text[512];
    ta_arena arena;
    ta_str formatted = {0};
    text[0] = 0;
    if (ta_arena_init(&arena, test_allocator, 256) != TA_OK) {
        return text;
    }
    ta_compile_error path_only = { .path = error->path, .path_length = error->path_length, .message = S("x") };
    if (ta_compile_error_format(&(ta_compile_error_format_params){ .error = &path_only, .arena = &arena,
                                                                   .out = &formatted }) == TA_OK &&
        formatted.len >= 3u && formatted.len - 3u < sizeof(text)) {
        memcpy(text, formatted.ptr, formatted.len - 3u); /* drop ": x" */
        text[formatted.len - 3u] = 0;
    }
    ta_arena_destroy(&arena);
    return text;
}

static bool str_is(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

#define EXPECT(spec_ptr, expected_result, expected_code, expected_path) \
    do { \
        outcome o; \
        compile((spec_ptr), &o); \
        CHECK(o.result == (expected_result)); \
        CHECK(o.error.code == (expected_code)); \
        if (strcmp(path_text(&o.error), (expected_path)) != 0) { \
            fprintf(stderr, "%s:%d: path %s, expected %s\n", __FILE__, __LINE__, path_text(&o.error), \
                    (expected_path)); \
            failures += 1; \
        } \
        release(&o); \
    } while (0)

/* ------------------------------------------------------------------------
 * The name index: sorted once after pass one, scanned at or below 16 states
 * and binary-searched above.
 */

enum { MANY = 100 };
static char many_names[MANY][8];

static ta_str many_name(uint32_t index) {
    return (ta_str){ .ptr = many_names[index], .len = (uint32_t)strlen(many_names[index]) };
}

/* Names interned in reverse, so atom order is the reverse of document order
 * and the index really has to sort. */
static void make_many_names(void) {
    for (uint32_t index = 0; index < MANY; index += 1u) {
        snprintf(many_names[index], sizeof(many_names[index]), "n%u", index);
    }
    for (uint32_t index = MANY; index > 0; index -= 1u) {
        ta_atom atom = 0;
        CHECK(ta_system_intern_atom(system_under_test, many_name(index - 1u), &atom) == TA_OK);
    }
}

/* Every state targets another; export gives each target back by name, so a
 * lookup that found the wrong state shows up as a different name. */
static void test_many_states_resolve(void) {
    static ta_str targets[MANY][2];
    static ta_scxml_transition_spec transitions[MANY][2];
    static ta_scxml_state_spec states[MANY + 1];
    for (uint32_t index = 0; index < MANY; index += 1u) {
        targets[index][0] = many_name((index * 37u + 11u) % MANY);
        transitions[index][0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                            .event = S("go"), .targets = targets[index] };
        states[index] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = many_name(index),
                                               .transitions = transitions[index] };
    }
    ta_scxml_spec spec = { .name = S("many"), .states = states };
    ta_definition *definition = NULL;
    ta_compile_error error = {0};
    CHECK(ta_definition_compile_spec(system_under_test,
                                     &(ta_definition_compile_params){ .spec = &spec,
                                                                      .datamodel = { .v = &model_vtable },
                                                                      .allocator = test_allocator,
                                                                      .out = &definition,
                                                                      .error = &error }) == TA_OK);
    ta_definition_id id = {0};
    CHECK(definition != NULL && ta_system_register_definition(system_under_test, definition, &id) == TA_OK);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 4096) == TA_OK);
    const ta_scxml_spec *exported = NULL;
    CHECK(ta_definition_export(system_under_test, &(ta_definition_export_params){ .definition = id, .arena = &arena,
                                                                                  .out = &exported }) == TA_OK);
    for (uint32_t index = 0; exported != NULL && index < MANY; index += 1u) {
        ta_str target = exported->states[index].transitions[0].targets[0];
        CHECK(ta_str_equal(target, targets[index][0]));
    }
    ta_arena_destroy(&arena);
    CHECK(ta_system_retire_definition(system_under_test, id) == TA_OK);

    /* An unknown target is still unknown above the threshold. */
    const ta_str unknown[] = { S("n1000"), END };
    transitions[MANY / 2][0].targets = unknown;
    outcome o;
    compile(&spec, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_UNKNOWN_STATE);
    CHECK(strcmp(path_text(&o.error), "states[50].transitions[0].targets[0]") == 0);
    CHECK(o.error.subject.ptr == unknown[0].ptr);
    release(&o);
}

/* Several duplicates among many states: the one the walk meets first wins,
 * and `related` is the first definition of that name. */
static void test_many_states_duplicate(void) {
    static ta_scxml_state_spec states[MANY + 1];
    for (uint32_t index = 0; index < MANY; index += 1u) {
        states[index] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = many_name(index) };
    }
    states[70].id = many_name(10); /* 10 repeated at 70 */
    states[50].id = many_name(20); /* 20 repeated at 50: met first */
    states[60].id = many_name(5);  /* 5 repeated at 60 and 90 */
    states[90].id = many_name(5);
    ta_scxml_spec spec = { .name = S("many"), .states = states };
    outcome o;
    compile(&spec, &o);
    CHECK(o.result == TA_ERR_INVALID);
    CHECK(o.error.code == TA_COMPILE_ERROR_DUPLICATE_STATE);
    CHECK(strcmp(path_text(&o.error), "states[50].id") == 0);
    CHECK(o.error.subject.ptr == states[50].id.ptr);
    CHECK(o.error.related.ptr == states[20].id.ptr);
    release(&o);
}

/* A duplicate and another pass-one fault: whichever comes first in document
 * order is reported, as when the check ran state by state. */
static void test_duplicate_against_other_faults(void) {
    {
        ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") }, END };
        ta_scxml_state_spec states[] = {
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            { .kind = TA_SCXML_STATE_COMPOUND, .id = S("b"), .states = inner },
            { .kind = (ta_scxml_state_kind)99, .id = S("c") },
            END,
        };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_DUPLICATE_STATE, "states[1].states[0].id");
    }
    {
        ta_scxml_state_spec states[] = {
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            { .kind = (ta_scxml_state_kind)99, .id = S("c") },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            END,
        };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MALFORMED_SPEC, "states[1]");
    }
    {
        /* The fault is inside a subtree whose root repeats an earlier ID. */
        ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("") }, END };
        ta_scxml_state_spec states[] = {
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            { .kind = TA_SCXML_STATE_COMPOUND, .id = S("a"), .states = inner },
            END,
        };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_DUPLICATE_STATE, "states[1].id");
    }
}

/* LIMIT: one state more than a configuration's 8191 words can hold. */
static void test_limit(void) {
    enum { LIMIT_STATES = 8191u * 64u + 1u };
    ta_scxml_state_spec *states = calloc(LIMIT_STATES + 1u, sizeof(*states));
    char *names = calloc(LIMIT_STATES, 8);
    CHECK(states != NULL && names != NULL);
    if (states == NULL || names == NULL) {
        free(states);
        free(names);
        return;
    }
    for (uint32_t index = 0; index < LIMIT_STATES; index += 1u) {
        int length = snprintf(names + (size_t)index * 8u, 8, "L%x", index);
        states[index] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC,
                                               .id = { .ptr = names + (size_t)index * 8u, .len = (uint32_t)length } };
    }
    ta_scxml_spec spec = { .name = S("huge"), .states = states };
    EXPECT(&spec, TA_ERR_UNSUPPORTED, TA_COMPILE_ERROR_LIMIT, "");
    free(names);
    free(states);
}

/* ------------------------------------------------------------------------
 * Every code
 */

static const ta_str idle_target[] = { S("idel"), END };

static void test_unknown_state(void) {
    ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = idle_target }, END };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle"), .transitions = transitions },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("busy") },
        END,
    };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    outcome o;
    compile(&spec, &o);
    CHECK(o.result == TA_ERR_INVALID);
    CHECK(o.error.code == TA_COMPILE_ERROR_UNKNOWN_STATE);
    CHECK(strcmp(path_text(&o.error), "states[0].transitions[0].targets[0]") == 0);
    CHECK(o.error.subject.ptr == idle_target[0].ptr && o.error.subject.len == 4); /* the spec's own pointer */
    CHECK(o.error.related.ptr == states[0].id.ptr); /* nearest, one transposition away */
    release(&o);

    /* Nothing near enough: no suggestion. */
    const ta_str far[] = { S("zzzzzz"), END };
    transitions[0].targets = far;
    compile(&spec, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_UNKNOWN_STATE);
    CHECK(o.error.related.len == 0);
    release(&o);
}

static void test_duplicate_state(void) {
    ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") }, END };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
        { .kind = TA_SCXML_STATE_COMPOUND, .id = S("b"), .states = inner },
        END,
    };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    outcome o;
    compile(&spec, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_DUPLICATE_STATE);
    CHECK(strcmp(path_text(&o.error), "states[1].states[0].id") == 0);
    CHECK(o.error.subject.ptr == inner[0].id.ptr);
    CHECK(o.error.related.ptr == states[0].id.ptr);
    release(&o);
}

static void test_codes(void) {
    /* MALFORMED_SPEC: a state kind out of range. */
    {
        ta_scxml_state_spec states[] = { { .kind = (ta_scxml_state_kind)99, .id = S("a") }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MALFORMED_SPEC, "states[0]");
    }
    /* MISSING_VALUE: an empty state ID. */
    {
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("") }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].id");
    }
    /* EXCLUSIVE_MEMBERS: a send with params and content. */
    {
        ta_scxml_param_spec params[] = { { .name = S("p"), .value = { .kind = TA_SCXML_VALUE_EXPR,
                                                                      .as.expr = { .kind = TA_EXPRESSION_SOURCE,
                                                                                   .as.source = S("1") } } },
                                         END };
        ta_scxml_action_spec actions[] = {
            { .kind = TA_SCXML_ACTION_SEND,
              .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("e") },
                           .data = { .kind = TA_SCXML_VALUE_CONTENT,
                                     .as.content = { .bytes = { .ptr = (const uint8_t *)"x", .len = 1 } } },
                           .params = params } },
            END,
        };
        ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .onentry = onentry }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_EXCLUSIVE_MEMBERS, "states[0].onentry[0].actions[0]");
    }
    /* TARGET_OUTSIDE: an <initial> naming a state outside its owner. */
    {
        const ta_str outside[] = { S("z"), END };
        ta_scxml_initial_spec initial = { .targets = outside };
        ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("y") }, END };
        ta_scxml_state_spec states[] = {
            { .kind = TA_SCXML_STATE_COMPOUND, .id = S("x"), .states = inner, .initial = &initial },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("z") },
            END,
        };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        outcome o;
        compile(&spec, &o);
        CHECK(o.error.code == TA_COMPILE_ERROR_TARGET_OUTSIDE);
        CHECK(strcmp(path_text(&o.error), "states[0].initial.targets[0]") == 0);
        CHECK(o.error.subject.ptr == outside[0].ptr);
        CHECK(o.error.related.ptr == states[0].id.ptr);
        release(&o);
    }
    /* BAD_EVENT_DESCRIPTOR: the empty segment is narrowed to its token. */
    {
        ta_scxml_transition_spec transitions[] = {
            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("ok a..b") }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions },
                                         END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        outcome o;
        compile(&spec, &o);
        CHECK(o.error.code == TA_COMPILE_ERROR_BAD_EVENT_DESCRIPTOR);
        CHECK(strcmp(path_text(&o.error), "states[0].transitions[0].event") == 0);
        CHECK(o.error.subject.ptr == transitions[0].event.ptr);
        CHECK(o.error.subject_offset == 3 && o.error.subject_length == 4);
        release(&o);
    }
    /* BAD_DURATION. */
    {
        ta_scxml_action_spec actions[] = {
            { .kind = TA_SCXML_ACTION_SEND,
              .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("e") },
                           .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("soon") } } },
            END,
        };
        ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .onentry = onentry }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_BAD_DURATION, "states[0].onentry[0].actions[0].delay");
    }
    /* BAD_STRUCTURE: an atomic state with a child. */
    {
        ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("y") }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("x"), .states = inner }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_BAD_STRUCTURE, "states[0].states[0]");
    }
    /* BAD_HISTORY_DEFAULT: two default transitions. */
    {
        const ta_str to_y[] = { S("y"), END };
        ta_scxml_transition_spec two[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_y },
                                           { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_y }, END };
        ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("y") },
                                        { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = S("h"), .transitions = two },
                                        END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_COMPOUND, .id = S("x"), .states = inner }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT, "states[0].states[1].transitions[1]");
    }
    /* NO_STATES. */
    {
        ta_scxml_spec spec = { .name = S("c") };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_NO_STATES, "");
    }
    /* NO_FAMILY: no name and no identity. */
    {
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") }, END };
        ta_scxml_spec spec = { .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_NO_FAMILY, "name");
    }
    /* DATAMODEL_MISMATCH. */
    {
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") }, END };
        ta_scxml_spec spec = { .name = S("c"), .datamodel = S("ecmascript"), .states = states };
        outcome o;
        compile(&spec, &o);
        CHECK(o.result == TA_ERR_UNSUPPORTED);
        CHECK(o.error.code == TA_COMPILE_ERROR_DATAMODEL_MISMATCH);
        CHECK(strcmp(path_text(&o.error), "datamodel") == 0);
        CHECK(o.error.subject.ptr == spec.datamodel.ptr);
        release(&o);
    }
    /* UNSUPPORTED_CONSTRUCT: <invoke> as an action. */
    {
        ta_scxml_action_spec actions[] = { { .kind = TA_SCXML_ACTION_INVOKE }, END };
        ta_scxml_handler_spec onentry[] = { { .actions = actions }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .onentry = onentry }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_UNSUPPORTED, TA_COMPILE_ERROR_UNSUPPORTED_CONSTRUCT, "states[0].onentry[0].actions[0]");
    }
    /* HOST_EXPRESSION: retain() refuses, and its result is the result. */
    {
        static const ta_expression_host_vtable host = { .retain = refusing_retain };
        ta_scxml_transition_spec transitions[] = {
            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
              .condition = { .kind = TA_EXPRESSION_HOST, .as.host = { .v = &host } } },
            END,
        };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions },
                                         END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_HOST_EXPRESSION, "states[0].transitions[0].condition");
    }
    test_unknown_state();
    test_duplicate_state();
}

/* ------------------------------------------------------------------------
 * A fault under every list member of the spec
 */

static const ta_scxml_action_spec bad_raise[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = S("fine") } },
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = S("") } },
    END,
};
static const ta_scxml_action_spec good_raise[] = {
    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = S("fine") } }, END };

static void test_every_list_member(void) {
    /* states */
    {
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
                                         { .kind = TA_SCXML_STATE_ATOMIC, .id = S("") }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[1].id");
    }
    /* transitions and targets */
    {
        const ta_str targets[] = { S("a"), S("nope"), END };
        ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("e") },
                                                   { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = targets },
                                                   END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions },
                                         END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_UNKNOWN_STATE, "states[0].transitions[1].targets[1]");
    }
    /* onentry, onexit and actions */
    {
        ta_scxml_handler_spec handlers[] = { { .actions = good_raise }, { .actions = bad_raise }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .onentry = handlers }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].onentry[1].actions[1].event");
        states[0].onentry = NULL;
        states[0].onexit = handlers;
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].onexit[1].actions[1].event");
    }
    /* then, else, body */
    {
        ta_scxml_action_spec actions[] = {
            { .kind = TA_SCXML_ACTION_IF,
              .as.conditional = { .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("true") },
                                  .then_actions = bad_raise } },
            END,
        };
        ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .actions = actions }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions },
                                         END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE,
               "states[0].transitions[0].actions[0].then[1].event");
        actions[0].as.conditional.then_actions = good_raise;
        actions[0].as.conditional.else_actions = bad_raise;
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE,
               "states[0].transitions[0].actions[0].else[1].event");
        actions[0] = (ta_scxml_action_spec){
            .kind = TA_SCXML_ACTION_FOREACH,
            .as.foreach = { .array = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("xs") }, .item = S("x"),
                            .body = bad_raise },
        };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE,
               "states[0].transitions[0].actions[0].body[1].event");
    }
    /* invokes, finalize, params */
    {
        ta_scxml_invoke_spec invokes[] = {
            { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("x") } },
            { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = S("") } },
            END,
        };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .invokes = invokes }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].invokes[1].type");
        invokes[1].type.as.literal = S("x");
        invokes[1].finalize = bad_raise;
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].invokes[1].finalize[1].event");
        ta_scxml_param_spec params[] = { { .name = S("p") }, { .name = S("") }, END };
        invokes[1].finalize = NULL;
        invokes[1].params = params;
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].invokes[1].params[1].name");
    }
    /* data, in a state and at the root */
    {
        ta_scxml_data_spec data[] = { { .name = S("d") }, { .name = S("") }, END };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .data = data }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "states[0].data[1].name");
        states[0].data = NULL;
        spec.data = data;
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_MISSING_VALUE, "data[1].name");
    }
}

/* ------------------------------------------------------------------------
 * Order, success, rejection, format
 */

/* Pass one runs over every state first, so a duplicate ID in the last state
 * wins over an unknown target in the first, every time. */
static void test_order(void) {
    const ta_str nope[] = { S("nope"), END };
    ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = nope }, END };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
        END,
    };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    for (int run = 0; run < 3; run += 1) {
        EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_DUPLICATE_STATE, "states[1].id");
    }
    states[1].id = S("b");
    EXPECT(&spec, TA_ERR_INVALID, TA_COMPILE_ERROR_UNKNOWN_STATE, "states[0].transitions[0].targets[0]");
}

static void test_success_costs_nothing(void) {
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") }, END };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 256) == TA_OK);
    ta_arena before = arena;
    ta_compile_error error = { .code = TA_COMPILE_ERROR_LIMIT };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system_under_test,
                                     &(ta_definition_compile_params){ .spec = &spec,
                                                                      .datamodel = { .v = &model_vtable },
                                                                      .allocator = test_allocator,
                                                                      .out = &definition,
                                                                      .error = &error,
                                                                      .error_arena = &arena }) == TA_OK);
    CHECK(memcmp(&before, &arena, sizeof(arena)) == 0);
    CHECK(error.code == TA_COMPILE_ERROR_NONE);
    ta_definition_destroy(definition);
    ta_arena_destroy(&arena);

    /* Without an arena the error keeps its code and subject, but no path. */
    ta_scxml_state_spec bad[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("") }, END };
    spec.states = bad;
    outcome o;
    compile_with(&spec, NULL, false, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_MISSING_VALUE);
    CHECK(o.error.path == NULL && o.error.path_length == 0);
}

static void test_rejection(void) {
    /* Expressions are numbered in walk order: the condition (0), then the
     * log expression (1). */
    ta_scxml_action_spec actions[] = {
        { .kind = TA_SCXML_ACTION_LOG, .as.log = { .expr = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("1 + + 2") } } },
        END,
    };
    ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL,
          .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("ok") },
          .actions = actions },
        END,
    };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions }, END };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    test_model model = { .refuse = true, .expression = 1, .offset = 4, .length = 1 };
    outcome o;
    compile_with(&spec, &model, true, &o);
    CHECK(o.result == TA_ERR_INVALID);
    CHECK(o.error.code == TA_COMPILE_ERROR_EXPRESSION_REJECTED);
    CHECK(strcmp(path_text(&o.error), "states[0].transitions[0].actions[0].expr") == 0);
    CHECK(o.error.subject.ptr == actions[0].as.log.expr.as.source.ptr);
    CHECK(o.error.subject_offset == 4 && o.error.subject_length == 1);
    memset(model.message, '?', sizeof(model.message)); /* the adapter reuses its buffer */
    CHECK(str_is(o.error.message, "unexpected token"));
    release(&o);

    /* A rejection about no single expression still says who refused. */
    model.expression = TA_EXPR_ID_NONE;
    compile_with(&spec, &model, true, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_EXPRESSION_REJECTED);
    CHECK(o.error.path_length == 0 && o.error.subject.len == 0);
    release(&o);
}

static void test_format(void) {
    ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = idle_target }, END };
    ta_scxml_state_spec inner[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle"), .transitions = transitions },
                                    END };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("first") },
                                     { .kind = TA_SCXML_STATE_COMPOUND, .id = S("outer"), .states = inner }, END };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    outcome o;
    compile(&spec, &o);
    ta_str line = {0};
    CHECK(ta_compile_error_format(&(ta_compile_error_format_params){ .spec = &spec, .error = &o.error,
                                                                     .arena = &o.arena, .out = &line }) == TA_OK);
    const char *expected =
        "states[1](outer).states[0](idle).transitions[0].targets[0]: unknown state \"idel\"; nearest is \"idle\"";
    if (!str_is(line, expected)) {
        fprintf(stderr, "format: %.*s\n", (int)line.len, line.ptr);
        failures += 1;
    }
    release(&o);
}

/* ------------------------------------------------------------------------
 * The formatted line is bounded: a typo at the bottom of a very deep chart
 * with very long IDs, very long and odd subjects, and a long message.
 */

static bool utf8_valid(ta_str text) {
    for (uint32_t at = 0; at < text.len;) {
        unsigned char lead = (unsigned char)text.ptr[at];
        uint32_t length = lead < 0x80u ? 1u : lead < 0xC0u ? 0u : lead < 0xE0u ? 2u : lead < 0xF0u ? 3u : lead < 0xF8u ? 4u : 0u;
        if (length == 0 || length > text.len - at) {
            return false;
        }
        for (uint32_t index = 1; index < length; index += 1u) {
            if (((unsigned char)text.ptr[at + index] & 0xC0u) != 0x80u) {
                return false;
            }
        }
        at += length;
    }
    return true;
}

static ta_str format_line(const ta_scxml_spec *spec, outcome *o) {
    ta_str line = {0};
    CHECK(ta_compile_error_format(&(ta_compile_error_format_params){ .spec = spec, .error = &o->error,
                                                                     .arena = &o->arena, .out = &line }) == TA_OK);
    CHECK(line.len <= TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES);
    CHECK(memchr(line.ptr, '\n', line.len) == NULL);
    CHECK(utf8_valid(line));
    return line;
}

static bool ends_with(ta_str text, const char *suffix) {
    size_t length = strlen(suffix);
    return text.len >= length && memcmp(text.ptr + text.len - length, suffix, length) == 0;
}

static bool contains(ta_str text, const char *needle) {
    size_t length = strlen(needle);
    for (uint32_t at = 0; length <= text.len && at <= text.len - length; at += 1u) {
        if (memcmp(text.ptr + at, needle, length) == 0) {
            return true;
        }
    }
    return false;
}

static void test_deep_typo_is_bounded(void) {
    enum { DEPTH = 500, ID_LENGTH = 20000 };
    ta_scxml_state_spec (*levels)[3] = calloc(DEPTH, sizeof(*levels));
    char *ids = malloc((size_t)DEPTH * ID_LENGTH);
    CHECK(levels != NULL && ids != NULL);
    if (levels == NULL || ids == NULL) {
        free(levels);
        free(ids);
        return;
    }
    static const ta_str typo[] = { S("idel"), END };
    static ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = typo }, END };
    for (uint32_t depth = 0; depth < DEPTH; depth += 1u) {
        char *id = ids + (size_t)depth * ID_LENGTH;
        memset(id, 'x', ID_LENGTH);
        int prefix = snprintf(id, ID_LENGTH, "level%u-", depth);
        id[prefix] = 'x'; /* snprintf wrote a terminator there */
        ta_str name = { .ptr = id, .len = ID_LENGTH };
        if (depth + 1u < DEPTH) {
            levels[depth][0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_COMPOUND, .id = name,
                                                      .states = levels[depth + 1u] };
        } else {
            /* The deepest level: the typo, and the state it meant. */
            levels[depth][0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = name,
                                                      .transitions = transitions };
            levels[depth][1] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") };
        }
    }
    ta_scxml_spec spec = { .name = S("deep"), .states = levels[0] };
    outcome o;
    compile(&spec, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_UNKNOWN_STATE);
    CHECK(o.error.path_length == DEPTH + 2u); /* the struct keeps the whole path */
    ta_str line = format_line(&spec, &o);
    CHECK(strncmp(line.ptr, "states[0](level0-xxx", 20) == 0);
    CHECK(contains(line, "...).states[0](level1-"));
    CHECK(contains(line, ".[494 steps]."));
    CHECK(ends_with(line, "...).transitions[0].targets[0]: unknown state \"idel\"; nearest is \"idle\""));
    release(&o);
    free(ids);
    free(levels);
}

static void test_long_subjects_are_bounded(void) {
    enum { LENGTH = 100000 };
    char *long_text = malloc(LENGTH);
    CHECK(long_text != NULL);
    if (long_text == NULL) {
        return;
    }
    /* A very long unknown target: cut, and no nearest-name search. */
    memset(long_text, 'q', LENGTH);
    const ta_str long_target[] = { { .ptr = long_text, .len = LENGTH }, END };
    ta_scxml_transition_spec transitions[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = long_target }, END };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions }, END };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    outcome o;
    compile(&spec, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_UNKNOWN_STATE);
    CHECK(o.error.subject.len == LENGTH && o.error.related.len == 0);
    ta_str line = format_line(&spec, &o);
    CHECK(ends_with(line, "qqq...\""));
    release(&o);

    /* Two-byte characters are cut between characters, never inside one. */
    for (uint32_t at = 0; at + 1u < LENGTH; at += 2u) {
        long_text[at] = (char)0xC3;
        long_text[at + 1u] = (char)0xA9; /* é */
    }
    compile(&spec, &o);
    line = format_line(&spec, &o);
    CHECK(ends_with(line, "\xC3\xA9...\""));
    release(&o);

    /* Control characters, quotes and backslashes are escaped. */
    const ta_str odd[] = { S("a\nb\"c\\d\x01"), END };
    transitions[0].targets = odd;
    compile(&spec, &o);
    line = format_line(&spec, &o);
    CHECK(contains(line, "unknown state \"a\\nb\\\"c\\\\d\\x01\""));
    release(&o);
    free(long_text);
}

static void test_long_message_is_bounded(void) {
    enum { LENGTH = 50000 };
    char *long_message = malloc(LENGTH);
    CHECK(long_message != NULL);
    if (long_message == NULL) {
        return;
    }
    memset(long_message, 'm', LENGTH);
    ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .condition = { .kind = TA_EXPRESSION_SOURCE, .as.source = S("x") } },
        END,
    };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a"), .transitions = transitions }, END };
    ta_scxml_spec spec = { .name = S("c"), .states = states };
    test_model model = { .refuse = true, .expression = 0, .long_message = { .ptr = long_message, .len = LENGTH } };
    outcome o;
    compile_with(&spec, &model, true, &o);
    CHECK(o.error.code == TA_COMPILE_ERROR_EXPRESSION_REJECTED);
    CHECK(o.error.message.len == TA_PARAM_DEFAULT_COMPILE_MESSAGE_BYTES); /* the copy kept is bounded */
    CHECK(o.error.message.ptr != long_message);
    ta_str line = format_line(&spec, &o);
    CHECK(contains(line, "mmm..."));
    release(&o);
    free(long_message);
}

/* ------------------------------------------------------------------------
 * History defaults that loop back to a history state never reach a state
 * (SCXML 3.11), so they are BAD_HISTORY_DEFAULT. Chains that end in real
 * states stay legal.
 */

static void test_history_default_loops(void) {
    /* h targets itself. */
    {
        static const ta_str self[] = { S("h"), END };
        ta_scxml_transition_spec fallback[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = self }, END };
        ta_scxml_state_spec inner[] = {
            { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = S("h"), .transitions = fallback },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            END,
        };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("start") },
                                         { .kind = TA_SCXML_STATE_COMPOUND, .id = S("p"), .states = inner }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        outcome o;
        compile(&spec, &o);
        CHECK(o.result == TA_ERR_INVALID);
        CHECK(o.error.code == TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT);
        CHECK(strcmp(path_text(&o.error), "states[1].states[0].transitions[0].targets[0]") == 0);
        CHECK(o.error.subject.ptr == self[0].ptr);
        CHECK(o.error.related.ptr == inner[0].id.ptr);
        release(&o);
    }
    /* h1 and h2 target each other: reported where the loop closes. */
    {
        static const ta_str to_h1[] = { S("h1"), END };
        static const ta_str to_h2[] = { S("h2"), END };
        ta_scxml_transition_spec first[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_h2 }, END };
        ta_scxml_transition_spec second[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_h1 }, END };
        ta_scxml_state_spec inner[] = {
            { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = S("h1"), .transitions = first },
            { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = S("h2"), .transitions = second },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = S("a") },
            END,
        };
        ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_COMPOUND, .id = S("p"), .states = inner }, END };
        ta_scxml_spec spec = { .name = S("c"), .states = states };
        outcome o;
        compile(&spec, &o);
        CHECK(o.error.code == TA_COMPILE_ERROR_BAD_HISTORY_DEFAULT);
        CHECK(strcmp(path_text(&o.error), "states[0].states[1].transitions[0].targets[0]") == 0);
        CHECK(o.error.subject.ptr == to_h1[0].ptr);
        CHECK(o.error.related.ptr == inner[0].id.ptr);
        ta_str line = format_line(&spec, &o);
        CHECK(ends_with(line, "history defaults loop and never reach a state \"h1\"; see also \"h1\""));
        release(&o);
    }
}

/* hp -> hq -> x, and two histories sharing hq: no loop. The chart compiles,
 * and entering hp for the first time enters x. */
static void test_history_default_chains(void) {
    static const ta_str to_hq[] = { S("hq"), END };
    static const ta_str to_x[] = { S("x"), END };
    static const ta_str to_hp[] = { S("hp"), END };
    ta_scxml_transition_spec hp_default[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_hq }, END };
    ta_scxml_transition_spec hr_default[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_hq }, END };
    ta_scxml_transition_spec hq_default[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = to_x }, END };
    ta_scxml_transition_spec go[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("go"), .targets = to_hp }, END };
    ta_scxml_state_spec q_children[] = {
        { .kind = TA_SCXML_STATE_HISTORY_SHALLOW, .id = S("hq"), .transitions = hq_default },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("w") },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("x") },
        END,
    };
    ta_scxml_state_spec p_children[] = {
        { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = S("hp"), .transitions = hp_default },
        { .kind = TA_SCXML_STATE_HISTORY_DEEP, .id = S("hr"), .transitions = hr_default },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("v") },
        { .kind = TA_SCXML_STATE_COMPOUND, .id = S("q"), .states = q_children },
        END,
    };
    ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("start"), .transitions = go },
        { .kind = TA_SCXML_STATE_COMPOUND, .id = S("p"), .states = p_children },
        END,
    };
    ta_scxml_spec spec = { .name = S("chains"), .states = states };
    ta_definition *definition = NULL;
    ta_compile_error error = {0};
    CHECK(ta_definition_compile_spec(system_under_test,
                                     &(ta_definition_compile_params){ .spec = &spec,
                                                                      .datamodel = { .v = &model_vtable },
                                                                      .allocator = test_allocator,
                                                                      .out = &definition,
                                                                      .error = &error }) == TA_OK);
    CHECK(error.code == TA_COMPILE_ERROR_NONE);
    ta_definition_id id = 0;
    CHECK(definition != NULL && ta_system_register_definition(system_under_test, definition, &id) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system_under_test, &(ta_actor_spawn_params){ .definition = id, .out = &actor }) == TA_OK);
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(system_under_test, &(ta_actor_resume_params){ .actor = actor, .budget = 100,
                                                                        .status = &status }) == TA_OK);
    ta_atom go_atom = 0;
    CHECK(ta_system_intern_atom(system_under_test, S("go"), &go_atom) == TA_OK);
    ta_event_name_id go_name = 0;
    CHECK(ta_event_name_intern(system_under_test, &(ta_atom_list){ .atoms = &go_atom, .count = 1 }, &go_name) == TA_OK);
    ta_event event = { .name = go_name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system_under_test, &(ta_actor_deliver_params){ .time = ta_system_time(system_under_test), .actor = actor, .event = &event }) == TA_OK);
    CHECK(ta_actor_resume(system_under_test, &(ta_actor_resume_params){ .actor = actor, .budget = 100,
                                                                        .status = &status }) == TA_OK);
    CHECK(status == TA_RUN_STABLE);
    ta_state_id x = 0;
    CHECK(ta_definition_find_state(system_under_test, &(ta_definition_find_state_params){ .definition = id,
                                                                                         .id = S("x"),
                                                                                         .out = &x }) == TA_OK);
    ta_state_id active[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system_under_test, &(ta_actor_active_states_params){ .actor = actor,
                                                                                     .states = active,
                                                                                     .capacity = 8,
                                                                                     .count = &count }) == TA_OK);
    CHECK(count == 3 && active[count - 1u] == x); /* p, q, x */
    CHECK(ta_actor_destroy(system_under_test, actor) == TA_OK);
    CHECK(ta_system_retire_definition(system_under_test, id) == TA_OK);
}

/*
 * compileTextBytes (ADR 0092): a deep path and long texts formatted at 256,
 * 1024 and 8192 bytes. Each line stays within its size, and a larger size
 * shows more of the path and more of each text.
 */
static void test_text_bytes(void) {
    ta_spec_step path[60];
    for (uint32_t index = 0; index < 60u; index += 1u) {
        path[index] = (ta_spec_step){ .member = index % 2u == 0 ? TA_SPEC_MEMBER_TRANSITIONS : TA_SPEC_MEMBER_TARGETS,
                                      .index = 4000000000u + index };
    }
    static char long_text[20000];
    memset(long_text, 'y', sizeof long_text);
    ta_str text = { .ptr = long_text, .len = (uint32_t)sizeof long_text };
    ta_compile_error error = { .code = TA_COMPILE_ERROR_UNKNOWN_STATE, .path = path, .path_length = 60u,
                               .subject = text, .related = text, .message = text };
    static const uint32_t sizes[3] = { 256u, 1024u, 8192u };
    uint32_t previous_length = 0;
    uint32_t previous_shown = 0;
    for (uint32_t which = 0; which < 3u; which += 1u) {
        ta_system *system = NULL;
        ta_param_value param = { .param = TA_PARAM_COMPILE_TEXT_BYTES, .value = sizes[which] };
        CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator, .params = &param, .param_count = 1 },
                               &system) == TA_OK);
        uint64_t read = 0;
        CHECK(ta_system_param(system, TA_PARAM_COMPILE_TEXT_BYTES, &read) == TA_OK && read == sizes[which]);
        ta_arena arena;
        CHECK(ta_arena_init(&arena, test_allocator, 256) == TA_OK);
        ta_str line = { 0 };
        CHECK(ta_compile_error_format(&(ta_compile_error_format_params){
                                          .error = &error, .arena = &arena, .out = &line, .system = system }) == TA_OK);
        CHECK(line.len <= sizes[which]);
        CHECK(memchr(line.ptr, '\n', line.len) == NULL);
        /* Steps shown: each "[4000000..." index. */
        uint32_t shown = 0;
        for (uint32_t at = 0; at + 4u <= line.len; at += 1u) {
            shown += memcmp(line.ptr + at, "[400", 4) == 0 ? 1u : 0u;
        }
        CHECK(strstr(line.ptr, " steps]") != NULL);
        CHECK(line.len > previous_length);
        CHECK(shown > previous_shown || (which == 0u && shown >= 2u));
        previous_length = line.len;
        previous_shown = shown;
        ta_arena_destroy(&arena);
        ta_system_destroy(system);
    }
    /* Without a system: the default, 1024. */
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator, 256) == TA_OK);
    ta_str line = { 0 };
    CHECK(ta_compile_error_format(&(ta_compile_error_format_params){ .error = &error, .arena = &arena, .out = &line }) ==
          TA_OK);
    CHECK(line.len <= TA_PARAM_DEFAULT_COMPILE_TEXT_BYTES && line.len > 256u);
    ta_arena_destroy(&arena);
    /* Out of bounds is refused, naming the parameter. */
    ta_system *refused = NULL;
    ta_param rejected = TA_PARAM_COUNT;
    ta_param_value small = { .param = TA_PARAM_COMPILE_TEXT_BYTES, .value = 255u };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator, .params = &small, .param_count = 1,
                                                .rejected = &rejected },
                           &refused) == TA_ERR_INVALID);
    CHECK(rejected == TA_PARAM_COMPILE_TEXT_BYTES);
}

int main(void) {
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system_under_test) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    make_many_names();
    test_codes();
    test_many_states_resolve();
    test_many_states_duplicate();
    test_duplicate_against_other_faults();
    test_limit();
    test_every_list_member();
    test_order();
    test_success_costs_nothing();
    test_rejection();
    test_text_bytes();
    test_format();
    test_deep_typo_is_bounded();
    test_long_subjects_are_bounded();
    test_long_message_is_bounded();
    test_history_default_loops();
    test_history_default_chains();
    ta_system_destroy(system_under_test);
    if (failures != 0) {
        fprintf(stderr, "compile error test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core compile error test passed");
    return 0;
}
