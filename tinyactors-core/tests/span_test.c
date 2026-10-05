/*
 * Source spans (ADR 0085): a spec's spans reach the definition's side table,
 * ta_definition_span() answers by kind and ID, export carries them back,
 * ta_definition_encode() reports where a codec wrote each element, and no
 * span ever changes a revision. A spec without spans keeps nothing, and a
 * compile that runs out of memory anywhere leaks nothing.
 *
 * The test includes the amalgamation so it can read a compiled definition's
 * identity before registration; it uses the C library, the core does not.
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define E(text, at) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .span = (at), .as.source = S(text) })
#define ACTIONS(...) ((const ta_scxml_action_spec[]){ __VA_ARGS__, {0} })

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * Host fixtures: a data model that evaluates nothing, and a test allocator.
 */

static ta_result null_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}
static void null_destroy_definition(void *self, void *shared, ta_allocator allocator) {
    (void)self, (void)shared, (void)allocator;
}
static uint32_t null_instance_size(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static uint32_t null_instance_alignment(void *self, void *shared) {
    (void)self, (void)shared;
    return 1;
}
static ta_result null_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self, (void)params;
    return TA_OK;
}
static void null_destroy_instance(void *self, void *shared, void *instance) {
    (void)self, (void)shared, (void)instance;
}

static const ta_datamodel_vtable null_datamodel_vtable = {
    .name = { .ptr = "null", .len = 4 },
    .prepare_definition = null_prepare,
    .destroy_definition = null_destroy_definition,
    .instance_size = null_instance_size,
    .instance_alignment = null_instance_alignment,
    .init_instance = null_init_instance,
    .destroy_instance = null_destroy_instance,
};
static const ta_datamodel null_datamodel = { .self = NULL, .v = &null_datamodel_vtable };

static test_allocator heap;

/* ------------------------------------------------------------------------
 * The chart. Every element that becomes a compiled ID has a span, except
 * the state `gone`, whose span is unknown (all zero), and the transition
 * out of `done`, which names none.
 *
 *   states:      0 main, 1 idle, 2 busy, 3 gone, 4 done
 *   transitions: 0 idle -> busy, 1 busy -> done, 2 done -> idle (no span);
 *                3 main's <initial>, 4 the document's (default: no element)
 *   actions:     block of transition 0: 0 <if>; the root <script> after them
 *   expressions: in compile order
 */

#define LINE(n) { .offset = (n) * 100u, .length = 40, .line = (n), .column = 3, .end_line = (n), .end_column = 43 }

static ta_source_span chart_spans[] = {
    LINE(2),   /* 1: main */
    LINE(3),   /* 2: idle */
    LINE(10),  /* 3: busy */
    {0},       /* 4: gone, unknown */
    LINE(20),  /* 5: done */
    LINE(4),   /* 6: main's <initial> */
    LINE(5),   /* 7: idle -> busy */
    LINE(6),   /* 8: <if> */
    LINE(7),   /* 9: <log> in then */
    LINE(8),   /* 10: <log> in else */
    LINE(11),  /* 11: busy -> done */
    LINE(12),  /* 12: <foreach> */
    LINE(13),  /* 13: <log> in body */
    LINE(1),   /* 14: the root <script> */
    { .offset = 612, .length = 7, .line = 6, .column = 13, .end_line = 6, .end_column = 20 },  /* 15: the <if>'s cond */
    { .source = 1, .offset = 0, .length = 9, .line = 1, .column = 1, .end_line = 1, .end_column = 10 }, /* 16: script text */
};

static const ta_scxml_source chart_sources[] = { { S("chart.scxml") }, { S("lib/boot.js") } };

typedef struct {
    ta_source_span *spans;
    uint32_t span_count;
    uint32_t source_count;
    uint32_t shift;
} chart_options;

static const ta_str to_idle[] = { S("idle"), {0} };
static const ta_str to_busy[] = { S("busy"), {0} };
static const ta_str to_done[] = { S("done"), {0} };

