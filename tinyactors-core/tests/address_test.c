/*
 * An event's named origin outlives the actor that sent it (ADR 0065). A
 * named sender is destroyed while one of its events is still held: by
 * core, as mail waiting for a target that is not resident, and as a
 * submitted delayed send its I/O processor refused once, and as the event a
 * host delivered and then let go of. The holder then reads the origin's name.
 * Built with AddressSanitizer (mise run asan), reading freed memory fails
 * the run; it did, before addresses were shared handles.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../include/tinyactors.h"
#include "test_allocator.h"
#include "test_drive.h"

/* The names of the TAKEN events of one ta_run() call (ta_run_params.names). */
static char taken_names[TA_RUN_NAMES_MIN_BYTES];

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

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * Runs, and the origin of the mail an actor takes
 */

/* Runs until idle; true when `actor` took mail whose origin is the named
 * sender `name`, read from the TAKEN outcome the way a host reads it. */
static bool took_mail_from(ta_system *system, ta_actor_id actor, const char *name) {
    bool seen = false;
    for (int call = 0; call < 100; call += 1) {
        ta_input_outcome outcomes[2];
        uint32_t count = 0;
        ta_run_status status = TA_RUN_IDLE;
        CHECK(ta_run(system, &(ta_run_params){ .budget = 100, .report_taken = true, .names = taken_names, .name_capacity = sizeof taken_names, .outcomes = outcomes,
                                               .outcome_capacity = 2, .outcome_count = &count,
                                               .status = &status }) == TA_OK);
        for (uint32_t index = 0; index < count; index += 1u) {
            const ta_origin *origin = &outcomes[index].event.origin;
            if (outcomes[index].kind == TA_INPUT_TAKEN && outcomes[index].has_event &&
                outcomes[index].actor == actor && origin->kind == TA_ORIGIN_ACTOR &&
                origin->as.actor.kind == TA_ACTOR_REF_NAMED) {
                ta_str text = ta_address_view(origin->as.actor.as.named).name;
                seen = seen || (text.len == strlen(name) && memcmp(text.ptr, name, text.len) == 0);
            }
            ta_event_release(&outcomes[index].event);
        }
        if (status == TA_RUN_IDLE) {
            break;
        }
    }
    return seen;
}

/* ------------------------------------------------------------------------
 * A directory of a few named actors.
 */

enum { NAMES = 4 };

typedef struct {
    /* The claimed addresses: equal addresses are the same handle (ADR 0065). */
    const ta_address *addresses[NAMES];
    ta_actor_directory_entry entries[NAMES];
    uint32_t count;
} small_directory;

static ta_result dir_claim(void *self, const ta_actor_directory_claim_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            if (dir->entries[index].kind != TA_ACTOR_DIRECTORY_NONE) {
                return TA_ERR_STALE;
            }
            dir->entries[index] = params->entry;
            return TA_OK;
        }
    }
    if (dir->count == NAMES) {
        return TA_ERR_NOMEM;
    }
    dir->addresses[dir->count] = params->address;
    dir->entries[dir->count] = params->entry;
    dir->count += 1u;
    return TA_OK;
}

/* An address no actor claimed has no entry: mail to it waits in core for
 * its load. */
static ta_result dir_resolve(void *self, const ta_actor_directory_resolve_params *params) {
    small_directory *dir = self;
    *params->out = (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            *params->out = dir->entries[index];
        }
    }
    return TA_OK;
}

static ta_result dir_replace(void *self, const ta_actor_directory_replace_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            dir->entries[index] = params->replacement;
            return TA_OK;
        }
    }
    return TA_ERR_STALE;
}

static ta_result dir_remove(void *self, const ta_actor_directory_remove_params *params) {
    small_directory *dir = self;
    for (uint32_t index = 0; index < dir->count; index += 1u) {
        if (dir->addresses[index] == params->address) {
            dir->entries[index] = (ta_actor_directory_entry){ .kind = TA_ACTOR_DIRECTORY_NONE };
            return TA_OK;
        }
    }
    return TA_ERR_STALE;
}

static const ta_actor_directory_vtable dir_vtable = {
    .claim = dir_claim, .resolve = dir_resolve, .replace = dir_replace, .remove = dir_remove,
};

/* ------------------------------------------------------------------------
 * The charts: the sender, on entry, sends `ping` to peer/p at once, and
 * `later` and `last` to it in a second; the spammer mails peer/p until the
 * pool is full; the peer takes anything.
 */

#define SEND_TO_PEER(event_text, delay_text)                                                        \
    { .kind = TA_SCXML_ACTION_SEND,                                                                \
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },           \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("#_actor_peer/p") }, \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }

static const ta_scxml_state_spec sender_chart[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND_TO_PEER("ping", "0s"), SEND_TO_PEER("later", "1s"),
                                                         SEND_TO_PEER("last", "1s"), {0} } },
            {0},
        },
    },
    {0},
};

/* A delayed send to the host I/O processor `slow`. */
static const ta_scxml_state_spec slow_sender_chart[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_SEND,
                    .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("late") },
                                 .type = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("slow") },
                                 .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("1s") } } },
                  {0} } },
            {0},
        },
    },
    {0},
};

/* `slow` refuses its first offer, so the send waits, submitted; it takes
 * the second and reads the name of the send's source. */
static int slow_offers;
static char slow_source[32];

static ta_op_status slow_send(void *self, const ta_send *send, ta_fault *fault) {
    (void)self;
    (void)fault;
    slow_offers += 1;
    if (slow_offers == 1) {
        return TA_OP_NOMEM;
    }
    if (send->source.kind == TA_ACTOR_REF_NAMED) {
        ta_actor_address address = ta_address_view(send->source.as.named);
        size_t length = address.name.len < sizeof slow_source - 1u ? address.name.len : sizeof slow_source - 1u;
        memcpy(slow_source, address.name.ptr, length);
        slow_source[length] = '\0';
    }
    return TA_OP_OK;
}

