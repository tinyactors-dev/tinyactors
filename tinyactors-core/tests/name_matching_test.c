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
 * 9. names reach a host as text: random names a host posts, inspected
 *    while their macrostep is paused and captured in a session image, and
 *    random names a session mails itself, which a TAKEN outcome gives in
 *    the caller's names buffer with no ID, read back as the text sent, and
 *    the atom table and the trie keep their size; a call whose names
 *    buffer has no room for the next name answers MORE and loses none.
 * 11. run-time names of internal events and invocations travel as text:
 *    random names sent to #_internal, done.invoke.<id> of local
 *    invocations whose IDs core generates again and again, and
 *    error.communication.invoke.<id>.<reason> for random reasons a host
 *    reports leave the atom table and the trie the same size, and every
 *    envelope that held such a name goes back.
 * 10. an invoked session keeps its last external event's name as text: one
 *    kept envelope holds it while the event is kept, the next event gives
 *    it back, a session image carries the name and restores it into a
 *    kept envelope of its own, and the parent's cancel lets it go; random
 *    names leave the atom table and the trie the same size.
 *
 * Paths that still add a run-time name to the tables, and that claims 4,
 * 5, 8 and 9 therefore do not cover, because an event there carries its
 * name as a trie node: the value of a `<send eventexpr>` that is stored (a
 * delayed send, one held for an unpublished session, one for #_internal or
 * for an I/O processor); the generated invocation ID and the
 * `done.invoke.<id>` and `error.communication.invoke.<id>.<reason>` names
 * built from it (the ID and the reason, not the `invoke` segment);
 * `#_<invokeid>` targets and the family of a session URI in a
 * `targetexpr`; and the public calls a host uses to turn text into IDs
 * (ta_system_intern_atom, ta_event_name_intern).
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

static char typed[70000];
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
                                                         .name = test_event_text(f->system, &event), .event = &event },
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

/* Whether `name`, as a host reads it (ta_event_name_format), is `want`. */
static bool reads_as(fixture *f, ta_event_name_id name, ta_str want) {
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u + 1u];
    uint32_t needed = 0;
    return ta_event_name_format(f->system, &(ta_event_name_format_params){ .name = name, .buffer = buffer,
                                                                           .capacity = sizeof buffer,
                                                                           .needed = &needed }) == TA_OK &&
           needed == want.len + 1u && memcmp(buffer, want.ptr, want.len) == 0;
}

