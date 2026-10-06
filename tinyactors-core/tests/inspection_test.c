/*
 * Definition and actor inspection (ADR 0042, work package 1), exercised
 * through the public header and the amalgamation only. Inspection reports
 * the event of a macrostep in progress from the pool envelope of the input
 * it took, which holds the event until the input completes (ADR 0069,
 * ADR 0099).
 */
#include <stdio.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "test_drive.h"

static int failures;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__,    \
                    #condition);                                                \
            failures += 1;                                                      \
        }                                                                       \
    } while (0)

/* ------------------------------------------------------------------------
 * A test allocator: the system returns every block to it.
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * A data model that counts calls to `inspect` and releases of its root.
 */

typedef struct {
    int inspects;
    int root_releases;
} counting_model;

static void root_release(void *self) {
    ((counting_model *)self)->root_releases += 1;
}

static const ta_value_vtable root_vtable = { .release = root_release };

static ta_result count_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}

static void count_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self;
    (void)shared;
    (void)unused;
}

static uint32_t count_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 24;
}

static uint32_t count_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 8;
}

static ta_result count_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static void count_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static ta_result count_inspect(void *self, const ta_datamodel_inspect_params *params) {
    counting_model *model = self;
    model->inspects += 1;
    *params->out = (ta_value){ .self = model, .v = &root_vtable };
    return TA_OK;
}

static const ta_datamodel_vtable counting_vtable = {
    .name = { .ptr = "counting", .len = 8 },
    .prepare_definition = count_prepare,
    .destroy_definition = count_destroy_definition,
    .instance_size = count_instance_size,
    .instance_alignment = count_instance_alignment,
    .init_instance = count_init_instance,
    .destroy_instance = count_destroy_instance,
    .inspect = count_inspect,
};

/* The same adapter without an `inspect` operation. */
static const ta_datamodel_vtable opaque_vtable = {
    .name = { .ptr = "opaque", .len = 6 },
    .prepare_definition = count_prepare,
    .destroy_definition = count_destroy_definition,
    .instance_size = count_instance_size,
    .instance_alignment = count_instance_alignment,
    .init_instance = count_init_instance,
    .destroy_instance = count_destroy_instance,
};

/* ------------------------------------------------------------------------
 * Charts
 */

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

static const ta_scxml_state_spec two_states[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_EXTERNAL,
              .event = STR("go"),
              .targets = (const ta_str[]){ STR("busy"), {0} } },
            {0},
        },
    },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("busy") },
    {0},
};

typedef struct {
    const ta_scxml_spec *spec;
    ta_datamodel model;
    ta_definition_id *out;
} compile_request;

/* Compiles and registers a chart. */
static ta_result compile(ta_system *system, compile_request request) {
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(system, &(ta_definition_compile_params){
                                                              .spec = request.spec,
                                                              .datamodel = request.model,
                                                              .allocator = allocator,
                                                              .out = &definition,
                                                          });
    if (result != TA_OK) {
        return result;
    }
    return ta_system_register_definition(system, definition, request.out);
}

static bool str_is(ta_str text, const char *expected) {
    uint32_t length = 0;
    while (expected[length] != '\0') {
        length += 1;
    }
    if (text.len != length || (length != 0 && text.ptr == NULL)) {
        return false;
    }
    for (uint32_t index = 0; index < length; index += 1) {
        if (text.ptr[index] != expected[index]) {
            return false;
        }
    }
    return true;
}

static bool bytes_equal(ta_bytes left, ta_bytes right) {
    if (left.len != right.len) {
        return false;
    }
    for (uint32_t index = 0; index < left.len; index += 1) {
        if (left.ptr[index] != right.ptr[index]) {
            return false;
        }
    }
    return true;
}

typedef struct {
    int visited;
    size_t allocated_bytes;
} visit_log;

static ta_result record_visit(void *self, const ta_actor_inspection *inspection) {
    visit_log *log = self;
    log->visited += 1;
    log->allocated_bytes = inspection->allocated_bytes;
    return TA_OK;
}

/* ------------------------------------------------------------------------
 * Tests
 */

