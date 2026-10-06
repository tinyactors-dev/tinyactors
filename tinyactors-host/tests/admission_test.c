/*
 * Admitting the application's input (ADRs 0005, 0008, 0014; tinyactors-core
 * ADR 0099), driven without Node-API: a post goes straight into core and the
 * kernel holds no mailbox. The test covers per-target order across the turn
 * budget, an input before the bootstrap, stale IDs, a malformed event that is
 * refused without harming the actor, finished and destroyed targets, the
 * limits (maxQueuedMessages, maxRetainedBytes) and core's own back-pressure,
 * the budget on reported dead letters, a verdict made between turns,
 * send() waiters (completion, dead letters, an actor that goes, cancel by
 * withdrawal, a waiter whose bookkeeping cannot be made), an operator
 * transition that does not queue, and no allocator call in steady state.
 */
#include "check.h"
#include <string.h>

#include "tinyactors_host/events.h"
#include "tinyactors_host/hooks.h"
#include "tinyactors_host/scheduler.h"
#include "tinyactors_host/system.h"
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
    return system;
}

static void finish_system(tah_system *system) {
    tah_system_close(system);
    CHECK(tah_system_leaked_bytes(system) == 0);
    tah_system_free(system);
}

static ta_definition_id define(tah_system *system, const ta_scxml_spec *spec) {
    ta_definition *definition = NULL;
    CHECK(ta_definition_compile_spec(system->core, &(ta_definition_compile_params){
                                                       .spec = spec,
                                                       .datamodel = ta_null_datamodel(),
                                                       .allocator = tah_allocator_for(&system->allocator, TAH_MEMORY_CORE),
                                                       .out = &definition,
                                                   }) == TA_OK);
    ta_definition_id id = 0;
    CHECK(ta_system_register_definition(system->core, definition, &id) == TA_OK);
    return id;
}

/* a --x--> b --y--> c, and c --z--> end (a top-level final). */
static ta_definition_id define_steps(tah_system *system) {
    static ta_str to_b[] = { { "b", 1 }, { 0 } };
    static ta_str to_c[] = { { "c", 1 }, { 0 } };
    static ta_str to_end[] = { { "end", 3 }, { 0 } };
    static ta_scxml_transition_spec on_x[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "x", 1 }, .targets = to_b }, { 0 } };
    static ta_scxml_transition_spec on_y[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "y", 1 }, .targets = to_c }, { 0 } };
    static ta_scxml_transition_spec on_z[] = { { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "z", 1 }, .targets = to_end }, { 0 } };
    static ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "a", 1 }, .transitions = on_x },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "b", 1 }, .transitions = on_y },
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "c", 1 }, .transitions = on_z },
        { .kind = TA_SCXML_STATE_FINAL, .id = { "end", 3 } },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("steps"), .states = states };
    return define(system, &spec);
}

/* idle --ping--> idle, idle --stop--> end (a top-level final). */
static ta_definition_id define_ping(tah_system *system) {
    static ta_str to_idle[] = { { "idle", 4 }, { 0 } };
    static ta_str to_end[] = { { "end", 3 }, { 0 } };
    static ta_scxml_transition_spec on_idle[] = {
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "ping", 4 }, .targets = to_idle },
        { .kind = TA_SCXML_TRANSITION_EXTERNAL, .event = { "stop", 4 }, .targets = to_end },
        { 0 },
    };
    static ta_scxml_state_spec states[] = {
        { .kind = TA_SCXML_STATE_ATOMIC, .id = { "idle", 4 }, .transitions = on_idle },
        { .kind = TA_SCXML_STATE_FINAL, .id = { "end", 3 } },
        { 0 },
    };
    ta_scxml_spec spec = { .name = S("ping"), .states = states };
    return define(system, &spec);
}

static ta_actor_id spawn(tah_system *system, ta_definition_id definition) {
    ta_actor_id actor = 0;
    CHECK(ta_actor_spawn(system->core, &(ta_actor_spawn_params){ .definition = definition, .out = &actor }) == TA_OK);
    CHECK(tah_hook_actor_created(system, actor) == TA_OK);
    return actor;
}

static void destroy(tah_system *system, ta_actor_id actor) {
    CHECK(ta_actor_destroy(system->core, actor) == TA_OK);
    CHECK(tah_hook_actor_removed(system, actor) == TA_OK);
}

