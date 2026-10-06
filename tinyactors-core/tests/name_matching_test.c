/*
 * Matching event names by lookup: a name's bytes, split at the dots and
 * looked up segment by segment in a system's atom table with the name
 * resolver (src/names.h), give the event matcher (ta_event_pattern_matches,
 * src/definition.h) the same answers as the name interned into the
 * event-name trie, for the descriptors of a registered definition; and
 * names nobody authored add nothing to the atom table or the trie.
 *
 * The test includes the amalgamation so it reaches the private units; it
 * uses the C library, the core does not.
 *
 * Claims proven here, in order:
 *
 * 1. system creation makes every segment of the names core generates an
 *    atom: error, execution, communication, platform, done, state and
 *    invoke, the last as the system's `invoke_segment`;
 * 2. the worked examples: a definition with the descriptors `order`,
 *    `order.*.paid`, `error.communication.invoke.payment` and `*`, and
 *    `<invoke id="payment">`, registers the atoms order, paid, error,
 *    communication, invoke and payment and no segment of the examples'
 *    other names; each example's bytes resolve to those atoms and UNKNOWN,
 *    and its descriptors match as the examples say;
 * 3. lookup gives the trie's answers: for every name of a corpus (prefix
 *    and segment-boundary cases, wildcards, generated names, names with
 *    segments no descriptor mentions), matching the resolved bytes against
 *    every descriptor of a registered definition answers as matching the
 *    same name interned into the trie does, looked up before it is
 *    interned and again after;
 * 4. random names, resolved and matched against every descriptor, leave
 *    the atom table and the trie unchanged and allocate nothing;
 * 5. a `<send typeexpr>` and an `<invoke typeexpr>` whose text names no
 *    processor or invoker fail as error.execution, as before, and leave the
 *    atom table and the trie unchanged, for random text and for text that
 *    is an atom but no type;
 * 6. error.communication.invoke.<id>.<reason> takes one segment per dotted
 *    part of the invocation ID, as done.invoke.<id> does: for an authored
 *    `pay.ment` and a generated `s3.<platformid>`, the name's segments are
 *    the parts, it is the node the full dotted text names, it matches
 *    `error.communication.invoke.*` and a descriptor naming the ID's first
 *    part (`error.communication.invoke.s3`, `.pay`) and not one that splits
 *    a part (`.paym`), and a host's ERROR report takes the transition such
 *    a descriptor selects;
 * 7. a name deeper than `eventNameDepth` is not made: an ERROR report whose
 *    error name would be too deep answers TA_ERR_INVALID and changes
 *    nothing the session sees, as a DONE report whose done.invoke name
 *    would be too deep does; a shallower name for the same session is
 *    still made;
 * 8. 10,000 random names sent with an immediate `<send eventexpr>` to a
 *    session travel as text: each is taken and matched by `*` (resolved
 *    from the envelope's bytes), and the atom table and the trie keep
 *    their size.
 *
 * Paths that still add a run-time name to the tables, and that claims 4,
 * 5 and 8 therefore do not cover, because an event there carries its name
 * as a trie node: the value of a `<send eventexpr>` that is stored (a
 * delayed send, one held for an unpublished session, one for #_internal or
 * for an I/O processor), or handed to a host (a TAKEN outcome's event,
 * inspection, a session image, an event forwarded to a host invoker, a
 * kept event); the generated invocation ID and the `done.invoke.<id>` and
 * `error.communication.invoke.<id>.<reason>` names built from it (the ID
 * and the reason, not the `invoke` segment); `#_<invokeid>` targets and
 * the family of a session URI in a `targetexpr`; and the public calls a
 * host uses to turn text into IDs (ta_system_intern_atom,
 * ta_event_name_intern, ta_send_id_intern, ta_system_intern_invoke_id).
 */
#include "../include/tinyactors.c"
#include "test_allocator.h"
#include "test_drive.h"

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

#define STR(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })
#define EXPR(text) ((ta_expression){ .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) })

static ta_str text(const char *cstr) {
    return (ta_str){ .ptr = cstr, .len = (uint32_t)strlen(cstr) };
}

/* ------------------------------------------------------------------------
 * A data model: every expression's value is the text `typed` holds, every
 * condition is false, and every assignment counts in `assigned`.
 */

