/*
 * Loading unloaded actors on demand (napi ADR 0020), driven without
 * Node-API: mail JavaScript posts for an unloaded name waits in core (core
 * ADR 0099), core asks for one load, and the load's end delivers it to the
 * loaded actor; a failed load refuses it, a dead letter that keeps the
 * name; a cancelled send() withdraws it from core; a virtual family's
 * unknown name starts a load; an unloaded actor's own timer wakes it; load
 * requests for a gone session share a load; and without a loader the post
 * is a dead letter at once. Run under AddressSanitizer by test-native-asan.
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
    CHECK(tah_event_name_intern(system, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &name) ==
          TA_OK);
    return name;
}

/* Posts an event named `text` to `address` as JavaScript's postNamed does,
 * with the send() `waiter`, or 0. */
static ta_result post_waiting(tah_system *system, ta_address *address, const char *text, uint32_t waiter) {
    ta_event event = { .type = TA_EVENT_EXTERNAL, .name = event_name(system, text), .invoke_id = TA_INVOKE_ID_NONE };
    return tah_system_post(system, &(tah_message){ .address = address, .event = &event, .waiter = waiter });
}

static ta_result post(tah_system *system, ta_address *address, const char *text) {
    return post_waiting(system, address, text, 0);
}

/* One turn: core runs, and the turn answers what core asks (system.c). */
static void one_turn(tah_system *system) {
    tah_system_turn(system);
    tah_system_end_turn(system);
}

static tah_load_request next_request(tah_system *system) {
    tah_load_request request;
    CHECK(tah_loads_next_request(&system->loads, &request));
    return request;
}

static ta_result finish(tah_system *system, tah_load_request request, tah_load_outcome outcome, ta_actor_id actor) {
    return tah_system_finish_load(system, &(tah_system_load_finish){ .index = request.index,
                                                                     .generation = request.generation,
                                                                     .outcome = outcome,
                                                                     .actor = actor });
}

/*
 * Mail JavaScript posts for an unloaded name waits in core, which asks
 * once; more mail joins the same request; when the load finishes, core
 * delivers all of it to the loaded actor, and later mail goes straight to
 * it.
 */
static void waiting_mail_moves_in_order(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id first = spawn(system, chart, &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
    ta_address *address = intern(system, &recv_name);

    CHECK(post(system, address, "a") == TA_OK);
    CHECK(post(system, address, "b") == TA_OK);
    one_turn(system);
    CHECK(system->loads.in_flight == 1u && system->loads.waiting == 2u);
    tah_load_request request = next_request(system);
    CHECK(request.address == address && request.why == TAH_LOAD_WHY_MAIL);
    CHECK(tah_loads_info(&system->loads, request.index, request.generation).waiting == 2u);
    tah_load_request none;
    CHECK(!tah_loads_next_request(&system->loads, &none)); /* one load per address */

    ta_actor_id loaded = load(system, image);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    CHECK(system->loads.in_flight == 0u && system->loads.waiting == 0u);
    CHECK(system->loads.outcomes[TAH_LOAD_LOADED] == 1u);
    CHECK(system->metrics.load_seconds.count == 1u);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_ERR_STALE);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 2u);

    /* Resident and no load in flight: mail goes straight to the actor. */
    CHECK(post(system, address, "e") == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 3u && system->loads.in_flight == 0u);
    ta_address_release(address);
    finish_system(system);
}

/* A load that fails refuses the mail JavaScript posted: dead letters that
 * keep the name and the failure's reason; the next mail starts a new load. */
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
        CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
        ta_address *address = intern(system, &recv_name);
        CHECK(post(system, address, "a") == TA_OK);
        CHECK(post(system, address, "b") == TA_OK);
        one_turn(system);
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
        one_turn(system);
        tah_load_request again = next_request(system);
        CHECK(again.index != request.index || again.generation != request.generation);
        ta_actor_id loaded = load(system, image);
        CHECK(finish(system, again, TAH_LOAD_LOADED, loaded) == TA_OK);
        CHECK(settle(system, NULL) == 0u);
        CHECK(system->sched.delivered == 1u);
        ta_address_release(address);
        finish_system(system);
    }
}

/* A virtual family: a name with no entry at all starts a load, and a spawn at it finishes it. */
static void a_virtual_family_spawns(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
    ta_address *address = intern(system, &recv_name);
    CHECK(post(system, address, "a") == TA_OK);
    one_turn(system);
    tah_load_request request = next_request(system);
    ta_actor_id spawned = spawn(system, chart, &recv_name);
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, spawned) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 1u && system->loads.outcomes[TAH_LOAD_SPAWNED] == 1u);
    /* Without a loader the post has nowhere to go: a dead letter. */
    CHECK(tah_loads_set_family(&system->loads, S("r"), false) == TA_OK);
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
    CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
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
 * A cancelled send() withdraws its message from core while it waits for
 * the load: the rest still go; cancelling it again changes nothing.
 */