static ta_event_name_id name(tah_system *system, const char *text) {
    ta_event_name_id id = 0;
    CHECK(tah_event_name_intern(system->core, (ta_str){ .ptr = text, .len = (uint32_t)strlen(text) }, &id) == TA_OK);
    return id;
}

static ta_result post(tah_system *system, ta_actor_id target, ta_event_name_id event) {
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return tah_system_post(system, &(tah_message){ .target = target, .event = &message });
}

static ta_result post_bytes(tah_system *system, ta_actor_id target, ta_event_name_id event, uint32_t bytes) {
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    return tah_system_post(system, &(tah_message){ .target = target, .event = &message, .bytes = bytes });
}

/* Posts `event` with a fresh waiter; returns the waiter. */
static uint32_t send(tah_system *system, ta_actor_id target, ta_event_name_id event) {
    uint32_t waiter = tah_sched_waiter_take(&system->sched);
    CHECK(waiter != 0);
    ta_event message = { .name = event, .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(tah_system_post(system, &(tah_message){ .target = target, .event = &message, .waiter = waiter }) == TA_OK);
    return waiter;
}

enum { MAX_SETTLED = 64 };
static tah_settlement settled[MAX_SETTLED];
static uint32_t settled_count;

/* Pops settlements as the Node-API layer does, and gives their waiters back. */
static void collect(tah_system *system) {
    tah_settlement settlement;
    while (tah_sched_settlement_pop(&system->sched, &settlement)) {
        CHECK(settled_count < MAX_SETTLED);
        CHECK(tah_sched_waiter_state(&system->sched, settlement.waiter) == TAH_WAITER_SETTLING);
        settled[settled_count] = settlement;
        settled_count += 1u;
        tah_sched_waiter_give(&system->sched, settlement.waiter);
    }
}

static const tah_settlement *settlement_of(uint32_t waiter) {
    for (uint32_t index = 0; index < settled_count; index += 1) {
        if (settled[index].waiter == waiter) {
            return &settled[index];
        }
    }
    return NULL;
}

/* One turn; returns the number of records of `kind` it produced, and collects settlements. */
static uint32_t turn(tah_system *system, tah_record_kind kind) {
    tah_system_turn(system);
    uint32_t count = 0;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        count += record->kind == kind ? 1u : 0u;
    }
    collect(system);
    tah_system_end_turn(system);
    return count;
}

/* Runs turns until quiet; returns the dead letters and checks for no runaways. */
static uint32_t settle(tah_system *system) {
    uint32_t dead = 0;
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 1000);
        tah_system_turn(system);
        for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
            CHECK(record->kind != TAH_RECORD_FAULT);
            dead += record->kind == TAH_RECORD_DEAD_LETTER ? 1u : 0u;
        }
        collect(system);
        tah_system_end_turn(system);
    }
    return dead;
}

/* The author's ID of the actor's one active atomic state. */
static const char *state_of(tah_system *system, ta_actor_id actor) {
    static char text[16];
    ta_state_id states[8];
    uint32_t count = 0;
    CHECK(ta_actor_active_states(system->core, &(ta_actor_active_states_params){
                                                   .actor = actor,
                                                   .states = states,
                                                   .capacity = 8,
                                                   .count = &count,
                                               }) == TA_OK);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    if (count == 0) {
        return view.final_state == TA_STATE_NONE ? "" : "final";
    }
    ta_state_info info;
    CHECK(ta_definition_state(system->core, &(ta_definition_state_params){
                                                .definition = view.definition_id,
                                                .state = states[count - 1u],
                                                .out = &info,
                                            }) == TA_OK);
    CHECK(info.id.len < sizeof text);
    memcpy(text, info.id.ptr, info.id.len);
    text[info.id.len] = '\0';
    return text;
}

static uint64_t host_inputs(tah_system *system) {
    ta_counters counters;
    ta_system_counters(system->core, &counters);
    return counters.host_inputs;
}