static char typed[64];
static uint32_t typed_len = 0;
static uint32_t assigned = 0;

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
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
    *params->out = false;
    return TA_OP_OK;
}
static ta_op_status dm_eval_value(void *self, const ta_datamodel_eval_value_params *params) {
    (void)self;
    *params->out = (ta_value){ .self = typed };
    return TA_OP_OK;
}
static ta_op_status dm_value_text(void *self, const ta_datamodel_value_text_params *params) {
    (void)self;
    *params->out = (ta_str){ .ptr = typed, .len = typed_len };
    return TA_OP_OK;
}
static ta_op_status dm_assign(void *self, const ta_datamodel_assign_params *params) {
    (void)self, (void)params;
    assigned += 1u;
    return TA_OP_OK;
}
static ta_op_status dm_set_named(void *self, const ta_datamodel_set_named_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("typed"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
    .eval_condition = dm_eval_condition,
    .eval_value = dm_eval_value,
    .value_text = dm_value_text,
    .assign = dm_assign,
    .set_named = dm_set_named,
};

/* ------------------------------------------------------------------------
 * Fixture: a system, one registered definition, and a resolver over the
 * system's atom table with a scratch of `eventNameDepth` atoms.
 */

typedef struct {
    test_allocator heap;
    ta_system *system;
    ta_definition_id id;
    const ta_definition *definition;
    ta_atom *scratch;
    ta_names names;
} fixture;

static void fixture_open(fixture *f, const ta_scxml_spec *spec) {
    memset(f, 0, sizeof *f);
    ta_allocator allocator = test_allocator_of(&f->heap);
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &f->system) == TA_OK);
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(f->system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = { .v = &dm_vtable },
                                                    .allocator = allocator,
                                                    .out = &compiled,
                                                }) == TA_OK);
    CHECK(ta_system_register_definition(f->system, compiled, &f->id) == TA_OK);
    f->definition = ta_definition_lookup(f->system, f->id);
    CHECK(f->definition != NULL);
    uint64_t depth = 0;
    CHECK(ta_system_param(f->system, TA_PARAM_EVENT_NAME_DEPTH, &depth) == TA_OK && depth > 0);
    f->scratch = malloc((size_t)depth * sizeof(ta_atom));
    ta_names_init(&f->names, &(ta_names_init_params){ .atoms = &f->system->atoms, .scratch = f->scratch,
                                                      .capacity = (uint32_t)depth });
}

static void fixture_close(fixture *f) {
    free(f->scratch);
    ta_system_destroy(f->system);
    CHECK(f->heap.live == 0);
    CHECK(test_allocator_finish(&f->heap) == 0);
}

static uint32_t atom_count(const fixture *f) {
    return f->system->atoms.entries.count;
}

static uint32_t node_count(const fixture *f) {
    return f->system->event_names.nodes.count;
}

static bool is_atom(const fixture *f, const char *cstr, ta_atom *out) {
    ta_atom atom = TA_ATOM_NONE;
    bool found = ta_atom_find(&f->system->atoms, text(cstr), &atom) == TA_OK;
    if (out != NULL) {
        *out = atom;
    }
    return found;
}

/* The first descriptor of transition `index`, in document order. */
static const ta_event_pattern *descriptor_of(const fixture *f, uint32_t index) {
    const ta_transition_def *transition = &f->definition->transitions[index];
    CHECK(transition->event_patterns.length == 1);
    return &f->definition->event_patterns[transition->event_patterns.offset];
}

/* Whether descriptor `index` matches the segments the resolver holds. */
static bool resolved_matches(const fixture *f, uint32_t index) {
    ta_atom_list event = { .atoms = f->names.scratch, .count = f->names.count };
    return ta_event_pattern_matches(f->definition, descriptor_of(f, index), &event);
}

/* Whether descriptor `index` matches the trie node `name`, by its path. */
static bool interned_matches(const fixture *f, uint32_t index, ta_event_name_id name) {
    ta_atom segments[TA_TRIE_MAX_DEPTH];
    ta_atom_list event = { .atoms = segments,
                           .count = ta_trie_path(&f->system->event_names, name, segments) };
    return ta_event_pattern_matches(f->definition, descriptor_of(f, index), &event);
}

/* ------------------------------------------------------------------------
 * The charts
 */

/* One atomic state with one transition per descriptor, in this order, and
 * an <invoke id="payment">. */
static const ta_scxml_spec *worked_chart(void) {
    static const ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("order") },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("order.*.paid") },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("error.communication.invoke.payment") },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("*") },
        {0},
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("payment") } },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("shop"), .transitions = transitions, .invokes = invokes },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("worked"), .states = states };
    return &spec;
}

/* The descriptors claim 3 matches against, one transition each. */
static const char *const corpus_descriptors[] = {
    "error", "error.execution", "error.communication", "error.communication.invoke",
    "done", "done.state", "done.state.work", "done.state.other", "done.invoke", "done.invoke.svc",
    "err", "error.exec", "order", "order.paid", "order.*.paid", "*", "*.paid", "a.*.c", "a.*.c.*.e",
    "*.error", "a.*", "a*", "*b", "x.y.z",
};