static void test_definition_inspect(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id named = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("light"), .binding = TA_SCXML_BINDING_LATE, .states = two_states },
                  counting, &named}) == TA_OK);

    ta_definition_inspection view;
    CHECK(ta_definition_inspect(system, named, &view) == TA_OK);
    CHECK(view.id == named);
    CHECK(str_is(view.identity.family.name, "light"));
    CHECK(view.identity.revision.len != 0);
    CHECK(str_is(view.document_name, "light"));
    CHECK(view.datamodel.self == model && view.datamodel.v == &counting_vtable);
    CHECK(str_is(view.datamodel.v->name, "counting"));
    CHECK(view.binding == TA_SCXML_BINDING_LATE);
    CHECK(view.state_count == 2);
    CHECK(!view.retired);
    CHECK(view.actor_count == 0);
    CHECK(view.definition_bytes > 0);
    CHECK(view.actor_fixed_bytes >= 24u);

    /* An explicit identity without <scxml name>: document_name is empty. */
    ta_definition_identity identity = { .family = { STR("family") },
                                        .revision = { .ptr = (const uint8_t *)"r1", .len = 2 } };
    ta_definition_id anonymous = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .identity = &identity, .states = two_states }, counting, &anonymous}) ==
          TA_OK);
    CHECK(ta_definition_inspect(system, anonymous, &view) == TA_OK);
    CHECK(view.document_name.len == 0);
    CHECK(view.binding == TA_SCXML_BINDING_EARLY);
    CHECK(str_is(view.identity.family.name, "family"));
    CHECK(bytes_equal(view.identity.revision, identity.revision));

    CHECK(ta_definition_inspect(system, anonymous + 100u, &view) == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_inspect(system, named, NULL) == TA_ERR_INVALID);
    CHECK(model->inspects == 0);
}

static void test_find_definition(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_identity identity = { .family = { STR("finder") },
                                        .revision = { .ptr = (const uint8_t *)"rev-a", .len = 5 } };
    const ta_scxml_spec spec = { .identity = &identity, .states = two_states };
    ta_definition_id first = 0;
    ta_definition_id second = 0;
    CHECK(compile(system, (compile_request){&spec, counting, &first}) == TA_OK);

    ta_definition_id found = UINT32_MAX;
    /* The caller's copy of the identity, not the definition's pointers. */
    char family[] = "finder";
    uint8_t revision[] = { 'r', 'e', 'v', '-', 'a' };
    ta_definition_identity probe = { .family = { { .ptr = family, .len = 6 } },
                                     .revision = { .ptr = revision, .len = 5 } };
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_OK);
    CHECK(found == first);

    /* Registration is idempotent by identity (ADR 0077). */
    CHECK(compile(system, (compile_request){&spec, counting, &second}) == TA_OK);
    CHECK(second == first);
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_OK);
    CHECK(found == first);

    revision[4] = 'b';
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_ERR_NOT_FOUND);
    revision[4] = 'a';
    probe.revision.len = 4;
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_ERR_NOT_FOUND);
    probe.revision.len = 5;
    family[0] = 'F';
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_find_definition(system, NULL, &found) == TA_ERR_INVALID);

    /* A fingerprinted definition is found by the identity it reports. */
    ta_definition_id printed = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("printed"), .states = two_states }, counting, &printed}) ==
          TA_OK);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(system, printed, &view) == TA_OK);
    CHECK(ta_system_find_definition(system, &view.identity, &found) == TA_OK);
    CHECK(found == printed);
}

static void test_actor_inspection(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id definition = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("actors"), .states = two_states }, counting, &definition}) ==
          TA_OK);

    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);

    ta_definition_inspection facts;
    CHECK(ta_definition_inspect(system, definition, &facts) == TA_OK);
    CHECK(facts.actor_count == 1);

    int inspects = model->inspects;
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(view.definition_id == definition);
    CHECK(view.active_state_count == 1);
    CHECK(view.allocated_bytes >= facts.actor_fixed_bytes);
    CHECK(model->inspects == inspects);

    visit_log log = {0};
    CHECK(ta_system_visit_actors(system, &(ta_actor_visit_params){
                                             .inspector = { .self = &log, .visit = record_visit } }) == TA_OK);
    CHECK(log.visited == 1);
    CHECK(log.allocated_bytes == view.allocated_bytes);
    CHECK(model->inspects == inspects);
    CHECK(model->root_releases == 0);

    ta_value root = {0};
    CHECK(ta_actor_datamodel_root(system, actor, &root) == TA_OK);
    CHECK(model->inspects == inspects + 1);
    CHECK(root.self == model && root.v == &root_vtable);
    ta_value_release(&root);
    CHECK(model->root_releases == 1);
    CHECK(ta_actor_datamodel_root(system, actor + 1u, &root) == TA_ERR_NOT_FOUND);
    CHECK(ta_actor_datamodel_root(system, actor, NULL) == TA_ERR_INVALID);

    /* An adapter without `inspect` has no root. */
    ta_definition_id opaque = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("opaque"), .states = two_states },
                  (ta_datamodel){ .self = model, .v = &opaque_vtable }, &opaque}) == TA_OK);
    ta_actor_id hidden = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = opaque, .out = &hidden }) == TA_OK);
    root = (ta_value){ .self = model, .v = &root_vtable };
    CHECK(ta_actor_datamodel_root(system, hidden, &root) == TA_ERR_UNSUPPORTED);
    CHECK(root.v == NULL);
    CHECK(ta_actor_inspect(system, hidden, &view) == TA_OK);

    /* Destroying an actor uncounts it. */
    CHECK(ta_actor_destroy(system, actor) == TA_OK);
    CHECK(ta_definition_inspect(system, definition, &facts) == TA_OK);
    CHECK(facts.actor_count == 0);
    CHECK(ta_definition_inspect(system, opaque, &facts) == TA_OK);
    CHECK(facts.actor_count == 1);
}