/* Per-target FIFO holds when a turn stops at its input budget. */
static void fifo_across_the_budget(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    enum { COUNT = 5 };
    ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    settle(system);
    ta_event_name_id x = name(system, "x");
    ta_event_name_id y = name(system, "y");
    for (int index = 0; index < COUNT; index += 1) {
        CHECK(post(system, actors[index], x) == TA_OK);
        CHECK(post(system, actors[index], y) == TA_OK);
    }
    CHECK(host_inputs(system) == 2u * COUNT);
    system->budgets.inputs = 2u;
    tah_system_turn(system);
    CHECK(host_inputs(system) == 2u * COUNT - 2u); /* the budget stopped the turn after two inputs */
    tah_system_end_turn(system);
    CHECK(settle(system) == 0u);
    for (int index = 0; index < COUNT; index += 1) {
        CHECK(strcmp(state_of(system, actors[index]), "c") == 0);
    }
    CHECK(system->sched.delivered == 2u * COUNT);
    CHECK(host_inputs(system) == 0u);
    finish_system(system);
}

/* An input posted between spawn and bootstrap waits for the bootstrap. */
static void input_before_bootstrap(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "b") == 0);
    finish_system(system);
}

/* A stale ID's post is a dead letter and never reaches the slot's next actor. */
static void stale_ids(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id old = spawn(system, chart);
    settle(system);
    destroy(system, old);
    ta_actor_id next = spawn(system, chart);
    CHECK(ta_actor_id_slot(next) == ta_actor_id_slot(old));
    settle(system);
    CHECK(post(system, old, name(system, "x")) == TA_OK);
    CHECK(system->sched.dead_letters == 0u); /* a verdict waits for the next turn */
    CHECK(tah_ring_count(&system->sched.verdicts) == 1u);
    tah_system_turn(system);
    const tah_turn_record *record = system->sched.records;
    CHECK(record != NULL && record->kind == TAH_RECORD_DEAD_LETTER);
    CHECK(record->actor == old && record->reason == TAH_DEAD_NOT_FOUND && record->event == name(system, "x"));
    tah_system_end_turn(system);
    CHECK(strcmp(state_of(system, next), "a") == 0);
    CHECK(system->sched.dead_letters == 1u);
    finish_system(system);
}

/* A malformed event is the caller's error: the post answers INVALID and the actor carries on. */
static void a_malformed_event_is_refused(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id bad = spawn(system, chart);
    ta_actor_id good = spawn(system, chart);
    settle(system);
    CHECK(post(system, bad, 0x7fffffffu) == TA_ERR_INVALID); /* a name core never issued */
    CHECK(host_inputs(system) == 0u);
    CHECK(post(system, good, name(system, "x")) == TA_OK);
    CHECK(post(system, bad, name(system, "x")) == TA_OK); /* the actor was not harmed */
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, bad), "b") == 0);
    CHECK(strcmp(state_of(system, good), "b") == 0);
    finish_system(system);
}

/* A post for a finished actor, and one for a destroyed one, become dead letters. */
static void finished_and_destroyed_targets(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    ta_event_name_id x = name(system, "x");
    ta_event_name_id y = name(system, "y");
    ta_event_name_id z = name(system, "z");
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(post(system, actor, y) == TA_OK);
    CHECK(post(system, actor, z) == TA_OK);
    CHECK(post(system, actor, x) == TA_OK); /* arrives after the actor finished */
    uint32_t dead = 0;
    uint32_t done = 0;
    for (int guard = 0; !tah_system_quiet(system); guard += 1) {
        CHECK(guard < 100);
        tah_system_turn(system);
        for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
            done += record->kind == TAH_RECORD_DONE ? 1u : 0u;
            if (record->kind == TAH_RECORD_DEAD_LETTER) {
                CHECK(record->reason == TAH_DEAD_DONE && record->actor == actor);
                dead += 1u;
            }
        }
        tah_system_end_turn(system);
    }
    CHECK(done == 1u && dead == 1u);
    /* A post to the retained finished actor is a verdict at once. */
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(tah_ring_count(&system->sched.verdicts) == 1u);
    CHECK(settle(system) == 1u);

    ta_actor_id other = spawn(system, chart);
    settle(system);
    CHECK(post(system, other, x) == TA_OK);
    destroy(system, other);
    tah_system_turn(system);
    CHECK(system->sched.records != NULL && system->sched.records->reason == TAH_DEAD_DESTROYED);
    tah_system_end_turn(system);
    CHECK(tah_system_quiet(system));
    finish_system(system);
}

