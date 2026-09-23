/*
 * ta_definition_export(): representative charts export to specs that
 * recompile to the same revision (ADR 0042), and the authored text the
 * compiler normalizes survives export.
 *
 * The test includes the amalgamation so it can read a compiled definition's
 * identity before registration; it uses the C library, the core does not.
 */
#include "../include/tinyactors.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define E(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = S(text) })
#define LIT(text) ((ta_scxml_string_spec){ .kind = TA_SCXML_STRING_LITERAL, .as.literal = S(text) })
#define EXPR(text) ((ta_scxml_string_spec){ .kind = TA_SCXML_STRING_EXPR, .as.expr = E(text) })
#define VEXPR(text) ((ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_EXPR, .as.expr = E(text) })
#define VLOC(text) ((ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_LOCATION, .as.location = E(text) })
#define VCONTENT(text) \
    ((ta_scxml_value_spec){ .kind = TA_SCXML_VALUE_CONTENT, \
                            .as.content.bytes = { .ptr = (const uint8_t *)(text), \
                                                   .len = (uint32_t)(sizeof(text) - 1u) } })
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, {0} })
#define ACTIONS(...) ((const ta_scxml_action_spec[]){ __VA_ARGS__, {0} })
#define NO_ACTIONS ((const ta_scxml_action_spec[]){ {0} })

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            failures += 1; \
        } \
    } while (0)

/* ------------------------------------------------------------------------
 * Host fixtures: a libc allocator and a data model that evaluates nothing.
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

/* ------------------------------------------------------------------------
 * Round trip: compile, register, export, recompile, compare revisions.
 */

static bool same_str(ta_str left, ta_str right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

static bool same_identity(const ta_definition_identity *left, const ta_definition_identity *right) {
    return same_str(left->family.name, right->family.name) && left->revision.len == right->revision.len &&
           memcmp(left->revision.ptr, right->revision.ptr, left->revision.len) == 0;
}

static ta_definition *compile(ta_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                              .spec = spec,
                                                              .datamodel = null_datamodel,
                                                              .allocator = test_allocator,
                                                              .out = &definition,
                                                          });
    CHECK(result == TA_OK);
    return result == TA_OK ? definition : NULL;
}

typedef struct {
    ta_system *system;
    ta_arena *arena;
    /* The exported spec of the original definition. */
    const ta_scxml_spec *exported;
    ta_definition_id id;
} roundtrip;

/*
 * Compiles `spec`, exports it, and checks that the export recompiles to the
 * same identity, and that exporting the recompiled definition is again
 * faithful (export is a fixed point).
 */
static void check_roundtrip(roundtrip *trip, const char *name, const ta_scxml_spec *spec) {
    ta_definition *original = compile(trip->system, spec);
    if (original == NULL) {
        fprintf(stderr, "%s: the original does not compile\n", name);
        return;
    }
    ta_definition_identity identity = original->identity;
    CHECK(ta_system_register_definition(trip->system, original, &trip->id) == TA_OK);
    trip->exported = NULL;
    CHECK(ta_definition_export(trip->system, &(ta_definition_export_params){
                                                 .definition = trip->id,
                                                 .arena = trip->arena,
                                                 .out = &trip->exported,
                                             }) == TA_OK);
    if (trip->exported == NULL) {
        fprintf(stderr, "%s: export failed\n", name);
        return;
    }
    CHECK((trip->exported->identity == NULL) == (spec->identity == NULL));
    ta_definition *again = compile(trip->system, trip->exported);
    if (again == NULL) {
        fprintf(stderr, "%s: the export does not compile\n", name);
        return;
    }
    if (!same_identity(&identity, &again->identity)) {
        fprintf(stderr, "%s: revision %.*s became %.*s\n", name, (int)identity.revision.len,
                (const char *)identity.revision.ptr, (int)again->identity.revision.len,
                (const char *)again->identity.revision.ptr);
        failures += 1;
    }
    ta_definition_id again_id = 0;
    CHECK(ta_system_register_definition(trip->system, again, &again_id) == TA_OK);
    const ta_scxml_spec *second = NULL;
    CHECK(ta_definition_export(trip->system, &(ta_definition_export_params){
                                                 .definition = again_id,
                                                 .arena = trip->arena,
                                                 .out = &second,
                                             }) == TA_OK);
    ta_definition *third = second == NULL ? NULL : compile(trip->system, second);
    CHECK(third != NULL && same_identity(&identity, &third->identity));
    ta_definition_destroy(third);
}

