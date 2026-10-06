/*
 * Delayed sends live with their owner (ADR 0054), exercised through the
 * public header and the amalgamation only: inspection returns an actor's
 * pending sends as a sorted, borrowed array however many sends other actors
 * hold; cancelling an actor's earliest send moves the global deadline; a
 * submitted send outlives its destroyed sender; an unloaded session
 * takes its sends into the image and gets them back on load; and a session
 * with an admitted input waiting does not unload. A send that falls due
 * goes into core's pool (ADR 0099): to a resident session's run queue, or,
 * for one that is not resident, to wait for its load, which core asks the
 * host for; one that finds the pool full waits, submitted, and is offered
 * again.
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
 * A test allocator, so teardown can prove nothing leaked.
 */

static test_allocator heap;
static const ta_allocator allocator = { .self = &heap, .v = &test_allocator_vtable };

/* ------------------------------------------------------------------------
 * The load requests core owes the host for targets that are not resident.
 */

/* Reads every request owed, and answers how many there were; `last` gets
 * the last one when it is not null. */
static uint32_t owed_requests(ta_system *system, ta_load_request *last) {
    uint32_t count = 0;
    ta_load_request request;
    while (ta_system_load_needed(system, &request) == TA_OK) {
        count += 1u;
        if (last != NULL) {
            *last = request;
        }
    }
    return count;
}

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

/* One send each, at four delays, so due order differs from spawn order. */
#define ONE_SEND(name, delay_text)                                                                 \
    static const ta_scxml_state_spec name[] = {                                                    \
        { .kind = TA_SCXML_STATE_ATOMIC,                                                           \
          .id = STR("waiting"),                                                                    \
          .onentry = (const ta_scxml_handler_spec[]){                                              \
              { .actions = (const ta_scxml_action_spec[]){ SEND("t", "t", delay_text), {0} } },    \
              {0},                                                                                 \
          } },                                                                                     \
        {0},                                                                                       \
    }
ONE_SEND(timer_7s, "7s");
ONE_SEND(timer_3s, "3s");
ONE_SEND(timer_5s, "5s");
ONE_SEND(timer_1s, "1s");

/* A send to a session that is not resident, due at 2s: one that finds the
 * pool full when it falls due becomes a submitted send. */
static const ta_scxml_state_spec peer_later[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){
                  { .kind = TA_SCXML_ACTION_SEND,
                    .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR("now") },
                                 .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR("ping") },
                                 .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("#_actor_peer/p") },
                                 .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR("2s") } } },
                  {0} } },
            {0},
        },
    },
    {0},
};

/* A session that sends nothing: a target the host loads. */
static const ta_scxml_state_spec quiet[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("quiet") }, {0} };

/* A send to self, due at 2s: one that finds the pool full when it falls
 * due becomes a submitted send. */
static const ta_scxml_state_spec ping_later[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("sent"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND("now", "ping", "2s"), {0} } },
            {0},
        },
    },
    {0},
};

/* Eight sends to self, due at 1.5s: falling due, they fill a pool of eight
 * envelopes until this session takes them. */
static const ta_scxml_state_spec filler[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("filling"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND("f1", "f", "1500ms"), SEND("f2", "f", "1500ms"),
                                                         SEND("f3", "f", "1500ms"), SEND("f4", "f", "1500ms"),
                                                         SEND("f5", "f", "1500ms"), SEND("f6", "f", "1500ms"),
                                                         SEND("f7", "f", "1500ms"), SEND("f8", "f", "1500ms"),
                                                         {0} } },
            {0},
        },
    },
    {0},
};

/* Two sends with one literal ID, as a chart may make them. */
static const ta_scxml_state_spec twins[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("waiting"),
        .onentry = (const ta_scxml_handler_spec[]){
            { .actions = (const ta_scxml_action_spec[]){ SEND("dup", "first", "1s"), SEND("dup", "second", "2s"),
                                                         SEND("other", "third", "3s"), {0} } },
            {0},
        },
    },
    {0},
};