/* Claim 9. */
static void test_names_reach_hosts_as_text(void) {
    fixture f;
    fixture_open(&f, eventexpr_chart());
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_event_name_id send = event_name(&f, "send");
    set_type("warm", 4);
    deliver(&f, actor, send);
    /* The warm-up's mail to itself is taken before the rounds start. */
    CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    static char names[TA_RUN_NAMES_MIN_BYTES];
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    uint32_t mailed = 0;
    for (uint32_t round = 0; round < 2000u; round += 1u) {
        ta_str name = random_name(buffer, f.names.capacity);
        if (name.len > sizeof typed) {
            name.len = sizeof typed;
            while (name.len > 0 && buffer[name.len - 1u] == '.') {
                name.len -= 1u;
            }
        }
        if (name.len == 0 || (name.len == 4 && memcmp(name.ptr, "send", 4) == 0)) {
            continue;
        }
        /* The session mails itself the name: its TAKEN outcome gives the
         * name as text in the names buffer, and the event no ID. */
        set_type(name.ptr, name.len);
        ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = actor,
                                                                     .name = text("send"), .event = &event }) == TA_OK);
        for (uint32_t call = 0; call < 8u; call += 1u) {
            ta_input_outcome outcomes[2];
            uint32_t count = 0;
            uint32_t used = 0;
            ta_run_status run = TA_RUN_IDLE;
            CHECK(ta_run(f.system, &(ta_run_params){ .budget = 100, .report_taken = true, .outcomes = outcomes,
                                                     .outcome_capacity = 2, .outcome_count = &count,
                                                     .status = &run, .names = names,
                                                     .name_capacity = sizeof names, .name_used = &used }) == TA_OK);
            for (uint32_t at = 0; at < count; at += 1u) {
                if (outcomes[at].has_event) {
                    CHECK(outcomes[at].event.name == 0u);
                    CHECK(outcomes[at].name.len == name.len && memcmp(outcomes[at].name.ptr, name.ptr, name.len) == 0);
                    CHECK(used == name.len + 1u);
                    mailed += 1u;
                }
                ta_event_release(&outcomes[at].event);
            }
            if (run == TA_RUN_IDLE) {
                break;
            }
        }
        /* A host posts the name: inspection reads it as text while its
         * macrostep is paused, and a session image copies it. */
        CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = actor,
                                                                     .name = name, .event = &event }) == TA_OK);
        CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .pause_at_microstep_start = true,
                                                                         .status = &status }) == TA_OK);
        CHECK(status == TEST_DRIVE_PAUSED);
        ta_actor_inspection view;
        CHECK(ta_actor_inspect(f.system, actor, &view) == TA_OK && view.current_event != NULL &&
              reads_as(&f, view.current_event->name, name));
        ta_session_image *image = NULL;
        CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
        ta_session_image_destroy(image);
        CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
        CHECK(status == TEST_DRIVE_STABLE);
    }
    CHECK(mailed >= 1990u);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);

    /* More long names than the names buffer holds: the call that has no
     * room for the next one answers MORE, and the next call gives it. */
    static char long_name[3000];
    memset(long_name, 'x', sizeof long_name);
    set_type(long_name, sizeof long_name);
    enum { LONG_MAILS = 30 };
    for (uint32_t round = 0; round < LONG_MAILS; round += 1u) {
        ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
        CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = actor,
                                                                     .name = text("send"), .event = &event }) == TA_OK);
    }
    uint32_t calls = 0;
    uint32_t given = 0;
    for (ta_run_status run = TA_RUN_MORE; run != TA_RUN_IDLE && calls < 8u; calls += 1u) {
        static ta_input_outcome outcomes[256];
        uint32_t count = 0;
        uint32_t used = 0;
        CHECK(ta_run(f.system, &(ta_run_params){ .budget = 10000, .report_taken = true, .outcomes = outcomes,
                                                 .outcome_capacity = 256, .outcome_count = &count, .status = &run,
                                                 .names = names, .name_capacity = sizeof names,
                                                 .name_used = &used }) == TA_OK);
        uint32_t taken = 0;
        for (uint32_t at = 0; at < count; at += 1u) {
            if (outcomes[at].has_event) {
                CHECK(outcomes[at].name.len == sizeof long_name && outcomes[at].name.ptr[0] == 'x');
                taken += 1u;
            }
            ta_event_release(&outcomes[at].event);
        }
        given += taken;
        CHECK(used == taken * (sizeof long_name + 1u) && used <= sizeof names);
        /* Stopped for want of room: one more name would not have fit. */
        CHECK(given == LONG_MAILS || (run == TA_RUN_MORE && used + sizeof long_name + 1u > sizeof names));
    }
    CHECK(given == LONG_MAILS && calls == 2u);
    fixture_close(&f);
}

/* The envelopes the pool keeps only for a kept event's name. */
static uint32_t kept_envelopes(const fixture *f) {
    const ta_envelope_pool *pool = &f->system->run.pool;
    uint32_t count = 0;
    for (uint32_t index = 1u; index <= pool->items.high_water; index += 1u) {
        const ta_envelope *envelope = ta_envelope_pool_at(pool, index);
        count += envelope != NULL && envelope->state == TA_ENVELOPE_KEPT ? 1u : 0u;
    }
    return count;
}

static ta_definition_id kept_child_definition = 0;
static ta_actor_id kept_child = 0;

