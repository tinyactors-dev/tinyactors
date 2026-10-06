/*
 * Replaying events (kernel ADR 0006 section 3): an actor prepared with `at`
 * takes the events a journal holds, each at its time, and runs each
 * macrostep to its end; nothing it does crosses the border until it is
 * published.
 *
 * - A replayed event changes the actor's state; an immediate send it makes
 *   reaches no one, before or after publish.
 * - A delayed send armed by a replayed event keeps its due time and fires
 *   after publish; one due by a later event's time counts as fired.
 * - A replay to a final state reports it, and an event after it is DONE.
 * - Refusals: an empty name, a time earlier than the last event's, a
 *   published actor.
 * - Mail the kernel admitted for the replaying actor before a replayed
 *   event runs first, and its send() settles at the next turn, once.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/replay.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
#include "system_private.h"

#define S(literal) ((ta_str){ .ptr = (literal), .len = (uint32_t)(sizeof(literal) - 1u) })
#define STR(text) { .ptr = (text), .len = sizeof(text) - 1u }
#define END { 0 }

static bool no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
    return true;
}

/*
 * Family "ping", address ping/p1:
 *   zero --ping--> one
 *   zero --arm--> waiting, arming `tick` to itself in 5s (send ID t1)
 *   zero --now--> sent, sending `hello` to sink/b at once
 *   waiting --tick--> ticked
 *   any --stop--> end (final)
 * Family "sink", address sink/b: h0 --hello--> h1
 */

#define SEND_SELF(id_text, event_text, delay_text)                                                \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define SEND_NOW(event_text, target_text)                                                         \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .target = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(target_text) } } }
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, END })
#define GO(event_text, target_text) \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text), .targets = TARGETS(STR(target_text)) }

static const ta_scxml_state_spec ping_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("zero"),
      .transitions = (const ta_scxml_transition_spec[]){
          GO("ping", "one"),
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("arm"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("t1", "tick", "5s"), END } },
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("now"), .targets = TARGETS(STR("sent")),
            .actions = (const ta_scxml_action_spec[]){ SEND_NOW("hello", "#_actor_sink/b"), END } },
          GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("one"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("tick", "ticked"), GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ticked"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("sent"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};

static const ta_scxml_state_spec sink_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("h0"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("hello", "h1"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("h1") },
    END,
};

static const ta_actor_address ping_address = { .family = { .name = STR("ping") }, .name = STR("p1") };
static const ta_actor_address sink_address = { .family = { .name = STR("sink") }, .name = STR("b") };

static ta_definition_id define(tah_system *system, const char *family, const ta_scxml_state_spec *states) {
    ta_definition_identity identity = {
        .family = { .name = { .ptr = family, .len = (uint32_t)strlen(family) } },
        .revision = { .ptr = (const uint8_t *)"v1", .len = 2 },
    };
    ta_scxml_spec spec = { .identity = &identity, .states = states };
    ta_definition *definition = NULL;
    ta_definition_id id = 0;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = &spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

typedef struct {
    tah_system *system;
    ta_definition_id ping;
    ta_actor_id sink;
} world;

static world make_world(void) {
    world w = { 0 };
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform(), .incarnation = 1 }, &w.system) ==
          TA_OK);
    tah_system_set_driver(w.system, &(tah_pump_driver){ .wake = no_wake });
    CHECK(tah_system_set_clock(w.system, TAH_CLOCK_MANUAL, 0) == TA_OK);
    w.ping = define(w.system, "ping", ping_states);
    ta_definition_id sink = define(w.system, "sink", sink_states);
    CHECK(ta_actor_spawn(w.system->core, &(ta_actor_spawn_params){
                                             .definition = sink, .address = &sink_address, .out = &w.sink }) == TA_OK);
    CHECK(tah_hook_actor_created(w.system, w.sink) == TA_OK);
    return w;
}

static void run(tah_system *system) {
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        tah_system_end_turn(system);
    }
}

static void finish_world(world *w) {
    tah_system_close(w->system);
    CHECK(tah_system_leaked_bytes(w->system) == 0);
    tah_system_free(w->system);
}

static ta_actor_id prepare(world *w, ta_time at) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_prepare(w->system->core, &(ta_actor_prepare_params){
                                                .address = &ping_address, .definition = w->ping, .at = &at,
                                                .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(w->system, actor) == TA_OK);
    return actor;
}

