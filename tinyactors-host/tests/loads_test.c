/*
 * Loading unloaded actors on demand (napi ADR 0020), driven without
 * Node-API: the host's posts for an unloaded name wait in its load's mailbox
 * and move, in order, to the front of the loaded actor's mailbox; the
 * per-address cap; waiting posts count against maxQueuedMessages; a failed
 * load's posts become dead letters that keep the name; a virtual family's
 * unknown name starts a load; an unloaded actor's own timer wakes it; and
 * without a loader nothing changes. Mail between sessions waits in core, and
 * core's LOAD_NEEDED notice starts the same loads: a gone session of a family
 * is served by a new instance, and a family without a loader answers at once
 * with error.communication for the sender. Run under AddressSanitizer by
 * test-native-asan.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host/events.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/loads.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "loads_private.h"
#include "scheduler_private.h"
#include "system_private.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })

static bool no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
    return true;
}

static tah_system *make_system(void) {
    tah_system *system = NULL;
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform() }, &system) == TA_OK);
    tah_system_set_driver(system, &(tah_pump_driver){ .wake = no_wake });
    CHECK(tah_system_set_clock(system, TAH_CLOCK_MANUAL, 0) == TA_OK);
    return system;
}

static void finish_system(tah_system *system) {
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

/* Family "r": idle, taking any event without changing state. */
static ta_definition_id define_receiver(tah_system *system) {
    static ta_scxml_transition_spec any[] = {
        { .kind = TA_SCXML_TRANSITION_INTERNAL, .event = { "*", 1 } }, { 0 } };
    static ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "idle", 4 }, .transitions = any },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("r"), .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = &spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

static const ta_actor_address recv_name = { .family = { .name = { "r", 1 } }, .name = { "recv", 4 } };

static ta_actor_id spawn(tah_system *system, ta_definition_id definition, const ta_actor_address *address) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = definition, .address = address,
                                                                 .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(system, actor) == TA_OK);
    return actor;
}

/* Runs turns until quiet; returns the dead letters, and the last one's reason. */
static uint32_t settle(tah_system *system, tah_dead_reason *reason) {
    uint32_t dead = 0;
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
            CHECK(record->kind != TAH_RECORD_FAULT);
            if (record->kind == TAH_RECORD_DEAD_LETTER) {
                dead += 1u;
                if (reason != NULL) {
                    *reason = record->reason;
                }
                CHECK(record->has_address || record->actor != 0);
            }
        }
        tah_system_end_turn(system);
    }
    return dead;
}

static ta_session_image *unload(tah_system *system, ta_actor_id actor) {
    ta_session_image *image = NULL;
    CHECK(ta_actor_capture(system->core, &(ta_actor_capture_params){ .actor = actor, .out = &image }) == TA_OK);
    CHECK(ta_actor_unload(system->core, &(ta_actor_unload_params){ .actor = actor, .image = image }) == TA_OK);
    CHECK(tah_hook_actor_unloaded(system, actor) == TA_OK);
    return image;
}

static ta_actor_id load(tah_system *system, ta_session_image *image) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_load(system->core, &(ta_actor_load_params){ .image = image, .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(system, actor) == TA_OK);
    ta_session_image_destroy(image);
    return actor;
}

static ta_address *intern(tah_system *system, const ta_actor_address *text) {
    ta_address *address = NULL;
    CHECK(ta_address_intern(system->core, text, &address) == TA_OK);
    return address;
}

static ta_event_name_id event_name(tah_system *system, const char *text) {
    ta_event_name_id name = 0;
    CHECK(tah_event_name_intern(system->core, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &name) ==
          TA_OK);
    return name;
}

/* Posts an event named `text` to `address` as JavaScript's postNamed does. */
static ta_result post(tah_system *system, ta_address *address, const char *text) {
    ta_event event = { .type = TA_EVENT_EXTERNAL, .name = event_name(system, text), .invoke_id = TA_INVOKE_ID_NONE };
    return tah_system_post_named(system, address, &(tah_message){ .event = &event });
}

static tah_load_request next_request(tah_system *system) {
    tah_load_request request;
    CHECK(tah_loads_next_request(&system->loads, &request));
    return request;
}

static ta_result finish(tah_system *system, tah_load_request request, tah_load_outcome outcome, ta_actor_id actor) {
    return tah_loads_finish(&system->loads, &(tah_load_finish){ .core = system->core,
                                                                .sched = &system->sched,
                                                                .directory = &system->directory,
                                                                .metrics = &system->metrics,
                                                                .index = request.index,
                                                                .generation = request.generation,
                                                                .outcome = outcome,
                                                                .actor = actor });
}