/* A ta_str as a call argument; STR() above is an initializer. */
#define S(text) ((ta_str){ .ptr = (text), .len = (uint32_t)(sizeof(text) - 1u) })

/* A host value that counts its retains and releases. */
typedef struct {
    int retains;
    int releases;
} counted_value;

static void counted_retain(void *self) {
    ((counted_value *)self)->retains += 1;
}

static void counted_release(void *self) {
    ((counted_value *)self)->releases += 1;
}

static const ta_value_vtable counted_vtable = { .retain = counted_retain, .release = counted_release };

/*
 * The event of a macrostep in progress comes from its input: nothing while
 * the input waits on the run queue; the delivered event, its name, send ID
 * and the very data value, while the macrostep that took it is paused; and
 * nothing once the input completes. The input holds the data from delivery
 * to completion, retained once and released once, never copied.
 */
static void test_current_event_from_the_input(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id definition = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("held"), .states = two_states }, counting,
                                            &definition}) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);

    ta_atom atom = 0;
    ta_event_name_id go = 0;
    CHECK(ta_system_intern_atom(system, S("go"), &atom) == TA_OK);
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &go) == TA_OK);
    counted_value data = {0};
    ta_event event = { .name = go, .type = TA_EVENT_EXTERNAL, .send_id = 42u, .invoke_id = TA_INVOKE_ID_NONE,
                       .data = { .self = &data, .v = &counted_vtable } };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                                .name = test_event_text(system, &event), .event = &event }) == TA_OK);
    CHECK(data.retains == 1 && data.releases == 0);

    /* Admitted, not taken: no macrostep holds it yet. */
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(!view.macrostep_in_progress && view.current_event == NULL);

    /* Taken, its macrostep paused at the first microstep's start. */
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .pause_at_microstep_start = true,
                                                                  .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(view.macrostep_in_progress && view.current_event != NULL);
    if (view.current_event != NULL) {
        CHECK(view.current_event->name == go);
        CHECK(view.current_event->type == TA_EVENT_EXTERNAL);
        CHECK(view.current_event->send_id == 42u);
        CHECK(view.current_event->data.self == &data && view.current_event->data.v == &counted_vtable);
    }
    CHECK(data.retains == 1 && data.releases == 0);

    /* Completed: the input gave the event back, its data released once. */
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(!view.macrostep_in_progress && view.current_event == NULL);
    CHECK(data.retains == 1 && data.releases == 1);
    CHECK(ta_actor_destroy(system, actor) == TA_OK);
}

/*
 * The current event an inspection reports is borrowed until the next
 * inspection reads another input's event: inspecting a second paused actor
 * replaces what the first inspection's pointer shows, and inspecting the
 * first again reads its own event back.
 */
