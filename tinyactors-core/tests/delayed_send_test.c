/*
 * Delayed sends live with their owner (ADR 0054), exercised through the
 * public header and the amalgamation only: inspection returns an actor's
 * pending sends as a sorted, borrowed array however many sends other actors
 * hold; cancelling an actor's earliest send moves the global deadline; a
 * submitted send outlives its destroyed sender; and an unloaded session
 * takes its sends into the image and gets them back on load.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
 * A counting libc allocator, so teardown can prove nothing leaked.
 */

static long live_blocks;

static void *count_alloc(void *self, size_t size, size_t alignment) {
    (void)self;
    (void)alignment;
    void *block = malloc(size);
    live_blocks += block != NULL ? 1 : 0;
    return block;
}

static void count_free(void *self, const ta_allocator_free_params *params) {
    (void)self;
    if (params->ptr != NULL) {
        live_blocks -= 1;
        free(params->ptr);
    }
}

static const ta_allocator_vtable count_vtable = { .alloc = count_alloc, .free = count_free };
static const ta_allocator allocator = { .self = NULL, .v = &count_vtable };

/* ------------------------------------------------------------------------
 * A data model with no data: these charts evaluate nothing.
 */

static ta_result dm_prepare(void *self, const ta_datamodel_prepare_definition_params *params) {
    (void)self;
    *params->shared = NULL;
    return TA_OK;
}

static void dm_destroy_definition(void *self, void *shared, ta_allocator unused) {
    (void)self;
    (void)shared;
    (void)unused;
}

static uint32_t dm_instance_size(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 0;
}

static uint32_t dm_instance_alignment(void *self, void *shared) {
    (void)self;
    (void)shared;
    return 1;
}

static ta_result dm_init_instance(void *self, const ta_datamodel_init_instance_params *params) {
    (void)self;
    (void)params;
    return TA_OK;
}

static void dm_destroy_instance(void *self, void *shared, void *instance) {
    (void)self;
    (void)shared;
    (void)instance;
}

static const ta_datamodel_vtable dm_vtable = {
    .name = STR("null"),
    .prepare_definition = dm_prepare,
    .destroy_definition = dm_destroy_definition,
    .instance_size = dm_instance_size,
    .instance_alignment = dm_instance_alignment,
    .init_instance = dm_init_instance,
    .destroy_instance = dm_destroy_instance,
};

/* ------------------------------------------------------------------------
 * A router that accepts or refuses, and records what it accepted.
 */

typedef struct {
    ta_external_event_submit_status answer;
    int accepted;
    ta_event_name_id last_event;
    ta_actor_ref last_origin;
} router_state;

static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    router_state *router = self;
    if (router->answer == TA_EXTERNAL_EVENT_ACCEPTED) {
        router->accepted += 1;
        router->last_event = params->delivery->event.name;
        router->last_origin = params->delivery->event.origin.as.actor;
    }
    return router->answer;
}

static const ta_external_event_router_vtable router_vtable = { .submit = route };

/* ------------------------------------------------------------------------
 * A one-address directory, enough for one named actor to unload and load.
 */

typedef struct {
    bool used;
    ta_actor_directory_entry entry;
} one_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    one_directory *dir = self;
    if (dir->used) {
        return TA_ERR_STALE;
    }
    dir->used = true;
    dir->entry = params->entry;
    return TA_OK;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    one_directory *dir = self;
    *params->out = dir->used ? dir->entry : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static bool same_entry(ta_actor_directory_entry left, ta_actor_directory_entry right) {
    return left.kind == right.kind && (left.kind != TA_ACTOR_DIRECTORY_RESIDENT || left.actor == right.actor);
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->entry = params->replacement;
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    one_directory *dir = self;
    if (!dir->used || !same_entry(dir->entry, params->expected)) {
        return TA_ERR_STALE;
    }
    dir->used = false;
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* ------------------------------------------------------------------------
 * Charts
 */

#define SEND(id_text, event_text, delay_text)                                                     \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }

#define CANCEL(id_text)                                                                            \
    { .kind = TA_SCXML_ACTION_CANCEL,                                                              \
      .as.cancel.sendid = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(id_text) } }