/* One turn: the sender's bootstrap runs, its mail waits in core, and core's LOAD_NEEDED reaches the loads. */
static void turn(tah_system *system) {
    tah_system_turn(system);
    tah_system_end_turn(system);
}

/* The event names in `actor`'s mailbox, in order, must be `expected`. */
static void mailbox_is(tah_system *system, ta_actor_id actor, const char *const *expected, uint32_t count) {
    const tah_sched_entry *entry = tah_sched_lookup(&system->sched, actor);
    CHECK(entry != NULL);
    uint32_t at = 0;
    for (uint32_t index = entry->first; index != 0; index = tah_sched_envelope(&system->sched, index)->next) {
        CHECK(at < count);
        CHECK(tah_sched_envelope(&system->sched, index)->event.name == event_name(system, expected[at]));
        CHECK(tah_sched_envelope(&system->sched, index)->target == actor);
        at += 1u;
    }
    CHECK(at == count);
}

/*
 * Mail for an unloaded name starts one load; more mail joins its mailbox,
 * even after the actor is resident again, until the load finishes; then it
 * goes, in order, ahead of mail sent to the actor meanwhile.
 */
static void waiting_mail_moves_in_order(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id first = spawn(system, chart, &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
    ta_address *address = intern(system, &recv_name);

    CHECK(post(system, address, "a") == TA_OK);
    CHECK(post(system, address, "b") == TA_OK);
    CHECK(system->loads.in_flight == 1u && system->loads.waiting == 2u);
    tah_load_request request = next_request(system);
    CHECK(request.address == address && request.why == TAH_LOAD_WHY_MAIL);
    tah_load_request none;
    CHECK(!tah_loads_next_request(&system->loads, &none)); /* one load per address */

    ta_actor_id loaded = load(system, image);
    CHECK(post(system, address, "c") == TA_OK); /* resident, but the load is in flight: it waits too */
    ta_event direct = { .type = TA_EVENT_EXTERNAL, .name = event_name(system, "d"), .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tah_system_post(system, &(tah_message){ .target = loaded, .event = &direct }) == TA_OK);

    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    static const char *const order[] = { "a", "b", "c", "d" };
    mailbox_is(system, loaded, order, 4);
    CHECK(system->loads.in_flight == 0u && system->loads.waiting == 0u);
    CHECK(system->loads.outcomes[TAH_LOAD_LOADED] == 1u);
    CHECK(system->metrics.load_seconds.count == 1u);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_ERR_STALE);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 4u);

    /* Resident and no load in flight: mail goes straight to the actor. */
    CHECK(post(system, address, "e") == TA_OK);
    CHECK(tah_sched_mailbox_count(&system->sched, loaded) == 1u && system->loads.in_flight == 0u);
    ta_address_release(address);
    finish_system(system);
}

/* At most max_waiting messages wait; waiting mail counts against max_queued. */
static void waiting_mail_is_bounded(void) {
    tah_system *system = make_system();
    ta_actor_id first = spawn(system, define_receiver(system), &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 2) == TA_OK);
    ta_address *address = intern(system, &recv_name);
    CHECK(post(system, address, "a") == TA_OK);
    CHECK(post(system, address, "b") == TA_OK);
    CHECK(post(system, address, "c") == TA_OK); /* accepted as a waiting-full dead letter */
    CHECK(system->loads.waiting == 2u && system->sched.queued == 3u);
    system->sched.max_queued = 3u;
    CHECK(post(system, address, "d") == TA_ERR_BUSY);
    system->sched.max_queued = 0;
    tah_load_request request = next_request(system); /* the pump hands it to JavaScript after a turn */
    tah_dead_reason reason = TAH_DEAD_NOT_FOUND;
    CHECK(settle(system, &reason) == 1u && reason == TAH_DEAD_WAITING_FULL);

    ta_actor_id loaded = load(system, image);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 2u);
    ta_address_release(address);
    finish_system(system);
}

