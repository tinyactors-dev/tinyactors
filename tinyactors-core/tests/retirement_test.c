/*
 * Definition retirement and generational definition IDs (ADR 0043, work
 * package 3), exercised through the public header and the amalgamation only.
 * The test uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "tel_reader.h"
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

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

/* ------------------------------------------------------------------------
 * A data model that counts its definition and instance lifecycles.
 */

typedef struct {
    int prepared;
    int destroyed;
    int instances;
    /* Live instances per definition; a definition's shared state points here. */
    int definition_instances[32];
    /* Set when destroy_definition runs while one of its instances is alive. */
    bool destroyed_early;
} counting_model;

static ta_result count_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    counting_model *model = self;
    *params->shared = &model->definition_instances[model->prepared % 32];
    model->prepared += 1;
    return TA_OK;
}

static void count_destroy_definition(void *self, void *shared, ta_allocator unused) {
    counting_model *model = self;
    (void)unused;
    model->destroyed += 1;
    if (*(int *)shared != 0) {
        model->destroyed_early = true;
    }
}

static uint32_t count_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 16;
}

static uint32_t count_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 8;
}

static ta_result count_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    ((counting_model *)self)->instances += 1;
    *(int *)params->shared += 1;
    return TA_OK;
}

static void count_destroy_instance(void *self, void *shared, void *instance) {
    (void)instance;
    ((counting_model *)self)->instances -= 1;
    *(int *)shared -= 1;
}

static const ta_datamodel_vtable counting_vtable = {
    .name = { .ptr = "counting", .len = 8 },
    .prepare_definition = count_prepare,
    .destroy_definition = count_destroy_definition,
    .instance_size = count_instance_size,
    .instance_alignment = count_instance_alignment,
    .init_instance = count_init_instance,
    .destroy_instance = count_destroy_instance,
};

/* ------------------------------------------------------------------------
 * Fixture
 */

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
    test_allocator counts;
    counting_model model;
    ta_allocator allocator;
    ta_system *system;
} fixture;

static void fixture_init(fixture *f) {
    *f = (fixture){0};
    f->allocator = test_allocator_of(&f->counts);
    if (ta_system_create(&(ta_system_config){ .allocator = f->allocator }, &f->system) != TA_OK) {
        fprintf(stderr, "retirement test: ta_system_create failed\n");
        exit(1);
    }
}

static const ta_scxml_state_spec one_state[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle") },
    {0},
};

/* Compiles `states` under an explicit identity (family, revision), and registers it. */
static ta_result register_states(fixture *f, const char *revision, const ta_scxml_state_spec *states,
                                 ta_definition_id *id) {
    uint32_t length = 0;
    while (revision[length] != '\0') {
        length += 1;
    }
    ta_definition_identity identity = { .family = { STR("retiree") },
                                        .revision = { .ptr = (const uint8_t *)revision, .len = length } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(f->system,
                                     &(ta_definition_compile_params){
                                         .spec = &(ta_scxml_spec){ .identity = &identity, .states = states },
                                         .datamodel = { .self = &f->model, .v = &counting_vtable },
                                         .allocator = f->allocator,
                                         .out = &definition,
                                     }) == TA_OK);
    ta_result result = ta_system_register_definition(f->system, definition, id);
    if (result != TA_OK) {
        ta_definition_destroy(definition);
    }
    return result;
}

/* A state that invokes an inline chart: literal <content>, and a literal src. */
static const ta_scxml_state_spec invoking[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .invokes = (const ta_scxml_invoke_spec[]){
            { .kind = TA_SCXML_INVOKE,
              .data = { .kind = TA_SCXML_VALUE_CONTENT,
                        .as.content = { .bytes = { .ptr = (const uint8_t *)"<scxml/>", .len = 8 } } } },
            { .kind = TA_SCXML_INVOKE, .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("http") } },
            {0},
        },
    },
    {0},
};

static uint32_t holds_of(fixture *f, ta_definition_id definition) {
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(f->system, definition, &view) == TA_OK);
    return view.holds;
}

/* Registers `two_states` under an explicit identity (family, revision). */
static ta_definition_id register_chart(fixture *f, const char *revision) {
    uint32_t length = 0;
    while (revision[length] != '\0') {
        length += 1;
    }
    ta_definition_identity identity = { .family = { STR("retiree") },
                                        .revision = { .ptr = (const uint8_t *)revision, .len = length } };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        f->system, &(ta_definition_compile_params){
                       .spec = &(ta_scxml_spec){ .identity = &identity, .states = two_states },
                       .datamodel = { .self = &f->model, .v = &counting_vtable },
                       .allocator = f->allocator,
                       .out = &definition,
                   });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(f->system, definition, &id) == TA_OK);
    CHECK(id != 0);
    return id;
}

static ta_result spawn(fixture *f, ta_definition_id definition, ta_actor_id *out) {
    return ta_actor_spawn(f->system, &(ta_actor_spawn_params){ .definition = definition, .out = out });
}

static test_drive_status run(fixture *f, ta_actor_id actor) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(f->system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    return status;
}

static ta_result find(fixture *f, const char *revision, ta_definition_id *out) {
    uint32_t length = 0;
    while (revision[length] != '\0') {
        length += 1;
    }
    ta_definition_identity identity = { .family = { STR("retiree") },
                                        .revision = { .ptr = (const uint8_t *)revision, .len = length } };
    return ta_system_find_definition(f->system, &identity, out);
}

static ta_result state_of(fixture *f, ta_definition_id definition) {
    ta_state_info info;
    return ta_definition_state(f->system,
                               &(ta_definition_state_params){ .definition = definition, .state = 0, .out = &info });
}