static ta_op_status kid_start(void *self, const ta_invoker_start_params *params) {
    (void)self;
    ta_result spawned = ta_actor_spawn(params->request->system,
                                       &(ta_actor_spawn_params){
                                           .definition = kept_child_definition,
                                           .parent = &(ta_actor_parent){ .actor = params->request->parent,
                                                                         .invoke_id = params->request->invoke_id },
                                           .out = &kept_child });
    if (spawned != TA_OK) {
        *params->fault = (ta_fault){ .code = spawned, .message = STR("no child") };
        return TA_OP_ERROR;
    }
    *params->service_handle = kept_child;
    return TA_OP_OK;
}
static ta_system *kept_system = NULL;
static ta_op_status kid_cancel(void *self, uint64_t service, ta_fault *fault) {
    (void)self, (void)fault;
    (void)ta_actor_destroy(kept_system, service); /* as a host invoker ends its service */
    return TA_OP_OK;
}
static ta_op_status kid_forward(void *self, const ta_invoker_forward_params *params) {
    (void)self, (void)params;
    return TA_OP_OK;
}
static const ta_invoker_vtable kid_vtable = { .start = kid_start, .cancel = kid_cancel, .forward = kid_forward };

/* Claim 10. */
static void test_kept_event_keeps_its_name(void) {
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("kid") } }, {0}
    };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_scxml_transition_spec working[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = to_idle }, {0}
    };
    static const ta_scxml_state_spec parent_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"), .invokes = invokes, .transitions = working },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle") },
        {0},
    };
    static const ta_scxml_spec parent = { .name = STR("parent"), .states = parent_states };
    static const ta_scxml_transition_spec any[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("*") }, {0}
    };
    static const ta_scxml_state_spec child_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c"), .transitions = any }, {0}
    };
    static const ta_scxml_spec child = { .name = STR("kid"), .states = child_states };
    fixture f;
    fixture_open(&f, &parent);
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(f.system, &(ta_definition_compile_params){
                                                   .spec = &child,
                                                   .datamodel = { .v = &dm_vtable },
                                                   .allocator = test_allocator_of(&f.heap),
                                                   .out = &compiled,
                                               }) == TA_OK);
    CHECK(ta_system_register_definition(f.system, compiled, &kept_child_definition) == TA_OK);
    ta_atom kid = 0;
    CHECK(ta_system_intern_atom(f.system, STR("kid"), &kid) == TA_OK);
    CHECK(ta_system_register_invoker(f.system, kid, (ta_invoker){ .v = &kid_vtable }) == TA_OK);
    kept_system = f.system;
    ta_actor_id parent_actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &parent_actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, parent_actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(kept_child != 0);
    CHECK(test_drive_run(f.system, kept_child, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = kept_child,
                                                                 .name = text("warm"), .event = &event }) == TA_OK);
    CHECK(test_drive_run(f.system, kept_child, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(kept_envelopes(&f) == 1u);
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    for (uint32_t round = 0; round < 500u; round += 1u) {
        ta_str name = random_name(buffer, f.names.capacity);
        if (name.len == 0) {
            continue;
        }
        CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system),
                                                                     .actor = kept_child, .name = name,
                                                                     .event = &event }) == TA_OK);
        CHECK(test_drive_run(f.system, kept_child, &(test_drive_run_params){ .status = &status }) == TA_OK);
        CHECK(status == TEST_DRIVE_STABLE);
        /* The new event's envelope is kept; the one before it went back. */
        CHECK(kept_envelopes(&f) == 1u);
    }
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    /* An image of the child carries the kept name, and a session restored
     * from it keeps the name in an envelope of its own until it goes. */
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(f.system, &(ta_actor_capture_params){ .actor = kept_child, .out = &image }) == TA_OK);
    ta_actor_id restored = 0;
    CHECK(ta_actor_reconstruct(f.system, &(ta_actor_reconstruct_params){ .image = image, .out = &restored }) == TA_OK);
    CHECK(kept_envelopes(&f) == 2u);
    ta_session_image_destroy(image);
    CHECK(ta_actor_destroy(f.system, restored) == TA_OK);
    CHECK(kept_envelopes(&f) == 1u);
    /* The parent's cancel ends the child, and the kept name goes with it. */
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .time = ta_system_time(f.system), .actor = parent_actor,
                                                                 .name = text("stop"), .event = &event }) == TA_OK);
    CHECK(test_drive_run(f.system, parent_actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(kept_envelopes(&f) == 0u);
    CHECK(atom_count(&f) == atoms);
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
    uint64_t names_depth = 0;
    CHECK(ta_system_param(f->system, TA_PARAM_EVENT_NAME_DEPTH, &names_depth) == TA_OK && names_depth > 0);
    f->scratch = malloc((size_t)names_depth * sizeof(ta_atom));
    ta_names_init(&f->names, &(ta_names_init_params){ .atoms = &f->system->atoms, .scratch = f->scratch,
                                                      .capacity = (uint32_t)names_depth });
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
        char buffer[128];
        ta_text_writer writer = { .buffer = buffer, .capacity = sizeof buffer };
        (void)ta_invoke_id_write(f->system, sparse->invokes[index].id, &writer);
        ta_str id = { .ptr = buffer, .len = writer.length < sizeof buffer ? writer.length : 0u };
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

/* `idle` sends `name` to #_internal on `send`, and counts every other
 * event its `*` transition takes. */
static const ta_scxml_spec *internal_chart(void) {
    static const ta_scxml_action_spec send[] = {
        { .kind = TA_SCXML_ACTION_SEND,
          .as.send = { .event = { .kind = TA_SCXML_EVENT_EXPR, .as.expr = EXPR("name") },
                       .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("#_internal") } } },
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
    static const ta_scxml_spec spec = { .name = STR("internal"), .states = states };
    return &spec;
}

/* Claim 11, #_internal. */
static void test_internal_names_travel_as_text(void) {
    fixture f;
    fixture_open(&f, internal_chart());
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f.system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    ta_event_name_id send = event_name(&f, "send");
    set_type("warm", 4);
    deliver(&f, actor, send);
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    assigned = 0;
    char buffer[TA_EVENT_NAME_MAX_DEPTH * 14u];
    uint32_t rounds = 0;
    for (uint32_t round = 0; round < 2000u; round += 1u) {
        ta_str name = random_name(buffer, f.names.capacity);
        bool sends = name.len >= 4 && memcmp(name.ptr, "send", 4) == 0 && (name.len == 4 || name.ptr[4] == '.');
        if (name.len == 0 || name.len > sizeof typed || sends) {
            continue; /* `send` and its descendants take the send transition */
        }
        set_type(name.ptr, name.len);
        deliver(&f, actor, send);
        rounds += 1u;
    }
    /* Each was taken from the internal queue and counted by `*`. */
    CHECK(assigned == rounds);
    /* A long name, held in many chunks, is taken too. */
    static char long_name[3005];
    memcpy(long_name, "long.", 5);
    memset(long_name + 5, 'x', sizeof long_name - 5);
    set_type(long_name, sizeof long_name);
    uint32_t before = assigned;
    deliver(&f, actor, send);
    CHECK(assigned == before + 1u);
    /* A name longer than one name-area field cannot be held: the send
     * fails once, as error.execution.capacity.names, which `*` counts. */
    static char too_long[70000];
    memset(too_long, 'z', sizeof too_long);
    set_type(too_long, sizeof too_long);
    before = assigned;
    deliver(&f, actor, send);
    CHECK(assigned == before + 1u);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    CHECK(kept_envelopes(&f) == 0u);
    fixture_close(&f);
}

/* Claim 11, local invocations: `working` invokes a `kid` with no ID, whose
 * session finishes at once; its done.invoke takes the parent to `idle`,
 * and `again` starts another, with a new generated ID. */
static void test_invocation_names_travel_as_text(void) {
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("kid") } }, {0}
    };
    static const ta_str to_idle[] = { STR("idle"), {0} };
    static const ta_str to_working[] = { STR("working"), {0} };
    static const ta_scxml_transition_spec working[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("done.invoke"), .targets = to_idle }, {0}
    };
    static const ta_scxml_transition_spec idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("again"), .targets = to_working }, {0}
    };
    static const ta_scxml_state_spec parent_states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("working"), .invokes = invokes, .transitions = working },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = idle },
        {0},
    };
    static const ta_scxml_spec parent = { .name = STR("parent"), .states = parent_states };
    static const ta_scxml_state_spec child_states[] = { { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") }, {0} };
    static const ta_scxml_spec child = { .name = STR("kid"), .states = child_states };
    fixture f;
    fixture_open(&f, &parent);
    ta_definition *compiled = NULL;
    CHECK(ta_definition_compile_spec(f.system, &(ta_definition_compile_params){
                                                   .spec = &child,
                                                   .datamodel = { .v = &dm_vtable },
                                                   .allocator = test_allocator_of(&f.heap),
                                                   .out = &compiled,
                                               }) == TA_OK);
    CHECK(ta_system_register_definition(f.system, compiled, &kept_child_definition) == TA_OK);
    ta_atom kid = 0;
    CHECK(ta_system_intern_atom(f.system, STR("kid"), &kid) == TA_OK);
    CHECK(ta_system_register_invoker(f.system, kid, (ta_invoker){ .v = &kid_vtable }) == TA_OK);
    kept_system = f.system;
    ta_actor_id parent_actor = 0;
    CHECK(ta_actor_spawn(f.system, &(ta_actor_spawn_params){ .definition = f.id, .out = &parent_actor }) == TA_OK);
    ta_event_name_id again = event_name(&f, "again");
    uint32_t atoms = 0;
    uint32_t nodes = 0;
    for (uint32_t round = 0; round < 300u; round += 1u) {
        /* The parent starts an invocation; the child finishes at once, and
         * its done.invoke.<stateid>.<serial> returns the parent to idle. */
        for (uint32_t turn = 0; turn < 4u; turn += 1u) {
            test_drive_status status = TEST_DRIVE_ERROR;
            CHECK(test_drive_run(f.system, parent_actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
            if (kept_child != 0) {
                (void)test_drive_run(f.system, kept_child, &(test_drive_run_params){ .status = &status });
            }
        }
        CHECK(session_in(&f, parent_actor, "idle"));
        kept_child = 0;
        if (round == 1u) {
            atoms = atom_count(&f);
            nodes = node_count(&f);
        }
        deliver(&f, parent_actor, again);
    }
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    CHECK(kept_envelopes(&f) == 0u);
    fixture_close(&f);
}

/* `s3` invokes a `svc` with no ID, and takes every error it reports
 * without leaving. */
static const ta_scxml_spec *reporting_chart(void) {
    static const ta_scxml_transition_spec stay[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("error.communication.invoke.*") }, {0}
    };
    static const ta_scxml_invoke_spec invokes[] = {
        { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") } }, {0}
    };
    static const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("s3"), .transitions = stay, .invokes = invokes }, {0}
    };
    static const ta_scxml_spec spec = { .name = STR("reporting"), .states = states };
    return &spec;
}

/* Claim 11, a host's error reports with random reasons. */
static void test_reported_errors_travel_as_text(void) {
    fixture f;
    invoke_fixture_open(&f, reporting_chart(), 0);
    ta_actor_id actor = spawn_invoking(&f);
    ta_invoke_id generated = invoke_named(&f, actor, "s3.", true);
    CHECK(generated != 0);
    CHECK(ta_actor_report_invoke(f.system, &(ta_invoke_report_params){
                                               .actor = actor, .invoke_id = generated,
                                               .kind = TA_INVOKE_REPORT_ERROR,
                                               .time = ta_system_time(f.system),
                                               .as.error = &(ta_invoke_error){ .reason = STR("warm") } }) == TA_OK);
    run_stable(&f, actor);
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    char reason[16];
    for (uint32_t round = 0; round < 1000u; round += 1u) {
        uint32_t length = 1u + (uint32_t)(rand() % 12);
        for (uint32_t at = 0; at < length; at += 1u) {
            reason[at] = (char)('a' + rand() % 26);
        }
        CHECK(ta_actor_report_invoke(f.system, &(ta_invoke_report_params){
                                                   .actor = actor, .invoke_id = generated,
                                                   .kind = TA_INVOKE_REPORT_ERROR,
                                                   .time = ta_system_time(f.system),
                                                   .as.error = &(ta_invoke_error){
                                                       .reason = { .ptr = reason, .len = length } } }) == TA_OK);
        run_stable(&f, actor);
    }
    CHECK(session_in(&f, actor, "s3"));
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);
    CHECK(kept_envelopes(&f) == 0u);
    fixture_close(&f);
}

