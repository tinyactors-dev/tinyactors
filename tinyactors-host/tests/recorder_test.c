/*
 * The recorder (kernel ADR 0006 section 2): the events the named actors of
 * the recorded families take, handed over once per turn with the wakes of
 * the addresses touched.
 *
 * - A delivery is recorded with its address, its event and its time, in
 *   the turn its macrostep ends: a macrostep a budget pauses across turns
 *   is recorded in its last turn.
 * - Nothing is recorded for an actor of a family not recorded, for mail
 *   that is not delivered (a dead letter), or for an unpublished actor.
 * - Wakes: an armed delayed send's due time; none once it fired; a given
 *   one for an eviction, and none for a forget.
 * - Records not released when the system closes leak nothing.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/recorder.h"
#include "tinyactors_host/system.h"
#include "system_private.h"

#define STR(text) { .ptr = (text), .len = sizeof(text) - 1u }
#define END { 0 }

static bool no_wake(tah_system *system, void *context) {
    (void)system;
    (void)context;
    return true;
}

/*
 * Family "ping", recorded:
 *   zero --ping--> one; zero --arm--> waiting (tick to itself in 5s);
 *   zero --long--> a, then eventless a -> b -> c -> d (a macrostep of
 *   several microsteps); waiting --tick--> ticked; any --stop--> end (final).
 * Family "quiet", not recorded: q0 --ping--> q1.
 */
#define SEND_SELF(id_text, event_text, delay_text)                                                \
    { .kind = TA_SCXML_ACTION_SEND,                                                               \
      .as.send = { .id = { .kind = TA_SCXML_ID_LITERAL, .as.literal = STR(id_text) },             \
                   .event = { .kind = TA_SCXML_EVENT_NAME, .as.name = STR(event_text) },          \
                   .delay = { .kind = TA_SCXML_STRING_LITERAL, .as.literal = STR(delay_text) } } }
#define TARGETS(...) ((const ta_str[]){ __VA_ARGS__, END })
#define GO(event_text, target_text) \
    { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR(event_text), .targets = TARGETS(STR(target_text)) }
#define ALWAYS(target_text) { .kind = TA_SCXML_TRANSITION_EXTERNAL, .targets = TARGETS(STR(target_text)) }