static ta_result export_of(fixture *f, ta_definition_id definition) {
    ta_arena arena;
    CHECK(ta_arena_init(&arena, f->allocator, 1024) == TA_OK);
    const ta_scxml_spec *spec = NULL;
    ta_result result = ta_definition_export(
        f->system, &(ta_definition_export_params){ .definition = definition, .arena = &arena, .out = &spec });
    ta_arena_destroy(&arena);
    return result;
}

/* Every entry point that takes the ID reports it as unknown. */
static void check_gone(fixture *f, ta_definition_id definition) {
    ta_definition_inspection view;
    ta_actor_id actor = 0;
    CHECK(ta_definition_inspect(f->system, definition, &view) == TA_ERR_NOT_FOUND);
    CHECK(state_of(f, definition) == TA_ERR_NOT_FOUND);
    CHECK(export_of(f, definition) == TA_ERR_NOT_FOUND);
    CHECK(spawn(f, definition, &actor) == TA_ERR_NOT_FOUND);
}

/* ------------------------------------------------------------------------
 * Tests
 */

static void test_ids(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id first = register_chart(&f, "a");
    ta_definition_id second = register_chart(&f, "b");
    CHECK((first & 0xffffffffu) != (second & 0xffffffffu));
    CHECK((first >> 32) != 0);

    /* Never issued: slot out of range, generation zero or in the future. */
    check_gone(&f, 0);
    check_gone(&f, first + 100u);
    check_gone(&f, first & 0xffffffffu);
    check_gone(&f, first + ((ta_definition_id)1 << 32));
    CHECK(ta_system_retire_definition(f.system, first + 100u) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_retire_definition(f.system, first + ((ta_definition_id)1 << 32)) == TA_ERR_NOT_FOUND);
    CHECK(ta_system_retire_definition(NULL, first) == TA_ERR_INVALID);
    ta_system_destroy(f.system);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

static void test_retire_without_actors(void) {
    fixture f;
    fixture_init(&f);
    /* Warm up: interns the chart's atoms and grows the slot table once. */
    CHECK(ta_system_retire_definition(f.system, register_chart(&f, "warm")) == TA_OK);
    uint32_t blocks = f.counts.live;
    size_t bytes = f.counts.live_bytes;
    int destroyed = f.model.destroyed;

    ta_definition_id id = register_chart(&f, "warm");
    CHECK(f.counts.live_bytes > bytes);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    /* Freed at once: the adapter released it and every byte came back. */
    CHECK(f.model.destroyed == destroyed + 1);
    CHECK(f.counts.live == blocks);
    CHECK(f.counts.live_bytes == bytes);
    check_gone(&f, id);
    ta_definition_id found = 0;
    CHECK(find(&f, "warm", &found) == TA_ERR_NOT_FOUND);

    /* Idempotent, even after the definition is freed. */
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(f.model.destroyed == destroyed + 1);

    /* The slot is reused under a new generation; the old ID stays stale. */
    ta_definition_id again = register_chart(&f, "warm");
    CHECK((again & 0xffffffffu) == (id & 0xffffffffu));
    CHECK((again >> 32) == (id >> 32) + 1u);
    check_gone(&f, id);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(f.system, again, &view) == TA_OK);
    CHECK(view.id == again && !view.retired);
    CHECK(find(&f, "warm", &found) == TA_OK && found == again);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

static void test_retire_with_actors(void) {
    fixture f;
    fixture_init(&f);
    /* Warm up: also grows the actor table and interns the event name. */
    ta_definition_id warm = register_chart(&f, "warm");
    ta_actor_id warm_actors[2] = {0};
    CHECK(spawn(&f, warm, &warm_actors[0]) == TA_OK);
    CHECK(spawn(&f, warm, &warm_actors[1]) == TA_OK);
    ta_atom warm_segment = 0;
    ta_event_name_id warm_go = 0;
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("go"), &warm_segment) == TA_OK);
    CHECK(ta_event_name_intern(f.system, &(ta_atom_list){ .atoms = &warm_segment, .count = 1 }, &warm_go) ==
          TA_OK);
    /* Running grows the system's scratch arena, which it keeps. */
    CHECK(run(&f, warm_actors[0]) == TEST_DRIVE_STABLE);
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_deliver(f.system,
                             &(ta_actor_deliver_params){ .name = test_event_text(f.system, &(ta_event){ .name = warm_go }), .time = ta_system_time(f.system), .actor = warm_actors[0],
                                                         .event = &(ta_event){ .name = warm_go,
                                                                              .type = TA_EVENT_EXTERNAL } },
                             &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
    CHECK(ta_system_retire_definition(f.system, warm) == TA_OK);
    CHECK(ta_actor_destroy(f.system, warm_actors[0]) == TA_OK);
    CHECK(ta_actor_destroy(f.system, warm_actors[1]) == TA_OK);
    uint32_t blocks = f.counts.live;
    size_t bytes = f.counts.live_bytes;

    ta_definition_id id = register_chart(&f, "warm");
    ta_actor_id first = 0;
    ta_actor_id second = 0;
    CHECK(spawn(&f, id, &first) == TA_OK);
    CHECK(spawn(&f, id, &second) == TA_OK);
    CHECK(run(&f, first) == TEST_DRIVE_STABLE);
    int destroyed = f.model.destroyed;

    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(f.model.destroyed == destroyed);

    /* No new actors; queries keep working. */
    ta_actor_id third = 0;
    CHECK(spawn(&f, id, &third) == TA_ERR_NOT_FOUND);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(f.system, id, &view) == TA_OK);
    CHECK(view.retired);
    CHECK(view.actor_count == 2);
    CHECK(state_of(&f, id) == TA_OK);
    CHECK(export_of(&f, id) == TA_OK);
    ta_definition_id found = 0;
    CHECK(find(&f, "warm", &found) == TA_ERR_NOT_FOUND);

    /* Resident actors keep running. */
    ta_event_name_id go = 0;
    ta_atom segment = 0;
    CHECK(ta_system_intern_atom(f.system, (ta_str)STR("go"), &segment) == TA_OK);
    CHECK(ta_event_name_intern(f.system, &(ta_atom_list){ .atoms = &segment, .count = 1 }, &go) == TA_OK);
    CHECK(test_drive_deliver(f.system,
                             &(ta_actor_deliver_params){ .name = test_event_text(f.system, &(ta_event){ .name = go }), .time = ta_system_time(f.system), .actor = first,
                                                         .event = &(ta_event){ .name = go,
                                                                              .type = TA_EVENT_EXTERNAL } },
                             &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
    CHECK(run(&f, second) == TEST_DRIVE_STABLE);
    ta_actor_inspection actor;
    CHECK(ta_actor_inspect(f.system, first, &actor) == TA_OK);
    CHECK(actor.definition_id == id);

    CHECK(ta_actor_destroy(f.system, first) == TA_OK);
    CHECK(ta_definition_inspect(f.system, id, &view) == TA_OK);
    CHECK(view.retired && view.actor_count == 1);
    CHECK(f.model.destroyed == destroyed);

    /* The last actor frees the definition, after the actor is torn down. */
    CHECK(ta_actor_destroy(f.system, second) == TA_OK);
    CHECK(f.model.destroyed == destroyed + 1);
    CHECK(!f.model.destroyed_early);
    CHECK(f.counts.live == blocks);
    CHECK(f.counts.live_bytes == bytes);
    check_gone(&f, id);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

static void test_find_after_retire(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id older = register_chart(&f, "same");
    ta_actor_id actor = 0;
    CHECK(spawn(&f, older, &actor) == TA_OK);
    ta_definition_id found = 0;
    CHECK(find(&f, "same", &found) == TA_OK && found == older);

    /* A retired definition still in use matches nothing: registering the
     * identity again makes a new definition, which is then the answer. */
    CHECK(ta_system_retire_definition(f.system, older) == TA_OK);
    CHECK(find(&f, "same", &found) == TA_ERR_NOT_FOUND);
    ta_definition_id newer = register_chart(&f, "same");
    CHECK(newer != older);
    CHECK(find(&f, "same", &found) == TA_OK && found == newer);

    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    check_gone(&f, older);
    CHECK(ta_system_retire_definition(f.system, newer) == TA_OK);
    CHECK(find(&f, "same", &found) == TA_ERR_NOT_FOUND);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* ADR 0077 rule 1: registering a live identity again answers the same definition. */
static void test_register_is_idempotent(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id first = register_chart(&f, "same");
    uint32_t blocks = f.counts.live;
    ta_definition_id again = register_chart(&f, "same");
    CHECK(again == first);
    CHECK(holds_of(&f, first) == 2);
    /* The second compile was destroyed: nothing new is kept. */
    CHECK(f.counts.live == blocks);

    /* The same explicit revision for other content is refused, not answered. */
    ta_definition_id other = 0;
    CHECK(register_states(&f, "same", one_state, &other) == TA_ERR_DEFINITION_MISMATCH);
    CHECK(holds_of(&f, first) == 2);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* ------------------------------------------------------------------------
 * Host expressions compared by handle (ADR 0087)
 */

/* An interning host: retain returns the value it was given, and counts. */
static int handle_a;
static int handle_b;
static uint32_t interned_holds;

static ta_result interned_retain(void *self, void *value, ta_allocator unused, void **out) {
    (void)self;
    (void)unused;
    interned_holds += 1u;
    *out = value;
    return TA_OK;
}

static void interned_release(void *self, void *value, ta_allocator unused) {
    (void)self;
    (void)value;
    (void)unused;
    interned_holds -= 1u;
}

static const ta_expression_host_vtable interned_vtable = { .retain = interned_retain, .release = interned_release };

/* A host whose retain returns a new value each time. */
static int fresh_values[8];
static uint32_t fresh_next;

static ta_result fresh_retain(void *self, void *value, ta_allocator unused, void **out) {
    (void)self;
    (void)value;
    (void)unused;
    *out = &fresh_values[fresh_next++ % 8u];
    return TA_OK;
}

static void fresh_release(void *self, void *value, ta_allocator unused) {
    (void)self;
    (void)value;
    (void)unused;
}

static const ta_expression_host_vtable fresh_vtable = { .retain = fresh_retain, .release = fresh_release };

typedef struct {
    const char *revision;       /* NULL: fingerprinted */
    ta_expression condition;
    bool second_transition;     /* another structure */
} hosted_chart;

/* Compiles a chart guarded by `chart->condition` and registers it. */
static ta_result register_hosted(fixture *f, const hosted_chart *chart, ta_definition_id *id) {
    const ta_scxml_transition_spec one[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .condition = chart->condition,
          .targets = (const ta_str[]){ STR("idle"), {0} } },
        {0},
    };
    const ta_scxml_transition_spec two[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("go"), .condition = chart->condition,
          .targets = (const ta_str[]){ STR("idle"), {0} } },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("stop"), .targets = (const ta_str[]){ STR("idle"), {0} } },
        {0},
    };
    const ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("idle"), .transitions = chart->second_transition ? two : one },
        {0},
    };
    uint32_t length = 0;
    while (chart->revision != NULL && chart->revision[length] != '\0') {
        length += 1;
    }
    ta_definition_identity identity = { .family = { STR("hosted") },
                                        .revision = { .ptr = (const uint8_t *)chart->revision, .len = length } };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(f->system,
                                     &(ta_definition_compile_params){
                                         .spec = &(ta_scxml_spec){ .name = STR("hosted"),
                                                                   .identity = chart->revision != NULL ? &identity : NULL,
                                                                   .states = states },
                                         .datamodel = { .self = &f->model, .v = &counting_vtable },
                                         .allocator = f->allocator,
                                         .out = &definition,
                                     }) == TA_OK);
    ta_result result = ta_system_register_definition(f->system, definition, id);
    if (result != TA_OK) {
        ta_definition_destroy(definition);
    }
    return result;
}