#define CORPUS_DESCRIPTORS (sizeof corpus_descriptors / sizeof corpus_descriptors[0])

static const ta_scxml_spec *corpus_chart(void) {
    static ta_scxml_transition_spec transitions[CORPUS_DESCRIPTORS + 1u];
    for (uint32_t index = 0; index < CORPUS_DESCRIPTORS; index += 1u) {
        transitions[index] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_INTERNAL,
                                                         .event = text(corpus_descriptors[index]) };
    }
    transitions[CORPUS_DESCRIPTORS] = (ta_scxml_transition_spec){0};
    static ta_scxml_state_spec states[2];
    states[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("work"), .transitions = transitions };
    states[1] = (ta_scxml_state_spec){0};
    static ta_scxml_spec spec;
    spec = (ta_scxml_spec){ .name = STR("corpus"), .states = states };
    return &spec;
}

/* `idle` sends with a typeexpr on `send` and enters `invoking`, whose
 * invocation has a typeexpr, on `invoke`; each counts the error.execution
 * it gets with an <assign> and goes back to `idle`. The invocation has an
 * authored ID: a generated one is still interned. */
static const ta_scxml_spec *typeexpr_chart(void) {
    static const ta_scxml_action_spec send[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .type = { .kind = TA_SCXML_STRING_EXPR, .as.expr = EXPR("type") },
                       .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("sent") } } },
        {0},
    };
    static const ta_scxml_action_spec count[] = {
        { .kind = TA_SCXML_ACTION_ASSIGN,
          .as.assign = { .location = EXPR("errors"),
                         .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("errors + 1") } } },
        {0},
    };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_str to_invoking[] = { STR("invoking"), {0} };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("send"), .actions = send },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("invoke"), .targets = to_invoking },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("error.execution"), .actions = count },
        {0},
    };
    static const ta_scxml_transition_spec invoking[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error.execution"), .targets = to_idle,
          .actions = count },
        {0},
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("svc") },
          .type = { .kind = TA_SCXML_STRING_EXPR, .as.expr = EXPR("type") } },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("invoking"), .transitions = invoking, .invokes = invokes },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("typeexpr"), .states = states };
    return &spec;
}

/* ------------------------------------------------------------------------
 * Random names
 */

static uint64_t random_state = 0x9e3779b97f4a7c15u;

static uint32_t random_below(uint32_t bound) {
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return (uint32_t)(random_state % bound);
}

/* Writes a random dotted name of 1 to `depth` segments into `buffer`: each
 * segment random bytes without a dot, or now and then a word a chart
 * registered, so names mix atoms and UNKNOWN. */
static ta_str random_name(char *buffer, uint32_t depth) {
    static const char *const words[] = { "order", "paid", "error", "communication", "invoke", "payment", "done" };
    uint32_t segments = 1u + random_below(depth);
    uint32_t len = 0;
    for (uint32_t segment = 0; segment < segments; segment += 1u) {
        if (segment > 0) {
            buffer[len++] = '.';
        }
        if (random_below(4) == 0) {
            const char *word = words[random_below(sizeof words / sizeof words[0])];
            memcpy(buffer + len, word, strlen(word));
            len += (uint32_t)strlen(word);
            continue;
        }
        uint32_t bytes = 1u + random_below(12);
        for (uint32_t index = 0; index < bytes; index += 1u) {
            char c = 0;
            do {
                c = (char)(1 + random_below(255));
            } while (c == '.');
            buffer[len++] = c;
        }
    }
    return (ta_str){ .ptr = buffer, .len = len };
}

/* ------------------------------------------------------------------------
 * Tests
 */

/* Claim 1. */
static void test_generated_prefixes(void) {
    fixture f;
    fixture_open(&f, worked_chart());
    static const char *const segments[] = { "error", "execution", "communication", "platform", "done", "state",
                                            "invoke" };
    for (uint32_t index = 0; index < sizeof segments / sizeof segments[0]; index += 1u) {
        CHECK(is_atom(&f, segments[index], NULL));
    }
    ta_atom invoke = TA_ATOM_NONE;
    CHECK(is_atom(&f, "invoke", &invoke) && invoke == f.system->invoke_segment);
    fixture_close(&f);

    /* Without any definition, too. */
    test_allocator heap = {0};
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = test_allocator_of(&heap) }, &system) == TA_OK);
    for (uint32_t index = 0; index < sizeof segments / sizeof segments[0]; index += 1u) {
        ta_atom atom = TA_ATOM_NONE;
        CHECK(ta_atom_find(&system->atoms, text(segments[index]), &atom) == TA_OK);
    }
    CHECK(ta_atom_text(&system->atoms, system->invoke_segment).len == 6u);
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
}