/* On `go`, content that sends `t` and `u`, then cancels `t`, in one block. */
static const ta_scxml_state_spec send_then_cancel[] = {
    {
        .kind = TA_SCXML_STATE_ATOMIC,
        .id = STR("idle"),
        .transitions = (const ta_scxml_transition_spec[]){
            { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = STR("go"),
              .actions = (const ta_scxml_action_spec[]){ SEND("t", "tick", "1s"), SEND("u", "tock", "2s"),
                                                         CANCEL("t"), {0} } },
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
                    .datamodel = ta_null_datamodel(),
                    .allocator = allocator,
                    .out = &definition,
                });
    CHECK(result == TA_OK);
    ta_definition_id id = 0;
    CHECK(result == TA_OK && ta_system_register_definition(system, definition, &id) == TA_OK);
    return id;
}

static void run(ta_system *system, ta_actor_id actor) {
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_run(system, actor, &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
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
    test_drive_status status = TEST_DRIVE_ERROR;
    CHECK(test_drive_deliver(system,
                             &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor,
                                                         .event = &delivered },
                             &(test_drive_run_params){ .status = &status }) == TA_OK);
    CHECK(status == TEST_DRIVE_STABLE);
}

static ta_system *create_system(void) {
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator }, &system) == TA_OK);
    return system;
}

/* A system whose pool holds eight envelopes, the fewest it may. */
static ta_system *create_small_system(void) {
    ta_param_value small_pool[] = { { TA_PARAM_POOL_ENVELOPES, 8 } };
    ta_system *system = NULL;
    CHECK(ta_system_create(&(ta_system_config){ .allocator = allocator, .params = small_pool, .param_count = 1 },
                           &system) == TA_OK);
    return system;
}

/* The mail and due sends sessions took, as a host that records inputs when
 * they are taken sees them in ta_run()'s TAKEN outcomes. */
typedef struct {
    int taken;
    ta_event_name_id last_event;
    ta_actor_ref_kind last_origin_kind;
    ta_actor_id last_origin_runtime;
    ta_actor_id last_actor;
} mail_seen;

/* Runs everything core holds until it is idle, counting into `mail` the
 * inputs core queued itself. */
static void drain(ta_system *system, mail_seen *mail) {
    for (uint32_t call = 0; call < 100000u; call += 1u) {
        ta_input_outcome outcomes[2];
        uint32_t count = 0;
        ta_run_status status = TA_RUN_IDLE;
        CHECK(ta_run(system, &(ta_run_params){ .budget = 100, .report_taken = true, .outcomes = outcomes,
                                               .outcome_capacity = 2, .outcome_count = &count,
                                               .status = &status }) == TA_OK);
        for (uint32_t index = 0; index < count; index += 1u) {
            if (outcomes[index].has_event) {
                mail->taken += 1;
                mail->last_event = outcomes[index].event.name;
                mail->last_origin_kind = outcomes[index].event.origin.as.actor.kind;
                mail->last_origin_runtime = outcomes[index].event.origin.as.actor.as.runtime;
                mail->last_actor = outcomes[index].actor;
                ta_event_release(&outcomes[index].event);
            }
        }
        if (status == TA_RUN_IDLE) {
            return;
        }
    }
    CHECK(!"core did not become idle");
}

/* Fires what is due at the system's time, up to `budget` sends; answers how many fired. */
static uint32_t dispatch_up_to(ta_system *system, uint32_t budget, bool *more) {
    uint32_t dispatched = 0;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = budget, .dispatched = &dispatched,
                                                                    .more_due = more }) == TA_OK);
    return dispatched;
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
    ta_system *system = create_system();
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

/*
 * The owner heap spans pages (TA_TIMER_PAGE_OWNERS each, as the actor table
 * does, ADR 0063): with owners spawned out of due order, some destroyed from
 * the middle, every remaining send is dispatched in due order, and no
 * allocation is larger than one 64 KiB page, whatever the population.
 */