static ta_scxml_spec chart(const chart_options *options) {
    for (uint32_t index = 0; index < options->span_count; index += 1u) {
        if (options->spans[index].line != 0) {
            options->spans[index].line += options->shift;
            options->spans[index].end_line += options->shift;
            options->spans[index].offset += options->shift * 100u;
        }
    }
    static ta_scxml_action_spec then_actions[2];
    then_actions[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_LOG, .span = 9, .as.log = { .label = S("then") } };
    static ta_scxml_action_spec else_actions[2];
    else_actions[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_LOG, .span = 10, .as.log = { .label = S("else") } };
    static ta_scxml_action_spec body[2];
    body[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_LOG, .span = 13, .as.log = { .label = S("each") } };
    static ta_scxml_action_spec if_action[2];
    if_action[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_IF, .span = 8,
                                           .as.conditional = { .condition = E("In('b')", 15),
                                                               .then_actions = then_actions,
                                                               .else_actions = else_actions } };
    static ta_scxml_action_spec foreach_action[2];
    foreach_action[0] = (ta_scxml_action_spec){ .kind = TA_SCXML_ACTION_FOREACH, .span = 12,
                                                .as.foreach = { .array = E("items", 0), .item = S("item"),
                                                                .body = body } };
    static ta_scxml_transition_spec idle_transitions[2];
    idle_transitions[0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL, .span = 7,
                                                      .event = S("go"), .targets = to_busy,
                                                      .actions = if_action };
    static ta_scxml_transition_spec busy_transitions[2];
    busy_transitions[0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL, .span = 11,
                                                      .event = S("finish"), .targets = to_done,
                                                      .actions = foreach_action };
    static ta_scxml_transition_spec done_transitions[2];
    done_transitions[0] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                                      .event = S("again"), .targets = to_idle };
    static ta_scxml_state_spec children[5];
    children[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .span = 2, .id = S("idle"),
                                         .transitions = idle_transitions };
    children[1] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .span = 3, .id = S("busy"),
                                         .transitions = busy_transitions };
    children[2] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .span = 4, .id = S("gone") };
    children[3] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .span = 5, .id = S("done"),
                                         .transitions = done_transitions };
    static ta_scxml_initial_spec initial;
    initial = (ta_scxml_initial_spec){ .targets = to_idle, .span = 6 };
    static ta_scxml_state_spec roots[2];
    roots[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_COMPOUND, .span = 1, .id = S("main"),
                                      .initial = &initial, .states = children };
    return (ta_scxml_spec){
        .name = S("spans"),
        .datamodel = S("null"),
        .states = roots,
        .script = E("boot()", 16),
        .script_span = 14,
        .spans = options->spans,
        .span_count = options->span_count,
        .sources = options->source_count == 0 ? NULL : chart_sources,
        .source_count = options->source_count,
    };
}

/* Zeroes every element's span, as a spec built without them has. */
static ta_scxml_spec without_spans(void) {
    ta_scxml_spec spec = chart(&(chart_options){0});
    ta_scxml_state_spec *main_state = (ta_scxml_state_spec *)spec.states;
    ta_scxml_state_spec *children = (ta_scxml_state_spec *)main_state->states;
    main_state->span = 0;
    ((ta_scxml_initial_spec *)main_state->initial)->span = 0;
    for (uint32_t index = 0; index < 4; index += 1u) {
        children[index].span = 0;
    }
    ta_scxml_transition_spec *go = (ta_scxml_transition_spec *)children[0].transitions;
    ta_scxml_transition_spec *finish = (ta_scxml_transition_spec *)children[1].transitions;
    go->span = 0;
    finish->span = 0;
    ta_scxml_action_spec *branch = (ta_scxml_action_spec *)go->actions;
    branch->span = 0;
    branch->as.conditional.condition.span = 0;
    ((ta_scxml_action_spec *)branch->as.conditional.then_actions)->span = 0;
    ((ta_scxml_action_spec *)branch->as.conditional.else_actions)->span = 0;
    ta_scxml_action_spec *each = (ta_scxml_action_spec *)finish->actions;
    each->span = 0;
    ((ta_scxml_action_spec *)each->as.foreach.body)->span = 0;
    spec.script.span = 0;
    spec.script_span = 0;
    return spec;
}

static ta_source_span spans_copy[TA_ARRAY_COUNT(chart_spans)];