/* Three sends scheduled out of due order; `drop` cancels the earliest. */
static const ta_scxml_state_spec three_timers[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("waiting"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){
                  SEND("late", "late", "3s"), SEND("early", "early", "1s"), SEND("middle", "middle", "2s"), {0} } },
            {0},
        },
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("drop"),
              .actions = (const ta_scxml_action_spec[]){ CANCEL("early"), {0} } },
            {0},
        },
    },
    {0},
};

/* One send far in the future: the rest of the population. */
static const ta_scxml_state_spec one_timer[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("waiting"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND("far", "far", "60s"), {0} } },
            {0},
        },
    },
    {0},
};

/* An immediate send: refused by the router, it becomes a submitted send. */
static const ta_scxml_state_spec immediate[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND("now", "ping", "0s"), {0} } },
            {0},
        },
    },
    {0},
};

static ta_definition_id define(ta_system *system, const char *family, const ta_scxml_state_spec *states) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    ta_result result = ta_definition_compile_spec(
        system, &(ta_definition_compile_params){
                    .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity, .states = states },
                    .datamodel = { .self = NULL, .v = &dm_vtable },
                    .allocator = allocator,
                    .out = &definition,
                });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static void run(ta_system *system, ta_actor_id actor) {
    ta_run_status status = TA_RUN_ERROR;
    CHECK(ta_actor_resume(system, &(ta_actor_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    CHECK(status == TA_RUN_STABLE);
}

static ta_actor_id spawn(ta_system *system, ta_definition_id definition, const ta_actor_address *address) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = definition, .address = address,
                                                           .out = &actor }) == TA_OK);
    run(system, actor);
    return actor;
}

static ta_event_name_id event_name(ta_system *system, const char *text) {
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &atom) == TA_OK);
    ta_event_name_id name = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &name) == TA_OK);
    return name;
}

static void deliver(ta_system *system, ta_actor_id actor, const char *event) {
    ta_event delivered = { .name = event_name(system, event), .type = TA_EVENT_EXTERNAL,
                           .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, actor, &delivered) == TA_OK);
    run(system, actor);
}

static ta_system *create_system(router_state *router) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(ta_system_set_external_event_router(
              system, (ta_external_event_router){ .self = router, .v = &router_vtable }) == TA_OK);
    return system;
}

/* ------------------------------------------------------------------------
 * Tests
 */

#define POPULATION 5000u

/*
 * The sends come back sorted by due time, and they are this actor's alone:
 * thousands of other actors holding sends, scheduled before and after this
 * actor's, neither appear in the list nor change it.
 */
static void test_inspection_lists_only_the_actors_sends(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    ta_system *system = create_system(&router);
    ta_definition_id many = define(system, "many", one_timer);
    ta_definition_id three = define(system, "three", three_timers);

    ta_actor_id neighbour = spawn(system, many, NULL);
    for (uint32_t index = 1; index < POPULATION / 2u; index += 1u) {
        (void)spawn(system, many, NULL);
    }
    ta_actor_id actor = spawn(system, three, NULL);
    CHECK(ta_system_set_time(system, 500) == TA_OK);
    for (uint32_t index = 0; index < POPULATION / 2u; index += 1u) {
        (void)spawn(system, many, NULL);
    }

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    CHECK(view.delayed_send_count == 3);
    CHECK(view.delayed_sends != NULL);
    if (view.delayed_send_count == 3 && view.delayed_sends != NULL) {
        CHECK(view.delayed_sends[0].due == 1000 && view.delayed_sends[0].event == event_name(system, "early"));
        CHECK(view.delayed_sends[1].due == 2000 && view.delayed_sends[1].event == event_name(system, "middle"));
        CHECK(view.delayed_sends[2].due == 3000 && view.delayed_sends[2].event == event_name(system, "late"));
        for (uint32_t index = 0; index < 3; index += 1u) {
            CHECK(!view.delayed_sends[index].submitted);
        }
    }

    /* An actor of the population holds exactly its own send. */
    ta_actor_inspection other;
    CHECK(ta_actor_inspect(system, neighbour, &other) == TA_OK && other.delayed_send_count == 1);
    CHECK(other.delayed_send_count != 1 || other.delayed_sends[0].due == 60000);

    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 1000);
    ta_system_destroy(system);
}