static ta_expression host(const ta_expression_host_vtable *v, void *value) {
    return (ta_expression){ .kind = TA_EXPRESSION_HOST, .as.host = { .v = v, .value = value } };
}

static void test_handles(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id first = 0;
    ta_definition_id again = 0;
    ta_definition_id second = 0;
    ta_definition_id third = 0;

    /* The same handles answer the live definition, which gains a hold. */
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_a), false }, &first) == TA_OK);
    uint32_t blocks = f.counts.live;
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_a), false }, &again) == TA_OK);
    CHECK(again == first && holds_of(&f, first) == 2);
    CHECK(f.counts.live == blocks);
    CHECK(interned_holds == 1u); /* the answered compile released its handle */

    /* Another handle registers beside; a third with its handle is answered by it. */
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_b), false }, &second) == TA_OK);
    CHECK(second != first && holds_of(&f, second) == 1);
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_b), false }, &third) == TA_OK);
    CHECK(third == second && holds_of(&f, second) == 2 && holds_of(&f, first) == 2);

    /* Another structure under the same identity is refused, host expressions or not. */
    ta_definition_id refused = 0;
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_a), true }, &refused) ==
          TA_ERR_DEFINITION_MISMATCH);
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", { .kind = TA_EXPRESSION_SOURCE, .as.source = STR("ready") },
                                               false },
                          &refused) == TA_ERR_DEFINITION_MISMATCH);
    CHECK(interned_holds == 2u);

    /* The newest match answers find(); retiring it leaves the other to answer. */
    ta_definition_identity identity = { .family = { STR("hosted") }, .revision = { (const uint8_t *)"v1", 2 } };
    ta_definition_id found = 0;
    CHECK(ta_system_find_definition(f.system, &identity, &found) == TA_OK && found == second);
    CHECK(ta_system_retire_definition(f.system, second) == TA_OK);
    check_gone(&f, second);
    CHECK(ta_system_find_definition(f.system, &identity, &found) == TA_OK && found == first);
    /* With the handle of the retired one, a new definition registers beside the first. */
    ta_definition_id fourth = 0;
    CHECK(register_hosted(&f, &(hosted_chart){ "v1", host(&interned_vtable, &handle_b), false }, &fourth) == TA_OK);
    CHECK(fourth != first && fourth != second);
    CHECK(ta_system_retire_definition(f.system, fourth) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, first) == TA_OK);
    check_gone(&f, first);
    CHECK(interned_holds == 0u);

    /* A host whose retain returns new values never matches. */
    ta_definition_id fresh_first = 0;
    ta_definition_id fresh_again = 0;
    CHECK(register_hosted(&f, &(hosted_chart){ "v2", host(&fresh_vtable, &handle_a), false }, &fresh_first) == TA_OK);
    CHECK(register_hosted(&f, &(hosted_chart){ "v2", host(&fresh_vtable, &handle_a), false }, &fresh_again) == TA_OK);
    CHECK(fresh_again != fresh_first);

    /* The fingerprint marks where a host expression is, not what it holds. */
    ta_definition_id printed_a = 0;
    ta_definition_id printed_b = 0;
    ta_definition_id printed_source = 0;
    CHECK(register_hosted(&f, &(hosted_chart){ NULL, host(&interned_vtable, &handle_a), false }, &printed_a) == TA_OK);
    CHECK(register_hosted(&f, &(hosted_chart){ NULL, host(&interned_vtable, &handle_b), false }, &printed_b) == TA_OK);
    CHECK(register_hosted(&f, &(hosted_chart){ NULL, { .kind = TA_EXPRESSION_SOURCE, .as.source = STR("ready") },
                                               false },
                          &printed_source) == TA_OK);
    ta_definition_inspection a;
    ta_definition_inspection b;
    ta_definition_inspection source;
    CHECK(ta_definition_inspect(f.system, printed_a, &a) == TA_OK);
    CHECK(ta_definition_inspect(f.system, printed_b, &b) == TA_OK);
    CHECK(ta_definition_inspect(f.system, printed_source, &source) == TA_OK);
    CHECK(printed_a != printed_b);
    CHECK(a.identity.revision.len == b.identity.revision.len &&
          memcmp(a.identity.revision.ptr, b.identity.revision.ptr, a.identity.revision.len) == 0);
    CHECK(a.identity.revision.len != source.identity.revision.len ||
          memcmp(a.identity.revision.ptr, source.identity.revision.ptr, a.identity.revision.len) != 0);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
    CHECK(interned_holds == 0u);
}

