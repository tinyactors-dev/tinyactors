/* test: runtime asan */
/*
 * An event's named origin outlives the actor that sent it (ADR 0065). A
 * named sender is destroyed while one of its events is still held: by the
 * envelope of mail it sent that has not been taken yet (ADR 0099), by an
 * asynchronous router that kept the delivery of a send to a session core does
 * not place, as a submitted delayed send the router refused, and as the event
 * a host delivered and then let go of. The holder then reads the origin's
 * name. Built with
 * AddressSanitizer (mise run asan), reading freed memory fails the run; it
 * did, before addresses were shared handles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "run_support.h"

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

static ta_test_allocator heap;
static const ta_allocator allocator = TA_TEST_ALLOCATOR_STATIC(&heap);

/* ------------------------------------------------------------------------
 * A router that keeps what it accepts, as an asynchronous router would.
 */

typedef struct {
    ta_external_event_submit_status answer;
    int accepted;
    ta_external_delivery kept;
    bool holding;
} router_state;

static ta_external_event_submit_status route(void *self, const ta_external_event_submit_params *params) {
    router_state *router = self;
    if (router->answer != TA_EXTERNAL_EVENT_ACCEPTED) {
        return router->answer;
    }
    router->accepted += 1;
    CHECK(!router->holding);
    ta_external_delivery_retain(params->delivery, &router->kept);
    router->holding = true;
    return TA_EXTERNAL_EVENT_ACCEPTED;
}

static const ta_external_event_router_vtable router_vtable = { .submit = route };

/* The name the kept delivery's origin carries, read the way a host reads it. */
static bool kept_origin_is(router_state *router, const char *name) {
    const ta_origin *origin = &router->kept.event.origin;
    if (origin->kind != TA_ORIGIN_ACTOR || origin->as.actor.kind != TA_ACTOR_REF_NAMED) {
        return false;
    }
    ta_str text = ta_address_view(origin->as.actor.as.named).name;
    return text.len == strlen(name) && memcmp(text.ptr, name, text.len) == 0;
}

/* ------------------------------------------------------------------------
 * A two-address directory: the sender and the peer are the only named actors.
 */

typedef struct {
    struct {
        bool used;
        ta_address *address;
        ta_actor_directory_entry entry;
    } slots[2];
} one_directory;

static int dir_find(one_directory *dir, const ta_address *address) {
    for (int index = 0; index < 2; index += 1) {
        if (dir->slots[index].used && dir->slots[index].address == address) {
            return index;
        }
    }
    return -1;
}

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    one_directory *dir = self;
    if (dir_find(dir, params->address) >= 0) {
        return TA_ERR_STALE;
    }
    for (int index = 0; index < 2; index += 1) {
        if (!dir->slots[index].used) {
            dir->slots[index].used = true;
            dir->slots[index].address = params->address;
            dir->slots[index].entry = params->entry;
            return TA_OK;
        }
    }
    return TA_ERR_STALE;
}

static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    one_directory *dir = self;
    int index = dir_find(dir, params->address);
    *params->out = index >= 0 ? dir->slots[index].entry : (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    one_directory *dir = self;
    int index = dir_find(dir, params->address);
    if (index >= 0) {
        dir->slots[index].entry = params->replacement;
    }
    return TA_OK;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    one_directory *dir = self;
    int index = dir_find(dir, params->address);
    if (index >= 0) {
        dir->slots[index].used = false;
    }
    return TA_OK;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* ------------------------------------------------------------------------
 * The sender: on entry, one immediate send to another named actor.
 */

static const ta_scxml_state_spec sender_chart[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_SEND,
                    .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("now") },
                                 .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("ping") },
                                 .target = { .kind = TA_SCXML_STRING_LITERAL,
                                             .as.literal = STR("#_actor_peer/p") },
                                 .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("0s") } } },
                  {0} } },
            {0},
        },
    },
    {0},
};

static ta_system *create_system(router_state *router, one_directory *dir) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    CHECK(ta_system_set_external_event_router(
              system, (ta_external_event_router){ .self = router, .v = &router_vtable }) == TA_OK);
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = dir, .v = &dir_vtable }) == TA_OK);
    return system;
}

/* A passive named actor: the peer the sender mails. */
static const ta_scxml_state_spec peer_chart[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("wait") },
    {0},
};

typedef struct {
    const char *family;
    const char *name;
    const ta_scxml_state_spec *states;
} named_chart;