static ta_bytes revision_of(ta_system *system, const ta_scxml_spec *spec, uint8_t buffer[16]) {
    ta_definition *definition = compile(system, spec);
    ta_bytes revision = {0};
    if (definition != NULL && definition->identity.revision.len == 16) {
        memcpy(buffer, definition->identity.revision.ptr, 16);
        revision = (ta_bytes){ .ptr = buffer, .len = 16 };
    }
    ta_definition_destroy(definition);
    return revision;
}

/* ------------------------------------------------------------------------
 * Charts
 */

/* Parallel regions, shallow and deep history, authored and default initial states. */
static void test_structure(roundtrip *trip) {
    ta_scxml_spec spec = {
        .name = S("structure"),
        .datamodel = S("null"),
        .states = (const ta_scxml_state_spec[]){
            {
                .kind = TA_SCXML_STATE_COMPOUND,
                .id = S("main"),
                .initial = &(ta_scxml_initial_spec){
                    .targets = TARGETS(S("work")),
                    .actions = ACTIONS({ .kind = TA_SCXML_ACTION_LOG, .as.log.label = S("init") }),
                },
                .states = (const ta_scxml_state_spec[]){
                    {
                        .kind = TA_SCXML_STATE_HISTORY_SHALLOW,
                        .id = S("main_history"),
                        .transitions = (const ta_scxml_transition_spec[]){
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = TARGETS(S("idle")) },
                            {0},
                        },
                    },
                    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("idle") },
                    {
                        .kind = TA_SCXML_STATE_PARALLEL,
                        .id = S("work"),
                        .states = (const ta_scxml_state_spec[]){
                            {
                                .kind = TA_SCXML_STATE_COMPOUND,
                                .id = S("left"),
                                .states = (const ta_scxml_state_spec[]){
                                    {
                                        .kind = TA_SCXML_STATE_HISTORY_DEEP,
                                        .id = S("left_history"),
                                        .transitions = (const ta_scxml_transition_spec[]){
                                            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                              .targets = TARGETS(S("l1")),
                                              .actions = ACTIONS({ .kind = TA_SCXML_ACTION_RAISE,
                                                                   .as.raise.event = S("restored.left") }) },
                                            {0},
                                        },
                                    },
                                    {
                                        .kind = TA_SCXML_STATE_ATOMIC,
                                        .id = S("l1"),
                                        .transitions = (const ta_scxml_transition_spec[]){
                                            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                                              .event = S("  next\t.*  "),
                                              .targets = TARGETS(S("l2")) },
                                            {0},
                                        },
                                    },
                                    { .kind = TA_SCXML_STATE_FINAL, .id = S("l2") },
                                    {0},
                                },
                            },
                            {
                                .kind = TA_SCXML_STATE_COMPOUND,
                                .id = S("right"),
                                .states = (const ta_scxml_state_spec[]){
                                    { .kind = TA_SCXML_STATE_ATOMIC, .id = S("r1") },
                                    {0},
                                },
                            },
                            {0},
                        },
                        .transitions = (const ta_scxml_transition_spec[]){
                            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = S("pause.*"),
                              .condition = E("paused"), .targets = TARGETS(S("idle")) },
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S(".*"),
                              .targets = TARGETS(S("l1"), S("r1")) },
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("*") },
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("done.state.work error") },
                            {0},
                        },
                    },
                    {0},
                },
            },
            { .kind = TA_SCXML_STATE_FINAL, .id = S("end") },
            {0},
        },
    };
    check_roundtrip(trip, "structure", &spec);
    const ta_scxml_spec *out = trip->exported;
    if (out == NULL) {
        return;
    }
    /* The document has no initial: the root keeps the SCXML default. */
    CHECK(out->initial == NULL);
    CHECK(out->binding == TA_SCXML_BINDING_DEFAULT);
    CHECK(same_str(out->datamodel, S("null")));
    CHECK(same_str(out->name, S("structure")));
    const ta_scxml_state_spec *main_state = &out->states[0];
    CHECK(main_state->initial != NULL && same_str(main_state->initial->targets[0], S("work")) &&
          main_state->initial->actions[0].kind == TA_SCXML_ACTION_LOG);
    const ta_scxml_state_spec *work = &main_state->states[2];
    CHECK(work->kind == TA_SCXML_STATE_PARALLEL && work->initial == NULL);
    CHECK(work->transitions[0].kind == TA_SCXML_TRANSITION_INTERNAL);
    CHECK(same_str(work->transitions[1].event, S(".*")));
    CHECK(work->transitions[2].targets == NULL && work->transitions[2].actions == NULL);
    const ta_scxml_state_spec *left = &work->states[0];
    CHECK(left->initial == NULL);
    CHECK(left->states[0].kind == TA_SCXML_STATE_HISTORY_DEEP);
    CHECK(same_str(left->states[1].transitions[0].event, S("  next\t.*  ")));
    CHECK(same_str(left->states[0].transitions[0].actions[0].as.raise.event, S("restored.left")));
    CHECK(out->states[1].kind == TA_SCXML_STATE_FINAL && out->states[2].kind == TA_SCXML_STATE_NONE);
}