/* ADR 0077: a parent holds the definitions it nests, and frees them with it. */
static void test_nesting(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id parent = 0;
    CHECK(register_states(&f, "parent", invoking, &parent) == TA_OK);
    ta_definition_id child = register_chart(&f, "child");

    ta_invoke_info info;
    CHECK(ta_definition_invoke(f.system, &(ta_definition_invoke_params){ .definition = parent, .invoke = 0,
                                                                        .out = &info }) == TA_OK);
    CHECK(info.content != TA_CONTENT_ID_NONE && info.src == TA_CONTENT_ID_NONE && info.nested == 0);
    CHECK(!info.has_type_expr && info.type.len == 27);
    CHECK(ta_definition_invoke(f.system, &(ta_definition_invoke_params){ .definition = parent, .invoke = 1,
                                                                        .out = &info }) == TA_OK);
    CHECK(info.content == TA_CONTENT_ID_NONE && info.type.len == 4);
    CHECK(ta_definition_invoke(f.system, &(ta_definition_invoke_params){ .definition = parent, .invoke = 2,
                                                                        .out = &info }) == TA_ERR_NOT_FOUND);

    ta_definition_nest_params nest = { .parent = parent, .invoke = 0, .child = child };
    CHECK(ta_definition_nest(f.system, &nest) == TA_OK);
    CHECK(ta_definition_nest(f.system, &nest) == TA_ERR_INVALID);
    CHECK(ta_definition_nest(f.system, &(ta_definition_nest_params){ .parent = parent, .invoke = 2,
                                                                   .child = child }) == TA_ERR_INVALID);
    CHECK(ta_definition_invoke(f.system, &(ta_definition_invoke_params){ .definition = parent, .invoke = 0,
                                                                        .out = &info }) == TA_OK);
    CHECK(info.nested == child);

    /* Released by its registrar, the child lives on while its parent nests it. */
    CHECK(ta_system_release_definition(f.system, child) == TA_OK);
    ta_definition_inspection view;
    CHECK(ta_definition_inspect(f.system, child, &view) == TA_OK && view.holds == 0 && view.holders == 1);
    CHECK(ta_system_retire_definition(f.system, parent) == TA_OK);
    check_gone(&f, parent);
    check_gone(&f, child);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* ADR 0077 rule 2: a definition nothing holds is freed after its last actor. */
static void test_release(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id id = register_chart(&f, "held");
    CHECK(register_chart(&f, "held") == id);
    CHECK(ta_system_release_definition(f.system, id) == TA_OK);
    CHECK(holds_of(&f, id) == 1);

    ta_actor_id actor = 0;
    CHECK(spawn(&f, id, &actor) == TA_OK);
    CHECK(ta_system_release_definition(f.system, id) == TA_OK);
    /* Released, not retired: still matched by identity and spawnable. */
    ta_definition_id found = 0;
    CHECK(find(&f, "held", &found) == TA_OK && found == id);
    ta_actor_id second = 0;
    CHECK(spawn(&f, id, &second) == TA_OK);
    CHECK(ta_system_release_definition(f.system, id) == TA_ERR_INVALID);

    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    CHECK(holds_of(&f, id) == 0);
    CHECK(ta_actor_destroy(f.system, second) == TA_OK);
    check_gone(&f, id);
    /* Every definition compiled so far is destroyed, the duplicate at registration. */
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(ta_system_release_definition(f.system, id) == TA_ERR_NOT_FOUND);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* Walks every held definition, `page` IDs at a time (ADR 0078). */
static uint32_t walk(fixture *f, uint32_t page, ta_definition_id *out, uint32_t capacity) {
    uint32_t total = 0;
    uint32_t from = 0;
    for (;;) {
        ta_definition_id ids[8];
        uint32_t count = 0;
        CHECK(ta_system_definitions(f->system, &(ta_system_definitions_params){
                                                   .from = from, .limit = page, .definitions = ids,
                                                   .capacity = 8, .count = &count }) == TA_OK);
        for (uint32_t at = 0; at < count && total < capacity; at += 1) {
            out[total++] = ids[at];
        }
        if (count < page || count == 0) {
            return total;
        }
        from = (uint32_t)(ids[count - 1u] & 0xffffffffu) + 1u;
    }
}

static bool listed(const ta_definition_id *ids, uint32_t count, ta_definition_id id) {
    uint32_t seen = 0;
    for (uint32_t at = 0; at < count; at += 1) {
        seen += ids[at] == id ? 1u : 0u;
    }
    return seen == 1;
}

/* ADR 0078: the walk lists every held definition once, retired ones in use too. */
static void test_walk(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id empty[1];
    CHECK(walk(&f, 2, empty, 1) == 0);

    ta_definition_id a = register_chart(&f, "a");
    ta_definition_id b = register_chart(&f, "b");
    ta_definition_id c = register_chart(&f, "c");
    ta_definition_id ids[16];
    uint32_t count = walk(&f, 2, ids, 16);
    CHECK(count == 3 && listed(ids, count, a) && listed(ids, count, b) && listed(ids, count, c));
    CHECK(walk(&f, 0, ids, 16) == 3);

    /* A size query: no buffer, TA_ERR_NOMEM and the page's count. */
    uint32_t needed = 0;
    CHECK(ta_system_definitions(f.system, &(ta_system_definitions_params){ .count = &needed }) == TA_ERR_NOMEM);
    CHECK(needed == 3);
    CHECK(ta_system_definitions(NULL, &(ta_system_definitions_params){ .count = &needed }) == TA_ERR_INVALID);
    CHECK(ta_system_definitions(f.system, &(ta_system_definitions_params){ .from = 99, .count = &needed }) == TA_OK);
    CHECK(needed == 0);

    /* Changes between pages: b is freed, d registers; a and c, held throughout, are listed once. */
    ta_definition_id page[8];
    uint32_t first = 0;
    CHECK(ta_system_definitions(f.system, &(ta_system_definitions_params){ .limit = 1, .definitions = page,
                                                                         .capacity = 8, .count = &first }) == TA_OK);
    CHECK(first == 1 && page[0] == a);
    CHECK(ta_system_retire_definition(f.system, b) == TA_OK);
    ta_definition_id d = register_chart(&f, "d");
    uint32_t rest = 0;
    CHECK(ta_system_definitions(f.system, &(ta_system_definitions_params){ .from = (uint32_t)(a & 0xffffffffu) + 1u,
                                                                         .definitions = page, .capacity = 8,
                                                                         .count = &rest }) == TA_OK);
    CHECK(listed(page, rest, c) && listed(page, rest, d) && !listed(page, rest, b));

    /* Retired but in use: listed until its last actor goes. */
    ta_actor_id actor = 0;
    CHECK(spawn(&f, c, &actor) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, c) == TA_OK);
    count = walk(&f, 2, ids, 16);
    CHECK(listed(ids, count, c));
    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    count = walk(&f, 2, ids, 16);
    CHECK(!listed(ids, count, c) && listed(ids, count, a) && listed(ids, count, d));

    /* A nested child is listed, and its parent names it. */
    ta_definition_id parent = 0;
    CHECK(register_states(&f, "parent", invoking, &parent) == TA_OK);
    ta_definition_id child = register_chart(&f, "child");
    CHECK(ta_definition_nest(f.system, &(ta_definition_nest_params){ .parent = parent, .invoke = 0,
                                                                   .child = child }) == TA_OK);
    CHECK(ta_system_release_definition(f.system, child) == TA_OK);
    count = walk(&f, 3, ids, 16);
    CHECK(listed(ids, count, parent) && listed(ids, count, child));
    ta_invoke_info info;
    CHECK(ta_definition_invoke(f.system, &(ta_definition_invoke_params){ .definition = parent, .invoke = 0,
                                                                        .out = &info }) == TA_OK);
    CHECK(info.nested == child);

    ta_system_destroy(f.system);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

static void test_system_destroy(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id live = register_chart(&f, "live");
    ta_definition_id retired_busy = register_chart(&f, "busy");
    ta_definition_id retired_free = register_chart(&f, "free");
    (void)register_chart(&f, "idle");
    ta_actor_id actor = 0;
    CHECK(spawn(&f, live, &actor) == TA_OK);
    CHECK(spawn(&f, retired_busy, &actor) == TA_OK);
    CHECK(spawn(&f, retired_busy, &actor) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, retired_busy) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, retired_free) == TA_OK);
    CHECK(f.model.destroyed == 1);

    /* Each definition is freed exactly once, after its actors. */
    ta_system_destroy(f.system);
    CHECK(f.model.prepared == 4);
    CHECK(f.model.destroyed == 4);
    CHECK(!f.model.destroyed_early);
    CHECK(f.model.instances == 0);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* ------------------------------------------------------------------------
 * The definition sink (ADR 0079)
 */

typedef struct {
    ta_system *system;
    ta_definition_event_kind kinds[32];
    ta_definition_id ids[32];
    uint32_t count;
    /* Every event found its definition intact: inspectable, its identity its own. */
    bool intact;
} recording_sink;

static void record_definition(void *self, const ta_definition_event *event) {
    recording_sink *sink = self;
    ta_definition_inspection view;
    if (ta_definition_inspect(sink->system, event->definition, &view) != TA_OK ||
        view.identity.revision.ptr != event->identity.revision.ptr) {
        sink->intact = false;
    }
    if (sink->count < 32u) {
        sink->kinds[sink->count] = event->kind;
        sink->ids[sink->count] = event->definition;
        sink->count += 1u;
    }
}

static const ta_definition_sink_vtable recording_vtable = { .notify = record_definition };

static void install(fixture *f, recording_sink *sink) {
    *sink = (recording_sink){ .system = f->system, .intact = true };
    CHECK(ta_system_set_definition_sink(f->system, (ta_definition_sink){ .self = sink, .v = &recording_vtable }) ==
          TA_OK);
}

static bool saw(const recording_sink *sink, uint32_t at, ta_definition_event_kind kind, ta_definition_id id) {
    return at < sink->count && sink->kinds[at] == kind && sink->ids[at] == id;
}

static void test_definition_sink(void) {
    fixture f;
    fixture_init(&f);
    /* The null sink is installed; a sink without notify is refused. */
    CHECK(ta_system_set_definition_sink(f.system, (ta_definition_sink){0}) == TA_ERR_INVALID);
    static const ta_definition_sink_vtable empty = {0};
    CHECK(ta_system_set_definition_sink(f.system, (ta_definition_sink){ .v = &empty }) == TA_ERR_INVALID);
    CHECK(ta_system_set_definition_sink(NULL, ta_null_definition_sink()) == TA_ERR_INVALID);
    CHECK(ta_system_set_definition_sink(f.system, ta_null_definition_sink()) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, register_chart(&f, "unheard")) == TA_OK);

    recording_sink sink;
    install(&f, &sink);

    /* REGISTERED once; a reuse by identity is a hold, not a registration. */
    ta_definition_id id = register_chart(&f, "told");
    CHECK(sink.count == 1 && saw(&sink, 0, TA_DEFINITION_EVENT_REGISTERED, id));
    CHECK(register_chart(&f, "told") == id);
    CHECK(sink.count == 1);

    /* Releasing a hold frees nothing while a hold or an actor is left. */
    CHECK(ta_system_release_definition(f.system, id) == TA_OK);
    ta_actor_id actor = 0;
    CHECK(spawn(&f, id, &actor) == TA_OK);
    CHECK(ta_system_release_definition(f.system, id) == TA_OK);
    CHECK(sink.count == 1);

    /* RETIRED once; FREED when the last actor goes, and not before. */
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(sink.count == 2 && saw(&sink, 1, TA_DEFINITION_EVENT_RETIRED, id));
    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    CHECK(sink.count == 3 && saw(&sink, 2, TA_DEFINITION_EVENT_FREED, id));
    check_gone(&f, id);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(sink.count == 3);

    /* A cascade frees leaves first: grandchild, child, then parent. */
    ta_definition_id parent = 0;
    ta_definition_id child = 0;
    CHECK(register_states(&f, "cascade-parent", invoking, &parent) == TA_OK);
    CHECK(register_states(&f, "cascade-child", invoking, &child) == TA_OK);
    ta_definition_id grandchild = register_chart(&f, "cascade-grandchild");
    CHECK(ta_definition_nest(f.system, &(ta_definition_nest_params){ .parent = child, .invoke = 0,
                                                                   .child = grandchild }) == TA_OK);
    CHECK(ta_definition_nest(f.system, &(ta_definition_nest_params){ .parent = parent, .invoke = 0,
                                                                   .child = child }) == TA_OK);
    CHECK(ta_system_release_definition(f.system, grandchild) == TA_OK);
    CHECK(ta_system_release_definition(f.system, child) == TA_OK);
    uint32_t before = sink.count;
    CHECK(ta_system_release_definition(f.system, parent) == TA_OK);
    CHECK(sink.count == before + 3u);
    CHECK(saw(&sink, before, TA_DEFINITION_EVENT_FREED, grandchild));
    CHECK(saw(&sink, before + 1u, TA_DEFINITION_EVENT_FREED, child));
    CHECK(saw(&sink, before + 2u, TA_DEFINITION_EVENT_FREED, parent));
    check_gone(&f, parent);
    check_gone(&f, child);
    check_gone(&f, grandchild);

    /* Destroy tells of every definition still alive, retired or not. */
    ta_definition_id live = register_chart(&f, "left-live");
    ta_definition_id busy = register_chart(&f, "left-busy");
    CHECK(spawn(&f, busy, &actor) == TA_OK);
    CHECK(ta_system_retire_definition(f.system, busy) == TA_OK);
    before = sink.count;
    ta_system_destroy(f.system);
    uint32_t freed_live = 0;
    uint32_t freed_busy = 0;
    for (uint32_t at = before; at < sink.count; at += 1u) {
        freed_live += saw(&sink, at, TA_DEFINITION_EVENT_FREED, live) ? 1u : 0u;
        freed_busy += saw(&sink, at, TA_DEFINITION_EVENT_FREED, busy) ? 1u : 0u;
    }
    CHECK(freed_live == 1 && freed_busy == 1);
    CHECK(sink.count == before + 2u);
    CHECK(sink.intact);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);
}