/* Cancelling an actor's earliest send moves the global deadline to its next. */
static void test_cancelling_the_front_send(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    ta_system *system = create_system(&router);
    ta_definition_id many = define(system, "many", one_timer);
    ta_definition_id three = define(system, "three", three_timers);
    (void)spawn(system, many, NULL);
    ta_actor_id actor = spawn(system, three, NULL);

    deliver(system, actor, "drop");
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 2);
    if (view.delayed_send_count == 2) {
        CHECK(view.delayed_sends[0].event == event_name(system, "middle"));
        CHECK(view.delayed_sends[1].event == event_name(system, "late"));
    }
    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 2000);

    /* Dispatch follows global (due, send order) across both owners. */
    CHECK(ta_system_set_time(system, 60000) == TA_OK);
    uint32_t dispatched = 0;
    bool more = true;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 3 && !more);
    CHECK(router.accepted == 3 && router.last_event == event_name(system, "far"));
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 0);
    CHECK(ta_system_next_deadline(system, &next) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/*
 * A send the router refused was already sent: destroying its sender keeps it
 * queued for the receiver, with its original envelope, until it is accepted.
 */
static void test_submitted_send_outlives_its_sender(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_RETRY };
    ta_system *system = create_system(&router);
    ta_definition_id definition = define(system, "immediate", immediate);
    ta_actor_id sender = spawn(system, definition, NULL);

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, sender, &view) == TA_OK && view.delayed_send_count == 1);
    if (view.delayed_send_count == 1) {
        CHECK(view.delayed_sends[0].submitted);
        CHECK(view.delayed_sends[0].event == event_name(system, "ping"));
    }

    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 0);

    router.answer = TA_EXTERNAL_EVENT_ACCEPTED;
    uint32_t dispatched = 0;
    bool more = true;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 1 && router.accepted == 1);
    CHECK(router.last_event == event_name(system, "ping"));
    CHECK(router.last_origin.kind == TA_ACTOR_REF_RUNTIME && router.last_origin.as.runtime == sender);
    CHECK(ta_system_next_deadline(system, &next) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/* An orphaned send still pending at teardown is released with the system. */
static void test_teardown_releases_orphans(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_RETRY };
    ta_system *system = create_system(&router);
    ta_definition_id definition = define(system, "immediate", immediate);
    ta_actor_id first = spawn(system, definition, NULL);
    (void)spawn(system, definition, NULL);
    CHECK(ta_actor_destroy(system, first) == TA_OK);
    ta_system_destroy(system);
}

/* Unloading takes the pending sends into the image; loading brings them back. */
static void test_capture_and_load_keep_the_sends(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    ta_system *system = create_system(&router);
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_definition_id three = define(system, "three", three_timers);
    ta_actor_address address = { .family = { STR("three") }, .name = STR("t1") };
    ta_actor_id actor = spawn(system, three, &address);

    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    ta_time image_next = 0;
    CHECK(ta_session_image_next_deadline(image, &image_next) == TA_OK && image_next == 1000);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);
    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_ERR_NOT_FOUND);

    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, loaded, &view) == TA_OK && view.delayed_send_count == 3);
    if (view.delayed_send_count == 3) {
        CHECK(view.delayed_sends[0].due == 1000 && view.delayed_sends[2].due == 3000);
    }
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 1000);
    ta_session_image_destroy(image);
    ta_system_destroy(system);
}

int main(void) {
    test_inspection_lists_only_the_actors_sends();
    test_cancelling_the_front_send();
    test_submitted_send_outlives_its_sender();
    test_teardown_releases_orphans();
    test_capture_and_load_keep_the_sends();
    CHECK(live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "delayed_send_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("delayed_send_test: passed\n");
    return 0;
}