static ta_result replay(world *w, ta_actor_id actor, const char *name, ta_time time, bool *consumed, bool *finished) {
    return tah_replay_event(w->system, &(tah_replay_event_params){
                                           .actor = actor,
                                           .name = { .ptr = name, .len = (uint32_t)strlen(name) },
                                           .type = TA_EVENT_EXTERNAL,
                                           .time = time,
                                           .consumed = consumed,
                                           .finished = finished,
                                       });
}

static void publish(world *w, ta_actor_id actor, ta_time at) {
    CHECK(tah_replay_events_done(w->system, actor) == TA_OK);
    ta_actor_publish_params params = { .actor = actor, .at = &at };
    CHECK(ta_actor_publish(w->system->core, &params) == TA_OK);
    CHECK(tah_hook_actor_published(w->system, &params) == TA_OK);
}

static bool text_equal(ta_str text, const char *expected) {
    return text.len == strlen(expected) && memcmp(text.ptr, expected, text.len) == 0;
}

static bool in_state(tah_system *system, ta_actor_id actor, const char *id) {
    ta_state_id states[4];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system->core, &(ta_actor_active_states_params){
                                                   .actor = actor, .states = states, .capacity = 4, .count = &count }) ==
          TA_OK);
    if (count != 1) {
        return false;
    }
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    ta_state_info info;
    CHECK(ta_definition_state(system->core, &(ta_definition_state_params){
                                                .definition = view.definition_id, .state = states[0], .out = &info }) ==
          TA_OK);
    return text_equal(info.id, id);
}

static uint32_t pending_sends(tah_system *system, ta_actor_id actor, ta_time *due) {
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    if (due != NULL && view.delayed_send_count != 0) {
        *due = view.delayed_sends[0].due;
    }
    return view.delayed_send_count;
}

/* An immediate send made in a replay reaches no one, before or after publish. */
static void crosses_nothing(void) {
    world w = make_world();
    CHECK(tah_system_advance_to(w.system, 3000) == TA_OK);
    ta_actor_id actor = prepare(&w, 0);
    bool consumed = false;
    CHECK(replay(&w, actor, "now", 1000, &consumed, NULL) == TA_OK);
    CHECK(consumed);
    CHECK(in_state(w.system, actor, "sent"));
    run(w.system);
    CHECK(in_state(w.system, w.sink, "h0"));
    publish(&w, actor, 1000);
    run(w.system);
    CHECK(in_state(w.system, actor, "sent"));
    CHECK(in_state(w.system, w.sink, "h0"));
    finish_world(&w);
}

/* An event the chart has no transition for is not consumed. */
static void unconsumed(void) {
    world w = make_world();
    ta_actor_id actor = prepare(&w, 0);
    bool consumed = true;
    CHECK(replay(&w, actor, "tick", 0, &consumed, NULL) == TA_OK);
    CHECK(!consumed);
    CHECK(in_state(w.system, actor, "zero"));
    finish_world(&w);
}

/* A delayed send keeps its due time, and fires after publish when it comes due. */
static void timer_after_publish(void) {
    world w = make_world();
    CHECK(tah_system_advance_to(w.system, 2000) == TA_OK);
    ta_actor_id actor = prepare(&w, 0);
    CHECK(replay(&w, actor, "arm", 1000, NULL, NULL) == TA_OK);
    CHECK(in_state(w.system, actor, "waiting"));
    publish(&w, actor, 1000); /* a replaying actor holds its delayed sends until it is published */
    run(w.system);
    ta_time due = 0;
    CHECK(pending_sends(w.system, actor, &due) == 1 && due == 6000);
    CHECK(in_state(w.system, actor, "waiting"));
    CHECK(tah_system_advance_to(w.system, 6000) == TA_OK);
    run(w.system);
    CHECK(in_state(w.system, actor, "ticked"));
    finish_world(&w);
}

/* A delayed send due by a later event's time fired already: its event is in the journal. */
static void timer_fired_in_journal(void) {
    world w = make_world();
    CHECK(tah_system_advance_to(w.system, 9000) == TA_OK);
    ta_actor_id actor = prepare(&w, 0);
    CHECK(replay(&w, actor, "arm", 1000, NULL, NULL) == TA_OK);
    CHECK(replay(&w, actor, "tick", 6000, NULL, NULL) == TA_OK);
    CHECK(in_state(w.system, actor, "ticked"));
    publish(&w, actor, 6000);
    run(w.system);
    CHECK(in_state(w.system, actor, "ticked"));
    CHECK(pending_sends(w.system, actor, NULL) == 0);
    finish_world(&w);
}