static chart_options with_spans(uint32_t shift) {
    memcpy(spans_copy, chart_spans, sizeof(chart_spans));
    return (chart_options){ .spans = spans_copy, .span_count = TA_ARRAY_COUNT(chart_spans), .source_count = 2,
                            .shift = shift };
}

/* ------------------------------------------------------------------------
 * Helpers
 */

static ta_result compile_with(ta_system *system, const ta_scxml_spec *spec, ta_allocator allocator,
                              ta_definition **out) {
    return ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                  .spec = spec,
                                                  .datamodel = null_datamodel,
                                                  .allocator = allocator,
                                                  .out = out,
                                              });
}

static ta_definition_id register_spec(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(compile_with(system, spec, test_allocator_of(&heap), &definition) == TA_OK);
    ta_definition_id id = 0;
    CHECK(definition != NULL && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static bool same_revision(ta_system *system, ta_definition_id left, ta_definition_id right) {
    ta_definition_inspection a = {0};
    ta_definition_inspection b = {0};
    CHECK(ta_definition_inspect(system, left, &a) == TA_OK);
    CHECK(ta_definition_inspect(system, right, &b) == TA_OK);
    return a.identity.revision.len == b.identity.revision.len &&
           memcmp(a.identity.revision.ptr, b.identity.revision.ptr, a.identity.revision.len) == 0;
}

static ta_result span_of(ta_system *system, ta_definition_id definition, ta_element_kind kind, uint32_t id,
                         ta_source_location *out) {
    return ta_definition_span(system, &(ta_definition_span_params){
                                          .definition = definition, .kind = kind, .id = id, .out = out });
}

static bool same_span(const ta_source_span *left, const ta_source_span *right) {
    return memcmp(left, right, sizeof(*left)) == 0;
}

static bool is_name(ta_str text, const char *name) {
    return text.len == strlen(name) && memcmp(text.ptr, name, text.len) == 0;
}

/* The spec's span `index` (from 1), as the definition should report it. */
static void check_span(ta_system *system, ta_definition_id definition, ta_element_kind kind, uint32_t id,
                       uint32_t index) {
    ta_source_location where = {0};
    ta_result result = span_of(system, definition, kind, id, &where);
    if (result != TA_OK || !same_span(&where.span, &spans_copy[index - 1u])) {
        fprintf(stderr, "element %d/%u: %d, line %u, expected span %u (line %u)\n", (int)kind, id, (int)result,
                where.span.line, index, spans_copy[index - 1u].line);
        failures += 1;
        return;
    }
    CHECK(is_name(where.source, spans_copy[index - 1u].source == 0 ? "chart.scxml" : "lib/boot.js"));
}

/* Every element of the chart reports its span, and the unknown ones none. */
static void check_chart_spans(ta_system *system, ta_definition_id definition) {
    ta_source_location where = {0};
    check_span(system, definition, TA_ELEMENT_STATE, 0, 1);
    check_span(system, definition, TA_ELEMENT_STATE, 1, 2);
    check_span(system, definition, TA_ELEMENT_STATE, 2, 3);
    CHECK(span_of(system, definition, TA_ELEMENT_STATE, 3, &where) == TA_ERR_NOT_FOUND);
    check_span(system, definition, TA_ELEMENT_STATE, 4, 5);
    CHECK(span_of(system, definition, TA_ELEMENT_STATE, 5, &where) == TA_ERR_NOT_FOUND);

    check_span(system, definition, TA_ELEMENT_TRANSITION, 0, 7);
    check_span(system, definition, TA_ELEMENT_TRANSITION, 1, 11);
    CHECK(span_of(system, definition, TA_ELEMENT_TRANSITION, 2, &where) == TA_ERR_NOT_FOUND);
    check_span(system, definition, TA_ELEMENT_TRANSITION, 3, 6);
    CHECK(span_of(system, definition, TA_ELEMENT_TRANSITION, 4, &where) == TA_ERR_NOT_FOUND);
    CHECK(span_of(system, definition, TA_ELEMENT_TRANSITION, 5, &where) == TA_ERR_NOT_FOUND);

    /* Actions by what ta_definition_action() says of them. */
    ta_definition_inspection facts = {0};
    CHECK(ta_definition_inspect(system, definition, &facts) == TA_OK);
    CHECK(facts.action_count == 6);
    for (uint32_t id = 0; id < facts.action_count; id += 1u) {
        ta_action_info info = {0};
        CHECK(ta_definition_action(system, &(ta_definition_action_params){
                                               .definition = definition, .action = id, .out = &info }) == TA_OK);
        uint32_t index = 0;
        if (info.kind == TA_SCXML_ACTION_IF) {
            index = 8;
        } else if (info.kind == TA_SCXML_ACTION_FOREACH) {
            index = 12;
        } else if (info.kind == TA_SCXML_ACTION_SCRIPT) {
            index = 14;
        } else if (info.block == TA_ACTION_IN_THEN) {
            index = 9;
        } else if (info.block == TA_ACTION_IN_ELSE) {
            index = 10;
        } else if (info.block == TA_ACTION_IN_FOREACH) {
            index = 13;
        }
        CHECK(index != 0);
        if (index != 0) {
            check_span(system, definition, TA_ELEMENT_ACTION, id, index);
        }
    }

    /* Expressions by their text: the <if>'s cond and the script have spans. */
    CHECK(facts.expression_count == 3);
    for (uint32_t id = 0; id < facts.expression_count; id += 1u) {
        ta_expression expression = {0};
        CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                                   .definition = definition, .expression = id,
                                                   .out = &expression }) == TA_OK);
        CHECK(expression.span == 0);
        if (is_name(expression.as.source, "In('b')")) {
            check_span(system, definition, TA_ELEMENT_EXPRESSION, id, 15);
        } else if (is_name(expression.as.source, "boot()")) {
            check_span(system, definition, TA_ELEMENT_EXPRESSION, id, 16);
        } else {
            CHECK(span_of(system, definition, TA_ELEMENT_EXPRESSION, id, &where) == TA_ERR_NOT_FOUND);
        }
    }
    CHECK(span_of(system, definition, TA_ELEMENT_EXPRESSION, facts.expression_count, &where) == TA_ERR_NOT_FOUND);
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Compile copies the spans; the query answers by kind and ID. */
static void test_compile_and_query(ta_system *system) {
    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    ta_definition_id definition = register_spec(system, &spec);
    check_chart_spans(system, definition);

    ta_source_location where = {0};
    CHECK(span_of(system, definition, (ta_element_kind)7, 0, &where) == TA_ERR_INVALID);
    CHECK(ta_definition_span(system, &(ta_definition_span_params){ .definition = definition }) == TA_ERR_INVALID);
    CHECK(ta_definition_span(NULL, &(ta_definition_span_params){ .definition = definition, .out = &where })
          == TA_ERR_INVALID);
    CHECK(span_of(system, definition + 99u, TA_ELEMENT_STATE, 0, &where) == TA_ERR_NOT_FOUND);
}