/* Executable content of every kind, handlers, data, and the <scxml> script. */
static void test_executable_content(roundtrip *trip) {
    const ta_scxml_action_spec *entry_actions = ACTIONS(
        { .kind = TA_SCXML_ACTION_ASSIGN, .as.assign = { .location = E("count"), .value = VEXPR("0") } },
        { .kind = TA_SCXML_ACTION_ASSIGN, .as.assign = { .location = E("copy"), .value = VLOC("count") } },
        { .kind = TA_SCXML_ACTION_ASSIGN, .as.assign = { .location = E("blob"), .value = VCONTENT("{\"a\":1}") } },
        { .kind = TA_SCXML_ACTION_SCRIPT, .as.script.script = E("count + 1") },
        { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = S("count"), .expr = E("count") } },
        { .kind = TA_SCXML_ACTION_LOG, .as.log.expr = E("copy") },
        { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = S("counted"), .data = VEXPR("count") } },
        {
            .kind = TA_SCXML_ACTION_IF,
            .as.conditional = {
                .condition = E("count > 1"),
                .then_actions = ACTIONS({ .kind = TA_SCXML_ACTION_RAISE, .as.raise.event = S("many") }),
                .else_actions = ACTIONS({
                    .kind = TA_SCXML_ACTION_IF,
                    .as.conditional = {
                        .condition = E("count == 1"),
                        .then_actions = ACTIONS({ .kind = TA_SCXML_ACTION_RAISE, .as.raise.event = S("one") }),
                        .else_actions = ACTIONS({ .kind = TA_SCXML_ACTION_RAISE, .as.raise.event = S("none") }),
                    },
                }),
            },
        },
        { .kind = TA_SCXML_ACTION_IF, .as.conditional.condition = E("empty") },
        {
            .kind = TA_SCXML_ACTION_FOREACH,
            .as.foreach = {
                .array = E("items"),
                .item = S("item"),
                .index = S("position"),
                .body = ACTIONS({ .kind = TA_SCXML_ACTION_LOG, .as.log.expr = E("item") }),
            },
        },
        { .kind = TA_SCXML_ACTION_FOREACH, .as.foreach = { .array = E("items"), .item = S("item") } });
    const ta_scxml_action_spec *sends = ACTIONS(
        {
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = {
                .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = S("timer") },
                .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("timeout.soft") },
                .delay = LIT("2.5s"),
                .params = (const ta_scxml_param_spec[]){
                    { .name = S("a"), .value = VEXPR("1") },
                    { .name = S("b"), .value = VLOC("count") },
                    { .name = {0} },
                },
            },
        },
        {
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = {
                .id = { .kind = TA_SCXML_ID_LOCATION, .as.location = E("last_id") },
                .type = LIT("http://www.w3.org/TR/scxml/#SCXMLEventProcessor"),
                .event = { .kind = TA_SCXML_EVENT_EXPR, .as.expr = E("event_name") },
                .target = LIT("#_parent"),
                .delay = EXPR("delay"),
                .data = VCONTENT("payload"),
            },
        },
        {
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = {
                .type = EXPR("io_type"),
                .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("empty") },
                .target = EXPR("target"),
                .delay = LIT("0500ms"),
                .data = VCONTENT(""),
            },
        },
        { .kind = TA_SCXML_ACTION_CANCEL, .as.cancel.sendid = LIT("timer") },
        { .kind = TA_SCXML_ACTION_CANCEL, .as.cancel.sendid = EXPR("last_id") });
    ta_scxml_spec spec = {
        .name = S("content"),
        .binding = TA_SCXML_BINDING_EARLY,
        .script = E("setup()"),
        .data = (const ta_scxml_data_spec[]){
            { .name = S("count"), .initializer = VEXPR("0") },
            { .name = S("items"), .initializer = VCONTENT("[1,2,3]") },
            { .name = S("unset") },
            { .name = {0} },
        },
        .initial = &(ta_scxml_initial_spec){ .targets = TARGETS(S("running")) },
        .states = (const ta_scxml_state_spec[]){
            {
                .kind = TA_SCXML_STATE_ATOMIC,
                .id = S("running"),
                .data = (const ta_scxml_data_spec[]){
                    { .name = S("local"), .initializer = VEXPR("{}") },
                    { .name = {0} },
                },
                .onentry = (const ta_scxml_handler_spec[]){
                    { .actions = entry_actions },
                    { .actions = NO_ACTIONS },
                    { .actions = sends },
                    {0},
                },
                .onexit = (const ta_scxml_handler_spec[]){
                    { .actions = ACTIONS({ .kind = TA_SCXML_ACTION_LOG, .as.log.label = S("bye") }) },
                    {0},
                },
                .transitions = (const ta_scxml_transition_spec[]){
                    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("timeout"), .targets = TARGETS(S("done")),
                      .actions = sends },
                    {0},
                },
            },
            { .kind = TA_SCXML_STATE_FINAL, .id = S("done") },
            {0},
        },
    };
    check_roundtrip(trip, "executable content", &spec);
    const ta_scxml_spec *out = trip->exported;
    if (out == NULL) {
        return;
    }
    CHECK(out->binding == TA_SCXML_BINDING_EARLY);
    CHECK(out->datamodel.ptr == NULL);
    CHECK(out->script.kind == TA_EXPRESSION_SOURCE && same_str(out->script.as.source, S("setup()")));
    CHECK(out->initial != NULL && same_str(out->initial->targets[0], S("running")) && out->initial->actions == NULL);
    CHECK(out->data[2].initializer.kind == TA_SCXML_VALUE_NONE && out->data[3].name.ptr == NULL);
    const ta_scxml_state_spec *running = &out->states[0];
    CHECK(running->onentry[1].actions != NULL && running->onentry[1].actions[0].kind == TA_SCXML_ACTION_NONE);
    CHECK(running->onentry[3].actions == NULL);
    const ta_scxml_action_spec *exported_sends = running->onentry[2].actions;
    CHECK(same_str(exported_sends[0].as.send.delay.as.literal, S("2.5s")));
    CHECK(same_str(exported_sends[0].as.send.event.as.name, S("timeout.soft")));
    CHECK(exported_sends[0].as.send.id.kind == TA_SCXML_ID_LITERAL);
    CHECK(same_str(exported_sends[0].as.send.params[1].name, S("b")) &&
          exported_sends[0].as.send.params[1].value.kind == TA_SCXML_VALUE_LOCATION);
    CHECK(exported_sends[1].as.send.id.kind == TA_SCXML_ID_LOCATION);
    CHECK(same_str(exported_sends[1].as.send.target.as.literal, S("#_parent")));
    CHECK(exported_sends[1].as.send.delay.kind == TA_SCXML_STRING_EXPR);
    CHECK(same_str(exported_sends[2].as.send.delay.as.literal, S("0500ms")));
    CHECK(exported_sends[2].as.send.data.kind == TA_SCXML_VALUE_CONTENT &&
          exported_sends[2].as.send.data.as.content.bytes.ptr != NULL &&
          exported_sends[2].as.send.data.as.content.bytes.len == 0);
    CHECK(exported_sends[2].as.send.id.kind == TA_SCXML_ID_NONE);
    const ta_scxml_action_spec *entry = running->onentry[0].actions;
    CHECK(entry[4].as.log.label.ptr != NULL && entry[5].as.log.label.ptr == NULL);
    CHECK(entry[7].as.conditional.else_actions[0].kind == TA_SCXML_ACTION_IF);
    CHECK(entry[8].as.conditional.then_actions == NULL && entry[8].as.conditional.else_actions == NULL);
    CHECK(same_str(entry[9].as.foreach.index, S("position")) && entry[10].as.foreach.index.ptr == NULL);
}