/* A load that fails dead-letters its mail with the name; the next mail starts a new load. */
static void a_failed_load_dead_letters_its_mail(void) {
    static const struct {
        tah_load_outcome outcome;
        tah_dead_reason reason;
    } cases[] = {
        { TAH_LOAD_NOT_FOUND, TAH_DEAD_NOT_FOUND },
        { TAH_LOAD_REJECTED, TAH_DEAD_REJECTED },
        { TAH_LOAD_FAILED, TAH_DEAD_LOAD_FAILED },
        { TAH_LOAD_TIMED_OUT, TAH_DEAD_LOAD_TIMEOUT },
    };
    for (uint32_t at = 0; at < sizeof cases / sizeof cases[0]; at += 1u) {
        tah_system *system = make_system();
        ta_actor_id first = spawn(system, define_receiver(system), &recv_name);
        CHECK(settle(system, NULL) == 0u);
        ta_session_image *image = unload(system, first);
        CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
        ta_address *address = intern(system, &recv_name);
        CHECK(post(system, address, "a") == TA_OK);
        CHECK(post(system, address, "b") == TA_OK);
        tah_load_request request = next_request(system);
        /* A LOADED finish for an actor that is not at the address changes nothing. */
        CHECK(finish(system, request, TAH_LOAD_LOADED, first) == TA_ERR_NOT_FOUND);
        CHECK(finish(system, request, cases[at].outcome, 0) == TA_OK);
        tah_dead_reason reason = TAH_DEAD_DONE;
        CHECK(settle(system, &reason) == 2u && reason == cases[at].reason);
        CHECK(system->loads.outcomes[cases[at].outcome] == 1u && system->loads.waiting == 0u);
        CHECK(system->sched.dead_by_reason[cases[at].reason] == 2u);
        /* The next message starts a new load. */
        CHECK(post(system, address, "c") == TA_OK);
        tah_load_request again = next_request(system);
        CHECK(again.index != request.index || again.generation != request.generation);
        ta_actor_id loaded = load(system, image);
        CHECK(finish(system, again, TAH_LOAD_LOADED, loaded) == TA_OK);
        CHECK(settle(system, NULL) == 0u);
        ta_address_release(address);
        finish_system(system);
    }
}

/* A virtual family: a name with no entry at all starts a load, and a spawn at it finishes it. */
static void a_virtual_family_spawns(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
    ta_address *address = intern(system, &recv_name);
    CHECK(post(system, address, "a") == TA_OK);
    tah_load_request request = next_request(system);
    ta_actor_id spawned = spawn(system, chart, &recv_name);
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, spawned) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 1u && system->loads.outcomes[TAH_LOAD_SPAWNED] == 1u);
    /* Without a loader, the old behaviour: a dead letter. */
    CHECK(tah_loads_set_family(&system->loads, S("r"), 0) == TA_OK);
    ta_actor_address other = { .family = { .name = S("r") }, .name = S("other") };
    ta_address *nobody = intern(system, &other);
    CHECK(post(system, nobody, "a") == TA_OK);
    tah_dead_reason reason = TAH_DEAD_DONE;
    CHECK(settle(system, &reason) == 1u && reason == TAH_DEAD_NOT_FOUND);
    ta_address_release(nobody);
    ta_address_release(address);
    finish_system(system);
}

/* An unloaded actor's wake-up starts a load when it is due, and not before. */
static void a_wake_up_starts_a_load(void) {
    tah_system *system = make_system();
    ta_actor_id first = spawn(system, define_receiver(system), &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
    ta_address *address = intern(system, &recv_name);
    CHECK(tah_loads_wake_at(&system->loads, address, 500) == TA_OK);
    CHECK(tah_loads_wake_at(&system->loads, address, 100) == TA_OK); /* replaces the first */
    ta_time due = 0;
    CHECK(tah_loads_next_wake(&system->loads, &due) && due == 100);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->loads.in_flight == 0u);
    CHECK(tah_system_advance_to(system, 100) == TA_OK);
    CHECK(!tah_system_quiet(system));
    tah_system_turn(system);
    tah_system_end_turn(system);
    tah_load_request request = next_request(system);
    CHECK(request.why == TAH_LOAD_WHY_TIMER && request.address == address);
    ta_actor_id loaded = load(system, image);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    /* The replaced wake-up at 500 is dropped when it comes up. */
    CHECK(tah_system_advance_to(system, 600) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->loads.in_flight == 0u && system->loads.wakes.count == 0u);
    /* A wake-up for a resident actor starts nothing. */
    CHECK(tah_loads_wake_at(&system->loads, address, 700) == TA_OK);
    CHECK(tah_system_advance_to(system, 700) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->loads.in_flight == 0u);
    ta_address_release(address);
    finish_system(system);
}