/* maxQueuedMessages refuses new input with FULL; admitted input is never dropped. */
static void backpressure(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    system->sched.max_queued = 2u;
    ta_event_name_id x = name(system, "x");
    CHECK(post(system, actor, x) == TA_OK);
    CHECK(post(system, actor, name(system, "y")) == TA_OK);
    CHECK(post(system, actor, x) == TA_ERR_FULL);
    CHECK(host_inputs(system) == 2u);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    CHECK(post(system, actor, x) == TA_OK); /* room again */
    settle(system);
    finish_system(system);
}

/* Core's own pool bounds the host's backlog: FULL at the watermark, room again after the turn. */
static void core_pool_back_pressure(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_ping(system));
    settle(system);
    ta_event_name_id ping = name(system, "ping");
    uint32_t admitted = 0;
    ta_result result = TA_OK;
    while ((result = post(system, actor, ping)) == TA_OK) {
        admitted += 1u;
        CHECK(admitted < 100000u);
    }
    CHECK(result == TA_ERR_FULL);
    CHECK(admitted > 100u); /* the default pool admits some thousands */
    CHECK(host_inputs(system) == admitted);
    CHECK(settle(system) == 0u);
    CHECK(post(system, actor, ping) == TA_OK);
    settle(system);
    finish_system(system);
}

/* At most `inputs` verdicts are reported per turn. */
static void dead_letter_budget(void) {
    tah_system *system = make_system();
    ta_actor_id gone = spawn(system, define_steps(system));
    settle(system);
    destroy(system, gone);
    ta_event_name_id x = name(system, "x");
    for (int index = 0; index < 10; index += 1) {
        CHECK(post(system, gone, x) == TA_OK);
    }
    system->budgets.inputs = 3u;
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 3u);
    CHECK(turn(system, TAH_RECORD_DEAD_LETTER) == 1u);
    CHECK(tah_system_quiet(system));
    CHECK(system->sched.dead_letters == 10u);
    finish_system(system);
}

/* An operator's transition does not queue: it starts at once, or the post answers BUSY. */
static void transition_does_not_queue(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    ta_actor_inspection view;
    CHECK(ta_actor_inspect(system->core, actor, &view) == TA_OK);
    ta_state_id to_c = TA_STATE_NONE;
    CHECK(ta_definition_find_state(system->core, &(ta_definition_find_state_params){
                                                     .definition = view.definition_id, .id = S("c"), .out = &to_c }) == TA_OK);
    ta_state_id targets[] = { to_c };
    tah_message_jump jump = { .definition = view.definition_id,
                              .transition = { .source = TA_STATE_NONE, .targets = targets, .target_count = 1 } };
    ta_event event = { .name = name(system, "go"), .type = TA_EVENT_EXTERNAL, .invoke_id = TA_INVOKE_ID_NONE };
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    tah_message message = { .target = actor, .event = &event, .jump = &jump };
    CHECK(tah_system_post(system, &message) == TA_ERR_BUSY); /* an input is queued: not idle */
    CHECK(settle(system) == 0u);
    CHECK(tah_system_post(system, &message) == TA_OK);
    CHECK(settle(system) == 0u);
    CHECK(strcmp(state_of(system, actor), "c") == 0);
    jump.definition += 1u;
    CHECK(tah_system_post(system, &message) == TA_ERR_INVALID); /* another definition's state IDs */
    finish_system(system);
}

/* A post made inside a core call that is not a macrostep answers BUSY. */
static void reentrant_post_is_busy(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(!tah_system_reentrant(system));
    tah_system_enter_core(system);
    CHECK(tah_system_reentrant(system));
    CHECK(post(system, actor, name(system, "x")) == TA_ERR_BUSY);
    tah_system_leave_core(system);
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    settle(system);
    finish_system(system);
}

static uint64_t addon_calls(const tah_system *system) {
    uint64_t calls = 0;
    for (int category = 0; category < TAH_MEMORY_CATEGORY_COUNT; category += 1) {
        if (category != TAH_MEMORY_CORE) {
            tah_memory_counters counters = tah_allocator_counters(&system->allocator, (tah_memory_category)category);
            calls += counters.allocations + counters.frees;
        }
    }
    return calls;
}