static void test_owner_heap_spans_pages(void) {
    ta_system *system = create_system();
    const ta_scxml_state_spec *const charts[] = { timer_7s, timer_3s, timer_5s, timer_1s };
    ta_definition_id definitions[4];
    for (uint32_t index = 0; index < 4u; index += 1u) {
        definitions[index] = define(system, index == 0 ? "t7" : index == 1 ? "t3" : index == 2 ? "t5" : "t1",
                                    charts[index]);
    }
    enum { OWNERS = 3 * 8192 + 5 };
    static ta_actor_id actors[OWNERS];
    heap.largest_request = 0;
    for (uint32_t index = 0; index < OWNERS; index += 1u) {
        CHECK(ta_system_set_time(system, index) == TA_OK);
        actors[index] = spawn(system, definitions[index % 4u], NULL);
    }
    uint32_t live = OWNERS;
    for (uint32_t index = 0; index < OWNERS; index += 3u) {
        CHECK(ta_actor_destroy(system, actors[index]) == TA_OK);
        live -= 1u;
    }
    CHECK(heap.largest_request <= 65536u);

    ta_time previous = 0;
    uint32_t total = 0;
    ta_time next = 0;
    mail_seen mail = {0};
    while (ta_system_next_deadline(system, &next) == TA_OK) {
        CHECK(next >= previous);
        previous = next;
        if (next > ta_system_time(system)) {
            CHECK(ta_system_set_time(system, next) == TA_OK);
        }
        bool more = false;
        uint32_t dispatched = dispatch_up_to(system, 1, &more);
        CHECK(dispatched == 1);
        total += dispatched;
        if (dispatched != 1) {
            break;
        }
        /* Each send went to its own session, in core: it takes it. */
        drain(system, &mail);
    }
    CHECK(total == live);
    CHECK(mail.taken == (int)live && owed_requests(system, NULL) == 0);
    ta_system_destroy(system);
}