/*
 * A cancelled send() withdraws its message from the load's mailbox: the
 * rest still move, in order; withdrawing it again, or a message that is
 * not waiting, changes nothing.
 */
static void a_waiting_message_can_be_withdrawn(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id first = spawn(system, chart, &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
    ta_address *address = intern(system, &recv_name);

    CHECK(post(system, address, "a") == TA_OK);
    CHECK(post(system, address, "b") == TA_OK);
    CHECK(post(system, address, "c") == TA_OK);
    tah_load_request request = next_request(system);
    tah_load *record = tah_loads_find(&system->loads, request.index, request.generation);
    CHECK(record != NULL);
    uint32_t middle = tah_sched_envelope(&system->sched, record->mail.first)->next;
    uint32_t live = system->sched.envelopes.live;
    CHECK(tah_loads_withdraw(&system->loads, &system->sched, middle));
    CHECK(system->loads.waiting == 2u && record->mail.count == 2u);
    CHECK(system->sched.envelopes.live == live - 1u);
    CHECK(!tah_loads_withdraw(&system->loads, &system->sched, middle));
    uint32_t last = record->mail.last;
    CHECK(tah_loads_withdraw(&system->loads, &system->sched, last)); /* the tail moves back */
    CHECK(record->mail.count == 1u && record->mail.first == record->mail.last);
    CHECK(post(system, address, "d") == TA_OK); /* appends after the new tail */

    ta_actor_id loaded = load(system, image);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    static const char *const order[] = { "a", "d" };
    mailbox_is(system, loaded, order, 2);
    CHECK(settle(system, NULL) == 0u);
    ta_address_release(address);
    finish_system(system);
}

/*
 * A chart of family "s" that, on entry, sends `a` and then `b` to `target`, and
 * counts nothing else: what the target's session hears is read from core's
 * counters.
 */
static ta_definition_id define_sender(tah_system *system, const char *target) {
    ta_scxml_action_spec actions[3];
    memset(actions, 0, sizeof actions);
    static const char *const names[2] = { "a", "b" };
    for (uint32_t index = 0; index < 2u; index += 1u) {
        actions[index] = (ta_scxml_action_spec){
            .kind = TA_SCXML_ACTION_SEND,
            .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = { .ptr = names[index], .len = 1 } },
                         .target = { .kind = TA_SCXML_STRING_LITERAL,
                                     .as.literal = { .ptr = target, .len = (uint32_t)strlen(target) } } },
        };
    }
    ta_scxml_handler_spec onentry[] = { { .actions = actions }, { 0 } };
    ta_scxml_state_spec states[] = { { .kind = TA_SCXML_STATE_ATOMIC, .id = S("sent"), .onentry = onentry }, { 0 } };
    ta_scxml_spec spec = { .name = S("s"), .states = states };
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = &spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

/* The `#_scxml_` target of the session `actor` is, as a chart writes it (core ADR 0088). */
static void session_target(tah_system *system, ta_actor_id actor, char *out, size_t capacity) {
    ta_actor_inspection view = {0};
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    char text[96];
    uint32_t needed = 0;
    CHECK(ta_session_id_format(system->core, &(ta_session_id_format_params){ .session = view.session_id,
                                                                            .family = S("r"), .buffer = text,
                                                                            .capacity = sizeof text,
                                                                            .needed = &needed }) == TA_OK);
    CHECK((size_t)snprintf(out, capacity, "#_scxml_%s", text) < capacity);
}

/*
 * Mail for a gone session of a family with a loader: core holds it on the
 * session's key and reports LOAD_NEEDED, which starts one load (with `why` =
 * session) however many messages wait; a new instance without a name serves
 * it, and the messages reach that instance in order.
 */