static void a_waiting_message_can_be_withdrawn(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_id first = spawn(system, chart, &recv_name);
    CHECK(settle(system, NULL) == 0u);
    ta_session_image *image = unload(system, first);
    CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
    ta_address *address = intern(system, &recv_name);

    uint32_t waiter = tah_sched_waiter_take(&system->sched);
    CHECK(waiter != 0);
    CHECK(post(system, address, "a") == TA_OK);
    CHECK(post_waiting(system, address, "b", waiter) == TA_OK);
    CHECK(post(system, address, "c") == TA_OK);
    one_turn(system);
    tah_load_request request = next_request(system);
    uint32_t live = system->sched.envelopes.live;
    CHECK(tah_system_cancel(system, waiter));
    CHECK(system->sched.envelopes.live == live - 1u);
    tah_sched_waiter_give(&system->sched, waiter);

    ta_actor_id loaded = load(system, image);
    CHECK(finish(system, request, TAH_LOAD_LOADED, loaded) == TA_OK);
    CHECK(settle(system, NULL) == 0u);
    CHECK(system->sched.delivered == 2u);
    ta_address_release(address);
    finish_system(system);
}

/* Asks for a load for a gone session's key, as the router does for a load
 * request core owes (napi ADR 0032 section 2). */
static ta_result ask_session(tah_system *system, ta_address *key, bool shared, uint64_t request) {
    return tah_loads_ask(&system->loads, &(tah_load_ask){ .address = key,
                                                          .why = TAH_LOAD_WHY_SESSION,
                                                          .shared = shared,
                                                          .request = request,
                                                          .waiting = 1 });
}

/*
 * Load requests for a gone session: requests for one session's key share
 * one load, which a new instance without a name finishes; a session this
 * system cannot name gets a load per request; a family without a loader
 * starts none. The mail itself waits in core, which delivers it in order
 * (core's waiting_test).
 */
static void a_gone_session_loads_a_new_instance(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_receiver(system);
    ta_actor_address session_key = { .family = { .name = S("r") }, .name = S("0.4294967297.r") };
    ta_address *key = intern(system, &session_key);
    CHECK(ask_session(system, key, true, 11) == TA_ERR_NOT_FOUND); /* no loader */
    CHECK(system->loads.in_flight == 0u);
    CHECK(tah_loads_set_family(&system->loads, S("r"), true) == TA_OK);
    CHECK(tah_loads_family_loads(&system->loads, S("r")) && !tah_loads_family_loads(&system->loads, S("x")));
    CHECK(ask_session(system, key, true, 11) == TA_OK);
    CHECK(ask_session(system, key, true, 12) == TA_OK); /* joins the load in flight */
    tah_load_request request = next_request(system);
    CHECK(request.why == TAH_LOAD_WHY_SESSION && system->loads.in_flight == 1u);
    CHECK(tah_loads_core_request(&system->loads, request.index, request.generation) == 12u);
    CHECK(!tah_loads_next_request(&system->loads, &(tah_load_request){ 0 }));
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, ta_actor_id_make(99, 7)) == TA_ERR_NOT_FOUND);
    ta_actor_id spawned = spawn(system, chart, NULL);
    CHECK(finish(system, request, TAH_LOAD_SPAWNED, spawned) == TA_OK);
    CHECK(system->loads.in_flight == 0u);

    ta_actor_address unknown_key = { .family = { .name = S("r") }, .name = S("0.0.r") };
    ta_address *unknown = intern(system, &unknown_key);
    CHECK(ask_session(system, unknown, false, 13) == TA_OK);
    CHECK(ask_session(system, unknown, false, 14) == TA_OK);
    tah_load_request first = next_request(system);
    tah_load_request second = next_request(system);
    CHECK(first.index != second.index && system->loads.in_flight == 2u);
    CHECK(finish(system, first, TAH_LOAD_NOT_FOUND, 0) == TA_OK);
    CHECK(finish(system, second, TAH_LOAD_NOT_FOUND, 0) == TA_OK);
    CHECK(system->loads.in_flight == 0u);
    ta_address_release(unknown);
    ta_address_release(key);
    finish_system(system);
}

int main(void) {
    waiting_mail_moves_in_order();
    a_waiting_message_can_be_withdrawn();
    a_failed_load_dead_letters_its_mail();
    a_virtual_family_spawns();
    a_wake_up_starts_a_load();
    a_gone_session_loads_a_new_instance();
    puts("loads_test passed");
    return 0;
}