/* Claim 2. */
static void test_worked_examples(void) {
    fixture f;
    fixture_open(&f, worked_chart());
    ta_atom order = 0, paid = 0, error = 0, communication = 0, invoke = 0, payment = 0;
    CHECK(is_atom(&f, "order", &order) && is_atom(&f, "paid", &paid) && is_atom(&f, "error", &error));
    CHECK(is_atom(&f, "communication", &communication) && is_atom(&f, "invoke", &invoke));
    CHECK(is_atom(&f, "payment", &payment));
    static const char *const unknown[] = { "123", "124", "orders", "1", "timeout", "s3", "7", "unavailable",
                                           "x9f2", "qq", "zz" };
    for (uint32_t index = 0; index < sizeof unknown / sizeof unknown[0]; index += 1u) {
        CHECK(!is_atom(&f, unknown[index], NULL));
    }
    const ta_atom U = TA_NAMES_UNKNOWN;
    static const struct {
        const char *event;
        uint32_t count;
        bool order, order_any_paid, invoke_payment, any;
    } rows[] = {
        { "order.123.paid", 3, true, true, false, true },
        { "order.124", 2, true, false, false, true },
        { "orders.1", 2, false, false, false, true },
        { "error.communication.invoke.payment.timeout", 5, false, false, true, true },
        { "error.communication.invoke.s3.7.unavailable", 6, false, false, false, true },
        { "x9f2.qq.zz", 3, false, false, false, true },
    };
    const ta_atom resolved[][6] = {
        { order, U, paid },
        { order, U },
        { U, U },
        { error, communication, invoke, payment, U },
        { error, communication, invoke, U, U, U },
        { U, U, U },
    };
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    for (uint32_t row = 0; row < sizeof rows / sizeof rows[0]; row += 1u) {
        CHECK(ta_names_resolve(&f.names, text(rows[row].event)) == TA_OK);
        CHECK(f.names.count == rows[row].count);
        CHECK(memcmp(f.scratch, resolved[row], rows[row].count * sizeof(ta_atom)) == 0);
        bool ok = resolved_matches(&f, 0) == rows[row].order && resolved_matches(&f, 1) == rows[row].order_any_paid &&
                  resolved_matches(&f, 2) == rows[row].invoke_payment && resolved_matches(&f, 3) == rows[row].any;
        if (!ok) {
            fprintf(stderr, "worked example %s matched wrongly\n", rows[row].event);
        }
        CHECK(ok);
    }
    CHECK(atom_count(&f) == atoms && node_count(&f) == nodes);
    fixture_close(&f);
}

/* Claim 3. */
static void test_same_answers_as_the_trie(void) {
    fixture f;
    fixture_open(&f, corpus_chart());
    static const char *const corpus[] = {
        "error", "error.execution", "error.communication.invoke.svc.unavailable", "done.state.work",
        "done.invoke.svc", "done.state", "done", "errors.execution", "error.exec", "err",
        "done.state.other", "done.state.worker", "order", "order.paid", "order.123.paid", "order.124",
        "orders.1", "a", "a.b", "a.c", "a.b.c", "a.b.x.c", "a.b.c.d", "a.x.c.y.z.e", "a.x.c.y.z",
        "x.error", "x.y.error.detail", "x.errors", "error.x", "b", "ab", "a*", "*b", "x.y.z.w", "x.y",
        "error.communication.invoke.payment.timeout", "x9f2.qq.zz", "paid", "q.paid", "q.r.paid.s",
    };
    for (uint32_t row = 0; row < sizeof corpus / sizeof corpus[0]; row += 1u) {
        /* Looked up before the name is interned: unknown segments are UNKNOWN. */
        CHECK(ta_names_resolve(&f.names, text(corpus[row])) == TA_OK);
        bool looked_up[CORPUS_DESCRIPTORS];
        for (uint32_t index = 0; index < CORPUS_DESCRIPTORS; index += 1u) {
            looked_up[index] = resolved_matches(&f, index);
        }
        ta_event_name_id name = 0;
        CHECK(ta_trie_intern_dotted(&f.system->event_names, text(corpus[row]), &name) == TA_OK);
        /* And again once every segment is an atom. */
        CHECK(ta_names_resolve(&f.names, text(corpus[row])) == TA_OK);
        for (uint32_t index = 0; index < CORPUS_DESCRIPTORS; index += 1u) {
            bool interned = interned_matches(&f, index, name);
            bool same = looked_up[index] == interned && resolved_matches(&f, index) == interned;
            if (!same) {
                fprintf(stderr, "%s against %s: the lookup and the trie disagree\n", corpus[row],
                        corpus_descriptors[index]);
            }
            CHECK(same);
        }
    }
    fixture_close(&f);
}

