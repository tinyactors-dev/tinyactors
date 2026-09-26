/*
 * Definition retirement and generational definition IDs (ADR 0043, work
 * package 3), exercised through the public header and the amalgamation only.
 * The test uses the C library; the core does not.
 */
#include <stdio.h>
#include <stdlib.h>

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

#define STR(text) { .ptr = text, .len = sizeof(text) - 1u }

/* ------------------------------------------------------------------------
 * An allocator that counts live allocations and bytes.
 */

typedef struct {
    long live_blocks;
    long live_bytes;
} counting_allocator;

static void *counted_alloc(void *self, size_t size, size_t alignment) {
    counting_allocator *counts = self;
    if (alignment < sizeof(void *)) {
        alignment = sizeof(void *);
    }
    size_t rounded = (size + alignment - 1u) / alignment * alignment;
    void *block = aligned_alloc(alignment, rounded == 0 ? alignment : rounded);
    if (block != NULL) {
        counts->live_blocks += 1;
        counts->live_bytes += (long)size;
    }
    return block;
}

static void counted_free(void *self, const ta_allocator_free_params *params) {
    counting_allocator *counts = self;
    if (params->ptr == NULL) {
        return;
    }
    counts->live_blocks -= 1;
    counts->live_bytes -= (long)params->size;
    free(params->ptr);
}

static const ta_allocator_vtable counted_vtable = { .alloc = counted_alloc, .free = counted_free };

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
    counting_allocator counts;
    counting_model model;
    ta_allocator allocator;
    ta_system *system;
} fixture;

static void fixture_init(fixture *f) {
    *f = (fixture){0};
    f->allocator = (ta_allocator){ .self = &f->counts, .v = &counted_vtable };
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

static ta_run_status run(fixture *f, ta_actor_id actor) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(f->system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
}

static void test_retire_without_actors(void) {
    fixture f;
    fixture_init(&f);
    /* Warm up: interns the chart's atoms and grows the slot table once. */
    CHECK(ta_system_retire_definition(f.system, register_chart(&f, "warm")) == TA_OK);
    long blocks = f.counts.live_blocks;
    long bytes = f.counts.live_bytes;
    int destroyed = f.model.destroyed;

    ta_definition_id id = register_chart(&f, "warm");
    CHECK(f.counts.live_bytes > bytes);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);
    /* Freed at once: the adapter released it and every byte came back. */
    CHECK(f.model.destroyed == destroyed + 1);
    CHECK(f.counts.live_blocks == blocks);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
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
    CHECK(run(&f, warm_actors[0]) == TA_RUN_STABLE);
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = warm_actors[0], .event = &(ta_event){ .name = warm_go, .type = TA_EVENT_EXTERNAL } }) ==
          TA_OK);
    CHECK(run(&f, warm_actors[0]) == TA_RUN_STABLE);
    CHECK(ta_system_retire_definition(f.system, warm) == TA_OK);
    CHECK(ta_actor_destroy(f.system, warm_actors[0]) == TA_OK);
    CHECK(ta_actor_destroy(f.system, warm_actors[1]) == TA_OK);
    long blocks = f.counts.live_blocks;
    long bytes = f.counts.live_bytes;

    ta_definition_id id = register_chart(&f, "warm");
    ta_actor_id first = 0;
    ta_actor_id second = 0;
    CHECK(spawn(&f, id, &first) == TA_OK);
    CHECK(spawn(&f, id, &second) == TA_OK);
    CHECK(run(&f, first) == TA_RUN_STABLE);
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
    CHECK(ta_actor_deliver(f.system, &(ta_actor_deliver_params){ .actor = first, .event = &(ta_event){ .name = go, .type = TA_EVENT_EXTERNAL } }) == TA_OK);
    CHECK(run(&f, first) == TA_RUN_STABLE);
    CHECK(run(&f, second) == TA_RUN_STABLE);
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
    CHECK(f.counts.live_blocks == blocks);
    CHECK(f.counts.live_bytes == bytes);
    check_gone(&f, id);
    CHECK(ta_system_retire_definition(f.system, id) == TA_OK);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
}

/* ADR 0077 rule 1: registering a live identity again answers the same definition. */
static void test_register_is_idempotent(void) {
    fixture f;
    fixture_init(&f);
    ta_definition_id first = register_chart(&f, "same");
    long blocks = f.counts.live_blocks;
    ta_definition_id again = register_chart(&f, "same");
    CHECK(again == first);
    CHECK(holds_of(&f, first) == 2);
    /* The second compile was destroyed: nothing new is kept. */
    CHECK(f.counts.live_blocks == blocks);

    /* The same explicit revision for other content is refused, not answered. */
    ta_definition_id other = 0;
    CHECK(register_states(&f, "same", one_state, &other) == TA_ERR_DEFINITION_MISMATCH);
    CHECK(holds_of(&f, first) == 2);

    ta_system_destroy(f.system);
    CHECK(f.model.destroyed == f.model.prepared);
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
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
    CHECK(f.counts.live_blocks == 0 && f.counts.live_bytes == 0);
}

int main(void) {
    test_ids();
    test_retire_without_actors();
    test_retire_with_actors();
    test_find_after_retire();
    test_register_is_idempotent();
    test_release();
    test_nesting();
    test_system_destroy();
    if (failures != 0) {
        fprintf(stderr, "retirement test: %d check(s) failed\n", failures);
        return 1;
    }
    puts("retirement test: ok");
    return 0;
}