/* A spec without spans compiles as before and keeps nothing. */
static void test_without_spans(ta_system *system) {
    ta_scxml_spec plain = without_spans();
    ta_definition_id definition = register_spec(system, &plain);
    ta_source_location where = {0};
    CHECK(span_of(system, definition, TA_ELEMENT_STATE, 0, &where) == TA_ERR_NOT_FOUND);
    CHECK(span_of(system, definition, TA_ELEMENT_ACTION, 0, &where) == TA_ERR_NOT_FOUND);
    const ta_definition *compiled = ta_definition_lookup(system, definition);
    CHECK(compiled != NULL && compiled->spans == NULL && compiled->sources == NULL);

    /* Spans that are all unknown keep nothing either. */
    ta_source_span unknown[TA_ARRAY_COUNT(chart_spans)] = {0};
    ta_scxml_spec blank = chart(&(chart_options){ .spans = unknown, .span_count = TA_ARRAY_COUNT(unknown) });
    ta_definition_id blank_id = register_spec(system, &blank);
    compiled = ta_definition_lookup(system, blank_id);
    CHECK(compiled != NULL && compiled->spans == NULL);

    /* Export of a definition without spans has none. */
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator_of(&heap), 512) == TA_OK);
    const ta_scxml_spec *exported = NULL;
    CHECK(ta_definition_export(system, &(ta_definition_export_params){
                                           .definition = definition, .arena = &arena, .out = &exported }) == TA_OK);
    CHECK(exported != NULL && exported->spans == NULL && exported->span_count == 0 && exported->sources == NULL);
    CHECK(exported != NULL && exported->states[0].span == 0 && exported->script_span == 0 &&
          exported->script.span == 0);
    ta_arena_destroy(&arena);
}