static void test_current_event_replaced_by_the_next_inspection(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id definition = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("held twice"), .states = two_states },
                                            counting, &definition}) == TA_OK);
    ta_atom atom = 0;
    ta_event_name_id go = 0;
    CHECK(ta_system_intern_atom(system, S("go"), &atom) == TA_OK);
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &go) == TA_OK);

    ta_actor_id actors[2] = {0};
    counted_value data[2] = {{0}};
    for (uint32_t index = 0; index < 2u; index += 1u) {
        CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .out = &actors[index] }) ==
              TA_OK);
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(test_drive_run(system, actors[index], &(test_drive_run_params){ .status = &status }) == TA_OK);
        ta_event event = { .name = go, .type = TA_EVENT_EXTERNAL, .send_id = 7u + index,
                           .invoke_id = TA_INVOKE_ID_NONE,
                           .data = { .self = &data[index], .v = &counted_vtable } };
        CHECK(test_drive_deliver(system,
                                 &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actors[index],
                                                             .name = test_event_text(system, &event), .event = &event },
                                 &(test_drive_run_params){ .pause_at_microstep_start = true, .status = &status }) ==
              TA_OK);
        CHECK(status == TEST_DRIVE_PAUSED);
    }

    ta_actor_inspection first;
    CHECK(ta_actor_inspect(system, actors[0], &first) == TA_OK);
    CHECK(first.current_event != NULL);
    if (first.current_event != NULL) {
        CHECK(first.current_event->send_id == 7u && first.current_event->data.self == &data[0]);
    }
    ta_actor_inspection second;
    CHECK(ta_actor_inspect(system, actors[1], &second) == TA_OK);
    CHECK(second.current_event != NULL);
    if (second.current_event != NULL) {
        CHECK(second.current_event->send_id == 8u && second.current_event->data.self == &data[1]);
    }
    /* The first pointer now shows the second actor's event. */
    if (first.current_event != NULL) {
        CHECK(first.current_event->send_id == 8u && first.current_event->data.self == &data[1]);
    }
    CHECK(ta_actor_inspect(system, actors[0], &first) == TA_OK);
    if (first.current_event != NULL) {
        CHECK(first.current_event->send_id == 7u && first.current_event->data.self == &data[0]);
    }

    for (uint32_t index = 0; index < 2u; index += 1u) {
        test_drive_status status = TEST_DRIVE_ERROR;
        CHECK(test_drive_run(system, actors[index], &(test_drive_run_params){ .status = &status }) == TA_OK);
        CHECK(status == TEST_DRIVE_STABLE);
        CHECK(data[index].retains == 1 && data[index].releases == 1);
        CHECK(ta_actor_destroy(system, actors[index]) == TA_OK);
    }
}

/* ------------------------------------------------------------------------
 * Reading identities back (ADR 0049): the inverses of ta_definition_state
 * and ta_system_intern_atom.
 *
 * Catches: a lookup that scans text instead of comparing the atom the
 * compiler interned (it would answer for a state of another definition, or
 * miss one whose text a caller spelled with a different buffer), an atom
 * table that hands out text for a handle it never issued, and a state ID
 * that does not round-trip through its author's ID.
 */
static void test_reading_identities_back(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id two = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("round"), .states = two_states },
                  counting, &two}) == TA_OK);

    /* Every state of the definition round-trips: id -> text -> id. */
    ta_definition_inspection facts;
    CHECK(ta_definition_inspect(system, two, &facts) == TA_OK);
    for (uint32_t index = 0; index < facts.state_count; index += 1) {
        ta_state_info info;
        CHECK(ta_definition_state(system, &(ta_definition_state_params){
                                              .definition = two, .state = index, .out = &info }) == TA_OK);
        ta_state_id found = UINT32_MAX;
        CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                                   .definition = two, .id = info.id, .out = &found }) == TA_OK);
        CHECK(found == index);
    }

    /* A state the definition does not have is a question, not a failure. */
    ta_state_id missing = 4242;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = two, .id = S("nowhere"),
                                               .out = &missing }) == TA_ERR_NOT_FOUND);
    CHECK(missing == 4242);
    /* Including one whose text this system has interned for something else. */
    ta_atom elsewhere = 0;
    CHECK(ta_system_intern_atom(system, S("interned-but-not-a-state"), &elsewhere) == TA_OK);
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = two, .id = S("interned-but-not-a-state"),
                                               .out = &missing }) == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = 999999u, .id = S("idle"),
                                               .out = &missing }) == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_find_state(system, NULL) == TA_ERR_INVALID);

    /* An atom reads back as the text it interns, and interning it again is
     * the same atom (ADR 0007). */
    ta_atom first = 0;
    ta_atom again = 0;
    CHECK(ta_system_intern_atom(system, S("client-stream"), &first) == TA_OK);
    CHECK(ta_system_intern_atom(system, S("client-stream"), &again) == TA_OK);
    CHECK(first == again);
    ta_str text = {0};
    CHECK(ta_system_atom_text(system, first, &text) == TA_OK);
    CHECK(str_is(text, "client-stream"));
    /* The type core reserves for its own processor reads back too. */
    CHECK(ta_system_atom_text(system, ta_system_scxml_io_type(system), &text) == TA_OK);
    CHECK(text.len != 0);
    /* A handle this system never issued is not text. */
    text = (ta_str){ .ptr = "stale", .len = 5 };
    CHECK(ta_system_atom_text(system, 987654u, &text) == TA_ERR_NOT_FOUND);
    CHECK(str_is(text, "stale"));
    CHECK(ta_system_atom_text(system, first, NULL) == TA_ERR_INVALID);

    /* An invocation's ID reads back as the text that names it: what
     * `done.invoke.<id>` carries and what a `#_<invokeid>` target spells.
     * Parsing such a target is how a caller without a running invocation
     * gets an ID to ask about, and the two grammars agree. */
    ta_scxml_target target = {0};
    CHECK(ta_scxml_target_parse(system, S("#_fetchUser"), &target) == TA_OK);
    CHECK(target.kind == TA_SCXML_TARGET_INVOKE);
    CHECK(ta_system_invoke_id_text(system, target.as.invoke, &text) == TA_OK);
    CHECK(str_is(text, "fetchUser"));
    /* The dotted form core generates for an <invoke> without an author ID is
     * one ID, not a path. */
    CHECK(ta_scxml_target_parse(system, S("#_work.2"), &target) == TA_OK);
    CHECK(ta_system_invoke_id_text(system, target.as.invoke, &text) == TA_OK);
    CHECK(str_is(text, "work.2"));

    text = (ta_str){ .ptr = "stale", .len = 5 };
    CHECK(ta_system_invoke_id_text(system, 0, &text) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_invoke_id_text(system, 987654u, &text) == TA_ERR_NOT_FOUND);
    CHECK(str_is(text, "stale"));
    CHECK(ta_system_invoke_id_text(system, target.as.invoke, NULL) == TA_ERR_INVALID);
}