/* A replay to a final state says so, and takes nothing after it. */
static void finished(void) {
    world w = make_world();
    ta_actor_id actor = prepare(&w, 0);
    bool done = false;
    CHECK(replay(&w, actor, "stop", 0, NULL, &done) == TA_OK);
    CHECK(done);
    CHECK(replay(&w, actor, "ping", 0, NULL, &done) == TA_ERR_DONE);
    CHECK(done);
    finish_world(&w);
}

/* An empty journal: the bootstrap runs before publish, which then succeeds. */
static void empty_journal(void) {
    world w = make_world();
    ta_actor_id actor = prepare(&w, 0);
    publish(&w, actor, 0);
    run(w.system);
    CHECK(in_state(w.system, actor, "zero"));
    finish_world(&w);
}

static void refusals(void) {
    world w = make_world();
    CHECK(tah_system_advance_to(w.system, 5000) == TA_OK);
    ta_actor_id actor = prepare(&w, 0);
    CHECK(replay(&w, actor, "", 0, NULL, NULL) == TA_ERR_INVALID);
    CHECK(replay(&w, actor, "ping", 2000, NULL, NULL) == TA_OK);
    CHECK(replay(&w, actor, "stop", 1000, NULL, NULL) == TA_ERR_INVALID); /* earlier than the last event */
    CHECK(replay(&w, actor, "stop", 6000, NULL, NULL) == TA_ERR_INVALID); /* later than the system's time */
    CHECK(in_state(w.system, actor, "one"));
    CHECK(replay(&w, w.sink, "hello", 5000, NULL, NULL) == TA_ERR_INVALID); /* published */
    CHECK(replay(&w, ta_actor_id_make(999, 1), "ping", 2000, NULL, NULL) == TA_ERR_NOT_FOUND);
    finish_world(&w);
}

/*
 * The replay's session filter takes the mail the kernel admitted for the
 * actor first: its outcome reaches the scheduler, and the send() waiting
 * for it settles at the next turn, exactly once.
 */
static void kernel_mail_during_replay(void) {
    world w = make_world();
    CHECK(tah_system_advance_to(w.system, 1000) == TA_OK);
    ta_actor_id actor = prepare(&w, 0);
    CHECK(tah_replay_events_done(w.system, actor) == TA_OK); /* its bootstrap */
    ta_event_name_id ping = 0;
    CHECK(tah_event_name_intern(w.system, S("ping"), &ping) == TA_OK);
    uint32_t waiter = tah_sched_waiter_take(&w.system->sched);
    CHECK(waiter != 0);
    ta_event message = { .name = ping, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tah_system_post(w.system, &(tah_message){ .target = actor, .event = &message, .waiter = waiter }) == TA_OK);
    bool consumed = true;
    CHECK(replay(&w, actor, "ping", 1000, &consumed, NULL) == TA_OK);
    /* The kernel's ping took it to one; the replayed one then found no transition. */
    CHECK(!consumed && in_state(w.system, actor, "one"));
    CHECK(!tah_system_quiet(w.system));
    uint32_t settled = 0;
    for (int guard = 0; !tah_system_quiet(w.system); guard += 1) {
        CHECK(guard < 100);
        tah_system_turn(w.system);
        tah_system_end_turn(w.system);
        tah_settlement settlement;
        while (tah_sched_settlement_pop(&w.system->sched, &settlement)) {
            CHECK(settlement.waiter == waiter && settlement.kind == TAH_SETTLE_STABLE && settlement.actor == actor);
            tah_sched_waiter_give(&w.system->sched, settlement.waiter);
            settled += 1u;
        }
    }
    CHECK(settled == 1u);
    finish_world(&w);
}

int main(void) {
    crosses_nothing();
    unconsumed();
    timer_after_publish();
    timer_fired_in_journal();
    finished();
    empty_journal();
    refusals();
    kernel_mail_during_replay();
    puts("tinyactors-host event replay test passed");
    return 0;
}