/* Claim 4. */
static void test_random_names_leave_no_trace(void) {
    fixture f;
    fixture_open(&f, worked_chart());
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    uint32_t attempts = f.heap.attempts;
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    uint32_t matched = 0;
    for (uint32_t round = 0; round < 2000u; round += 1u) {
        ta_str name = random_name(buffer, f.names.capacity);
        CHECK(ta_names_resolve(&f.names, name) == TA_OK);
        for (uint32_t index = 0; index < 4u; index += 1u) {
            matched += resolved_matches(&f, index) ? 1u : 0u;
        }
    }
    /* `*` took every one of them. */
    CHECK(matched >= 2000u);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    CHECK(f.heap.attempts == attempts);
    fixture_close(&f);
}

static void deliver(fixture *f, ta_actor_id actor, ta_event_name_id name) {
    ta_event event = { .name = name, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_deliver(f->system,
                             &(ta_actor_deliver_params){ .time = ta_system_time(f->system), .actor = actor,
                                                         .event = &event },
                             &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
}

static ta_event_name_id event_name(fixture *f, const char *cstr) {
    ta_event_name_id name = 0;
    CHECK(ta_trie_intern_dotted(&f->system->event_names, text(cstr), &name) == TA_OK);
    return name;
}

static void set_type(const char *bytes, uint32_t len) {
    memcpy(typed, bytes, len);
    typed_len = len;
}

/* Claim 5. */
static void test_typeexpr_looks_up(void) {
    fixture f;
    fixture_open(&f, typeexpr_chart());
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_event_name_id send = event_name(&f, "send");
    ta_event_name_id invoke = event_name(&f, "invoke");
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    assigned = 0;
    uint32_t rounds = 0;
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    for (uint32_t round = 0; round < 200u; round += 1u) {
        ta_str type = random_name(buffer, 4);
        if (type.len > sizeof typed) {
            type.len = sizeof typed;
        }
        set_type(type.ptr, type.len);
        deliver(&f, actor, send);
        deliver(&f, actor, invoke);
        rounds += 1u;
    }
    /* Text that is an atom (a state ID), but names no processor or
     * invoker, and text that is none. */
    static const char *const fixed[] = { "idle", "order" };
    CHECK(is_atom(&f, "idle", NULL) && !is_atom(&f, "order", NULL));
    for (uint32_t index = 0; index < sizeof fixed / sizeof fixed[0]; index += 1u) {
        set_type(fixed[index], (uint32_t)strlen(fixed[index]));
        deliver(&f, actor, send);
        deliver(&f, actor, invoke);
        rounds += 1u;
    }
    /* Each send and each invocation failed as error.execution. */
    CHECK(assigned == 2u * rounds);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    fixture_close(&f);
}

/* `idle` sends to itself with an eventexpr on `send`, and counts, with an
 * <assign>, every other event its `*` transition takes. */
static const ta_scxml_spec *eventexpr_chart(void) {
    static const ta_scxml_action_spec send[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_EXPR, .as.expr = EXPR("name") } } },
        {0},
    };
    static const ta_scxml_action_spec count[] = {
        { .kind = TA_SCXML_ACTION_ASSIGN,
          .as.assign = { .location = EXPR("taken"),
                         .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = EXPR("taken + 1") } } },
        {0},
    };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("send"), .actions = send },
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("*"), .actions = count },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("eventexpr"), .states = states };
    return &spec;
}

/* Claim 8. */
static void test_eventexpr_travels_as_text(void) {
    fixture f;
    fixture_open(&f, eventexpr_chart());
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_event_name_id send = event_name(&f, "send");
    /* A first round sizes the name buffer and the reserves. */
    set_type("warm", 4);
    deliver(&f, actor, send);
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    assigned = 0;
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    for (uint32_t round = 0; round < 10000u; round += 1u) {
        ta_str name = random_name(buffer, f.names.capacity);
        if (name.len > sizeof typed) {
            name.len = sizeof typed;
            while (name.len > 0 && buffer[name.len - 1u] == '.') {
                name.len -= 1u;
            }
        }
        if (name.len == 4 && memcmp(name.ptr, "send", 4) == 0) {
            continue;
        }
        set_type(name.ptr, name.len);
        deliver(&f, actor, send);
        CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    }
    CHECK(assigned >= 9990u);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    fixture_close(&f);
}

/* ------------------------------------------------------------------------
 * Invocation names
 */