/* The name lives in its own heap block, so nothing else keeps it. Spawns and does not run. */
static ta_actor_id spawn_named(ta_system *system, const named_chart *chart) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = chart->family, .len = (uint32_t)strlen(chart->family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              system, &(ta_definition_compile_params){
                          .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                    .states = chart->states },
                          .datamodel = ta_null_datamodel(),
                          .allocator = allocator,
                          .out = &definition,
                      }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    size_t length = strlen(chart->name);
    char *name = malloc(length + 1u);
    memcpy(name, chart->name, length + 1u);
    ta_actor_address address = { .family = identity.family, .name = { .ptr = name, .len = (uint32_t)length } };
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .address = &address,
                                                           .out = &actor }) == TA_OK);
    free(name); /* core keeps its own copy of the address */
    return actor;
}

static ta_actor_id spawn_sender(ta_system *system) {
    ta_actor_id actor = spawn_named(system, &(named_chart){ "sender", "sender1", sender_chart });
    test_run_status status = TEST_RUN_ERROR;
    CHECK(test_resume(system, &(test_resume_params){ .actor = actor, .budget = 100, .status = &status }) ==
          TA_OK);
    return actor;
}

/*
 * (a) The sender mails a named peer and is destroyed before the peer takes
 * the mail: the envelope in the run queue holds the event, and so the
 * sender's named origin, which the peer then reads.
 */
static void test_queued_mail_outlives_its_sender(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    one_directory dir = {0};
    ta_system *system = create_system(&router, &dir);
    (void)spawn_named(system, &(named_chart){ "peer", "p", peer_chart });
    ta_actor_id sender = spawn_named(system, &(named_chart){ "sender", "sender1", sender_chart });
    /* Two inputs, one at a time: the peer's bootstrap, then the sender's, which mails the peer. */
    CHECK(test_run_input(system) == TA_OK);
    CHECK(test_run_input(system) == TA_OK);
    CHECK(router.accepted == 0); /* core placed the mail itself */
    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    ta_run_notice notices[8];
    uint32_t count = 0;
    ta_run_status stopped = TA_RUN_IDLE;
    CHECK(ta_run(system, &(ta_run_params){ .budget = 100, .report_taken = true, .status = &stopped,
                                           .notices = notices, .notice_capacity = 8, .notice_count = &count }) ==
          TA_OK);
    bool read = false;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (notices[index].kind == TA_NOTICE_INPUT_TAKEN && notices[index].event != NULL &&
            notices[index].event->origin.kind == TA_ORIGIN_ACTOR &&
            notices[index].event->origin.as.actor.kind == TA_ACTOR_REF_NAMED) {
            ta_str held = ta_address_view(notices[index].event->origin.as.actor.as.named).name;
            CHECK(held.len == 7 && memcmp(held.ptr, "sender1", 7) == 0);
            read = true;
        }
    }
    CHECK(read);
    ta_system_destroy(system);
}

/* (a') A router kept the delivery of a send core does not place; the sender is destroyed; the router reads. */
static void test_kept_delivery_outlives_its_sender(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    one_directory dir = {0};
    ta_system *system = create_system(&router, &dir);
    ta_actor_id sender = spawn_sender(system); /* the peer is not resident: the router gets the send */
    CHECK(router.accepted == 1 && router.holding);
    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    CHECK(kept_origin_is(&router, "sender1"));
    ta_external_delivery_release(&router.kept);
    ta_system_destroy(system);
}

/* (b) A refused send is kept as submitted; the sender is destroyed; the send
 * is dispatched later, and the router reads its origin. */
static void test_submitted_send_outlives_its_sender(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_RETRY };
    one_directory dir = {0};
    ta_system *system = create_system(&router, &dir);
    ta_actor_id sender = spawn_sender(system);
    CHECK(router.accepted == 0);
    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    router.answer = TA_EXTERNAL_EVENT_ACCEPTED;
    uint32_t dispatched = 0;
    bool more = true;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 1 && router.holding);
    CHECK(kept_origin_is(&router, "sender1"));
    ta_external_delivery_release(&router.kept);
    ta_system_destroy(system);
}

/* (c) A host interns an origin, delivers an event from it and lets go of its
 * own reference; the event the actor holds still reads the origin. */