/*
 * Where an element was written never changes what it means: the chart with
 * spans, without them, moved down or with its source unnamed has one
 * revision, so registering it again is answered by the same definition
 * (ADR 0077). That definition then says where the chart is written now: a
 * registration that brings other spans hands them over, and one that brings
 * none keeps what it has.
 */
static void test_revision_ignores_spans(ta_system *system) {
    ta_scxml_spec plain = without_spans();
    ta_definition_id without = register_spec(system, &plain);
    chart_options options = with_spans(0);
    ta_scxml_spec spanned = chart(&options);
    ta_definition_id with = register_spec(system, &spanned);
    CHECK(with == without && same_revision(system, without, with));
    ta_source_location where = {0};
    CHECK(span_of(system, with, TA_ELEMENT_STATE, 0, &where) == TA_OK && where.span.line == 2);
    /* The same chart moved down five lines. */
    chart_options moved_options = with_spans(5);
    ta_scxml_spec moved = chart(&moved_options);
    ta_definition_id lower = register_spec(system, &moved);
    CHECK(lower == with);
    CHECK(span_of(system, lower, TA_ELEMENT_STATE, 0, &where) == TA_OK && where.span.line == 7);
    /* A source name is not part of the revision either. */
    options = with_spans(0);
    ta_scxml_spec unnamed = chart(&options);
    for (uint32_t index = 0; index < options.span_count; index += 1u) {
        spans_copy[index].source = 0;
    }
    unnamed.sources = NULL;
    unnamed.source_count = 0;
    ta_definition_id nameless = register_spec(system, &unnamed);
    CHECK(nameless == with);
    CHECK(span_of(system, nameless, TA_ELEMENT_STATE, 0, &where) == TA_OK && where.source.len == 0 &&
          where.span.line == 2);
    /* A registration without spans keeps the ones the definition has. The
     * spec is built again: chart() reuses its element arrays. */
    plain = without_spans();
    CHECK(register_spec(system, &plain) == with);
    CHECK(span_of(system, with, TA_ELEMENT_STATE, 0, &where) == TA_OK && where.span.line == 2);
    CHECK(ta_system_retire_definition(system, with) == TA_OK);
}

/* Export carries the spans back, and compiling it keeps them. */
static void test_export(ta_system *system) {
    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    ta_definition_id original = register_spec(system, &spec);
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator_of(&heap), 512) == TA_OK);
    const ta_scxml_spec *exported = NULL;
    CHECK(ta_definition_export(system, &(ta_definition_export_params){
                                           .definition = original, .arena = &arena, .out = &exported }) == TA_OK);
    if (exported == NULL) {
        ta_arena_destroy(&arena);
        return;
    }
    /* Laid out by kind and ID: 5 states, 5 transitions, 6 actions, 3 expressions. */
    CHECK(exported->span_count == 5u + 5u + 6u + 3u);
    CHECK(exported->source_count == 2 && is_name(exported->sources[1].name, "lib/boot.js"));
    CHECK(exported->states[0].span == 1u && exported->states[0].states[3].span == 5u);
    /* An element without a known span indexes nothing. */
    CHECK(exported->states[0].states[2].span == 0 && exported->states[0].states[3].transitions[0].span == 0);
    CHECK(exported->states[0].initial->span == 5u + 3u + 1u);
    CHECK(exported->states[0].states[0].transitions[0].span == 5u + 0u + 1u);
    CHECK(same_span(&exported->spans[exported->states[0].states[1].span - 1u], &spans_copy[2]));
    CHECK(exported->script_span != 0 && same_span(&exported->spans[exported->script_span - 1u], &spans_copy[13]));
    CHECK(exported->script.span != 0 && same_span(&exported->spans[exported->script.span - 1u], &spans_copy[15]));

    ta_definition_id again = register_spec(system, exported);
    CHECK(same_revision(system, original, again));
    check_chart_spans(system, again);

    /* A fixed point: exporting the recompiled definition gives the same table. */
    const ta_scxml_spec *second = NULL;
    CHECK(ta_definition_export(system, &(ta_definition_export_params){
                                           .definition = again, .arena = &arena, .out = &second }) == TA_OK);
    CHECK(second != NULL && second->span_count == exported->span_count &&
          memcmp(second->spans, exported->spans, exported->span_count * sizeof(ta_source_span)) == 0);
    ta_arena_destroy(&arena);
}