/* ------------------------------------------------------------------------
 * Transitions, actions and expressions by ID (ADR 0071)
 */

#define SRC(text) { .kind = TA_EXPRESSION_SOURCE, .as.source = STR(text) }
#define LOG(text) { .kind = TA_SCXML_ACTION_LOG, .as.log = { .label = STR(text) } }

/*
 * A chart with every kind of block: <onentry> twice, <onexit>, transition
 * content, <if> with an <else>, a <foreach> body, <finalize> of two
 * <invoke>s, an <initial> with content, and the document's <script>. q has
 * no <initial>, so SCXML's default applies to it and to the document.
 */
static const ta_scxml_state_spec every_block[] = {
    {
        .kind = TA_SCXML_STATE_COMPOUND,
        .id = STR("p"),
        .initial = &(const ta_scxml_initial_spec){
            .targets = (const ta_str[]){ STR("a"), {0} },
            .actions = (const ta_scxml_action_spec[]){ LOG("init"), {0} },
        },
        .states = (const ta_scxml_state_spec[]){
            {
                .kind = TA_SCXML_STATE_ATOMIC,
                .id = STR("a"),
                .onentry = (const ta_scxml_handler_spec[]){
                    { .actions = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_ASSIGN,
                            .as.assign = { .location = SRC("x"),
                                           .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SRC("1") } } },
                          { .kind = TA_SCXML_ACTION_IF,
                            .as.conditional = {
                                .condition = SRC("c1"),
                                .then_actions = (const ta_scxml_action_spec[]){
                                    { .kind = TA_SCXML_ACTION_RAISE, .as.raise = { .event = STR("r") } }, {0} },
                                .else_actions = (const ta_scxml_action_spec[]){ LOG("no"), {0} },
                            } },
                          {0} } },
                    { .actions = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_SEND,
                            .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("e") } } },
                          {0} } },
                    {0},
                },
                .onexit = (const ta_scxml_handler_spec[]){
                    { .actions = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_FOREACH,
                            .as.foreach = { .array = SRC("items"), .item = STR("it"),
                                            .body = (const ta_scxml_action_spec[]){ LOG("each"), {0} } } },
                          {0} } },
                    {0},
                },
                .transitions = (const ta_scxml_transition_spec[]){
                    { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                      .event = STR("go.now"),
                      .condition = SRC("ready"),
                      .targets = (const ta_str[]){ STR("b"), {0} },
                      .actions = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_CANCEL,
                            .as.cancel = { .sendid = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("s1") } } },
                          {0} } },
                    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = (const ta_str[]){ STR("b"), {0} } },
                    {0},
                },
                .invokes = (const ta_scxml_invoke_spec[]){
                    { .kind = TA_SCXML_INVOKE,
                      .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") },
                      .finalize = (const ta_scxml_action_spec[]){
                          { .kind = TA_SCXML_ACTION_ASSIGN,
                            .as.assign = { .location = SRC("y"),
                                           .value = { .kind = TA_SCXML_VALUE_EXPR, .as.expr = SRC("2") } } },
                          {0} } },
                    { .kind = TA_SCXML_INVOKE,
                      .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("svc") },
                      .finalize = (const ta_scxml_action_spec[]){ LOG("done"), {0} } },
                    {0},
                },
            },
            { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b") },
            {0},
        },
    },
    {
        .kind = TA_SCXML_STATE_COMPOUND,
        .id = STR("q"),
        .states = (const ta_scxml_state_spec[]){ { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c") }, {0} },
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("back"), .targets = (const ta_str[]){ STR("c"), {0} } },
            {0},
        },
    },
    {0},
};