static const ta_io_vtable slow_vtable = { .send = slow_send };

static const ta_scxml_state_spec peer_chart[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("p") }, {0} };

static ta_system *create_system(small_directory *dir, const ta_param_value *params, uint32_t count) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = params, .param_count = count },
                           &system) == TA_OK);
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = dir, .v = &dir_vtable }) == TA_OK);
    return system;
}

/* Spawns an actor of `chart` at `family`/`name`, whose name lives in its own
 * heap block so nothing else keeps it, and runs its bootstrap. */
static ta_actor_id spawn_at(ta_system *system, const ta_scxml_state_spec *chart, const char *family,
                            const char *name) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(
              system, &(ta_definition_compile_params){
                          .spec = &(ta_scxml_spec){ .datamodel = STR("null"), .identity = &identity,
                                                    .states = chart },
                          .datamodel = ta_null_datamodel(),
                          .allocator = allocator,
                          .out = &definition,
                      }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system, definition, &id) == TA_OK);
    size_t length = strlen(name);
    char *copy = malloc(length + 1u);
    memcpy(copy, name, length + 1u);
    ta_actor_address address = { .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
                                 .name = { .ptr = copy, .len = (uint32_t)length } };
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system, &(ta_actor_spawn_params){ .definition = id, .address = &address,
                                                           .out = &actor }) == TA_OK);
    free(copy); /* core keeps its own copy of the address */
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    return actor;
}

static ta_actor_id spawn_sender(ta_system *system) {
    return spawn_at(system, sender_chart, "sender", "sender1");
}

/* Reads the load request core owes for peer/p, spawns the peer and settles
 * the request with it. */
static ta_actor_id load_peer(ta_system *system) {
    ta_load_request request = { 0 };
    CHECK(ta_system_load_needed(system, &request) == TA_OK);
    ta_actor_id peer = spawn_at(system, peer_chart, "peer", "p");
    CHECK(ta_system_load_settled(system, &(ta_load_settled_params){ .request = request.request,
                                                                     .actor = peer }) == TA_OK);
    return peer;
}

/* (a) Mail waits in core for its target's load; the sender is destroyed;
 * the peer, loaded, takes it and reads its origin. */
static void test_waiting_mail_outlives_its_sender(void) {
    small_directory dir = {0};
    ta_system *system = create_system(&dir, NULL, 0);
    ta_actor_id sender = spawn_sender(system);
    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    ta_actor_id peer = load_peer(system);
    CHECK(took_mail_from(system, peer, "sender1"));
    ta_system_destroy(system);
}

/* (b) A due send's I/O processor refuses it, and it is kept as submitted;
 * the sender is destroyed; the send is offered again, and the processor
 * reads its origin's name. */
static void test_submitted_send_outlives_its_sender(void) {
    small_directory dir = {0};
    ta_system *system = create_system(&dir, NULL, 0);
    ta_atom type = 0;
    CHECK(ta_system_intern_atom(system, (ta_str)STR("slow"), &type) == TA_OK);
    CHECK(ta_system_register_io(system, &(ta_io_registration_params){ .type = type, .io = { .v = &slow_vtable } }) ==
          TA_OK);
    slow_offers = 0;
    slow_source[0] = '\0';
    ta_actor_id sender = spawn_at(system, slow_sender_chart, "sender", "sender1");
    CHECK(ta_system_set_time(system, &(ta_set_time_params){ .now = 1000 }) == TA_OK);
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_set_time(system, &(ta_set_time_params){ .now = ta_system_time(system), .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 0 && more && slow_offers == 1);
    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    CHECK(ta_system_set_time(system, &(ta_set_time_params){ .now = ta_system_time(system), .budget = 10, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
    CHECK(dispatched == 1 && !more && slow_offers == 2);
    CHECK(strcmp(slow_source, "sender1") == 0);
    ta_system_destroy(system);
}

/* (c) A host interns an origin, delivers an event from it and lets go of its
 * own reference; the event the actor takes, and then holds, still reads the
 * origin. */
static void test_delivered_origin_outlives_the_host_reference(void) {
    small_directory dir = {0};
    ta_system *system = create_system(&dir, NULL, 0);
    ta_actor_id actor = spawn_sender(system);

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
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .name = test_event_text(system, &event), .event = &event }) == TA_OK);
    ta_address_release(origin);
    /* The pool held the event until the actor takes it; one step takes it,
     * and the macrostep pauses holding it. */
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .budget = 1, .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_PAUSED);

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.current_event != NULL);
    if (view.current_event != NULL) {
        ta_actor_address held = ta_address_view(view.current_event->origin.as.actor.as.named);
        CHECK(held.name.len == 7 && memcmp(held.name.ptr, "client9", 7) == 0);
    }
    ta_system_destroy(system);
}

/* Equal addresses are one handle, and the last release frees it. */
static void test_equal_addresses_are_one_handle(void) {
    small_directory dir = {0};
    ta_system *system = create_system(&dir, NULL, 0);
    long before = heap.live;
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
    CHECK(heap.live == before + 2); /* the record is gone; the first page and its directory stay */
    /* Many addresses: the table grows a bucket at a time and finds them all. */
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
    test_waiting_mail_outlives_its_sender();
    test_submitted_send_outlives_its_sender();
    test_delivered_origin_outlives_the_host_reference();
    test_equal_addresses_are_one_handle();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "address_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("address_test: passed\n");
    return 0;
}