/* A span index outside the table, or a span naming no source, is refused. */
static void test_malformed(ta_system *system) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator_of(&heap), 512) == TA_OK);
    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    spec.span_count = 14; /* the <if>'s cond is span 15 */
    ta_definition *definition = NULL;
    ta_compile_error error = {0};
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &spec, .datamodel = null_datamodel,
                                                 .allocator = test_allocator_of(&heap), .out = &definition,
                                                 .error = &error, .error_arena = &arena }) == TA_ERR_INVALID);
    CHECK(error.code == TA_COMPILE_ERROR_MALFORMED_SPEC);
    CHECK(error.path_length != 0 && error.path[error.path_length - 1u].member == TA_SPEC_MEMBER_SPAN);
    CHECK(error.path_length >= 2 && error.path[error.path_length - 2u].member == TA_SPEC_MEMBER_CONDITION);
    ta_str line = {0};
    CHECK(ta_compile_error_format(&(ta_compile_error_format_params){
                                      .spec = &spec, .error = &error, .arena = &arena, .out = &line,
                                      .system = system }) == TA_OK);
    CHECK(line.ptr != NULL && strstr(line.ptr, ".actions[0].condition.span:") != NULL);

    options = with_spans(0);
    spec = chart(&options);
    spans_copy[2].source = 2; /* busy names a third source */
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &spec, .datamodel = null_datamodel,
                                                 .allocator = test_allocator_of(&heap), .out = &definition,
                                                 .error = &error, .error_arena = &arena }) == TA_ERR_INVALID);
    CHECK(error.code == TA_COMPILE_ERROR_MALFORMED_SPEC && error.path_length == 3 &&
          error.path[1].member == TA_SPEC_MEMBER_STATES && error.path[1].index == 1 &&
          error.path[2].member == TA_SPEC_MEMBER_SPAN);

    /* Without sources, a known span must name source 0. */
    options = with_spans(0);
    spec = chart(&options);
    spec.sources = NULL;
    spec.source_count = 0;
    CHECK(ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                 .spec = &spec, .datamodel = null_datamodel,
                                                 .allocator = test_allocator_of(&heap), .out = &definition,
                                                 .error = &error, .error_arena = &arena }) == TA_ERR_INVALID);
    CHECK(error.code == TA_COMPILE_ERROR_MALFORMED_SPEC && error.path_length == 2 &&
          error.path[0].member == TA_SPEC_MEMBER_SCRIPT && error.path[1].member == TA_SPEC_MEMBER_SPAN);

    /* An unknown span is never checked further. */
    options = with_spans(0);
    spec = chart(&options);
    spans_copy[3].source = 99;
    CHECK(compile_with(system, &spec, test_allocator_of(&heap), &definition) == TA_OK);
    ta_definition_destroy(definition);
    ta_arena_destroy(&arena);
}

/*
 * A codec that writes one line per element, "<kind> <span>", and reports
 * each line through the span sink. It walks with the C stack: it is test
 * code, not core.
 */
typedef struct {
    char text[4096];
    uint32_t length;
    uint32_t lines;
    const ta_chart_encode_params *params;
    uint32_t reported;
} line_writer;

static void write_line(line_writer *writer, const char *kind, uint32_t span) {
    if (span == 0) {
        return;
    }
    int written = snprintf(writer->text + writer->length, sizeof(writer->text) - writer->length, "%s %u\n", kind,
                           span);
    ta_source_span where = { .offset = writer->length, .length = (uint32_t)written - 1u, .line = ++writer->lines,
                             .column = 1, .end_line = writer->lines, .end_column = (uint32_t)written };
    writer->length += (uint32_t)written;
    const ta_chart_span_sink *sink = &writer->params->spans;
    if (sink->v != NULL) {
        sink->v->written(sink->self, span, &where);
        writer->reported += 1u;
    }
}