/* ------------------------------------------------------------------------
 * Definition records in the trace stream (ADR 0080)
 */

/* One stream both sinks write to, so their order against each other shows. */
typedef struct {
    char source;          /* 't': a trace record, 'd': the definition sink */
    int kind;             /* ta_trace_kind for 't', ta_definition_event_kind for 'd' */
    ta_definition_id definition;
    ta_step_key key;
    bool identity_matches; /* 't': the record's identity is the definition's own */
    uint32_t live_watched; /* 'd': how many of the stream's watched actors still exist */
} stream_entry;

typedef struct {
    ta_system *system;
    stream_entry entries[64];
    uint32_t count;
    /* Actors whose existence each definition event notes. */
    ta_actor_id watched[2];
} stream;

static void stream_put(stream *log, stream_entry entry) {
    if (log->count < 64u) {
        log->entries[log->count] = entry;
        log->count += 1u;
    }
}

static void stream_trace(void *self, const ta_trace_record *record) {
    stream *log = self;
    bool matches = true;
    if (record->kind >= TA_TRACE_DEFINITION_REGISTERED) {
        ta_definition_inspection view;
        ta_str family = ta_trace_record_text(record, TA_TRACE_TEXT_FAMILY);
        matches = ta_definition_inspect(log->system, record->definition, &view) == TA_OK &&
                  view.identity.revision.len == record->revision.len &&
                  memcmp(view.identity.revision.ptr, record->revision.ptr, view.identity.revision.len) == 0 &&
                  view.identity.family.name.len == family.len &&
                  memcmp(view.identity.family.name.ptr, family.ptr, view.identity.family.name.len) == 0;
    }
    stream_put(log, (stream_entry){ .source = 't', .kind = (int)record->kind, .definition = record->definition,
                                    .key = record->key, .identity_matches = matches });
}