/* An invoker whose services start and stop at once and do nothing else. */
static ta_op_status svc_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    *params->service_handle = 1;
    return TA_OP_OK;
}
static ta_op_status svc_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self, (void)service, (void)fault;
    return TA_OP_OK;
}
static ta_op_status svc_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static const ta_invoker_vtable svc_vtable = { .start = svc_start, .cancel = svc_cancel, .forward = svc_forward };

/* The descriptors of `s3`'s transitions, in document order: the first two
 * leave for `by_part` and `by_state`, the others stay. */
static const char *const invoke_descriptors[] = {
    "error.communication.invoke.pay",
    "error.communication.invoke.s3",
    "error.communication.invoke.pay.ment.failed",
    "error.communication.invoke.*",
    "error.communication.invoke.pay.ment",
    "error.communication.invoke.paym",
    "done.invoke.s3",
};

#define INVOKE_DESCRIPTORS (sizeof invoke_descriptors / sizeof invoke_descriptors[0])

/* `s3` invokes `pay.ment` and an invocation with a generated ID, both of
 * type `svc`. */
static const ta_scxml_spec *invoke_chart(void) {
    static const ta_str to_by_part[] = { STR("by_part"), {0} };
    static const ta_str to_by_state[] = { STR("by_state"), {0} };
    static ta_scxml_transition_spec transitions[INVOKE_DESCRIPTORS + 1u];
    for (uint32_t index = 0; index < INVOKE_DESCRIPTORS; index += 1u) {
        transitions[index] = (ta_scxml_transition_spec){ .kind = TA_SCXML_TRANSITION_INTERNAL,
                                                         .event = text(invoke_descriptors[index]) };
    }
    transitions[0].kind = TA_SCXML_TRANSITION_EXTERNAL;
    transitions[0].targets = to_by_part;
    transitions[1].kind = TA_SCXML_TRANSITION_EXTERNAL;
    transitions[1].targets = to_by_state;
    transitions[INVOKE_DESCRIPTORS] = (ta_scxml_transition_spec){0};
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("pay.ment") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
        {0},
    };
    static ta_scxml_state_spec states[4];
    states[0] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3"), .transitions = transitions,
                                       .invokes = invokes };
    states[1] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("by_part") };
    states[2] = (ta_scxml_state_spec){ .kind = TA_SCXML_STATE_ATOMIC, .id = STR("by_state") };
    states[3] = (ta_scxml_state_spec){0};
    static ta_scxml_spec spec;
    spec = (ta_scxml_spec){ .name = STR("invoking"), .states = states };
    return &spec;
}

/* A system with `depth` as its eventNameDepth (0: the default), `spec`
 * registered, and the invoker `svc`. */
static void invoke_fixture_open(fixture *f, const ta_scxml_spec *spec, uint32_t depth) {
    memset(f, 0, sizeof *f);
    ta_allocator allocator = test_allocator_of(&f->heap);
    ta_param_value params[] = { { TA_PARAM_EVENT_NAME_DEPTH, depth } };
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params,
                                                .param_count = depth == 0 ? 0u : 1u },
                           &f->system) == TA_OK);
    ta_atom svc = 0;
    CHECK(ta_system_intern_atom(f->system, STR("svc"), &svc) == TA_OK);
    CHECK(ta_system_register_invoker(f->system, svc, (ta_invoker){ .v = &svc_vtable }) == TA_OK);
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(f->system, &(ta_definition_compile_params){
                                                    .spec = spec,
                                                    .datamodel = { .v = &dm_vtable },
                                                    .allocator = allocator,
                                                    .out = &compiled,
                                                }) == TA_OK);
    CHECK(ta_system_register_definition(f->system, compiled, &f->id) == TA_OK);
    f->definition = ta_definition_lookup(f->system, f->id);
    CHECK(f->definition != NULL);
}

static void run_stable(fixture *f, ta_actor_id actor) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f->system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
}

/* Spawns a session and runs it into its first state, which invokes. */
static ta_actor_id spawn_invoking(fixture *f) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = f->id, .out = &actor }) == TA_OK);
    run_stable(f, actor);
    return actor;
}

/* The ID of `actor`'s active invocation whose text is `cstr`, or whose text
 * starts with it when `prefix`; 0 when there is none. */
static ta_invoke_id invoke_named(fixture *f, ta_actor_id actor, const char *cstr, bool prefix) {
    const ta_actor *record = ta_actor_lookup(f->system, actor);
    const ta_actor_sparse_state *sparse = record == NULL ? NULL : record->sparse;
    size_t len = strlen(cstr);
    for (uint32_t index = 0; sparse != NULL && index < sparse->invoke_count; index += 1u) {
        ta_str id = ta_invoke_id_text(f->system, sparse->invokes[index].id);
        if ((prefix ? id.len > len : id.len == len) && memcmp(id.ptr, cstr, len) == 0) {
            return sparse->invokes[index].id;
        }
    }
    return 0;
}