static void write_actions(line_writer *writer, const ta_scxml_action_spec *actions) {
    for (uint32_t index = 0; actions != NULL && actions[index].kind != TA_SCXML_ACTION_NONE; index += 1u) {
        const ta_scxml_action_spec *action = &actions[index];
        write_line(writer, "action", action->span);
        if (action->kind == TA_SCXML_ACTION_IF) {
            write_line(writer, "expression", action->as.conditional.condition.span);
            write_actions(writer, action->as.conditional.then_actions);
            write_actions(writer, action->as.conditional.else_actions);
        } else if (action->kind == TA_SCXML_ACTION_FOREACH) {
            write_line(writer, "expression", action->as.foreach.array.span);
            write_actions(writer, action->as.foreach.body);
        }
    }
}

static void write_states(line_writer *writer, const ta_scxml_state_spec *states) {
    for (uint32_t index = 0; states != NULL && states[index].kind != TA_SCXML_STATE_NONE; index += 1u) {
        const ta_scxml_state_spec *state = &states[index];
        write_line(writer, "state", state->span);
        if (state->initial != NULL) {
            write_line(writer, "initial", state->initial->span);
        }
        for (uint32_t at = 0; state->transitions != NULL && state->transitions[at].kind != TA_SCXML_TRANSITION_NONE;
             at += 1u) {
            write_line(writer, "transition", state->transitions[at].span);
            write_actions(writer, state->transitions[at].actions);
        }
        write_states(writer, state->states);
    }
}

static line_writer lines;

static ta_result lines_encode(void *self, const ta_chart_encode_params *params) {
    (void)self;
    lines = (line_writer){ .params = params };
    write_line(&lines, "script", params->spec->script_span);
    write_line(&lines, "expression", params->spec->script.span);
    write_states(&lines, params->spec->states);
    *params->out = (ta_bytes){ .ptr = (const uint8_t *)lines.text, .len = lines.length };
    return TA_OK;
}

static const ta_chart_codec_vtable lines_codec = {
    .info = { .name = { .ptr = "text/x-lines", .len = 12 }, .flags = TA_CHART_CODEC_ENCODES },
    .encode = lines_encode,
};

/* The line the encoding gave `span` (the exported element's index). */
static uint32_t line_of(uint32_t span) {
    char needle[32];
    int length = snprintf(needle, sizeof(needle), " %u\n", span);
    uint32_t line = 1;
    for (uint32_t at = 0; at + (uint32_t)length <= lines.length; at += 1u) {
        if (memcmp(lines.text + at, needle, (size_t)length) == 0) {
            return line;
        }
        line += lines.text[at] == '\n' ? 1u : 0u;
    }
    return 0;
}

/* Encode reports where each element landed, spans or not. */
static void check_encode(ta_system *system, ta_definition_id definition) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, test_allocator_of(&heap), 512) == TA_OK);
    ta_bytes out = {0};
    ta_source_map map = {0};
    ta_chart_codec codec = { .self = NULL, .v = &lines_codec };
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = definition, .codec = codec, .arena = &arena,
                                           .out = &out, .spans = &map }) == TA_OK);
    CHECK(map.state_count == 5 && map.transition_count == 5 && map.action_count == 6 && map.expression_count == 3);
    /* Every state, every <transition> and <initial>, every action and every expression was written. */
    CHECK(lines.reported == 5u + 3u + 1u + 6u + 3u);
    for (uint32_t id = 0; id < map.state_count; id += 1u) {
        CHECK(map.states[id].line != 0 && map.states[id].line == line_of(id + 1u));
    }
    for (uint32_t id = 0; id < map.transition_count; id += 1u) {
        /* Transition 4 is the document's initial transition, which has no element. */
        CHECK((map.transitions[id].line != 0) == (id != 4));
        CHECK(id == 4 || map.transitions[id].line == line_of(5u + id + 1u));
    }
    for (uint32_t id = 0; id < map.action_count; id += 1u) {
        CHECK(map.actions[id].line != 0 && map.actions[id].line == line_of(10u + id + 1u));
        CHECK(memcmp(out.ptr + map.actions[id].offset, "action", 6) == 0 ||
              memcmp(out.ptr + map.actions[id].offset, "script", 6) == 0);
    }
    for (uint32_t id = 0; id < map.expression_count; id += 1u) {
        CHECK(map.expressions[id].line == line_of(16u + id + 1u));
    }

    /* Not asked, the codec is given no sink. */
    CHECK(ta_definition_encode(system, &(ta_definition_encode_params){
                                           .definition = definition, .codec = codec, .arena = &arena,
                                           .out = &out }) == TA_OK);
    CHECK(lines.reported == 0);
    ta_arena_destroy(&arena);
}