/* The records written before an event come first: read them, then the event. */
static void stream_definition(void *self, const ta_definition_event *event) {
    stream *log = self;
    tel_drain(log->system, stream_trace, log);
    uint32_t live = 0;
    for (uint32_t at = 0; at < 2u; at += 1u) {
        ta_actor_inspection view;
        live += log->watched[at] != 0 && ta_actor_inspect(log->system, log->watched[at], &view) == TA_OK ? 1u : 0u;
    }
    stream_put(log, (stream_entry){ .source = 'd', .kind = (int)event->kind, .definition = event->definition,
                                    .live_watched = live });
}

/* The entries so far, after reading what the system wrote to tel. */
static uint32_t logged(stream *log) {
    tel_drain(log->system, stream_trace, log);
    return log->count;
}
static const ta_definition_sink_vtable stream_definition_vtable = { .notify = stream_definition };

#define DEFINITION_KINDS                                                                       \
    (TA_TRACE_BIT(TA_TRACE_DEFINITION_REGISTERED) | TA_TRACE_BIT(TA_TRACE_DEFINITION_RETIRED) | \
     TA_TRACE_BIT(TA_TRACE_DEFINITION_FREED))
#define ACTOR_KINDS (TA_TRACE_BIT(TA_TRACE_SPAWNED) | TA_TRACE_BIT(TA_TRACE_DESTROYED))