/* Invocations, finalize, and done data. */
static void test_invokes_and_done_data(roundtrip *trip) {
    ta_scxml_spec spec = {
        .name = S("invokes"),
        .binding = TA_SCXML_BINDING_LATE,
        .states = (const ta_scxml_state_spec[]){
            {
                .kind = TA_SCXML_STATE_COMPOUND,
                .id = S("parent"),
                .invokes = (const ta_scxml_invoke_spec[]){
                    {
                        .kind = TA_SCXML_INVOKE,
                        .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = S("child") },
                        .type = LIT("scxml"),
                        .src = LIT("file:child.scxml"),
                        .autoforward = true,
                        .params = (const ta_scxml_param_spec[]){ { .name = S("x"), .value = VEXPR("1") }, { .name = {0} } },
                        .finalize = ACTIONS({ .kind = TA_SCXML_ACTION_ASSIGN,
                                              .as.assign = { .location = E("result"), .value = VEXPR("_event.data") } }),
                    },
                    {
                        .kind = TA_SCXML_INVOKE,
                        .id = { .kind = TA_SCXML_ID_LOCATION, .as.location = E("child_id") },
                        .type = EXPR("child_type"),
                        .src = EXPR("child_src"),
                    },
                    { .kind = TA_SCXML_INVOKE, .data = VCONTENT("{\"states\":[]}") },
                    {0},
                },
                .states = (const ta_scxml_state_spec[]){
                    {
                        .kind = TA_SCXML_STATE_ATOMIC,
                        .id = S("waiting"),
                        .invokes = (const ta_scxml_invoke_spec[]){ { .kind = TA_SCXML_INVOKE, .type = LIT("http") }, {0} },
                        .transitions = (const ta_scxml_transition_spec[]){
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("done.invoke.child"),
                              .targets = TARGETS(S("with_content")) },
                            { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("error.*"),
                              .targets = TARGETS(S("with_params")) },
                            {0},
                        },
                    },
                    {
                        .kind = TA_SCXML_STATE_FINAL,
                        .id = S("with_content"),
                        .done_data = &(ta_scxml_done_data_spec){ .content = VEXPR("result") },
                    },
                    {
                        .kind = TA_SCXML_STATE_FINAL,
                        .id = S("with_params"),
                        .done_data = &(ta_scxml_done_data_spec){
                            .params = (const ta_scxml_param_spec[]){
                                { .name = S("code"), .value = VCONTENT("42") },
                                { .name = S("where"), .value = VLOC("location") },
                                { .name = {0} },
                            },
                        },
                    },
                    { .kind = TA_SCXML_STATE_FINAL, .id = S("empty_done"), .done_data = &(ta_scxml_done_data_spec){ .params = NULL } },
                    {0},
                },
            },
            {0},
        },
    };
    check_roundtrip(trip, "invokes and done data", &spec);
    const ta_scxml_spec *out = trip->exported;
    if (out == NULL) {
        return;
    }
    CHECK(out->binding == TA_SCXML_BINDING_LATE);
    const ta_scxml_state_spec *parent = &out->states[0];
    CHECK(parent->invokes[0].autoforward && same_str(parent->invokes[0].src.as.literal, S("file:child.scxml")));
    CHECK(parent->invokes[0].finalize != NULL && parent->invokes[0].finalize[0].kind == TA_SCXML_ACTION_ASSIGN);
    CHECK(parent->invokes[1].id.kind == TA_SCXML_ID_LOCATION && parent->invokes[1].src.kind == TA_SCXML_STRING_EXPR);
    CHECK(parent->invokes[2].data.kind == TA_SCXML_VALUE_CONTENT && parent->invokes[2].id.kind == TA_SCXML_ID_NONE);
    CHECK(parent->invokes[3].kind == TA_SCXML_INVOKE_NONE);
    CHECK(parent->states[0].invokes != NULL && parent->states[1].invokes == NULL);
    CHECK(parent->states[1].done_data != NULL && parent->states[1].done_data->content.kind == TA_SCXML_VALUE_EXPR);
    CHECK(parent->states[2].done_data != NULL && same_str(parent->states[2].done_data->params[1].name, S("where")));
    CHECK(parent->states[3].done_data != NULL && parent->states[3].done_data->params == NULL);
}

