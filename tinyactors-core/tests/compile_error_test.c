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

static void *test_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size = (size + alignment - 1u) / alignment * alignment;
    return aligned_alloc(alignment, size == 0 ? alignment : size);
}

static void test_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    free(params->ptr);
}

static const ta_allocator_vtable test_allocator_vtable = { .alloc = test_alloc, .free = test_free };
static const ta_allocator test_allocator = { .self = NULL, .v = &test_allocator_vtable };

/* A data model that accepts everything, or refuses `refuse` when set. */
typedef struct {
    bool refuse;
    ta_expr_id expression;
    uint32_t offset;
    uint32_t length;
    char message[32];
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
                                                       .message = { .ptr = model->message, .len = 16 } };
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
    /* LIMIT needs more than 524224 states; see the ADR's implementation notes. */
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

int main(void) {
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system_under_test) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    test_codes();
    test_every_list_member();
    test_order();
    test_success_costs_nothing();
    test_rejection();
    test_format();
    ta_system_destroy(system_under_test);
    if (failures != 0) {
        fprintf(stderr, "compile error test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core compile error test passed");
    return 0;
}