/* After warm-up, posting and running makes no allocator call in the addon (ADR 0014). */
static void steady_state(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    enum { COUNT = 200 };
    ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    ta_event_name_id ping = name(system, "ping");
    for (int round = 0; round < 3; round += 1) {
        settle(system);
        for (int index = 0; index < COUNT; index += 1) {
            CHECK(post(system, actors[index], ping) == TA_OK);
        }
    }
    settle(system);
    uint64_t before = addon_calls(system);
    uint64_t core_before = tah_allocator_counters(&system->allocator, TAH_MEMORY_CORE).allocations;
    for (int round = 0; round < 5; round += 1) {
        for (int index = 0; index < COUNT; index += 1) {
            CHECK(post(system, actors[index], ping) == TA_OK);
        }
        settle(system);
    }
    CHECK(addon_calls(system) == before);
    uint64_t core_after = tah_allocator_counters(&system->allocator, TAH_MEMORY_CORE).allocations;
    printf("steady state: 0 addon allocator calls, %llu core allocations for %d messages\n",
           (unsigned long long)(core_after - core_before), 5 * COUNT);
    finish_system(system);
}

/* Unloading an actor with an input waiting refuses that input as a dead letter (destroyed). */
static void unload_with_input(void) {
    tah_system *system = make_system();
    ta_actor_id actor = spawn(system, define_steps(system));
    settle(system);
    CHECK(post(system, actor, name(system, "x")) == TA_OK);
    CHECK(tah_hook_actor_removed(system, actor) == TA_OK); /* as after ta_actor_destroy */
    CHECK(ta_actor_destroy(system->core, actor) == TA_OK);
    tah_system_turn(system);
    uint32_t refused = 0;
    for (const tah_turn_record *record = system->sched.records; record != NULL; record = record->next) {
        refused += record->kind == TAH_RECORD_DEAD_LETTER && record->reason == TAH_DEAD_DESTROYED ? 1u : 0u;
    }
    tah_system_end_turn(system);
    CHECK(refused == 1u);
    finish_system(system);
}

/* Close releases admitted input and verdicts and leaks nothing. */
static void close_with_input(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_steps(system);
    ta_actor_id actor = spawn(system, chart);
    ta_actor_id gone = spawn(system, chart);
    settle(system);
    destroy(system, gone);
    for (int index = 0; index < 1000; index += 1) {
        CHECK(post(system, actor, name(system, "y")) == TA_OK);
    }
    CHECK(post(system, gone, name(system, "x")) == TA_OK);
    finish_system(system);
}

/* ------------------------------------------------------------------------
 * send() waiters (ADR 0005 points 9 and 11)
 */

/* A waiter settles when its message's macrostep ends, stable or done. */
static void settles_on_completion(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    settled_count = 0;
    uint32_t ping = send(system, actor, name(system, "ping"));
    CHECK(tah_sched_waiter_state(&system->sched, ping) == TAH_WAITER_PENDING);
    uint32_t stop = send(system, actor, name(system, "stop"));
    settle(system);
    const tah_settlement *first = settlement_of(ping);
    const tah_settlement *second = settlement_of(stop);
    CHECK(first != NULL && first->kind == TAH_SETTLE_STABLE && first->actor == actor);
    CHECK(second != NULL && second->kind == TAH_SETTLE_DONE && second->actor == actor);
    CHECK(first < second); /* settlements follow order */
    CHECK(system->sched.waiters_live == 0);
    CHECK(system->sched.followed == 0u);
    finish_system(system);
}

/* A message to a finished or stale target settles as a dead letter. */
static void settles_as_a_dead_letter(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    ta_actor_id gone = spawn(system, chart);
    settle(system);
    destroy(system, gone);
    settled_count = 0;
    ta_event_name_id stop = name(system, "stop");
    uint32_t first = send(system, actor, stop);
    uint32_t after = send(system, actor, stop); /* the actor is done when it arrives */
    uint32_t stale = send(system, gone, stop);
    settle(system);
    CHECK(settlement_of(first)->kind == TAH_SETTLE_DONE);
    CHECK(settlement_of(after)->kind == TAH_SETTLE_DEAD && settlement_of(after)->reason == TAH_DEAD_DONE);
    CHECK(settlement_of(stale)->kind == TAH_SETTLE_DEAD && settlement_of(stale)->reason == TAH_DEAD_NOT_FOUND);
    finish_system(system);
}