static void test_delivered_origin_outlives_the_host_reference(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    one_directory dir = {0};
    ta_system *system = create_system(&router, &dir);
    ta_actor_id actor = spawn_sender(system);
    ta_external_delivery_release(&router.kept);
    router.holding = false;

    char *family = malloc(5);
    char *name = malloc(8);
    memcpy(family, "host", 5);
    memcpy(name, "client9", 8);
    ta_address *origin = NULL;
    CHECK(ta_address_intern(system, &(ta_actor_address){ .family = { .name = { .ptr = family, .len = 4 } },
                                                         .name = { .ptr = name, .len = 7 } },
                            &origin) == TA_OK);
    free(family);
    free(name);
    ta_atom atom = 0;
    CHECK(ta_system_intern_atom(system, (ta_str)STR("poke"), &atom) == TA_OK);
    ta_event_name_id poke = 0;
    CHECK(ta_event_name_intern(system, &(ta_atom_list){ .atoms = &atom, .count = 1 }, &poke) == TA_OK);
    ta_event event = { .name = poke, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE,
                       .origin = { .kind = TA_ORIGIN_ACTOR,
                                   .as.actor = { .kind = TA_ACTOR_REF_NAMED, .as.named = origin } } };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &event }) == TA_OK);
    ta_address_release(origin);

    /* The envelope holds the event, and so the origin, until the input has run: the TAKEN notice lends it. */
    ta_run_notice notices[4];
    uint32_t count = 0;
    ta_run_status stopped = TA_RUN_IDLE;
    CHECK(ta_run(system, &(ta_run_params){ .budget = 100, .report_taken = true, .status = &stopped,
                                           .notices = notices, .notice_capacity = 4, .notice_count = &count }) ==
          TA_OK);
    bool read = false;
    for (uint32_t index = 0; index < count; index += 1u) {
        if (notices[index].kind == TA_NOTICE_INPUT_TAKEN && notices[index].event != NULL) {
            ta_actor_address held = ta_address_view(notices[index].event->origin.as.actor.as.named);
            CHECK(held.name.len == 7 && memcmp(held.name.ptr, "client9", 7) == 0);
            read = true;
        }
    }
    CHECK(read);
    ta_system_destroy(system);
}

/* Equal addresses are one handle, and the last release frees it. */
static void test_equal_addresses_are_one_handle(void) {
    router_state router = { .answer = TA_EXTERNAL_EVENT_ACCEPTED };
    one_directory dir = {0};
    ta_system *system = create_system(&router, &dir);
    size_t before = heap.stats.live_blocks;
    ta_actor_address text = { .family = { STR("f") }, .name = STR("n") };
    ta_address *first = NULL;
    ta_address *second = NULL;
    CHECK(ta_address_intern(system, &text, &first) == TA_OK);
    char copy[] = "n";
    CHECK(ta_address_intern(system, &(ta_actor_address){ .family = { STR("f") }, .name = { .ptr = copy, .len = 1 } },
                            &second) == TA_OK);
    CHECK(first == second);
    ta_address_release(first);
    ta_address_release(second);
    CHECK(heap.stats.live_blocks == before + 4); /* the record is gone; the pool's and the index's first page and directory stay */
    /* Many addresses: the index grows a page at a time and finds them all. */
    enum { MANY = 20000 };
    static ta_address *many[MANY];
    char name[16];
    for (int index = 0; index < MANY; index += 1) {
        int length = snprintf(name, sizeof(name), "a%d", index);
        CHECK(ta_address_intern(system, &(ta_actor_address){ .family = { STR("f") },
                                                             .name = { .ptr = name, .len = (uint32_t)length } },
                                &many[index]) == TA_OK);
    }
    for (int index = 0; index < MANY; index += 1) {
        int length = snprintf(name, sizeof(name), "a%d", index);
        ta_address *found = NULL;
        CHECK(ta_address_intern(system, &(ta_actor_address){ .family = { STR("f") },
                                                             .name = { .ptr = name, .len = (uint32_t)length } },
                                &found) == TA_OK && found == many[index]);
        ta_address_release(found);
        ta_address_release(many[index]);
    }
    ta_system_destroy(system);
}

int main(void) {
    test_queued_mail_outlives_its_sender();
    test_kept_delivery_outlives_its_sender();
    test_submitted_send_outlives_its_sender();
    test_delivered_origin_outlives_the_host_reference();
    test_equal_addresses_are_one_handle();
    CHECK(heap.stats.live_blocks == 0);
    if (failures != 0) {
        fprintf(stderr, "address_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("address_test: passed\n");
    return 0;
}