static const ta_scxml_state_spec ping_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("zero"),
      .transitions = (const ta_scxml_transition_spec[]){
          GO("ping", "one"),
          GO("long", "a"),
          { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = STR("arm"), .targets = TARGETS(STR("waiting")),
            .actions = (const ta_scxml_action_spec[]){ SEND_SELF("t1", "tick", "5s"), END } },
          GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("one"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("a"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS("b"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("b"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS("c"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("c"), .transitions = (const ta_scxml_transition_spec[]){ ALWAYS("d"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("d"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("waiting"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("tick", "ticked"), GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("ticked"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("stop", "end"), END } },
    { .kind = TA_SCXML_STATE_FINAL, .id = STR("end") },
    END,
};

static const ta_scxml_state_spec quiet_states[] = {
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("q0"),
      .transitions = (const ta_scxml_transition_spec[]){ GO("ping", "q1"), END } },
    { .kind = TA_SCXML_STATE_ATOMIC, .id = STR("q1") },
    END,
};

static const ta_actor_address ping_address = { .family = { .name = STR("ping") }, .name = STR("p1") };
static const ta_actor_address quiet_address = { .family = { .name = STR("quiet") }, .name = STR("q") };

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
    ta_definition_id quiet;
} world;

static world make_world(void) {
    world w = { 0 };
    CHECK(tah_system_create(&(tah_system_create_params){ .platform = check_platform(), .incarnation = 1 }, &w.system) ==
          TA_OK);
    tah_system_set_driver(w.system, &(tah_pump_driver){ .wake = no_wake });
    CHECK(tah_system_set_clock(w.system, TAH_CLOCK_MANUAL, 0) == TA_OK);
    w.ping = define(w.system, "ping", ping_states);
    w.quiet = define(w.system, "quiet", quiet_states);
    static const ta_str families[] = { STR("ping") };
    CHECK(tah_recorder_configure(w.system, families, 1) == TA_OK);
    CHECK(tah_recorder_enabled(w.system));
    return w;
}

static void finish_world(world *w) {
    tah_system_close(w->system);
    CHECK(tah_system_leaked_bytes(w->system) == 0);
    tah_system_free(w->system);
}

static ta_actor_id spawn(world *w, ta_definition_id definition, const ta_actor_address *address) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(w->system->core, &(ta_actor_spawn_params){
                                              .definition = definition, .address = address, .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(w->system, actor) == TA_OK);
    return actor;
}

static void post(world *w, ta_actor_id target, const char *name) {
    ta_event event = { .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tah_event_name_intern(w->system->core, (ta_str){ .ptr = name, .len = (uint32_t)strlen(name) }, &event.name) ==
          TA_OK);
    CHECK(tah_system_post(w->system, &(tah_message){ .target = target, .event = &event }) == TA_OK);
}

/* What one turn hands over. */
typedef struct {
    uint32_t records;
    char names[8][32];
    char addresses[8][32];
    ta_time times[8];
    uint32_t wakes;
    char wake_addresses[8][32];
    bool wake_has[8];
    ta_time wake_at[8];
} batch;

static void text_into(char *buffer, size_t capacity, ta_str text) {
    size_t length = text.len < capacity - 1u ? text.len : capacity - 1u;
    memcpy(buffer, text.ptr, length);
    buffer[length] = '\0';
}

static void take(world *w, batch *out) {
    memset(out, 0, sizeof *out);
    out->records = tah_recorder_count(w->system);
    CHECK(out->records <= 8);
    for (uint32_t index = 0; index < out->records; index += 1u) {
        tah_record record = tah_recorder_record(w->system, index);
        char name[64];
        uint32_t needed = 0;
        CHECK(ta_event_name_format(w->system->core, &(ta_event_name_format_params){
                                                        .name = record.event->name, .buffer = name,
                                                        .capacity = sizeof name, .needed = &needed }) == TA_OK);
        text_into(out->names[index], sizeof out->names[index], (ta_str){ .ptr = name, .len = needed - 1u });
        text_into(out->addresses[index], sizeof out->addresses[index], record.address.name);
        out->times[index] = record.at;
    }
    const tah_record_wake *wakes = tah_recorder_wakes(w->system, &out->wakes);
    CHECK(out->wakes <= 8);
    for (uint32_t index = 0; index < out->wakes; index += 1u) {
        text_into(out->wake_addresses[index], sizeof out->wake_addresses[index], wakes[index].address.name);
        out->wake_has[index] = wakes[index].has_at;
        out->wake_at[index] = wakes[index].at;
    }
    tah_recorder_release(w->system);
}

/* One turn, and what it hands over. */
static void turn(world *w, batch *out) {
    tah_system_turn(w->system);
    take(w, out);
    tah_system_end_turn(w->system);
}

/* Turns until quiet; the records of all of them, counted, and the last name. */
static uint32_t run(world *w, batch *last) {
    uint32_t records = 0;
    for (int guard = 0; !tah_system_quiet(w->system); guard += 1) {
        CHECK(guard < 100);
        batch b;
        turn(w, &b);
        records += b.records;
        if (b.records != 0 || b.wakes != 0) {
            *last = b;
        }
    }
    return records;
}

/* A delivery is recorded with its address, event and time, and its address's wake. */
static void records_a_delivery(void) {
    world w = make_world();
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    batch b = { 0 };
    CHECK(run(&w, &b) == 0); /* the bootstrap records nothing */
    CHECK(tah_system_advance_to(w.system, 1000) == TA_OK);
    post(&w, actor, "ping");
    CHECK(run(&w, &b) == 1);
    CHECK(strcmp(b.names[0], "ping") == 0);
    CHECK(strcmp(b.addresses[0], "p1") == 0);
    CHECK(b.times[0] == 1000);
    CHECK(b.wakes == 1 && strcmp(b.wake_addresses[0], "p1") == 0 && !b.wake_has[0]);
    finish_world(&w);
}

/* A macrostep paused across turns is recorded in the turn it ends. */
static void records_at_the_end(void) {
    world w = make_world();
    w.system->budgets.microsteps = 1;
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    batch b = { 0 };
    (void)run(&w, &b);
    post(&w, actor, "long");
    uint32_t turns = 0;
    uint32_t recorded_in = 0;
    for (int guard = 0; !tah_system_quiet(w.system); guard += 1) {
        CHECK(guard < 100);
        turn(&w, &b);
        turns += 1u;
        if (b.records != 0) {
            CHECK(recorded_in == 0 && b.records == 1 && strcmp(b.names[0], "long") == 0);
            recorded_in = turns;
        }
    }
    CHECK(turns > 2);
    CHECK(recorded_in == turns);
    finish_world(&w);
}

/* Not recorded: another family, a dead letter, an unpublished actor. */
static void records_nothing_else(void) {
    world w = make_world();
    ta_actor_id quiet = spawn(&w, w.quiet, &quiet_address);
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    batch b = { 0 };
    (void)run(&w, &b);
    post(&w, quiet, "ping");
    CHECK(run(&w, &b) == 0);
    post(&w, actor, "stop");
    CHECK(run(&w, &b) == 1);
    post(&w, actor, "ping"); /* the actor finished: a dead letter */
    CHECK(run(&w, &b) == 0);
    ta_actor_id prepared = 0;
    ta_time at = 0;
    static const ta_actor_address other = { .family = { .name = STR("ping") }, .name = STR("p2") };
    CHECK(ta_actor_prepare(w.system->core, &(ta_actor_prepare_params){
                                               .address = &other, .definition = w.ping, .at = &at, .out = &prepared }) ==
          TA_OK);
    CHECK(tah_hook_actor_created(w.system, prepared) == TA_OK);
    CHECK(run(&w, &b) == 0);
    finish_world(&w);
}

/* Wakes: an armed send's due time, none once it fired; given for an eviction, none for a forget. */
static void wakes(void) {
    world w = make_world();
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    batch b = { 0 };
    (void)run(&w, &b);
    CHECK(tah_system_advance_to(w.system, 1000) == TA_OK);
    post(&w, actor, "arm");
    CHECK(run(&w, &b) == 1);
    CHECK(b.wakes == 1 && b.wake_has[0] && b.wake_at[0] == 6000);
    CHECK(tah_system_advance_to(w.system, 6000) == TA_OK);
    CHECK(run(&w, &b) == 1); /* the tick it sent itself */
    CHECK(strcmp(b.names[0], "tick") == 0 && b.times[0] == 6000);
    CHECK(b.wakes == 1 && !b.wake_has[0]);

    tah_recorder_evicted(w.system, actor, true, 9000);
    CHECK(ta_actor_destroy(w.system->core, actor) == TA_OK);
    CHECK(tah_hook_actor_removed(w.system, actor) == TA_OK);
    take(&w, &b);
    CHECK(b.wakes == 1 && b.wake_has[0] && b.wake_at[0] == 9000);

    actor = spawn(&w, w.ping, &ping_address);
    (void)run(&w, &b);
    tah_recorder_forget(w.system, actor);
    CHECK(ta_actor_destroy(w.system->core, actor) == TA_OK);
    CHECK(tah_hook_actor_removed(w.system, actor) == TA_OK);
    take(&w, &b);
    CHECK(b.wakes == 1 && !b.wake_has[0]);
    finish_world(&w);
}

/* Records the binding never took are freed when the system closes. */
static void close_with_records(void) {
    world w = make_world();
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    post(&w, actor, "ping");
    for (int guard = 0; !tah_system_quiet(w.system); guard += 1) {
        CHECK(guard < 100);
        tah_system_turn(w.system);
        tah_system_end_turn(w.system);
    }
    CHECK(tah_recorder_count(w.system) == 1);
    finish_world(&w);
}

/* Configured off, nothing is recorded. */
static void off(void) {
    world w = make_world();
    CHECK(tah_recorder_configure(w.system, NULL, 0) == TA_OK);
    CHECK(!tah_recorder_enabled(w.system));
    ta_actor_id actor = spawn(&w, w.ping, &ping_address);
    post(&w, actor, "ping");
    batch b = { 0 };
    CHECK(run(&w, &b) == 0);
    finish_world(&w);
}

int main(void) {
    records_a_delivery();
    records_at_the_end();
    records_nothing_else();
    wakes();
    close_with_records();
    off();
    puts("tinyactors-host recorder test passed");
    return 0;
}