/* An explicit identity is exported as-is and survives recompilation. */
static void test_explicit_identity(roundtrip *trip) {
    const ta_definition_identity identity = {
        .family = { .name = S("orders") },
        .revision = { .ptr = (const uint8_t *)"v7", .len = 2 },
    };
    const ta_scxml_state_spec *states = (const ta_scxml_state_spec[]){ { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s") }, {0} };
    ta_scxml_spec spec = { .identity = &identity, .states = states };
    check_roundtrip(trip, "explicit identity", &spec);
    if (trip->exported == NULL) {
        return;
    }
    CHECK(trip->exported->identity != NULL && same_identity(trip->exported->identity, &identity));
    CHECK(trip->exported->name.ptr == NULL);
}

/* The retained text matters: equivalent authored forms have different revisions. */
static void test_normalizations_change_revisions(ta_system *system) {
    const ta_scxml_state_spec *two_seconds = (const ta_scxml_state_spec[]){
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s"),
          .onentry = (const ta_scxml_handler_spec[]){
              { .actions = ACTIONS({ .kind = TA_SCXML_ACTION_SEND,
                                     .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("e") },
                                                  .delay = LIT("2s") } }) },
              {0},
          } },
        {0},
    };
    const ta_scxml_state_spec *two_thousand = (const ta_scxml_state_spec[]){
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s"),
          .onentry = (const ta_scxml_handler_spec[]){
              { .actions = ACTIONS({ .kind = TA_SCXML_ACTION_SEND,
                                     .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = S("e") },
                                                  .delay = LIT("2000ms") } }) },
              {0},
          } },
        {0},
    };
    uint8_t left[16];
    uint8_t right[16];
    ta_bytes a = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = two_seconds }, left);
    ta_bytes b = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = two_thousand }, right);
    CHECK(a.len == 16 && b.len == 16 && memcmp(a.ptr, b.ptr, 16) != 0);

    const ta_scxml_state_spec *spaced = (const ta_scxml_state_spec[]){
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s"),
          .transitions = (const ta_scxml_transition_spec[]){ { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("a  b") }, {0} } },
        {0},
    };
    const ta_scxml_state_spec *single = (const ta_scxml_state_spec[]){
        { .kind = TA_SCXML_STATE_ATOMIC, .id = S("s"),
          .transitions = (const ta_scxml_transition_spec[]){ { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = S("a b") }, {0} } },
        {0},
    };
    a = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = spaced }, left);
    b = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = single }, right);
    CHECK(a.len == 16 && b.len == 16 && memcmp(a.ptr, b.ptr, 16) != 0);

    a = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = single }, left);
    b = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = single, .binding = TA_SCXML_BINDING_EARLY }, right);
    CHECK(a.len == 16 && b.len == 16 && memcmp(a.ptr, b.ptr, 16) != 0);

    /* The default initial state and the same state named explicitly. */
    a = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = single }, left);
    b = revision_of(system, &(ta_scxml_spec){ .name = S("n"), .states = single,
                                              .initial = &(ta_scxml_initial_spec){ .targets = TARGETS(S("s")) } },
                    right);
    CHECK(a.len == 16 && b.len == 16 && memcmp(a.ptr, b.ptr, 16) != 0);
}