static ta_state_id state_named(ta_system *system, ta_definition_id definition, const char *id) {
    uint32_t length = 0;
    while (id[length] != '\0') {
        length += 1;
    }
    ta_state_id state = TA_STATE_NONE;
    CHECK(ta_definition_find_state(system, &(ta_definition_find_state_params){
                                               .definition = definition, .id = { id, length }, .out = &state }) == TA_OK);
    return state;
}

static bool targets_are(const ta_transition_info *info, ta_state_id only) {
    return info->target_count == 1 && info->targets != NULL && info->targets[0] == only;
}

static ta_transition_info transition_at(ta_system *system, ta_definition_id definition, ta_transition_id transition) {
    ta_transition_info info = { .origin = TA_TRANSITION_AUTHORED };
    CHECK(ta_definition_transition(system, &(ta_definition_transition_params){
                                               .definition = definition, .transition = transition, .out = &info }) ==
          TA_OK);
    return info;
}

/* Every transition, <initial>s and the document's included; returns go.now's ID. */
static ta_transition_id check_transitions(ta_system *system, ta_definition_id definition, uint32_t count) {
    ta_state_id p = state_named(system, definition, "p");
    ta_state_id q = state_named(system, definition, "q");
    ta_state_id a = state_named(system, definition, "a");
    ta_state_id b = state_named(system, definition, "b");
    ta_state_id c = state_named(system, definition, "c");
    CHECK(count == 6); /* three <transition>s, then the initial transitions of p, q and the document */
    uint32_t authored = 0;
    uint32_t initial = 0;
    uint32_t bootstrap = 0;
    ta_transition_id go_now = TA_TRANSITION_NONE;
    for (ta_transition_id id = 0; id < count; id += 1u) {
        ta_transition_info info = transition_at(system, definition, id);
        switch (info.origin) {
            case TA_TRANSITION_AUTHORED:
                CHECK(authored == id); /* the <transition>s come first */
                authored += 1u;
                CHECK(!info.default_targets);
                if (str_is(info.event, "go.now")) {
                    go_now = id;
                    CHECK(info.source == a && targets_are(&info, b));
                    CHECK(info.type == TA_SCXML_TRANSITION_EXTERNAL);
                    CHECK(info.condition != TA_EXPR_ID_NONE && info.action_count == 1);
                } else if (str_is(info.event, "back")) {
                    CHECK(info.source == q && targets_are(&info, c));
                    CHECK(info.type == TA_SCXML_TRANSITION_INTERNAL);
                    CHECK(info.condition == TA_EXPR_ID_NONE && info.action_count == 0);
                } else {
                    CHECK(info.event.len == 0); /* eventless */
                    CHECK(info.source == a && targets_are(&info, b));
                }
                break;
            case TA_TRANSITION_INITIAL:
                initial += 1u;
                CHECK(info.event.len == 0 && info.condition == TA_EXPR_ID_NONE);
                CHECK(info.type == TA_SCXML_TRANSITION_EXTERNAL);
                if (info.source == p) {
                    CHECK(targets_are(&info, a) && !info.default_targets && info.action_count == 1);
                } else {
                    CHECK(info.source == q && targets_are(&info, c) && info.default_targets);
                    CHECK(info.action_count == 0);
                }
                break;
            case TA_TRANSITION_BOOTSTRAP:
                bootstrap += 1u;
                CHECK(info.source == TA_STATE_NONE && targets_are(&info, p) && info.default_targets);
                break;
        }
    }
    CHECK(authored == 3 && initial == 2 && bootstrap == 1);
    CHECK(go_now != TA_TRANSITION_NONE);
    ta_transition_info info;
    CHECK(ta_definition_transition(system, &(ta_definition_transition_params){
                                               .definition = definition, .transition = count, .out = &info }) ==
          TA_ERR_NOT_FOUND);
    CHECK(ta_definition_transition(system, &(ta_definition_transition_params){
                                               .definition = definition, .transition = TA_TRANSITION_NONE,
                                               .out = &info }) == TA_ERR_NOT_FOUND);
    return go_now;
}

static ta_action_info action_at(ta_system *system, ta_definition_id definition, ta_action_id action) {
    ta_action_info info = { .kind = TA_SCXML_ACTION_NONE };
    CHECK(ta_definition_action(system, &(ta_definition_action_params){
                                           .definition = definition, .action = action, .out = &info }) == TA_OK);
    return info;
}