static bool session_in(fixture *f, ta_actor_id actor, const char *state) {
    ta_state_id active[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(f->system, &(ta_actor_active_states_params){ .actor = actor, .states = active,
                                                                             .capacity = 4, .count = &count }) ==
          TA_OK);
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_state_info info = {0};
        CHECK(ta_definition_state(f->system, &(ta_definition_state_params){ .definition = f->id,
                                                                            .state = active[index],
                                                                            .out = &info }) == TA_OK);
        if (info.id.len == strlen(state) && memcmp(info.id.ptr, state, info.id.len) == 0) {
            return true;
        }
    }
    return false;
}

static ta_result report_error(fixture *f, ta_actor_id actor, ta_invoke_id invoke) {
    return ta_actor_report_invoke(f->system, &(ta_invoke_report_params){
                                                 .actor = actor,
                                                 .invoke_id = invoke,
                                                 .kind = TA_INVOKE_REPORT_ERROR,
                                                 .time = ta_system_time(f->system),
                                                 .as.error = &(ta_invoke_error){ .reason = STR("failed") },
                                             });
}

/* Whether the segments of `name` are, in order, the `count` texts. */
static bool segments_are(fixture *f, ta_event_name_id name, const char *const *texts, uint32_t count) {
    ta_atom segments[TA_TRIE_MAX_DEPTH];
    if (ta_trie_path(&f->system->event_names, name, segments) != count) {
        return false;
    }
    for (uint32_t index = 0; index < count; index += 1u) {
        ta_str segment = ta_atom_text(&f->system->atoms, segments[index]);
        if (segment.len != strlen(texts[index]) || memcmp(segment.ptr, texts[index], segment.len) != 0) {
            return false;
        }
    }
    return true;
}

/* Claim 6. */
static void test_error_name_splits_the_id(void) {
    fixture f;
    invoke_fixture_open(&f, invoke_chart(), 0);
    ta_actor_id actor = spawn_invoking(&f);
    ta_invoke_id authored = invoke_named(&f, actor, "pay.ment", false);
    ta_invoke_id generated = invoke_named(&f, actor, "s3.", true);
    CHECK(authored != 0 && generated != 0);
    ta_str generated_text = ta_invoke_id_text(f.system, generated);
    char platform[32] = {0};
    CHECK(generated_text.len > 3u && generated_text.len - 3u < sizeof platform);
    memcpy(platform, generated_text.ptr + 3, generated_text.len - 3u);
    CHECK(strchr(platform, '.') == NULL);

    ta_event_name_id authored_name = 0;
    ta_event_name_id generated_name = 0;
    CHECK(ta_invoke_error_name(f.system, authored, STR("failed"), &authored_name) == TA_OK);
    CHECK(ta_invoke_error_name(f.system, generated, STR("failed"), &generated_name) == TA_OK);

    /* One segment per dotted part. */
    static const char *const authored_segments[] = { "error", "communication", "invoke", "pay", "ment", "failed" };
    const char *const generated_segments[] = { "error", "communication", "invoke", "s3", platform, "failed" };
    CHECK(segments_are(&f, authored_name, authored_segments, 6));
    CHECK(segments_are(&f, generated_name, generated_segments, 6));

    /* The node the full dotted text names: the full name matches itself. */
    char full[96];
    ta_event_name_id dotted = 0;
    CHECK(ta_trie_intern_dotted(&f.system->event_names, text("error.communication.invoke.pay.ment.failed"),
                                &dotted) == TA_OK && dotted == authored_name);
    snprintf(full, sizeof full, "error.communication.invoke.s3.%s.failed", platform);
    CHECK(ta_trie_intern_dotted(&f.system->event_names, text(full), &dotted) == TA_OK && dotted == generated_name);

    /* The descriptors match by prefix of whole segments. */
    static const bool authored_matches[INVOKE_DESCRIPTORS] = { true, false, true, true, true, false, false };
    static const bool generated_matches[INVOKE_DESCRIPTORS] = { false, true, false, true, false, false, false };
    for (uint32_t index = 0; index < INVOKE_DESCRIPTORS; index += 1u) {
        bool ok = interned_matches(&f, index, authored_name) == authored_matches[index] &&
                  interned_matches(&f, index, generated_name) == generated_matches[index];
        if (!ok) {
            fprintf(stderr, "%s matched an invocation error name wrongly\n", invoke_descriptors[index]);
        }
        CHECK(ok);
    }

    /* A host's report takes the transition the first part selects. */
    CHECK(report_error(&f, actor, generated) == TA_OK);
    run_stable(&f, actor);
    CHECK(session_in(&f, actor, "by_state"));
    ta_actor_id other = spawn_invoking(&f);
    CHECK(report_error(&f, other, invoke_named(&f, other, "pay.ment", false)) == TA_OK);
    run_stable(&f, other);
    CHECK(session_in(&f, other, "by_part"));
    fixture_close(&f);
}