/* Cancelling an actor's earliest send moves the global deadline to its next. */
static void test_cancelling_the_front_send(void) {
    ta_system *system = create_system();
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

    /* Dispatch follows global (due, send order) across both owners, and the
     * sessions take the sends in that order. */
    CHECK(ta_system_set_time(system, 60000) == TA_OK);
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 3 && !more);
    mail_seen mail = {0};
    drain(system, &mail);
    CHECK(mail.taken == 3 && mail.last_event == event_name(system, "far") && owed_requests(system, NULL) == 0);
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 0);
    CHECK(ta_system_next_deadline(system, &next) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/*
 * A pool of eight, filled when the filler's sends fall due at 1.5s, before
 * each peer_later session's `ping` to peer/p at 2s: those find no free
 * envelope and wait, submitted. Answers the first sender.
 */
static ta_actor_id submit_to_peer(ta_system *system, uint32_t senders) {
    (void)spawn(system, define(system, "filler", filler), NULL);
    ta_definition_id definition = define(system, "later", peer_later);
    ta_actor_id sender = 0;
    for (uint32_t index = 0; index < senders; index += 1u) {
        ta_actor_id spawned = spawn(system, definition, NULL);
        sender = index == 0u ? spawned : sender;
    }
    CHECK(ta_system_set_time(system, 2000) == TA_OK);
    bool more = false;
    CHECK(dispatch_up_to(system, 100, &more) == 8 && more);
    return sender;
}

/*
 * A send that found no envelope when it fell due was already sent:
 * destroying its sender keeps it queued for the receiver, with its original
 * envelope, until it goes; then it waits for the receiver's load, and the
 * receiver the host loads takes it.
 */
static void test_submitted_send_outlives_its_sender(void) {
    ta_system *system = create_small_system();
    ta_actor_id sender = submit_to_peer(system, 1);

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, sender, &view) == TA_OK && view.delayed_send_count == 1);
    if (view.delayed_send_count == 1) {
        CHECK(view.delayed_sends[0].submitted);
        CHECK(view.delayed_sends[0].event == event_name(system, "ping"));
    }

    CHECK(ta_actor_destroy(system, sender) == TA_OK);
    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 2000);

    /* The filler takes its sends, which frees the pool. */
    mail_seen mail = {0};
    drain(system, &mail);
    CHECK(mail.taken == 8);
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 1 && !more);
    ta_load_request request = { 0 };
    CHECK(owed_requests(system, &request) == 1 && request.destination.kind == TA_ACTOR_REF_NAMED);
    ta_actor_id peer = spawn(system, define(system, "peer", quiet), NULL);
    CHECK(ta_system_load_settled(system, &(ta_load_settled_params){ .request = request.request,
                                                                     .actor = peer }) == TA_OK);
    drain(system, &mail);
    CHECK(mail.taken == 9 && mail.last_actor == peer && mail.last_event == event_name(system, "ping"));
    CHECK(mail.last_origin_kind == TA_ACTOR_REF_RUNTIME && mail.last_origin_runtime == sender);
    CHECK(ta_system_next_deadline(system, &next) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/* An orphaned send still pending at teardown is released with the system. */
static void test_teardown_releases_orphans(void) {
    ta_system *system = create_small_system();
    ta_actor_id first = submit_to_peer(system, 2);
    CHECK(ta_actor_destroy(system, first) == TA_OK);
    ta_system_destroy(system);
}

/* Unloading takes the pending sends into the image; loading brings them back. */
static void test_capture_and_load_keep_the_sends(void) {
    ta_system *system = create_system();
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

/*
 * A send to self that found no room when it fell due is already sent (ADR
 * 0054). When its owner unloads, it moves to the system's own list, not into
 * the image. It names its session by session ID (ADR 0066), so its retry
 * finds the session wherever it is: by address while it is unloaded,
 * waiting for its load, which core asks the host for, and by its new
 * runtime ID once it is loaded again.
 */
static bool same_text(ta_str left, ta_str right) {
    return left.len == right.len && (left.len == 0 || memcmp(left.ptr, right.ptr, left.len) == 0);
}

/*
 * A pool of eight, filled when the filler's sends fall due at 1.5s, before
 * the named session's `ping` at 2s: that one finds no free envelope and
 * waits, submitted. Answers the session.
 */
static ta_actor_id refuse_a_self_send(ta_system *system, const ta_actor_address *address) {
    (void)spawn(system, define(system, "filler", filler), NULL);
    ta_actor_id actor = spawn(system, define(system, "immediate", ping_later), address);
    CHECK(ta_system_set_time(system, 2000) == TA_OK);
    bool more = false;
    CHECK(dispatch_up_to(system, 100, &more) == 8 && more);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 1);
    CHECK(view.delayed_send_count != 1 || view.delayed_sends[0].submitted);
    return actor;
}

static void test_refused_self_send_follows_its_owner_across_unload(void) {
    ta_system *system = create_small_system();
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_actor_address address = { .family = { STR("immediate") }, .name = STR("self") };
    ta_actor_id actor = refuse_a_self_send(system, &address);

    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);
    /* The filler takes its sends, which frees the pool for the load. */
    mail_seen mail = {0};
    drain(system, &mail);
    CHECK(mail.taken == 8);
    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    CHECK(loaded != actor);
    ta_session_image_destroy(image);
    drain(system, &mail); /* the loaded session's wake */
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 1 && !more);
    drain(system, &mail);
    CHECK(mail.taken == 9 && mail.last_event == event_name(system, "ping"));
    /* Not the runtime ID of the session that unloaded: the session, loaded
     * again, takes it, and its origin is the session's address. */
    CHECK(mail.last_actor == loaded && mail.last_origin_kind == TA_ACTOR_REF_NAMED);
    CHECK(owed_requests(system, NULL) == 0);
    ta_system_destroy(system);
}