/* Claim 6. */
static void test_error_name_splits_the_id(void) {
    fixture f;
    invoke_fixture_open(&f, invoke_chart(), 0);
    ta_actor_id actor = spawn_invoking(&f);
    ta_invoke_id authored = invoke_named(&f, actor, "pay.ment", false);
    ta_invoke_id generated = invoke_named(&f, actor, "s3.", true);
    CHECK(authored != 0 && generated != 0);
    char generated_buffer[64];
    ta_text_writer generated_writer = { .buffer = generated_buffer, .capacity = sizeof generated_buffer };
    CHECK(ta_invoke_id_write(f.system, generated, &generated_writer) && generated_writer.length < 64u);
    ta_str generated_text = { .ptr = generated_buffer, .len = generated_writer.length };
    char platform[32] = {0};
    CHECK(generated_text.len > 3u && generated_text.len - 3u < sizeof platform);
    memcpy(platform, generated_text.ptr + 3, generated_text.len - 3u);
    CHECK(strchr(platform, '.') == NULL);
    /* The generated ID's text is a state ID and a serial, and nothing of
     * it is interned: the serial is no atom. */
    CHECK(!is_atom(&f, platform, NULL));

    /* The names are text, built without interning (ADR 0100). */
    uint32_t atoms = atom_count(&f);
    uint32_t nodes = node_count(&f);
    ta_str authored_name = { 0 };
    ta_str generated_name = { 0 };
    CHECK(ta_invoke_name_text(f.system, (ta_invoke_name){ .system = f.system, .invoke = authored,
                                                           .reason = STR("failed") },
                              &authored_name) == TA_OK);
    CHECK(ta_invoke_name_text(f.system, (ta_invoke_name){ .system = f.system, .invoke = generated,
                                                           .reason = STR("failed") },
                              &generated_name) == TA_OK);
    char full[96];
    snprintf(full, sizeof full, "error.communication.invoke.s3.%s.failed", platform);
    CHECK(authored_name.len == strlen("error.communication.invoke.pay.ment.failed") &&
          memcmp(authored_name.ptr, "error.communication.invoke.pay.ment.failed", authored_name.len) == 0);
    CHECK(generated_name.len == strlen(full) && memcmp(generated_name.ptr, full, generated_name.len) == 0);

    /* One segment per dotted part: the resolver splits the ID, the serial
     * being no atom (UNKNOWN), and the descriptors match by prefix of
     * whole segments. */
    static const bool authored_matches[INVOKE_DESCRIPTORS] = { true, false, true, true, true, false, false };
    static const bool generated_matches[INVOKE_DESCRIPTORS] = { false, true, false, true, false, false, false };
    for (uint32_t index = 0; index < INVOKE_DESCRIPTORS; index += 1u) {
        CHECK(ta_names_resolve(&f.names, authored_name) == TA_OK && f.names.count == 6u);
        bool ok = resolved_matches(&f, index) == authored_matches[index];
        CHECK(ta_names_resolve(&f.names, generated_name) == TA_OK && f.names.count == 6u);
        ok = ok && resolved_matches(&f, index) == generated_matches[index];
        if (!ok) {
            fprintf(stderr, "%s matched an invocation error name wrongly\n", invoke_descriptors[index]);
        }
        CHECK(ok);
    }
    ta_invoke_name_free(f.system, authored_name);
    ta_invoke_name_free(f.system, generated_name);
    CHECK(atom_count(&f) == atoms);
    CHECK(node_count(&f) == nodes);

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
    ta_str done = { 0 };
    CHECK(ta_invoke_name_text(f.system, (ta_invoke_name){ .system = f.system, .invoke = five }, &done) == TA_OK);
    CHECK(ta_names_resolve(&f.names, done) == TA_OK && f.names.count == 7u);
    ta_invoke_name_free(f.system, done);

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
    test_names_reach_hosts_as_text();
    test_kept_event_keeps_its_name();
    test_internal_names_travel_as_text();
    test_invocation_names_travel_as_text();
    test_reported_errors_travel_as_text();
    if (failures != 0) {
        fprintf(stderr, "name_matching_test: %d failure(s)\n", failures);
        return 1;
    }
    puts("name_matching_test: ok");
    return 0;
}