static void a_gone_session_loads_a_new_instance(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id gone = spawn(system, chart, NULL);
    CHECK(settle(system, NULL) == 0u);
    char target[128];
    session_target(system, gone, target, sizeof target);
    CHECK(ta_actor_destroy(system->core, gone) == TA_OK);
    CHECK(tah_hook_actor_removed(system, gone) == TA_OK);
    CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
    ta_counters before;
    ta_system_counters(system->core, &before);

    (void)spawn(system, define_sender(system, target), NULL);
    turn(system);
    CHECK(system->loads.in_flight == 1u); /* two messages, one load */
    tah_load_request request = next_request(system);
    CHECK(request.why == TAH_LOAD_WHY_SESSION);
    CHECK(!tah_loads_next_request(&system->loads, &(tah_load_request){ 0 }));
    CHECK(tah_loads_info(&system->loads, request.index, request.generation).address == request.address);
    CHECK(ta_address_waiting(system->core, request.address) == 2u);
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, ta_actor_id_make(99, 7)) == TA_ERR_NOT_FOUND);
    ta_actor_id spawned = spawn(system, chart, NULL);
    ta_address *key = (ta_address *)request.address; /* the load's reference ends with the load; keep one to look afterwards */
    ta_address_retain(key);
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, spawned) == TA_OK);
    CHECK(ta_address_waiting(system->core, key) == 0u);
    ta_address_release(key);
    CHECK(settle(system, NULL) == 0u);
    ta_counters after;
    ta_system_counters(system->core, &after);
    CHECK(after.events_taken - before.events_taken == 2u); /* a and b, which the new instance took */
    CHECK(after.errors_communication == before.errors_communication);
    finish_system(system);
}

/*
 * A family without a loader: nothing would bring the session back, so core's
 * LOAD_NEEDED is answered in the same turn, and the sender hears
 * error.communication for each message, and no load starts.
 */
static void a_gone_session_without_a_loader_is_an_error(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id gone = spawn(system, chart, NULL);
    CHECK(settle(system, NULL) == 0u);
    char target[128];
    session_target(system, gone, target, sizeof target);
    CHECK(ta_actor_destroy(system->core, gone) == TA_OK);
    CHECK(tah_hook_actor_removed(system, gone) == TA_OK);
    ta_counters before;
    ta_system_counters(system->core, &before);

    (void)spawn(system, define_sender(system, target), NULL);
    turn(system);
    CHECK(settle(system, NULL) == 0u);
    ta_counters after;
    ta_system_counters(system->core, &after);
    CHECK(system->loads.in_flight == 0u);
    CHECK(after.errors_communication - before.errors_communication == 2u);
    finish_system(system);
}

/*
 * Mail for an unloaded name of a family with a loader: core holds it and
 * reports LOAD_NEEDED, which starts the load; the load's LOADED outcome
 * leaves core's list to the actor's becoming resident (core moved it when the
 * image loaded); a failed load recycles it, and the sender hears an error.
 */
static void chart_mail_for_an_unloaded_name(void) {
    for (uint32_t failed = 0; failed < 2u; failed += 1u) {
        tah_system *system = make_system();
        ta_definition_id chart = define_receiver(system);
        ta_actor_id first = spawn(system, chart, &recv_name);
        CHECK(settle(system, NULL) == 0u);
        ta_session_image *image = unload(system, first);
        CHECK(tah_loads_set_family(&system->loads, S("r"), 10) == TA_OK);
        ta_counters before;
        ta_system_counters(system->core, &before);
        (void)spawn(system, define_sender(system, "#_actor_r/recv"), NULL);
        turn(system);
        ta_address *address = intern(system, &recv_name);
        CHECK(ta_address_waiting(system->core, address) == 2u);
        tah_load_request request = next_request(system);
        CHECK(request.why == TAH_LOAD_WHY_MAIL && request.address == address);
        if (failed == 0u) {
            ta_actor_id loaded = load(system, image);
            CHECK(ta_address_waiting(system->core, address) == 0u); /* core moved it when the image loaded */
            CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
        } else {
            CHECK(finish(system, request, TAH_LOAD_REJECTED, 0) == TA_OK);
            ta_session_image_destroy(image);
        }
        CHECK(settle(system, NULL) == 0u);
        ta_counters after;
        ta_system_counters(system->core, &after);
        CHECK(after.events_taken - before.events_taken == 2u); /* a and b at the actor, or the errors at the sender */
        CHECK(after.errors_communication - before.errors_communication == (failed == 0u ? 0u : 2u));
        ta_address_release(address);
        finish_system(system);
    }
}

int main(void) {
    waiting_mail_moves_in_order();
    a_waiting_message_can_be_withdrawn();
    waiting_mail_is_bounded();
    a_failed_load_dead_letters_its_mail();
    a_virtual_family_spawns();
    a_wake_up_starts_a_load();
    a_gone_session_loads_a_new_instance();
    a_gone_session_without_a_loader_is_an_error();
    chart_mail_for_an_unloaded_name();
    puts("loads_test passed");
    return 0;
}