static void test_refused_self_send_to_an_unloaded_owner_goes_by_address(void) {
    ta_system *system = create_small_system();
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_actor_address address = { .family = { STR("immediate") }, .name = STR("self") };
    ta_actor_id actor = refuse_a_self_send(system, &address);

    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);

    mail_seen mail = {0};
    drain(system, &mail);
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 1);
    /* The owner is out: the retry waits for it by address, and core asks
     * the host to load it. */
    ta_load_request request = { 0 };
    CHECK(owed_requests(system, &request) == 1);
    CHECK(request.destination.kind == TA_ACTOR_REF_NAMED && request.hint == TA_ERR_UNLOADED &&
          request.waiting == 1);
    if (request.destination.kind == TA_ACTOR_REF_NAMED) {
        ta_actor_address waited = ta_address_view(request.destination.as.named);
        CHECK(same_text(waited.name, address.name) && same_text(waited.family.name, address.family.name));
    }
    ta_session_image_destroy(image);
    ta_system_destroy(system);
}

/* ------------------------------------------------------------------------
 * Withdrawing one send from outside the chart (ADR 0084)
 */

/* The WITHDRAWN and CANCELLED records a sink saw, in order. */
typedef struct {
    ta_trace_kind kind;
    ta_step_key key;
    ta_send_id send;
    ta_actor_id actor;
} seen_record;

static seen_record seen[16];
static uint32_t seen_count;

static void seen_write(void *self, const ta_trace_record *record) {
    (void)self;
    if (seen_count < 16u) {
        seen[seen_count++] = (seen_record){ .kind = record->kind, .key = record->key, .send = record->send,
                                            .actor = record->actor };
    }
}
static void watch_withdrawals(ta_system *system) {
    seen_count = 0;
    uint32_t kinds = TA_TRACE_BIT(TA_TRACE_WITHDRAWN) | TA_TRACE_BIT(TA_TRACE_CANCELLED);
    CHECK(ta_system_set_trace_masks(system, &(ta_trace_masks){ .kinds = kinds }) == TA_OK);
}

/* The records seen so far, after reading what the system wrote to tel. */
static uint32_t seen_now(ta_system *system) {
    tel_drain(system, seen_write, NULL);
    return seen_count;
}

static ta_send_id send_id(ta_system *system, const char *text) {
    ta_send_id id = 0;
    CHECK(ta_send_id_parse(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &id) == TA_OK);
    return id;
}

static uint64_t cancelled_count(ta_system *system) {
    ta_counters counters;
    ta_system_counters(system, &counters);
    return counters.sends_cancelled;
}

/*
 * An operator withdraws one pending send by owner and ID: it is gone from
 * inspection and from the global deadline, it never fires, the others do,
 * nothing allocates, and a lifecycle-like WITHDRAWN record says so. Only
 * the named actor's sends can be withdrawn (ADR 0012).
 */