/* Where each of the twelve actions sits. */
static void check_actions(ta_system *system, ta_definition_id definition, uint32_t count, ta_transition_id go_now) {
    ta_state_id p = state_named(system, definition, "p");
    ta_state_id a = state_named(system, definition, "a");
    CHECK(count == 12);
    uint32_t logs = 0;
    uint32_t assigns = 0;
    for (ta_action_id id = 0; id < count; id += 1u) {
        ta_action_info info = action_at(system, definition, id);
        switch (info.kind) {
            case TA_SCXML_ACTION_SCRIPT:
                CHECK(info.block == TA_ACTION_IN_SCRIPT && info.owner == TA_STATE_NONE && info.index == 0);
                break;
            case TA_SCXML_ACTION_ASSIGN:
                assigns += 1u;
                CHECK(info.owner == a && info.block_index == 0 && info.index == 0);
                /* The first <onentry>, or the first <invoke>'s <finalize>. */
                CHECK(info.block == TA_ACTION_IN_ONENTRY || info.block == TA_ACTION_IN_FINALIZE);
                break;
            case TA_SCXML_ACTION_IF:
                CHECK(info.block == TA_ACTION_IN_ONENTRY && info.owner == a && info.block_index == 0 &&
                      info.index == 1);
                break;
            case TA_SCXML_ACTION_RAISE:
                CHECK(info.block == TA_ACTION_IN_THEN && info.index == 0);
                CHECK(action_at(system, definition, info.owner).kind == TA_SCXML_ACTION_IF);
                break;
            case TA_SCXML_ACTION_SEND:
                /* The second <onentry>. */
                CHECK(info.block == TA_ACTION_IN_ONENTRY && info.owner == a && info.block_index == 1 &&
                      info.index == 0);
                break;
            case TA_SCXML_ACTION_FOREACH:
                CHECK(info.block == TA_ACTION_IN_ONEXIT && info.owner == a && info.block_index == 0 && info.index == 0);
                break;
            case TA_SCXML_ACTION_CANCEL:
                CHECK(info.block == TA_ACTION_IN_TRANSITION && info.owner == go_now && info.index == 0);
                break;
            case TA_SCXML_ACTION_LOG:
                logs += 1u;
                CHECK(info.index == 0);
                if (info.block == TA_ACTION_IN_TRANSITION) {
                    /* p's <initial>, numbered after the <transition>s. */
                    ta_transition_info owner = transition_at(system, definition, info.owner);
                    CHECK(owner.origin == TA_TRANSITION_INITIAL && owner.source == p);
                } else if (info.block == TA_ACTION_IN_ELSE) {
                    CHECK(action_at(system, definition, info.owner).kind == TA_SCXML_ACTION_IF);
                } else if (info.block == TA_ACTION_IN_FOREACH) {
                    CHECK(action_at(system, definition, info.owner).kind == TA_SCXML_ACTION_FOREACH);
                } else {
                    /* The second <invoke>'s <finalize>. */
                    CHECK(info.block == TA_ACTION_IN_FINALIZE && info.owner == a && info.block_index == 1);
                }
                break;
            default:
                CHECK(!"an action of an unexpected kind");
                break;
        }
    }
    CHECK(logs == 4 && assigns == 2);
    ta_action_info info;
    CHECK(ta_definition_action(system, &(ta_definition_action_params){
                                           .definition = definition, .action = count, .out = &info }) ==
          TA_ERR_NOT_FOUND);
}

/* Every expression comes back as the source text the compiler copied. */
static void check_expressions(ta_system *system, ta_definition_id definition, uint32_t count,
                              ta_transition_id go_now) {
    static const char *const expected[] = { "boot()", "x", "1", "c1", "items", "ready", "y", "2" };
    const uint32_t expected_count = (uint32_t)(sizeof(expected) / sizeof(expected[0]));
    CHECK(count == expected_count);
    uint32_t seen = 0;
    for (ta_expr_id id = 0; id < count; id += 1u) {
        ta_expression expression = { .kind = TA_EXPRESSION_NONE };
        CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                                   .definition = definition, .expression = id,
                                                   .out = &expression }) == TA_OK);
        CHECK(expression.kind == TA_EXPRESSION_SOURCE);
        for (uint32_t index = 0; index < expected_count; index += 1u) {
            seen |= str_is(expression.as.source, expected[index]) ? 1u << index : 0u;
        }
    }
    CHECK(seen == (1u << expected_count) - 1u);
    ta_expression condition = { .kind = TA_EXPRESSION_NONE };
    CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                               .definition = definition,
                                               .expression = transition_at(system, definition, go_now).condition,
                                               .out = &condition }) == TA_OK);
    CHECK(condition.kind == TA_EXPRESSION_SOURCE && str_is(condition.as.source, "ready"));
    CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                               .definition = definition, .expression = count,
                                               .out = &condition }) == TA_ERR_NOT_FOUND);
}

