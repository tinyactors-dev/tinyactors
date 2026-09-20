/*
 * Definition and actor inspection (ADR 0042, work package 1), exercised
 * through the public header and the amalgamation only.
 */
#include <stdio.h>

#include "../include/tinyactors.h"

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
 * A bump allocator over static storage; nothing is returned to it.
 */

static _Alignas(16) unsigned char heap[1u << 20];
static size_t heap_used;

static void *bump_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    size_t at = (heap_used + alignment - 1u) & ~(alignment - 1u);
    if (at + size > sizeof(heap)) {
        return NULL;
    }
    heap_used = at + size;
    return heap + at;
}

static void bump_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    (void)params;
}

static const ta_allocator_vtable bump_vtable = { .alloc = bump_alloc, .free = bump_free };
static const ta_allocator allocator = { .self = NULL, .v = &bump_vtable };

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

    CHECK(compile(system, (compile_request){&spec, counting, &second}) == TA_OK);
    CHECK(second != first);
    CHECK(ta_system_find_definition(system, &probe, &found) == TA_OK);
    CHECK(found == second);

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
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    CHECK(status == TA_RUN_STABLE);

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
    CHECK(ta_system_visit_actors(system, (ta_actor_inspector){ .self = &log, .visit = record_visit }) == TA_OK);
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
    test_reading_identities_back(system, &model);
    ta_system_destroy(system);
    if (failures != 0) {
        fprintf(stderr, "inspection test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("inspection test: ok");
    return 0;
}