static void test_withdrawing_one_send(void) {
    ta_system *system = create_system();
    ta_definition_id many = define(system, "many", one_timer);
    ta_definition_id three = define(system, "three", three_timers);
    ta_actor_id neighbour = spawn(system, many, NULL);
    ta_actor_id actor = spawn(system, three, NULL);
    watch_withdrawals(system);

    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 3);
    ta_send_id early = view.delayed_sends[0].send_id;
    CHECK(early == send_id(system, "early"));
    uint64_t cancelled = cancelled_count(system);
    long before = heap.attempts;
    CHECK(ta_actor_withdraw_send(system, actor, early) == TA_OK);
    CHECK(heap.attempts == before);
    CHECK(cancelled_count(system) == cancelled + 1u);

    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 2);
    if (view.delayed_send_count == 2) {
        CHECK(view.delayed_sends[0].event == event_name(system, "middle"));
        CHECK(view.delayed_sends[1].event == event_name(system, "late"));
    }
    ta_time next = 0;
    CHECK(ta_system_next_deadline(system, &next) == TA_OK && next == 2000);

    /* One record, outside any macrostep: the session's second lifecycle key
     * (SPAWNED was the first). */
    CHECK(seen_now(system) == 1);
    CHECK(seen[0].kind == TA_TRACE_WITHDRAWN && seen[0].send == early && seen[0].actor == actor);
    CHECK(seen[0].key.session == view.session_id && seen[0].key.macrostep == 0 && seen[0].key.microstep == 2);

    /* Gone once: again, it is not found, and nothing is recorded. */
    CHECK(ta_actor_withdraw_send(system, actor, early) == TA_ERR_NOT_FOUND);
    CHECK(seen_now(system) == 1 && cancelled_count(system) == cancelled + 1u);
    /* Only the owner's sends: the neighbour's `far` is not this actor's. */
    ta_send_id far = send_id(system, "far");
    CHECK(ta_actor_withdraw_send(system, actor, far) == TA_ERR_NOT_FOUND);
    CHECK(ta_actor_inspect(system, neighbour, &view) == TA_OK && view.delayed_send_count == 1);
    /* No ID, and no actor. */
    CHECK(ta_actor_withdraw_send(system, actor, 0) == TA_ERR_INVALID);
    CHECK(ta_actor_withdraw_send(NULL, actor, far) == TA_ERR_NOT_FOUND);
    CHECK(ta_actor_withdraw_send(system, ta_actor_id_make(ta_actor_id_slot(actor), 0), far) == TA_ERR_NOT_FOUND);

    /* The chart's own <cancel> of the withdrawn send finds nothing. */
    deliver(system, actor, "drop");
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 2);

    CHECK(ta_system_set_time(system, 60000) == TA_OK);
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 3);
    mail_seen mail = {0};
    drain(system, &mail);
    CHECK(mail.taken == 3 && owed_requests(system, NULL) == 0);

    /* A destroyed actor's ID names nothing. */
    CHECK(ta_actor_destroy(system, actor) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, actor, send_id(system, "late")) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/* Every pending send with the ID goes, as a <cancel> of it would; the rest stay. */
static void test_withdrawing_a_repeated_id(void) {
    ta_system *system = create_system();
    ta_actor_id actor = spawn(system, define(system, "twins", twins), NULL);
    CHECK(ta_actor_withdraw_send(system, actor, send_id(system, "dup")) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 1);
    CHECK(view.delayed_send_count != 1 || view.delayed_sends[0].event == event_name(system, "third"));
    ta_system_destroy(system);
}

/*
 * A submitted send was sent: nothing had room for it once and it waits for
 * its receiver (ADR 0054). It is not withdrawn, and it goes later.
 */
static void test_a_submitted_send_is_not_withdrawn(void) {
    ta_system *system = create_small_system();
    ta_actor_id sender = submit_to_peer(system, 1);
    watch_withdrawals(system);
    uint64_t cancelled = cancelled_count(system);
    CHECK(ta_actor_withdraw_send(system, sender, send_id(system, "now")) == TA_ERR_DONE);
    CHECK(seen_now(system) == 0 && cancelled_count(system) == cancelled);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, sender, &view) == TA_OK && view.delayed_send_count == 1);

    mail_seen mail = {0};
    drain(system, &mail); /* the filler takes its sends, which frees the pool */
    bool more = true;
    CHECK(dispatch_up_to(system, 10, &more) == 1 && owed_requests(system, NULL) == 1);
    CHECK(ta_actor_withdraw_send(system, sender, send_id(system, "now")) == TA_ERR_NOT_FOUND);
    ta_system_destroy(system);
}

/*
 * While a macrostep is paused inside a block: a send the block has not made
 * yet is not found; one it made is withdrawn, and the block's own <cancel>
 * of it later in the same macrostep finds nothing. The record is outside
 * the macrostep, between its CANCELLED record's neighbours.
 */