static void follow(fixture *f, stream *log, uint32_t kinds) {
    *log = (stream){ .system = f->system };
    CHECK(ta_system_set_trace_masks(f->system, &(ta_trace_masks){ .kinds = kinds }) == TA_OK);
    CHECK(ta_system_set_definition_sink(f->system, (ta_definition_sink){ .self = log,
                                                                         .v = &stream_definition_vtable }) == TA_OK);
}

/* A trace record of `kind` for `definition` at `at`, with its ADR 0080 key. */
static bool is_record(const stream *log, uint32_t at, ta_trace_kind kind, ta_definition_id definition) {
    if (at >= log->count) {
        return false;
    }
    const stream_entry *entry = &log->entries[at];
    if (entry->source != 't' || entry->kind != (int)kind || entry->definition != definition) {
        return false;
    }
    if (kind < TA_TRACE_DEFINITION_REGISTERED) {
        return true;
    }
    uint32_t ordinal = (uint32_t)(kind - TA_TRACE_DEFINITION_REGISTERED) + 1u;
    return entry->identity_matches && entry->key.session == TA_TRACE_DEFINITIONS_SESSION &&
           entry->key.macrostep == definition && entry->key.microstep == ordinal;
}

static bool is_event(const stream *log, uint32_t at, ta_definition_event_kind kind, ta_definition_id definition) {
    return at < log->count && log->entries[at].source == 'd' && log->entries[at].kind == (int)kind &&
           log->entries[at].definition == definition;
}