/* An actor destroyed while its inputs wait settles their sends as dead letters (destroyed). */
static void settles_when_its_actor_goes(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    settled_count = 0;
    uint32_t waiter = send(system, actor, name(system, "ping"));
    uint32_t queued = send(system, actor, name(system, "ping"));
    destroy(system, actor);
    settle(system);
    CHECK(settlement_of(waiter)->kind == TAH_SETTLE_DEAD && settlement_of(waiter)->reason == TAH_DEAD_DESTROYED);
    CHECK(settlement_of(queued)->kind == TAH_SETTLE_DEAD && settlement_of(queued)->reason == TAH_DEAD_DESTROYED);
    finish_system(system);
}

/* Cancel withdraws an input core has not taken, so it never runs. */
static void cancel_withdraws(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    settled_count = 0;
    ta_event_name_id ping = name(system, "ping");
    ta_event_name_id stop = name(system, "stop");
    uint32_t withdrawn = send(system, actor, stop);
    uint32_t kept = send(system, actor, ping);
    CHECK(host_inputs(system) == 2u);
    CHECK(tah_system_cancel(system, withdrawn));
    tah_sched_waiter_give(&system->sched, withdrawn);
    CHECK(host_inputs(system) == 1u);
    CHECK(!tah_system_cancel(system, withdrawn)); /* given back already */
    settle(system);
    /* `stop` was withdrawn, so the actor is still running and got `kept`. */
    CHECK(settlement_of(kept)->kind == TAH_SETTLE_STABLE);
    CHECK(settlement_of(withdrawn) == NULL);
    CHECK(strcmp(state_of(system, actor), "idle") == 0);
    CHECK(system->sched.waiters_live == 0 && system->sched.followed == 0u);
    finish_system(system);
}

/* maxRetainedBytes refuses a message that would pass it and counts bytes back down. */
static void retained_bytes(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    settle(system);
    system->sched.max_retained = 100;
    ta_event_name_id ping = name(system, "ping");
    CHECK(post_bytes(system, actor, ping, 60) == TA_OK);
    CHECK(post_bytes(system, actor, ping, 50) == TA_ERR_FULL);
    CHECK(post_bytes(system, actor, ping, 40) == TA_OK);
    CHECK(system->sched.retained_total == 100u);
    CHECK(post_bytes(system, actor, ping, 1) == TA_ERR_FULL);
    CHECK(post_bytes(system, actor, ping, 0) == TA_OK); /* no data: always fits */
    settle(system);
    CHECK(system->sched.retained_total == 0);
    CHECK(system->sched.followed == 0u);
    CHECK(post_bytes(system, actor, ping, 101) == TA_ERR_FULL); /* never fits */
    finish_system(system);
}

/* After warm-up, a send and its settlement make no allocator call in the addon. */
static void send_steady_state(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    enum { COUNT = 60 };
    ta_actor_id actors[COUNT];
    for (int index = 0; index < COUNT; index += 1) {
        actors[index] = spawn(system, chart);
    }
    ta_event_name_id ping = name(system, "ping");
    for (int round = 0; round < 3; round += 1) {
        settle(system);
        settled_count = 0;
        for (int index = 0; index < COUNT; index += 1) {
            send(system, actors[index], ping);
        }
    }
    settle(system);
    uint64_t before = addon_calls(system);
    for (int round = 0; round < 5; round += 1) {
        settled_count = 0;
        for (int index = 0; index < COUNT; index += 1) {
            send(system, actors[index], ping);
        }
        settle(system);
        CHECK(settled_count == COUNT);
    }
    CHECK(addon_calls(system) == before);
    finish_system(system);
}

/* Close with waiters still pending leaks nothing (the Node-API layer rejects them). */
static void close_with_waiters(void) {
    tah_system *system = make_system();
    ta_definition_id chart = define_ping(system);
    ta_actor_id actor = spawn(system, chart);
    send(system, actor, name(system, "ping"));
    finish_system(system);
}

int main(void) {
    fifo_across_the_budget();
    input_before_bootstrap();
    stale_ids();
    a_malformed_event_is_refused();
    finished_and_destroyed_targets();
    backpressure();
    core_pool_back_pressure();
    dead_letter_budget();
    transition_does_not_queue();
    reentrant_post_is_busy();
    steady_state();
    unload_with_input();
    close_with_input();
    settles_on_completion();
    settles_as_a_dead_letter();
    settles_when_its_actor_goes();
    cancel_withdraws();
    retained_bytes();
    send_steady_state();
    close_with_waiters();
    puts("admission_test passed");
    return 0;
}