static void test_withdrawing_inside_a_paused_macrostep(void) {
    ta_system *system = create_system();
    ta_actor_id actor = spawn(system, define(system, "stepper", send_then_cancel), NULL);
    watch_withdrawals(system);
    ta_event go = { .name = event_name(system, "go"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .time = ta_system_time(system), .actor = actor, .event = &go }) == TA_OK);
    ta_send_id t = send_id(system, "t");
    /* Delivered, not yet run: `t` does not exist yet. */
    CHECK(ta_actor_withdraw_send(system, actor, t) == TA_ERR_NOT_FOUND);

    test_drive_status status = TEST_DRIVE_ERROR;
    test_drive_run_params step = { .status = &status, .action_budget = 1 };
    CHECK(test_drive_run(system, actor, &step) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK);
    /* One unit ran: the first <send>. */
    CHECK(status == TEST_DRIVE_PAUSED && view.microstep_in_progress && view.delayed_send_count == 1);
    CHECK(ta_actor_withdraw_send(system, actor, t) == TA_OK);
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 0);
    CHECK(view.macrostep_in_progress);

    step.action_budget = 0;
    CHECK(test_drive_run(system, actor, &step) == TA_OK && status == TEST_DRIVE_STABLE);
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 1);
    CHECK(view.delayed_send_count != 1 || view.delayed_sends[0].event == event_name(system, "tock"));
    /* WITHDRAWN outside the macrostep, then the chart's own CANCELLED in it. */
    CHECK(seen_now(system) == 2);
    CHECK(seen[0].kind == TA_TRACE_WITHDRAWN && seen[0].key.macrostep == 0 && seen[0].send == t);
    CHECK(seen[1].kind == TA_TRACE_CANCELLED && seen[1].key.macrostep == 2 && seen[1].send == t);
    ta_system_destroy(system);
}

/*
 * Withdrawing changes the actor: an image captured before it is stale and
 * cannot unload the actor, so the withdrawn send cannot come back through
 * it. An image captured after it does not hold the send.
 */
static void test_an_image_from_before_a_withdrawal_is_stale(void) {
    ta_system *system = create_system();
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_definition_id three = define(system, "three", three_timers);
    ta_actor_address address = { .family = { STR("three") }, .name = STR("t1") };
    ta_actor_id actor = spawn(system, three, &address);

    ta_session_image *before = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &before }) == TA_OK);
    CHECK(ta_actor_withdraw_send(system, actor, send_id(system, "early")) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = before }) == TA_ERR_STALE);
    ta_session_image_destroy(before);

    ta_session_image *after = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &after }) == TA_OK);
    ta_time image_next = 0;
    CHECK(ta_session_image_next_deadline(after, &image_next) == TA_OK && image_next == 2000);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = after }) == TA_OK);
    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(system, &(ta_actor_load_params){ .image = after, .out = &loaded }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, loaded, &view) == TA_OK && view.delayed_send_count == 2);
    CHECK(view.delayed_send_count != 2 || view.delayed_sends[0].due == 2000);
    ta_session_image_destroy(after);
    ta_system_destroy(system);
}

static void dispatch(ta_system *system) {
    uint32_t dispatched = 0;
    bool more = false;
    CHECK(ta_system_dispatch_due(system, &(ta_due_dispatch_params){ .budget = 100, .dispatched = &dispatched,
                                                                    .more_due = &more }) == TA_OK);
}

/*
 * A delayed send that fires leaves its owner's pending sends, and so changes
 * the owner as a withdrawal does: an image captured before it fired still
 * holds the send, and unloading with it would bring the send back to fire a
 * second time. The image is stale, whether the pool takes the send or
 * refuses it and keeps it as a submitted one.
 */