static void test_definition_records(void) {
    fixture f;
    fixture_init(&f);
    stream log;
    follow(&f, &log, DEFINITION_KINDS | ACTOR_KINDS);

    /* REGISTERED, just before the sink hears it; a reuse writes nothing. */
    ta_definition_id id = register_chart(&f, "followed");
    CHECK(logged(&log) == 2);
    CHECK(is_record(&log, 0, TA_TRACE_DEFINITION_REGISTERED, id));
    CHECK(is_event(&log, 1, TA_DEFINITION_EVENT_REGISTERED, id));
    CHECK(register_chart(&f, "followed") == id);
    CHECK(logged(&log) == 2);

    /* Before the first SPAWNED of the definition. */
    ta_actor_id actor = 0;
    CHECK(spawn(&f, id, &actor) == TA_OK);
    CHECK(logged(&log) == 3 && is_record(&log, 2, TA_TRACE_SPAWNED, id));

    /* RETIRED from the retiring call; FREED after the last DESTROYED. */
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    CHECK(logged(&log) == 5);
    CHECK(is_record(&log, 3, TA_TRACE_DEFINITION_RETIRED, id));
    CHECK(is_event(&log, 4, TA_DEFINITION_EVENT_RETIRED, id));
    CHECK(ta_actor_destroy(f.system, actor) == TA_OK);
    CHECK(logged(&log) == 8);
    CHECK(is_record(&log, 5, TA_TRACE_DESTROYED, id));
    CHECK(is_record(&log, 6, TA_TRACE_DEFINITION_FREED, id));
    CHECK(is_event(&log, 7, TA_DEFINITION_EVENT_FREED, id));

    /* A cascade: each record before its event, leaves first. */
    ta_definition_id parent = 0;
    CHECK(register_states(&f, "followed-parent", invoking, &parent) == TA_OK);
    ta_definition_id child = register_chart(&f, "followed-child");
    CHECK(ta_definition_nest(f.system, &(ta_definition_nest_params){ .parent = parent, .invoke = 0,
                                                                   .child = child }) == TA_OK);
    CHECK(ta_system_release_definition(f.system, child) == TA_OK);
    uint32_t before = logged(&log);
    CHECK(ta_system_release_definition(f.system, parent) == TA_OK);
    CHECK(logged(&log) == before + 4u);
    CHECK(is_record(&log, before, TA_TRACE_DEFINITION_FREED, child));
    CHECK(is_event(&log, before + 1u, TA_DEFINITION_EVENT_FREED, child));
    CHECK(is_record(&log, before + 2u, TA_TRACE_DEFINITION_FREED, parent));
    CHECK(is_event(&log, before + 3u, TA_DEFINITION_EVENT_FREED, parent));

    /* A mask without the kinds builds no definition record; the sink still hears. */
    CHECK(ta_system_set_trace_masks(f.system, &(ta_trace_masks){ .kinds = ACTOR_KINDS }) == TA_OK);
    before = logged(&log);
    ta_definition_id unrecorded = register_chart(&f, "unrecorded");
    CHECK(logged(&log) == before + 1u && is_event(&log, before, TA_DEFINITION_EVENT_REGISTERED, unrecorded));
    CHECK(ta_system_set_trace_masks(f.system, &(ta_trace_masks){ .kinds = DEFINITION_KINDS | ACTOR_KINDS }) == TA_OK);

    /* Destroy: every actor goes first, then FREED for each definition left.
     * The ring goes with the system, so teardown writes no record: the
     * definition sink sees that both actors are gone by the first FREED. */
    ta_definition_id busy = register_chart(&f, "left-busy");
    CHECK(spawn(&f, busy, &log.watched[0]) == TA_OK);
    CHECK(spawn(&f, busy, &log.watched[1]) == TA_OK);
    before = logged(&log);
    ta_actor_inspection alive;
    CHECK(ta_actor_inspect(f.system, log.watched[0], &alive) == TA_OK &&
          ta_actor_inspect(f.system, log.watched[1], &alive) == TA_OK);
    ta_system_destroy(f.system);
    uint32_t records = 0;
    uint32_t freed = 0;
    for (uint32_t at = before; at < log.count; at += 1u) {
        const stream_entry *entry = &log.entries[at];
        records += entry->source == 't' ? 1u : 0u;
        if (entry->source == 'd' && entry->kind == (int)TA_DEFINITION_EVENT_FREED) {
            freed += 1u;
            CHECK(entry->live_watched == 0);
        }
    }
    CHECK(records == 0 && freed == 2);
    CHECK(f.counts.live == 0 && f.counts.live_bytes == 0);
    CHECK(test_allocator_finish(&f.counts) == 0);

    /* No definition key is an actor's: no runtime ID ends in slot UINT32_MAX. */
    CHECK((TA_TRACE_DEFINITIONS_SESSION & 0xffffffffu) == 0xffffffffu);
}

int main(void) {
    test_ids();
    test_retire_without_actors();
    test_retire_with_actors();
    test_find_after_retire();
    test_register_is_idempotent();
    test_handles();
    test_release();
    test_nesting();
    test_walk();
    test_system_destroy();
    test_definition_sink();
    test_definition_records();
    if (failures != 0) {
        fprintf(stderr, "retirement test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("retirement test: ok");
    return 0;
}