static void test_encode(ta_system *system) {
    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    check_encode(system, register_spec(system, &spec));
    ta_scxml_spec plain = without_spans();
    check_encode(system, register_spec(system, &plain));
}

/*
 * Out of memory at every allocation a spanned compile makes: each attempt
 * fails with TA_ERR_NOMEM and gives back all it took, and the first that
 * succeeds keeps every span (ADR 0070 style).
 */
static void test_out_of_memory(ta_system *system) {
    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    bool compiled = false;
    for (uint32_t limit = 1; limit < 2000 && !compiled; limit += 1u) {
        test_allocator failing = { .policy.fail_from = limit + 1u };
        ta_definition *definition = NULL;
        ta_result result = compile_with(system, &spec, test_allocator_of(&failing), &definition);
        if (result == TA_OK) {
            compiled = true;
            CHECK(definition->spans != NULL && definition->source_count == 2);
            ta_definition_destroy(definition);
        } else {
            CHECK(result == TA_ERR_NOMEM);
        }
        if (failing.live != 0) {
            fprintf(stderr, "failing after %u allocations: %u blocks leaked\n", limit, failing.live);
            failures += 1;
        }
        CHECK(test_allocator_finish(&failing) == 0);
    }
    CHECK(compiled);
}

/* What the side table costs a definition: one block of 32 bytes a span and
 * the sources, counted in its definition_bytes, and nothing without spans. */
static void test_cost(ta_system *system) {
    ta_scxml_spec plain = without_spans();
    ta_definition_id without = register_spec(system, &plain);
    ta_definition_inspection before = {0};
    CHECK(ta_definition_inspect(system, without, &before) == TA_OK);
    const ta_definition *a = ta_definition_lookup(system, without);
    CHECK(a != NULL && a->spans == NULL && a->span_bytes == 0);
    CHECK(ta_system_retire_definition(system, without) == TA_OK);

    chart_options options = with_spans(0);
    ta_scxml_spec spec = chart(&options);
    ta_definition_id with = register_spec(system, &spec);
    const ta_definition *b = ta_definition_lookup(system, with);
    CHECK(b != NULL);
    if (b == NULL) {
        return;
    }
    CHECK(b->span_count == 19);
    CHECK(sizeof(ta_source_span) == 32);
    CHECK(b->span_bytes == 19u * sizeof(ta_source_span) + 2u * sizeof(ta_scxml_source) + strlen("chart.scxml") +
                               strlen("lib/boot.js"));
    ta_definition_inspection after = {0};
    CHECK(ta_definition_inspect(system, with, &after) == TA_OK);
    CHECK(after.definition_bytes == before.definition_bytes + b->span_bytes);
}

/* Each test in a system of its own: registering a chart again is answered
 * by the definition an earlier test left (ADR 0077). */
static void run(void (*test)(ta_system *system)) {
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator_of(&heap) }, &system) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        failures += 1;
        return;
    }
    test(system);
    ta_system_destroy(system);
}

int main(void) {
    run(test_compile_and_query);
    run(test_without_spans);
    run(test_revision_ignores_spans);
    run(test_export);
    run(test_malformed);
    run(test_encode);
    run(test_out_of_memory);
    run(test_cost);
    if (heap.live != 0) {
        fprintf(stderr, "span test: %u blocks leaked\n", heap.live);
        failures += 1;
    }
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "span test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core span test passed");
    return 0;
}