/* `s3` invokes `a.b.c.d.e`, whose error name has nine segments and its
 * done.invoke name seven, and `a.b.c.d.e.f.g`, whose done.invoke name has
 * nine; and `x`, whose names are short. Any error leaves for `failed`, any
 * done.invoke for `done`. */
static const ta_scxml_spec *deep_chart(void) {
    static const ta_str to_failed[] = { STR("failed"), {0} };
    static const ta_str to_done[] = { STR("done"), {0} };
    static const ta_scxml_transition_spec transitions[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("error"), .targets = to_failed },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("done.invoke"), .targets = to_done },
        {0},
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("a.b.c.d.e") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("a.b.c.d.e.f.g") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
        { .kind = TA_SCXML_INVOKE, .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("x") },
          .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } },
        {0},
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3"), .transitions = transitions, .invokes = invokes },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("failed") },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("done") },
        {0},
    };
    static const ta_scxml_spec spec = { .name = STR("deep"), .states = states };
    return &spec;
}

/* Whether `actor` is idle in `s3` with its three invocations and nothing
 * queued. */
static bool untouched(fixture *f, ta_actor_id actor) {
    const ta_actor *record = ta_actor_lookup(f->system, actor);
    return record != NULL && !record->macrostep && record->sparse != NULL && record->sparse->invoke_count == 3u &&
           !ta_interpreter_internal_pending(record) && session_in(f, actor, "s3");
}

/* Claim 7. */
static void test_too_deep_names(void) {
    fixture f;
    invoke_fixture_open(&f, deep_chart(), 8);
    ta_actor_id actor = spawn_invoking(&f);
    ta_invoke_id five = invoke_named(&f, actor, "a.b.c.d.e", false);
    ta_invoke_id seven = invoke_named(&f, actor, "a.b.c.d.e.f.g", false);
    ta_invoke_id short_id = invoke_named(&f, actor, "x", false);
    CHECK(five != 0 && seven != 0 && short_id != 0);
    CHECK(untouched(&f, actor));

    /* The error name would have nine segments: refused, nothing changes,
     * whether or not the report ends the invocation. */
    CHECK(report_error(&f, actor, five) == TA_ERR_INVALID);
    CHECK(ta_actor_report_invoke(f.system, &(ta_invoke_report_params){
                                               .actor = actor,
                                               .invoke_id = five,
                                               .kind = TA_INVOKE_REPORT_ERROR,
                                               .time = ta_system_time(f.system),
                                               .as.error = &(ta_invoke_error){ .reason = STR("x"), .ends = true },
                                           }) == TA_ERR_INVALID);
    CHECK(untouched(&f, actor));

    /* So would the done.invoke name of the seven-part ID: refused the same way. */
    CHECK(ta_actor_report_invoke(f.system, &(ta_invoke_report_params){ .actor = actor,
                                                                       .invoke_id = seven,
                                                                       .kind = TA_INVOKE_REPORT_DONE,
                                                                       .time = ta_system_time(f.system) }) ==
          TA_ERR_INVALID);
    CHECK(untouched(&f, actor));

    /* The five-part ID's done.invoke name has seven segments and is made. */
    ta_event_name_id done = 0;
    CHECK(ta_invoke_done_name(f.system, five, &done) == TA_OK);
    CHECK(ta_trie_path(&f.system->event_names, done, (ta_atom[TA_TRIE_MAX_DEPTH]){0}) == 7u);

    /* A short name for the same session is still made and taken. */
    CHECK(report_error(&f, actor, short_id) == TA_OK);
    run_stable(&f, actor);
    CHECK(session_in(&f, actor, "failed"));
    fixture_close(&f);
}

int main(void) {
    test_generated_prefixes();
    test_worked_examples();
    test_same_answers_as_the_trie();
    test_random_names_leave_no_trace();
    test_typeexpr_looks_up();
    test_error_name_splits_the_id();
    test_too_deep_names();
    test_eventexpr_travels_as_text();
    if (failures != 0) {
        fprintf(stderr, "name_matching_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("name_matching_test: ok");
    return 0;
}