static void test_errors(roundtrip *trip) {
    const ta_scxml_spec *out = NULL;
    CHECK(ta_definition_export(trip->system, &(ta_definition_export_params){
                                                 .definition = 9999, .arena = trip->arena, .out = &out })
          == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_export(trip->system, &(ta_definition_export_params){ .definition = 0, .out = &out })
          == TA_ERR_INVALID);
    CHECK(ta_definition_export(trip->system, &(ta_definition_export_params){ .definition = 0, .arena = trip->arena })
          == TA_ERR_INVALID);
    CHECK(ta_definition_export(NULL, &(ta_definition_export_params){ .arena = trip->arena, .out = &out })
          == TA_ERR_INVALID);
    CHECK(out == NULL);
}

int main(void) {
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = test_allocator }, &system) != TA_OK) {
        fprintf(stderr, "cannot create a system\n");
        return 1;
    }
    ta_arena arena;
    /* A small block size makes export span many arena blocks. */
    if (ta_arena_init(&arena, test_allocator, 256) != TA_OK) {
        fprintf(stderr, "cannot create an arena\n");
        return 1;
    }
    roundtrip trip = { .system = system, .arena = &arena };
    test_structure(&trip);
    test_executable_content(&trip);
    test_invokes_and_done_data(&trip);
    test_explicit_identity(&trip);
    test_normalizations_change_revisions(system);
    test_errors(&trip);
    ta_arena_destroy(&arena);
    ta_system_destroy(system);
    if (failures != 0) {
        fprintf(stderr, "export test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("tinyactors-core export test passed");
    return 0;
}