static void test_an_image_from_before_a_send_fired_is_stale(void) {
    ta_system *system = create_small_system();
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_definition_id three = define(system, "three", three_timers);
    ta_actor_address address = { .family = { STR("three") }, .name = STR("t1") };
    ta_actor_id actor = spawn(system, three, &address);
    (void)spawn(system, define(system, "filler", filler), NULL);

    ta_session_image *before = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &before }) == TA_OK);
    CHECK(ta_system_set_time(system, 1000) == TA_OK);
    dispatch(system);
    mail_seen mail = {0};
    drain(system, &mail);
    CHECK(mail.taken == 1 && mail.last_event == event_name(system, "early") && mail.last_actor == actor);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = before }) == TA_ERR_STALE);
    ta_session_image_destroy(before);

    /* Refused, the send stays with its owner as a submitted one, which no
     * image holds: an image from before still holds it as pending. The
     * filler's sends, due at 1.5s, fill the pool before `middle` falls due. */
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &before }) == TA_OK);
    CHECK(ta_system_set_time(system, 2000) == TA_OK);
    dispatch(system);
    ta_actor_inspection fired;
    CHECK(ta_actor_inspect(system, actor, &fired) == TA_OK && fired.delayed_send_count == 2);
    CHECK(fired.delayed_send_count != 2 || fired.delayed_sends[0].submitted);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = before }) == TA_ERR_STALE);
    ta_session_image_destroy(before);
    drain(system, &mail);

    /* An image from after holds only the send still pending. */
    ta_session_image *after = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &after }) == TA_OK);
    ta_time image_next = 0;
    CHECK(ta_session_image_next_deadline(after, &image_next) == TA_OK && image_next == 3000);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = after }) == TA_OK);
    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(system, &(ta_actor_load_params){ .image = after, .out = &loaded }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, loaded, &view) == TA_OK && view.delayed_send_count == 1);
    CHECK(view.delayed_send_count != 1 || view.delayed_sends[0].due == 3000);
    ta_session_image_destroy(after);
    ta_system_destroy(system);
}

/*
 * An input core admitted names the session's runtime ID, which a loaded
 * actor does not keep: a session with one waiting does not unload, and
 * changes nothing, until the input is taken or withdrawn.
 */
static void test_admitted_mail_keeps_a_session_resident(void) {
    ta_system *system = create_system();
    one_directory dir = {0};
    CHECK(ta_system_set_actor_directory(system, (ta_actor_directory){ .self = &dir, .v = &dir_vtable }) == TA_OK);
    ta_definition_id three = define(system, "three", three_timers);
    ta_actor_address address = { .family = { STR("three") }, .name = STR("t1") };
    ta_actor_id actor = spawn(system, three, &address);

    ta_event waiting = { .name = event_name(system, "nothing"), .type = TA_EVENT_EXTERNAL,
                         .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(ta_actor_deliver(system, &(ta_actor_deliver_params){ .actor = actor, .event = &waiting,
                                                               .time = ta_system_time(system),
                                                               .correlation = 7 }) == TA_OK);
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_ERR_BUSY);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system, actor, &view) == TA_OK && view.delayed_send_count == 3);
    CHECK(ta_system_withdraw_input(system, 7) == TA_OK);
    CHECK(ta_actor_unload(system, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);
    ta_actor_id loaded = 0;
    CHECK(ta_actor_load(system, &(ta_actor_load_params){ .image = image, .out = &loaded }) == TA_OK);
    CHECK(ta_actor_inspect(system, loaded, &view) == TA_OK && view.delayed_send_count == 3);
    ta_session_image_destroy(image);
    ta_system_destroy(system);
}

int main(void) {
    test_inspection_lists_only_the_actors_sends();
    test_cancelling_the_front_send();
    test_owner_heap_spans_pages();
    test_submitted_send_outlives_its_sender();
    test_teardown_releases_orphans();
    test_capture_and_load_keep_the_sends();
    test_refused_self_send_follows_its_owner_across_unload();
    test_refused_self_send_to_an_unloaded_owner_goes_by_address();
    test_withdrawing_one_send();
    test_withdrawing_a_repeated_id();
    test_a_submitted_send_is_not_withdrawn();
    test_withdrawing_inside_a_paused_macrostep();
    test_an_image_from_before_a_withdrawal_is_stale();
    test_an_image_from_before_a_send_fired_is_stale();
    test_admitted_mail_keeps_a_session_resident();
    CHECK(heap.live == 0);
    CHECK(test_allocator_finish(&heap) == 0);
    if (failures != 0) {
        fprintf(stderr, "delayed_send_test: %d check(s) failed\n", failures);
        return 1;
    }
    printf("delayed_send_test: passed\n");
    return 0;
}