/* A host expression comes back as the host's value; core has no text for it. */
static int host_value;

static ta_result host_retain(void *self, void *value, ta_allocator unused, void **out) {
    (void)self;
    (void)unused;
    *out = value;
    return TA_OK;
}

static void host_release(void *self, void *value, ta_allocator unused) {
    (void)self;
    (void)value;
    (void)unused;
}

static const ta_expression_host_vtable host_vtable = { .retain = host_retain, .release = host_release };

static void check_host_expression(ta_system *system, ta_datamodel model) {
    const ta_scxml_state_spec guarded[] = {
        { .kind = TA_SCXML_STATE_ATOMIC,
          .id = STR("idle"),
          .transitions = (const ta_scxml_transition_spec[]){
              { .kind = TA_SCXML_TRANSITION_EXTERNAL,
                .event = STR("go"),
                .condition = { .kind = TA_EXPRESSION_HOST, .as.host = { .v = &host_vtable, .value = &host_value } },
                .targets = (const ta_str[]){ STR("idle"), {0} } },
              {0} } },
        {0},
    };
    ta_definition_id definition = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("hosted"), .states = guarded }, model,
                                            &definition}) == TA_OK);
    ta_expression expression = { .kind = TA_EXPRESSION_NONE };
    CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                               .definition = definition, .expression = 0, .out = &expression }) ==
          TA_OK);
    CHECK(expression.kind == TA_EXPRESSION_HOST && expression.as.host.value == &host_value);
    CHECK(expression.as.host.v == &host_vtable);
}

static void test_elements_by_id(ta_system *system, counting_model *model) {
    ta_datamodel counting = { .self = model, .v = &counting_vtable };
    ta_definition_id definition = 0;
    CHECK(compile(system, (compile_request){&(ta_scxml_spec){ .name = STR("blocks"), .script = SRC("boot()"),
                                                              .states = every_block },
                                            counting, &definition}) == TA_OK);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(system, definition, &view) == TA_OK);
    ta_transition_id go_now = check_transitions(system, definition, view.transition_count);
    check_actions(system, definition, view.action_count, go_now);
    check_expressions(system, definition, view.expression_count, go_now);
    check_host_expression(system, counting);

    /* Unknown definitions and missing outputs, as for ta_definition_state. */
    ta_transition_info transition;
    ta_action_info action;
    ta_expression expression;
    CHECK(ta_definition_transition(system, &(ta_definition_transition_params){
                                               .definition = definition + 100u, .transition = 0,
                                               .out = &transition }) == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_action(system, &(ta_definition_action_params){
                                           .definition = definition + 100u, .action = 0, .out = &action }) ==
          TA_ERR_NOT_FOUND);
    CHECK(ta_definition_expression(system, &(ta_definition_expression_params){
                                               .definition = definition + 100u, .expression = 0,
                                               .out = &expression }) == TA_ERR_NOT_FOUND);
    CHECK(ta_definition_transition(system, &(ta_definition_transition_params){ .definition = definition }) ==
          TA_ERR_INVALID);
    CHECK(ta_definition_action(system, &(ta_definition_action_params){ .definition = definition }) == TA_ERR_INVALID);
    CHECK(ta_definition_expression(system, &(ta_definition_expression_params){ .definition = definition }) ==
          TA_ERR_INVALID);

    /* Retired with no actors, the definition is freed and its ID is stale. */
    CHECK(ta_system_retire_definition(system, definition) == TA_OK);
    CHECK(ta_definition_action(system, &(ta_definition_action_params){
                                           .definition = definition, .action = 0, .out = &action }) ==
          TA_ERR_NOT_FOUND);
}

int main(void) {
    ta_system *system = NULL;
    if (ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) != TA_OK) {
        fprintf(stderr, "inspection test: ta_system_create failed\n");
        return 1;
    }
    counting_model model = {0};
    test_definition_inspect(system, &model);
    test_find_definition(system, &model);
    test_actor_inspection(system, &model);
    test_current_event_from_the_input(system, &model);
    test_current_event_replaced_by_the_next_inspection(system, &model);
    test_reading_identities_back(system, &model);
    test_elements_by_id(system, &model);
    ta_system_destroy(system);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "inspection test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("inspection test: ok");
    return 0;
}
